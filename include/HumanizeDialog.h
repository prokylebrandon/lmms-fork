/*
 * HumanizeDialog.h - dialog for randomizing velocity, timing and length of notes
 */

#ifndef LMMS_GUI_HUMANIZE_DIALOG_H
#define LMMS_GUI_HUMANIZE_DIALOG_H

#include <QDialog>
#include <functional>
#include <random>
#include <vector>

#include "Note.h"

class QCheckBox;
class QPushButton;
class QSpinBox;

namespace lmms
{

class MidiClip;

namespace gui
{

//! Randomizes velocity, timing and length of the given notes by amounts the user chooses.
//! The result is previewed live in the clip. "Apply" stores exactly one undo checkpoint,
//! so a single Ctrl+Z undoes every humanized note at once. Cancel restores the originals.
class HumanizeDialog : public QDialog
{
public:
HumanizeDialog(MidiClip* clip, const NoteVector& notes, std::function<void()> onChanged, QWidget* parent = nullptr);

private:
struct Entry
{
Note original;      //!< the note as it was before humanizing
Note* current;      //!< the note that is currently in the clip
float velocityRoll; //!< random factors in [-1, 1]
float timingRoll;
float lengthRoll;
};

void reject() override;
void onApply();
void rollAll();
void refresh();
void showState(bool humanized);
void stopPreviewPlayback();

MidiClip* m_clip;
std::function<void()> m_onChanged;
std::mt19937 m_rng;
std::vector<Entry> m_entries;

QSpinBox* m_velocitySpin = nullptr;
QSpinBox* m_timingSpin = nullptr;
QSpinBox* m_lengthSpin = nullptr;
QCheckBox* m_previewBox = nullptr;
QPushButton* m_playButton = nullptr;
};

} // namespace gui
} // namespace lmms

#endif // LMMS_GUI_HUMANIZE_DIALOG_H