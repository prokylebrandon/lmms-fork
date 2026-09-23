/*
 * Vst3BaseTest.cpp - Unit tests for the Vst3Base host layer (Phase 1)
 *
 * Copyright (c) 2024 LMMS contributors
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

#ifdef LMMS_HAVE_VST3

#include <QtTest>
#include <QDir>
#include <QStandardPaths>

#include "Vst3PluginInstance.h"
#include "Vst3MemoryStream.h"
#include "Vst3EventList.h"
#include "Vst3ParameterChanges.h"
#include "MidiEvent.h"
#include "Midi.h"

namespace lmms
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Returns the path to the SDK's compiled "again" sample plugin, which is
/// built alongside the SDK during CMake configuration when
/// SMTG_ADD_VST3_PLUGINS_SAMPLES is ON. Falls back to an empty string if
/// not found — tests that depend on it will skip gracefully.
static QString agianPluginPath()
{
    // The sample plugin lands in the build directory when the SDK samples
    // are built. We look relative to the test binary's directory.
    const QString testBinDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        testBinDir + "/VST3/again.vst3",
        testBinDir + "/../VST3/again.vst3",
        testBinDir + "/../../VST3/again.vst3",
    };
    for (const auto& c : candidates)
        if (QDir(c).exists() || QFile::exists(c))
            return c;
    return {};
}

// ---------------------------------------------------------------------------

class Vst3BaseTest : public QObject
{
    Q_OBJECT

private slots:

    // -----------------------------------------------------------------------
    // 1. Discovery — invalid bundle
    // -----------------------------------------------------------------------
    void testDiscoverInvalidBundle()
    {
        QString error;
        const auto classes = Vst3PluginInstance::discoverClasses(
            "/nonexistent/path/fake.vst3", &error);

        QVERIFY(classes.empty());
        QVERIFY(!error.isEmpty());
    }

    // -----------------------------------------------------------------------
    // 2. Load — invalid bundle doesn't crash
    // -----------------------------------------------------------------------
    void testLoadInvalidBundle()
    {
        auto result = Vst3PluginInstance::load(
            "/nonexistent/path/fake.vst3", 0, 44100.0, 512);

        QVERIFY(!result);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.instance == nullptr);
    }

    // -----------------------------------------------------------------------
    // 3. Vst3MemoryStream — write then read round-trip
    // -----------------------------------------------------------------------
    void testMemoryStreamRoundTrip()
    {
        Vst3MemoryStream ws;

        const char data[] = "Hello VST3 stream";
        Steinberg::int32 written = 0;
        QCOMPARE(ws.write(const_cast<char*>(data), sizeof(data), &written),
                 Steinberg::kResultOk);
        QCOMPARE(written, static_cast<Steinberg::int32>(sizeof(data)));

        // Seek back and read
        ws.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);

        char buf[64] = {};
        Steinberg::int32 readBytes = 0;
        QCOMPARE(ws.read(buf, sizeof(data), &readBytes), Steinberg::kResultOk);
        QCOMPARE(readBytes, static_cast<Steinberg::int32>(sizeof(data)));
        QCOMPARE(QByteArray(buf, readBytes),
                 QByteArray(data, sizeof(data)));
    }

    // -----------------------------------------------------------------------
    // 4. Vst3MemoryStream — read-only stream
    // -----------------------------------------------------------------------
    void testMemoryStreamReadOnly()
    {
        const QByteArray src("read-only content");
        Vst3MemoryStream rs(src);

        char buf[64] = {};
        Steinberg::int32 readBytes = 0;
        QCOMPARE(rs.read(buf, src.size(), &readBytes), Steinberg::kResultOk);
        QCOMPARE(readBytes, src.size());
        QCOMPARE(QByteArray(buf, readBytes), src);

        // Write should fail on a read-only stream
        Steinberg::int32 written = 0;
        const auto res = rs.write(buf, 1, &written);
        QCOMPARE(res, Steinberg::kResultFalse);
    }

    // -----------------------------------------------------------------------
    // 5. Vst3EventList — NoteOn / NoteOff conversion
    // -----------------------------------------------------------------------
    void testEventListNoteOnOff()
    {
        Vst3EventList list;

        // NoteOn
        MidiEvent noteOn(MidiNoteOn, 0, 60, 100);
        list.addMidiEvent(noteOn, 0);

        // NoteOff
        MidiEvent noteOff(MidiNoteOff, 0, 60, 64);
        list.addMidiEvent(noteOff, 128);

        QCOMPARE(list.getEventCount(), 2);

        Steinberg::Vst::Event e{};
        QCOMPARE(list.getEvent(0, e), Steinberg::kResultOk);
        QCOMPARE(e.type, Steinberg::Vst::Event::kNoteOnEvent);
        QCOMPARE(e.noteOn.pitch, static_cast<Steinberg::int16>(60));
        QVERIFY(e.noteOn.velocity > 0.0f && e.noteOn.velocity <= 1.0f);
        QCOMPARE(e.sampleOffset, 0);

        QCOMPARE(list.getEvent(1, e), Steinberg::kResultOk);
        QCOMPARE(e.type, Steinberg::Vst::Event::kNoteOffEvent);
        QCOMPARE(e.noteOff.pitch, static_cast<Steinberg::int16>(60));
        QCOMPARE(e.sampleOffset, 128);
    }

    // -----------------------------------------------------------------------
    // 6. Vst3EventList — velocity-0 NoteOn treated as NoteOff
    // -----------------------------------------------------------------------
    void testEventListVelocityZeroNoteOn()
    {
        Vst3EventList list;
        MidiEvent vel0NoteOn(MidiNoteOn, 0, 69, 0); // vel=0 → NoteOff
        list.addMidiEvent(vel0NoteOn, 0);

        QCOMPARE(list.getEventCount(), 1);
        Steinberg::Vst::Event e{};
        list.getEvent(0, e);
        QCOMPARE(e.type, Steinberg::Vst::Event::kNoteOffEvent);
    }

    // -----------------------------------------------------------------------
    // 7. Vst3EventList — sample offset preservation
    // -----------------------------------------------------------------------
    void testEventListSampleOffsetPreserved()
    {
        Vst3EventList list;
        const int offset = 317;
        MidiEvent n(MidiNoteOn, 0, 48, 80);
        list.addMidiEvent(n, offset);

        Steinberg::Vst::Event e{};
        list.getEvent(0, e);
        QCOMPARE(e.sampleOffset, offset);
    }

    // -----------------------------------------------------------------------
    // 8. Vst3EventList — clear between blocks
    // -----------------------------------------------------------------------
    void testEventListClear()
    {
        Vst3EventList list;
        MidiEvent n(MidiNoteOn, 0, 60, 100);
        list.addMidiEvent(n, 0);
        QCOMPARE(list.getEventCount(), 1);
        list.clear();
        QCOMPARE(list.getEventCount(), 0);
    }

    // -----------------------------------------------------------------------
    // 9. Vst3ParameterChanges — basic add and retrieve
    // -----------------------------------------------------------------------
    void testParameterChanges()
    {
        Vst3ParameterChanges changes;
        changes.addChange(42, 0.75);
        changes.addChange(99, 0.1);

        QCOMPARE(changes.getParameterCount(), 2);

        auto* q0 = changes.getParameterData(0);
        QVERIFY(q0 != nullptr);
        QCOMPARE(q0->getParameterId(), static_cast<Steinberg::Vst::ParamID>(42));
        QCOMPARE(q0->getPointCount(), 1);

        Steinberg::int32 sampleOff = -1;
        Steinberg::Vst::ParamValue val = -1.0;
        QCOMPARE(q0->getPoint(0, sampleOff, val), Steinberg::kResultOk);
        QCOMPARE(val, 0.75);
    }

    // -----------------------------------------------------------------------
    // 10. Vst3ParameterChanges — clear between blocks
    // -----------------------------------------------------------------------
    void testParameterChangesClear()
    {
        Vst3ParameterChanges changes;
        changes.addChange(1, 0.5);
        QCOMPARE(changes.getParameterCount(), 1);
        changes.clear();
        QCOMPARE(changes.getParameterCount(), 0);
    }

    // -----------------------------------------------------------------------
    // 11–15. Plugin-dependent tests (skipped if sample plugin not available)
    // -----------------------------------------------------------------------

    void testDiscoverRealPlugin()
    {
        const QString path = agianPluginPath();
        if (path.isEmpty())
            QSKIP("SDK sample plugin 'again.vst3' not found in build dir — skipping");

        QString error;
        const auto classes = Vst3PluginInstance::discoverClasses(path, &error);
        QVERIFY(error.isEmpty());
        QVERIFY(!classes.empty());
        QVERIFY(!classes[0].name.isEmpty());
    }

    void testLoadAndLifecycle()
    {
        const QString path = agianPluginPath();
        if (path.isEmpty())
            QSKIP("SDK sample plugin 'again.vst3' not found in build dir — skipping");

        auto result = Vst3PluginInstance::load(path, 0, 44100.0, 512);
        QVERIFY2(result, qPrintable(result.error));

        auto& inst = *result.instance;
        QVERIFY(!inst.name().isEmpty());

        // Start → process one silent block → stop
        QVERIFY(inst.startProcessing());

        std::vector<float> in(512 * 2, 0.0f);
        std::vector<float> out(512 * 2, 0.0f);
        inst.processAudio(in.data(), out.data(), 512);

        inst.stopProcessing();
        QVERIFY(!inst.isProcessing());
    }

    void testParameterMetadata()
    {
        const QString path = agianPluginPath();
        if (path.isEmpty())
            QSKIP("SDK sample plugin 'again.vst3' not found in build dir — skipping");

        auto result = Vst3PluginInstance::load(path, 0, 44100.0, 512);
        QVERIFY(result);

        const auto& params = result.instance->parameters();
        QVERIFY(!params.empty());

        // All parameters must have valid IDs (not kVst3NoParamID)
        for (const auto& p : params)
        {
            QVERIFY(p.id != kVst3NoParamID);
            // Normalised default must be in [0, 1]
            QVERIFY(p.defaultNormalisedValue >= 0.0);
            QVERIFY(p.defaultNormalisedValue <= 1.0);
        }
    }

    void testStateRoundTrip()
    {
        const QString path = agianPluginPath();
        if (path.isEmpty())
            QSKIP("SDK sample plugin 'again.vst3' not found in build dir — skipping");

        auto r1 = Vst3PluginInstance::load(path, 0, 44100.0, 512);
        QVERIFY(r1);

        // Tweak a parameter so state is non-default
        if (!r1.instance->parameters().empty())
        {
            const auto& p = r1.instance->parameters()[0];
            r1.instance->queueParameterChange(p.id, 0.3333);
        }

        const QByteArray savedState = r1.instance->saveState();
        QVERIFY(!savedState.isEmpty());

        // Load a fresh instance and restore
        auto r2 = Vst3PluginInstance::load(path, 0, 44100.0, 512);
        QVERIFY(r2);
        QVERIFY(r2.instance->restoreState(savedState));

        // Verify: saveState again and compare blobs
        const QByteArray restored = r2.instance->saveState();
        QCOMPARE(restored, savedState);
    }

    void testMidiEventQueuing()
    {
        const QString path = agianPluginPath();
        if (path.isEmpty())
            QSKIP("SDK sample plugin 'again.vst3' not found in build dir — skipping");

        auto result = Vst3PluginInstance::load(path, 0, 44100.0, 512);
        QVERIFY(result);
        QVERIFY(result.instance->startProcessing());

        // Queue a note-on, process a block (must not crash)
        MidiEvent noteOn(MidiNoteOn, 0, 60, 100);
        result.instance->queueMidiEvent(noteOn, 0);

        std::vector<float> in(512 * 2, 0.0f);
        std::vector<float> out(512 * 2, 0.0f);
        result.instance->processAudio(in.data(), out.data(), 512);

        result.instance->stopProcessing();
    }
};

} // namespace lmms

QTEST_GUILESS_MAIN(lmms::Vst3BaseTest)
#include "Vst3BaseTest.moc"

#endif // LMMS_HAVE_VST3
