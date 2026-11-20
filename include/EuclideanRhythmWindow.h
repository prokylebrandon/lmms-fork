/*
 * EuclideanRhythmWindow.h - Euclidean rhythm generator window
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

#ifndef LMMS_GUI_EUCLIDEAN_RHYTHM_WINDOW_H
#define LMMS_GUI_EUCLIDEAN_RHYTHM_WINDOW_H

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

class QLabel;
class QPushButton;
class QSlider;
class QVBoxLayout;

namespace lmms
{

class InstrumentTrack;

namespace euclid
{

//! Euclidean rhythm of `hits` onsets spread as evenly as possible over `steps` steps,
//! rotated to the right by `rotation` steps. The first onset lands on step 0 before rotation.
inline std::vector<bool> pattern(int steps, int hits, int rotation = 0)
{
	if (steps <= 0) { return {}; }
	hits = std::clamp(hits, 0, steps);
	const int shift = ((rotation % steps) + steps) % steps;
	auto result = std::vector<bool>(static_cast<std::size_t>(steps), false);
	for (int i = 0; i < steps; ++i)
	{
		if ((i * hits) % steps < hits) { result[static_cast<std::size_t>((i + shift) % steps)] = true; }
	}
	return result;
}

//! Key that generated notes use for this instrument: its configured base (root) note,
//! limited to the key range the instrument reacts to.
int rootKey(const InstrumentTrack* track);

} // namespace euclid

namespace gui
{

class EuclideanCircleView;

class EuclideanRhythmWindow : public QWidget
{
	Q_OBJECT
public:
	EuclideanRhythmWindow();
	~EuclideanRhythmWindow() override;

	//! Creates the window on first use and shows it
	static void showWindow();

protected:
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;

private slots:
	void addRhythm();
	void removeRhythm();
	void refreshInstruments();
	void togglePreview(bool on);
	void previewTick();

private:
	struct Row;

	void stepsChanged(Row& row);
	void rowEdited(Row& row);
	void applyPreset(Row& row, int index);
	void updateLabels(Row& row);
	void updateCircle();
	QString clipDataFor(const Row& row) const;

	QPushButton* m_previewButton = nullptr;
	QSlider* m_tempo = nullptr;
	QSlider* m_swing = nullptr;
	QLabel* m_tempoLabel = nullptr;
	QLabel* m_swingLabel = nullptr;
	EuclideanCircleView* m_circle = nullptr;
	QWidget* m_rowsHost = nullptr;
	QVBoxLayout* m_rowsLayout = nullptr;
	QPushButton* m_addButton = nullptr;
	QPushButton* m_removeButton = nullptr;

	std::vector<std::unique_ptr<Row>> m_rows;

	QTimer m_previewTimer;
	qint64 m_lastPlayheadUpdateNs = 0; //!< Preview clock time of the last circle repaint
	QElapsedTimer m_clock;
	double m_lastPos = -1.0;
};

} // namespace gui

} // namespace lmms

#endif // LMMS_GUI_EUCLIDEAN_RHYTHM_WINDOW_H