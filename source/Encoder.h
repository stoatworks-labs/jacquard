#pragma once

#include <cstdint>
#include <vector>

/**
	The row encoder: one pick's weft colour and its warp-up / weft-up
	pattern, exactly optimal under the float constraint.

	**The problem.** A pick of n crossings. Choose one weft colour c from the K
	shuttles loaded, and for each crossing i whether the warp (x = 1) or the
	weft (x = 0) is on top, to minimise

		sum over i of  cost[ c ][ i ][ x_i ]

	subject to:

	- **no weft float longer than M**: no run of more than M equal x along
	  the row (a run of 0s is the weft floating on the face, a run of 1s the
	  weft floating behind the warp; both snag);
	- **no warp float longer than M**: some crossings are FORBIDDEN a value,
	  because the warp end above has already run M picks on that side. This
	  is how the column constraint enters: the rows are woven in order, top to
	  bottom, as a loom weaves them, and each row inherits the column runs of
	  the rows before it.

	The costs are integers (`Loom.cpp` builds them), so "exactly optimal" is a
	claim the harness checks against an exhaustive search.

	**The programme.** For each colour, a dynamic programme along the row with
	the run as its state: (value on top, run length 1..M), 2M states. A run
	extends if it is shorter than M; either value may start a new run of 1;
	a forbidden value is never taken. The colours are tried in order of a
	lower bound (each crossing's cheaper value, ignoring every constraint) and
	the search stops when the bound reaches the best row found, so usually
	one or two programmes run per row and the answer is still the joint
	optimum over colour and pattern.

	**Always feasible.** If the row above satisfied the row constraint, its
	complement satisfies every forbidden value here (a crossing is forbidden
	exactly the value the row above has there) and has the same runs, so
	there is always a row that satisfies both constraints. The first row has
	no forbidden values. The harness asserts feasibility on every row.

	The negative control is a greedy weaver: follow the preferred value until
	a float would run too long or a crossing is forbidden, and flip there.
*/
namespace jacquard::encoder
{

constexpr int64_t kInfinite = INT64_MAX / 4;

/// One pick's costs.
struct RowTable
{
	int n = 0;       ///< crossings
	int colours = 0; ///< shuttles, K
	int maxFloat = 0;///< M; <= 0 means unlimited (the negative control)
	/// cost[ ( c * n + i ) * 2 + x ]
	std::vector< int64_t > cost;
	/// Per crossing: -1, or the value (0 or 1) this crossing may NOT take.
	std::vector< int8_t > forbidden;

	void Resize( int crossings, int shuttles, int limit );
	int64_t& At( int c, int i, int x )
	{
		return cost[ ( static_cast< size_t >( c ) * n + i ) * 2 + x ];
	}
	int64_t At( int c, int i, int x ) const
	{
		return cost[ ( static_cast< size_t >( c ) * n + i ) * 2 + x ];
	}
};

struct RowResult
{
	int colour = 0;
	std::vector< uint8_t > x;///< 1 = warp on top
	int64_t cost      = kInfinite;
	bool feasible     = false;
	int programmesRun = 0;///< how many colours the search had to try
};

/// The exact programme (or, with `greedy`, the negative control).
/// `preferred` is only read by the greedy weaver: per colour and crossing,
/// the value it follows until it must not ( preferred[ c * n + i ] ).
RowResult SolveRow( const RowTable& table, bool greedy = false, const uint8_t* preferred = nullptr );

/// The cost of a given row, and whether it satisfies both constraints.
/// Plain loops, sharing nothing with the programme; the harness has its own.
int64_t RowCost( const RowTable& table, int colour, const uint8_t* x, bool& valid );

} // namespace jacquard::encoder
