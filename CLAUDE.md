# jacquard

The picture woven on a jacquard loom, as an FFGL **effect** for Resolume Arena/Avenue.
C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.
v0.1.0, local only (built 2026-09-25, tranche five); never loaded into Resolume, no GitHub
repo, no release, not on the website.

Read `AGENTS.md` before changing the encoder, the loom, the structures or the render pass.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/jqtest --out /tmp/f.png --size 1920x1080`
  (`--source card|noise|grey|halves`, `--frames N`; prints the shuttles it used)
- Set anything by name: `--set "Ends=240" --set "Structure=2" --set "Warp Colour=0.9"`
  (0..1 for sliders, the real integer for integers, the element index for options:
  Palette 0 Clip, 1 Heritage Dyes, 2 Mono, 3 Bright; Structure 0 Auto by Tone, 1 Plain,
  2 Twill, 3 Satin; Picks 0 is Auto)
- List parameters, kinds, defaults and ranges: `./build/jqtest --list`
- The exact GLSL the plugin compiles: `./build/jqtest --dump-shaders DIR`
- Per-pick shuttle choices on a render: `JQTEST_DEBUG=1 ./build/jqtest --out /dev/null`
- Footage through the real plugin — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH` and an optional `--script` of `frame Parameter Name value` cues;
  sliders ramp between cues, options, booleans, integers and events STEP; a cue naming no
  parameter is refused with exit 2, a partial frame at the end of stdin ends the stream
  cleanly, a failed render (`--fail-render-at N`) or a closed stdout exits 1 (SIGPIPE is
  ignored, so `| head -c 1` gives 1, not 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/jqtest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + a reserved-word grep +
  names + optimal + every check at 320x180 AND 1280x720 + the same checks on the software
  renderer + the --pipe contract + two sweeps + bench + the bundle + oxbow)
- Every woven pick equals an exhaustive search, exactly (no GL): `./build/jqtest --optimal`
- No float past Max Float, on the grid and read from the picture: `./build/jqtest --floats`
- Each structure's weft ratio on a flat field, exactly: `./build/jqtest --coverage`
- The twill's diagonal at atan( pick height / end width ): `./build/jqtest --twill`
- One weft colour a pick, a shuttle's: `./build/jqtest --shuttle`
- From a distance, nearer the picture than any single colour: `./build/jqtest --distance`
- The shuttles and the loom's memory survive a resize: `./build/jqtest --resize`
- The checks can fail: `./build/jqtest --negative`; one perturbation verbosely:
  `./build/jqtest --perturb BITS --floats` (bits in `Weave.h`)
- Names ≤ 16 characters and unique: `./build/jqtest --names`
- Every check takes `--size WxH`; CI runs them at 320x180; `JQTEST_RENDERER=software`
  asks for Apple's software renderer
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/jqtest --bench` (defaults and the largest grid; best of three)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Jacquard.bundle`

## Notes
- **The encoder runs on the CPU**, from a read-back of one linear mean per crossing
  (at most 320 x 320). The row programme is integer arithmetic and the harness proves it
  against an exhaustive search on every pick of 120 small woven cloths; that is not a
  shader. Cost: about 1.4 ms a frame at the defaults, about 5 ms at the largest grid.
- **Picks are woven top to bottom**, as a loom weaves them. The column constraint enters
  each pick as forbidden crossings; the complement of the pick above always satisfies
  them, so no pick is ever infeasible.
- **Costs are measured on the sRGB encoding**, and the error carried down the cloth is
  carried on the encoding too, clamped to the range the threads span. Linear light made
  every pick black; a clamped target made every pick grey (AGENTS.md, traps).
- **The loom remembers its last frame** (lifts, shuttles, structures) as small integer
  costs in each pick's table, so a still clip holds still. It forgets when the grid, the
  shuttle count or the structure mode changes.
- **Thread Shading fades out below about 4 pixels a crossing** (fully off at 2), by
  design: the sweep runs it at 40 ends.
- **Output alpha is 1** where the cloth is; Mix blends the whole RGBA with the source.
- **`Perturb` bits and the forced structure are test hooks**, always at rest in the plugin.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it, so Ends, Picks, Max Float and Shuttles are `FF_TYPE_INTEGER`. Options are mapped
  by index in `Controls.cpp`; an option's range reads back 0..1.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `jacquard_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- Never name anything `far` or `near` (windef.h macros on MSVC) or use GLSL 4.10's
  reserved words (`packed` included); no `M_PI`.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `JQ01`, display name `SW Jacquard`.

## Not done yet
- **Never loaded into Resolume**, on either platform. Only `oxbow` has loaded it.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies (`guide=""`);
  no user guide, no browser demo, no OpenFX port, no presets.
- No brocade or lampas mode (a second weft a pick).

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/jacquard/jacquard.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\jacquard\logs\jacquard.YYYY-MM-DD.log   (Windows)
