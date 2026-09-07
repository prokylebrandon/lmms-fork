# Freeze Track feature — file package

This zip contains only the files that were changed or added to implement the
"Freeze Track" feature for InstrumentTrack, mirroring their real folder
locations inside the LMMS source tree (`lmms-master/`).

## Files included

**Changed:**
- `include/InstrumentTrack.h`
- `include/RenderManager.h`
- `include/SamplePlayHandle.h`
- `include/TrackOperationsWidget.h`
- `include/InstrumentTrackWindow.h`
- `src/core/RenderManager.cpp`
- `src/tracks/InstrumentTrack.cpp`
- `src/gui/tracks/TrackOperationsWidget.cpp`
- `src/gui/instrument/InstrumentTrackWindow.cpp`
- `tests/CMakeLists.txt` (one new line registering the test below)

**New:**
- `tests/src/tracks/InstrumentTrackFreezeTest.cpp`

No other files were touched, and no new files need registering in any other
CMakeLists.txt — none of the changes added a new .cpp to the main build,
only to the test suite.

## How to apply these to a fresh LMMS checkout

1. Download/clone the real LMMS source (e.g. from your fork of
   https://github.com/LMMS/lmms).
2. For each file listed above, copy it into the matching path in your LMMS
   checkout, **overwriting** the existing file at that path (all of the
   "Changed" files already exist in LMMS; this replaces them with the
   modified version).
3. For the one "New" file, just add it — it doesn't exist yet.
4. Commit and push to your fork, then open a PR or just push to a branch —
   GitHub Actions will attempt to build it automatically.

## If uploading through the GitHub website (no local git needed)

For each file: navigate to that exact path in your fork on github.com,
click the pencil/edit icon, delete the existing content, paste in the new
file's content, and commit. For the one new test file, use "Add file → 
Create new file" and type the full path (`tests/src/tracks/InstrumentTrackFreezeTest.cpp`)
into the filename box — GitHub will create the folders automatically.

## Important caveat

These files were written and manually cross-checked against the real LMMS
source, but were never run through an actual compiler (no build toolchain
was available in the environment they were written in). Expect the
possibility of small compile errors on the first CI run — if any build
check fails (red X), copy the error text from that check's log and share it
so it can be fixed.
