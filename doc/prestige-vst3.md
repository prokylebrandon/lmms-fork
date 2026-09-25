# PRESTIGE — VST3 Host for LMMS: Phase 1 Developer Notes

This document covers the Phase 1 foundation layer only.  Phases 2–5 extend
it; consult their own notes for instrument, UI, effect, and hardening work.

---

## Vendored SDK

| Field            | Value |
|------------------|-------|
| Repository       | https://github.com/steinbergmedia/vst3sdk |
| Pinned tag       | `v3.7.14_build_34` (first tag on the MIT-licensed branch) |
| Submodule path   | `plugins/Vst3Base/vst3sdk` |
| CMake variable   | `LMMS_HAVE_VST3` (set when submodule is present and `WANT_VST3=ON`) |

### SDK License (as read from that commit's `LICENSE.txt`)

As of the 3.8.0 / `v3.7.14_build_34` release line, the core VST3 SDK is
released under the **MIT License**.  The full text is in
`plugins/Vst3Base/vst3sdk/LICENSE.txt`.  Key points:

- The core SDK (`pluginterfaces/`, `base/`, `public.sdk/source/vst/`)
  is MIT.
- Bundled sample plugins (`public.sdk/samples/`) carry the same MIT
  license (checked: all sample `CMakeLists.txt` and source directories
  reference the top-level `LICENSE.txt`).
- VSTGUI (in `vstgui4/`) has its own BSD-style license — but VSTGUI is
  **not vendored** by this phase (and not needed for hosting).

MIT is compatible with GPLv2-or-later, which is what every source file in
this repository uses.  No Steinberg proprietary agreement is needed.

---

## Architecture decision: in-process hosting

VST3 is hosted **in-process**, mirroring the LV2 precedent in this
codebase (`Lv2Manager`, `Lv2ControlBase`), not the VST2 out-of-process
bridge (`RemotePlugin` / `RemotePluginClient`).

**Rationale:** VST3 plugins are native `.so`/`.dll`/`.bundle` files built
for the same architecture as LMMS.  The VST2 bridge exists to handle
32-bit plugins and Wine bridging — neither applies to VST3.  In-process
hosting eliminates the RPC overhead and substantial code complexity of the
subprocess path.

**Accepted tradeoff:** A crashing or misbehaving VST3 plugin will bring
down the LMMS process.  VST2 is crash-isolated in a subprocess; VST3 is
not, in Phase 1.

**Mitigations in Phase 1:**
- All return values and pointer outputs from plugin interfaces are
  validated before use.
- `Vst3PluginInstance::load()` never throws; all failures return a
  displayable `LoadResult::error` string.
- Discovery of a bundle with no usable classes, or a component that
  fails `initialize()`, results in an error message, not a crash.

**Future direction:** Out-of-process VST3 via the existing `RemotePlugin`
machinery is a legitimate future option if crash isolation becomes a hard
requirement.  It is **out of scope for all five phases**.

---

## Host layer public API (for Phase 2)

Phase 2 builds the PRESTIGE instrument plugin (`plugins/Prestige/`) on top
of this layer.  The entire public API surface is in one header:

```
plugins/Vst3Base/Vst3PluginInstance.h
```

### Key types

| Type | Header | Purpose |
|------|--------|---------|
| `Vst3PluginInstance` | `Vst3PluginInstance.h` | Core host object; one per loaded plugin |
| `Vst3PluginInstance::LoadResult` | same | Returned by `load()`; holds instance or error |
| `Vst3ClassInfo` | `Vst3Types.h` | Metadata from `discoverClasses()` |
| `Vst3Parameter` | `Vst3Parameter.h` | Immutable param metadata; keyed by `id` (never index) |
| `Vst3ParamID` | `Vst3Types.h` | `uint32_t` alias for VST3 parameter ID |

### Factory / discovery

```cpp
// Enumerate classes inside a bundle without loading one:
std::vector<Vst3ClassInfo> classes =
    Vst3PluginInstance::discoverClasses(path, &error);

// Load class at index 0, 44100 Hz, 512 samples/block:
auto result = Vst3PluginInstance::load(path, 0, 44100.0, 512);
if (!result) { showError(result.error); return; }
auto& inst = *result.instance;
```

### Lifecycle

```cpp
inst.startProcessing();   // activate + setProcessing(true)
// ... audio thread calls processAudio() ...
inst.stopProcessing();    // setProcessing(false)
// destructor cleans up controller, component, module in the right order
```

### Audio (audio thread only)

```cpp
// inputs/outputs: interleaved stereo (L0 R0 L1 R1 …), numFrames frames.
// inputs may be nullptr for instrument plugins.
inst.processAudio(inputs, outputs, numFrames);
```

Only the plugin's **first main stereo output bus** is read.  Additional
output buses and input sidechain buses are ignored (Phase 1 limitation).

### MIDI / events (main thread, before processAudio)

```cpp
MidiEvent noteOn(MidiNoteOn, channel, key, velocity);
inst.queueMidiEvent(noteOn, sampleOffsetInBlock);
```

Events are delivered sample-accurately within the block.  The queue is
consumed and cleared inside `processAudio()`.

### Parameters (main thread)

```cpp
// Read metadata:
for (const auto& p : inst.parameters())
    qDebug() << p.id << p.title << p.defaultNormalisedValue;

// Queue a value change (normalised [0,1], keyed by param ID — never index):
inst.queueParameterChange(paramId, 0.75);

// Display string:
QString s = inst.parameterDisplayString(paramId, 0.75); // e.g. "440 Hz"
```

### State (main thread)

```cpp
QByteArray blob = inst.saveState();
inst.restoreState(blob);
```

State blob format: `[4B compLen][compBytes][4B ctrlLen][ctrlBytes]`
(little-endian uint32 lengths).  The format is an internal detail; use
`saveState`/`restoreState` to round-trip it.

---

## CMake variables for Phase 2

Phase 2's `plugins/Prestige/CMakeLists.txt` should:

```cmake
if(NOT LMMS_HAVE_VST3)
    return()
endif()

include(BuildPlugin)

BUILD_PLUGIN(prestige
    Prestige.cpp  ...
    MOCFILES Prestige.h ...
    EMBEDDED_RESOURCES logo.svg
)

target_link_libraries(prestige PRIVATE vst3base)
target_include_directories(prestige PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/Vst3Base"
    "${CMAKE_SOURCE_DIR}/plugins/Vst3Base/vst3sdk"
)
```

Gate on `LMMS_HAVE_VST3` (not `WANT_VST3`) — the same pattern as
`LMMS_HAVE_LV2` vs `WANT_LV2`.

Add `Prestige` to `cmake/modules/PluginList.cmake`'s `LMMS_PLUGIN_LIST`,
alongside `Vst3Base`.

---

## Stereo boundary

LMMS's audio pipeline is hard-coded stereo at its edges (`SampleFrame` =
`std::array<sample_t, 2>`).  `Vst3PluginInstance::processAudio()` presents
a stereo-interleaved interface to callers and handles the interleaved ↔
planar conversion internally.  The VST3-facing side supports arbitrary bus
layouts, but only the first main stereo output bus is read back.

Phase 2 instrument callers pass an interleaved `SampleFrame*` buffer
directly.

---

## Known Phase 1 limitations

- No transport / `ProcessContext` wired into `processAudio()` (BPM, beat
  position, etc.).  Phase 3 adds this.
- Only the first main stereo I/O bus pair is used.
- No crash isolation.  A misbehaving plugin crashes LMMS.
- No host context (`IHostApplication`) supplied to plugin `initialize()`
  calls.  Most plugins work fine without it; Phase 3 can add it if needed.
- No plugin GUI / editor (`IPlugView`).  Phase 2/3 add this.

---

## Phase 3 Part 2 additions (for Phase 4)

Part 2 hardened the plugin-lifecycle half of Vst3Base and PRESTIGE's own
teardown/replacement/state-save paths. Phase 4's VST3 *effect* builds on
the same `Vst3Base` layer PRESTIGE does, so — per Part 2's own
instructions — every public `Vst3Base` API change, the final state-
versioning scheme, and any general-purpose crash-containment technique are
recorded here rather than only in `plugins/Prestige`'s own comments.
Cross-reference: `PRESTIGE-Phase-3-Part-2.md`'s punch list; item numbers
below match it.

**Correction to this document's own "Known Phase 1 limitations" above:**
that list says "Phase 3 adds this" for transport/`ProcessContext`. That
has **not** happened — `processAudio()` still passes
`pd.processContext = nullptr` unchanged. Flagging this now rather than
leaving a stale forward-reference: if Phase 4's effect needs transport
position (tempo-synced effects are common — delays, gated effects), it
cannot assume this exists yet and should either add it itself or raise it
as a `Vst3Base`-level prerequisite before depending on it. The
`IHostApplication` gap noted in the same list is likewise still open, for
the same reason (out of scope for Part 2's punch list).

### Vst3Base public API changes (item 2)

All in `Vst3PluginInstance.h` / `Vst3Parameter.h`, additive — nothing from
Phase 1/2's surface changed shape or was removed.

| Addition | Purpose | Threading |
|---|---|---|
| `flushActiveNotes()` | Sends an explicit NoteOff for every (channel, key) the instance believes is held, delivered synchronously via one extra `processAudio()` call. Call before `stopProcessing()`, never after. | Caller's GUI-thread-equivalent; caller must additionally guarantee no concurrent `processAudio()` call for the duration — `Vst3PluginInstance` has no lock of its own around `processAudio()`. See PRESTIGE's `closePluginLocked()` for how it provides that guarantee (holds the same mutex `play()` takes before calling `processAudio()`). |
| `droppedMidiEventCount()` / `droppedParameterChangeCount()` | Diagnostics counters for the lock-free pending queues (see item 4 below); expected to stay 0. | Any thread (relaxed atomic load). |
| `hasSeparateControllerState()` | True when the component and controller are genuinely two separate VST3 objects rather than one object implementing both interfaces. | Main thread. |
| `saveComponentState()` / `saveControllerState()` | Serialise component/controller state as two independent blobs instead of one combined blob. `saveControllerState()` returns empty when `hasSeparateControllerState()` is false — nothing distinct to save. | Main thread. |
| `restoreComponentState(bytes)` / `restoreControllerState(bytes)` | Restore the pieces `saveComponentState()`/`saveControllerState()` produced. `restoreComponentState()` also syncs the controller via `setComponentState()`, same as the combined-blob `restoreState()` already did. | Main thread. |
| `Vst3Parameter::isHidden` | From VST3's `kIsHidden` flag. UI hint only — does not affect automatability/readability. PRESTIGE's parameter window hides these by default with a "show hidden" toggle; still builds a model and still allows automation. | N/A (metadata). |

The combined-blob `saveState()`/`restoreState()` from Phase 1 are
unchanged and still the right choice for a single-object plugin (still
the common case) — the new separate-piece API is additive, not a
replacement.

### State-versioning scheme, final shape through `kSaveVersion` 2 (item 2)

This is PRESTIGE-level (`plugins/Prestige/Prestige.h`/`.cpp`), not
`Vst3Base`, but Phase 4's effect should mirror the *scheme*, not
necessarily the exact XML shape:

```
<prestige version="2" bundlepath=".." classcid=".." pluginname=".."
          pluginvendor="..">
  <state format="combined">base64</state>
    -- OR, when hasSeparateControllerState() is true --
  <state format="separate">
    <component>base64</component>
    <controller>base64</controller>
  </state>
  <parameters><param id=".." .../>...</parameters>
  <ui editor="0|1" parameters="0|1"/>
</prestige>
```

Rules a Phase 4 reader/writer should copy:

- **Identity is never positional.** Plugin class identity is the VST3
  class UID (`classcid`), matched strictly on load (a bundle that no
  longer contains the recorded UID fails rather than silently loading a
  different class). Parameter identity is the VST3 parameter ID
  (`<param id>`), matched by ID when reapplying saved parameter state —
  never by array position, since a plugin update can reorder or drop
  parameters between a project's save and its later load.
- **`version` gates the whole element, not individual pieces.** A reader
  refuses (without altering the data) anything with `version` greater
  than its own `kSaveVersion`, and retains the element verbatim so
  re-saving doesn't lose it. Missing `version` = version 0 (pre-
  versioning; same layout as version 1 minus name/vendor/ui).
- **Every new `<state>` shape needs both a `format` attribute AND a
  version bump**, not one or the other. The `format` attribute alone
  isn't enough protection: a reader has to check it *before* calling
  `.text()`/equivalent, and an old reader that predates the attribute's
  meaning won't know to check it — that's what the version bump is for
  (forces old readers to refuse the file instead of misreading it). This
  is exactly the 1 → 2 bump `format="separate"` required.
- **Retain-verbatim, not partial-apply, for anything a reader doesn't
  fully understand.** A too-new version, or a plugin that's missing when
  the project loads, keeps the entire saved element (`m_retainedXml` in
  PRESTIGE) and writes it back unchanged rather than attempting a
  best-effort partial parse that could silently drop data.

### Crash-containment / concurrency patterns established (items 1 and 4)

Two patterns from Part 2 are general-purpose enough that Phase 4's effect
should reuse them rather than re-derive its own:

1. **Single teardown funnel, called under the same lock `play()` takes.**
   PRESTIGE's `closePluginLocked()` is the one function every unload/
   replace/reload path goes through, called while holding the mutex that
   `play()` also takes before touching the plugin. This is what makes
   "stale model/pointer left pointing at a torn-down plugin" a class of
   bug that has exactly one place to audit instead of N call sites, and
   it's also the precondition `flushActiveNotes()` leans on for its
   "nothing else can be calling `processAudio()` right now" requirement.
   An effect's teardown (parameters, any wet/dry or sidechain state) should
   go through the same shape: one funnel, called under the lock its own
   process-thread entry point takes.
2. **Lock-free MPMC ring buffer for GUI/audio-thread-crossing data**
   (`Vst3LockFreeQueue`, private to `Vst3PluginInstance.cpp` — not
   exposed, so Phase 4 will need its own copy or a promoted shared one).
   Bounded, fixed capacity, `push()`/`pop()` never allocate and never
   block; a full queue drops and counts rather than retrying, since
   retrying on a realtime thread against a stalled consumer is worse than
   a dropped event. Verified with a standalone stress test (4 producers /
   3 consumers, 800k items, clean under ThreadSanitizer/AddressSanitizer/
   UBSan) outside the LMMS build, since no `.vst3` fixture exists in CI to
   exercise it end-to-end — see the limitations below.

### Known limitations / open questions (NOT verified against a real plugin)

No `.vst3` fixture exists in this repository or CI (`Vst3BaseTest`'s
plugin-dependent cases `QSKIP` without one — see Part 1's own note on
this). Everything below is from code-level audit, not observed runtime
behaviour, and should be treated as such until checked against a real
instrument:

- `Vst3EventList::addMidiEvent()` (Part 1) converts MIDI CC, pitch bend,
  and program-change events to `kLegacyMIDICCOutEvent` for delivery *to*
  the plugin. Per the VST3 SDK's own naming/intent, this event type is
  documented for a MIDI-effect plugin's *output* direction; using it as a
  host-to-plugin input event is unconfirmed against real plugin behaviour.
  Not changed in Part 2 — flagging since it's adjacent to the MIDI-event
  work Part 2 did (note tracking, `flushActiveNotes()`), which reuses this
  same conversion path for its own NoteOff events (NoteOn/NoteOff use
  `kNoteOnEvent`/`kNoteOffEvent`, unaffected by this concern).
- `queueParameterChange()` calls `IEditController::setParamNormalized()`
  directly, and this can run on the **audio thread** when LMMS automation
  applies a value during playback (automation is evaluated on the audio
  thread; that write reaches the controller through
  `Vst3ParameterModel::onModelChanged()` → here). The VST3 spec treats
  the controller as host-UI-thread-only. Pre-existing since Part 1;
  Part 2 did not change or fix this. The right fix depends on a design
  choice (defer the controller sync to the GUI thread on a queued basis?
  skip the controller sync entirely for audio-thread-originated writes,
  since the processor already gets the value through the normal parameter
  queue regardless?) that's a judgment call, not a mechanical fix.
- `flushActiveNotes()` can only account for notes it was told about via
  `queueMidiEvent()`. A plugin that generates or holds a voice with no
  corresponding host-sent NoteOn (an internal arpeggiator/sequencer, for
  instance) can still ring past teardown; nothing in this codebase
  exercises that case.
- The lock-free queue and active-note-tracking logic were unit-verified
  standalone (outside the LMMS/VST3 build, using the real `MidiEvent.h`/
  `Midi.h`) but never inside a real `processAudio()`/plugin loop, since no
  fixture plugin is available.
- Item 3 from the Part 2 punch list (crash-containment audit: mid-session
  sample-rate/buffer-size changes, offline export driving, rapid editor
  open/close, project-reload-during-playback, etc.) has **not** been
  completed. It remains open.
- Item 6 (tests: round-trip, replacement, the automation feedback-loop
  case) has **not** been added yet.
- Presets (item 2's third bullet — `IUnitInfo`/`IProgramListData`) — not
  started.

### Cross-platform (item 5)

`Select-String` over `plugins\Prestige\*.cpp,*.h` and
`plugins\Vst3Base\*.cpp,*.h` (top-level, not recursive — deliberately
excludes the vendored `vst3sdk` submodule) for `.dll` / literal backslash
patterns came back clean. Bundle opening itself goes through the SDK's
own `VST3::Hosting::Module::create()`, so the macOS-bundle-as-directory
vs. Windows/Linux-single-file distinction is delegated to the SDK rather
than reimplemented per-platform here. Still unverified: the browse
dialog's behaviour on macOS/Linux (a Part 1 open question, unchanged by
Part 2 — this needs someone running it on those platforms, not a grep).
