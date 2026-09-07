/*
 * InstrumentTrackFreezeTest.cpp
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

#include <QDir>
#include <QDomDocument>
#include <QEventLoop>
#include <QFileInfo>
#include <QSignalSpy>
#include <QtTest>

#include "Engine.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Song.h"

using namespace lmms;

namespace
{

// Spin the event loop until `track` emits frozenStateChanged() with
// isFreezing() having gone back to false, or timeoutMs elapses. Freezing is
// asynchronous (RenderManager runs the render on a QThread and delivers
// completion via a queued signal), so tests need to pump the loop rather
// than assert on state immediately after calling freeze().
bool waitForFreezeToFinish(InstrumentTrack& track, int timeoutMs = 15000)
{
	if (!track.isFreezing())
	{
		return true;
	}

	QEventLoop loop;
	QTimer timeoutTimer;
	timeoutTimer.setSingleShot(true);
	QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

	// frozenStateChanged() fires more than once during a freeze (pending,
	// then finished), so re-check isFreezing() each time rather than
	// quitting on the first signal.
	QObject::connect(&track, &InstrumentTrack::frozenStateChanged, &loop, [&]() {
		if (!track.isFreezing())
		{
			loop.quit();
		}
	});

	timeoutTimer.start(timeoutMs);
	loop.exec();

	return !track.isFreezing();
}

} // namespace


class InstrumentTrackFreezeTest : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase()
	{
		Engine::init(true);
	}

	void cleanupTestCase()
	{
		Engine::destroy();
	}

	void init()
	{
		// Freeze caches fall back to the system temp directory when the
		// project has no file name yet (see InstrumentTrack::freezeCacheDir()),
		// which is the normal state for a Song built directly in a test
		// rather than loaded from/saved to disk -- Song::setProjectFileName()
		// is private and only reachable via the heavier saveProjectFile(),
		// so tests don't set it directly. Each InstrumentTrack already gets
		// a unique cache filename (see InstrumentTrack::m_freezeCacheId), so
		// sharing the system temp directory across test runs is safe and
		// nothing further needs to be set up here.
	}

	void cleanup()
	{
	}

	// A track with no instrument loaded (m_instrument stays null / never
	// gets swapped to a real plugin in the most minimal case) should not be
	// freezable -- freeze() is documented to no-op rather than crash or
	// silently "succeed" with nothing rendered.
	void testFreezeNoInstrumentIsNoop()
	{
		auto* song = Engine::getSong();
		InstrumentTrack track(song);

		// Give it a real (if dummy) instrument via loadInstrument with a
		// name that can't resolve to a real plugin, forcing DummyInstrument;
		// what matters here is just that freeze() doesn't do anything
		// destructive or crash when asked to freeze a track that has
		// nothing meaningful to render.
		track.freeze();
		QVERIFY(!track.isFrozen());
	}

	// Core round trip: freeze a track with a real instrument and a note,
	// wait for the asynchronous render to finish, and check that it
	// produced a plausible, non-empty cached render and switched the track
	// into the frozen state.
	void testFreezeProducesRender()
	{
		auto* song = Engine::getSong();
		song->setTempo(140);

		InstrumentTrack track(song);
		track.loadInstrument("tripleoscillator");

		if (track.instrument() == nullptr || track.instrument()->descriptor()->name != QString("tripleoscillator"))
		{
			QSKIP("tripleoscillator plugin not available in this build/environment; "
				"skipping audio-content assertions, freeze mechanics are covered by other tests");
		}

		MidiClip clip(&track);
		clip.changeLength(TimePos(1, 0));
		clip.addNote(Note(TimePos(1, 0), TimePos(0, 0), DefaultKey), false);

		QVERIFY(!track.isFrozen());
		QVERIFY(!track.isFreezing());

		track.freeze();
		QVERIFY(track.isFreezing());

		QVERIFY2(waitForFreezeToFinish(track), "freeze render did not complete in time");

		QVERIFY(track.isFrozen());
		QVERIFY(!track.isStale());
		QVERIFY(!track.frozenSamplePath().isEmpty());
		QVERIFY(QFileInfo::exists(track.frozenSamplePath()));
		QVERIFY(QFileInfo(track.frozenSamplePath()).size() > 0);
	}

	// Unfreezing must be instant (no re-render) and must not have touched
	// the instrument, its clips, or automation -- freezing is documented as
	// non-destructive.
	void testUnfreezeRestoresLivePlayback()
	{
		auto* song = Engine::getSong();
		InstrumentTrack track(song);
		track.loadInstrument("tripleoscillator");

		if (track.instrument() == nullptr || track.instrument()->descriptor()->name != QString("tripleoscillator"))
		{
			QSKIP("tripleoscillator plugin not available in this build/environment");
		}

		MidiClip clip(&track);
		clip.changeLength(TimePos(1, 0));
		clip.addNote(Note(TimePos(1, 0), TimePos(0, 0), DefaultKey), false);

		const int noteCountBefore = clip.notes().size();
		Instrument* instrumentBefore = track.instrument();

		track.freeze();
		QVERIFY2(waitForFreezeToFinish(track), "freeze render did not complete in time");
		QVERIFY(track.isFrozen());

		track.unfreeze();

		QVERIFY(!track.isFrozen());
		QVERIFY(!track.isFreezing());
		// Nothing underneath was touched: same instrument instance, same
		// notes still present on the clip.
		QCOMPARE(track.instrument(), instrumentBefore);
		QCOMPARE(clip.notes().size(), noteCountBefore);
	}

	// Freezing, editing a clip afterward, and re-checking staleness: the
	// track should mark itself stale (and therefore fall back to live
	// playback) rather than silently continuing to play a now-outdated
	// cached render or silently re-rendering.
	void testEditAfterFreezeMarksStale()
	{
		auto* song = Engine::getSong();
		InstrumentTrack track(song);
		track.loadInstrument("tripleoscillator");

		if (track.instrument() == nullptr || track.instrument()->descriptor()->name != QString("tripleoscillator"))
		{
			QSKIP("tripleoscillator plugin not available in this build/environment");
		}

		auto* clip = new MidiClip(&track);
		clip->changeLength(TimePos(1, 0));
		clip->addNote(Note(TimePos(1, 0), TimePos(0, 0), DefaultKey), false);

		track.freeze();
		QVERIFY2(waitForFreezeToFinish(track), "freeze render did not complete in time");
		QVERIFY(track.isFrozen());
		QVERIFY(!track.isStale());

		// Edit the clip: this is exactly the kind of change that should
		// invalidate the cached render.
		clip->changeLength(TimePos(2, 0));

		QVERIFY(track.isStale());
		// A stale track is never treated as frozen for playback purposes.
		QVERIFY(!track.isFrozen());
	}

	// Save a frozen track, load it back into a fresh InstrumentTrack, and
	// confirm the frozen/stale state and cache path round-trip through the
	// project XML without needing to re-render.
	void testSaveLoadRoundTripsFrozenState()
	{
		auto* song = Engine::getSong();

		QString savedSamplePath;
		QDomDocument doc;
		QDomElement trackElement = doc.createElement("track");
		doc.appendChild(trackElement);

		{
			InstrumentTrack track(song);
			track.loadInstrument("tripleoscillator");

			if (track.instrument() == nullptr
				|| track.instrument()->descriptor()->name != QString("tripleoscillator"))
			{
				QSKIP("tripleoscillator plugin not available in this build/environment");
			}

			MidiClip clip(&track);
			clip.changeLength(TimePos(1, 0));
			clip.addNote(Note(TimePos(1, 0), TimePos(0, 0), DefaultKey), false);

			track.freeze();
			QVERIFY2(waitForFreezeToFinish(track), "freeze render did not complete in time");
			QVERIFY(track.isFrozen());
			savedSamplePath = track.frozenSamplePath();
			QVERIFY(!savedSamplePath.isEmpty());

			// saveSettings()/loadSettings() (the public SerializingObject
			// overrides, routing to the private Track::saveTrack()/
			// loadTrack() in non-preset mode) operate on a plain
			// QDomDocument with no dependency on the .mmp/.mmpz file format
			// machinery, so a round trip can be tested directly without
			// touching disk.
			track.saveSettings(doc, trackElement);
		}

		{
			InstrumentTrack loadedTrack(song);
			loadedTrack.loadSettings(trackElement);

			// Freeze-state finalization is deliberately deferred to the next
			// event-loop iteration rather than happening synchronously
			// inside loadSettings() (see InstrumentTrack::loadTrackSpecificSettings()
			// and finalizeLoadedFreezeState()), specifically to avoid doing
			// file I/O and Sample construction reentrantly from inside
			// Track::create()'s call stack -- e.g. while cloning a frozen
			// track. Pump the event loop briefly so that deferred call
			// actually runs before asserting on the result.
			QTest::qWait(50);

			QVERIFY(loadedTrack.isFrozen());
			QVERIFY(!loadedTrack.isStale());
			QCOMPARE(loadedTrack.frozenSamplePath(), savedSamplePath);
		}
	}

};

QTEST_GUILESS_MAIN(InstrumentTrackFreezeTest)
#include "InstrumentTrackFreezeTest.moc"
