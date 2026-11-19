# PRESTIGE / VST3 Effect — Phase 4c handoff: state-restore fix + per-parameter knobs

## 1. Root cause of the preset bug — verified in the files

**Both symptoms (effect and instrument) share one root cause.** In
`Vst3EffectControls::applySavedElement()` and
`PrestigeInstrument::applySavedElement()`, project load runs, in order:

1. `restoreState()` / `restoreComponentState()` — correctly restores the
   plugin's own preset/program from its saved binary blob.
2. Every `Vst3ParameterModel` is refreshed from the plugin's new values.
3. The saved `<parameters>` block is walked and `Vst3ParameterModel::
   loadSettings()` is called for every `<param>` element that was saved in
   a **previous** session.

Step 3's `loadSettings()` called `AutomatableModel::loadSettings()`, which
unconditionally ends with `setValue()`. That fires `dataChanged()` →
`Vst3ParameterModel::onModelChanged()` → `queueParameterChange()`, pushing
the *old, previously-saved* normalised value back into the plugin —
silently overwriting whatever the preset in step 1 had just restored.

This explains the reported behaviour exactly:
- **No preset loaded**: saved values already equal the plugin's defaults,
  so re-applying them is a no-op. Looks fine.
- **Preset loaded**: the previously-saved values (from before the preset,
  or from a stale earlier save) conflict with the preset's values and
  clobber them right after restore. No warning is shown for this path —
  the state blob itself restored fine; it's the *parameters* that undid it
  afterwards.

Confirmed byte-for-byte identical pattern in both `Vst3EffectControls.cpp`
and `Prestige.cpp` (`PrestigeInstrument::applySavedElement()`).

### The fix
`Vst3ParameterModel::loadSettings()` (`plugins/Vst3Base/Vst3ParameterModel.{h,cpp}`)
now checks `m_info.isProgramChange`. For a program-change parameter, the
saved value is still loaded into the model (so an automation clip or
controller connection recorded on it survives a reload, same as any other
parameter), but the resulting `onModelChanged()` forward-to-plugin is
suppressed — reusing the existing `m_settingFromPlugin` guard that already
exists for the *other* direction (plugin editor → host). This is a
**shared-code fix**: it applies automatically to both `Vst3EffectControls`
and `PrestigeInstrument`, since both call through the same
`Vst3ParameterModel::loadSettings()`.

No `<state>` shape changed, so **no version bump was needed** —
`kSaveVersion` is untouched in both hosts, and projects saved before this
fix still load exactly as before (they just stop losing their preset).

### Secondary bugs found during the same review (inference, not confirmed against a real plugin)
Found while reading every `tresult`-returning call in `restoreState()`/
`saveState()` per Task A step 1. Fixed alongside the root cause since
they're in the same functions and low-risk, but they are **not** believed
to be the cause of the reported symptom (a real preset is nowhere near the
size where these matter) — flagged separately so they aren't confused with
the actual fix if testing still turns up something odd with a real plugin:

- `saveState()` ignored `IComponent::getState()`/`IEditController::
  getState()`'s return value. If a plugin's `getState()` legitimately
  failed, `saveState()` would previously write whatever partial/empty
  bytes happened to be in the stream into the project as if it had
  succeeded — silently producing a bad save with no record of why. Now
  checked, logged via `qWarning`, and `saveState()` returns an empty
  `QByteArray` on failure instead.
- `restoreState()`'s length-prefix bounds check compared
  `static_cast<int>(compLen) > data.size() - 8`, truncating a `uint32_t`
  to a signed 32-bit `int` **before** comparing. A `compLen` at or above
  2^31 (impossible for any real preset, reachable only from corrupted or
  foreign data) reinterprets as negative and can pass a check it should
  fail, causing an out-of-bounds read. Now compared as `uint64_t`
  throughout. Also added a bounds check on the controller-length field
  itself, which was previously read unconditionally.
- `IEditController::setComponentState()` / `setState()` return values were
  silently ignored (correct not to hard-fail on them — `setComponentState`
  is documented optional — but previously not even logged). Now logged via
  `qWarning` when non-OK and non-`kNotImplemented`, without changing
  pass/fail behaviour. Matches Task A hypothesis 2 ("a `kNotImplemented`/
  `kResultFalse` from an optional call treated as a hard failure") —
  investigated and found NOT to be happening (the code already ignored
  these results), but logging was added so this can be ruled in/out for
  real if presets still misbehave after the main fix.

## 2. What still needs a real `.vst3` to confirm

CI has no `.vst3` file with a real preset/program list (the SDK's `again`
sample plugin has no program-change parameter), so **the actual bug fix
could not be exercised end-to-end by an automated test in this session.**
What the new tests in `tests/src/plugins/Vst3BaseTest.cpp` do and don't
cover:

- `testProgramChangeParamNotReappliedOnLoad()` / `testOrdinaryParamStillReappliedOnLoad()`
  construct a `Vst3ParameterModel` directly (bypassing `Vst3EffectControls`/
  `PrestigeInstrument`, which need a full `Song`/`Project`) around the real
  `again.vst3` plugin instance, with a **synthetic** `Vst3Parameter` marked
  `isProgramChange = true` for the first test. They verify
  `Vst3ParameterModel::loadSettings()`'s own contract in isolation: a
  program-change parameter's model value updates from a saved `<param>`,
  but the plugin's controller is not told about it; an ordinary
  parameter's model value **and** the plugin's controller both update, as
  before.
- What they do **not** cover: the full `applySavedElement()` sequence
  (`restoreState()` → refresh models → `loadSettings()` on saved
  `<parameters>`) against a plugin that has a *real* program list and
  round-trips it through an actual project XML save/reload. That needs a
  real `.vst3` with presets, run by hand.

### Manual test steps (please run these)
1. Load a VST3 **instrument** (Prestige) with a plugin that has at least
   one built-in preset/program (a synth with factory presets is ideal).
2. Load a preset via the plugin's own editor.
3. Save the project, close LMMS, reopen it, load the project.
4. Confirm: no "Plugin state could not be restored." warning, and the
   preset's sound is actually back (not just silence-that-happens-to-match).
5. Repeat steps 1–4 with a VST3 **effect** (Vst3Effect) using a plugin with
   presets.
6. Open an old project saved *before* this fix (if you have one with a
   VST3 effect/instrument) and confirm it still opens normally.
7. For Task B: add a VST3 effect, open its dialog, confirm the knob grid
   appears, right-click a knob → create an automation clip, play the
   project, confirm the parameter moves and the plugin audibly responds.
   Move a knob directly — confirm the plugin's own processing changes.
   If the plugin has a native editor, open it (still via "Show editor"),
   move a control there, and confirm the matching knob in the grid moves
   too.
8. Specifically test a plugin with an `isBypass` parameter and one with a
   real `isProgramChange` parameter, if you have one — these are the two
   flag-driven paths that could not be exercised against a real plugin in
   this session (see the "Known gaps" section below).
9. Test a plugin with more than ~100 parameters if you have one, to check
   the "Show more" / lazy-paging behaviour doesn't stall the dialog or
   behave oddly with the search filter.

## 3. Task B: per-parameter knobs — what was built

New files: `plugins/Vst3Effect/Vst3ParameterGrid.{h,cpp}`.

- A scrollable grid (`QScrollArea` + `QGridLayout`, 4 columns) of real LMMS
  `Knob` widgets, one per exposed parameter, each bound via
  `Knob::setModel(parameter->valueModel())` — the standard
  `AutomatableModelView` path, so right-click automate/connect, drag-to-
  automation-clip, and the context menu all work with zero custom code.
- A search box filters by parameter title, unit string, and numeric/hex
  VST3 ID, case-insensitively.
- **Lazy creation**: only builds `Knob` widgets for parameters that are
  both filtered-in and within the current "page" (64 shown initially, +64
  per "Show more" click or on scrolling near the bottom). A plugin with
  hundreds of parameters does not build hundreds of `Knob`s up front.
- Flag handling:
  - `isHidden` → not shown, not even behind the filter.
  - `isReadOnly` → shown, `Knob::setEnabled(false)`. Writes are also
    already refused at the model level (`Vst3ParameterModel::
    onModelChanged()`), independently of this.
  - `isProgramChange` → shown as a normal (stepped) knob with a
    "(program)" label suffix — not skipped, per the project's own firm
    rule ("every plugin parameter is exposed") and matching
    `Vst3ParameterWindow`'s existing precedent on the Prestige side.
  - `isBypass` → shown as a stepped knob with a "(bypass)" label suffix,
    **not a literal checkbox widget**. See "Known gaps" below for why.
  - `stepCount > 0` → no separate spin-box widget type is used; the
    underlying `FloatModel` is already constructed with a matching step
    size (`Vst3ParameterModel`'s constructor), so a plain `Knob` bound to
    it already snaps to the discrete values.
- Value text on each knob shows the plugin's own formatted string (via
  `Vst3ParameterModel::formattedValue()`), not the bare 0..1 normalised
  number.
- The existing "Show editor" toggle and its window-hosting code
  (`openEditorWindow()`/`closeEditorWindow()`, the two event-filter helper
  classes) are **unchanged** — the grid is added alongside them in the
  same dialog.
- Plugin-editor → host sync (`onPluginParameterEdited()` →
  `setValueFromPlugin()`) needed **no new code**: it already calls
  `FloatModel::setValue()` on the same model a grid knob is bound to, so
  `dataChanged()` already repaints the knob. Verified by reading the
  existing wiring, not modified.

### A real lifetime bug found and fixed while building this
`Vst3EffectControlDialog` (and its new `Vst3ParameterGrid`) is a GUI
object owned by `EffectView` — a *separate* object from `Vst3Effect`/
`Vst3EffectControls`, with an independent lifetime (confirmed by reading
`EffectView.cpp`: `createView()`'s result is owned by `EffectView`, not by
the effect). This means `Vst3EffectControls::teardownParameterModels()`
(called from `Vst3Effect`'s destructor) can run while a dialog showing the
knob grid is still open, e.g. if the effect is removed from the chain
while its control dialog is visible. Before this was accounted for, every
`Knob` in the grid would have been left pointing at a freed
`Vst3ParameterModel`/`FloatModel` — a use-after-free.

Fixed by adding a new signal, `Vst3EffectControls::
parameterModelsAboutToClear()`, emitted at the very start of
`teardownParameterModels()`, before anything is detached or destroyed.
`Vst3EffectControlDialog`'s constructor connects to it (direct/same-thread
connection, not queued, since teardown proceeds synchronously right after)
and calls `m_grid->clearParameters()`, dropping every `Knob`'s model
pointer before the models go away. This mirrors the ordering contract
`Vst3ParameterWindow::clearParameters()` already documents on the Prestige
side, adapted to a signal since (unlike `PrestigeInstrument`)
`Vst3EffectControls` doesn't hold a pointer back to its dialog.

**Not independently exercised** against the actual "remove effect from
chain while its dialog is open" sequence in this session — that needs
`EffectChain`/`EffectChainView`, which weren't part of the uploaded files.
Please test this by hand: open a VST3 effect's dialog, then delete/remove
that effect from its chain while the dialog stays open, and confirm LMMS
doesn't crash.

### Known gaps — real, not overlooked
- **`isBypass` is not a literal checkbox.** Task B asked for one.
  `Vst3ParameterModel` only exposes a `FloatModel` (confirmed by reading
  `Vst3ParameterModel.h` first, per the task's own instruction), and the
  natural LMMS checkbox widget for a bool-shaped control, `LedCheckBox`
  (an `AutomatableButton`), binds to a `BoolModel`-family model.
  `AutomatableButton.h` was not part of this session's uploaded files, so
  its `setModel()` contract was never confirmed and no bridging model was
  built against a guess. `isBypass` parameters are fully functional as a
  stepped knob (click/drag toggles between the two positions, automates
  and connects normally) — just not a checkbox widget. Swapping in a real
  `LedCheckBox` is a follow-up once `AutomatableButton.h` is available;
  please attach it by name if you want this closed out.
- **`isAutomatable == false` is not enforced.** Checked
  `AutomatableModel.h`/`Model.h` directly: neither has a per-model "refuse
  automation/controller connection" capability (`isAutomated()` means "has
  an automation clip attached right now", not "is allowed to"), and
  `Vst3ParameterModel` itself never reads `isAutomatable` either (grepped
  for it — stored on `Vst3Parameter`, never checked). So every knob in the
  grid offers the normal automate/connect context menu regardless of what
  the plugin reported. Enforcing this would need either a new
  `AutomatableModel`-level capability (affects every model in LMMS, well
  beyond this task) or a local `Knob` subclass overriding context-menu
  construction, and `AutomatableModelView.cpp` (needed to know whether
  that's even easily overridable) wasn't part of this session's files
  either. Left unenforced rather than guessed at.
- **Grid column count is fixed at 4**, not responsive to dialog width.
  Simple to change if it looks wrong in practice; not tuned against a real
  plugin's dialog in this session (no windowing/rendering available here).

## 4. Workarounds still in place — do not remove without replacing

- `plugins/Vst3Base/CMakeLists.txt`: `if(MSVC) target_link_options(vst3base
  PRIVATE /FORCE:MULTIPLE)`. Still needed; nothing in this session touched
  why it was added (the LNK2005/LNK1169 duplicate-symbol issue from
  `vst3base.dll` linking `lmms.lib`).
- `tests/CMakeLists.txt`: Windows `PATH` also gets
  `$<TARGET_FILE_DIR:lmms>` for `Vst3BaseTest`. Untouched, still
  unverified whether it actually fixed the `0xc0000135` exit — please
  report back once you've run the Windows test suite.
- macOS `@rpath/libvst3base.dylib` CPack warning: untouched, not in scope
  this session (Task C was "only if asked").

## 5. Files changed in this delivery

```
plugins/Vst3Base/Vst3ParameterModel.h       - suppress plugin-forward for isProgramChange on load
plugins/Vst3Base/Vst3ParameterModel.cpp     - same
plugins/Vst3Base/Vst3PluginInstance.cpp     - saveState()/restoreState() diagnostics + bounds-check fix
plugins/Vst3Effect/CMakeLists.txt           - add Vst3ParameterGrid.{h,cpp} to the build
plugins/Vst3Effect/Vst3EffectControlDialog.h    - add m_grid, forward-declare Vst3ParameterModel
plugins/Vst3Effect/Vst3EffectControlDialog.cpp  - build/populate the grid, connect teardown signal
plugins/Vst3Effect/Vst3EffectControls.h     - Q_OBJECT + parameterModelsAboutToClear() signal
plugins/Vst3Effect/Vst3EffectControls.cpp   - emit the signal first in teardownParameterModels()
plugins/Vst3Effect/Vst3ParameterGrid.h      - NEW: the knob grid widget
plugins/Vst3Effect/Vst3ParameterGrid.cpp    - NEW: same
tests/src/plugins/Vst3BaseTest.cpp          - two new regression tests for the root-cause fix
```

Prestige's own `Vst3ParameterWindow` and `Prestige.cpp` were **not**
modified — the root-cause fix lives entirely in the shared
`Vst3ParameterModel`, so it applies to the instrument automatically. Task
B (the knob grid) was explicitly scoped to `Vst3Effect` only (requirement
8: "do not promote GUI widgets into `Vst3Base`" — Prestige already has its
own, different, parameter UI).
