# jacquard

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The weaving is not asserted but
> measured: an offline harness drives the real plugin class in a headless GL context and
> reads each claim back — every pick the loom weaves costs exactly what an exhaustive search
> over shuttle and pattern finds, on 960 woven picks and 200 arbitrary tables; no warp or weft
> float runs past Max Float in 48 cloths, counted on the grid and read back from the picture;
> each of eight weave structures shows exactly its stated weft ratio over 3,600 crossings; a
> twill's diagonal lands at exactly one end across per pick down; every pick shows one weft
> colour and it is a shuttle's; from a distance the cloth is nearer the picture than any
> single colour; the shuttles and the loom's memory survive a resize; and the cloth is opaque
> over a clip with alpha — with eight negative controls that prove each check can fail, at
> two rasters and on a software renderer. It has **never been loaded into Resolume**; the only
> host it has met is [oxbow](https://github.com/stoatworks-labs/oxbow), which is a real FFGL
> host and is not Resolume. See [Status](#status).

The picture woven on a jacquard loom, as an FFGL effect for
[Resolume](https://resolume.com) Arena and Avenue.

![The test card woven: a sky of blue weft picks over a black warp with scattered ties, a white twill band at the horizon, the ground and the colour patches in orange, pale blue and black picks, and the grey ramp in black and white twills](docs/hero.png)

![The same cloth at eight times: black warp ends and lit white and pale blue weft floats stepping in a twill's diagonal, each float's ends dipping under the crossing thread](docs/close.png)

<sub>Two frames rendered by `jqtest`, the offline harness — not captured from Resolume. The
test card at the defaults, then at Zoom 8x.</sub>

## The one idea

At every crossing of a woven cloth one of two threads is on top: the **warp**, running
down, or the **weft**, running across. A jacquard head lifts every warp end on its own, so
any picture can be woven — but three constraints make a woven picture look woven:

1. **Colour comes from threads, not pixels.** One warp colour; and each row, a *pick*,
   carries **one weft colour** from the shuttles loaded. A pick cannot be two colours.
2. **Floats must be tied down.** A thread that passes over or under too many crossings in
   a row snags, so no float may run longer than **Max Float**, warp or weft, on either face.
3. **Tone is carried by weave structure.** A satin shows more weft than a twill, a twill
   more than a plain weave; a designer assigns structures to tones.

So a jacquard encoder is an optimiser under constraints. This plugin weaves the clip pick
by pick, top to bottom as a loom does, and chooses each pick's shuttle and its lift pattern
with an **exactly optimal** programme under both float limits. What falls out:

- **The twill's diagonal in the midtones**, and a satin's smooth face near the ends of the
  range: tone is posterised into regions of one structure, each phase-aligned to the grid so
  the diagonals run straight.
- **Colour banding along the picks.** A pick is one colour across the whole width, so where
  the picture wants two hues side by side the shuttles alternate from pick to pick and mix at
  a distance — the error each crossing is left with is carried to the next pick until one
  pays it.
- **Ties sprinkled through the solids.** A solid is one unbroken float; the programme ties
  it at least cost, in satin order where nothing else decides, and a pick's ties are always
  its own shuttle's colour, so a bright pick crossing a black ground leaves a line of specks.
- **A picture from across the room and thread up close.** Each crossing is drawn as its top
  thread, a lit cylinder that dips under at the end of every float, with the other thread in
  shadow in the gap; Zoom goes from the picture to the cloth.

### The honest limit

The loom weaves top to bottom and a pick cannot see the picks below it: each pick is exactly
optimal given the picks above, and the cloth as a whole is not a two-dimensional optimum.
Two saturated hues side by side weave as an uneven alternation, not two clean halves. There
is one weft a pick — no brocade or lampas mode. And the cloth is woven afresh every frame
with a memory of the last one, which holds a still clip still; how steady it is on footage
was measured once, by hand, and is not a check.

## Controls

| Group | |
| --- | --- |
| **Loom** | Ends (16–320 warp threads across), Picks (0 = Auto, or up to 320), Thread Aspect (a pick's height over an end's width, 0.5–2, with Picks on Auto), Max Float (2–16 crossings). |
| **Threads** | Warp Colour, Shuttles (2–8 weft colours), Palette (Clip — k-means of the clip — Heritage Dyes, Mono, Bright), Structure (Auto by Tone, Plain, Twill, Satin). |
| **Look** | Thread Shading (the lit thread; it fades out below a few pixels a crossing), Zoom (1–8x about the centre), Mix. |

The defaults are 160 ends, square crossings, floats of at most ten, a black warp, six
shuttles dyed from the clip and structure by tone, fully shaded, the whole cloth in view.
They were chosen by weaving Resolume's bundled demo clips through the harness; a black warp
keeps a black ground black.

## Status

**v0.1.0, local — 25 September 2026.** Not released, not on GitHub, not on the website.
Built in one session; everything below was measured on the machine it was built on.

### Measured offline, on macOS

`tools/verify.sh` passes on this machine (M4 Max, macOS 26.4) against a fresh universal
Release build, running every picture check at **two rasters**, 320×180 and 1280×720, and
again at 320×180 on Apple's **software renderer**. What it establishes, in numbers:

| check | result |
| --- | --- |
| `--optimal` | 960 picks woven from 60 random cloths (4–13 ends, 2–8 shuttles, Max Float 2–6, every structure mode) and a second frame of each that remembers the first, plus 200 arbitrary integer tables: the programme's cost **equals** an exhaustive search over every shuttle and every pattern, exactly, on every one, and every table was feasible; the greedy weaver is worse on 49 of 480 |
| `--floats` | 48 cloths (four modes × card, noise, black, white × Max Float 2, 4, 6), counted on the grid and read back from the picture: **no float over the limit**, 0 infeasible picks |
| `--coverage` | each structure forced over a flat field, read from the picture over 60 × 60 crossings: warp satin 720, 3/1 twill 900, 2/1 1200, plain and 2/2 1800, 1/2 2400, 1/3 2700, weft satin 2880 — **exactly** 1/5, 1/4, 1/3, 1/2, 2/3, 3/4, 4/5; the tied solids 612 (0.17) |
| `--twill` | the autocorrelation's nearest peak at **exactly** one end across per pick down, in pixels: 45° for square crossings, 63.43° at Thread Aspect 2, 26.57° at 0.5 |
| `--shuttle` | on the card and a random picture with four shuttle sets: **0** crossings off the colour the grid says, **0** picks with two weft colours, **0** off the shuttles |
| `--distance` | 8 × 8-crossing blocks against the source, on the encoding: RMS error **0.58×** (shaded) and **0.53×** (flat) that of the best single colour at 1280×720 |
| `--resize` | through a change of raster the shuttles are bit-identical and the loom keeps its memory |
| `--alpha` | over a half-transparent card: alpha 255 everywhere at Mix 1, the source byte for byte at Mix 0, the blend to one step at Mix 0.5 |
| `--negative` | eight perturbed models — a greedy weaver, no float limit, heavier twills, twills that do not step, two wefts a pick, the wefts swapped after the weave, a loom that forgets on a resize, a cloth that takes the clip's alpha — each **fails** its check |
| mutation | one character of the shipped GLSL (the lift read from the wrong channel) was caught by five checks at both rasters, then reverted |
| `tools/sweep.py` | all **13** controls measurably change the picture, at 320×180 and 480×270 |
| shaders | all 3 compile through `glslc`, and none uses a GLSL 4.10 reserved word |
| `--pipe` | 2.5 frames in, exactly 2 out; an unknown cue refused (exit 2); a failed render and a closed stdout exit 1, not SIGPIPE; an option steps between cues |
| the bundle | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs; `oxbow` reports `SW Jacquard` / `JQ01` / `effect` and renders 120 frames through `plugMain` |

Render cost, best of three runs of 30 frames after a warm-up, `glFinish` both sides, on a
machine shared with other builds:

| grid | 1280×720 | 1920×1080 | 3840×2160 | of which the CPU encoder |
| --- | --- | --- | --- | --- |
| defaults, 160 × 90 crossings | 2.2 ms | 2.9 ms | 3.2 ms | 1.4 ms |
| largest, 320 × 180 crossings | 5.7 ms | 5.8 ms | 7.8 ms | 4.9 ms |

### Not established

It has **never been loaded into Resolume**, on macOS or Windows; the Windows build has never
been configured. Everything above was compiled, rendered and measured offline against the
real plugin class, plus an `oxbow` load. The look on footage has been judged by eye, on
Resolume's bundled demo clips through the harness's `--pipe`; so has "thread up close". The
cloth's steadiness on video was measured once, by hand: on a near-still clip it changes on
about 1% of pixels a frame against the source's 0.6%. No user guide, no presets, no browser
demo, no OpenFX port.

## Build

Needs CMake 3.15+, a C++17 compiler, and the FFGL SDK submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/jacquard
cd jacquard
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

macOS builds are universal (Apple Silicon + Intel) by default; add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster dev build. Windows needs GLEW via vcpkg.

## Building and testing

The offline harness renders the real plugin class headlessly:

```bash
./build/jqtest --out /tmp/frame.png --size 1920x1080   # the test card
./build/jqtest --list                                  # every control, kind and default
./build/jqtest --optimal                               # the programme against an exhaustive search
./build/jqtest --floats --coverage --twill             # each claim, measured on the picture
./build/jqtest --shuttle --distance --resize --alpha
./build/jqtest --negative                              # and the checks can fail
./build/jqtest --bench                                 # 720p, 1080p and 4K
python3 tools/sweep.py                                 # no control is silently dead
tools/verify.sh                                        # all of it, on a fresh universal build
```

Every check takes `--size`; run it at 320×180 as well as the raster you care about, and with
`JQTEST_RENDERER=software` for the renderer CI has. Footage goes through the real plugin with
`--pipe`, in the fleet's frame format:

```bash
ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
  | ./build/jqtest --pipe --size 1920x1080 --script cues.txt \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
```

See [`CLAUDE.md`](CLAUDE.md) for the full command reference and
[`AGENTS.md`](AGENTS.md) for the model and the traps.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).
