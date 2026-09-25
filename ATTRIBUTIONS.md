# Attributions

Jacquard is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

PROVISIONAL: a hand copy in the shape `stoatworks-backend/scripts/sync-attributions.py`
generates. Jacquard is not yet registered in the backend's master lists; once it is, the
sync overwrites this file.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Effect skeleton, the harness and the --pipe contract — Stoatworks teletext

<https://github.com/stoatworks-labs/teletext>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin skeleton, the About block, the harness's shape (a headless CGL context driving the real plugin class, --list, --set, --dump-shaders), the read-back of a reduced grid to a CPU encoder that is exact in integers and checked against an exhaustive search, the verify script and the negative-control pattern are teletext's, which had them from rebate.

### Stepped cues and the software-renderer pass — Stoatworks fax

<https://github.com/stoatworks-labs/fax>  
Licence: MIT  
Copyright: Stoatworks Labs

Options, booleans, integers and events stepping between --pipe cues instead of ramping, the failed-render exit path, and the JQTEST_RENDERER=software pass (Apple's generic float renderer by id) follow fax's harness.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer is tinsel's, with the colour-texture leak in the SDK's FFGLFBO::Release fixed.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9 like the rest of the fleet.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0, and through vcpkg on Windows.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The jacquard loom, and the people who weave pictures on it

Joseph-Marie Jacquard's head (1804) lifts every warp end on its own, which is what made woven pictures possible at all. The vocabulary here — ends and picks, floats and ties, plain, twill and satin, a weft colour a pick from the shuttles loaded — is the weaver's, from general textile knowledge. No woven design, draft or pattern was copied.

## Standards and published specifications

What the implementation is measured against.

- **IEC 61966-2-1 (sRGB)** — The transfer function both ways: cell means are taken in linear light, and costs are measured on the encoding.
- **ITU-R BT.709** — The luma coefficients 0.2126, 0.7152, 0.0722 that order the shuttles on a cold start.
- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — The pcg_hash output mix behind the harness's random pictures and the scramble perturbation, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
