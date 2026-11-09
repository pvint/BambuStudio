#ifndef slic3r_Cutaway_hpp_
#define slic3r_Cutaway_hpp_

#include "Polygon.hpp"

namespace Slic3r {

class ExtrusionEntityCollection;

// A cutaway (ModelVolumeType::CUTAWAY) removes what would be extruded inside of it, like a negative volume does,
// but it does not wall off the cut: the walls, shells and infill end open at the cut, exposing the inside of the object
// to drain water or to let air through, for example.
//
// Removes the parts of the extrusions lying inside of mask. What is left keeps the order and the direction of the source,
// loops cut open become open paths. Collections left empty are removed.
void clip_extrusions_by_cutaway(ExtrusionEntityCollection &extrusions, const Polygons &mask);

} // namespace Slic3r

#endif // slic3r_Cutaway_hpp_
