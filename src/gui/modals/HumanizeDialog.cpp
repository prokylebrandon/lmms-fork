/*
 * HumanizeDialog.cpp - dialog for randomizing velocity, timing and length of notes
 */

#include "HumanizeDialog.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

#include "Engine.h"
#include "MidiClip.h"
#include "Song.h"

namespace lmms::gui
{

HumanizeDialog::HumanizeDialog(MidiClip* clip, const NoteVector& notes, std::function<void()> onChanged, QWidget* parent)
: QDialog(parent)
, m_clip(clip)
, m_onChanged(std::move(onChanged))
, m_rng(std::random_device{}())
{
setWindowTitle(tr("Humanize"));

m_entries.reserve(notes.size());
for (Note* note : notes)
{
if (note->length() == TimePos(0)) { continue; }
m_entries.push_back(Entry{*note, note, 0.0f, 0.0f, 0.0f});
}

auto settingsBox = new QGroupBox(tr("Humanize settings"));
auto form = new QFormLayout(settingsBox);

auto addAmountRow = [this, form](const QString& label, int maxValue, int defaultValue,
const QString& prefix, const QString& suffix, const QString& tip)
{
auto spin = new QSpinBox();
spin->setRange(0, maxValue);
spin->setValue(defaultValue);
spin->setPrefix(prefix);
spin->setSuffix(suffix);
spin->setToolTip(tip);

auto slider = new QSlider(Qt::Horizontal);
slider->setRange(0, maxValue);
slider->setValue(defaultValue);
slider->setToolTip(tip);

connect(spin, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this]() { refresh(); });

auto rowLayout = new QHBoxLayout();
rowLayout->addWidget(slider, 1);
rowLayout->addWidget(spin);
form->addRow(label, rowLayout);
return spin;
};

m_velocitySpin = addAmountRow(tr("Velocity:"), 100, 10, tr("+/- "), QString{},
tr("Maximum change of the note volume, up or down"));
m_timingSpin = addAmountRow(tr("Timing:"), 48, 6, tr("+/- "), tr(" ticks"),
tr("Maximum shift of the note start, earlier or later (12 ticks = 1/16 note)"));
m_lengthSpin = addAmountRow(tr("Length:"), 100, 10, tr("+/- "), tr(" %"),
tr("Maximum change of the note length, in percent of its length"));

m_previewBox = new QCheckBox(tr("Preview in the piano roll"));
m_previewBox->setChecked(true);
connect(m_previewBox, &QCheckBox::toggled, this, [this]() { refresh(); });

auto randomizeButton = new QPushButton(tr("Randomize"));
m_playButton = new QPushButton(tr("Play preview"));
m_playButton->setCheckable(true);
auto applyButton = new QPushButton(tr("Apply"));
auto cancelButton = new QPushButton(tr("Cancel"));
for (auto button : {randomizeButton, m_playButton, applyButton, cancelButton}) { button->setAutoDefault(false); }

connect(randomizeButton, &QPushButton::clicked, this, [this]() { rollAll(); refresh(); });
connect(m_playButton, &QPushButton::toggled, this, [this](bool on)
{
if (on) { Engine::getSong()->playMidiClip(m_clip); }
else { Engine::getSong()->stop(); }
});
connect(applyButton, &QPushButton::clicked, this, &HumanizeDialog::onApply);
connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

auto buttonsLayout = new QHBoxLayout();
buttonsLayout->addWidget(randomizeButton);
buttonsLayout->addWidget(m_playButton);
buttonsLayout->addStretch();
buttonsLayout->addWidget(applyButton);
buttonsLayout->addWidget(cancelButton);

auto mainLayout = new QVBoxLayout(this);
mainLayout->addWidget(settingsBox);
mainLayout->addWidget(m_previewBox);
mainLayout->addStretch();
mainLayout->addLayout(buttonsLayout);

rollAll();
refresh();
}

void HumanizeDialog::rollAll()
{
auto dist = std::uniform_real_distribution<float>(-1.0f, 1.0f);
for (auto& entry : m_entries)
{
entry.velocityRoll = dist(m_rng);
entry.timingRoll = dist(m_rng);
entry.lengthRoll = dist(m_rng);
}
}

void HumanizeDialog::refresh()
{
if (!m_previewBox) { return; }
showState(m_previewBox->isChecked());
}

//! Writes either the original or the humanized version of every note into the clip.
void HumanizeDialog::showState(bool humanized)
{
const auto velocityAmount = m_velocitySpin->value();
const auto timingAmount = m_timingSpin->value();
const auto lengthAmount = m_lengthSpin->value();

for (auto& entry : m_entries)
{
Note note(entry.original);
if (humanized)
{
const auto volume = static_cast<int>(entry.original.getVolume());
const auto newVolume = volume + static_cast<int>(std::lround(entry.velocityRoll * velocityAmount));
note.setVolume(static_cast<volume_t>(std::clamp(newVolume, 1, static_cast<int>(MaxVolume))));

const auto pos = static_cast<int>(entry.original.pos().getTicks());
note.setPos(TimePos(std::max(0, pos + static_cast<int>(std::lround(entry.timingRoll * timingAmount)))));

const auto length = static_cast<int>(entry.original.length().getTicks());
const auto factor = 1.0f + entry.lengthRoll * static_cast<float>(lengthAmount) / 100.0f;
note.setLength(TimePos(std::max(1, static_cast<int>(std::lround(static_cast<float>(length) * factor)))));
}

m_clip->removeNote(entry.current);
entry.current = m_clip->addNote(note, false);
}

if (m_onChanged) { m_onChanged(); }
}

void HumanizeDialog::onApply()
{
stopPreviewPlayback();

// Put the original notes back first, so the checkpoint holds the state from before humanizing.
// One checkpoint for the whole operation means one Ctrl+Z undoes all humanized notes at once.
showState(false);
m_clip->addJournalCheckPoint();
showState(true);

accept();
}

void HumanizeDialog::reject()
{
stopPreviewPlayback();
showState(false);
QDialog::reject();
}

void HumanizeDialog::stopPreviewPlayback()
{
if (m_playButton && m_playButton->isChecked()) { m_playButton->setChecked(false); }
}

} // namespace lmms::gui