/*
 * EuclideanRhythmWindow.cpp - Euclidean rhythm generator window
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include "EuclideanRhythmWindow.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QMenu>

#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QDomElement>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <utility>

#include "DataFile.h"
#include "embed.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "MainWindow.h"
#include "Midi.h"
#include "MidiEvent.h"
#include "Note.h"
#include "Song.h"
#include "StringPairDrag.h"
#include "SubWindow.h"
#include "TimePos.h"
#include "Track.h"

namespace lmms
{

int euclid::rootKey(const InstrumentTrack* track)
{
	if (!track) { return DefaultKey; }

	// Everything in LMMS (frequency maths, Sf2 key mapping, sample playback) is relative to the
	// track's base note, so that is the note an instrument treats as its neutral key.
	int key = track->baseNote();
	const int first = track->firstKey();
	const int last = track->lastKey();
	if (first <= last) { key = std::clamp(key, first, last); }
	return std::clamp(key, 0, NumKeys - 1);
}

namespace gui
{

namespace
{

constexpr int MaxRhythms = 8;
constexpr int MinSteps = 2;
constexpr int MaxSteps = 32;
constexpr unsigned int NoSourceContainer = 0xFFFFFFFFu; // matches no real track container
constexpr int AnchorTrackIndex = 1000;                   // keeps the paste on the track that gets the drop
constexpr double TwoPi = 6.283185307179586;

struct Preset
{
	const char* name;
	int hits;
	int steps;
	int rotation;
};

constexpr Preset Presets[] = {
	{"Tresillo", 3, 8, 0},
	{"Cinquillo", 5, 8, 0},
	{"Cumbia", 3, 4, 0},
	{"York-Samai", 5, 6, 0},
	{"Ruchenitza", 4, 7, 0},
	{"Aksak", 4, 9, 0},
	{"Venda", 5, 12, 0},
	{"West African bell", 7, 12, 0},
	{"Bossa-nova", 5, 16, 0},
	{"Samba", 7, 16, 0},
};

QColor ringColor(int index)
{
	return QColor::fromHsv((index * 47 + 20) % 360, 170, 235);
}

QSlider* addSliderRow(QWidget* parent, QGridLayout* grid, int row, const QString& title,
	int min, int max, int value, QLabel*& valueLabel)
{
	grid->addWidget(new QLabel(title, parent), row, 1);
	auto slider = new QSlider(Qt::Horizontal, parent);
	slider->setRange(min, max);
	slider->setValue(value);
	valueLabel = new QLabel(QString::number(value), parent);
	valueLabel->setMinimumWidth(22);
	valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	grid->addWidget(slider, row, 2);
	grid->addWidget(valueLabel, row, 3);
	return slider;
}

} // namespace


//! Draws every rhythm track as a ring of steps, hits are filled and joined by a polygon
class EuclideanCircleView : public QWidget
{
public:
	struct Ring
	{
		std::vector<bool> hits;
		double swing = 0.0; // 0..1
		QColor color;
	};

	explicit EuclideanCircleView(QWidget* parent)
		: QWidget(parent)
	{
		setMinimumSize(260, 260);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	}

	void setRings(std::vector<Ring> rings)
	{
		m_rings = std::move(rings);
		update();
	}

	void setPlayhead(double position)
	{
		m_playhead = position;
		update();
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QPalette& pal = palette();

		const QPointF center(width() / 2.0, height() / 2.0);
		const double outer = std::min(width(), height()) / 2.0 - 12.0;
		if (outer <= 0.0) { return; }

		const int count = static_cast<int>(m_rings.size());
		const double gap = count > 0 ? std::min(34.0, (outer - 14.0) / count) : 0.0;

		const auto point = [&](double radius, double fraction)
		{
			const double angle = TwoPi * fraction - TwoPi / 4.0;
			return QPointF(center.x() + radius * std::cos(angle), center.y() + radius * std::sin(angle));
		};

		for (int k = 0; k < count; ++k)
		{
			const Ring& ring = m_rings[static_cast<std::size_t>(k)];
			const int steps = static_cast<int>(ring.hits.size());
			if (steps <= 0) { continue; }

			const double radius = outer - k * gap;
			const auto fractionOf = [&](int i)
			{
				return (i + ((i % 2) ? ring.swing * 0.5 : 0.0)) / steps;
			};

			p.setPen(QPen(pal.color(QPalette::Mid), 1));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(center, radius, radius);

			QPolygonF polygon;
			for (int i = 0; i < steps; ++i)
			{
				if (ring.hits[static_cast<std::size_t>(i)]) { polygon << point(radius, fractionOf(i)); }
			}
			if (polygon.size() > 1)
			{
				QColor fill = ring.color;
				fill.setAlpha(45);
				p.setPen(QPen(ring.color, 1.5));
				p.setBrush(fill);
				p.drawPolygon(polygon);
			}

			for (int i = 0; i < steps; ++i)
			{
				const QPointF pt = point(radius, fractionOf(i));
				if (ring.hits[static_cast<std::size_t>(i)])
				{
					p.setPen(QPen(ring.color.darker(150), 1));
					p.setBrush(ring.color);
					p.drawEllipse(pt, 5.0, 5.0);
				}
				else
				{
					p.setPen(QPen(pal.color(QPalette::Mid), 1));
					p.setBrush(pal.color(QPalette::Base));
					p.drawEllipse(pt, 2.5, 2.5);
				}
			}
		}

		if (m_playhead >= 0.0)
		{
			p.setPen(QPen(pal.color(QPalette::Highlight), 2));
			p.drawLine(center, point(outer + 6.0, m_playhead));
		}
	}

private:
	std::vector<Ring> m_rings;
	double m_playhead = -1.0;
};


//! Grip that starts a Song Editor clip drag for one rhythm track
class EuclideanDragHandle : public QWidget
{
public:
	EuclideanDragHandle(std::function<QString()> payload, const QColor& color, QWidget* parent)
		: QWidget(parent)
		, m_payload(std::move(payload))
		, m_color(color)
	{
		setFixedWidth(26);
		setMinimumHeight(48);
		setCursor(Qt::OpenHandCursor);
		setToolTip(tr("Drag this rhythm into the Song Editor to create a pattern"));
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(m_color);
		p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 4, 4);
		p.setPen(QPen(palette().color(QPalette::Window), 2));
		const int cx = width() / 2;
		const int cy = height() / 2;
		for (int i = -1; i <= 1; ++i) { p.drawLine(cx - 5, cy + i * 6, cx + 5, cy + i * 6); }
	}

	void mousePressEvent(QMouseEvent* e) override
	{
		if (e->button() == Qt::LeftButton)
		{
			m_pressPos = mapFromGlobal(QCursor::pos());
			m_pressed = true;
		}
	}

	void mouseMoveEvent(QMouseEvent* e) override
	{
		if (!m_pressed || !(e->buttons() & Qt::LeftButton)) { return; }
		if ((mapFromGlobal(QCursor::pos()) - m_pressPos).manhattanLength() < QApplication::startDragDistance()) { return; }
		m_pressed = false;

		const QString data = m_payload();
		if (data.isEmpty()) { return; }

		// Same drag key the Song Editor accepts for instrument track clips; the drag runs its own loop
		new StringPairDrag(
			QStringLiteral("clip_%1").arg(static_cast<int>(Track::Type::Instrument)), data, QPixmap(), this);
	}

	void mouseReleaseEvent(QMouseEvent*) override { m_pressed = false; }

private:
	std::function<QString()> m_payload;
	QColor m_color;
	QPoint m_pressPos;
	bool m_pressed = false;
};


struct EuclideanRhythmWindow::Row
{
	QFrame* widget = nullptr;
	EuclideanDragHandle* handle = nullptr;
	QComboBox* instrument = nullptr;
	QComboBox* preset = nullptr;
	QSlider* steps = nullptr;
	QSlider* hits = nullptr;
	QSlider* rotation = nullptr;
	QLabel* stepsLabel = nullptr;
	QLabel* hitsLabel = nullptr;
	QLabel* rotationLabel = nullptr;
	QColor color;
	std::vector<QPointer<InstrumentTrack>> tracks; // parallel to the items of the instrument combo box

	InstrumentTrack* selectedTrack() const
	{
		const int i = instrument->currentIndex();
		return (i >= 0 && i < static_cast<int>(tracks.size())) ? tracks[static_cast<std::size_t>(i)].data() : nullptr;
	}
};


EuclideanRhythmWindow::EuclideanRhythmWindow()
	: QWidget(nullptr)
{
	setWindowTitle(tr("Euclidean Rhythm Generator"));
	setWindowIcon(embed::getIconPixmap("gear"));
	setMinimumSize(640, 420);

	auto root = new QVBoxLayout(this);

	auto top = new QHBoxLayout();
	m_previewButton = new QPushButton(tr("Preview"), this);
	m_previewButton->setCheckable(true);
	m_previewButton->setToolTip(tr("Play the rhythms on the selected instrument tracks at this tempo"));
	top->addWidget(m_previewButton);
	top->addSpacing(12);

	top->addWidget(new QLabel(tr("Tempo"), this));
	m_tempo = new QSlider(Qt::Horizontal, this);
	m_tempo->setRange(40, 300);
	m_tempo->setValue(std::clamp(static_cast<int>(Engine::getSong()->getTempo()), 40, 300));
	m_tempoLabel = new QLabel(QString::number(m_tempo->value()), this);
	m_tempoLabel->setMinimumWidth(28);
	top->addWidget(m_tempo, 1);
	top->addWidget(m_tempoLabel);
	top->addSpacing(12);

	top->addWidget(new QLabel(tr("Swing"), this));
	m_swing = new QSlider(Qt::Horizontal, this);
	m_swing->setRange(0, 100);
	m_swing->setValue(0);
	m_swingLabel = new QLabel(QStringLiteral("0%"), this);
	m_swingLabel->setMinimumWidth(34);
	top->addWidget(m_swing, 1);
	top->addWidget(m_swingLabel);
	root->addLayout(top);

	auto body = new QHBoxLayout();
	m_circle = new EuclideanCircleView(this);
	body->addWidget(m_circle, 1);

	auto right = new QVBoxLayout();
	auto hint = new QLabel(tr("Right-click a rhythm to delete it, or empty space to add one"), this);
	hint->setWordWrap(true);
	hint->setEnabled(false); // greyed-out text: a hint, not a control
	right->addWidget(hint);

	auto scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	m_rowsHost = new QWidget(scroll);
	m_rowsLayout = new QVBoxLayout(m_rowsHost);
	m_rowsLayout->setContentsMargins(0, 0, 0, 0);
	m_rowsLayout->addStretch(1);
	scroll->setWidget(m_rowsHost);
	scroll->setMinimumWidth(340);
	right->addWidget(scroll, 1);

	body->addLayout(right, 1);
	root->addLayout(body, 1);

	connect(m_previewButton, &QPushButton::toggled, this, &EuclideanRhythmWindow::togglePreview);
	connect(m_tempo, &QSlider::valueChanged, this, [this](int value)
	{
		m_tempoLabel->setText(QString::number(value));
		if (m_previewButton->isChecked())
		{
			m_clock.restart();
			m_lastPos = -1.0;
		}
	});
	connect(m_swing, &QSlider::valueChanged, this, [this](int value)
	{
		m_swingLabel->setText(QString::number(value) + QStringLiteral("%"));
		updateCircle();
	});

	m_previewTimer.setTimerType(Qt::PreciseTimer);
	m_previewTimer.setInterval(5);
	connect(&m_previewTimer, &QTimer::timeout, this, &EuclideanRhythmWindow::previewTick);

	connect(Engine::getSong(), SIGNAL(projectLoaded()), this, SLOT(refreshInstruments()));
	connect(Engine::getSong(), SIGNAL(trackAdded(lmms::Track*)), this, SLOT(refreshInstruments()));

	addRhythm();
}


EuclideanRhythmWindow::~EuclideanRhythmWindow() = default;


void EuclideanRhythmWindow::showWindow()
{
	static QPointer<EuclideanRhythmWindow> s_window;
	static QPointer<SubWindow> s_subWindow;

	if (!s_window)
	{
		s_window = new EuclideanRhythmWindow();
		s_subWindow = getGUI()->mainWindow()->addWindowedWidget(s_window);
		s_subWindow->setAttribute(Qt::WA_DeleteOnClose, false);
		s_subWindow->resize(680, 480);
	}

	s_subWindow->show();
	s_window->show();
	s_subWindow->raise();
}


void EuclideanRhythmWindow::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	refreshInstruments();
}


void EuclideanRhythmWindow::hideEvent(QHideEvent* event)
{
	QWidget::hideEvent(event);
	m_previewButton->setChecked(false);
}


void EuclideanRhythmWindow::addRhythm()
{
	if (m_rows.size() >= static_cast<std::size_t>(MaxRhythms)) { return; }

	auto owned = std::make_unique<Row>();
	Row* r = owned.get();
	int index = static_cast<int>(m_rows.size());
	for (int candidate = 0; candidate < MaxRhythms; ++candidate)
	{
		const QColor candidateColor = ringColor(candidate);
		const bool taken = std::any_of(m_rows.begin(), m_rows.end(),
			[&candidateColor](const std::unique_ptr<Row>& other) { return other->color == candidateColor; });
		if (!taken) { index = candidate; break; }
	}
	r->color = ringColor(index);

	r->widget = new QFrame(m_rowsHost);
	r->widget->setFrameShape(QFrame::StyledPanel);
	auto grid = new QGridLayout(r->widget);
	grid->setContentsMargins(6, 6, 6, 6);
	grid->setSpacing(4);

	r->handle = new EuclideanDragHandle([this, r]() { return clipDataFor(*r); }, r->color, r->widget);
	grid->addWidget(r->handle, 0, 0, 5, 1);

	r->instrument = new QComboBox(r->widget);
	r->instrument->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	r->instrument->setMinimumContentsLength(12);
	r->instrument->setToolTip(tr("Instrument track that plays this rhythm and decides the note pitch"));
	grid->addWidget(r->instrument, 0, 1, 1, 3);

	r->preset = new QComboBox(r->widget);
	r->preset->addItem(tr("Custom"));
	for (const Preset& preset : Presets)
	{
		r->preset->addItem(QStringLiteral("%1 (%2/%3)").arg(QString::fromUtf8(preset.name)).arg(preset.hits).arg(preset.steps));
	}
	grid->addWidget(r->preset, 1, 1, 1, 3);

	r->steps = addSliderRow(r->widget, grid, 2, tr("Steps"), MinSteps, MaxSteps, 16, r->stepsLabel);
	r->hits = addSliderRow(r->widget, grid, 3, tr("Hits"), 0, 16, 4, r->hitsLabel);
	r->rotation = addSliderRow(r->widget, grid, 4, tr("Rotate"), 0, 15, 0, r->rotationLabel);

	connect(r->steps, &QSlider::valueChanged, r->widget, [this, r](int) { stepsChanged(*r); });
	connect(r->hits, &QSlider::valueChanged, r->widget, [this, r](int) { rowEdited(*r); });
	connect(r->rotation, &QSlider::valueChanged, r->widget, [this, r](int) { rowEdited(*r); });
	connect(r->preset, qOverload<int>(&QComboBox::currentIndexChanged), r->widget,
		[this, r](int i) { applyPreset(*r, i); });

	m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, r->widget);
	r->widget->show();

	m_rows.push_back(std::move(owned));

	refreshInstruments();
	updateCircle();
}


void EuclideanRhythmWindow::removeRhythm(Row* row)
{
	if (m_rows.size() <= 1) { return; }

	const auto it = std::find_if(m_rows.begin(), m_rows.end(),
		[row](const std::unique_ptr<Row>& other) { return other.get() == row; });
	if (it == m_rows.end()) { return; }

	delete (*it)->widget;
	m_rows.erase(it);

	updateCircle();
}


void EuclideanRhythmWindow::contextMenuEvent(QContextMenuEvent* event)
{
	// Only the rhythm list reacts: right-clicking the circle, the sliders on top or the scroll bar does nothing
	QWidget* viewport = m_rowsHost->parentWidget();
	if (!viewport || !viewport->rect().contains(viewport->mapFromGlobal(event->globalPos())))
	{
		event->ignore();
		return;
	}

	Row* clicked = nullptr;
	for (const auto& rowPtr : m_rows)
	{
		const QWidget* widget = rowPtr->widget;
		if (widget->rect().contains(widget->mapFromGlobal(event->globalPos())))
		{
			clicked = rowPtr.get();
			break;
		}
	}

	QMenu menu(this);
	QAction* deleteAction = nullptr;
	QAction* addAction = nullptr;
	if (clicked)
	{
		deleteAction = menu.addAction(tr("Delete rhythm"));
		deleteAction->setEnabled(m_rows.size() > 1); // the generator always keeps one rhythm
	}
	else
	{
		addAction = menu.addAction(tr("Add rhythm"));
		addAction->setEnabled(m_rows.size() < static_cast<std::size_t>(MaxRhythms));
	}

	const QAction* chosen = menu.exec(event->globalPos());
	event->accept();
	if (!chosen) { return; }

	if (chosen == deleteAction) { removeRhythm(clicked); }
	else if (chosen == addAction) { addRhythm(); }
}


void EuclideanRhythmWindow::refreshInstruments()
{
	auto available = std::vector<InstrumentTrack*>{};
	for (Track* track : Engine::getSong()->tracks())
	{
		if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track)) { available.push_back(instrumentTrack); }
	}

	int rowIndex = 0;
	for (auto& rowPtr : m_rows)
	{
		Row& row = *rowPtr;
		InstrumentTrack* previous = row.selectedTrack();

		const QSignalBlocker blocker(row.instrument);
		row.instrument->clear();
		row.tracks.clear();

		int selected = -1;
		for (InstrumentTrack* t : available)
		{
			if (t == previous) { selected = row.instrument->count(); }
			row.instrument->addItem(t->name());
			row.tracks.emplace_back(t);
		}
		if (selected < 0 && !available.empty()) { selected = rowIndex % static_cast<int>(available.size()); }

		row.instrument->setCurrentIndex(selected);
		row.instrument->setEnabled(!available.empty());
		++rowIndex;
	}
}


void EuclideanRhythmWindow::stepsChanged(Row& row)
{
	const int steps = row.steps->value();
	{
		const QSignalBlocker hitsBlocker(row.hits);
		const QSignalBlocker rotationBlocker(row.rotation);
		row.hits->setMaximum(steps);
		row.rotation->setMaximum(std::max(0, steps - 1));
	}
	rowEdited(row);
}


void EuclideanRhythmWindow::rowEdited(Row& row)
{
	updateLabels(row);
	if (row.preset->currentIndex() != 0)
	{
		const QSignalBlocker blocker(row.preset);
		row.preset->setCurrentIndex(0);
	}
	updateCircle();
}


void EuclideanRhythmWindow::applyPreset(Row& row, int index)
{
	if (index <= 0 || index > static_cast<int>(std::size(Presets))) { return; }
	const Preset& preset = Presets[index - 1];
	{
		const QSignalBlocker stepsBlocker(row.steps);
		const QSignalBlocker hitsBlocker(row.hits);
		const QSignalBlocker rotationBlocker(row.rotation);
		row.steps->setValue(preset.steps);
		row.hits->setMaximum(preset.steps);
		row.rotation->setMaximum(preset.steps - 1);
		row.hits->setValue(preset.hits);
		row.rotation->setValue(preset.rotation);
	}
	updateLabels(row);
	updateCircle();
}


void EuclideanRhythmWindow::updateLabels(Row& row)
{
	row.stepsLabel->setNum(row.steps->value());
	row.hitsLabel->setNum(row.hits->value());
	row.rotationLabel->setNum(row.rotation->value());
}


void EuclideanRhythmWindow::updateCircle()
{
	auto rings = std::vector<EuclideanCircleView::Ring>{};
	const double swing = m_swing->value() / 100.0;
	for (const auto& rowPtr : m_rows)
	{
		const Row& row = *rowPtr;
		EuclideanCircleView::Ring ring;
		ring.hits = euclid::pattern(row.steps->value(), row.hits->value(), row.rotation->value());
		ring.swing = swing;
		ring.color = row.color;
		rings.push_back(std::move(ring));
	}
	m_circle->setRings(std::move(rings));
}


void EuclideanRhythmWindow::togglePreview(bool on)
{
	if (on)
	{
		m_clock.start();
		m_lastPos = -1.0;
		m_previewTimer.start();
	}
	else
	{
		m_previewTimer.stop();
		m_circle->setPlayhead(-1.0);
	}
}


void EuclideanRhythmWindow::previewTick()
{
	// One cycle is one bar (4 beats) at the preview tempo
	const double cycleMs = 240000.0 / std::max(1, m_tempo->value());
	const double elapsed = static_cast<double>(m_clock.nsecsElapsed()) / 1.0e6;
	const double pos = std::fmod(elapsed, cycleMs) / cycleMs;
	const double previous = m_lastPos;
	m_lastPos = pos;
	// Notes are still scheduled on every tick, but the circle only needs about 60 repaints a second.
	// The check for a smaller value covers the preview clock being restarted.
	const qint64 nowNs = m_clock.nsecsElapsed();
	if (nowNs < m_lastPlayheadUpdateNs || nowNs - m_lastPlayheadUpdateNs >= 16000000)
	{
		m_lastPlayheadUpdateNs = nowNs;
		m_circle->setPlayhead(pos);
	}

	const double swing = m_swing->value() / 100.0;
	for (const auto& rowPtr : m_rows)
	{
		const Row& row = *rowPtr;
		InstrumentTrack* track = row.selectedTrack();
		if (!track) { continue; }

		const auto hits = euclid::pattern(row.steps->value(), row.hits->value(), row.rotation->value());
		const int steps = static_cast<int>(hits.size());
		for (int i = 0; i < steps; ++i)
		{
			if (!hits[static_cast<std::size_t>(i)]) { continue; }

			const double fraction = (i + ((i % 2) ? swing * 0.5 : 0.0)) / steps;
			const bool due = previous <= pos
				? (fraction > previous && fraction <= pos)
				: (fraction > previous || fraction <= pos);
			if (!due) { continue; }

			const auto key = static_cast<int16_t>(euclid::rootKey(track));
			track->processInEvent(MidiEvent(MidiNoteOn, 0, key, MidiDefaultVelocity));
			const int lengthMs = std::clamp(static_cast<int>(cycleMs / steps * 0.75), 40, 250);
			QTimer::singleShot(lengthMs, track, [track, key]()
			{
				track->processInEvent(MidiEvent(MidiNoteOff, 0, key, 0));
			});
		}
	}
}


QString EuclideanRhythmWindow::clipDataFor(const Row& row) const
{
	InstrumentTrack* track = row.selectedTrack();
	if (!track) { return {}; }

	const int steps = row.steps->value();
	const int hitCount = row.hits->value();
	const auto hits = euclid::pattern(steps, hitCount, row.rotation->value());
	if (std::count(hits.begin(), hits.end(), true) == 0) { return {}; }

	const int key = euclid::rootKey(track);
	const double swing = m_swing->value() / 100.0;
	const int barTicks = static_cast<int>(TimePos::ticksPerBar());
	const double stepTicks = static_cast<double>(barTicks) / steps;

	// Same layout ClipView::createClipDataFiles produces for a copied/dragged clip, so
	// TrackContentWidget::pasteSelection creates an ordinary MidiClip from it.
	DataFile dataFile(DataFile::Type::DragNDropData);
	QDomElement clips = dataFile.createElement("clips");
	QDomElement clip = dataFile.createElement("clip");
	clip.setAttribute("trackIndex", AnchorTrackIndex);
	clip.setAttribute("trackType", static_cast<int>(Track::Type::Instrument));
	clip.setAttribute("trackName", track->name());

	QDomElement midi = dataFile.createElement("midiclip");
	midi.setAttribute("type", 1);
	midi.setAttribute("name", QStringLiteral("Euclid %1/%2").arg(hitCount).arg(steps));
	midi.setAttribute("autoresize", 1);
	midi.setAttribute("off", 0);
	midi.setAttribute("pos", 0);
	midi.setAttribute("muted", 0);
	midi.setAttribute("steps", static_cast<int>(TimePos::stepsPerBar()));
	midi.setAttribute("len", barTicks);

	for (int i = 0; i < steps; ++i)
	{
		if (!hits[static_cast<std::size_t>(i)]) { continue; }

		const double exact = (i + ((i % 2) ? swing * 0.5 : 0.0)) * stepTicks;
		const int pos = std::min(barTicks - 1, static_cast<int>(std::lround(exact)));
		const int length = std::max(1, std::min(barTicks - pos, static_cast<int>(stepTicks * 0.75)));

		QDomElement note = dataFile.createElement("note");
		note.setAttribute("key", key);
		note.setAttribute("vol", static_cast<int>(DefaultVolume));
		note.setAttribute("pan", static_cast<int>(DefaultPanning));
		note.setAttribute("len", length);
		note.setAttribute("pos", pos);
		note.setAttribute("type", static_cast<int>(Note::Type::Regular));
		midi.appendChild(note);
	}

	clip.appendChild(midi);
	clips.appendChild(clip);
	dataFile.content().appendChild(clips);

	QDomElement metadata = dataFile.createElement("copyMetadata");
	metadata.setAttribute("initialTrackIndex", AnchorTrackIndex);
	metadata.setAttribute("trackContainerId", NoSourceContainer);
	metadata.setAttribute("grabbedClipPos", 0);
	dataFile.content().appendChild(metadata);

	return dataFile.toString();
}

} // namespace gui

} // namespace lmms