# PRESTIGE - VST3 Hosting for LMMS (Consolidated Developer Notes)

This is the single reference document for the PRESTIGE VST3 work in this fork
(`plugins/Vst3Base`, `plugins/Prestige`, `plugins/Vst3Effect`). It replaces the
earlier per-phase notes (`prestige-vst3.md` Phase 1 notes and
`HANDOFF-Phase-4c.md`) and folds in the Phase 5 changes.

Honesty convention used throughout: statements marked **(verified)** were
confirmed by reading code or running something; **(from commit message)** means
taken from a commit description and not independently re-read; **(unverified)**
means audit/inference only, never observed against a real plugin.

---

## 1. Phase overview

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | Host foundation layer: `Vst3Base` (module load, discovery, parameters, audio, MIDI events, state) | Done |
| 2 | PRESTIGE instrument plugin (`plugins/Prestige`) built on `Vst3Base` | Done |
| 3 (Part 1 + Part 2) | Instrument UI/editor; Part 2 hardened lifecycle: teardown funnel, lock-free queues, active-note flush, split state, versioned save format | Done, with open items (section 12) |
| 4 (incl. 4c) | VST3 effect (`plugins/Vst3Effect`); 4c: preset state-restore bug fix and per-parameter knob grid | Done, with known gaps (section 9) |
| 5 | Integration and hardening (commit `0eb1cc9d3`, cleanup `7e0810f19`) | Committed and merged to master; not yet verified against a real plugin |

Remaining open work is consolidated in section 12.

---

## 2. Vendored SDK and licence

| Field            | Value |
|------------------|-------|
| Repository       | https://github.com/steinbergmedia/vst3sdk |
| Pinned tag       | `v3.7.14_build_55` (commit `43b4e366ff84afc9b4247776acbf5e234683b77f`) |
| Submodule path   | `plugins/Vst3Base/vst3sdk` |
| CMake variable   | `LMMS_HAVE_VST3` (set when submodule is present and `WANT_VST3=ON`) |

The pinned commit's LICENSE.txt (copyright 2025, Steinberg Media Technologies
GmbH) offers the SDK under EITHER of two licences, at the user's choice:

a) the proprietary **Steinberg VST3 License**: the SDK may not be distributed in
   parts or in its entirety without prior written agreement from Steinberg, and
   publishing software under it requires a License Agreement signed by
   Steinberg; or
b) the **GNU General Public License, version 3 (GPLv3)**.

**This is not MIT.** Steinberg moved the SDK to MIT only with 3.8.0; this pin
(v3.7.14) predates that. An earlier revision of these notes said MIT and was
wrong (corrected in Phase 5, together with the `.gitmodules` pin fix).

Consequences for LMMS and packagers:

- PRESTIGE uses the **GPLv3** option. Any build that includes the `vst3base`
  library (which compiles SDK sources) must be distributed under GPLv3 terms,
  including the corresponding source. PRESTIGE source files carry
  GPLv2-or-later headers, so the combined work can be offered under GPLv3; it
  cannot be distributed as GPLv2-only.
- A distributor who wants the proprietary option must first obtain a signed
  agreement from Steinberg.
- LICENSE.txt applies only to files that reference it; other SDK files carry
  their own embedded licence text, which applies to them.
- Use of the VST name and logo is governed by Steinberg's VST usage guidelines.
- Building with `WANT_VST3=OFF`, or without the submodule, leaves the SDK out
  of the build entirely.
- VSTGUI (`vstgui4/`) has its own licence but is **not vendored** (and is not
  needed for hosting).

---

## 3. Architecture decision: in-process hosting

VST3 is hosted **in-process**, mirroring the LV2 precedent in this codebase
(`Lv2Manager`, `Lv2ControlBase`), not the VST2 out-of-process bridge
(`RemotePlugin` / `RemotePluginClient`).

**Rationale:** VST3 plugins are native `.so`/`.dll`/`.bundle` files built for
the same architecture as LMMS. The VST2 bridge exists for 32-bit plugins and
Wine bridging - neither applies to VST3. In-process hosting avoids the RPC
overhead and the code complexity of the subprocess path.

**Accepted tradeoff:** a crashing or misbehaving VST3 plugin brings down the
LMMS process. VST2 is crash-isolated in a subprocess; VST3 is not.

**Mitigations:**
- All return values and pointer outputs from plugin interfaces are validated
  before use (extended in Phase 5: VST3 result codes are checked throughout,
  from commit message).
- `Vst3PluginInstance::load()` never throws; all failures return a displayable
  `LoadResult::error` string.
- Discovery of a bundle with no usable classes, or a component that fails
  `initialize()`, yields an error message, not a crash.

**Future direction:** out-of-process VST3 via the existing `RemotePlugin`
machinery is a legitimate option if crash isolation becomes a hard
requirement. It is **out of scope for all five phases**.

---

## 4. Host layer public API (`Vst3Base`)

The public API surface is in one header: `plugins/Vst3Base/Vst3PluginInstance.h`.

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
// inputs/outputs: interleaved stereo (L0 R0 L1 R1 ...), numFrames frames.
// inputs may be nullptr for instrument plugins.
inst.processAudio(inputs, outputs, numFrames);
```

### MIDI / events (main thread, before processAudio)

```cpp
MidiEvent noteOn(MidiNoteOn, channel, key, velocity);
inst.queueMidiEvent(noteOn, sampleOffsetInBlock);
```

Events are delivered sample-accurately within the block. The queue is consumed
and cleared inside `processAudio()`.

### Parameters (main thread)

```cpp
for (const auto& p : inst.parameters())
    qDebug() << p.id << p.title << p.defaultNormalisedValue;

// Queue a value change (normalised [0,1], keyed by param ID - never index):
inst.queueParameterChange(paramId, 0.75);

QString s = inst.parameterDisplayString(paramId, 0.75); // e.g. "440 Hz"
```

### State (main thread)

```cpp
QByteArray blob = inst.saveState();
inst.restoreState(blob);
```

Combined blob format: `[4B compLen][compBytes][4B ctrlLen][ctrlBytes]`
(little-endian uint32 lengths). Internal detail; use `saveState`/`restoreState`
to round-trip it.

### Transport (added in Phase 5)

```cpp
inst.setTransport(tempoBpm, playing, projectTimeQuarters,
                  timeSigNumerator, timeSigDenominator);
```

`processAudio()` now builds a `Steinberg::Vst::ProcessContext` (tempo, time
signature, project time in quarters, playing flag; `kOffline` set during
export per commit message) and passes it to the plugin when transport has been
set. **(verified in code via grep)** Both `PrestigeInstrument` and
`Vst3Effect` feed it from the song's tempo and play/export state. Before
Phase 5, `pd.processContext` was always `nullptr`.

### Additions from Phase 3 Part 2 (all additive)

| Addition | Purpose | Threading |
|---|---|---|
| `flushActiveNotes()` | Sends an explicit NoteOff for every (channel, key) the instance believes is held, via one extra `processAudio()` call. Call before `stopProcessing()`, never after. | GUI-thread-equivalent; caller must guarantee no concurrent `processAudio()` (the instance has no lock of its own). See PRESTIGE's `closePluginLocked()`, which holds the same mutex `play()` takes. |
| `droppedMidiEventCount()` / `droppedParameterChangeCount()` | Diagnostic counters for the lock-free pending queues; expected to stay 0. | Any thread (relaxed atomic load). |
| `hasSeparateControllerState()` | True when component and controller are two genuinely separate VST3 objects. | Main thread. |
| `saveComponentState()` / `saveControllerState()` | Serialise component/controller state as two independent blobs. `saveControllerState()` returns empty when `hasSeparateControllerState()` is false. | Main thread. |
| `restoreComponentState(bytes)` / `restoreControllerState(bytes)` | Restore the pieces above. `restoreComponentState()` also syncs the controller via `setComponentState()`. | Main thread. |
| `Vst3Parameter::isHidden` | From VST3 `kIsHidden`. UI hint only; does not affect automatability. | Metadata. |

The combined-blob `saveState()`/`restoreState()` remain the right choice for
single-object plugins (the common case).

---

## 5. Stereo boundary and buses

LMMS's audio pipeline is hard-coded stereo at its edges (`SampleFrame` =
`std::array<sample_t, 2>`). `processAudio()` presents a stereo-interleaved
interface and does the interleaved <-> planar conversion internally.

Phases 1-4 read only the plugin's first main stereo output bus and ignored
additional output and sidechain input buses. Phase 5 added "bus negotiation"
(from commit message). **The exact multi-bus behaviour after Phase 5 has not
been re-audited for this document** - check `Vst3PluginInstance.cpp` before
relying on sidechain or extra-output support.

---

## 6. CMake

`plugins/Prestige/CMakeLists.txt`, gated on `LMMS_HAVE_VST3` (not `WANT_VST3`),
same pattern as `LMMS_HAVE_LV2` vs `WANT_LV2`:

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

`Prestige` is listed in `cmake/modules/PluginList.cmake`'s `LMMS_PLUGIN_LIST`
alongside `Vst3Base`.

---

## 7. Save format and versioning

PRESTIGE-level (`plugins/Prestige/Prestige.h`/`.cpp`); the effect mirrors the
scheme. Current shape (`kSaveVersion` 2):

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

Rules any reader/writer must follow:

- **Identity is never positional.** Plugin class identity is the VST3 class UID
  (`classcid`), matched strictly on load (a bundle that no longer contains the
  recorded UID fails rather than silently loading a different class).
  Parameter identity is the VST3 parameter ID (`<param id>`), never array
  position, since a plugin update can reorder or drop parameters.
- **`version` gates the whole element.** A reader refuses (without altering
  the data) anything with `version` greater than its own `kSaveVersion`, and
  retains the element verbatim so re-saving does not lose it. Missing
  `version` = version 0 (pre-versioning).
- **Every new `<state>` shape needs both a `format` attribute AND a version
  bump.** An old reader that predates the attribute cannot know to check it;
  the version bump forces it to refuse the file instead of misreading it.
  This is exactly why `format="separate"` required the 1 -> 2 bump.
- **Retain verbatim, never partially apply**, anything a reader does not fully
  understand (too-new version, or plugin missing at load): keep the whole
  saved element (`m_retainedXml`) and write it back unchanged.

Phase 4c did not change the `<state>` shape, so no version bump was needed
there; projects saved before the 4c fix load as before.

---

## 8. Concurrency and crash-containment patterns

1. **Single teardown funnel under the process lock.** PRESTIGE's
   `closePluginLocked()` is the one function every unload/replace/reload path
   goes through, called under the mutex `play()` also takes before touching
   the plugin. One place to audit; also the precondition for
   `flushActiveNotes()`. An effect's teardown should follow the same shape.
2. **Lock-free bounded MPMC ring buffer** for GUI/audio-thread-crossing data
   (`Vst3LockFreeQueue`, private to `Vst3PluginInstance.cpp`). `push()`/`pop()`
   never allocate or block; a full queue drops and counts rather than retrying.
   Verified with a standalone stress test (4 producers / 3 consumers, 800k
   items, clean under ThreadSanitizer/AddressSanitizer/UBSan) outside the LMMS
   build. Phase 5 raised the parameter-change cap from 128 to 4096 and
   coalesces parameter queues (commits `12d119bae`, `0eb1cc9d3`).

---

## 9. Vst3Effect: state-restore fix and per-parameter knobs (Phase 4c)

### 9.1 Root cause of the preset bug (verified in the files)

Both symptoms (effect and instrument) shared one root cause. In
`Vst3EffectControls::applySavedElement()` and
`PrestigeInstrument::applySavedElement()`, project load runs:

1. `restoreState()` / `restoreComponentState()` - correctly restores the
   plugin's own preset/program from its saved blob.
2. Every `Vst3ParameterModel` is refreshed from the plugin's new values.
3. The saved `<parameters>` block is walked and
   `Vst3ParameterModel::loadSettings()` is called for every saved `<param>`.

Step 3's `loadSettings()` called `AutomatableModel::loadSettings()`, which ends
with `setValue()`. That fires `dataChanged()` ->
`Vst3ParameterModel::onModelChanged()` -> `queueParameterChange()`, pushing the
old, previously saved value back into the plugin and silently overwriting the
preset restored in step 1.

- **No preset loaded:** saved values equal the plugin defaults, so re-applying
  is a no-op - looks fine.
- **Preset loaded:** stale saved values clobber the preset's values right
  after restore, with no warning (the blob restore itself succeeded).

### 9.2 The fix

`Vst3ParameterModel::loadSettings()` (`plugins/Vst3Base/Vst3ParameterModel.{h,cpp}`)
checks `m_info.isProgramChange`. For a program-change parameter the saved value
is still loaded into the model (so an automation clip or controller connection
survives a reload), but the resulting forward-to-plugin is suppressed, reusing
the existing `m_settingFromPlugin` guard. Shared-code fix: it applies to both
`Vst3EffectControls` and `PrestigeInstrument`.

A later Prestige commit, `3ff1e30a4` ("trust cleanly restored plugin state over
saved parameter values"), tightened the instrument's restore path further.

### 9.3 Secondary bugs fixed in the same review (inference, not confirmed against a real plugin)

- `saveState()` ignored `getState()` return values and could write partial or
  empty bytes as a successful save. Now checked, logged via `qWarning`, and
  returns an empty `QByteArray` on failure.
- `restoreState()`'s length-prefix bounds check truncated a `uint32_t` to a
  signed `int` before comparing, allowing an out-of-bounds read from
  corrupted/foreign data. Now compared as `uint64_t`; the controller-length
  field also gets a bounds check.
- `setComponentState()` / `setState()` results were silently ignored; now
  logged via `qWarning` when non-OK and non-`kNotImplemented`, with pass/fail
  behaviour unchanged.

### 9.4 Per-parameter knob grid

New files: `plugins/Vst3Effect/Vst3ParameterGrid.{h,cpp}`.

- Scrollable grid (`QScrollArea` + `QGridLayout`, 4 columns) of real LMMS `Knob`
  widgets, one per exposed parameter, bound via
  `Knob::setModel(parameter->valueModel())` - so right-click automate/connect
  and drag-to-automation-clip work with no custom code.
- Search box filters by title, unit string, and numeric/hex VST3 ID.
- **Lazy creation:** only filtered-in parameters within the current page are
  built (64 initially, +64 per "Show more" or on scrolling near the bottom).
- Flag handling:
  - `isHidden` - not shown.
  - `isReadOnly` - shown, `Knob::setEnabled(false)`; writes also refused at the
    model level.
  - `isProgramChange` - shown as a stepped knob with a "(program)" suffix.
  - `isBypass` - shown as a stepped knob with a "(bypass)" suffix (not a
    checkbox - see known gaps).
  - `stepCount > 0` - the `FloatModel` already has a matching step size, so a
    plain `Knob` snaps to discrete values.
- Value text uses the plugin's own formatted string
  (`Vst3ParameterModel::formattedValue()`).
- The "Show editor" toggle and its window-hosting code are unchanged.
- Plugin-editor -> host sync needed no new code
  (`onPluginParameterEdited()` -> `setValueFromPlugin()` already updates the
  bound model).

### 9.5 Lifetime bug found and fixed

`Vst3EffectControlDialog` (and its `Vst3ParameterGrid`) is owned by `EffectView`,
with a lifetime independent of `Vst3Effect`/`Vst3EffectControls`. Teardown could
run while a dialog with the knob grid was open, leaving every `Knob` pointing at
a freed model (use-after-free).

Fix: new signal `Vst3EffectControls::parameterModelsAboutToClear()`, emitted
first in `teardownParameterModels()`, before anything is detached. The dialog
connects (direct connection) and calls `m_grid->clearParameters()`. Mirrors the
ordering contract `Vst3ParameterWindow::clearParameters()` documents on the
Prestige side.

**Not exercised** against the real "remove effect from chain while its dialog
is open" sequence - test by hand (section 11).

### 9.6 Known gaps (real, not overlooked)

- **`isBypass` is not a literal checkbox.** `Vst3ParameterModel` exposes only a
  `FloatModel`; `LedCheckBox` binds to a `BoolModel`-family model, and
  `AutomatableButton.h`'s `setModel()` contract was never confirmed. It works
  as a two-position stepped knob. A real `LedCheckBox` needs
  `AutomatableButton.h` reviewed first.
- **`isAutomatable == false` is not enforced.** Neither `AutomatableModel.h`
  nor `Model.h` has a per-model "refuse automation" capability, and
  `Vst3ParameterModel` never reads the flag. Enforcing it needs either a new
  model-level capability (affects all of LMMS) or a `Knob` subclass overriding
  the context menu (needs `AutomatableModelView.cpp` reviewed).
- **Grid column count is fixed at 4**, not responsive to dialog width.

---

## 10. Phase 5: integration and hardening

Commit `0eb1cc9d3` (10 files, +413/-60) and cleanup commit `7e0810f19`.
Items below are **(from commit message)** unless marked verified.

- VST3 result codes checked throughout.
- Per-block event and parameter buffers reused (no per-block allocation).
- Parameter queues coalesced; parameter-change cap raised to 4096.
- `IMidiMapping` used for CC and pitch bend (replaces reliance on
  `kLegacyMIDICCOutEvent` for those; see section 12 for what remains
  unverified).
- Controller sync moved to the GUI thread, addressing the earlier audio-thread
  `setParamNormalized` concern.
- Sample-rate and block-size reconfigure support.
- Bus negotiation.
- `ProcessContext` (transport) wired in - **verified** via grep of
  `Vst3PluginInstance.cpp`, `Prestige.cpp`, `Vst3Effect.cpp`.
- `kOffline` processing mode during export.
- Standard VST3 folders searched during discovery
  (`Vst3SubPluginFeatures.cpp` changed).
- GPLv3 licence documentation and SDK pin fix (`.gitmodules`).
- Cleanup (`7e0810f19`): leftover state diagnostic logging removed and EOF
  newlines restored in `ConfigManager.h`, `SubWindow.h`, `fenv.h`.

Later related commits on master: `40e353510` (per-note MPE-style bend for
plugins without tuning expression), `d8614912f` (per-note tuning at the latest
event offset), `39a4527c2` (pitch-bend release tails, per-note VST3 tuning).

---

## 11. Manual test checklist (still to be run against real plugins)

No `.vst3` with a real preset/program list exists in CI (the SDK's `again`
sample has no program-change parameter), so none of the following has been
exercised end to end.

1. Load a VST3 **instrument** (Prestige) whose plugin has factory presets.
2. Load a preset via the plugin's own editor.
3. Save the project, close LMMS, reopen, load the project.
4. Confirm no "Plugin state could not be restored." warning and that the
   preset's sound is actually back.
5. Repeat 1-4 with a VST3 **effect** (Vst3Effect).
6. Open an old project saved before the 4c fix and confirm it opens normally.
7. Effect knob grid: confirm the grid appears; right-click a knob -> create an
   automation clip; play and confirm the plugin responds. Move a knob directly
   and confirm processing changes. Open the native editor ("Show editor"), move
   a control, confirm the matching grid knob moves.
8. Test a plugin with an `isBypass` parameter and one with a real
   `isProgramChange` parameter.
9. Test a plugin with more than ~100 parameters ("Show more" paging with the
   search filter).
10. Open a VST3 effect's dialog, then remove that effect from its chain with
    the dialog open; confirm no crash (section 9.5).
11. Phase 5 specifics: tempo-synced effect follows project tempo and play
    state; export (offline) renders correctly; change sample rate / buffer
    size mid-session; pitch bend and CC reach a plugin via `IMidiMapping`.

Test coverage that does exist: `tests/src/plugins/Vst3BaseTest.cpp` includes
`testProgramChangeParamNotReappliedOnLoad()` and
`testOrdinaryParamStillReappliedOnLoad()`, which exercise
`Vst3ParameterModel::loadSettings()` in isolation around the `again.vst3`
plugin with a synthetic program-change parameter. They do not cover the full
`applySavedElement()` sequence against a real preset list. Plugin-dependent
cases `QSKIP` without a fixture.

---

## 12. Open items and known limitations

### Resolved since the original notes
- Transport / `ProcessContext`: done in Phase 5 (verified in code).
- Audio-thread controller sync (`setParamNormalized` on the audio thread):
  addressed in Phase 5 (from commit message; not re-read).
- MIDI CC / pitch bend via `IMidiMapping`: added in Phase 5 (from commit
  message).
- Mid-session sample-rate / buffer-size reconfigure and offline export
  driving: addressed in Phase 5 (from commit message). The broader
  crash-containment audit (rapid editor open/close, project reload during
  playback, etc.) has no record of being completed.

### Still open
- **`IHostApplication`:** no host context is passed to plugins (grep for it
  finds nothing in `Vst3PluginInstance.cpp`). Most plugins work without it.
- **Presets** (`IUnitInfo` / `IProgramListData`): not started.
- **Tests:** round-trip, replacement, and the automation feedback-loop test are
  not added. Phase 5 changed no test files. No `.vst3` fixture in CI.
- **`isBypass` checkbox, `isAutomatable` enforcement, responsive grid columns:**
  see section 9.6.
- **Manual verification:** section 11 is unrun.
- **Bus negotiation details:** re-audit what multi-bus / sidechain support
  actually exists (section 5).
- **`flushActiveNotes()` limit:** only accounts for notes it was told about via
  `queueMidiEvent()`. A plugin voice with no host-sent NoteOn (internal
  arpeggiator/sequencer) can ring past teardown.
- **Fixture-less verification:** the lock-free queue and note-tracking logic
  were verified standalone, never inside a real `processAudio()`/plugin loop.
- **Browse dialog on macOS/Linux:** unverified; needs someone to run it there.

### Temporary diagnostics still in the code (remove once behaviour is confirmed)
- `plugins/Prestige/Prestige.cpp` (~lines 979-1003): `diagBlobWon` /
  `diagSavedApplied` counters and the `qWarning` "restore summary".
- `plugins/Vst3Base/Vst3PluginInstance.cpp` (~lines 1042-1053 and 1092-1095):
  note-expression / `IMidiMapping` `qWarning` diagnostics (introduced by
  `d8614912f`, labelled temporary).
- `plugins/Vst3Base/Vst3PluginInstance.cpp` ~line 1753: a leftover
  "Diagnostics for Task A" comment block that references a deleted per-phase
  document - review and remove.

---

## 13. Workarounds still in place - do not remove without replacing

- `plugins/Vst3Base/CMakeLists.txt`:
  `if(MSVC) target_link_options(vst3base PRIVATE /FORCE:MULTIPLE)`. Needed for
  the LNK2005/LNK1169 duplicate-symbol issue from `vst3base.dll` linking
  `lmms.lib`.
- `tests/CMakeLists.txt`: the Windows `PATH` for `Vst3BaseTest` also gets
  `$<TARGET_FILE_DIR:lmms>`. Unverified whether it fixed the `0xc0000135` exit.
- macOS `@rpath/libvst3base.dylib` CPack warning: untouched.

---

## 14. Cross-platform notes

A `Select-String` over `plugins\Prestige\*.cpp,*.h` and
`plugins\Vst3Base\*.cpp,*.h` (top level, excluding the vendored `vst3sdk`) for
`.dll` / literal backslash patterns came back clean. Bundle opening goes
through the SDK's `VST3::Hosting::Module::create()`, so the macOS
bundle-as-directory vs Windows/Linux single-file distinction is delegated to
the SDK. Phase 5 added the standard per-OS VST3 search folders.

---

## 15. File index

### Phase 4c deliverable

```
plugins/Vst3Base/Vst3ParameterModel.h           - suppress plugin-forward for isProgramChange on load
plugins/Vst3Base/Vst3ParameterModel.cpp         - same
plugins/Vst3Base/Vst3PluginInstance.cpp         - saveState()/restoreState() diagnostics + bounds-check fix
plugins/Vst3Effect/CMakeLists.txt               - add Vst3ParameterGrid.{h,cpp}
plugins/Vst3Effect/Vst3EffectControlDialog.h    - add m_grid
plugins/Vst3Effect/Vst3EffectControlDialog.cpp  - build/populate the grid, connect teardown signal
plugins/Vst3Effect/Vst3EffectControls.h         - Q_OBJECT + parameterModelsAboutToClear()
plugins/Vst3Effect/Vst3EffectControls.cpp       - emit the signal first in teardownParameterModels()
plugins/Vst3Effect/Vst3ParameterGrid.h          - NEW: knob grid widget
plugins/Vst3Effect/Vst3ParameterGrid.cpp        - NEW
tests/src/plugins/Vst3BaseTest.cpp              - two regression tests for the root-cause fix
```

### Phase 5 deliverable (commit 0eb1cc9d3)

```
.gitmodules                                  - SDK pin fix
doc/prestige-vst3.md                         - licence/phase notes (superseded by this document)
plugins/Prestige/Prestige.cpp                - transport feed, reconfigure/hardening
plugins/Prestige/Prestige.h
plugins/Vst3Base/Vst3EventList.h
plugins/Vst3Base/Vst3ParameterChanges.h      - coalescing
plugins/Vst3Base/Vst3PluginInstance.cpp      - result codes, buffers, IMidiMapping, ProcessContext, bus negotiation, kOffline
plugins/Vst3Base/Vst3PluginInstance.h        - setTransport() and related API
plugins/Vst3Effect/Vst3Effect.cpp            - transport feed
plugins/Vst3Effect/Vst3SubPluginFeatures.cpp - standard VST3 folders
```

Note: item numbers referenced in older Phase 3 Part 2 comments (for example
"item 3", "item 6") came from a separate punch-list document that is not in
this repository; the open items they refer to are restated in section 12.
