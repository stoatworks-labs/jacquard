#pragma once

#include <cstdint>

/**
	The loom's vocabulary: the weave structures, which of them a Structure
	setting may use, the tie lattice, and the test hooks.

	A crossing is one of two things: the **warp** (the vertical thread, end i)
	on top, or the **weft** (the horizontal thread, pick j) on top. `WarpUp`
	says which, for a structure at an ABSOLUTE grid position -- every pattern
	is phase-aligned to the grid, so two cells of the same structure anywhere
	in the cloth agree and a twill's diagonal runs straight across a region
	whatever the region's shape.

	The structures, by weft coverage (the fraction of crossings where the weft
	shows, over a whole repeat):

	| structure      | repeat | weft coverage | longest float |
	| ---            | ---    | ---           | ---           |
	| warp solid     | 1      | 0             | unbounded     |
	| warp satin     | 5      | 1/5  = 0.20   | 4 (warp)      |
	| 3/1 twill      | 4      | 1/4  = 0.25   | 3 (warp)      |
	| 2/1 twill      | 3      | 1/3  = 0.333  | 2 (warp)      |
	| plain          | 2      | 1/2  = 0.50   | 1             |
	| 2/2 twill      | 4      | 1/2  = 0.50   | 2             |
	| 1/2 twill      | 3      | 2/3  = 0.667  | 2 (weft)      |
	| 1/3 twill      | 4      | 3/4  = 0.75   | 3 (weft)      |
	| weft satin     | 5      | 4/5  = 0.80   | 4 (weft)      |
	| weft solid     | 1      | 1             | unbounded     |

	`a/b twill` is warp over a, under b: a 3/1 twill is warp-faced. Every
	twill steps one end per pick, so its diagonal runs from the lower left to
	the upper right at atan( pick height / end width ). The satins step two
	ends per pick over five, which scatters the ties so no diagonal forms.

	The two solids are the ends of the tone range, and they are the reason the
	float limit exists: a solid is one unbroken float, and the encoder must tie
	it down (`Encoder.h`). The tie lattice is where it prefers to: a satin
	order of period Max Float + 1, one tie per period in every row and every
	column, which is the minimum density and has no diagonal. Since v0.1.1 a
	tie off the lattice is charged its speck twice (`Loom.h`), which is what
	makes the preference hold over a black ground.
*/
namespace jacquard::weave
{

enum Structure
{
	kWarpSolid = 0,
	kWarpSatin,
	kTwill31,
	kTwill21,
	kPlain,
	kTwill22,
	kTwill12,
	kTwill13,
	kWeftSatin,
	kWeftSolid,
	kStructureCount
};

/// The Structure control, in menu order.
enum Mode
{
	kModeAuto = 0,///< tone picks from every structure but plain
	kModePlain,
	kModeTwill,   ///< 3/1, 2/2, 1/3 by tone
	kModeSatin,   ///< warp satin, weft satin by tone
	kModeCount
};

const char* ModeName( int mode );
const char* StructureName( int structure );

/// The repeat, in ends and in picks (square for every structure here).
int Repeat( int structure );

/// Weft coverage over one repeat, as the exact fraction num / den.
void Coverage( int structure, int& num, int& den );
double Coverage( int structure );

/// The structures a mode may choose from, in order of rising weft coverage.
/// Returns the count, <= kStructureCount.
int Family( int mode, int* out );

/// Test hooks, a bitmask. Always 0 in the plugin; they exist so
/// `jqtest --negative` can prove each check can fail.
enum Perturb
{
	kPerturbGreedy      = 1 << 0,///< greedy tie placement instead of the programme
	kPerturbNoFloatLimit = 1 << 1,///< the float constraint dropped
	kPerturbTwillStep0  = 1 << 2,///< twills do not step: warp ribs, no diagonal
	kPerturbCoverage    = 1 << 3,///< every twill one warp crossing heavier per repeat
	kPerturbTwoWefts    = 1 << 4,///< the render draws odd ends in the next shuttle's colour
	kPerturbScramble    = 1 << 5,///< each pick's weft replaced by a hashed palette entry
	kPerturbColdResize  = 1 << 6,///< the shuttles and the loom forget themselves when the raster changes
	kPerturbAlphaThrough = 1 << 7,///< the cloth takes the clip's alpha instead of being opaque
	kPerturbTies010     = 1 << 8,///< v0.1.0's loom: the lattice's discount alone, its first coprime step, its shuttle hold
};

/// Warp on top at end i, pick j, for this structure. Phase-aligned: i and j
/// are absolute grid positions.
bool WarpUp( int structure, int i, int j, int perturb = 0 );

/// The satin step of the tie lattice for period n: of the s in [2, n - 2]
/// coprime with n, the one whose lattice has the longest shortest vector
/// (ties farthest apart), the smallest on a draw; else 1. With
/// kPerturbTies010, v0.1.0's rule: the smallest such s.
int LatticeStep( int period, int perturb = 0 );

/// On the tie lattice for a float limit M: ( i + s j ) mod ( M + 1 ) == 0.
bool OnTieLattice( int i, int j, int maxFloat );

/// A 32-bit integer hash (PCG output mix), for the harness's random rows and
/// the scramble perturbation.
uint32_t Hash( uint32_t a, uint32_t b, uint32_t c );

} // namespace jacquard::weave
