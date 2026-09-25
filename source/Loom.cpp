#include "Loom.h"

#include "Encoder.h"
#include "Weave.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace jacquard::loom
{
namespace
{
/// The sRGB encoding by table: a pow per channel per crossing per frame is
/// most of the CPU budget at the largest grid. 4096 intervals on [0, 1],
/// linearly interpolated, within 1e-4 of the curve everywhere (the steepest
/// part, below 0.0031, is the linear segment).
class Encoding
{
public:
	Encoding()
	{
		for( int k = 0; k <= kSize; ++k )
			table[ static_cast< size_t >( k ) ] = palette::ToEncoded( static_cast< float >( k ) / kSize );
	}

	float operator()( float v ) const
	{
		v             = std::clamp( v, 0.0f, 1.0f ) * kSize;
		const int k   = std::min( static_cast< int >( v ), kSize - 1 );
		const float f = v - static_cast< float >( k );
		return table[ static_cast< size_t >( k ) ] + ( table[ static_cast< size_t >( k ) + 1 ] - table[ static_cast< size_t >( k ) ] ) * f;
	}

private:
	static constexpr int kSize = 4096;
	std::array< float, kSize + 1 > table{};
};

const Encoding& encoding()
{
	static const Encoding e;
	return e;
}

/// A squared error (never negative) to cost units, rounded half up.
int64_t quantise( float squaredError )
{
	return static_cast< int64_t >( squaredError * static_cast< float >( kCostScale ) + 0.5f );
}

/// Eight lanes: the family (at most seven structures) padded, so the
/// nearest-mixture search is one fixed-length loop the compiler vectorises.
constexpr int kLanes = 8;

float error2( const palette::Colour& a, const palette::Colour& b )
{
	const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
	return dr * dr + dg * dg + db * db;
}
} // namespace

void Weave( const float* means, const Settings& s, Cloth& out, const Cloth* previous )
{
	const int n = s.ends;
	const int K = std::clamp( s.shuttleCount, 1, palette::kMaxShuttles );
	const bool unlimited = ( s.perturb & weave::kPerturbNoFloatLimit ) != 0 || s.maxFloat <= 0;
	const int M          = unlimited ? 0 : s.maxFloat;
	const bool greedy    = ( s.perturb & weave::kPerturbGreedy ) != 0;

	out.ends  = n;
	out.picks = s.picks;
	out.lift.assign( static_cast< size_t >( n ) * s.picks, 0 );
	out.weft.assign( static_cast< size_t >( s.picks ), 0 );
	out.structure.assign( static_cast< size_t >( n ) * s.picks, 0 );
	out.remembered      = false;
	out.shuttleCount    = K;
	out.mode            = s.mode;
	out.forcedStructure = s.forcedStructure;
	out.cost            = 0;
	out.infeasiblePicks = 0;
	out.programmes      = 0;
	if( n <= 0 || s.picks <= 0 || s.shuttles == nullptr )
		return;

	const Encoding& enc = encoding();
	const bool remember = previous != nullptr && previous != &out && previous->ends == n && previous->picks == s.picks
	                      && previous->shuttleCount == K && previous->mode == s.mode && previous->forcedStructure == s.forcedStructure
	                      && previous->lift.size() == out.lift.size();
	out.remembered = remember;

	//-----------------------------------------------------------------
	// Per frame: the family, and every (structure, shuttle) mixture in
	// linear light, encoded, and its encoded luminance.
	//-----------------------------------------------------------------
	int family[ weave::kStructureCount ];
	int levels = weave::Family( s.mode, family );
	if( s.forcedStructure >= 0 && s.forcedStructure < weave::kStructureCount )
	{
		family[ 0 ] = s.forcedStructure;
		levels      = 1;
	}

	const palette::Colour& W = s.warp;
	std::vector< palette::Colour > mixEncoded( static_cast< size_t >( levels ) * K );
	for( int l = 0; l < levels; ++l )
	{
		const float f = static_cast< float >( weave::Coverage( family[ l ] ) );
		for( int c = 0; c < K; ++c )
		{
			const palette::Colour& weft = s.shuttles[ c ];
			palette::Colour m{ f * weft.r + ( 1.0f - f ) * W.r, f * weft.g + ( 1.0f - f ) * W.g, f * weft.b + ( 1.0f - f ) * W.b };
			const size_t at   = static_cast< size_t >( l ) * K + c;
			mixEncoded[ at ]  = palette::Colour{ palette::ToEncoded( m.r ), palette::ToEncoded( m.g ), palette::ToEncoded( m.b ) };
		}
	}

	const palette::Colour warpEncoded{ palette::ToEncoded( W.r ), palette::ToEncoded( W.g ), palette::ToEncoded( W.b ) };
	std::vector< palette::Colour > weftEncoded( static_cast< size_t >( K ) );
	for( int c = 0; c < K; ++c )
		weftEncoded[ static_cast< size_t >( c ) ] = palette::Colour{ palette::ToEncoded( s.shuttles[ c ].r ), palette::ToEncoded( s.shuttles[ c ].g ),
			                                                         palette::ToEncoded( s.shuttles[ c ].b ) };

	//The range the threads span, per channel, on the encoding.
	palette::Colour lo = warpEncoded, hi = warpEncoded;
	for( const palette::Colour& w : weftEncoded )
	{
		lo = palette::Colour{ std::min( lo.r, w.r ), std::min( lo.g, w.g ), std::min( lo.b, w.b ) };
		hi = palette::Colour{ std::max( hi.r, w.r ), std::max( hi.g, w.g ), std::max( hi.b, w.b ) };
	}

	//-----------------------------------------------------------------
	// The picks, top to bottom.
	//-----------------------------------------------------------------
	encoder::RowTable table;
	table.Resize( n, K, M );
	std::vector< uint8_t > preferred( static_cast< size_t >( K ) * n );
	std::vector< uint8_t > chosen( static_cast< size_t >( K ) * n );
	std::vector< palette::Colour > carry( static_cast< size_t >( n ) ), residual( static_cast< size_t >( n ) );
	std::vector< palette::Colour > targetEncoded( static_cast< size_t >( n ) );
	std::vector< uint8_t > lattice( static_cast< size_t >( n ), 0 );
	std::vector< uint8_t > pattern( static_cast< size_t >( levels ) * n, 0 );
	//Each structure's repeat, tiled once: WarpUp at every crossing of every
	//pick was a tenth of the frame in divisions. The perturbation that stops
	//a twill stepping still goes through WarpUp, here.
	constexpr int kMaxRepeat = 5;
	std::vector< int > repeat( static_cast< size_t >( levels ) );
	std::vector< uint8_t > tiles( static_cast< size_t >( levels ) * kMaxRepeat * kMaxRepeat, 0 );
	for( int l = 0; l < levels; ++l )
	{
		const int R                          = weave::Repeat( family[ l ] );
		repeat[ static_cast< size_t >( l ) ] = R;
		for( int jj = 0; jj < R; ++jj )
			for( int ii = 0; ii < R; ++ii )
				tiles[ ( static_cast< size_t >( l ) * kMaxRepeat + jj ) * kMaxRepeat + ii ] = weave::WarpUp( family[ l ], ii, jj, s.perturb ) ? 1 : 0;
	}
	//The mixtures again, as three eight-lane arrays a shuttle, padded far away.
	std::vector< float > lanes( static_cast< size_t >( K ) * 3 * kLanes, 1.0e6f );
	for( int c = 0; c < K; ++c )
		for( int l = 0; l < levels; ++l )
		{
			const palette::Colour& m = mixEncoded[ static_cast< size_t >( l ) * K + c ];
			lanes[ ( static_cast< size_t >( c ) * 3 + 0 ) * kLanes + l ] = m.r;
			lanes[ ( static_cast< size_t >( c ) * 3 + 1 ) * kLanes + l ] = m.g;
			lanes[ ( static_cast< size_t >( c ) * 3 + 2 ) * kLanes + l ] = m.b;
		}
	const int latticeStep = M > 0 ? weave::LatticeStep( M + 1 ) : 1;
	std::vector< uint8_t > columnValue( static_cast< size_t >( n ), 0 );
	std::vector< int > columnRun( static_cast< size_t >( n ), 0 );

	for( int j = 0; j < s.picks; ++j )
	{
		for( int i = 0; i < n; ++i )
		{
			const float* t           = means + ( static_cast< size_t >( j ) * n + i ) * 4;
			const palette::Colour& k = carry[ static_cast< size_t >( i ) ];
			targetEncoded[ static_cast< size_t >( i ) ] =
				palette::Colour{ enc( t[ 0 ] ) + kCarry * k.r, enc( t[ 1 ] ) + kCarry * k.g, enc( t[ 2 ] ) + kCarry * k.b };
		}
		//The tie lattice along this pick: ( i + s j ) mod ( M + 1 ) == 0.
		for( int i = 0; i < n; ++i )
			lattice[ static_cast< size_t >( i ) ] =
				M > 0 && ( static_cast< unsigned >( i ) + static_cast< unsigned >( latticeStep ) * static_cast< unsigned >( j ) ) % static_cast< unsigned >( M + 1 ) == 0;

		//Each structure's pattern along this pick, from its tiled repeat.
		for( int l = 0; l < levels; ++l )
		{
			const int R          = repeat[ static_cast< size_t >( l ) ];
			const uint8_t* line  = tiles.data() + ( static_cast< size_t >( l ) * kMaxRepeat + static_cast< size_t >( j % R ) ) * kMaxRepeat;
			uint8_t* out         = pattern.data() + static_cast< size_t >( l ) * n;
			for( int i = 0, ii = 0; i < n; ++i, ii = ii + 1 == R ? 0 : ii + 1 )
				out[ i ] = line[ ii ];
		}

		for( int c = 0; c < K; ++c )
		{
			const float* laneR = lanes.data() + ( static_cast< size_t >( c ) * 3 + 0 ) * kLanes;
			const float* laneG = lanes.data() + ( static_cast< size_t >( c ) * 3 + 1 ) * kLanes;
			const float* laneB = lanes.data() + ( static_cast< size_t >( c ) * 3 + 2 ) * kLanes;
			for( int i = 0; i < n; ++i )
			{
				//Tone to structure: the mixture nearest the target, on the
				//encoding. On the line from warp to weft this is the nearest
				//coverage by tone; off it, a crossing whose colour this shuttle
				//cannot give falls back towards the warp instead of painting
				//the wrong hue at the right luminance. Ties go to the lower
				//coverage.
				const palette::Colour& te = targetEncoded[ static_cast< size_t >( i ) ];
				float distance[ kLanes ];
				for( int l = 0; l < kLanes; ++l )
				{
					const float dr = te.r - laneR[ l ], dg = te.g - laneG[ l ], db = te.b - laneB[ l ];
					distance[ l ] = dr * dr + dg * dg + db * db;
				}
				int best    = 0;
				float bestD = distance[ 0 ];
				for( int l = 1; l < levels; ++l )
					if( distance[ l ] < bestD )
					{
						bestD = distance[ l ];
						best  = l;
					}
				const size_t cell = static_cast< size_t >( j ) * n + i;
				int64_t E         = quantise( bestD );
				if( remember && family[ best ] != previous->structure[ cell ] )
				{
					//Keep last frame's structure while it is nearly as good.
					for( int l = 0; l < levels; ++l )
						if( family[ l ] == previous->structure[ cell ] )
						{
							const int64_t kept = quantise( distance[ l ] );
							if( kept <= E + kHoldStructure )
							{
								best = l;
								E    = kept;
							}
						}
				}
				const int d       = pattern[ static_cast< size_t >( best ) * n + i ];
				const int64_t tie = kTie + quantise( error2( te, d == 1 ? weftEncoded[ static_cast< size_t >( c ) ] : warpEncoded ) )
				                    - ( lattice[ static_cast< size_t >( i ) ] ? kLattice : 0 );
				const int64_t shuttleHold = remember && previous->weft[ static_cast< size_t >( j ) ] != c ? kHoldShuttle : 0;
				table.At( c, i, d )     = E + shuttleHold;
				table.At( c, i, 1 - d ) = E + tie + shuttleHold;
				if( remember )
					table.At( c, i, 1 - previous->lift[ cell ] ) += kHoldLift;
				preferred[ static_cast< size_t >( c ) * n + i ] = static_cast< uint8_t >( d );
				chosen[ static_cast< size_t >( c ) * n + i ]    = static_cast< uint8_t >( best );
			}
		}

		for( int i = 0; i < n; ++i )
			table.forbidden[ static_cast< size_t >( i ) ] =
				( M > 0 && columnRun[ static_cast< size_t >( i ) ] >= M ) ? static_cast< int8_t >( columnValue[ static_cast< size_t >( i ) ] ) : -1;

		const encoder::RowResult row = encoder::SolveRow( table, greedy, preferred.data() );
		if( s.observe )
			s.observe( j, table, row );
		out.cost += row.cost;
		out.programmes += row.programmesRun;
		if( !row.feasible )
			++out.infeasiblePicks;

		const int c = row.colour;
		out.weft[ static_cast< size_t >( j ) ] = static_cast< uint8_t >( c );
		for( int i = 0; i < n; ++i )
		{
			const uint8_t x    = row.x.empty() ? 0 : row.x[ static_cast< size_t >( i ) ];
			const size_t cell  = static_cast< size_t >( j ) * n + i;
			const int level    = chosen[ static_cast< size_t >( c ) * n + i ];
			out.lift[ cell ]      = x;
			out.structure[ cell ] = static_cast< uint8_t >( family[ level ] );

			if( columnRun[ static_cast< size_t >( i ) ] > 0 && columnValue[ static_cast< size_t >( i ) ] == x )
				++columnRun[ static_cast< size_t >( i ) ];
			else
			{
				columnValue[ static_cast< size_t >( i ) ] = x;
				columnRun[ static_cast< size_t >( i ) ]   = 1;
			}

			//What the crossing still owes, on the encoding: only what some
			//thread could repay (the target clamped to the threads' own range,
			//per channel), and bounded. A black warp is lighter than a black
			//picture; without the clamp every crossing of a black ground owes
			//that difference, pick after pick, until the whole width is forced
			//into a dark shuttle at once.
			const palette::Colour& m  = mixEncoded[ static_cast< size_t >( level ) * K + c ];
			const palette::Colour& te = targetEncoded[ static_cast< size_t >( i ) ];
			residual[ static_cast< size_t >( i ) ] =
				palette::Colour{ std::clamp( std::clamp( te.r, lo.r, hi.r ) - m.r, -kCarryLimit, kCarryLimit ),
				                 std::clamp( std::clamp( te.g, lo.g, hi.g ) - m.g, -kCarryLimit, kCarryLimit ),
				                 std::clamp( std::clamp( te.b, lo.b, hi.b ) - m.b, -kCarryLimit, kCarryLimit ) };
		}

		//Spread 1/4, 1/2, 1/4 to the next pick, renormalised at the selvedges.
		for( int i = 0; i < n; ++i )
		{
			float w = 0.5f, r = 0.5f * residual[ static_cast< size_t >( i ) ].r, g = 0.5f * residual[ static_cast< size_t >( i ) ].g,
			      b = 0.5f * residual[ static_cast< size_t >( i ) ].b;
			for( int side : { i - 1, i + 1 } )
				if( side >= 0 && side < n )
				{
					w += 0.25f;
					r += 0.25f * residual[ static_cast< size_t >( side ) ].r;
					g += 0.25f * residual[ static_cast< size_t >( side ) ].g;
					b += 0.25f * residual[ static_cast< size_t >( side ) ].b;
				}
			carry[ static_cast< size_t >( i ) ] = palette::Colour{ r / w, g / w, b / w };
		}

		if( ( s.perturb & weave::kPerturbScramble ) != 0 && K > 1 )
			out.weft[ static_cast< size_t >( j ) ] =
				static_cast< uint8_t >( ( c + 1 + weave::Hash( static_cast< uint32_t >( j ), 7u, 11u ) % static_cast< uint32_t >( K - 1 ) ) % K );
	}
}

} // namespace jacquard::loom
