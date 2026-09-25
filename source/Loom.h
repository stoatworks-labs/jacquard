#pragma once

#include "Palette.h"

#include "Encoder.h"

#include <cstdint>
#include <functional>
#include <vector>

/**
	The loom: a frame's cell means in, a woven cloth out. CPU, no GL, so the
	harness can drive it directly as well as through the plugin.

	The picks are woven in order, top to bottom, the way a loom weaves them.
	For each pick:

	1. **Target.** Each crossing's cell mean (linear light) plus the error
	   carried down from the pick above (see below).
	2. **Tone to structure**, per shuttle colour c. Mixing c with the warp
	   colour W at a structure's weft coverage f gives the colour the cloth
	   shows there from a distance: f c + (1 - f) W, in linear light, which
	   is how threads mix optically. The structure is the one in the mode's
	   family whose mixture's luminance, sRGB-encoded, is nearest the
	   target's. Its pattern, phase-aligned to the grid, is what the crossing
	   would like to be.
	3. **Costs**, integers: a crossing that follows its structure costs the
	   squared error of the structure's mixture against the target, on the
	   sRGB encoding, x 65536. One that does not -- a tie -- costs that plus
	   kTie plus the squared error of the thread it shows instead (a warp tie
	   in a dark weft solid is a light speck, and costs what a light speck
	   costs there), less kLattice on the tie lattice -- and, OFF the
	   lattice, that speck's error again, kOffLattice times over.
	4. **The programme** (`Encoder.h`) chooses the weft colour and the
	   pattern, exactly, with the warp runs of the picks above as forbidden
	   values.
	5. **Carry.** A pick is one colour across the whole width, so where the
	   picture wants red on the left and blue on the right no single pick can
	   give both. The error each crossing is left with -- target minus the
	   structure mixture it got, in linear light -- is carried to the next
	   pick, spread 1/4, 1/2, 1/4 over the crossing and its neighbours and
	   scaled by kCarry. The next pick then wants the colour the last one
	   could not give, the shuttles alternate, and from a distance the picks
	   mix. That alternation is the colour banding along rows a woven
	   picture has.

	The tie lattice terms are integers the programme optimises like any other;
	they make the least-cost ties land in satin order, one per Max Float + 1
	in every row and column, so they scatter instead of lining up.

	**Why the lattice needs the second term** (v0.1.1). The carry varies from
	end to end by a few hundredths on the encoding even over a flat black
	ground (measured: 0.025 rms, Galactucity), and it runs down the ends, so
	it is much the same pick after pick. A bright tie over black costs its
	speck's squared error, which follows the carry: about 2,600 units rms
	across a pick, five times kLattice. So in v0.1.0 the programme put each
	bright pick's ties wherever the carry made a speck cheapest, which was
	the same ends every time, and a black ground crossed by bright picks
	showed short vertical dashes. Nothing pushes back: the carry is the
	target less the STRUCTURE's mixture, so a tie's own speck is never owed.
	A bigger constant cannot fix it without letting a tie pay (kLattice must
	stay below kTie). Charging an off-lattice tie its speck twice scales with
	exactly the thing that varies: the carry would have to halve a speck's
	error to move it off the lattice, which only the picture itself does.
	On-lattice ties cost what they did; a tie that shows nearly the target's
	own colour (an invisible tie) is almost as free to move as before.
*/
namespace jacquard::loom
{

/// Cost units: a squared error on the encoding, x 65536, rounded.
constexpr double kCostScale = 65536.0;
/// A tie's own cost, over and above what it shows: 1/64 of a unit error.
constexpr int64_t kTie = 1024;
/// The lattice's discount on a tie. Less than kTie, so a tie never pays.
constexpr int64_t kLattice = 512;
/// Off the lattice a tie pays for what it shows this many times again: a
/// speck out of satin order costs twice its error (see above). Integer, so
/// the table stays integer and the programme exact.
constexpr int64_t kOffLattice = 1;
/// The share of a pick's error carried to the next: all of it, as error
/// diffusion does, so a colour a region is owed keeps accumulating until a
/// pick pays it.
constexpr float kCarry = 1.0f;
/// The most a crossing may be owed, per channel, in linear light. A colour
/// no shuttle and no mixture can reach would otherwise accumulate without
/// bound and hold its column hostage for the rest of the cloth.
constexpr float kCarryLimit = 0.5f;

/// Memory. A pick is woven again every frame, and error carried down the
/// cloth is chaotic: left alone, a change of one crossing at the top moves
/// ties and shuttles all the way down, and a still clip shimmers. So the
/// table charges, over and above what a crossing shows:
///   kHoldLift     for a crossing whose lift differs from the last frame's;
///   kHoldShuttle  per crossing, for a pick whose shuttle differs;
/// and a crossing keeps last frame's structure while that structure's error
/// is within kHoldStructure of the best one's. Integers in the table like
/// every other cost, so the programme is still exact for what it is given.
///
/// kHoldShuttle was 384 in v0.1.0. The off-lattice charge makes a pick's
/// total depend more on where the picture's solids fall against the fixed
/// lattice, and on the skulls clip it doubled the picks changing shuttle
/// from one frame to the next (7.1 to 13.4 of 90); at 768 it is 6.3, and no
/// clip measured changes more from frame to frame than it did in v0.1.0.
constexpr int64_t kHoldLift      = 2048;
constexpr int64_t kHoldShuttle   = 768;
constexpr int64_t kHoldStructure = 256;

struct Settings
{
	int ends  = 0;
	int picks = 0;
	palette::Colour warp;
	const palette::Colour* shuttles = nullptr;
	int shuttleCount = 0;
	int maxFloat     = 6;///< <= 0: unlimited
	int mode         = 0;///< weave::Mode
	int forcedStructure = -1;///< test hook: every crossing this structure
	int perturb      = 0;///< weave::Perturb bits
	/// Test hook: every pick's table and the programme's answer, as woven.
	/// Empty in the plugin; `jqtest --optimal` checks each against an
	/// exhaustive search.
	std::function< void( int pick, const encoder::RowTable&, const encoder::RowResult& ) > observe;
};

struct Cloth
{
	int ends  = 0;
	int picks = 0;
	std::vector< uint8_t > lift;     ///< picks x ends, row 0 top: 1 = warp on top
	std::vector< uint8_t > weft;     ///< per pick: the shuttle index
	std::vector< uint8_t > structure;///< picks x ends: the structure each crossing asked for
	int shuttleCount    = 0;         ///< what it was woven with, so the next
	int mode            = -1;        ///< frame knows whether it may remember
	int forcedStructure = -1;
	int64_t cost        = 0;         ///< the sum of the picks' programme costs
	int infeasiblePicks = 0;         ///< always 0 unless the model is perturbed
	bool remembered     = false;     ///< whether the last frame was remembered
	int programmes      = 0;         ///< colour programmes run, over the frame
};

/// Weave a frame. `means` is ends x picks linear RGBA (stride 4), row 0 top.
/// `previous`, when it is the same grid, shuttle count and structure mode,
/// is remembered (the kHold* costs); otherwise the frame is woven fresh.
/// `out` must not be `previous`.
void Weave( const float* means, const Settings& settings, Cloth& out, const Cloth* previous = nullptr );

} // namespace jacquard::loom
