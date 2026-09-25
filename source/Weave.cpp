#include "Weave.h"

#include <algorithm>
#include <numeric>

namespace jacquard::weave
{
namespace
{
/// Warp over a, under b, for the twills; 0 for anything that is not one.
void twillShape( int structure, int& over, int& repeat )
{
	over   = 0;
	repeat = 0;
	switch( structure )
	{
	case kTwill31: over = 3; repeat = 4; break;
	case kTwill21: over = 2; repeat = 3; break;
	case kTwill22: over = 2; repeat = 4; break;
	case kTwill12: over = 1; repeat = 3; break;
	case kTwill13: over = 1; repeat = 4; break;
	default: break;
	}
}

unsigned umod( int value, int period )
{
	//Every caller passes non-negative grid positions; the cast is the
	//promise, and GLSL's % would be undefined on a negative operand anyway.
	return static_cast< unsigned >( value ) % static_cast< unsigned >( period );
}
} // namespace

const char* ModeName( int mode )
{
	static const char* const names[ kModeCount ] = { "Auto by Tone", "Plain", "Twill", "Satin" };
	return names[ std::clamp( mode, 0, kModeCount - 1 ) ];
}

const char* StructureName( int structure )
{
	static const char* const names[ kStructureCount ] = {
		"warp solid", "warp satin", "3/1 twill", "2/1 twill", "plain",
		"2/2 twill",  "1/2 twill",  "1/3 twill", "weft satin", "weft solid",
	};
	return names[ std::clamp( structure, 0, kStructureCount - 1 ) ];
}

int Repeat( int structure )
{
	switch( structure )
	{
	case kWarpSolid:
	case kWeftSolid: return 1;
	case kWarpSatin:
	case kWeftSatin: return 5;
	case kPlain: return 2;
	default:
	{
		int over, repeat;
		twillShape( structure, over, repeat );
		return repeat;
	}
	}
}

void Coverage( int structure, int& num, int& den )
{
	switch( structure )
	{
	case kWarpSolid: num = 0; den = 1; return;
	case kWeftSolid: num = 1; den = 1; return;
	case kWarpSatin: num = 1; den = 5; return;
	case kWeftSatin: num = 4; den = 5; return;
	case kPlain: num = 1; den = 2; return;
	default:
	{
		int over, repeat;
		twillShape( structure, over, repeat );
		num = repeat - over;
		den = repeat;
		return;
	}
	}
}

double Coverage( int structure )
{
	int num, den;
	Coverage( structure, num, den );
	return static_cast< double >( num ) / static_cast< double >( den );
}

int Family( int mode, int* out )
{
	int n = 0;
	switch( mode )
	{
	case kModePlain: out[ n++ ] = kPlain; break;
	case kModeTwill:
		out[ n++ ] = kTwill31;
		out[ n++ ] = kTwill22;
		out[ n++ ] = kTwill13;
		break;
	case kModeSatin:
		out[ n++ ] = kWarpSatin;
		out[ n++ ] = kWeftSatin;
		break;
	default:
		//Auto: the solids at the ends, the satins next (the sheen in the
		//highlights and the shadows), the three-end and four-end twills in
		//the midtones. Plain and the 3/1 / 1/3 twills are left out: plain
		//ties 50 % like 2/2 does and has no diagonal, and 0.25 / 0.75 sit
		//too close to the satins' 0.2 / 0.8 for a tone to choose between
		//them steadily.
		out[ n++ ] = kWarpSolid;
		out[ n++ ] = kWarpSatin;
		out[ n++ ] = kTwill21;
		out[ n++ ] = kTwill22;
		out[ n++ ] = kTwill12;
		out[ n++ ] = kWeftSatin;
		out[ n++ ] = kWeftSolid;
		break;
	}
	return n;
}

bool WarpUp( int structure, int i, int j, int perturb )
{
	switch( structure )
	{
	case kWarpSolid: return true;
	case kWeftSolid: return false;
	case kWarpSatin: return umod( i + 2 * j, 5 ) != 0;
	case kWeftSatin: return umod( i + 2 * j, 5 ) == 0;
	case kPlain: return umod( i + j, 2 ) == 0;
	default:
	{
		int over, repeat;
		twillShape( structure, over, repeat );
		const int step = ( perturb & kPerturbTwillStep0 ) ? 0 : 1;
		if( perturb & kPerturbCoverage )
			++over;
		return static_cast< int >( umod( i + step * j, repeat ) ) < over;
	}
	}
}

int LatticeStep( int period, int perturb )
{
	if( perturb & kPerturbTies010 )
	{
		for( int s = 2; s <= period - 2; ++s )
			if( std::gcd( s, period ) == 1 )
				return s;
		return 1;
	}
	//A weaver's satin counter: of the steps coprime with the period, the one
	//whose ties lie farthest apart -- the longest shortest vector of the
	//lattice { ( i, j ) : i + s j = 0 mod period }, in crossings -- so no
	//diagonal is shorter than it must be. The smallest such step on a draw.
	int best = 1, bestLength = 0;
	for( int s = 2; s <= period - 2; ++s )
	{
		if( std::gcd( s, period ) != 1 )
			continue;
		int shortest = period * period;
		for( int b = 1; b < period; ++b )
		{
			const int a  = ( period - ( s * b ) % period ) % period;//-s b mod period; umod wants non-negative
			const int da = std::min( a, period - a );
			shortest     = std::min( shortest, da * da + b * b );
		}
		if( shortest > bestLength )
		{
			bestLength = shortest;
			best       = s;
		}
	}
	return best;
}

bool OnTieLattice( int i, int j, int maxFloat )
{
	const int period = maxFloat + 1;
	return umod( i + LatticeStep( period ) * j, period ) == 0;
}

uint32_t Hash( uint32_t a, uint32_t b, uint32_t c )
{
	uint32_t state = a * 747796405u + 2891336453u;
	state ^= b * 2654435761u;
	state = state * 747796405u + 2891336453u;
	state ^= c * 2246822519u;
	state = state * 747796405u + 2891336453u;
	const uint32_t word = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

} // namespace jacquard::weave
