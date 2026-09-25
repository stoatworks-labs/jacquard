/**
	jqtest -- weave Jacquard offline, and read the cloth back out of it.

	Every check renders a synthetic source through the REAL plugin class in a
	headless GL context, or drives the plugin's own loom with no GL at all,
	and measures rather than eyeballs.

		jqtest --out /tmp/frame.png     a picture, on the test card
		jqtest --list                   every parameter, its kind and default
		jqtest --names                  every name 16 characters or fewer, unique
		jqtest --optimal                every pick the loom weaves costs exactly what
		                                an exhaustive search over colour and pattern
		                                finds (no GL)
		jqtest --floats                 no warp or weft float in the cloth runs past
		                                Max Float, in every structure mode
		jqtest --coverage               each structure's weft coverage on a flat
		                                field is its stated ratio
		jqtest --twill                  a twill's diagonal runs at atan( pick height /
		                                end width ), by the autocorrelation peak
		jqtest --shuttle                every pick shows one weft colour, a shuttle's
		jqtest --distance               from a distance the cloth is the picture
		jqtest --resize                 a resize mid-run keeps the shuttles
		jqtest --negative               every check above can FAIL
		jqtest --bench                  the render cost, and the CPU encoder's share
		jqtest --dump-shaders DIR       the exact GLSL the plugin compiles
		jqtest --pipe                   raw frames in, raw frames out

	Every GL check takes `--size WxH`. Run each at two rasters at least -- the
	one you develop at and 320x180, which is what CI uses.
	JQTEST_RENDERER=software asks for Apple's software renderer, which is what
	CI's macOS runner has. AGENTS.md has one line per check on where each
	tolerance comes from.

	`--script` is a plain text file of `frame  Parameter Name  value` lines,
	the fleet's format. Sliders ramp between cues; options, booleans, integers
	and events STEP (they hold the earlier cue's value until the next cue's
	frame). `--pipe` takes the fleet's frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | jqtest --pipe --size 1920x1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Controls.h"
#include "Encoder.h"
#include "Jacquard.h"
#include "Loom.h"
#include "Palette.h"
#include "Shaders.h"
#include "Weave.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace jacquard;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Sources. Rows are top-first in every source, the way a picture is, and
// flipped on the way into GL. The checks read back through the same flip,
// so "row 0" is the top of the picture everywhere in this file.
//---------------------------------------------------------------------------
using Image = std::vector< unsigned char >;

/// The test card: a sky gradient over a warm ground, a row of greys, eight
/// colour patches, a specular highlight, and a disc on a slow Lissajous path
/// so consecutive frames differ.
Image buildCard( int width, int height, int frame )
{
	Image card( static_cast< size_t >( width ) * height * 4 );

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float t = static_cast< float >( frame );

	const float discX = 0.5f * w + 0.30f * w * std::sin( t * 0.05f );
	const float discY = 0.42f * h + 0.12f * h * std::sin( t * 0.037f + 1.1f );
	const float discR = 0.12f * h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			float r, g, b;
			if( v < 0.55f )
			{
				const float k = v / 0.55f;
				r             = 0.15f + 0.55f * k;
				g             = 0.35f + 0.45f * k;
				b             = 0.95f - 0.15f * k;
			}
			else
			{
				r = 0.42f;
				g = 0.33f;
				b = 0.22f;
			}

			if( v > 0.84f )
			{
				const int step = std::min( 10, static_cast< int >( u * 11.0f ) );
				r = g = b = static_cast< float >( step ) / 10.0f;
			}
			else if( v > 0.66f && v < 0.80f )
			{
				static const float patches[ 8 ][ 3 ] = {
					{ 0.75f, 0.12f, 0.10f }, { 0.20f, 0.60f, 0.20f }, { 0.12f, 0.20f, 0.75f },
					{ 0.10f, 0.65f, 0.70f }, { 0.70f, 0.15f, 0.60f }, { 0.90f, 0.80f, 0.15f },
					{ 0.87f, 0.64f, 0.52f },//skin
					{ 0.03f, 0.03f, 0.03f },//deep shadow
				};
				const int patch = std::min( 7, static_cast< int >( u * 8.0f ) );
				r               = patches[ patch ][ 0 ];
				g               = patches[ patch ][ 1 ];
				b               = patches[ patch ][ 2 ];
			}

			const float hx = u - 0.85f, hy = v - 0.15f;
			if( hx * hx + hy * hy < 0.0015f )
				r = g = b = 1.0f;

			const float dx   = static_cast< float >( x ) + 0.5f - discX;
			const float dy   = static_cast< float >( y ) + 0.5f - discY;
			const float dist = std::sqrt( dx * dx + dy * dy );
			if( dist < discR )
			{
				const float edge = std::min( 1.0f, ( discR - dist ) / ( discR * 0.2f ) );
				r                = r + ( 0.95f - r ) * edge;
				g                = g + ( 0.55f - g ) * edge;
				b                = b + ( 0.15f - b ) * edge;
			}

			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i + 0 ]  = static_cast< unsigned char >( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 1 ]  = static_cast< unsigned char >( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 2 ]  = static_cast< unsigned char >( std::clamp( b, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 3 ]  = 255;
		}
	}
	return card;
}

Image buildFlat( int width, int height, unsigned char r, unsigned char g, unsigned char b )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		image[ i + 0 ] = r;
		image[ i + 1 ] = g;
		image[ i + 2 ] = b;
		image[ i + 3 ] = 255;
	}
	return image;
}

/// A random picture: 6 x 6 pixel blocks of hashed colour, so every crossing
/// sees an arbitrary target and every structure turns up somewhere.
Image buildNoise( int width, int height, uint32_t seed )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const uint32_t h = weave::Hash( seed, static_cast< uint32_t >( x / 6 ), static_cast< uint32_t >( y / 6 ) );
			unsigned char* p = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 0 ]           = static_cast< unsigned char >( h & 0xFF );
			p[ 1 ]           = static_cast< unsigned char >( ( h >> 8 ) & 0xFF );
			p[ 2 ]           = static_cast< unsigned char >( ( h >> 16 ) & 0xFF );
			p[ 3 ]           = 255;
		}
	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//JQTEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU. It is what a GPU-less CI runner falls back to, and it
	//is not repeatable at the last bit (repousse's CI failed by one ulp), so a
	//check that only holds on this Mac's GPU is found here before CI finds it.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "JQTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "jqtest: JQTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

Image flipRows( const Image& image, int width, int height )
{
	Image flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ), image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Jacquard::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_RED: return "red";
	case FF_TYPE_GREEN: return "green";
	case FF_TYPE_BLUE: return "blue";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Jacquard& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Jacquard::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range. An integer's real range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Jacquard& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

/// Options, booleans, integers and events STEP between cues; sliders ramp.
/// A ramp through an option would visit every option between the two cues,
/// and a ramp through an integer would visit every grid on the way.
bool stepsBetweenCues( Jacquard& plugin, unsigned int index )
{
	const unsigned int type = plugin.GetParamType( index );
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_INTEGER || type == FF_TYPE_EVENT;
}

bool applySetting( Jacquard& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	const int index         = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( value.c_str(), nullptr ) );
	return true;
}

bool set( Jacquard& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and its output.
//---------------------------------------------------------------------------
struct Session
{
	Jacquard plugin;
	int width  = 0;
	int height = 0;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputTexture = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	bool renderAt( int frame )
	{
		//Nothing in this plugin reads the clock; it is set anyway, the way a
		//host sets it, so a future dependence on it would not go unseen.
		plugin.SetTime( static_cast< double >( frame ) / 60.0 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
		return ok;
	}

	/// Render one frame of 8-bit `pixels` (top row first).
	bool render( int frame, const Image& pixels )
	{
		const Image flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	/// The output, top row first, 8-bit.
	Image readBack()
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

//---------------------------------------------------------------------------
// The baseline every GL check starts from: 160 ends, Picks Auto, square
// crossings, Max Float 6; the default warp, six shuttles from the clip, tone
// picks the structure; Thread Shading OFF (a crossing is then exactly its
// top thread's colour), the whole cloth in view, fully in. Each check then
// moves the one or two things it is about.
//---------------------------------------------------------------------------
using Settings = std::vector< std::pair< const char*, float > >;

void baseline( Jacquard& p )
{
	set( p, "Ends", 160.0f );
	set( p, "Picks", 0.0f );
	set( p, "Thread Aspect", 0.5f );
	set( p, "Max Float", 6.0f );
	set( p, "Shuttles", 6.0f );
	set( p, "Palette", static_cast< float >( palette::kClip ) );
	set( p, "Structure", static_cast< float >( weave::kModeAuto ) );
	set( p, "Thread Shading", 0.0f );
	set( p, "Zoom", 0.0f );
	set( p, "Mix", 1.0f );
}

bool prepare( Session& session, const Settings& settings, int perturb, int structure, int width, int height )
{
	baseline( session.plugin );
	for( const auto& s : settings )
		if( !set( session.plugin, s.first, s.second ) )
			return false;
	session.plugin.SetPerturbForTest( perturb );
	session.plugin.SetStructureForTest( structure );
	return session.begin( width, height );
}

struct Still
{
	Image picture;
	loom::Cloth cloth;
	std::vector< palette::Colour > shuttles;
	palette::Colour warp;
};

/// Render `frames` frames of one still image through a fresh instance and
/// read the last one back, with the cloth as woven.
bool renderStill( int width, int height, const Settings& settings, const Image& source, int perturb, Still& out, int structure = -1,
                  int frames = 3 )
{
	Session session;
	if( !prepare( session, settings, perturb, structure, width, height ) )
		return false;
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( frame, source ) )
		{
			session.end();
			return false;
		}
	out.picture  = session.readBack();
	out.cloth    = session.plugin.ClothForTest();
	out.shuttles = session.plugin.ShuttlesForTest();
	out.warp     = session.plugin.WarpForTest();
	session.end();
	return true;
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAILED";
}

//---------------------------------------------------------------------------
// Reading the cloth out of the picture.
//
// A crossing (i, j) covers x in [ i W / E, ( i + 1 ) W / E ), and the pixel
// read for it is the one whose centre is nearest the crossing's centre. With
// Zoom 1 and at least 2 pixels a crossing, that pixel's centre is at least a
// quarter of a crossing inside it, far past any float rounding in the
// shader's ( p + 0.5 ) E / W.
//---------------------------------------------------------------------------
int centrePixel( int index, int count, int pixels )
{
	return static_cast< int >( std::floor( ( index + 0.5 ) * pixels / static_cast< double >( count ) ) );
}

const unsigned char* pixelAt( const Image& image, int width, int x, int y )
{
	return image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
}

/// A linear colour as the render writes it: sRGB-encoded, 8 bits.
void expectedBytes( const palette::Colour& c, int* out )
{
	out[ 0 ] = static_cast< int >( std::lround( palette::ToEncoded( c.r ) * 255.0f ) );
	out[ 1 ] = static_cast< int >( std::lround( palette::ToEncoded( c.g ) * 255.0f ) );
	out[ 2 ] = static_cast< int >( std::lround( palette::ToEncoded( c.b ) * 255.0f ) );
}

/// Within one 8-bit step per channel: the shader encodes in float and the
/// harness in float with a different pow, and a value within float error of
/// a rounding boundary may land either side of it. Nothing more.
bool within1( const unsigned char* p, const int* want )
{
	for( int ch = 0; ch < 3; ++ch )
		if( std::abs( static_cast< int >( p[ ch ] ) - want[ ch ] ) > 1 )
			return false;
	return true;
}

/// The grid a picture shows, by classifying each crossing's centre pixel as
/// the warp colour or not. Only meaningful when no shuttle is within a step
/// of the warp colour; `ambiguous` says when one is.
std::vector< uint8_t > liftFromPicture( const Still& s, int width, int height, bool& ambiguous )
{
	const int E = s.cloth.ends, P = s.cloth.picks;
	int warp[ 3 ];
	expectedBytes( s.warp, warp );
	ambiguous = false;
	for( const palette::Colour& c : s.shuttles )
	{
		int b[ 3 ];
		expectedBytes( c, b );
		int d = 0;
		for( int ch = 0; ch < 3; ++ch )
			d = std::max( d, std::abs( b[ ch ] - warp[ ch ] ) );
		if( d <= 2 )
			ambiguous = true;
	}
	std::vector< uint8_t > lift( static_cast< size_t >( E ) * P );
	for( int j = 0; j < P; ++j )
		for( int i = 0; i < E; ++i )
			lift[ static_cast< size_t >( j ) * E + i ] =
				within1( pixelAt( s.picture, width, centrePixel( i, E, width ), centrePixel( j, P, height ) ), warp ) ? 1 : 0;
	return lift;
}

/// The longest run of equal values along any row and any column.
void longestFloats( const std::vector< uint8_t >& lift, int E, int P, int& alongPick, int& alongEnd )
{
	alongPick = alongEnd = 0;
	for( int j = 0; j < P; ++j )
	{
		int run = 0;
		for( int i = 0; i < E; ++i )
		{
			run       = ( i > 0 && lift[ static_cast< size_t >( j ) * E + i ] == lift[ static_cast< size_t >( j ) * E + i - 1 ] ) ? run + 1 : 1;
			alongPick = std::max( alongPick, run );
		}
	}
	for( int i = 0; i < E; ++i )
	{
		int run = 0;
		for( int j = 0; j < P; ++j )
		{
			run      = ( j > 0 && lift[ static_cast< size_t >( j ) * E + i ] == lift[ static_cast< size_t >( j - 1 ) * E + i ] ) ? run + 1 : 1;
			alongEnd = std::max( alongEnd, run );
		}
	}
}

//---------------------------------------------------------------------------
// --optimal
//
// No GL. The loom weaves small random pictures -- 4 to 13 ends, 8 picks,
// 2 to 8 shuttles, Max Float 2 to 6, every structure mode, a random warp --
// and hands every pick's cost table and answer to the check as woven, with
// the forbidden crossings the real column runs produced. For each pick an
// exhaustive search tries every shuttle and every one of the 2^n patterns,
// keeps those that obey the float limit and the forbidden crossings, and
// finds the minimum by summing the table: nothing shared with the
// programme's recursion. Costs are integers, so EQUAL is the claim.
//
// Then 200 tables of arbitrary integer costs, with forbidden crossings drawn
// from a random valid pick above, so the programme is also tried on costs
// the loom would never build.
//
// Negative control: the greedy weaver must cost more than the optimum (or
// break a constraint) on at least one pick.
//---------------------------------------------------------------------------
int64_t exhaustive( const encoder::RowTable& table, bool& any )
{
	const int n = table.n;
	int64_t best = encoder::kInfinite;
	any          = false;
	for( int c = 0; c < table.colours; ++c )
		for( uint32_t mask = 0; mask < ( 1u << n ); ++mask )
		{
			bool ok = true;
			int run = 0;
			int64_t cost = 0;
			for( int i = 0; i < n && ok; ++i )
			{
				const int x = ( mask >> i ) & 1u;
				if( table.forbidden[ static_cast< size_t >( i ) ] == x )
					ok = false;
				run = ( i > 0 && x == static_cast< int >( ( mask >> ( i - 1 ) ) & 1u ) ) ? run + 1 : 1;
				if( table.maxFloat > 0 && run > table.maxFloat )
					ok = false;
				cost += table.cost[ ( static_cast< size_t >( c ) * n + i ) * 2 + x ];
			}
			if( ok )
			{
				any  = true;
				best = std::min( best, cost );
			}
		}
	return best;
}

struct OptimalTally
{
	int rows = 0, exact = 0, infeasible = 0, invalidAnswers = 0, greedyWorse = 0;
	long long programmes = 0;
};

void judgeRow( const encoder::RowTable& table, const encoder::RowResult& result, OptimalTally& tally, bool greedyAnswer )
{
	bool any               = false;
	const int64_t optimum  = exhaustive( table, any );
	bool valid             = false;
	const int64_t realised = result.x.empty() ? encoder::kInfinite : encoder::RowCost( table, result.colour, result.x.data(), valid );
	++tally.rows;
	tally.programmes += result.programmesRun;
	if( !any )
		++tally.infeasible;
	if( !valid || realised != result.cost )
		++tally.invalidAnswers;
	if( valid && realised == optimum && result.cost == optimum )
		++tally.exact;
	if( greedyAnswer && ( !valid || realised > optimum ) )
		++tally.greedyWorse;
}

int runOptimal( int perturb = 0, bool quiet = false )
{
	OptimalTally woven, arbitrary;

	//1. As the loom weaves them.
	for( int trial = 0; trial < 60; ++trial )
	{
		const uint32_t seed = 5000u + static_cast< uint32_t >( trial );
		const int ends      = 4 + static_cast< int >( weave::Hash( seed, 1u, 0u ) % 10u );//4..13
		const int picks     = 8;
		const int K         = 2 + static_cast< int >( weave::Hash( seed, 2u, 0u ) % 7u );//2..8
		const int M         = 2 + static_cast< int >( weave::Hash( seed, 3u, 0u ) % 5u );//2..6

		std::vector< float > means( static_cast< size_t >( ends ) * picks * 4 );
		for( size_t k = 0; k < means.size(); ++k )
			means[ k ] = static_cast< float >( weave::Hash( seed, 4u, static_cast< uint32_t >( k ) ) % 1001u ) / 1000.0f;
		std::vector< palette::Colour > shuttles( static_cast< size_t >( K ) );
		for( int c = 0; c < K; ++c )
			for( int ch = 0; ch < 3; ++ch )
				( &shuttles[ static_cast< size_t >( c ) ].r )[ ch ] =
					static_cast< float >( weave::Hash( seed, 5u, static_cast< uint32_t >( c * 3 + ch ) ) % 1001u ) / 1000.0f;

		loom::Settings s;
		s.ends         = ends;
		s.picks        = picks;
		s.warp         = palette::Colour{ static_cast< float >( weave::Hash( seed, 6u, 0u ) % 1001u ) / 1000.0f,
                                  static_cast< float >( weave::Hash( seed, 6u, 1u ) % 1001u ) / 1000.0f,
                                  static_cast< float >( weave::Hash( seed, 6u, 2u ) % 1001u ) / 1000.0f };
		s.shuttles     = shuttles.data();
		s.shuttleCount = K;
		s.maxFloat     = M;
		s.mode         = static_cast< int >( weave::Hash( seed, 7u, 0u ) % static_cast< uint32_t >( weave::kModeCount ) );
		s.perturb      = perturb;
		s.observe      = [ & ]( int, const encoder::RowTable& table, const encoder::RowResult& result ) {
            judgeRow( table, result, woven, ( perturb & weave::kPerturbGreedy ) != 0 );
		};
		loom::Cloth cloth;
		loom::Weave( means.data(), s, cloth );

		//The same picture through the greedy weaver, for the negative
		//control's own count, independent of `perturb`.
		if( perturb == 0 )
		{
			s.perturb = weave::kPerturbGreedy;
			s.observe = [ & ]( int, const encoder::RowTable& table, const encoder::RowResult& result ) {
				bool any = false, valid = false;
				const int64_t optimum = exhaustive( table, any );
				const int64_t cost    = encoder::RowCost( table, result.colour, result.x.data(), valid );
				if( !valid || cost > optimum )
					++woven.greedyWorse;
			};
			loom::Weave( means.data(), s, cloth );
		}
	}

	//2. Arbitrary integer tables.
	for( int trial = 0; trial < 200; ++trial )
	{
		const uint32_t seed = 9000u + static_cast< uint32_t >( trial );
		const int n         = 3 + static_cast< int >( weave::Hash( seed, 1u, 1u ) % 11u );//3..13
		const int K         = 1 + static_cast< int >( weave::Hash( seed, 2u, 1u ) % 8u ); //1..8
		const int M         = 1 + static_cast< int >( weave::Hash( seed, 3u, 1u ) % 6u ); //1..6
		encoder::RowTable table;
		table.Resize( n, K, M );
		for( size_t k = 0; k < table.cost.size(); ++k )
			table.cost[ k ] = static_cast< int64_t >( weave::Hash( seed, 4u, static_cast< uint32_t >( k ) ) % 100000u );
		//A valid pick above: random runs of at most M. Some of its crossings
		//have run M picks down their end already, and are forbidden that value.
		int value = static_cast< int >( weave::Hash( seed, 5u, 0u ) & 1u ), run = 0;
		for( int i = 0; i < n; ++i )
		{
			const bool flip = run >= M || ( weave::Hash( seed, 6u, static_cast< uint32_t >( i ) ) % 3u ) == 0;
			if( flip && i > 0 )
			{
				value = 1 - value;
				run   = 0;
			}
			++run;
			if( ( weave::Hash( seed, 7u, static_cast< uint32_t >( i ) ) % 3u ) == 0 )
				table.forbidden[ static_cast< size_t >( i ) ] = static_cast< int8_t >( value );
		}
		std::vector< uint8_t > preferred( static_cast< size_t >( K ) * n );
		for( size_t k = 0; k < preferred.size(); ++k )
			preferred[ k ] = static_cast< uint8_t >( weave::Hash( seed, 8u, static_cast< uint32_t >( k ) ) & 1u );
		const bool greedy = ( perturb & weave::kPerturbGreedy ) != 0;
		judgeRow( table, encoder::SolveRow( table, greedy, preferred.data() ), arbitrary, greedy );
	}

	const bool wovenOk     = woven.exact == woven.rows && woven.infeasible == 0 && woven.invalidAnswers == 0;
	const bool arbitraryOk = arbitrary.exact == arbitrary.rows && arbitrary.infeasible == 0 && arbitrary.invalidAnswers == 0;
	if( !quiet )
	{
		std::printf( "optimal, as woven: %d picks (60 random cloths, 4-13 ends, 2-8 shuttles, Max Float 2-6, all modes): "
		             "%d equal the exhaustive search exactly, %d infeasible, %d answers invalid; %.2f colour programmes a pick  %s\n",
		             woven.rows, woven.exact, woven.infeasible, woven.invalidAnswers,
		             woven.rows ? static_cast< double >( woven.programmes ) / woven.rows : 0.0, verdict( wovenOk ) );
		std::printf( "optimal, arbitrary costs: %d tables (3-13 crossings, 1-8 colours, Max Float 1-6, forbidden crossings from a valid pick): "
		             "%d exact, %d infeasible, %d invalid  %s\n",
		             arbitrary.rows, arbitrary.exact, arbitrary.infeasible, arbitrary.invalidAnswers, verdict( arbitraryOk ) );
		if( perturb == 0 )
			std::printf( "optimal: the greedy weaver is worse than the optimum on %d of %d woven picks\n", woven.greedyWorse, woven.rows );
		std::printf( "%s\n", wovenOk && arbitraryOk ? "optimal: every pick is exactly optimal and every table was feasible" : "optimal: FAILURES" );
	}
	return ( wovenOk ? 0 : 1 ) + ( arbitraryOk ? 0 : 1 );
}

//---------------------------------------------------------------------------
// --floats
//
// No warp float and no weft float longer than Max Float, counted two ways:
// on the woven grid the plugin reports, for every structure mode on the
// card, a random picture and the two flat extremes, at Max Float 2, 4 and 6;
// and on the PICTURE, by classifying each crossing's centre pixel as the
// warp colour or not, with a mid-grey warp that no Bright shuttle is near.
// Plus: no pick was ever infeasible.
//
// Negative control: the float constraint dropped. The flat black and flat
// white fields are one unbroken solid, and must float the whole width.
//---------------------------------------------------------------------------
int runFloats( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	struct Source
	{
		const char* name;
		Image image;
	};
	const Source sources[] = {
		{ "card", buildCard( width, height, 0 ) },
		{ "noise", buildNoise( width, height, 17u ) },
		{ "black", buildFlat( width, height, 0, 0, 0 ) },
		{ "white", buildFlat( width, height, 255, 255, 255 ) },
	};
	int worstPick = 0, worstEnd = 0, cases = 0, pictureCases = 0, infeasible = 0;
	for( int mode = 0; mode < weave::kModeCount; ++mode )
		for( const Source& source : sources )
			for( int M : { 2, 4, 6 } )
			{
				//The grid, with the clip's own shuttles.
				Still still;
				if( !renderStill( width, height, { { "Structure", static_cast< float >( mode ) }, { "Max Float", static_cast< float >( M ) } },
				                  source.image, perturb, still ) )
					return failures + 1;
				int alongPick, alongEnd;
				longestFloats( still.cloth.lift, still.cloth.ends, still.cloth.picks, alongPick, alongEnd );
				infeasible += still.cloth.infeasiblePicks;
				bool ok = alongPick <= M && alongEnd <= M && still.cloth.infeasiblePicks == 0;
				++cases;

				//The picture, with a warp no shuttle is near.
				Still shown;
				if( !renderStill( width, height,
				                  { { "Structure", static_cast< float >( mode ) }, { "Max Float", static_cast< float >( M ) },
				                    { "Palette", static_cast< float >( palette::kBright ) }, { "Shuttles", 8.0f },
				                    { "Warp Colour", 0.5f }, { "Warp_Green", 0.5f }, { "Warp_Blue", 0.5f } },
				                  source.image, perturb, shown ) )
					return failures + 1;
				bool ambiguous = false;
				const std::vector< uint8_t > seen = liftFromPicture( shown, width, height, ambiguous );
				int seenPick, seenEnd;
				longestFloats( seen, shown.cloth.ends, shown.cloth.picks, seenPick, seenEnd );
				const bool agrees = seen == shown.cloth.lift;
				ok = ok && !ambiguous && agrees && seenPick <= M && seenEnd <= M;
				++pictureCases;

				worstPick = std::max( { worstPick, alongPick, seenPick } );
				worstEnd  = std::max( { worstEnd, alongEnd, seenEnd } );
				if( !ok )
				{
					++failures;
					if( !quiet )
						std::printf( "floats %-12s %-6s Max Float %d: grid %d across / %d down, picture %d / %d%s%s, %d infeasible picks  FAILED\n",
						             weave::ModeName( mode ), source.name, M, alongPick, alongEnd, seenPick, seenEnd,
						             agrees ? "" : " (picture disagrees with the grid)", ambiguous ? " (ambiguous warp)" : "",
						             still.cloth.infeasiblePicks );
				}
			}
	if( !quiet )
		std::printf( "floats: %d cloths on the grid and %d read from the picture (4 modes x card, noise, black, white x Max Float 2, 4, 6): "
		             "longest weft float %d, longest warp float %d, %d infeasible picks, %d over the limit  %s\n",
		             cases, pictureCases, worstPick, worstEnd, infeasible, failures, verdict( failures == 0 ) );
	return failures;
}

//---------------------------------------------------------------------------
// --coverage
//
// Every structure but the two solids, forced over the whole cloth, on a flat
// mid-grey, read from the PICTURE: over a square of 60 x 60 crossings from
// the top-left (60 is a whole number of repeats of 2, 3, 4 and 5), the
// fraction of crossings that do not show the warp colour. EXACT: the count
// is an integer, and the stated ratio times 3600 is an integer for every
// structure. Max Float 8, above every structure's own longest float (4), so
// no tie is needed and none may appear.
//
// The solids have no ratio of their own once they are tied: their line
// states the tie density, which the float limit bounds from below.
//
// Negative control: every twill one warp crossing heavier per repeat.
//---------------------------------------------------------------------------
int runCoverage( int width, int height, int perturb = 0, bool quiet = false )
{
	constexpr int kSquare = 60;
	int failures          = 0;
	const Image grey      = buildFlat( width, height, 128, 128, 128 );
	for( int s = 0; s < weave::kStructureCount; ++s )
	{
		Still still;
		if( !renderStill( width, height,
		                  { { "Max Float", 8.0f }, { "Palette", static_cast< float >( palette::kMono ) }, { "Shuttles", 2.0f },
		                    { "Warp Colour", 0.85f }, { "Warp_Green", 0.2f }, { "Warp_Blue", 0.2f } },
		                  grey, perturb, still, s ) )
			return failures + 1;
		bool ambiguous = false;
		const std::vector< uint8_t > seen = liftFromPicture( still, width, height, ambiguous );
		const int E = still.cloth.ends, P = still.cloth.picks;
		if( E < kSquare || P < kSquare )
		{
			std::printf( "coverage: the grid is %d x %d, under %d x %d  FAILED\n", E, P, kSquare, kSquare );
			return failures + 1;
		}
		int weft = 0;
		for( int j = 0; j < kSquare; ++j )
			for( int i = 0; i < kSquare; ++i )
				weft += seen[ static_cast< size_t >( j ) * E + i ] ? 0 : 1;
		int num, den;
		weave::Coverage( s, num, den );
		const bool solid = s == weave::kWarpSolid || s == weave::kWeftSolid;
		bool ok          = !ambiguous && seen == still.cloth.lift;
		if( solid )
		{
			//Tied: every run of 9 crossings has a tie, so at least
			//floor( 60 / 9 ) = 6 a line; report the density.
			const int ties = s == weave::kWeftSolid ? kSquare * kSquare - weft : weft;
			ok             = ok && ties >= kSquare * ( kSquare / 9 );
			if( !quiet )
				std::printf( "coverage %-10s: %4d of %d crossings tied (%.4f; one per 9 would be %.4f)  %s\n", weave::StructureName( s ), ties,
				             kSquare * kSquare, ties / 3600.0, 1.0 / 9.0, verdict( ok ) );
		}
		else
		{
			const int want = kSquare * kSquare * num / den;
			ok             = ok && weft == want;
			if( !quiet )
				std::printf( "coverage %-10s: %4d of %d crossings weft = %.4f, stated %d/%d = %.4f  %s\n", weave::StructureName( s ), weft,
				             kSquare * kSquare, weft / 3600.0, num, den, static_cast< double >( num ) / den, verdict( ok ) );
		}
		if( !ok )
			++failures;
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "coverage: every structure shows exactly its stated weft ratio" : "coverage: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --twill
//
// A twill forced over a flat field, read from the PICTURE as a binary field
// (warp colour or not) per pixel, over a central crop of at most 256 x 256.
// The normalised autocorrelation is computed at every lag with 0 <= dy <= L
// and |dx| <= L, L = three crossings; the peak's nearest lag to the origin
// is the diagonal. A twill steps one end per pick, so that lag must be
// EXACTLY ( -end width, +pick height ) in pixels -- down one pick, left one
// end -- and the diagonal's angle atan( pick height / end width ).
//
// Exact, because the checks choose rasters where a crossing is a whole
// number of pixels (Ends 160 across 1280 or 320; Picks from the aspect), so
// the pattern translates exactly by its lag. At a fractional crossing size
// the peak would land on the nearest pixel lag instead, and this check
// would say so rather than pass.
//
// Negative control: twills that do not step (warp ribs): the nearest peak is
// straight down, at 90 degrees.
//---------------------------------------------------------------------------
int runTwill( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures     = 0;
	const Image grey = buildFlat( width, height, 128, 128, 128 );
	struct Case
	{
		int structure;
		float aspectParam;
	};
	const Case cases[] = { { weave::kTwill22, 0.5f }, { weave::kTwill31, 0.5f }, { weave::kTwill21, 0.5f }, { weave::kTwill13, 1.0f },
		                   { weave::kTwill12, 0.0f } };
	for( const Case& c : cases )
	{
		Still still;
		if( !renderStill( width, height,
		                  { { "Thread Aspect", c.aspectParam }, { "Max Float", 8.0f }, { "Palette", static_cast< float >( palette::kMono ) },
		                    { "Shuttles", 2.0f }, { "Warp Colour", 0.85f }, { "Warp_Green", 0.2f }, { "Warp_Blue", 0.2f } },
		                  grey, perturb, still, c.structure ) )
			return failures + 1;
		const int E = still.cloth.ends, P = still.cloth.picks;
		const bool whole = width % E == 0 && height % P == 0;
		const int cw = width / E, ch = height / P;

		int warp[ 3 ];
		expectedBytes( still.warp, warp );
		const int side = std::min( { 256, width, height } );
		const int x0 = ( width - side ) / 2, y0 = ( height - side ) / 2;
		std::vector< float > field( static_cast< size_t >( side ) * side );
		double mean = 0.0;
		for( int y = 0; y < side; ++y )
			for( int x = 0; x < side; ++x )
			{
				const float v = within1( pixelAt( still.picture, width, x0 + x, y0 + y ), warp ) ? 1.0f : -1.0f;
				field[ static_cast< size_t >( y ) * side + x ] = v;
				mean += v;
			}
		mean /= static_cast< double >( field.size() );
		for( float& v : field )
			v -= static_cast< float >( mean );

		const int L = 3 * std::max( cw, ch );
		double peak = -2.0;
		std::vector< std::pair< int, int > > lags;
		std::map< std::pair< int, int >, double > correlation;
		for( int dy = 0; dy <= L; ++dy )
			for( int dx = -L; dx <= L; ++dx )
			{
				if( dy == 0 && dx <= 0 )
					continue;//the origin, and the half-plane's mirror
				double sab = 0.0, saa = 0.0, sbb = 0.0;
				for( int y = 0; y + dy < side; ++y )
					for( int x = std::max( 0, -dx ); x < side && x + dx < side; ++x )
					{
						const double a = field[ static_cast< size_t >( y ) * side + x ];
						const double b = field[ static_cast< size_t >( y + dy ) * side + x + dx ];
						sab += a * b;
						saa += a * a;
						sbb += b * b;
					}
				const double r = sab / std::sqrt( std::max( saa * sbb, 1e-30 ) );
				correlation[ { dx, dy } ] = r;
				peak = std::max( peak, r );
			}
		//The nearest lag at the peak (within a part in a million of it: the
		//edge-cropped sums differ in the last bits between lags).
		int bestDx = 0, bestDy = 0;
		double bestNorm = 1e9;
		for( const auto& entry : correlation )
			if( entry.second >= peak - 1e-6 )
			{
				const double norm = std::hypot( entry.first.first, entry.first.second );
				if( norm < bestNorm - 1e-9 )
				{
					bestNorm = norm;
					bestDx   = entry.first.first;
					bestDy   = entry.first.second;
				}
			}
		const double measured = std::atan2( static_cast< double >( bestDy ), static_cast< double >( -bestDx ) ) * 180.0 / 3.14159265358979323846;
		const double stated   = std::atan2( static_cast< double >( height ) / P, static_cast< double >( width ) / E ) * 180.0 / 3.14159265358979323846;
		const bool ok         = whole && bestDx == -cw && bestDy == ch;
		if( !quiet )
			std::printf( "twill %s, crossings %d x %d px: peak %.4f at lag (%d, %d) px, diagonal %.2f deg; stated atan( %d / %d ) = %.2f deg  %s\n",
			             weave::StructureName( c.structure ), cw, ch, peak, bestDx, bestDy, measured, ch, cw, stated,
			             whole ? verdict( ok ) : "FAILED (not a whole-pixel crossing)" );
		if( !ok )
			++failures;
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "twill: every twill's diagonal runs at atan( pick height / end width )" : "twill: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --shuttle
//
// Every pick shows ONE weft colour, and it is a shuttle's. On the card and
// on a random picture, the clip's own six shuttles and the Bright eight, read
// from the PICTURE at each crossing's centre pixel:
//
//   - every crossing is either the warp colour or the colour of the pick's
//     shuttle, and which one agrees with the grid the plugin reports;
//   - independently of the grid: the crossings of a pick that are not the
//     warp colour are all ONE colour (to one 8-bit step), and that colour is
//     one of the loaded shuttles.
//
// Negative control: odd ends drawn in the next shuttle's colour.
//---------------------------------------------------------------------------
int runShuttle( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	struct Case
	{
		const char* name;
		Image image;
		int source;
		int shuttles;
	};
	const Case cases[] = {
		{ "card, Clip x6", buildCard( width, height, 0 ), palette::kClip, 6 },
		{ "noise, Clip x8", buildNoise( width, height, 3u ), palette::kClip, 8 },
		{ "card, Bright x8", buildCard( width, height, 0 ), palette::kBright, 8 },
		{ "card, Heritage x3", buildCard( width, height, 0 ), palette::kHeritage, 3 },
	};
	for( const Case& c : cases )
	{
		Still still;
		if( !renderStill( width, height, { { "Palette", static_cast< float >( c.source ) }, { "Shuttles", static_cast< float >( c.shuttles ) } },
		                  c.image, perturb, still ) )
			return failures + 1;
		const int E = still.cloth.ends, P = still.cloth.picks;
		int warp[ 3 ];
		expectedBytes( still.warp, warp );
		std::vector< std::array< int, 3 > > shuttleBytes;
		for( const palette::Colour& s : still.shuttles )
		{
			std::array< int, 3 > b;
			expectedBytes( s, b.data() );
			shuttleBytes.push_back( b );
		}
		int wrong = 0, multiColourPicks = 0, offPalettePicks = 0, weftCrossings = 0;
		std::set< int > coloursUsed;
		for( int j = 0; j < P; ++j )
		{
			const int y = centrePixel( j, P, height );
			const std::array< int, 3 >& mine = shuttleBytes[ still.cloth.weft[ static_cast< size_t >( j ) ] ];
			std::vector< std::array< int, 3 > > distinct;
			for( int i = 0; i < E; ++i )
			{
				const unsigned char* p = pixelAt( still.picture, width, centrePixel( i, E, width ), y );
				const bool lifted      = still.cloth.lift[ static_cast< size_t >( j ) * E + i ] != 0;
				if( !within1( p, lifted ? warp : mine.data() ) )
					++wrong;
				if( within1( p, warp ) )
					continue;
				++weftCrossings;
				bool seen = false;
				for( const auto& d : distinct )
					seen = seen || within1( p, d.data() );
				if( !seen )
					distinct.push_back( { p[ 0 ], p[ 1 ], p[ 2 ] } );
			}
			if( distinct.size() > 1 )
				++multiColourPicks;
			if( distinct.size() == 1 )
			{
				int which = -1;
				for( size_t k = 0; k < shuttleBytes.size(); ++k )
				{
					const unsigned char q[ 3 ] = { static_cast< unsigned char >( distinct[ 0 ][ 0 ] ), static_cast< unsigned char >( distinct[ 0 ][ 1 ] ),
						                           static_cast< unsigned char >( distinct[ 0 ][ 2 ] ) };
					if( within1( q, shuttleBytes[ k ].data() ) )
						which = static_cast< int >( k );
				}
				if( which < 0 )
					++offPalettePicks;
				else
					coloursUsed.insert( which );
			}
		}
		const bool ok = wrong == 0 && multiColourPicks == 0 && offPalettePicks == 0 && weftCrossings > 0;
		if( !quiet )
			std::printf( "shuttle %-18s: %d picks, %d weft crossings; %d crossings not the colour the grid says, %d picks with two weft colours, "
			             "%d off the shuttles; %zu of %zu shuttles used  %s\n",
			             c.name, P, weftCrossings, wrong, multiColourPicks, offPalettePicks, coloursUsed.size(), shuttleBytes.size(), verdict( ok ) );
		if( !ok )
			++failures;
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "shuttle: every pick is one weft colour, from the shuttles loaded" : "shuttle: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --distance
//
// From across the room the cloth is the picture. The rendered frame and the
// source are each reduced to blocks of 8 x 8 crossings, in linear light (a
// distance does that optically), and compared on the sRGB encoding of the
// block means. The claim is comparative, so it needs no fitted tolerance:
// the cloth must be nearer the picture than the best picture-free cloth, a
// single colour everywhere -- the frame's own mean, which is the constant
// that minimises the linear error.
//
// Run at the defaults (Thread Shading 1, lit thread with its gaps and dips)
// and with the shading off.
//
// Negative control: each pick's weft swapped for another shuttle after the
// weave, so the colour the loom chose is no longer the colour shown.
//---------------------------------------------------------------------------
int runDistance( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	const Image card = buildCard( width, height, 0 );
	for( float shading : { 1.0f, 0.0f } )
	{
		Still still;
		if( !renderStill( width, height, { { "Thread Shading", shading } }, card, perturb, still ) )
			return failures + 1;
		const int E = still.cloth.ends, P = still.cloth.picks;
		constexpr int kBlock = 8;
		const int bx = E / kBlock, by = P / kBlock;
		std::vector< double > woven( static_cast< size_t >( bx ) * by * 3, 0.0 ), source( woven.size(), 0.0 );
		std::vector< double > count( static_cast< size_t >( bx ) * by, 0.0 );
		double meanSource[ 3 ] = { 0, 0, 0 };
		double pixels          = 0.0;
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
			{
				const int i = static_cast< int >( ( x + 0.5 ) * E / width ) / kBlock;
				const int j = static_cast< int >( ( y + 0.5 ) * P / height ) / kBlock;
				const unsigned char* w = pixelAt( still.picture, width, x, y );
				const unsigned char* s = pixelAt( card, width, x, y );
				for( int ch = 0; ch < 3; ++ch )
					meanSource[ ch ] += palette::ToLinear( s[ ch ] / 255.0f );
				pixels += 1.0;
				if( i >= bx || j >= by )
					continue;
				const size_t b = static_cast< size_t >( j ) * bx + i;
				for( int ch = 0; ch < 3; ++ch )
				{
					woven[ b * 3 + ch ] += palette::ToLinear( w[ ch ] / 255.0f );
					source[ b * 3 + ch ] += palette::ToLinear( s[ ch ] / 255.0f );
				}
				count[ b ] += 1.0;
			}
		double errWoven = 0.0, errFlat = 0.0;
		for( size_t b = 0; b < count.size(); ++b )
			for( int ch = 0; ch < 3; ++ch )
			{
				const double sv = palette::ToEncoded( static_cast< float >( source[ b * 3 + ch ] / count[ b ] ) );
				const double wv = palette::ToEncoded( static_cast< float >( woven[ b * 3 + ch ] / count[ b ] ) );
				const double fv = palette::ToEncoded( static_cast< float >( meanSource[ ch ] / pixels ) );
				errWoven += ( wv - sv ) * ( wv - sv );
				errFlat += ( fv - sv ) * ( fv - sv );
			}
		const double n    = static_cast< double >( count.size() ) * 3.0;
		const double rmsW = std::sqrt( errWoven / n ), rmsF = std::sqrt( errFlat / n );
		const bool ok     = rmsW < rmsF;
		if( !quiet )
			std::printf( "distance, Thread Shading %.0f: %d x %d blocks of 8 x 8 crossings, RMS error on the encoding %.4f; the frame's mean colour "
			             "everywhere %.4f; ratio %.3f  %s\n",
			             shading, bx, by, rmsW, rmsF, rmsW / rmsF, verdict( ok ) );
		if( !ok )
			++failures;
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "distance: from a distance the cloth is nearer the picture than any single colour" : "distance: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//
// The one state across frames is the shuttles (the clip's k-means, warm-
// started). Six frames of the card at the raster given, then the SAME
// instance handed a differently sized input, as a host does: the k-means
// must start from the shuttles it had, bit for bit, and must not have cold-
// started again. Then back.
//
// Negative control: a palette that forgets itself on a resize.
//---------------------------------------------------------------------------
int runResize( int width, int height, int perturb = 0, bool quiet = false )
{
	Session session;
	if( !prepare( session, {}, perturb, -1, width, height ) )
		return 1;
	const int otherW = width == 320 ? 480 : 320, otherH = width == 320 ? 270 : 180;
	int failures = 0;
	auto step = [ & ]( int frame, int w, int h, const char* what ) {
		const std::vector< palette::Colour > before = session.plugin.KMeansForTest().Centres();
		const int coldBefore                        = session.plugin.KMeansForTest().ColdStarts();
		if( w != session.width || h != session.height )
			session.resize( w, h );
		if( !session.render( frame, buildCard( w, h, frame ) ) )
		{
			++failures;
			return;
		}
		const auto& seed = session.plugin.KMeansForTest().LastSeed();
		bool same        = seed.size() == before.size();
		for( size_t k = 0; same && k < seed.size(); ++k )
			same = std::memcmp( &seed[ k ], &before[ k ], sizeof( palette::Colour ) ) == 0;
		const bool ok = same && session.plugin.KMeansForTest().ColdStarts() == coldBefore;
		if( !quiet )
			std::printf( "resize %s to %dx%d at frame %d: started from the shuttles it had: %s; cold starts %d  %s\n", what, w, h, frame,
			             same ? "yes" : "NO", session.plugin.KMeansForTest().ColdStarts(), verdict( ok ) );
		if( !ok )
			++failures;
	};
	for( int frame = 0; frame < 6; ++frame )
		if( !session.render( frame, buildCard( width, height, frame ) ) )
			return 1;
	step( 6, otherW, otherH, "out" );
	step( 7, otherW, otherH, "held" );
	step( 8, width, height, "back" );
	session.end();
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "resize: the shuttles survive a change of raster" : "resize: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --negative
//
// A check that cannot fail is not a check. Each of these perturbs the MODEL
// through a hook the shipped plugin carries at zero, and asserts that the
// check catches it.
//---------------------------------------------------------------------------
int runNegative( int width, int height )
{
	struct Control
	{
		const char* name;
		int failuresSeen;
	};
	const Control controls[] = {
		{ "optimal with the greedy weaver                  ", runOptimal( weave::kPerturbGreedy, true ) },
		{ "floats with the float limit dropped             ", runFloats( width, height, weave::kPerturbNoFloatLimit, true ) },
		{ "coverage with every twill one crossing heavier  ", runCoverage( width, height, weave::kPerturbCoverage, true ) },
		{ "twill with twills that do not step              ", runTwill( width, height, weave::kPerturbTwillStep0, true ) },
		{ "shuttle with odd ends in the next shuttle       ", runShuttle( width, height, weave::kPerturbTwoWefts, true ) },
		{ "distance with each pick's weft swapped          ", runDistance( width, height, weave::kPerturbScramble, true ) },
		{ "resize with a palette that forgets on a resize  ", runResize( width, height, weave::kPerturbColdResize, true ) },
	};
	int failures = 0;
	for( const Control& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		std::printf( "negative %s: %s  %s\n", c.name, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
			++failures;
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed model is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
int runNames()
{
	Jacquard plugin;
	std::set< std::string > seen;
	int failures = 0;
	size_t longest = 0;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		longest = std::max( longest, p.name.size() );
		if( p.name.size() > 16 )
		{
			std::printf( "names: '%s' is %zu characters  FAILED\n", p.name.c_str(), p.name.size() );
			++failures;
		}
		if( !seen.insert( p.name ).second )
		{
			std::printf( "names: '%s' is declared twice  FAILED\n", p.name.c_str() );
			++failures;
		}
	}
	std::printf( "names: %zu parameters, longest %zu characters, all unique  %s\n", seen.size(), longest, verdict( failures == 0 ) );
	return failures;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
struct BenchResult
{
	double ms  = -1.0;
	double cpu = 0.0;
	int ends = 0, picks = 0;
};

BenchResult benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	BenchResult result;
	Session session;
	for( const std::string& s : settings )
	{
		std::string error;
		applySetting( session.plugin, s, error );
	}
	if( !session.begin( width, height ) )
		return result;

	//The card moves, so every frame is woven afresh -- the real cost. Eight
	//frames of it are uploaded ONCE and cycled by handle, the way a host
	//hands over a texture it already has: the upload is not in the figure.
	constexpr int kCards = 8;
	GLuint textures[ kCards ];
	for( int i = 0; i < kCards; ++i )
	{
		const Image flipped = flipRows( buildCard( width, height, i * 5 ), width, height );
		textures[ i ]       = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, flipped.data() );
	}
	auto renderCard = [ & ]( int frame ) {
		session.inputStruct.Handle = textures[ frame % kCards ];
		session.renderAt( frame );
	};

	for( int frame = 0; frame < 10; ++frame )
		renderCard( frame );
	glFinish();

	//Best of three: the GPU and the CPU here are shared with other builds,
	//and the minimum is the run nothing else interrupted.
	double best = 1e9, bestCpu = 0.0;
	for( int run = 0; run < 3; ++run )
	{
		double cpu       = 0.0;
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
		{
			renderCard( 10 + run * frames + frame );
			cpu += session.plugin.CpuMillisecondsForTest();
		}
		glFinish();
		const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / frames;
		if( ms < best )
		{
			best    = ms;
			bestCpu = cpu / frames;
		}
	}
	result.ms    = best;
	result.cpu   = bestCpu;
	result.ends  = session.plugin.ClothForTest().ends;
	result.picks = session.plugin.ClothForTest().picks;

	session.inputStruct.Handle = session.sourceTexture;
	glDeleteTextures( kCards, textures );
	session.end();
	return result;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720 ", 1280, 720 }, { "1920x1080", 1920, 1080 }, { "3840x2160", 3840, 2160 } };
	struct Grid
	{
		const char* name;
		std::vector< std::string > extra;
	};
	const Grid grids[] = { { "defaults (160 ends)", {} }, { "largest grid (320 ends)", { "Ends=320" } } };

	std::printf( "%d frames each, best of three runs, after a 10-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "grid                      resolution   crossings    ms/frame   of which CPU   %% of a 60fps frame\n" );
	for( const Grid& grid : grids )
		for( const Size& size : sizes )
		{
			std::vector< std::string > all = settings;
			all.insert( all.end(), grid.extra.begin(), grid.extra.end() );
			const BenchResult r = benchAt( all, size.width, size.height, frames );
			std::printf( "%-24s  %s    %3d x %3d   %7.3f      %6.3f         %5.1f%%\n", grid.name, size.name, r.ends, r.picks, r.ms, r.cpu,
			             r.ms / 16.667 * 100.0 );
		}
	std::printf( "\nms/frame includes the cells pass, the read-back, the CPU half (the\n"
	             "shuttles' k-means and the loom's programme for every pick) and the render.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() },
		{ "cells.frag", shaders::Cells() },
		{ "render.frag", shaders::Render() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line. Same format as the rest
// of the fleet, so one filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

/// The value at `frame`: a slider ramps linearly between cues; a stepped
/// parameter holds the last cue at or before the frame.
float valueAt( const Track& track, int frame, bool stepped )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame < track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			if( stepped )
				return a.second;
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"jqtest -- weave and measure the Jacquard effect\n"
		"\n"
		"  --out PATH          render the test card through the plugin (default /tmp/jacquard.png)\n"
		"  --size WxH          raster (default 1280x720)\n"
		"  --frames N          frames to render before reading back (default 4)\n"
		"  --source S          card (default), noise, grey, halves (red | blue)\n"
		"  --set \"Name=V\"      set a parameter by its display name. Repeatable.\n"
		"  --list              print every parameter, its kind, default and range, then exit\n"
		"  --names             every name 16 characters or fewer, and unique\n"
		"  --optimal           every pick equals an exhaustive search, exactly (no GL)\n"
		"  --floats            no float runs past Max Float, grid and picture\n"
		"  --coverage          each structure's weft coverage is its stated ratio\n"
		"  --twill             a twill's diagonal runs at atan( pick height / end width )\n"
		"  --shuttle           every pick is one weft colour, a shuttle's\n"
		"  --distance          from a distance the cloth is the picture\n"
		"  --resize            the shuttles survive a change of raster\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks against a perturbed model (Weave.h), verbosely\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K, default and largest grid\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'; sliders ramp,\n"
		"                      options, booleans, integers and events step\n"
		"  --fail-render-at N  (--pipe) fail the render at frame N, to test the exit path\n"
		"  --help\n"
		"\n"
		"JQTEST_RENDERER=software asks for Apple's software renderer.\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/jacquard.png";
	std::string scriptPath;
	std::string sourceName = "card";
	std::string dumpDir;
	int width        = 1280;
	int height       = 720;
	int frames       = 4;
	int perturb      = 0;
	int failRenderAt = -1;
	bool wantList    = false;
	bool wantBench   = false;
	bool wantPipe    = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			sourceName = argv[ ++i ];
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRenderAt = std::atoi( argv[ ++i ] );
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--optimal" || argument == "--floats" || argument == "--coverage" || argument == "--twill"
		         || argument == "--shuttle" || argument == "--distance" || argument == "--resize" || argument == "--negative"
		         || argument == "--names" )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 )
	{
		std::fprintf( stderr, "width, height and frames must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed, so it is answered before a context is made -- which also
		//means it works on a machine where creating one fails, and in CI.
		Jacquard plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	//--optimal and --names need no context either.
	const bool glFree = !checks.empty()
	                    && std::all_of( checks.begin(), checks.end(), []( const std::string& c ) { return c == "--optimal" || c == "--names"; } );
	if( glFree )
	{
		int failures = 0;
		for( const std::string& check : checks )
			failures += check == "--optimal" ? runOptimal( perturb ) : runNames();
		return failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( !checks.empty() )
	{
		int failures = 0;
		for( const std::string& check : checks )
		{
			int result = 0;
			if( check == "--optimal" )
				result = runOptimal( perturb );
			else if( check == "--names" )
				result = runNames();
			else if( check == "--floats" )
				result = runFloats( width, height, perturb );
			else if( check == "--coverage" )
				result = runCoverage( width, height, perturb );
			else if( check == "--twill" )
				result = runTwill( width, height, perturb );
			else if( check == "--shuttle" )
				result = runShuttle( width, height, perturb );
			else if( check == "--distance" )
				result = runDistance( width, height, perturb );
			else if( check == "--resize" )
				result = runResize( width, height, perturb );
			else if( check == "--negative" )
				result = runNegative( width, height );
			failures += result;
			std::printf( "\n" );

		}
		return finish( failures == 0 ? 0 : 1 );
	}

	if( wantBench )
		return finish( runBench( settings, frames < 10 ? 30 : frames ) );

	Session session;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( !session.begin( width, height ) )
		return finish( 1 );

	if( wantPipe )
	{
		//A reader that hangs up must end the take with exit 1 and a message,
		//not SIGPIPE's silent 141: write() then fails and the loop says so.
		std::signal( SIGPIPE, SIG_IGN );
		struct Automation
		{
			Track track;
			bool stepped;
		};
		std::map< unsigned int, Automation > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = { entry.second,
					                                                   stepsBetweenCues( session.plugin, static_cast< unsigned int >( index ) ) };
			}
		}

		Image frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame at the end of a pipe is the end of the stream,
			//not a frame to render: the stream ends cleanly, and only whole
			//frames ever come out.
			if( got < frame.size() )
				break;

			//Through the plugin's own setter, so a cue moves the same thing an
			//operator's slider would.
			for( const auto& a : automation )
				session.plugin.SetFloatParameter( a.first, valueAt( a.second.track, index, a.second.stepped ) );

			if( index == failRenderAt || !session.render( index, frame ) )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const Image out = session.readBack();
			size_t written  = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone. Rendering on into a closed pipe is work
			//nobody will see, and a short frame on stdout is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}

		session.end();
		return finish( status );
	}

	for( int frame = 0; frame < frames; ++frame )
	{
		Image pixels;
		if( sourceName == "noise" )
			pixels = buildNoise( width, height, 17u );
		else if( sourceName == "grey" )
			pixels = buildFlat( width, height, 128, 128, 128 );
		else if( sourceName == "halves" )
		{
			pixels = buildFlat( width, height, 220, 30, 30 );
			for( int y = 0; y < height; ++y )
				for( int x = width / 2; x < width; ++x )
				{
					unsigned char* p = pixels.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
					p[ 0 ] = 30;
					p[ 1 ] = 50;
					p[ 2 ] = 220;
				}
		}
		else
			pixels = buildCard( width, height, frame );
		if( !session.render( frame, pixels ) )
			return finish( 1 );
	}

	const Image image = session.readBack();
	const loom::Cloth& cloth = session.plugin.ClothForTest();
	std::printf( "shuttles (sRGB):" );
	for( const palette::Colour& c : session.plugin.ShuttlesForTest() )
		std::printf( " (%.2f %.2f %.2f)", palette::ToEncoded( c.r ), palette::ToEncoded( c.g ), palette::ToEncoded( c.b ) );
	std::printf( "\n" );
	if( std::getenv( "JQTEST_DEBUG" ) )
	{
		const std::vector< float >& m = session.plugin.MeansForTest();
		for( int j = 0; j < cloth.picks; ++j )
		{
			double r = 0, g = 0, b = 0;
			int lifted = 0;
			for( int i = 0; i < cloth.ends; ++i )
			{
				const float* t = m.data() + ( static_cast< size_t >( j ) * cloth.ends + i ) * 4;
				r += t[ 0 ];
				g += t[ 1 ];
				b += t[ 2 ];
				lifted += cloth.lift[ static_cast< size_t >( j ) * cloth.ends + i ];
			}
			std::printf( "pick %2d: weft %d, warp up %3d of %d, row mean (%.2f %.2f %.2f), left (%.2f %.2f %.2f)\n", j, cloth.weft[ static_cast< size_t >( j ) ],
			             lifted, cloth.ends, palette::ToEncoded( static_cast< float >( r / cloth.ends ) ), palette::ToEncoded( static_cast< float >( g / cloth.ends ) ),
			             palette::ToEncoded( static_cast< float >( b / cloth.ends ) ), palette::ToEncoded( m[ static_cast< size_t >( j ) * cloth.ends * 4 ] ),
			             palette::ToEncoded( m[ static_cast< size_t >( j ) * cloth.ends * 4 + 1 ] ), palette::ToEncoded( m[ static_cast< size_t >( j ) * cloth.ends * 4 + 2 ] ) );
		}
	}
	std::printf( "wrote %s (%dx%d, %d frames; %d ends x %d picks, CPU %.3f ms)\n", outPath.c_str(), width, height, frames, cloth.ends, cloth.picks,
	             session.plugin.CpuMillisecondsForTest() );
	session.end();

	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	return finish( 0 );
}
