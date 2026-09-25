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
#include <atomic>
#include <cstdint>
#include <functional>
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
 *    **main / GUI thread**.  (queueMidiEvent()/queueParameterChange()/
 *    clearMidiQueue() are additionally safe to call from any thread: they
 *    only touch the lock-free pending queues -- see kQueueCapacity's
 *    comment below for why they need to tolerate concurrent producers
 *    *and* concurrent consumers, not just concurrent producers.)
 *  - processAudio() is called from the **audio thread** while the plugin is
 *    active.  The audio thread must never race against destruction: call
 *    stopProcessing() from the main thread and wait for it to complete
 *    before destroying this object.
 *  - Parameter state visible to the audio thread is pushed via
 *    queueParameterChange() (LMMS -> plugin) or via the plugin's own
 *    IComponentHandler::performEdit() (plugin editor -> host); the audio
 *    thread drains both through the IParameterChanges passed to process().
 *
 * Crash-isolation note
 * --------------------
 *  VST3 is hosted in-process (mirroring the LV2 precedent in this
 *  codebase, not the VST2 out-of-process bridge).  A crashing plugin will
 *  take down LMMS.  This is a documented, accepted tradeoff for Phase 1–2;
 *  see doc/prestige-vst3.md for details.
 *
 * Public API surface, Phase 1-2 plus Phase 3 Part 2 additions
 * -----------------------------------------------------------
 *  Header:  plugins/Vst3Base/Vst3PluginInstance.h
 *  Factory: Vst3PluginInstance::load(path, classIndex, sampleRate, blockSize)
 *  Audio:   processAudio(inputs, outputs, numFrames)
 *  MIDI:    queueMidiEvent(MidiEvent, sampleOffset), clearMidiQueue(),
 *           flushActiveNotes() [Part 2: explicit NoteOff for every note
 *           this instance believes is held, delivered synchronously --
 *           see its own doc comment for the threading requirement it
 *           places on the caller], droppedMidiEventCount() [Part 2,
 *           diagnostics]
 *  Params:  parameters(), queueParameterChange(id, normValue),
 *           setParameterEditedCallback(cb), droppedParameterChangeCount()
 *           [Part 2, diagnostics]
 *  Editor:  createEditor() / attachEditor() / closeEditor()
 *  State:   saveState() / restoreState(bytes) [combined component +
 *           controller blob]; Part 2 adds hasSeparateControllerState(),
 *           saveComponentState() / saveControllerState() and
 *           restoreComponentState(bytes) / restoreControllerState(bytes)
 *           for a plugin where those are genuinely two separate objects
 *  Info:    name(), vendor(), category(), isInstrument()
 *  Parameters: Vst3Parameter now carries isHidden (Part 2, from VST3's
 *           kIsHidden flag) alongside the existing isReadOnly/isBypass/
 *           isProgramChange/isAutomatable flags.
 */
#include "vst3base_export.h"

class VST3BASE_EXPORT Vst3PluginInstance
{
public:
    /// Called (on the GUI thread) when the plugin asks the host to resize the
    /// editor window.  The host must resize its native window to exactly
    /// width x height; onSize() is forwarded to the view afterwards.
    using EditorResizeCallback = std::function<void(int width, int height)>;

    /// Called (on the GUI thread) when the plugin's own editor changes a
    /// parameter (IComponentHandler::performEdit).  The controller already
    /// holds the new value: the callback must NOT write it back into the
    /// plugin, or the two sides will feed each other.
    using ParameterEditedCallback = std::function<void(Vst3ParamID id, double normalisedValue)>;

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
     * @p classIndex is the index into the factory's FULL class list, i.e. the
     * value reported in Vst3ClassInfo::classIndex by discoverClasses().
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
     * @param numFrames Number of sample frames in this block.  Must not
     *                 exceed the blockSize passed to load(); a larger block
     *                 is refused (outputs are zeroed) rather than allowed to
     *                 overrun the scratch buffers.
     *
     * Only the plugin's first main stereo output bus is read.  Additional
     * output buses are ignored (Phase 1 limitation, documented in
     * doc/prestige-vst3.md).  Input sidechain buses are left silent.
     */
    void processAudio(const float* inputs,
                      float*       outputs,
                      int          numFrames);

    // ------------------------------------------------------------------ //
    // MIDI / event input  (any thread, delivered on the next processAudio)
    // ------------------------------------------------------------------ //

    /**
     * Queue a MIDI event to be delivered in the next processAudio() call.
     * @p sampleOffset is the sample-accurate position within the upcoming block.
     */
    void queueMidiEvent(const MidiEvent& event, int sampleOffset);

    /** Discard all pending MIDI events (e.g. on transport stop). */
    void clearMidiQueue();

    /**
     * Diagnostics only: number of MIDI events dropped because the lock-free
     * queue (see kQueueCapacity) was full when queueMidiEvent() was called.
     * Expected to stay 0 for any real plugin/session; a nonzero value means
     * something is producing far more than one audio block's worth of MIDI
     * between processAudio() calls.
     */
    std::uint64_t droppedMidiEventCount() const { return m_droppedMidiEvents.load(std::memory_order_relaxed); }

    /**
     * Send an explicit NoteOff for every (channel, key) this instance
     * believes is currently held down -- tracked from queueMidiEvent()
     * calls, see m_activeNotes -- and deliver them into the plugin
     * synchronously (not on the next processAudio() call: callers use
     * this immediately before stopProcessing(), after which no further
     * processAudio() call is coming). A no-op if nothing is tracked as
     * active, or if the plugin is not currently processing.
     *
     * This is a best-effort clean release, not a guarantee: a plugin that
     * manages voices independently of the NoteOff events it is sent can
     * still cut off mid-note. What this fixes is PRESTIGE never even
     * trying -- see PrestigeInstrument::closePluginLocked()'s previously
     * flagged rough edge (it only cleared the pending-event queue, which
     * does nothing for a note the plugin already started on a prior
     * block).
     *
     * Threading: must be called from the same thread queueMidiEvent()'s
     * "GUI thread" callers use, and -- unlike every other queueMidiEvent()
     * caller -- the caller must additionally guarantee no other thread can
     * be inside processAudio() for the duration of this call.
     * Vst3PluginInstance has no lock of its own around processAudio(); see
     * PrestigeInstrument::closePluginLocked(), which holds m_pluginMutex
     * for the whole call (the same mutex PrestigeInstrument::play() takes
     * before calling processAudio()) -- that external lock is what makes
     * this safe there. A future caller (e.g. Phase 4's effect) needs the
     * equivalent guarantee from its own host integration; record this
     * requirement for that phase, per the Part 2 prompt's instruction to
     * track every public Vst3Base API change.
     */
    void flushActiveNotes();

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
     * LMMS -> plugin: set a parameter (normalised value in [0, 1]).  Updates
     * the controller and queues the change for the next processAudio() call.
     * Identity is always the VST3 parameter ID, never a positional index.
     */
    void queueParameterChange(Vst3ParamID id, double normalisedValue);

    /**
     * Diagnostics only: number of parameter changes dropped because the
     * lock-free queue (see kQueueCapacity) was full when queueParameterChange()
     * or the internal queueForProcessor() was called. Expected to stay 0; see
     * droppedMidiEventCount().
     */
    std::uint64_t droppedParameterChangeCount() const { return m_droppedParamChanges.load(std::memory_order_relaxed); }

    /**
     * Plugin -> LMMS: register a callback fired when the plugin's own
     * editor changes a parameter.  See ParameterEditedCallback for the
     * feedback-loop rule.  Pass an empty function to clear.
     */
    void setParameterEditedCallback(ParameterEditedCallback cb);

    /**
     * Return a formatted display string for a parameter value.
     * E.g. "440.0 Hz" or "0.5 dB".
     */
    QString parameterDisplayString(Vst3ParamID id, double normalisedValue) const;

    // ------------------------------------------------------------------ //
    // Native editor (IPlugView)  (main thread)
    // ------------------------------------------------------------------ //
    //
    // Two-step so the host can size its native window BEFORE the plugin
    // attaches to it:
    //
    //   1. createEditor()  - asks the controller for a view, checks the
    //                        platform type is supported, reads its initial
    //                        size, installs the IPlugFrame.
    //   2. attachEditor()  - attaches the view to a native parent window
    //                        (HWND / NSView* / X11 window id).
    //
    // closeEditor() is idempotent and safe at any point, including from the
    // destructor.  The editor never owns the processor or controller:
    // closing it does not touch plugin state, and a later createEditor()
    // builds a fresh view against the same controller.

    /**
     * Create the editor view.  On success @p widthOut / @p heightOut hold the
     * initial size (validated to a sane range).  On failure @p errorOut holds
     * a short user-facing message (e.g. the plugin simply has no editor);
     * technical detail goes to the debug log.
     */
    bool createEditor(int*                widthOut,
                      int*                heightOut,
                      EditorResizeCallback onResize,
                      QString*            errorOut = nullptr);

    /** Attach a view created by createEditor() to a native parent window. */
    bool attachEditor(void* nativeParent, QString* errorOut = nullptr);

    /** Detach and release the editor view (removed(), setFrame(nullptr),
     *  release), then the frame.  Does nothing if no editor exists. */
    void closeEditor();

    /** True between a successful createEditor() and closeEditor(). */
    bool isEditorOpen() const { return m_plugView != nullptr; }

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

    /**
     * True when the component and controller are genuinely two separate
     * VST3 objects (as opposed to one object implementing both interfaces
     * -- see initController()'s "single-object plugin" path). Callers that
     * want to store component/controller state as two separate pieces
     * (rather than the one combined blob saveState() produces) should only
     * do so when this is true; for a single-object plugin, saveState()'s
     * combined blob already IS just the component's own state; storing the
     * same thing twice under two different tags on save/restore is exactly
     * what saveControllerState() below refuses to do here.
     */
    bool hasSeparateControllerState() const;

    /**
     * Serialise ONLY the component's state. Always meaningful, whether or
     * not the controller is a separate object.  Returns an empty
     * QByteArray on failure.
     */
    QByteArray saveComponentState() const;

    /**
     * Serialise ONLY the controller's state.  Returns an empty QByteArray
     * when there is no controller, when it is the same object as the
     * component (hasSeparateControllerState() is false -- there is nothing
     * distinct to save), or on failure.
     */
    QByteArray saveControllerState() const;

    /**
     * Restore component state saved by saveComponentState(), and sync the
     * controller from it (IEditController::setComponentState()), same as
     * restoreState() already does for the combined blob.  Returns false on
     * failure or an empty @p data.
     */
    bool restoreComponentState(const QByteArray& data);

    /**
     * Restore controller state saved by saveControllerState().  Only valid
     * when hasSeparateControllerState() is true; returns false otherwise
     * (including for an empty @p data, since saveControllerState() never
     * produces one for a plugin where this call would make sense).
     */
    bool restoreControllerState(const QByteArray& data);

    // ------------------------------------------------------------------ //
    // CMake / build variables for Phase 2 reference
    // ------------------------------------------------------------------ //
    // Gate on:  LMMS_HAVE_VST3
    // Submodule: plugins/Vst3Base/vst3sdk  (pinned to v3.7.14_build_34 or later)
    // Header include path: plugins/Vst3Base/vst3sdk (added by Vst3Base target)

private:
    explicit Vst3PluginInstance() = default;

    // The host-callback objects (defined in the .cpp) call back into these.
    friend class Vst3ComponentHandlerImpl;
    friend class Vst3PlugFrameImpl;

    // Initialisation helpers (called from load())
    bool initFactory(const QString& bundlePath, QString& error);
    bool initComponent(int classIndex, QString& error);
    bool initController(QString& error);
    bool initAudioSetup(double sampleRate, int blockSize, QString& error);
    bool connectComponentController(QString& error);
    void loadParameters();
    void installComponentHandler();

    // Host-callback entry points (GUI thread)
    void onControllerEdit(Vst3ParamID id, double value);
    void onEditorResizeRequested(int width, int height);

    // Queue a parameter change for the processor WITHOUT touching the
    // controller (used when the controller is the origin of the change).
    void queueForProcessor(Vst3ParamID id, double value);

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
    void* m_componentHandler    = nullptr; // Vst3ComponentHandlerImpl*
    void* m_plugView            = nullptr; // IPlugView*
    void* m_plugFrame           = nullptr; // Vst3PlugFrameImpl*

    // --- Editor state (main thread) ---
    bool                 m_editorAttached = false;
    bool                 m_inEditorResize = false; // re-entrancy guard for resizeView/onSize
    EditorResizeCallback m_editorResizeCb;

    // --- Plugin -> host parameter edits (main thread) ---
    ParameterEditedCallback m_paramEditedCb;

    // --- Plugin metadata ---
    QString m_name;
    QString m_vendor;
    QString m_category;
    bool    m_isInstrument = false;

    // --- Audio setup ---
    double m_sampleRate = 44100.0;
    int    m_blockSize  = 512;
    bool   m_processing = false;
    bool   m_hasInputBus = false; // false for synths with no audio input bus

    // --- Per-block scratch buffers (allocated once in initAudioSetup) ---
    // These avoid heap allocation inside processAudio().
    std::vector<float>  m_inputBuf;   // planar stereo input  (L plane, R plane)
    std::vector<float>  m_outputBuf;  // planar stereo output (L plane, R plane)
    std::vector<float*> m_inputPtrs;  // pointers for VST3 channel pointers
    std::vector<float*> m_outputPtrs; // pointers for VST3 channel pointers

    // --- Parameters ---
    std::vector<Vst3Parameter> m_parameters;

    // --- Pending queues (any thread -> audio thread), lock-free -----------
    //
    // Phase 2's mutex-protected pending/active vector pairs are gone (see
    // doc/prestige-vst3.md's Phase 3 notes for the removed KNOWN LIMITATION
    // comment this replaces). Each queue is now a bounded, lock-free MPMC
    // ring buffer (Vst3LockFreeQueue, defined in the .cpp so this public
    // header does not need to expose the template/atomics -- the same
    // reasoning as the void* SDK pointers above). Multiple producers:
    // queueMidiEvent()/queueParameterChange() are called from the GUI
    // thread (manual edits) and, when automation is applied during
    // playback, from the audio thread. There are also two consumers in
    // practice, not one: processAudio() (audio thread) and clearMidiQueue()
    // (GUI thread, e.g. transport stop or plugin teardown), so the queue
    // has to tolerate concurrent consumers too -- hence MPMC, not an
    // SPSC ring restricted to one producer and one consumer.
    //
    // push() never allocates and never blocks: against a full queue it
    // drops the item and counts it (m_droppedMidiEvents / m_droppedParam-
    // Changes) rather than retrying, since a retry loop on a realtime
    // caller could stall against an audio thread that is itself stalled.
    // kQueueCapacity is sized well beyond one block's worth of activity
    // for any plugin encountered so far; the counters are the way to
    // confirm that holds for a specific heavy plugin/session rather than
    // assuming it.
    static constexpr std::size_t kQueueCapacity = 2048; // must be a power of two

    void* m_paramQueue = nullptr; // Vst3LockFreeQueue<PendingParamChange>*, owned
    void* m_midiQueue  = nullptr; // Vst3LockFreeQueue<PendingMidiEvent>*, owned

    std::atomic<std::uint64_t> m_droppedParamChanges{0};
    std::atomic<std::uint64_t> m_droppedMidiEvents{0};

    // --- Active-note tracking (updated from queueMidiEvent(), any thread) -
    //
    // Feeds flushActiveNotes() only (see its doc comment above). A plain
    // (channel, key) grid: MIDI notes are naturally identified by channel
    // + 7-bit pitch, and Vst3EventList (see addMidiEvent()) always assigns
    // VST3's noteId as -1 (host-unassigned) today, so there is no finer
    // VST3-side identity to track even if two notes at the same
    // (channel, key) briefly overlapped. Sized directly from Midi.h's
    // MidiChannelCount / MidiMaxKey rather than a duplicated magic number.
    //
    // NOT verified: whether LMMS can ever send two NoteOns for the same
    // (channel, key) without an intervening NoteOff (e.g. overlapping
    // legato/retrigger at the LMMS level) -- if so, this tracker treats
    // the second NoteOn as "still just one held note" and flushActiveNotes()
    // sends one NoteOff, not two, which is the standard interpretation but
    // worth confirming against real usage if a plugin ever seems to leave
    // a voice ringing after a flush.
    //
    // Deliberately asymmetric about when it updates (see queueMidiEvent()'s
    // matching comment in the .cpp): a NoteOn marks a note active even if
    // queueing it was dropped (harmless over-approximation -- worst case,
    // one spurious NoteOff later for a note the plugin never started); a
    // NoteOff/velocity-0-NoteOn only clears the flag when the queue push
    // actually succeeded, so a dropped NoteOff can never cause a stuck
    // note to be skipped by flushActiveNotes().
    std::atomic<bool> m_activeNotes[MidiChannelCount][MidiMaxKey + 1] {};
};

} // namespace lmms

#endif // LMMS_VST3_PLUGIN_INSTANCE_H
