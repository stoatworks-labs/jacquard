#pragma once

#include "Loom.h"
#include "Palette.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include "StoatworksAboutParams.h"

#include <string>
#include <vector>

/**
	Jacquard -- the picture woven on a jacquard loom, as an FFGL effect.

	**The one idea.** At every crossing of a woven cloth one of two threads is
	on top: the warp, running down, or the weft, running across. A jacquard
	head lifts each warp end on its own, so any picture can be woven -- but
	under three constraints that make a woven picture look woven. Colour comes
	from threads, not pixels: one warp colour, and one weft colour per pick
	from the shuttles loaded. Floats must be tied down: no thread may pass
	over or under more than Max Float crossings in a row. And tone is carried
	by weave structure -- how much weft a satin, a twill or a plain weave
	shows. So the encoder is an optimiser under constraints: per pick, the weft
	colour and the lift pattern that best reproduce the picture while no float
	runs too long, solved exactly (`Encoder.h`), pick by pick down the cloth
	(`Loom.h`).

	Two GPU passes and a CPU stage (`Shaders.h`): the cells pass reduces the
	source to one linear mean per crossing, which is read back; the palette and
	the loom run on the CPU and write an Ends x Picks lift texture; the render
	pass draws the cloth as lit thread. See AGENTS.md.
*/
class Jacquard : public CFFGLPlugin
{
public:
	Jacquard();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks: always at rest in the plugin ------------------------
	/// A bitmask of `weave::Perturb`, for the negative controls.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}
	/// Every crossing asks for this structure (-1: tone decides).
	void SetStructureForTest( int structure )
	{
		forcedStructure = structure;
	}
	const jacquard::loom::Cloth& ClothForTest() const
	{
		return cloth;
	}
	/// The shuttles the last frame was woven with, linear.
	const std::vector< jacquard::palette::Colour >& ShuttlesForTest() const
	{
		return shuttles;
	}
	jacquard::palette::Colour WarpForTest() const
	{
		return warp;
	}
	const std::vector< float >& MeansForTest() const
	{
		return means;
	}
	const jacquard::palette::KMeans& KMeansForTest() const
	{
		return kmeans;
	}
	/// Milliseconds of CPU the last frame spent on the palette and the loom.
	double CpuMillisecondsForTest() const
	{
		return cpuMilliseconds;
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Loom
		PT_ENDS,
		PT_PICKS,
		PT_THREAD_ASPECT,
		PT_MAX_FLOAT,

		//Threads
		PT_WARP_R,
		PT_WARP_G,
		PT_WARP_B,
		PT_SHUTTLES,
		PT_PALETTE,
		PT_STRUCTURE,

		//Look
		PT_SHADING,
		PT_ZOOM,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	void uploadLift();

	ffglex::FFGLShader cellsShader;
	ffglex::FFGLShader renderShader;
	ffglex::FFGLScreenQuad quad;

	jacquard::PassBuffer cells;///< Ends x Picks linear means, RGBA32F
	GLuint liftTexture = 0;    ///< Ends x Picks RG8: warp on top, shuttle index
	int liftEnds       = 0;
	int liftPicks      = 0;

	std::vector< float > means;
	std::vector< uint8_t > liftBytes;
	std::vector< jacquard::palette::Colour > shuttles;
	jacquard::palette::Colour warp;
	jacquard::palette::KMeans kmeans;
	int lastPaletteSource = -1;
	int lastWidth = 0, lastHeight = 0;
	jacquard::loom::Cloth cloth; ///< the last frame's, which the next remembers
	jacquard::loom::Cloth woven; ///< the one being woven

	int perturb         = 0;
	int forcedStructure = -1;
	double cpuMilliseconds = 0.0;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
