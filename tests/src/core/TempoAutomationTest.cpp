/*
 * TempoAutomationTest.cpp - regression tests for tempo changes while notes are sounding
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

#include <QtTest>

#include <cmath>
#include <vector>

#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "Song.h"

namespace
{

//! A tempo change that takes effect at an exact frame, counted from the start of the first rendered period.
struct TempoChange
{
	double frame;
	int tempo;
};

//! The frame at which a note was released, and the start of the period in which that happened.
struct Release
{
	double frame;
	double periodStart;
};

double framesPerTick(int tempo)
{
	return lmms::Engine::audioEngine()->outputSampleRate() * 60.0 * 4.0 / lmms::DefaultTicksPerBar / tempo;
}

//! The frame at which a note of noteTicks ticks, starting at frame 0, ends when every tempo change applies at
//! exactly its own frame. Computed tick by tick, independent of NotePlayHandle::resize().
double expectedEndFrame(double noteTicks, int startTempo, const std::vector<TempoChange>& changes)
{
	auto ticksLeft = noteTicks;
	auto frame = 0.0;
	auto tempo = startTempo;
	for (const auto& change : changes)
	{
		const auto ticksUntilChange = (change.frame - frame) / framesPerTick(tempo);
		if (ticksUntilChange >= ticksLeft) { break; }
		ticksLeft -= ticksUntilChange;
		frame = change.frame;
		tempo = change.tempo;
	}
	return frame + ticksLeft * framesPerTick(tempo);
}

//! Creates a note of the given length that starts at the given offset in the first period, at the given tempo.
lmms::NotePlayHandle* createNote(lmms::InstrumentTrack& track, int ticks, int tempo, lmms::f_cnt_t offset = 0)
{
	using namespace lmms;
	Engine::getSong()->setTempo(tempo);
	const auto frames = static_cast<f_cnt_t>(std::lround(ticks * framesPerTick(tempo)));
	return NotePlayHandlePool.construct(&track, offset, frames, Note(TimePos(ticks), TimePos(0), DefaultKey));
}

//! Plays the note one period at a time like the audio engine does, applying every tempo change at its exact frame
//! within the period (the way automation does), until the note is released.
Release playUntilRelease(lmms::NotePlayHandle* nph, const std::vector<TempoChange>& changes)
{
	const auto periodFrames = lmms::Engine::audioEngine()->framesPerPeriod();
	std::vector<lmms::SampleFrame> buffer(periodFrames);
	auto next = std::size_t{0};

	for (auto periodStart = std::size_t{0}; periodStart < 100'000'000; periodStart += periodFrames)
	{
		while (next < changes.size() && changes[next].frame < static_cast<double>(periodStart + periodFrames))
		{
			nph->resize(changes[next].tempo, static_cast<lmms::f_cnt_t>(changes[next].frame - static_cast<double>(periodStart)));
			++next;
		}
		nph->play(buffer.data());
		if (nph->isReleased()) { return {static_cast<double>(nph->frames()), static_cast<double>(periodStart)}; }
	}
	return {-1.0, -1.0};
}

QString describe(const Release& release, double expected)
{
	return QString("released at frame %1, expected %2").arg(release.frame).arg(expected);
}

} // namespace

class TempoAutomationTest : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase()
	{
		using namespace lmms;
		Engine::init(true);
	}

	void cleanupTestCase()
	{
		using namespace lmms;
		Engine::destroy();
	}

	// #5122: a note that ends exactly where the tempo changes must not be stretched past the change. It used to be
	// rescaled as if the tempo had changed at the start of the period, which pushed its NoteOff behind the NoteOn
	// of the next note on the same key, and that NoteOff then cancelled the new note.
	void noteEndingAtTempoChangeIsNotStretched()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());
		auto nph = createNote(track, 48, 60);
		const auto changeFrame = static_cast<double>(nph->frames());

		const auto release = playUntilRelease(nph, {{changeFrame, 54}});
		NotePlayHandlePool.destroy(nph);

		QVERIFY2(std::abs(release.frame - changeFrame) <= 1.0, qPrintable(describe(release, changeFrame)));
		const auto periodFrames = static_cast<double>(Engine::audioEngine()->framesPerPeriod());
		QVERIFY(release.frame >= release.periodStart && release.frame < release.periodStart + periodFrames);
	}

	// #4928: a note that is sounding when the tempo jumps keeps the frames it has played and only the rest of it
	// is converted to the new tempo.
	void noteLengthAcrossSuddenTempoChange()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());
		auto nph = createNote(track, 96, 60);
		const auto sampleRate = static_cast<double>(Engine::audioEngine()->outputSampleRate());
		const std::vector<TempoChange> changes{{0.5 * sampleRate + 37, 120}};

		const auto release = playUntilRelease(nph, changes);
		NotePlayHandlePool.destroy(nph);

		const auto expected = expectedEndFrame(96, 60, changes);
		QVERIFY2(std::abs(release.frame - expected) <= 1.0, qPrintable(describe(release, expected)));
	}

	// #5319: continuous tempo automation changes the tempo every few ticks. Every step has to be applied from the
	// frame it happens at, or the errors add up and notes end at the wrong time.
	void noteLengthDuringTempoRamps()
	{
		using namespace lmms;
		const auto check = [](int startTempo, int stepsUp, int stepsDown, int stepSize)
		{
			std::vector<TempoChange> changes;
			auto tempo = startTempo;
			auto frame = 0.0;
			for (int i = 0; i < stepsUp + stepsDown; ++i)
			{
				tempo += i < stepsUp ? stepSize : -stepSize;
				frame += 1999.0;
				changes.push_back({frame, tempo});
			}

			InstrumentTrack track(Engine::getSong());
			auto nph = createNote(track, 384, startTempo);
			const auto release = playUntilRelease(nph, changes);
			NotePlayHandlePool.destroy(nph);

			const auto expected = expectedEndFrame(384, startTempo, changes);
			QVERIFY2(std::abs(release.frame - expected) <= 2.0, qPrintable(describe(release, expected)));
		};

		check(140, 0, 60, 1); // falling ramp
		check(100, 40, 40, 3); // rising, then falling
	}

	// A note created earlier in the same period has only played from its own start offset when the tempo changes.
	void noteStartingLaterInThePeriodIsResizedFromItsStart()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());

		// The note starts at frame 100 of the period; the tempo halves at frame 150.
		auto nph = createNote(track, 48, 120, 100);
		auto total = static_cast<double>(nph->frames());
		nph->resize(60, 150);
		auto expected = 50.0 + (total - 50.0) * 2.0;
		QVERIFY2(std::abs(static_cast<double>(nph->frames()) - expected) <= 1.0,
			qPrintable(QString("frames %1, expected %2").arg(nph->frames()).arg(expected)));
		NotePlayHandlePool.destroy(nph);

		// A change before the note starts converts the whole note.
		nph = createNote(track, 48, 120, 100);
		total = static_cast<double>(nph->frames());
		nph->resize(60, 50);
		expected = total * 2.0;
		QVERIFY2(std::abs(static_cast<double>(nph->frames()) - expected) <= 1.0,
			qPrintable(QString("frames %1, expected %2").arg(nph->frames()).arg(expected)));
		NotePlayHandlePool.destroy(nph);
	}

	// Song::setTempo() runs on every automation write, also when the tempo did not change, and often for tiny
	// steps. Repeating the same tempo must not change the note, and a round trip must not drift.
	void unchangedTempoDoesNotChangeNote()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());
		auto nph = createNote(track, 48, 60);
		const auto frames = nph->frames();

		for (int i = 0; i < 1000; ++i) { nph->resize(60, 0); }
		QCOMPARE(nph->frames(), frames);

		nph->resize(120, 0);
		nph->resize(60, 0);
		QVERIFY(std::abs(static_cast<double>(nph->frames()) - static_cast<double>(frames)) <= 1.0);
		NotePlayHandlePool.destroy(nph);
	}

	// Tempo automation in the middle of a period: the tick that starts at the change has to last as long as the new
	// tempo says. Time on the timeline has to stay equal to the time that was rendered.
	void timelineFollowsTempoAutomation()
	{
		using namespace lmms;
		// Keep the render-only device thread from rendering extra periods while the song is stepped here (the mutex is recursive, so this thread can still render)
		const auto changesGuard = Engine::audioEngine()->requestChangesGuard();
		auto song = Engine::getSong();
		auto audioEngine = Engine::audioEngine();
		auto& timeline = song->getTimeline(Song::PlayMode::Song);

		constexpr auto periods = 400;
		auto elapsedFrames = 0.0;
		auto tempoAtEnd = 0; QString trace; auto lastDiff = 0.0;
		{
			AutomationClip clip(song->globalAutomationTrack());
			clip.setProgressionType(AutomationClip::ProgressionType::Discrete);
			clip.addObject(&song->tempoModel());
			clip.putValue(0, 60.0f, false);
			clip.putValue(48, 120.0f, false);
			clip.putValue(96, 40.0f, false);

			timeline.setTicks(0);
			song->playSong();
			for (int i = 0; i < periods; ++i)
{
audioEngine->renderNextPeriod();
const auto diff = timeline.getElapsedSeconds() * audioEngine->outputSampleRate() - (i + 1.0) * audioEngine->framesPerPeriod();
if (std::abs(diff - lastDiff) > 1.0 && trace.size() < 1500)
{
trace += QString(" [period %1: diff %2 (was %3), tempo %4, tick %5, offset %6]")
.arg(i).arg(diff, 0, 'f', 2).arg(lastDiff, 0, 'f', 2).arg(song->getTempo()).arg(timeline.ticks()).arg(timeline.frameOffset());
lastDiff = diff;
}
}
			elapsedFrames = timeline.getElapsedSeconds() * audioEngine->outputSampleRate();
			tempoAtEnd = song->getTempo();
			song->stop();
		}
		timeline.setTicks(0);
		song->setTempo(DefaultTempo);

		const auto renderedFrames = static_cast<double>(periods) * audioEngine->framesPerPeriod();
		QCOMPARE(tempoAtEnd, 40);
		QVERIFY2(std::abs(elapsedFrames - renderedFrames) <= 2.0,
			qPrintable(QString("timeline shows %1 frames, %2 were rendered").arg(elapsedFrames).arg(renderedFrames) + trace));
	}

	// Notes created earlier in the period that is being rendered are not in AudioEngine::playHandles() yet. A
	// tempo change must still resize them.
	void pendingNotesAreResizedBySong()
	{
		using namespace lmms;
		auto song = Engine::getSong();
		InstrumentTrack track(song);
		auto nph = createNote(track, 48, 60);
		const auto frames = static_cast<double>(nph->frames());

		QVERIFY(Engine::audioEngine()->addPlayHandle(nph));
		song->setTempo(120);
		const auto resized = static_cast<double>(nph->frames());

		Engine::audioEngine()->renderNextPeriod(); // hands the note over to playHandles()
		track.silenceAllNotes(true);
		song->setTempo(DefaultTempo);

		QVERIFY2(std::abs(resized - frames / 2.0) <= 1.0,
			qPrintable(QString("frames %1, expected %2").arg(resized).arg(frames / 2.0)));
	}
};

QTEST_GUILESS_MAIN(TempoAutomationTest)
#include "TempoAutomationTest.moc"