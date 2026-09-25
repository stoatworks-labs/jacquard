#include "Jacquard.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"
#include "Weave.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace jacquard;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Jacquard >,                                   // Create method
	"JQ01",                                                      // Plugin unique ID of maximum length 4.
	"SW Jacquard",                                               // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	0,                                                           // Plugin major version number
	1,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"The picture woven on a jacquard loom.\n\nOne warp colour, one weft colour a pick from the shuttles loaded, tone carried by weave structure -- satin, twill, plain -- and no float longer than the weaver allows. An exactly optimal encoder weaves each pick; the threads are drawn lit, so the cloth reads as thread up close and as the picture from across the room.",// Plugin description
	"Jacquard FFGL effect"                                       // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// The k-means reads at most this many crossings a frame.
constexpr int kPaletteSamples = 16384;
} // namespace

//---------------------------------------------------------------------------
Jacquard::Jacquard()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//---------------------------------------------------------------------
	// Defaults. 160 ends across, square crossings, floats of at most ten;
	// a black warp, six shuttles dyed from the clip, structure by tone;
	// the threads shaded, the whole cloth in view, the effect fully in.
	//---------------------------------------------------------------------
	params[ PT_ENDS ]          = static_cast< float >( controls::kEndsDefault );
	params[ PT_PICKS ]         = 0.0f;
	params[ PT_THREAD_ASPECT ] = 0.5f;
	params[ PT_MAX_FLOAT ]     = static_cast< float >( controls::kMaxFloatDefault );

	params[ PT_WARP_R ]    = 0.08f;
	params[ PT_WARP_G ]    = 0.07f;
	params[ PT_WARP_B ]    = 0.07f;
	params[ PT_SHUTTLES ]  = static_cast< float >( controls::kShuttlesDefault );
	params[ PT_PALETTE ]   = static_cast< float >( palette::kClip );
	params[ PT_STRUCTURE ] = static_cast< float >( weave::kModeAuto );

	params[ PT_SHADING ] = 1.0f;
	params[ PT_ZOOM ]    = 0.0f;
	params[ PT_MIX ]     = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. The four counts are FF_TYPE_INTEGER, which SetParamInfo
	// does not clamp into 0..1; the option lists are mapped by index in
	// Controls.cpp because an option's range reads back 0..1.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};
	auto declareInteger = [ this ]( unsigned int id, const char* name, int lo, int hi ) {
		SetParamInfo( id, name, FF_TYPE_INTEGER, params[ id ] );
		SetParamRange( id, static_cast< float >( lo ), static_cast< float >( hi ) );
	};

	declareInteger( PT_ENDS, "Ends", controls::kEndsMin, controls::kEndsMax );
	declareInteger( PT_PICKS, "Picks", 0, controls::kPicksMax );
	SetParamInfo( PT_THREAD_ASPECT, "Thread Aspect", FF_TYPE_STANDARD, params[ PT_THREAD_ASPECT ] );
	declareInteger( PT_MAX_FLOAT, "Max Float", controls::kMaxFloatMin, controls::kMaxFloatMax );

	SetParamInfo( PT_WARP_R, "Warp Colour", FF_TYPE_RED, params[ PT_WARP_R ] );
	SetParamInfo( PT_WARP_G, "Warp_Green", FF_TYPE_GREEN, params[ PT_WARP_G ] );
	SetParamInfo( PT_WARP_B, "Warp_Blue", FF_TYPE_BLUE, params[ PT_WARP_B ] );
	declareInteger( PT_SHUTTLES, "Shuttles", controls::kShuttlesMin, controls::kShuttlesMax );
	declareOptions( PT_PALETTE, "Palette", palette::kSourceCount, palette::SourceName );
	declareOptions( PT_STRUCTURE, "Structure", weave::kModeCount, weave::ModeName );

	SetParamInfo( PT_SHADING, "Thread Shading", FF_TYPE_STANDARD, params[ PT_SHADING ] );
	SetParamInfo( PT_ZOOM, "Zoom", FF_TYPE_STANDARD, params[ PT_ZOOM ] );
	SetParamInfo( PT_MIX, "Mix", FF_TYPE_STANDARD, params[ PT_MIX ] );

	for( FFUInt32 i = PT_ENDS; i <= PT_MAX_FLOAT; ++i )
		SetParamGroup( i, "Loom" );
	for( FFUInt32 i = PT_WARP_R; i <= PT_STRUCTURE; ++i )
		SetParamGroup( i, "Threads" );
	for( FFUInt32 i = PT_SHADING; i <= PT_MIX; ++i )
		SetParamGroup( i, "Look" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Jacquard effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Jacquard::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &cellsShader, shaders::Cells(), "cells" },
		{ &renderShader, shaders::Render(), "render" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Jacquard: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Jacquard: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenTextures( 1, &liftTexture );
	liftEnds = liftPicks = 0;
	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void Jacquard::uploadLift()
{
	const int ends  = cloth.ends;
	const int picks = cloth.picks;
	liftBytes.resize( static_cast< size_t >( ends ) * picks * 2 );
	for( int j = 0; j < picks; ++j )
		for( int i = 0; i < ends; ++i )
		{
			const size_t cell       = static_cast< size_t >( j ) * ends + i;
			liftBytes[ cell * 2 ]     = cloth.lift[ cell ] ? 255 : 0;
			liftBytes[ cell * 2 + 1 ] = cloth.weft[ static_cast< size_t >( j ) ];
		}
	glBindTexture( GL_TEXTURE_2D, liftTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, ends, picks, GL_RG, GL_UNSIGNED_BYTE, liftBytes.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

//---------------------------------------------------------------------------
FFResult Jacquard::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's viewport, read before anything of ours changes it.
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const int ends       = controls::Ends( params[ PT_ENDS ] );
	const double aspect  = controls::ThreadAspect( params[ PT_THREAD_ASPECT ] );
	const int picks      = controls::Picks( params[ PT_PICKS ], ends, width, height, aspect );
	const int maxFloat   = controls::MaxFloat( params[ PT_MAX_FLOAT ] );
	const int shuttleCount = controls::Shuttles( params[ PT_SHUTTLES ] );
	const int source     = controls::OptionIndex( params[ PT_PALETTE ], palette::kSourceCount );
	const int mode       = controls::OptionIndex( params[ PT_STRUCTURE ], weave::kModeCount );
	const float shading  = std::clamp( params[ PT_SHADING ], 0.0f, 1.0f );
	const float zoom     = static_cast< float >( controls::Zoom( params[ PT_ZOOM ] ) );
	const float mixAmount = std::clamp( params[ PT_MIX ], 0.0f, 1.0f );
	warp = palette::Colour{ palette::ToLinear( params[ PT_WARP_R ] ), palette::ToLinear( params[ PT_WARP_G ] ),
		                    palette::ToLinear( params[ PT_WARP_B ] ) };

	//---------------------------------------------------------------------
	// Buffers. Allocation happens here, before anything binds a texture:
	// allocating leaves the active unit bound to nothing, and the symptom of
	// getting the order wrong is correct on every frame except the one that
	// allocates. Nothing here holds a previous frame on the GPU; the one
	// piece of state across frames is the shuttles, K colours on the CPU.
	//---------------------------------------------------------------------
	if( !cells.Ensure( ends, picks, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the cells buffer at " + std::to_string( ends ) + " x " + std::to_string( picks ) );
		return FF_FAIL;
	}
	if( ends != liftEnds || picks != liftPicks )
	{
		glBindTexture( GL_TEXTURE_2D, liftTexture );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RG8, ends, picks, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, 0 );
		liftEnds  = ends;
		liftPicks = picks;
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );

	//---------------------------------------------------------------------
	// 1. Cells: one linear mean per crossing.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( cells.GetGLID(), ScopedFBOBinding::RB_REVERT );
		cells.ResizeViewPort();
		ScopedShaderBinding shader( cellsShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		cellsShader.Set( "InputTexture", 0 );
		glUniform2i( cellsShader.FindUniform( "InputSize" ), width, height );
		glUniform2i( cellsShader.FindUniform( "Grid" ), ends, picks );
		quad.Draw();
	}

	//Read them back: at most 320 x 320 x 4 floats. The one stall in the
	//plugin, and the price of an encoder that is exact.
	means.resize( static_cast< size_t >( ends ) * picks * 4 );
	glBindTexture( GL_TEXTURE_2D, cells.TextureID() );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, means.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );

	//---------------------------------------------------------------------
	// 2. The shuttles, and the loom. CPU.
	//---------------------------------------------------------------------
	const auto cpuStart = std::chrono::steady_clock::now();

	const bool resized = ( width != lastWidth || height != lastHeight ) && lastWidth != 0;
	lastWidth          = width;
	lastHeight         = height;
	if( source != lastPaletteSource || ( resized && ( perturb & weave::kPerturbColdResize ) ) )
		kmeans.Reset();
	lastPaletteSource = source;

	shuttles.resize( static_cast< size_t >( shuttleCount ) );
	if( source == palette::kClip )
	{
		const int count = ends * picks;
		kmeans.Update( means.data(), count, shuttleCount, ( count + kPaletteSamples - 1 ) / kPaletteSamples );
		shuttles = kmeans.Centres();
	}
	else
		palette::Fixed( source, shuttleCount, shuttles.data() );

	loom::Settings settings;
	settings.ends            = ends;
	settings.picks           = picks;
	settings.warp            = warp;
	settings.shuttles        = shuttles.data();
	settings.shuttleCount    = static_cast< int >( shuttles.size() );
	settings.maxFloat        = maxFloat;
	settings.mode            = mode;
	settings.forcedStructure = forcedStructure;
	settings.perturb         = perturb;
	loom::Weave( means.data(), settings, woven, &cloth );
	std::swap( cloth, woven );

	cpuMilliseconds = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - cpuStart ).count();

	uploadLift();

	//---------------------------------------------------------------------
	// 3. Render, straight to the host.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( renderShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding liftBinding( liftTexture );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding sourceBinding( input.Handle );

		renderShader.Set( "Lift", 0 );
		renderShader.Set( "Source", 1 );
		renderShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		glUniform2i( renderShader.FindUniform( "OutSize" ), width, height );
		glUniform2i( renderShader.FindUniform( "Grid" ), ends, picks );
		renderShader.Set( "Warp", warp.r, warp.g, warp.b );
		float packedShuttles[ palette::kMaxShuttles * 3 ] = {};
		for( size_t k = 0; k < shuttles.size() && k < static_cast< size_t >( palette::kMaxShuttles ); ++k )
		{
			packedShuttles[ k * 3 + 0 ] = shuttles[ k ].r;
			packedShuttles[ k * 3 + 1 ] = shuttles[ k ].g;
			packedShuttles[ k * 3 + 2 ] = shuttles[ k ].b;
		}
		glUniform3fv( renderShader.FindUniform( "Shuttle" ), palette::kMaxShuttles, packedShuttles );
		renderShader.Set( "Shuttles", static_cast< int >( shuttles.size() ) );
		renderShader.Set( "Zoom", zoom );
		renderShader.Set( "Shading", shading );
		renderShader.Set( "MixAmount", mixAmount );
		renderShader.Set( "Perturb", perturb );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Jacquard::DeInitGL()
{
	cellsShader.FreeGLResources();
	renderShader.FreeGLResources();
	quad.Release();
	cells.Destroy();
	if( liftTexture != 0 )
	{
		glDeleteTextures( 1, &liftTexture );
		liftTexture = 0;
	}
	liftEnds = liftPicks = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Jacquard::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Jacquard::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Jacquard::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Jacquard::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}
