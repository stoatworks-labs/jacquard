#pragma once

/**
	Host parameters are 0..1 (sliders), real integers (FF_TYPE_INTEGER) or
	element indices (options); these are what they mean.

	`SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange`
	can widen it, so the counts -- Ends, Picks, Max Float, Shuttles -- are
	FF_TYPE_INTEGER, which is exempt and holds its real value. An option's
	range reads back 0..1 from the SDK whatever its element count, so options
	are mapped by INDEX here and nowhere else.
*/
namespace jacquard::controls
{

int OptionIndex( float value, int count );

/// Ends: warp threads across the frame.
constexpr int kEndsMin     = 16;
constexpr int kEndsMax     = 320;
constexpr int kEndsDefault = 160;
int Ends( float value );

/// Picks: weft rows down the frame. 0 is Auto: as many as make each
/// crossing Thread Aspect times as tall as it is wide.
constexpr int kPicksMax = 320;
int Picks( float value, int ends, int width, int height, double aspect );

/// Thread Aspect: a pick's height over an end's width, 0.5..2 on a log
/// scale; the default 0.5 is 1 (square crossings).
double ThreadAspect( float value );

/// Max Float: the longest run of crossings any thread may make on one side.
constexpr int kMaxFloatMin     = 2;
constexpr int kMaxFloatMax     = 16;
constexpr int kMaxFloatDefault = 10;
int MaxFloat( float value );

/// Shuttles: weft colours loaded.
constexpr int kShuttlesMin     = 2;
constexpr int kShuttlesMax     = 8;
constexpr int kShuttlesDefault = 6;
int Shuttles( float value );

/// Zoom: 1..8 on a log scale, about the frame's centre.
double Zoom( float value );

} // namespace jacquard::controls
