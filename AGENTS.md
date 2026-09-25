# AGENTS.md — Jacquard

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

The picture woven on a jacquard loom, as an FFGL 2.1 effect (`JQ01`, shown as
`SW Jacquard`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal
macOS `.bundle` and a Windows `.dll` (built by CI; MSVC compiled it first time).
MIT, at `github.com/stoatworks-labs/jacquard`. Released v0.1.0 on 2026-09-25, and v0.1.1
the same evening: ties over a black ground scatter in satin order instead of lining up
("Ties over black lined up", below).

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
     the thread it shows, less kLattice on the tie lattice and **plus that error again
     off it** (kOffLattice, v0.1.1); memory terms on top;
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

### Ties over black lined up (fixed in v0.1.1)

Filming v0.1.0 found it: where a bright pick crosses a black ground, the ties sat in the
same ends pick after pick, short vertical dashes with dark picks between, not a satin
scatter. Measured from the picture (`--ties`, and the same measure over the demo clips
through `--pipe`): on Galactucity a bright speck over black had a speck 2 picks above it
in the same end **26% of the time** (4,317 of 16,445), where chance is 1.1%.

**The cause, measured** (scratch hooks in Loom.cpp, since removed). Over Galactucity's
black ground the target is flat (0.0007 rms on the encoding across a pick) but the carry
is not: 0.025 rms from end to end, and it runs down the ends, so it is much the same pick
after pick. A bright tie over black costs its speck's squared error against target plus
carry, so its cost follows the carry: **2,566 units rms across a pick, five times
kLattice (512)**. Only 17% of the bright ties landed on the lattice (chance 9%); 95% of
them were chosen, not forced by a warp run. So the programme, exact as ever, put each
bright pick's ties where the carry made a speck cheapest: the same ends every time.
Nothing pushes back, because the carry is the target less the STRUCTURE's mixture, so a
tie's own speck is never owed. That was half of the old guess (the clamp was not it).
The lift hold is not it either: with kHoldLift 0 the dashes were more regular.

**Why not a small offset.** kLattice at 1000 (the most it can be below kTie, so that a
tie never pays) changed nothing: 17.8x chance against 17.9x. A constant big enough to
win (20,000 worked) would let a tie pay wherever it shows little error, and put ties into
every structure at the lattice (`--coverage` would fail).

**Why not a penalty on a tie below a tie.** The dashes are at lag 2 (a bright pick, a
dark pick, a bright pick), so a previous-pick penalty cannot see them; a window of picks
needs per-end history in the costs, the carry would still choose among the ends it left,
and ties placed by history move whenever the picture does.

**The fix: charge an off-lattice tie its speck twice.** `tie = kTie + shown - kLattice`
on the lattice (as before) and `kTie + shown + kOffLattice x shown` off it, kOffLattice
1. It scales with exactly the thing that varies: the carry would have to halve a speck's
error to move it off the lattice, which only the picture itself does. On-lattice ties
cost what they did, a tie that shows nearly the target's colour is almost as free as
before, and the lattice is fixed to the grid, so the ties are deterministic and stay put
in a still ground whatever moves elsewhere. Integers, so `--optimal` is untouched. Half
the charge (0.5) left 1.6x chance on the figures ground; 2 was no better than 1.

**Two things it exposed.**
- **The lattice step was a twill.** LatticeStep took the smallest step coprime with the
  period: 2 of 11 at the default Max Float 10, whose ties, once they obeyed the lattice,
  read as diagonal lines. It now takes a weaver's satin counter: of the coprime steps,
  the one whose ties lie farthest apart (the longest shortest vector of the lattice, the
  smallest step on a draw). Changed at Max Float 10, 12, 14 and 16 (periods 11, 13, 15,
  17: steps 3, 5, 4, 4, from 2); the rest were already that. The first C++ draft
  computed `-s b mod n` through `umod`, which casts a negative to unsigned and is wrong
  for any period that does not divide 2^32; the demo's port check found it (the JS was
  right), before anything was committed.
- **More shuttle changes on busy footage.** The charge makes a pick's total depend more
  on where the picture's solids fall against the fixed lattice: on the skulls
  (NoHopeJustFear_44) the picks changing shuttle from frame to frame went from 7.1 to 13.4
  of 90, and the pixels changing from 28.3% to 30.1%. kHoldShuttle 384 -> 768 brings it
  to 6.3 picks and 27.4%, and no clip measured is less steady than in v0.1.0.

**Measured before and after**, 120 frames at 960 x 540 through `--pipe` at the defaults
(alignment and error with Thread Shading 0, steadiness with it on; alignment is a speck
above a speck, in the same end, 1 / 2 picks up, over chance; steadiness is the share of
pixels changing by more than 8/255 from one frame to the next after the first ten, the
source premultiplied by its alpha; error is the RMS on the encoding of 10 x 10-crossing
block means against the source):

| clip | alignment v0.1.0 | v0.1.1 | steadiness (source) v0.1.0 | v0.1.1 | error v0.1.0 | v0.1.1 |
| --- | --- | --- | --- | --- | --- | --- |
| Galactucity, one frame held | 12.8x / 13.7x | 0.00 / 0.00 | 0.00% (0.00%) | 0.00% | 0.0725 | 0.0730 |
| Galactucity_21 (moving) | 17.7x / 23.3x | 0.09 / 0.10 | 13.2% (10.4%) | 11.3% | 0.0737 | 0.0753 |
| SpaceUniverse_04 (near still) | 11.9x / 128x | 0.00 / 0.00 | 1.5% (1.9%) | 1.5% | 0.0327 | 0.0314 |
| Metalive 01 | 10.0x / 8.7x | 0.11 / 0.02 | 13.6% (18.5%) | 12.0% | 0.0919 | 0.0924 |
| NoHopeJustFear_44 (skulls) | 0.72 / 0.34 | 0.38 / 0.43 | 28.3% (76.5%) | 27.4% | 0.0678 | 0.0685 |
| Cyberspace_09 | 26.9x / 153x | 3.4x / 5.5x | 16.9% (8.1%) | 16.1% | 0.0655 | 0.0649 |

Cyberspace's v0.1.1 ratios are 2 and 3 specks of about 900 (p = 0.0006), against 17 and
89 in v0.1.0; the skulls have almost no black ground. The error is up by 0.2–2% on four
clips and down on two: ties in satin order are a few more than the fewest (Galactucity's
held frame 16,320 specks against 15,960). The script is not in the repo (the numbers
need the demo clips); `--ties` is the check that holds the claim.

### Least-cost is not fewest

In v0.1.0 a forced solid on a flat field tied 612 of 3600 crossings (0.17) at Max Float
8, not the 1 in 9 the lattice alone would give: the carried target varied by more than
the lattice's discount from crossing to crossing, the ties landed off the lattice, and
the column runs then forced more. The same cause as the aligned ties, above; since
v0.1.1 the solids tie **exactly 400 of 3600, one in nine**, in satin order. The
programme is exact for the costs it is given; "least cost" is the claim, not "fewest
ties". `--coverage` states the solids' tie density and asserts only the lower bound the
float limit implies.

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
| `--ties` | over a black ground (every source pixel of the crossing black), a speck (a crossing showing neither the warp nor anything darker) with a speck 1, 2 or 3 picks above it in the same end, against the share of black crossings that are specks | **chance itself**: at no lag above p, what ties with no vertical order would give; no fitted number; at least 100 specks | the grid (160 x 90) is the same at 320 x 180 and 1280 x 720, and each crossing is classified at its centre pixel as `--floats` does; worst 0.52 of chance at 1280 x 720, 0.30 at 320 x 180, v0.1.0 7.9–8.2x |
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

`jqtest --negative` runs nine; `--perturb BITS` runs any check verbosely against one.
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
| v0.1.0's loom (`kPerturbTies010`: no off-lattice charge, the first coprime step, kHoldShuttle 384) | `--ties`: 4.2x, 8.2x, 1.9x, 2.6x chance (figures and disc, Max Float 6 and 10); byte-identical to the v0.1.0 binary through `--pipe` on 240 frames of footage |

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

### The mutation, v0.1.1

One character of the shipped C++, on a clean committed tree: `kOffLattice = 1` became
`kOffLattice = 0` in `Loom.h` (v0.1.0's tie cost, with the new step and hold).
Caught at both rasters by **`--ties`** (8.34x chance at 320 x 180, 8.25x at 1280 x 720), and by
`demo/tools/check_shaders.py` (the constant no longer matches the page's). Correctly not
caught by `--optimal` (exact for whatever costs it is given), `--coverage` (the solids' tie
density went back to 612 of 3600, above the bound it asserts) or `--floats`. Reverted with
`git checkout source/Loom.h` and a `touch`; the tree was clean before and after (942a316),
and the rebuilt `--ties` passes.

## The browser demo

`demo/` is the page at **jacquard-demo.stoatworks-labs.com** (2026-09-25), on the
fleet's kit (`stoatworks-backend/resolume-demo`, vendored by its `sync.sh`).

**What is the plugin's.** The version line and the three GLSL bodies of `Shaders.cpp`
are spliced into `demo/plugin.js` by `demo/tools/sync_shaders.py`, tabs and comments
included; the same script copies Loom.h's cost and memory constants, Controls.h's
ranges, Palette.h's, the two swatch cards and the Palette / Structure option names
into `demo/loom.js`. `demo/tools/check_shaders.py` holds all of it to the C++
character for character (`tools/verify.sh` runs it; mutating one character of a
shader, one constant or one option name each made it fail).

**What is a hand port, checked by nobody but a reader:** `demo/loom.js` —
Controls.cpp's conversions (Picks Auto, Thread Aspect, Zoom, the integer clamps with
`lround`), Palette.cpp (sRGB both ways, the swatch cards, the farthest-point-seeded,
warm-started k-means), Weave.cpp (coverage, repeats, families, `WarpUp`, the lattice
step), Encoder.cpp (the row programme and its colour bound; not the greedy weaver)
and Loom.cpp (the encoding table, tone to structure, the integer costs, the kHold
memory, the forbidden crossings, the carry) — and in `plugin.js` the frame sequence of
`ProcessOpenGL` (the read-back as floats, the palette reset on a Palette change, the
lift upload). Every float operation is wrapped in `Math.fround`; costs are integers in
doubles, exact. The page says so in its banner and disclosure, and states the plugin's
own limits: exact per pick and greedy between picks, the float limit enforced inside
each pick, one weft colour a pick, output alpha 1.

**Measured once (2026-09-25), and how closely.**
- *The CPU half alone*: a scratch driver over the C++ `Palette`/`Loom`/`Encoder`/`Weave`
  (compiled `-O3`, three ways) and a node run of `demo/loom.js` over the same fixed
  means: 31 frames, seven grids from 16 x 9 to 320 x 320, every palette, every
  structure mode, Max Float 2 to 16, a warm start carried across frames and a cold
  start mid-run. Against the **x86_64** build: identical, bit for bit — shuttles,
  wefts, lifts, the total cost and the programme count. Against the **arm64** build
  (what `build/jqtest` is; clang's default `-ffp-contract=on` fuses some float
  multiply-adds, 48 `fmadd`s in Loom.o): the same weft and lift on all 608,292
  crossings, but 6 shuttle floats differ in the last bit and the frame's total cost
  differs by a few units on 25 of 31 frames. So the port reproduces the Intel slice's
  arithmetic exactly and the Apple Silicon slice's cloth on everything tried; a
  crossing on a knife edge could still go the other way there.
- *End to end*: the page driven frame by frame from a fresh instance
  (`window.__jacquardDemo.hooks`: `fresh()`, `afterRender`), its input frames read back
  and piped through `jqtest --pipe` (arm64): **0 pixels differ** on 8 frames of the
  moving Synthetic scene and 5 of Colour bars at 960x540 at the defaults, on 5 frames of
  the Geometry card at 640x360 with Heritage x8, Twill, Max Float 2, 96 ends, Thread
  Aspect 0.8, and 1 to 2 pixels by one level on 3 frames with Mix 0.5, Zoom 0.5, Thread
  Shading 0.6 (ANGLE on Metal, M4 Max). Through SwiftShader, 0 to 230 pixels of 518,400
  differ, by one level. The comparer fails when it should: jqtest at Max Float 9 against
  the page at 10 differs on 17–21% of pixels.

**Re-measured for v0.1.1 (2026-09-25).** `demo/loom.js` carries the off-lattice charge
and the satin counter by hand; the constants (kOffLattice, kHoldShuttle 768) by
`sync_shaders.py`. The same scratch driver, with two black-ground scenarios added (160 x 90
at the defaults, and Bright x4 at Max Float 12, both over two drifting figures): against
the **x86_64** build 41 of 41 frames identical bit for bit (shuttles, wefts, lifts, cost,
programme count) over 731,556 crossings; against **arm64** the same weft and lift on every
crossing, 10 shuttle floats a last bit apart and the total cost a few units off on 35
frames, as in v0.1.0. End to end, the page served locally from the worktree and driven headlessly (ANGLE on
Metal, M4 Max), its input frames piped through `jqtest --pipe` (arm64): **0 pixels differ** on
8 frames of the Synthetic scene and 5 of Colour bars at the defaults, 5 of the Geometry card
at Max Float 12 with Bright x4, and 4 of the scene at Max Float 16 in Satin (both steps that
v0.1.1 changed), 960x540. The comparer still fails when it should: jqtest with `--perturb
256` (v0.1.0's loom) against the v0.1.1 page differs on 17–22% of pixels.

**What differs, each said on the page:** Ends, Picks, Max Float and Shuttles are
dropdowns (no integer control in the kit); no About block; `Perturb` 0 and no forced
structure; the loom's memory is lost on a reload; a browser's GPU rounds the crossing
means its own way, so another GPU may choose a crossing differently. No audio and no
clock caveat: the plugin has neither.

Deploy: `cf-run npx wrangler deploy` from the repo root, or push to main
(`.github/workflows/deploy.yml`). The host is a Worker **route** over a proxied
`AAAA 100::` record made through the API on 2026-09-25, not a custom domain: the zone
is at Cloudflare's limit of 100. Delete that record and the page goes dark while deploys
stay green. Verify by content:
`curl -s 'https://jacquard-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

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
- **The off-lattice charge (v0.1.1)** over a small offset or an adjacency penalty: see
  "Ties over black lined up". kOffLattice 1, kHoldShuttle 768 and the satin counter are
  chosen by measurement on the demo clips, like the other cost constants.
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
- **About block, ATTRIBUTIONS and the issue forms are generated** since registration
  (2026-09-25): `sync-about.py`, `sync-attributions.py --adopt`, `sync-issue-templates.py`. The
  guide button made it 18 parameters.
- **Commit trailer** is `Claude Opus 5.5`, as the session was told, not the briefs'
  `Fable 5.1`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-25)

`tools/verify.sh` on this machine against a fresh universal Release build; for v0.1.1,
re-run in full the same evening.

- **Optimal.** 960 woven picks (60 random cloths of 4–13 ends and a second frame of each
  that remembers the first, 2–8 shuttles, Max Float 2–6, all four modes) and 200
  arbitrary integer tables: every one equals the exhaustive search exactly, every table
  feasible. The greedy weaver is worse on 54 of 480 woven picks (49 in v0.1.0).
- **Floats.** 48 cloths (four modes x card, noise, black, white x Max Float 2, 4, 6), on
  the grid and read from the picture: longest float 6 against a limit of 6 at the
  largest, 0 infeasible picks.
- **Coverage.** All eight non-solid structures exactly their ratio over 3,600
  crossings: 720, 900, 1200, 1800, 1800, 2400, 2700, 2880. The solids tie 400 (1/9,
  the satin minimum; 612 in v0.1.0).
- **Ties** (v0.1.1). Over a black ground crossed by bright picks (two figures and a bar;
  a disc; Max Float 6 and 10), a speck above a speck in the same end at 1, 2 or 3 picks is
  at most 0.52 of chance at 1280 x 720 and 0.30 at 320 x 180; v0.1.0's loom 7.9–8.2x.
- **Twill.** 2/2, 3/1, 2/1 at 45°; 1/3 at aspect 2, 63.43°; 1/2 at aspect 0.5, 26.57°;
  each peak 1.0000 at exactly ( -end width, +pick height ).
- **Shuttle.** Card and noise, Clip x6, Clip x8, Bright x8, Heritage x3: 0 crossings off
  the grid's colour, 0 picks with two wefts, 0 off the shuttles.
- **Distance.** RMS error ratio against the best single colour: 0.579 (shaded) and 0.531
  (flat) at 1280 x 720; 0.537 at 320 x 180 (v0.1.0: 0.575, 0.532, 0.537).
- **Resize.** Shuttles bit-identical and the loom's memory kept, out, held and back.
- **Alpha.** 0 pixels wrong at Mix 1, 0 and 0.5, at both rasters.
- **Negative controls** all nine fail their check; **the mutations** are caught (the
  shader's by five checks, v0.1.1's C++ one below).
- **The software renderer** agrees at 320 x 180 on every check.
- **No dead controls**, all 13, at 320 x 180 and 480 x 270.
- **Every shader compiles** through `glslc`, and none uses a GLSL 4.10 reserved word.
- **`--pipe`**: 2.5 frames in, 2 out; unknown cue exit 2; failed render exit 1 and one
  frame; `| head -c 1` exit 1; an option steps between cues.
- **The bundle** is universal, exports `_plugMain`, carries `com.stoatworks.ffgl.jacquard`
  and 0.1.1, ad-hoc signs; `oxbow` reports `SW Jacquard` / `JQ01` / `effect` and renders
  120 frames through `plugMain`.
- **Render cost**, best of three runs of 30 frames after a warm-up, `glFinish` both
  sides, on a machine shared with other builds:

  | grid | 720p | 1080p | 4K | of which CPU |
  | --- | --- | --- | --- | --- |
  | defaults, 160 x 90 | 1.84 ms | 1.97 ms | 2.04 ms | 1.3 ms |
  | largest, 320 x 180 | 5.54 ms | 5.72 ms | 6.72 ms | 4.7 ms |

  (v0.1.1, 2026-09-25 evening; v0.1.0's run on a busier machine read 2.21–3.21 and
  5.69–7.77 ms. The off-lattice charge is one multiply a crossing.)

  The CPU half is the shuttles' k-means and the loom's programme for every pick; fax's
  is about 10 ms.

### Assumed, or not done

- **Never loaded into Resolume**, on either platform. Everything numeric was compiled,
  rendered and measured offline against the real plugin class in a headless CGL context,
  plus an `oxbow` load. How the read-back stall behaves inside a busy host is untested.
- **The look on footage is judged by eye**, on the demo clips above through `--pipe`,
  and so is "reads as thread up close" (docs/close.png). `--distance` measures the far
  half of that claim on the card only.
- **Temporal stability is measured, not checked**: see the trap, the filming bullet below
  for v0.1.0's numbers on eight moving clips, and "Ties over black lined up" for v0.1.1's
  against v0.1.0 on six.
- **Windows, in Resolume Arena 7.27.1** (win-lab, Mesa llvmpipe, no GPU, 2026-09-25): On Windows it has: the DLL release.yml built from this source loads in Resolume Arena 7.27.1 on software rendering (win-lab, Mesa llvmpipe, no GPU), registers as `SW Jacquard` / `JQ01` / effect, all 19 host controls match what the plugin declares, it renders, Arena's log stays clean, and all 14 valued controls move the picture (35 to 66 levels against a noise floor of 0): 9 of the fleet Arena gate's 9 checks, one run. The gate's picture is a still, so it says nothing about how the cloth moves; software rendering says nothing about a GPU or about speed. MSVC compiled it first time.
- **The fixed swatches, the cost constants (kTie, kLattice, the kHold terms) and the
  shading constants** are chosen, not derived.
- **No brocade mode, no presets, no OpenFX port.** There is a user guide
  (`docs/USER-GUIDE.md`, the only copy anyone edits; the PDF and the site page are generated by
  the website's `build_guides.py`) and a browser demo (above).
- **Filming the release video** (`stoatworks-backend/video/projects/jacquard/`): The video is rendered through `jqtest --pipe` over Resolume's demo clips. Filming found no
defect in the code, and three facts now in the guide. **Plain on a grey clip loses the picture
entirely**: every crossing is the same plain weave and a pick is one shuttle, so only each pick's
average is left, and a grey clip's rows all average alike. **Over a black ground crossed by a
bright pick the ties line up** into short vertical dashes with dark picks between, not a satin
scatter; it is there on a single frame, so it is the per-pick weave, not the memory (a scratch
build with the lift hold at 0 made the columns more regular, and doubled the frame-to-frame
change). **Fixed in v0.1.1** (the trap above); the video shows v0.1.0 and says so. And **steadiness on moving footage**, measured through `--pipe` at 960×540 as the share
of pixels changing by more than 8/255 from one frame to the next: a held frame 0.00%; the dancers
(Galactucity) 10.4% in the clip and 13.2% in the cloth; the skulls 76.9% and 28.3%; Metalive 18.6%
and 13.6%; SpaceUniverse 1.9% and 1.5%; Cyberspace's thin bright lines 7.5% and 16.9%, with one
frame in ten re-weaving half the picture.

---

## Open questions

- **Should the carry look ahead?** A pick cannot see the picks below; two saturated hues
  side by side (the halves test) weave as uneven alternation rather than even halves. A
  two-pick look-ahead, or choosing shuttles for a band of picks at once, would help and
  would cost the exactness story nothing (it is outside the programme).
- **A brocade mode**: a supplementary weft a pick, floating behind where unused. The
  natural answer to the halves problem, and the spec's own suggestion.
- ~~Why do ties over black line up?~~ Answered and fixed in v0.1.1: the carry, not the clamp
  (see the trap). Still open from it: whether a tie's own speck should be owed in the carry,
  which would push ties apart by error diffusion rather than by the lattice. Not done: it
  changes every pick's colour decision, and the lattice already scatters them.
- **Should temporal stability be a check?** A still-with-noise source through `--pipe`,
  asserting the cloth changes on no more crossings than the source does, would hold the
  memory terms to a number.
- **Holes in the cloth?** Weaving only where a DXV clip is opaque would suit layered
  compositions; it would also make floats and ties undefined at the edges of the holes.
- **k-means in the encoding** rather than linear light would spend more shuttles on the
  darks, which most VJ footage is.
- ~~Solid tie density~~: since v0.1.1 flat solids tie in exact satin order, 1 in 9 at Max
  Float 8 (the off-lattice charge did it, not a bigger discount).

---

## Siblings

- **teletext** — the skeleton, the CPU encoder over a read-back, the exhaustive search,
  the negative-control pattern, the two-raster discipline.
- **fax** — stepped cues, the failed-render exit path, the software renderer pass.
- **tinsel** — `PassBuffer`, `sweep.py`, CI, and the fleet's trap list.
- **rosette**, **intaglio** — how print and line structures carry tone; read, not copied.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
