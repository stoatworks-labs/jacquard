#include "Shaders.h"

namespace jacquard::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader both passes share.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: cells. One fragment per crossing; texture row 0 is the TOP pick,
// so the CPU reads the cloth in weaving order.
//---------------------------------------------------------------------------
const char* const kCellsBody = R"(
uniform sampler2D InputTexture;
uniform ivec2 InputSize;   //the picture in texels (not the hardware size)
uniform ivec2 Grid;        //ends, picks

in vec2 uv;
out vec4 fragColor;

float toLinear( float v )
{
	return v <= 0.04045 ? v / 12.92 : pow( ( v + 0.055 ) / 1.055, 2.4 );
}

void main()
{
	ivec2 crossing = ivec2( floor( gl_FragCoord.xy ) );   //end, pick (top-down)
	vec2 span = vec2( InputSize ) / vec2( Grid );

	//The crossing's rectangle, y down, and the pixels whose centres lie in
	//it: p + 0.5 in [ a, b ) is p in [ ceil( a - 0.5 ), ceil( b - 0.5 ) ).
	vec2 a = vec2( crossing ) * span;
	vec2 b = vec2( crossing + 1 ) * span;
	ivec2 p0 = clamp( ivec2( ceil( a - 0.5 ) ), ivec2( 0 ), InputSize );
	ivec2 p1 = clamp( ivec2( ceil( b - 0.5 ) ), ivec2( 0 ), InputSize );
	p1 = max( p1, min( p0 + 1, InputSize ) );   //at least one tap

	//Past 16 a side the taps are strided.
	int sx = max( 1, ( p1.x - p0.x + 15 ) / 16 );
	int sy = max( 1, ( p1.y - p0.y + 15 ) / 16 );

	vec3 sum = vec3( 0.0 );
	float n  = 0.0;
	for( int y = p0.y; y < p1.y; y += sy )
	{
		int glY = InputSize.y - 1 - y;   //the texture's row 0 is the bottom
		for( int x = p0.x; x < p1.x; x += sx )
		{
			vec4 t = texelFetch( InputTexture, ivec2( x, glY ), 0 );
			sum += vec3( toLinear( t.r ), toLinear( t.g ), toLinear( t.b ) );
			n += 1.0;
		}
	}
	fragColor = vec4( sum / max( n, 1.0 ), 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 2: render. The lift texture is Ends x Picks RG8, row 0 the top pick:
// R 255 where the warp is on top, G the pick's shuttle index.
//---------------------------------------------------------------------------
const char* const kRenderBody = R"(
uniform sampler2D Lift;
uniform sampler2D Source;
uniform vec2 MaxUV;
uniform ivec2 OutSize;
uniform ivec2 Grid;          //ends, picks
uniform vec3 Warp;           //linear
uniform vec3 Shuttle[ 8 ];   //linear
uniform int Shuttles;
uniform float Zoom;
uniform float Shading;
uniform float MixAmount;
uniform int Perturb;

in vec2 uv;
out vec4 fragColor;

//The light, from the upper left in screen space (y down), and the
//thread's lit profile scaled so a flat crossing of warp averages 1.
const vec3 kLight   = vec3( -0.45, -0.55, 0.70 );
const float kWidth  = 0.90;   //a thread's width, of its spacing
const float kDip    = 0.32;   //how far along a float its end dives under
const float kTwist  = 3.0;    //ply twists along one crossing
const float kGain   = 1.18;   //1 / the profile's mean, on a flat crossing

float encode( float v )
{
	v = clamp( v, 0.0, 1.0 );
	return v <= 0.0031308 ? 12.92 * v : 1.055 * pow( v, 1.0 / 2.4 ) - 0.055;
}

bool warpAt( ivec2 c )
{
	c = clamp( c, ivec2( 0 ), Grid - 1 );
	return texelFetch( Lift, c, 0 ).r > 0.5;
}

int weftOf( ivec2 c )
{
	return int( texelFetch( Lift, c, 0 ).g * 255.0 + 0.5 );
}

void main()
{
	vec4 source = texture( Source, uv * MaxUV );

	//This pixel's centre, top-down, through the zoom about the frame centre.
	vec2 p      = vec2( floor( gl_FragCoord.x ), float( OutSize.y ) - 1.0 - floor( gl_FragCoord.y ) ) + 0.5;
	vec2 centre = vec2( OutSize ) * 0.5;
	vec2 q      = centre + ( p - centre ) / Zoom;
	vec2 g      = q * vec2( Grid ) / vec2( OutSize );
	ivec2 cell  = clamp( ivec2( floor( g ) ), ivec2( 0 ), Grid - 1 );
	vec2 f      = clamp( g - vec2( cell ), 0.0, 1.0 );

	bool warpTop = warpAt( cell );
	int weft     = weftOf( cell );
	if( ( Perturb & 16 ) != 0 && ( cell.x & 1 ) == 1 )
		weft = ( weft + 1 ) % max( Shuttles, 1 );
	vec3 weftColour = Shuttle[ clamp( weft, 0, 7 ) ];
	vec3 colour     = warpTop ? Warp : weftColour;

	//Thread Shading fades out where a crossing is under a few pixels: the
	//profile there can only alias, and from that far the cloth is its colour.
	vec2 crossingPx = vec2( OutSize ) / vec2( Grid ) * Zoom;
	float amount    = Shading * smoothstep( 2.0, 6.0, min( crossingPx.x, crossingPx.y ) );

	vec3 cloth = colour;
	if( amount > 0.0 )
	{
		float across = warpTop ? f.x : f.y;
		float along  = warpTop ? f.y : f.x;
		ivec2 before = warpTop ? ivec2( 0, -1 ) : ivec2( -1, 0 );
		bool holdsBefore = cell.x + before.x < 0 || cell.y + before.y < 0 || warpAt( cell + before ) == warpTop;
		bool holdsAfter  = cell.x - before.x >= Grid.x || cell.y - before.y >= Grid.y || warpAt( cell - before ) == warpTop;

		float x = ( across - 0.5 ) / ( 0.5 * kWidth );
		float shade;
		vec3 sheen = vec3( 0.0 );
		if( abs( x ) > 1.0 )
		{
			//The gap between two threads: the other system, in shadow.
			colour = warpTop ? weftColour : Warp;
			shade  = 0.30;
		}
		else
		{
			float z       = sqrt( max( 1.0 - x * x, 0.0 ) );
			vec3 axis     = warpTop ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 );
			vec3 normal   = warpTop ? vec3( x, 0.0, z ) : vec3( 0.0, x, z );

			//Where a float ends the thread dives under the crossing thread:
			//its surface tilts away along its length and falls into shadow.
			float tilt = 0.0;
			if( !holdsBefore && along < kDip )
				tilt = -( 1.0 - along / kDip );
			if( !holdsAfter && along > 1.0 - kDip )
				tilt = ( along - ( 1.0 - kDip ) ) / kDip;
			normal = normalize( normal + axis * tilt * 0.9 );

			vec3 light   = normalize( kLight );
			float lambert = max( dot( normal, light ), 0.0 );
			float twist   = 1.0 - 0.10 * ( 0.5 + 0.5 * cos( 6.2831853 * ( along * kTwist - x * 0.45 ) ) );
			float hollow  = 1.0 - 0.35 * tilt * tilt;
			shade = kGain * ( 0.30 + 0.85 * lambert ) * twist * hollow;

			vec3 halfway = normalize( light + vec3( 0.0, 0.0, 1.0 ) );
			//The sheen is the thread's own colour, brightened: a black satin
			//shines, but only as black silk does, and a white sheen on it
			//would lift every shadow in the picture to grey.
			float spec   = pow( max( dot( normal, halfway ), 0.0 ), 28.0 ) * 0.35 * hollow;
			sheen        = spec * colour;
		}
		cloth = colour * mix( 1.0, shade, amount ) + amount * sheen;
	}

	vec4 woven = vec4( encode( cloth.r ), encode( cloth.g ), encode( cloth.b ), 1.0 );
	fragColor  = mix( source, woven, MixAmount );
}
)";

std::string assemble( const char* body )
{
	return std::string( kVersion ) + body;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody );
}

std::string Cells()
{
	return assemble( kCellsBody );
}

std::string Render()
{
	return assemble( kRenderBody );
}

} // namespace jacquard::shaders
