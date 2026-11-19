# Tempo automation fixes (LMMS #5122, #4928, #5319)

## Status of the three issues

- **#4928** (note length is calculated before tempo automation): the upstream fix, PR #4942, is present in this fork.
  The tempo model's `dataChanged` and `dataUnchanged` signals are connected to `Song::setTempo()` with
  `Qt::DirectConnection`. That fix only makes the tempo reach the notes sooner. It does not fix the arithmetic of
  `NotePlayHandle::resize()`, which is where the remaining errors were (see below).
- **#5122** (broken sound when tempo automation changes the tempo): still present before these changes.
- **#5319** (silence and missing VST notes during gradual tempo automation): same root cause as #5122. It was closed
  upstream as a probable duplicate.

## Root causes and fixes

1. **`NotePlayHandle::resize()` rescaled the whole note as if the tempo had changed at the start of the period.**
   Automation changes the tempo at an exact frame inside a period, so a note that should end at the change point had
   its end pushed later. Its NoteOff then landed after the NoteOn of the next note on the same key. Instruments sort
   events by sample offset (`Vst3EventList::sortBySampleOffset`, `RemoteVstPlugin` `stable_sort`), and the VST3 noteId
   is the MIDI key, so the stale NoteOff cancelled the new note. On a tempo ramp this happened at every step, which is
   the silence in #5319.
   *Fix:* `resize(newTempo, framesUntilChange)` keeps the frames already played (including the part of the current
   period before the change) and converts only the rest. The unrounded length is kept in `m_exactFrames`, so many small
   steps do not accumulate rounding error. `m_totalFramesPlayed` is no longer rescaled, because `play()` and the
   instruments use it to tell whether a note has started. A tempo that does not change is a no-op, and notes that
   already end before the change keep their length.
2. **`Song::processNextBuffer()` read `framesPerTick` once per period.** After a tempo change inside the period the rest
   of it still advanced at the old rate, and the tick that starts at the change lasted as long as the old tempo said.
   *Fix:* the tick length is read on every pass, automation runs before the segment length is computed, and
   `m_tempoChangeOffset` tells `setTempo()` at which frame of the period the change happens.
3. **Notes created earlier in the same period were never resized.** They are still in `AudioEngine::m_newPlayHandles`,
   and `setTempo()` only walked `playHandles()`. This needs two or more ticks per period (above about 215 BPM at 256
   frames).
   *Fix:* `AudioEngine::forEachNewPlayHandle()`; `setTempo()` visits those notes too.
4. **A tempo change made from the GUI in the middle of a tick left the timeline's frame offset in old-tempo frames.**
   If it was larger than the new tick length, the tick's notes and automation were skipped.
   *Fix:* `setTempo()` rescales the frame offset of every timeline by `oldFramesPerTick / newFramesPerTick`.
5. **Safety net in `InstrumentTrack::processOutEvent()`.** If a NoteOn arrives with a sample offset earlier than a
   NoteOff already sent for the same key in the same period, the NoteOn is moved to the NoteOff's offset. With fixes 1 to
   3 this only absorbs a rounding difference of a frame or two. `AudioEngine::periodCounter()` tells the two calls apart.
6. `NotePlayHandle::resize()` no longer divides by `m_frames`, which can be 0.
7. **`Prestige` computed `projectTimeSamples` from the tick position and the current tempo.** That product jumps
   whenever the tempo is automated, so a plugin that syncs to the host's sample position (delays, arpeggiators, LFOs)
   glitched at every tempo step. *Fix:* `Vst3PluginInstance::setTransport()` takes an optional `projectTimeSeconds`, and
   `Prestige::play()` passes `Timeline::getElapsedSeconds()`, which is accumulated tick by tick at the tempo in effect.
   Callers that pass nothing keep the old calculation. After a jump in the song position the elapsed time is
   recomputed from the tick position at the current tempo (`Timeline::setTicks()`), so it is exact only for continuous
   playback.

## Tests

`tests/src/core/TempoAutomationTest.cpp`:

| Test | Covers |
| --- | --- |
| `noteEndingAtTempoChangeIsNotStretched` | #5122 (the 68 to 76 BPM case from the issue, in frame terms) |
| `noteLengthAcrossSuddenTempoChange` | #4928 |
| `noteLengthDuringTempoRamps` | #5319 (falling ramp, and rising then falling) |
| `noteStartingLaterInThePeriodIsResizedFromItsStart` | fix 3 (arithmetic) |
| `unchangedTempoDoesNotChangeNote` | no drift from repeated `setTempo()` calls |
| `timelineFollowsTempoAutomation` | fix 2: timeline time equals rendered time across automated tempo changes |
| `pendingNotesAreResizedBySong` | fix 3 (`Song::setTempo()` reaches notes that are not in `playHandles()` yet) |

The tests use no instrument. They drive `NotePlayHandle` period by period the way the audio engine does, and compare
against a tick-by-tick reference. The NoteOn clamp in fix 5 has no test, because that needs an instrument that records
its MIDI events.

## Not covered

- Real-time and offline rendering were not run here. The tests exercise the shared code paths (`Song::processNextBuffer`
  and `NotePlayHandle`) that both use; listen to an exported tempo-automated project to confirm.