#include "Encoder.h"

#include <algorithm>
#include <numeric>

namespace jacquard::encoder
{
namespace
{
/// The longest run the programme tracks. Unlimited (the negative control)
/// is a run as long as the row, which no row can exceed.
int runCap( const RowTable& table )
{
	return table.maxFloat > 0 ? std::min( table.maxFloat, table.n ) : std::max( table.n, 1 );
}

bool allowed( const RowTable& table, int i, int x )
{
	return table.forbidden[ static_cast< size_t >( i ) ] != x;
}

/// The programme for one colour. Returns the cost and fills x.
///
/// State (v, r): value v on top, in a run of r crossings ending here,
/// 1 <= r <= M, stored at v * M + r - 1. Two moves into crossing i: extend
/// the run of v (r < M), or start a run of the other value at 1. The
/// extension's predecessor is implicit, so the only pointer kept is, for
/// each crossing and value, the length of the run it switched from.
int64_t programme( const RowTable& table, int c, std::vector< uint8_t >& x )
{
	const int n = table.n;
	const int M = runCap( table );

	thread_local std::vector< int64_t > cur, nxt;
	thread_local std::vector< uint16_t > from;
	cur.assign( static_cast< size_t >( 2 * M ), kInfinite );
	nxt.assign( static_cast< size_t >( 2 * M ), kInfinite );
	from.assign( static_cast< size_t >( n ) * 2, 0 );

	for( int v = 0; v < 2; ++v )
		if( allowed( table, 0, v ) )
			cur[ static_cast< size_t >( v * M ) ] = table.At( c, 0, v );

	for( int i = 1; i < n; ++i )
	{
		//The cheapest run of each value ending at i - 1, and its length.
		int64_t best[ 2 ] = { kInfinite, kInfinite };
		int bestR[ 2 ]    = { 0, 0 };
		for( int v = 0; v < 2; ++v )
			for( int r = 0; r < M; ++r )
			{
				const int64_t value = cur[ static_cast< size_t >( v * M + r ) ];
				if( value < best[ v ] )
				{
					best[ v ]  = value;
					bestR[ v ] = r + 1;
				}
			}

		std::fill( nxt.begin(), nxt.end(), kInfinite );
		for( int u = 0; u < 2; ++u )
		{
			if( !allowed( table, i, u ) )
				continue;
			const int64_t here = table.At( c, i, u );
			//Start a run of u after a run of the other value.
			if( best[ 1 - u ] < kInfinite )
			{
				nxt[ static_cast< size_t >( u * M ) ]             = best[ 1 - u ] + here;
				from[ static_cast< size_t >( i ) * 2 + u ] = static_cast< uint16_t >( bestR[ 1 - u ] );
			}
			//Extend a run of u that is still shorter than M.
			for( int r = 1; r < M; ++r )
			{
				const int64_t previous = cur[ static_cast< size_t >( u * M + r - 1 ) ];
				if( previous < kInfinite )
					nxt[ static_cast< size_t >( u * M + r ) ] = previous + here;
			}
		}
		std::swap( cur, nxt );
	}

	int64_t total = kInfinite;
	int endV = 0, endR = 0;
	for( int v = 0; v < 2; ++v )
		for( int r = 0; r < M; ++r )
			if( cur[ static_cast< size_t >( v * M + r ) ] < total )
			{
				total = cur[ static_cast< size_t >( v * M + r ) ];
				endV  = v;
				endR  = r + 1;
			}
	if( total >= kInfinite )
		return kInfinite;

	//Walk back run by run.
	x.assign( static_cast< size_t >( n ), 0 );
	int i = n - 1, v = endV, r = endR;
	while( i >= 0 )
	{
		const int start = i - r + 1;
		for( int k = start; k <= i; ++k )
			x[ static_cast< size_t >( k ) ] = static_cast< uint8_t >( v );
		if( start == 0 )
			break;
		const int previousR = from[ static_cast< size_t >( start ) * 2 + v ];
		i                   = start - 1;
		v                   = 1 - v;
		r                   = previousR;
	}
	return total;
}

/// The negative control: follow the preferred value, flip only when the
/// float limit or a forbidden crossing says so. No look-ahead.
int64_t greedyRow( const RowTable& table, int c, const uint8_t* preferred, std::vector< uint8_t >& x, bool& feasible )
{
	const int n = table.n;
	const int M = runCap( table );
	x.assign( static_cast< size_t >( n ), 0 );
	feasible      = true;
	int64_t total = 0;
	int runValue = -1, runLength = 0;
	for( int i = 0; i < n; ++i )
	{
		int want = preferred ? preferred[ static_cast< size_t >( c ) * n + i ] : 0;
		if( !allowed( table, i, want ) )
			want = 1 - want;
		if( want == runValue && runLength >= M )
			want = 1 - want;
		if( !allowed( table, i, want ) || ( want == runValue && runLength >= M ) )
			feasible = false;
		x[ static_cast< size_t >( i ) ] = static_cast< uint8_t >( want );
		total += table.At( c, i, want );
		runLength = want == runValue ? runLength + 1 : 1;
		runValue  = want;
	}
	return total;
}
} // namespace

void RowTable::Resize( int crossings, int shuttles, int limit )
{
	n        = crossings;
	colours  = shuttles;
	maxFloat = limit;
	cost.assign( static_cast< size_t >( shuttles ) * crossings * 2, 0 );
	forbidden.assign( static_cast< size_t >( crossings ), -1 );
}

RowResult SolveRow( const RowTable& table, bool greedy, const uint8_t* preferred )
{
	RowResult result;
	if( table.n <= 0 || table.colours <= 0 )
		return result;

	if( greedy )
	{
		std::vector< uint8_t > x;
		for( int c = 0; c < table.colours; ++c )
		{
			bool feasible        = false;
			const int64_t cost   = greedyRow( table, c, preferred, x, feasible );
			const bool better    = ( feasible && !result.feasible ) || ( feasible == result.feasible && cost < result.cost );
			if( better )
			{
				result.colour   = c;
				result.x        = x;
				result.cost     = cost;
				result.feasible = feasible;
			}
			++result.programmesRun;
		}
		return result;
	}

	//A lower bound per colour: each crossing's cheaper permitted value, with
	//the float limit ignored. The constrained optimum can only cost more.
	thread_local std::vector< int64_t > bound;
	thread_local std::vector< int > order;
	bound.assign( static_cast< size_t >( table.colours ), 0 );
	for( int c = 0; c < table.colours; ++c )
	{
		int64_t sum = 0;
		for( int i = 0; i < table.n; ++i )
		{
			int64_t cheapest = kInfinite;
			for( int v = 0; v < 2; ++v )
				if( allowed( table, i, v ) )
					cheapest = std::min( cheapest, table.At( c, i, v ) );
			sum += cheapest;
		}
		bound[ static_cast< size_t >( c ) ] = sum;
	}
	order.resize( static_cast< size_t >( table.colours ) );
	std::iota( order.begin(), order.end(), 0 );
	std::stable_sort( order.begin(), order.end(),
	                  [ & ]( int a, int b ) { return bound[ static_cast< size_t >( a ) ] < bound[ static_cast< size_t >( b ) ]; } );

	std::vector< uint8_t > x;
	for( int c : order )
	{
		if( bound[ static_cast< size_t >( c ) ] >= result.cost )
			break;//no remaining colour can beat the row found
		const int64_t cost = programme( table, c, x );
		++result.programmesRun;
		if( cost < result.cost )
		{
			result.colour   = c;
			result.x        = x;
			result.cost     = cost;
			result.feasible = true;
		}
	}
	return result;
}

int64_t RowCost( const RowTable& table, int colour, const uint8_t* x, bool& valid )
{
	valid         = true;
	int64_t total = 0;
	int run       = 0;
	for( int i = 0; i < table.n; ++i )
	{
		if( !allowed( table, i, x[ i ] ) )
			valid = false;
		run = ( i > 0 && x[ i ] == x[ i - 1 ] ) ? run + 1 : 1;
		if( table.maxFloat > 0 && run > table.maxFloat )
			valid = false;
		total += table.At( colour, i, x[ i ] );
	}
	return total;
}

} // namespace jacquard::encoder
