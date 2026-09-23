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
