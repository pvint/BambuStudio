#ifndef slic3r_FillConcentricInsideOut_hpp_
#define slic3r_FillConcentricInsideOut_hpp_

#include "FillConcentric.hpp"

namespace Slic3r {

// Loops like FillConcentric, but grown only from the shore: from where the region meets other regions (the land).
// Where the region reaches the outline of the object, the loops run on until they are cut off there, instead of
// new loops starting from the outline and meeting the shore loops in a crease. The loops are printed from the
// middle outwards, so the last line laid down meets a wall instead of being squeezed in between two fronts.
class FillConcentricInsideOut : public FillConcentric
{
public:
    ~FillConcentricInsideOut() override = default;

protected:
    Fill* clone() const override { return new FillConcentricInsideOut(*this); };
	void _fill_surface_single(
	    const FillParams                &params,
	    unsigned int                     thickness_layers,
	    const std::pair<float, Point>   &direction,
	    ExPolygon     		             expolygon,
	    Polylines                       &polylines_out) override;

	void _fill_surface_single(const FillParams& params,
		unsigned int                   thickness_layers,
		const std::pair<float, Point>& direction,
		ExPolygon                      expolygon,
		ThickPolylines& thick_polylines_out) override;

	// The whole object on this layer, and the parts of it that belong to other regions. Set by Layer::make_fills().
	const ExPolygons* object_slices = nullptr;
	ExPolygons        land;

	friend class Layer;
};

} // namespace Slic3r

#endif // slic3r_FillConcentricInsideOut_hpp_
