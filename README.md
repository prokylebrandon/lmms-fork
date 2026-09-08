# VST3 hosting for LMMS — consolidated final state

Everything from Phases 1–5 in one place, at its final (Phase 4/5) revision
— not the incremental per-phase snapshots from earlier in the session.
Read the individual PHASE0-5 reports for the why; this is just the what,
gathered so you don't have to hand-merge five zips where the same file
(`Vst3ControlBase.h` especially) changed more than once.

## Apply to your real clone

1. **New files** — copy as-is, no merging needed:
   ```
   include/Vst3ControlBase.h
   include/Vst3SubPluginFeatures.h
   src/core/vst3/Vst3ControlBase.cpp
   src/core/vst3/Vst3SubPluginFeatures.cpp
   plugins/Vst3Effect/          (whole directory)
   plugins/Vst3Instrument/      (whole directory)
   ```

2. **Existing files** — apply the diffs in `touched-files-diffs/` (small:
   145 lines total across all 5):
   ```
   git apply touched-files-diffs/root-CMakeLists.txt.diff      # CMakeLists.txt
   git apply touched-files-diffs/PluginList.cmake.diff          # cmake/modules/PluginList.cmake
   git apply touched-files-diffs/src-CMakeLists.txt.diff        # src/CMakeLists.txt
   git apply touched-files-diffs/src-core-CMakeLists.txt.diff   # src/core/CMakeLists.txt
   git apply touched-files-diffs/src-lmmsconfig.h.in.diff       # src/lmmsconfig.h.in
   ```
   (Run from your repo root; `git apply` needs the right `-p` level if it
   complains — check the `---`/`+++` paths in each diff match, or just
   apply the small hunks by hand, they're short.)

3. **Vendor the SDK** — not included here (large, and you want a real git
   submodule in your actual repo, not a flat copy). See `VENDOR_SDK.md`
   from the Phase 1 delivery for exact commands and the commit this was
   built against.

4. **`vst3_phase4_test.cpp`** — the throwaway runtime-proof harness from
   Phase 4. Not part of the product; useful if you want to re-run the
   save/load proof yourself, or adapt it to test against a real-world
   plugin rather than the SDK's own examples. Needs the CMake target
   wiring from `touched-files-diffs/src-CMakeLists.txt.diff` (already
   included there).

## Before you build

- Your real clone's submodules are already correctly pinned via
  `git submodule update --init` — use those, not anything implied by my
  session (I was working from a `.git`-less zip and had to fetch several
  third-party deps from live upstream HEAD to get *anything* to configure;
  one of them, `portsmf`, had drifted enough from the pinned version to
  break an unrelated file, `MidiImport.cpp`, in the Phase 5 full-build
  check — not a concern for you, but don't copy my fetched submodule
  state hoping it saves you a step).

## Known gaps, unchanged from the phase reports

- Nothing in this project has run on Windows — the actual target.
  Everything Windows-specific (`module_win32.cpp` linkage, bundle/flat-file
  discovery, `%LOCALAPPDATA%`/`Program Files` scan paths) was reasoned
  through by reading the SDK/CMake source, not executed.
- Native VST3 GUI embedding (`IPlugView`, VSTGUI) — Phase 3's stretch goal
  — was never attempted.
- `Vst3Instrument::handleMidiEvent` queues directly rather than through a
  thread-safe ring buffer the way `Lv2ControlBase::handleMidiInputEvent`
  does. Fine for the tests run so far; a real gap under live MIDI input
  from a separate thread.
- Phase 4's runtime test isolated, but didn't fully root-cause, a
  teardown-order crash involving `Engine::init()`'s globals — worked
  around in the throwaway test harness, not something that should affect
  a real LMMS session (which has its own proper shutdown path the harness
  skips), but not verified clean either.
