#include "Palette.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace jacquard::palette
{
namespace
{
struct Swatch
{
	float r, g, b;///< sRGB
};

//Natural dyes, as a weaver's card reads them: iron black, undyed cream,
//madder, indigo, weld, weld over indigo, walnut, madder rose. Our own
//values, picked by eye from what the dyes are known to give; not measured.
const Swatch kHeritageSwatches[ kMaxShuttles ] = {
	{ 0.10f, 0.09f, 0.09f }, { 0.90f, 0.85f, 0.72f }, { 0.66f, 0.16f, 0.14f }, { 0.14f, 0.22f, 0.42f },
	{ 0.86f, 0.72f, 0.26f }, { 0.28f, 0.42f, 0.24f }, { 0.42f, 0.28f, 0.18f }, { 0.84f, 0.52f, 0.52f },
};

//A woven label's brights.
const Swatch kBrightSwatches[ kMaxShuttles ] = {
	{ 0.05f, 0.05f, 0.06f }, { 0.96f, 0.96f, 0.94f }, { 0.86f, 0.10f, 0.12f }, { 0.08f, 0.22f, 0.78f },
	{ 0.98f, 0.82f, 0.08f }, { 0.08f, 0.62f, 0.26f }, { 0.82f, 0.16f, 0.60f }, { 0.10f, 0.70f, 0.82f },
};

Colour fromSwatch( const Swatch& s )
{
	return Colour{ ToLinear( s.r ), ToLinear( s.g ), ToLinear( s.b ) };
}

float distance2( const Colour& a, const Colour& b )
{
	const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
	return dr * dr + dg * dg + db * db;
}
} // namespace

const char* SourceName( int source )
{
	static const char* const names[ kSourceCount ] = { "Clip", "Heritage Dyes", "Mono", "Bright" };
	return names[ std::clamp( source, 0, kSourceCount - 1 ) ];
}

float ToLinear( float v )
{
	v = std::clamp( v, 0.0f, 1.0f );
	return v <= 0.04045f ? v / 12.92f : std::pow( ( v + 0.055f ) / 1.055f, 2.4f );
}

float ToEncoded( float v )
{
	v = std::clamp( v, 0.0f, 1.0f );
	return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow( v, 1.0f / 2.4f ) - 0.055f;
}

void Fixed( int source, int count, Colour* out )
{
	count = std::clamp( count, 1, kMaxShuttles );
	for( int k = 0; k < count; ++k )
	{
		switch( source )
		{
		case kMono:
		{
			//Evenly spaced greys in the encoding, black to white.
			const float v = count > 1 ? static_cast< float >( k ) / static_cast< float >( count - 1 ) : 0.5f;
			const float l = ToLinear( v );
			out[ k ]      = Colour{ l, l, l };
			break;
		}
		case kBright: out[ k ] = fromSwatch( kBrightSwatches[ k ] ); break;
		default: out[ k ] = fromSwatch( kHeritageSwatches[ k ] ); break;
		}
	}
}

void KMeans::Update( const float* rgba, int count, int k, int sampleStep )
{
	k          = std::clamp( k, 1, kMaxShuttles );
	sampleStep = std::max( 1, sampleStep );

	points.clear();
	for( int i = 0; i < count; i += sampleStep )
	{
		const float* t = rgba + static_cast< size_t >( i ) * 4;
		points.push_back( Colour{ t[ 0 ], t[ 1 ], t[ 2 ] } );
	}
	if( points.empty() )
	{
		centres.assign( static_cast< size_t >( k ), Colour{ 0.2f, 0.2f, 0.2f } );
		seed = centres;
		return;
	}

	int iterations = 3;
	if( static_cast< int >( centres.size() ) != k )
	{
		//Cold start, by farthest point: the first centre is the median by
		//luminance, and each next one the colour farthest from every centre
		//so far. Deterministic, and unlike quantile or random seeds it finds
		//the small saturated patches -- a red coat, a green light -- that a
		//woven picture most needs a shuttle for and that Lloyd's iterations,
		//which weigh by area, would never go looking for.
		++coldStarts;
		iterations = 6;
		std::vector< float > luma( points.size() );
		std::vector< int > byLuma( points.size() );
		for( size_t i = 0; i < points.size(); ++i )
			luma[ i ] = Luma( points[ i ] );
		std::iota( byLuma.begin(), byLuma.end(), 0 );
		std::stable_sort( byLuma.begin(), byLuma.end(), [ & ]( int a, int b ) { return luma[ static_cast< size_t >( a ) ] < luma[ static_cast< size_t >( b ) ]; } );
		centres.assign( 1, points[ static_cast< size_t >( byLuma[ byLuma.size() / 2 ] ) ] );
		std::vector< float > nearest( points.size() );
		for( size_t i = 0; i < points.size(); ++i )
			nearest[ i ] = distance2( points[ i ], centres[ 0 ] );
		while( static_cast< int >( centres.size() ) < k )
		{
			size_t farthest = 0;
			for( size_t i = 1; i < points.size(); ++i )
				if( nearest[ i ] > nearest[ farthest ] )
					farthest = i;
			centres.push_back( points[ farthest ] );
			for( size_t i = 0; i < points.size(); ++i )
				nearest[ i ] = std::min( nearest[ i ], distance2( points[ i ], centres.back() ) );
		}
	}
	seed = centres;

	label.assign( points.size(), 0 );
	std::vector< double > sum( static_cast< size_t >( k ) * 3 );
	std::vector< int > members( static_cast< size_t >( k ) );
	for( int iteration = 0; iteration < iterations; ++iteration )
	{
		std::fill( sum.begin(), sum.end(), 0.0 );
		std::fill( members.begin(), members.end(), 0 );
		float worst   = -1.0f;
		size_t worstI = 0;
		for( size_t i = 0; i < points.size(); ++i )
		{
			int best    = 0;
			float bestD = distance2( points[ i ], centres[ 0 ] );
			for( int c = 1; c < k; ++c )
			{
				const float d = distance2( points[ i ], centres[ static_cast< size_t >( c ) ] );
				if( d < bestD )
				{
					bestD = d;
					best  = c;
				}
			}
			label[ i ] = best;
			if( bestD > worst )
			{
				worst  = bestD;
				worstI = i;
			}
			sum[ static_cast< size_t >( best ) * 3 + 0 ] += points[ i ].r;
			sum[ static_cast< size_t >( best ) * 3 + 1 ] += points[ i ].g;
			sum[ static_cast< size_t >( best ) * 3 + 2 ] += points[ i ].b;
			++members[ static_cast< size_t >( best ) ];
		}
		for( int c = 0; c < k; ++c )
		{
			Colour& centre = centres[ static_cast< size_t >( c ) ];
			const int m    = members[ static_cast< size_t >( c ) ];
			if( m == 0 )
			{
				//An empty shuttle takes the worst-served colour in the frame.
				centre = points[ worstI ];
				continue;
			}
			centre.r = static_cast< float >( sum[ static_cast< size_t >( c ) * 3 + 0 ] / m );
			centre.g = static_cast< float >( sum[ static_cast< size_t >( c ) * 3 + 1 ] / m );
			centre.b = static_cast< float >( sum[ static_cast< size_t >( c ) * 3 + 2 ] / m );
		}
	}
}

} // namespace jacquard::palette
