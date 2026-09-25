#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace jacquard::controls
{

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

int Ends( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), kEndsMin, kEndsMax );
}

int Picks( float value, int ends, int width, int height, double aspect )
{
	const int explicitPicks = static_cast< int >( std::lround( value ) );
	if( explicitPicks > 0 )
		return std::min( explicitPicks, kPicksMax );
	if( width <= 0 || height <= 0 )
		return 1;
	//An end is width / ends pixels wide; a pick aspect times that tall.
	const double picks = static_cast< double >( height ) * ends / ( static_cast< double >( width ) * aspect );
	return std::clamp( static_cast< int >( std::lround( picks ) ), 1, kPicksMax );
}

double ThreadAspect( float value )
{
	return 0.5 * std::pow( 4.0, std::clamp( static_cast< double >( value ), 0.0, 1.0 ) );
}

int MaxFloat( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), kMaxFloatMin, kMaxFloatMax );
}

int Shuttles( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), kShuttlesMin, kShuttlesMax );
}

double Zoom( float value )
{
	return std::pow( 8.0, std::clamp( static_cast< double >( value ), 0.0, 1.0 ) );
}

} // namespace jacquard::controls
