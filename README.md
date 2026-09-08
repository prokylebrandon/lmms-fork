# Two things fixed/added, four files changed

## 1. Root cause of "freeze removed for both original and clone"

This turned out to be a real, fairly deep bug -- not clone-specific, and
not something a quick patch could fix safely.

**What was happening:** `Instrument`, `Effect`, and `Clip` all inherit
`JournallingObject` (used for undo/redo), whose `saveState()` unconditionally
embeds that object's own bookkeeping id -- assigned fresh, per-instance, at
construction time, with no relationship to the object's actual content. The
staleness fingerprint was hashing that id along with everything else. Since
a clone (and, it turns out, an ordinary project reload) constructs brand
new Instrument/Effect/Clip objects with fresh ids, the fingerprint would
*always* differ from what was saved at freeze time -- even with zero real
edits -- making the track look stale/unfrozen immediately.

**Fix:** `freezeSourceFingerprint()` now strips that bookkeeping metadata
(the id attributes and `<journallingObject>` nodes) from the snapshot
before hashing, so only genuine content differences register. This also
fixes staleness detection on ordinary save-and-reopen, which had the exact
same underlying problem, even though you hadn't hit it yet.

A new test, `testCloneFrozenTrackPreservesFreezeState`, exercises
`Track::clone()` directly as the most literal regression test for this.

## 2. Auto re-freeze when stale (no more manual re-freezing after every edit)

New behavior, on by default: when a frozen track goes stale (e.g. you add a
note), instead of requiring you to click "Re-freeze" every time, it waits
~2 seconds after your last edit and re-freezes itself automatically in the
background.

Two things worth knowing:
- **Debounced, not instant** -- a burst of edits (adding several notes in a
  row) coalesces into one render once things settle, not one render per
  note.
- **Never interrupts playback** -- freezing has to temporarily swap the
  audio engine to an offline render device. If it fired while you were
  listening to playback, your audio would cut out. So if you're mid-playback
  when the debounce elapses, it waits until you stop before actually
  re-rendering.

You can turn this off per-track via the new "Auto re-freeze when stale"
checkbox in the same right-click gear menu as Freeze/Unfreeze -- unchecking
it restores the original manual-only behavior. The preference is saved with
the project.

Two new tests cover this: `testAutoRefreezeAfterEditSettles` and
`testAutoRefreezeCanBeDisabled`.

## Files changed

- `include/InstrumentTrack.h`
- `src/tracks/InstrumentTrack.cpp`
- `src/gui/tracks/TrackOperationsWidget.cpp`
- `tests/src/tracks/InstrumentTrackFreezeTest.cpp`

No other files from earlier rounds need touching again -- these four
replace what you already have at the same paths.

## Caveat

As before: written and carefully cross-checked against your actual repo
(not assumed), but never compiled -- no build toolchain available here.
Please build and run the tests for real; if `testAutoRefreezeAfterEditSettles`
or `testCloneFrozenTrackPreservesFreezeState` fail, that's exactly the kind
of thing that would tell us something in this reasoning was wrong.
