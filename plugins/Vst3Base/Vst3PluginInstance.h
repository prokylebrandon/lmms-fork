/*
 * Vst3PluginInstance.h - In-process VST3 plugin host core
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

#ifndef LMMS_VST3_PLUGIN_INSTANCE_H
#define LMMS_VST3_PLUGIN_INSTANCE_H

#include "Vst3Types.h"
#include "Vst3Parameter.h"

#include <QString>
#include <QByteArray>
#include <memory>
#include <string>
#include <vector>
#include <optional>

// Forward declarations to avoid including heavy VST3 SDK headers here
namespace Steinberg
{
    class FUnknown;
    struct IPluginFactory;
    namespace Vst
    {
        class IComponent;
        class IAudioProcessor;
        class IEditController;
        class IConnectionPoint;
        class IEventList;
        class IParameterChanges;
        struct ProcessContext;
        struct BusInfo;
    }
}

#include "MidiEvent.h"

namespace lmms
{

class SampleFrame;

/**
 * @brief Core in-process VST3 plugin host.
 *
 * Vst3PluginInstance owns the lifecycle of exactly one VST3 plugin class
 * loaded from a .vst3 bundle.  It is deliberately not instrument- or
 * effect-specific — both Prestige (instrument, Phase 2) and a future
 * VstEffect3 (Phase 4) build on top of this object.
 *
 * Threading contract
 * ------------------
 *  - All public methods except processAudio() must be called from the
 *    **main / GUI thread**.
 *  - processAudio() is called from the **audio thread** while the plugin is
 *    active.  The audio thread must never race against destruction: call
 *    stopProcessing() from the main thread and wait for it to complete
 *    before destroying this object.
 *  - Parameter state visible to the audio thread is pushed via
 *    queueParameterChange() from the main thread; the audio thread reads it
 *    through the IParameterChanges passed into processAudio().
 *
 * Crash-isolation note
 * --------------------
 *  VST3 is hosted in-process (mirroring the LV2 precedent in this
 *  codebase, not the VST2 out-of-process bridge).  A crashing plugin will
 *  take down LMMS.  This is a documented, accepted tradeoff for Phase 1–2;
 *  see doc/prestige-vst3.md for details.
 *
 * Public API surface for Phase 2 (Prestige instrument)
 * -----------------------------------------------------
 *  Header:  plugins/Vst3Base/Vst3PluginInstance.h
 *  Factory: Vst3PluginInstance::load(path, classIndex, sampleRate, blockSize)
 *  Audio:   processAudio(inputs, outputs, numFrames, context, eventList, paramChanges)
 *  MIDI:    queueMidiEvent(MidiEvent, sampleOffset) [main thread, pre-process]
 *  Params:  parameters(), queueParameterChange(id, normValue)
 *  State:   saveState() / restoreState(bytes)
 *  Info:    name(), vendor(), category(), isInstrument()
 */
#include "vst3base_export.h"

class VST3BASE_EXPORT Vst3PluginInstance
{
public:
    /// Result of load(); holds either a valid instance or an error string.
    struct LoadResult
    {
        std::unique_ptr<Vst3PluginInstance> instance; // non-null on success
        QString                             error;    // non-empty on failure
        explicit operator bool() const { return instance != nullptr; }
    };

    /**
     * Load a VST3 bundle, pick the plugin class at @p classIndex, and
     * initialise it for stereo processing at @p sampleRate / @p blockSize.
     *
     * All failures are reported via LoadResult::error; this never throws.
     */
    static LoadResult load(const QString& bundlePath,
                           int            classIndex,
                           double         sampleRate,
                           int            blockSize);

    /**
     * Enumerate the plugin classes inside a bundle without fully
     * instantiating one.  Returns one Vst3ClassInfo per VST3 audio class.
     * Returns an empty list (and populates @p errorOut) on failure.
     */
    static std::vector<Vst3ClassInfo> discoverClasses(const QString& bundlePath,
                                                       QString*       errorOut = nullptr);

    ~Vst3PluginInstance();

    // Non-copyable, non-movable (owns raw COM/VST3 pointers)
    Vst3PluginInstance(const Vst3PluginInstance&)            = delete;
    Vst3PluginInstance& operator=(const Vst3PluginInstance&) = delete;
    Vst3PluginInstance(Vst3PluginInstance&&)                 = delete;
    Vst3PluginInstance& operator=(Vst3PluginInstance&&)      = delete;

    // ------------------------------------------------------------------ //
    // Plugin identity / metadata
    // ------------------------------------------------------------------ //

    QString name()     const { return m_name;     }
    QString vendor()   const { return m_vendor;   }
    QString category() const { return m_category; }

    /** True when the plugin's kVstAudioEffectClass sub-category contains
     *  "Instrument" or "Synth". */
    bool isInstrument() const { return m_isInstrument; }

    /** Number of main stereo output channels the adapter exposes to LMMS.
     *  Always 2 in Phase 1 (left + right). */
    int outputChannelCount() const { return 2; }

    // ------------------------------------------------------------------ //
    // Processing lifecycle  (main thread unless noted)
    // ------------------------------------------------------------------ //

    /**
     * Activate the processor and prepare it for audio processing.
     * Must be called before the first processAudio() call.
     */
    bool startProcessing(QString* errorOut = nullptr);

    /**
     * Flush any pending events, deactivate the processor.
     * Must be called from the **main thread** before destruction or
     * before a new startProcessing() call.
     */
    void stopProcessing();

    bool isProcessing() const { return m_processing; }

    // ------------------------------------------------------------------ //
    // Audio processing  (audio thread)
    // ------------------------------------------------------------------ //

    /**
     * Process one block of audio.
     *
     * @param inputs   Pointer to numFrames interleaved stereo input samples
     *                 (L0 R0 L1 R1 …), or nullptr for instrument plugins.
     * @param outputs  Pointer to numFrames interleaved stereo output samples.
     *                 Must be pre-allocated by the caller.
     * @param numFrames Number of sample frames in this block.
     *
     * Only the plugin's first main stereo output bus is read.  Additional
     * output buses are ignored (Phase 1 limitation, documented in
     * doc/prestige-vst3.md).  Input sidechain buses are left silent.
     *
     * No heap allocation occurs inside this method.
     */
    void processAudio(const float* inputs,
                      float*       outputs,
                      int          numFrames);

    // ------------------------------------------------------------------ //
    // MIDI / event input  (main thread, call before processAudio)
    // ------------------------------------------------------------------ //

    /**
     * Queue a MIDI event to be delivered in the next processAudio() call.
     * @p sampleOffset is the sample-accurate position within the upcoming block.
     * Thread: main thread only.
     */
    void queueMidiEvent(const MidiEvent& event, int sampleOffset);

    /** Discard all pending MIDI events (e.g. on transport stop). */
    void clearMidiQueue();

    // ------------------------------------------------------------------ //
    // Parameters  (main thread)
    // ------------------------------------------------------------------ //

    /** All parameters exposed by this plugin, indexed 0…n-1. */
    const std::vector<Vst3Parameter>& parameters() const { return m_parameters; }

    /** Find a parameter by its VST3 parameter ID (the authoritative key). */
    const Vst3Parameter* findParameter(Vst3ParamID id) const;

    /**
     * Read the current normalised value of a parameter from the controller.
     * Returns std::nullopt if the ID is unknown.
     */
    std::optional<double> getParameterNormalized(Vst3ParamID id) const;

    /**
     * Queue a parameter change (normalised value in [0, 1]) to be delivered
     * in the next processAudio() call.  Identity is always the VST3
     * parameter ID, never a positional index.
     * Thread: main thread only.
     */
    void queueParameterChange(Vst3ParamID id, double normalisedValue);

    /**
     * Return a formatted display string for a parameter value.
     * E.g. "440.0 Hz" or "0.5 dB".
     */
    QString parameterDisplayString(Vst3ParamID id, double normalisedValue) const;

    // ------------------------------------------------------------------ //
    // State serialisation
    // ------------------------------------------------------------------ //

    /**
     * Serialise both component and controller state into a single blob
     * suitable for storing in an LMMS project file.
     * Returns an empty QByteArray on failure.
     */
    QByteArray saveState() const;

    /**
     * Restore component and controller state from a blob produced by
     * saveState().  Returns false on failure.
     */
    bool restoreState(const QByteArray& data);

    // ------------------------------------------------------------------ //
    // CMake / build variables for Phase 2 reference
    // ------------------------------------------------------------------ //
    // Gate on:  LMMS_HAVE_VST3
    // Submodule: plugins/Vst3Base/vst3sdk  (pinned to v3.7.14_build_34 or later)
    // Header include path: plugins/Vst3Base/vst3sdk (added by Vst3Base target)

private:
    explicit Vst3PluginInstance() = default;

    // Initialisation helpers (called from load())
    bool initFactory(const QString& bundlePath, QString& error);
    bool initComponent(int classIndex, QString& error);
    bool initController(QString& error);
    bool initAudioSetup(double sampleRate, int blockSize, QString& error);
    bool connectComponentController(QString& error);
    void loadParameters();

    // Internal processing helpers
    void applyQueuedParameterChanges();
    void buildEventList();

    // --- VST3 interface pointers (main-thread owned) ---
    // Using raw void* here to avoid including SDK headers in this public
    // header; the implementation casts them to the correct types.
    void* m_moduleHandle        = nullptr; // dlopen handle
    void* m_factory             = nullptr; // IPluginFactory*
    void* m_component           = nullptr; // IComponent*
    void* m_audioProcessor      = nullptr; // IAudioProcessor*
    void* m_controller          = nullptr; // IEditController*
    void* m_compConnection      = nullptr; // IConnectionPoint* (component side)
    void* m_ctrlConnection      = nullptr; // IConnectionPoint* (controller side)

    // --- Plugin metadata ---
    QString m_name;
    QString m_vendor;
    QString m_category;
    bool    m_isInstrument = false;

    // --- Audio setup ---
    double m_sampleRate = 44100.0;
    int    m_blockSize  = 512;
    bool   m_processing = false;

    // --- Per-block scratch buffers (allocated once in initAudioSetup) ---
    // These avoid heap allocation inside processAudio().
    std::vector<float>  m_inputBuf;   // interleaved stereo input
    std::vector<float>  m_outputBuf;  // interleaved stereo output
    std::vector<float*> m_inputPtrs;  // pointers for VST3 channel pointers
    std::vector<float*> m_outputPtrs; // pointers for VST3 channel pointers

    // --- Parameters ---
    std::vector<Vst3Parameter> m_parameters;

    // --- Pending queues (main thread → audio thread, lock-free by design) ---
    // Simple vector; safe because queueMidiEvent/queueParameterChange and
    // processAudio are on the same thread pair and processAudio is never
    // concurrent with queue writes (startProcessing/stopProcessing provide
    // the synchronisation barrier).
    struct PendingParamChange { Vst3ParamID id; double value; };
    struct PendingMidiEvent   { MidiEvent event; int sampleOffset; };

    std::vector<PendingParamChange> m_pendingParamChanges;
    std::vector<PendingMidiEvent>   m_pendingMidiEvents;
};

} // namespace lmms

#endif // LMMS_VST3_PLUGIN_INSTANCE_H

