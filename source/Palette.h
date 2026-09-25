#pragma once

#include <vector>

/**
	The shuttles: which weft colours are loaded.

	**Clip** is k-means of the frame's cell means in LINEAR light -- the
	colour a yarn has is a reflectance, and the mean of a cluster of
	reflectances is the reflectance of their optical mix. Lloyd's algorithm,
	warm-started from the previous frame's shuttles so the cloth does not
	re-dye itself every frame; a cold start (the first frame, or a change of
	Shuttles or Palette) seeds the centres at luminance quantiles, which is
	deterministic. The state is K colours, never the size of the raster, so a
	resize cannot clear it (`jqtest --resize`).

	The fixed sets are swatch cards, in the order they are loaded: Shuttles
	= K takes the first K. They are sRGB values here, as a dyer's card would
	be read, and converted once.
*/
namespace jacquard::palette
{

struct Colour
{
	float r = 0.0f, g = 0.0f, b = 0.0f;///< linear light
};

constexpr int kMaxShuttles = 8;

enum Source
{
	kClip = 0,
	kHeritage,
	kMono,
	kBright,
	kSourceCount
};

const char* SourceName( int source );

/// sRGB transfer, both ways, on 0..1.
float ToLinear( float v );
float ToEncoded( float v );

/// A fixed swatch set's first K colours, in linear light.
void Fixed( int source, int count, Colour* out );

/// Luminance, Rec. 709, of a linear colour.
inline float Luma( const Colour& c )
{
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

class KMeans
{
public:
	/// Cluster `count` linear RGBA texels (stride 4 floats) into K colours,
	/// warm-starting from the last result when K is unchanged. `sampleStep`
	/// takes every n-th texel, which bounds the cost on the largest grids.
	void Update( const float* rgba, int count, int k, int sampleStep );

	/// Forget the centres: the next Update is a cold start.
	void Reset()
	{
		centres.clear();
	}

	const std::vector< Colour >& Centres() const
	{
		return centres;
	}

	/// For the harness: how many cold starts so far, and the centres the
	/// last Update began from.
	int ColdStarts() const
	{
		return coldStarts;
	}
	const std::vector< Colour >& LastSeed() const
	{
		return seed;
	}

private:
	std::vector< Colour > centres;
	std::vector< Colour > seed;
	std::vector< Colour > points;
	std::vector< int > label;
	int coldStarts = 0;
};

} // namespace jacquard::palette
