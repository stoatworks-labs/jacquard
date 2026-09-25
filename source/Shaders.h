#pragma once

#include <string>

/**
	The two passes.

	1. **cells** -- Ends x Picks, RGBA32F, one fragment per crossing. The
	   mean of the source pixels whose centres fall in the crossing's
	   rectangle, in linear light, strided past 16 taps a side. Read back to
	   the CPU, where the loom runs (`Loom.h`): the row programme is serial and
	   its exactness is an integer claim, so it is not a shader.

	2. **render** -- the host's framebuffer. For each output pixel, through
	   Zoom about the centre, the crossing it lands in and which thread is on
	   top there; that thread's colour (the warp colour, or the pick's
	   shuttle); and, scaled by Thread Shading, the thread drawn as a lit
	   cylinder: its cross-section across the thread, a dip where it passes
	   under at the end of a float, the ply's twist, a sheen along it, and the
	   other thread showing in the gap between two. Thread Shading fades out
	   below a few pixels a crossing, where the profile could only alias.
	   Output alpha is 1 where the cloth is (Mix blends the whole RGBA): a
	   cloth is opaque.

	`jqtest --dump-shaders DIR` writes exactly these strings, which is what
	`tools/verify.sh` hands to glslc.
*/
namespace jacquard::shaders
{

std::string Vertex();
std::string Cells();
std::string Render();

} // namespace jacquard::shaders
