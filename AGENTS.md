# AGENTS.md — Jacquard

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

The picture woven on a jacquard loom, as an FFGL 2.1 effect (`JQ01`, shown as
`SW Jacquard`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal
macOS `.bundle` and a Windows `.dll` (the Windows build is written into CMake and CI, and has never been configured or built).
MIT; intended home `github.com/stoatworks-labs/jacquard`, which does not exist yet.
Local v0.1.0 in `~/dev/jacquard`, for a human to land in the fleet.

Built 2026-09-25 in one session from `specs/SPEC-jacquard.md`, `BRIEF.md` and
`BRIEF-ADDENDUM.md`. Tranche five; Allan's own pick. Templates: **teletext** for the
effect skeleton, the read-back to an exact integer encoder on the CPU, the exhaustive
search and the negative controls; **fax** for stepped `--pipe` cues and the software
renderer pass; **tinsel** for `PassBuffer`, the sweep and CI; **rosette** and
**intaglio** read for how a print or line structure carries tone (nothing copied).
`stencil` had not landed.

---

## The one idea

At every crossing of a woven cloth one of two threads is on top: the **warp**,
running down (an *end*), or the **weft**, running across (a *pick*). A jacquard head
lifts each end independently, so any picture can be woven — under three constraints,
and the look is what the optimum under them looks like:

| the constraint | what comes out |
| --- | --- |
| one warp colour; one weft colour a pick, from the shuttles loaded | a pick is one colour across the whole width, so colour arrives in horizontal bands, and where the picture wants two hues side by side the shuttles alternate pick by pick and mix from a distance |
| no float longer than Max Float, warp or weft, on either face | a solid is one unbroken float and must be tied: ties sprinkled through every solid, in satin order where nothing else decides |
| tone is carried by weave structure | the twill's diagonal in the midtones, a satin's smooth face near the ends of the range, and the tone posterised into regions of one structure |

So the encoder is an optimiser under constraints, like teletext's.

### The pipeline

1. **Cells** (GPU, Ends x Picks RGBA32F). Each crossing's mean over the source pixels
   whose centres fall in its rectangle, in linear light; strided past 16 taps a side.
2. **Read-back** (at most 320 x 320 x 16 bytes, 1.6 MB) to the CPU.
3. **Shuttles** (CPU, `Palette.cpp`): Clip is k-means of the crossings in linear light,
   warm-started from the last frame; the fixed sets are swatch cards.
4. **The loom** (CPU, `Loom.cpp`), pick by pick, top to bottom:
   - the target: the crossing's mean, sRGB-encoded (LUT), plus the error carried down;
   - **tone to structure**, per shuttle c: the structure in the mode's family whose
     mixture `f c + (1 - f) W` (linear light; f its weft coverage, W the warp) is
     nearest the target on the encoding, with last frame's kept while within
     kHoldStructure;
   - **costs**, integers (squared error on the encoding x 65536): following the
     structure costs its mixture's error; a tie costs that plus kTie plus the error of
     the thread it shows, less kLattice on the tie lattice; memory terms on top;
   - **the programme** (`Encoder.cpp`) finds the shuttle and the lift pattern of least
     total cost under both float limits, exactly;
   - **the carry**: target minus mixture, clamped to the threads' range, spread
     1/4, 1/2, 1/4 to the next pick.
5. **Render** (GPU): per output pixel through Zoom, the crossing and the thread on top;
   its colour; and, by Thread Shading, a lit cylinder with a dip where a float ends, a
   ply twist, a coloured sheen, and the other thread in shadow in the gap between two.

### The row programme, and why it is exact

A pick of n crossings: choose shuttle c and x_i (1 = warp on top) minimising
Σ cost[c][i][x_i], with no run of more than M equal x_i along the pick (weft float,
face or back), and some crossings *forbidden* one value because the end above has
already run M picks on that side (warp float). State (value, run 1..M), 2M states; a
run extends while shorter than M, either value may start a run of 1. Colours are tried
in order of a lower bound (each crossing's cheaper permitted value) and the search
stops when the bound reaches the best row found: the joint optimum over colour and
pattern, at 1.18 programmes a pick on average.

**Always feasible.** A crossing is forbidden exactly the value the pick above has
there, so the complement of the pick above satisfies every forbidden value and has the
same runs. The first pick has none. `--optimal` asserts feasibility on every table.

**Weaving top to bottom is greedy between picks** — a pick cannot see the picks below
it. The per-pick optimum is exact; the cloth is not a 2-D optimum, and nobody should
claim it is. A separate column pass (the spec's wording) would undo the row constraint;
entering the column constraint as forbidden crossings is what keeps both exact per pick.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Weave.{h,cpp}` | The ten structures, their repeats and coverages as exact fractions, the Structure modes' families, the tie lattice, the `Perturb` bits, the hash. |
| `source/Encoder.{h,cpp}` | The row table, the exact programme with its colour bound, the greedy weaver (the negative control), `RowCost`. |
| `source/Loom.{h,cpp}` | The cloth, pick by pick: targets, tone to structure, costs, memory, the programme, the carry. The cost constants live in `Loom.h`. |
| `source/Palette.{h,cpp}` | sRGB both ways, the swatch cards, the warm-started k-means with farthest-point seeding. |
| `source/Controls.{h,cpp}` | Integer ranges, Picks Auto, Thread Aspect, Zoom. |
| `source/Shaders.{h,cpp}` | The cells pass and the render pass. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed. |
| `source/Jacquard.{h,cpp}` | The plugin: parameters, the two passes, the read-back, the CPU stage, the state across frames. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/jqtest/` | The harness: checks, negative controls, bench, `--pipe`, shader dump. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters and on the software renderer, plus the release-time checks done locally. |

---

## Traps

In the order they bit.

### Tone by luminance alone painted the wrong hue

The spec maps a crossing's *luminance* to a structure. On a red | blue test (`--source
halves`, Bright shuttles) every red pick then wove the blue half red too, at the
right luminance: a crossing cannot refuse a shuttle it only compares by luminance. The
structure is now the mixture nearest the target in colour, on the encoding — which is
the luminance rule wherever the target lies between warp and weft, and falls back
towards the warp where the shuttle's hue is wrong.

### Linear light made every pick black

Costs measured in linear light: a saturated red or blue is nearer black than any other
hue there, so every pick chose black and the halves came out as black cloth with satin
ties. The pick's colour decision is measured on the encoding.

### A clamped target made every pick grey, and the fix for that made it black again

With the error carried in linear light and the target clamped into the gamut before
encoding, a region owed more red than any thread holds looked no different to the
cost, so the carry never tipped a pick: the halves wove as a mid grey (a black weft
satin), which is the encoded "centroid" of red and blue. Extending the encoding past
0 along its tangents let the owed colour count — and multiplied a small owed darkness
by 12.92, so black won again. **The carry now runs on the encoding itself**, where it
is balanced with what the cost measures.

### A black warp is lighter than a black picture

The default warp is near-black (0.08). A black ground then owed the difference at
every crossing, pick after pick, until the clamp; the carried noise outweighed the
tie lattice's discount, so the ties stopped scattering, the warp floats of the whole
ground expired together, and a full-width dark pick was forced every Max Float picks.
On footage it read as a grid of vertical dashes over horizontal bars. The residual is
now the target **clamped to the range the threads span**, per channel, minus the
mixture: only what some thread could repay is owed.

### An undyed warp and a white sheen turned black grounds grey

The first defaults (cream warp, white specular) wove Resolume's dark demo clips as a
grey mesh: every tie in a black ground was a cream speck, and the sheen added
near-white light to black thread. The default warp is black, and the sheen is the
thread's own colour brightened.

### k-means by luminance quantile never found the small saturated patches

Seeded at luminance quantiles, six shuttles on the test card were all sky blues,
browns and greys; the eight colour patches got none. Farthest-point seeding (the
median, then the colour farthest from every centre so far) finds them, and is
deterministic.

### Error diffusion is chaotic in time

On a clip whose source changes on 0.6% of pixels a frame, the cloth changed on 19%:
one crossing's change at the top moves shuttles and ties all the way down. The loom
now remembers the last frame as costs in each pick's table (kHoldLift, kHoldShuttle,
kHoldStructure): 0.8–1.0% on the same clip, and on a moving clip (Galactucity) it
tracks the source (10–15% against 14%). Measured with a one-off script through
`--pipe`; not a check (see Open questions).

### Least-cost is not fewest

A forced solid on a flat field ties 612 of 3600 crossings (0.17) at Max Float 8, not
the 1 in 9 the lattice alone would give: the carried target varies by more than the
lattice's discount from crossing to crossing, the ties land off the lattice, and the
column runs then force more. The programme is exact for the costs it is given; "least
cost" is the claim, not "fewest ties". `--coverage` states the solids' tie density and
asserts only the lower bound the float limit implies.

### A CPU encoder spends its time in arithmetic nobody sees

The tie lattice's satin step (a gcd loop) was being found for every crossing and every
shuttle, and each structure's pattern computed with a division per crossing. Once a
frame now, and tiled. The CPU half is about 1.4 ms at the defaults and 4.9 ms at the
largest grid.

### Thread Shading is dead at 320 x 180, on purpose

The shading fades out below about 4 pixels a crossing (fully off at 2), because a
cylinder drawn in two pixels can only alias. At the default 160 ends a 320 x 180 frame
is exactly 2 px a crossing, so the sweep reported Thread Shading dead; its sweep runs at
40 ends. The same fade is why both lines of `--distance` agree at 320 x 180.

### `PIPESTATUS` after a command substitution

`got=$( jqtest … | wc -c ); status=${PIPESTATUS[0]}` reads the *assignment's* status,
and only `set -o pipefail` made it jqtest's. The first draft of the failed-render test
passed in verify.sh and failed outside it. The pipe tests now write to a file and take
`$?` straight from jqtest; the `| head -c 1` test is a bare pipeline, where
`PIPESTATUS[0]` is jqtest's own.

### Names

`kBright` and `kHeritage` were both an enum value in `Palette.h` and a swatch array in
`Palette.cpp`: an ambiguous reference. The farthest-point loop's first draft called its
index `far`, which is a macro in windef.h: renamed before MSVC could find it.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and put
back before the render pass); every `ffglex::Scoped*` clears to 0 on exit, so the cells
buffer and the lift texture are allocated before anything binds a texture;
`FFGLFBO::Release()` leaks the colour texture (`PassBuffer::Destroy()` deletes it
first); `SetParamInfo` clamps a STANDARD default into 0..1 (Ends, Picks, Max Float and
Shuttles are `FF_TYPE_INTEGER`); an option's range reads back 0..1; the core is an
**OBJECT** library; `SetTextParameter` must return `FF_SUCCESS` for the About block;
`nm | grep -q` fails under pipefail when grep succeeds; the DXV demo clips carry alpha,
so the output alpha is decided (below) and checked (`--alpha`); state across frames is
CPU-side and never the size of the raster, so a resize cannot clear it (`--resize`);
nothing reads the host clock, so its float overflow cannot reach this plugin.

---

## Would this hold on another rasteriser, at another raster?

Every check ran at 320 x 180 and 1280 x 720, and at 320 x 180 on Apple's software
renderer (`JQTEST_RENDERER=software`), in `verify.sh`. Checks that read the picture use
Thread Shading 0 and Zoom 1 unless they say otherwise; a crossing then renders exactly
its top thread's colour, sRGB-encoded in float and stored to 8 bits.

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--optimal` | every woven pick's cost against an exhaustive search over shuttle and 2^n patterns | **exact equality of 64-bit integers**: both sides sum the same table | none: no GL |
| `--floats` | longest warp and weft run, on the grid and read from the picture | **exact** counts; the picture read classifies each crossing's centre pixel as the warp colour within **one 8-bit step** (the shader's float encode against the harness's float pow; a value within float error of a rounding boundary lands either side) | the centre pixel of a crossing of ≥ 2 px lies ≥ 1/4 crossing inside it, far past float error in ( p + 0.5 ) E / W; at the defaults a crossing is 2 px at 320 x 180 and 8 px at 1280 x 720 |
| `--coverage` | weft crossings over 60 x 60 crossings of each forced structure | **exact** integer against num/den x 3600; same one-step colour classification | as `--floats`; 60 is a whole number of every repeat |
| `--twill` | the nearest autocorrelation peak's pixel lag | **exact** lag ( -end width, +pick height ); the peak is taken within **1e-6** of the maximum, because edge-cropped sums at different lags differ in the last bits | only at rasters where a crossing is a whole number of pixels (Ends 160 across 1280 or 320; Picks from the aspect), which the check asserts and reports; a fractional crossing would put the peak on the nearest pixel lag, and the check would fail and say why rather than pass |
| `--shuttle` | every crossing's colour against the grid; one weft colour a pick; that colour a shuttle's | **one 8-bit step**, as above; nothing else | as `--floats` |
| `--distance` | RMS error of 8 x 8-crossing block means on the encoding, against the frame's mean colour everywhere | **comparative**: the cloth must beat the best single colour; no fitted number | block means average many pixels, so rasteriser rounding is noise far below the margin (ratios 0.54–0.58 against a threshold of 1) |
| `--resize` | the k-means seed after a resize equals the centres before it, bit for bit; the loom remembered | **exact** (memcmp): CPU state | the grid (160 x Auto) is the same at every 16:9 raster, which is why memory must survive |
| `--alpha` | output alpha at Mix 1, 0, 0.5 over a half-transparent card | Mix 1: **exactly 255**; Mix 0: **byte-identical** to the source (`mix( x, y, 0 )` multiplies by exact 1 and 0, no cancellation); Mix 0.5: **one 8-bit step** | the source is sampled at pixel centres, which a linear filter returns exactly |
| sweep | any subpixel differs | ≥ 1 | 320 x 180 and 480 x 270 |

Deliberately not relied on: `pow( 1, x ) == 1`; exact cancellation; any filtered or
interpolated value in a check. What might differ on another GPU: the cells pass's means
(a sum of `pow` decodes) round differently, so the loom's *choices* on a real picture
can differ by a crossing here and there; no check asserts a specific weave of the card,
only structure on forced flat fields and invariants (floats, one weft a pick) on the
card and noise. The loom is integer and float CPU code and must agree bit for bit given
the same means.

### The negative controls

`jqtest --negative` runs eight; `--perturb BITS` runs any check verbosely against one.
Each perturbs the *plugin's* model through a hook the shipped plugin carries at zero.

| perturbation | what fails (320 x 180 / 1280 x 720) |
| --- | --- |
| greedy weaver (follow the structure, flip only when forced) | `--optimal`: 860 of 960 woven picks exact, 3 invalid; 38 of 200 arbitrary tables exact |
| float limit dropped | `--floats`: runs of 160 across and 90 down, 24 of 48 cloths over the limit |
| every twill one warp crossing heavier | `--coverage`: 3/1 twill 0.17 against 0.25, 2/1 0.17 against 0.333, … |
| twills that do not step | `--twill`: the nearest peak at (0, 1) px, 90 degrees against 45 |
| odd ends drawn in the next shuttle's colour | `--shuttle`: 90 of 90 picks with two weft colours |
| each pick's weft swapped after the weave | `--distance`: ratio **1.120 / 1.039** — it fails, but by 4% at 1280 x 720: the structures still carry the tone |
| shuttles and loom that forget on a resize | `--resize`: seed differs, 3 cold starts, no memory |
| a cloth that takes the clip's alpha | `--alpha`: 28,800 / 460,800 pixels wrong at Mix 1 |

The pipe's stepping test was also run against a harness that ramps every cue: frames 1,
5 and 9 then differ, and the test fails.

### The mutation

One character of the shipped GLSL, on a clean committed tree (98fed84): in the render
pass's `warpAt`, `texelFetch( Lift, c, 0 ).r > 0.5` became `.g > 0.5` — the lift read
from the shuttle-index channel. Caught at both rasters by **`--shuttle`** (6,416 of
14,400 crossings not the colour the grid says, at 320 x 180), **`--coverage`** (3/1
twill 1.0 against 0.25), **`--floats`** (runs of 160 read from the picture),
**`--twill`** and **`--distance`**. Correctly not caught by `--resize`, which reads only
CPU state. Reverted with `git checkout source/Shaders.cpp` and a `touch`; the tree was
clean before and after, and the rebuilt `--shuttle` passes.

---

## Decisions taken without asking

- **The encoder runs on the CPU, from a read-back.** The programme is serial, and the
  claim of exactness wants integers. The read-back is the one stall, and is in the bench.
- **The column constraint enters each pick as forbidden crossings**, not as a separate
  column pass (see the one idea).
- **Both faces count.** A run of warp-up crossings along a pick is the weft floating
  behind; a run of weft-up down an end is the warp floating behind. Both are limited.
- **Auto's family** is warp solid, warp satin (0.2), 2/1 twill (0.333), 2/2 twill (0.5),
  1/2 twill (0.667), weft satin (0.8), weft solid. Plain is left out (it ties 50% like
  2/2 and has no diagonal: the midtones should be twill), and 3/1 / 1/3 (0.25 / 0.75)
  sit too near the satins for a tone to choose between them steadily. Twill mode is
  3/1, 2/2, 1/3; Satin mode the two 5-end satins; Plain mode plain alone, so a plain
  cloth carries its picture only in the shuttles.
- **Structure is the nearest mixture in colour**, not luminance alone (traps).
- **Costs and the carry live on the sRGB encoding**; the means and the mixtures are
  linear light, because threads mix optically in linear light.
- **k-means is in linear light**, as the spec says, seeded by farthest point.
- **Picks 0 is Auto**: as many picks as make a crossing Thread Aspect times as tall as
  it is wide. With Picks set, Thread Aspect does nothing.
- **Thread Aspect is 0.5–2 on a log scale**, default 1 (square crossings).
- **Defaults**: 160 ends, Picks Auto, Max Float 10, a black warp (0.08, 0.07, 0.07),
  six shuttles from the clip, Auto, Thread Shading 1, Zoom at 1x, Mix 1. Chosen by weaving
  Resolume's bundled demo clips through `--pipe` (IntoTheGlow_02, Ethnik2, Metalive 01,
  NeonRoom2_32, Galactucity_21, Cyberspace_09, FogAndDust_3, Beat 003): the black warp
  keeps VJ-style black grounds black, and Max Float 10 (one tie in eleven) keeps their
  ties sparse.
- **Output alpha is 1**: a cloth has no holes, and ties and floats are defined across
  the whole grid. Transparent regions of a DXV clip are woven as the black ground their
  premultiplied colour is. Mix blends the whole RGBA with the source.
- **Zoom is about the frame centre**, 1–8x, nearest crossing; the cloth is woven at the
  grid, not re-woven for the zoom.
- **No brocade / lampas mode.** The spec allows "a couple" of wefts a pick with such a
  structure; the controls it lists have no switch for it, and a second weft with its own
  float limits is a second programme. `--shuttle` checks one weft a pick.
- **The fixed swatch values are ours**, picked by eye; not measured from any dye.
- **About block and ATTRIBUTIONS are provisional hand copies**, `guide = ""`, in
  graticule's shape, until the backend's sync generates them.
- **Commit trailer** is `Claude Opus 5.5`, as the session was told, not the briefs'
  `Fable 5.1`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-25)

`tools/verify.sh` on this machine against a fresh universal Release build.

- **Optimal.** 960 woven picks (60 random cloths of 4–13 ends and a second frame of each
  that remembers the first, 2–8 shuttles, Max Float 2–6, all four modes) and 200
  arbitrary integer tables: every one equals the exhaustive search exactly, every table
  feasible. The greedy weaver is worse on 49 of 480 woven picks.
- **Floats.** 48 cloths (four modes x card, noise, black, white x Max Float 2, 4, 6), on
  the grid and read from the picture: longest float 6 against a limit of 6 at the
  largest, 0 infeasible picks.
- **Coverage.** All eight non-solid structures exactly their ratio over 3,600
  crossings: 720, 900, 1200, 1800, 1800, 2400, 2700, 2880. The solids tie 612 (0.17).
- **Twill.** 2/2, 3/1, 2/1 at 45°; 1/3 at aspect 2, 63.43°; 1/2 at aspect 0.5, 26.57°;
  each peak 1.0000 at exactly ( -end width, +pick height ).
- **Shuttle.** Card and noise, Clip x6, Clip x8, Bright x8, Heritage x3: 0 crossings off
  the grid's colour, 0 picks with two wefts, 0 off the shuttles.
- **Distance.** RMS error ratio against the best single colour: 0.575 (shaded) and 0.532
  (flat) at 1280 x 720; 0.537 at 320 x 180.
- **Resize.** Shuttles bit-identical and the loom's memory kept, out, held and back.
- **Alpha.** 0 pixels wrong at Mix 1, 0 and 0.5, at both rasters.
- **Negative controls** all fail their check; **the mutation** is caught by five checks.
- **The software renderer** agrees at 320 x 180 on every check.
- **No dead controls**, all 13, at 320 x 180 and 480 x 270.
- **Every shader compiles** through `glslc`, and none uses a GLSL 4.10 reserved word.
- **`--pipe`**: 2.5 frames in, 2 out; unknown cue exit 2; failed render exit 1 and one
  frame; `| head -c 1` exit 1; an option steps between cues.
- **The bundle** is universal, exports `_plugMain`, carries `com.stoatworks.ffgl.jacquard`
  and 0.1.0, ad-hoc signs; `oxbow` reports `SW Jacquard` / `JQ01` / `effect` and renders
  120 frames through `plugMain`.
- **Render cost**, best of three runs of 30 frames after a warm-up, `glFinish` both
  sides, on a machine shared with other builds:

  | grid | 720p | 1080p | 4K | of which CPU |
  | --- | --- | --- | --- | --- |
  | defaults, 160 x 90 | 2.21 ms | 2.88 ms | 3.21 ms | 1.4 ms |
  | largest, 320 x 180 | 5.69 ms | 5.79 ms | 7.77 ms | 4.9 ms |

  The CPU half is the shuttles' k-means and the loom's programme for every pick; fax's
  is about 10 ms.

### Assumed, or not done

- **Never loaded into Resolume**, on either platform. Everything numeric was compiled,
  rendered and measured offline against the real plugin class in a headless CGL context,
  plus an `oxbow` load. How the read-back stall behaves inside a busy host is untested.
- **The look on footage is judged by eye**, on the demo clips above through `--pipe`,
  and so is "reads as thread up close" (docs/close.png). `--distance` measures the far
  half of that claim on the card only.
- **Temporal stability is measured once, not checked**: see the trap.
- **The Windows build** has never been configured, let alone run.
- **The fixed swatches, the cost constants (kTie, kLattice, the kHold terms) and the
  shading constants** are chosen, not derived.
- **No brocade mode, no presets, no OpenFX port, no browser demo, no user guide.**

---

## Open questions

- **Should the carry look ahead?** A pick cannot see the picks below; two saturated hues
  side by side (the halves test) weave as uneven alternation rather than even halves. A
  two-pick look-ahead, or choosing shuttles for a band of picks at once, would help and
  would cost the exactness story nothing (it is outside the programme).
- **A brocade mode**: a supplementary weft a pick, floating behind where unused. The
  natural answer to the halves problem, and the spec's own suggestion.
- **Should temporal stability be a check?** A still-with-noise source through `--pipe`,
  asserting the cloth changes on no more crossings than the source does, would hold the
  memory terms to a number.
- **Holes in the cloth?** Weaving only where a DXV clip is opaque would suit layered
  compositions; it would also make floats and ties undefined at the edges of the holes.
- **k-means in the encoding** rather than linear light would spend more shuttles on the
  darks, which most VJ footage is.
- **Solid tie density**: the lattice discount could be raised until flat solids tie in
  exact satin order; nothing measured says the current 0.17 looks worse.

---

## Siblings

- **teletext** — the skeleton, the CPU encoder over a read-back, the exhaustive search,
  the negative-control pattern, the two-raster discipline.
- **fax** — stepped cues, the failed-render exit path, the software renderer pass.
- **tinsel** — `PassBuffer`, `sweep.py`, CI, and the fleet's trap list.
- **rosette**, **intaglio** — how print and line structures carry tone; read, not copied.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
