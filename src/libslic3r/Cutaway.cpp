#include "Cutaway.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"

#include <algorithm>
#include <limits>

namespace Slic3r {

namespace {

struct PolylinePosition
{
    // Distance from the start of the polyline.
    double distance;
    // Direction of the polyline segment there.
    Vec2d  direction;
};

// Finds the point of polyline closest to pt.
PolylinePosition closest_position(const Polyline &polyline, const Vec2d &pt)
{
    PolylinePosition out { 0., Vec2d::Zero() };
    double           min_dist2 = std::numeric_limits<double>::max();
    double           distance  = 0.;
    for (size_t i = 1; i < polyline.points.size(); ++ i) {
        const Vec2d  a      = polyline.points[i - 1].cast<double>();
        const Vec2d  v      = polyline.points[i].cast<double>() - a;
        const double length = v.norm();
        const double t      = length > 0. ? std::clamp((pt - a).dot(v) / (length * length), 0., 1.) : 0.;
        if (const double dist2 = (a + t * v - pt).squaredNorm(); dist2 < min_dist2) {
            min_dist2 = dist2;
            out       = { distance + t * length, v };
        }
        distance += length;
    }
    return out;
}

// Parts of polyline outside of mask. Clipper returns them in an arbitrary order and orientation, put them back
// into the order and orientation of polyline, so that the pieces of loops and of multi-paths chain up again.
Polylines diff_pl_ordered(const Polyline &polyline, const Polygons &mask)
{
    Polylines pieces = diff_pl(polyline, mask);
    std::vector<std::pair<double, size_t>> order;
    order.reserve(pieces.size());
    for (size_t i = 0; i < pieces.size(); ++ i) {
        Polyline &piece = pieces[i];
        if (piece.size() < 2)
            continue;
        // Locate the piece by the middle of its first segment: unlike its end points, it is unambiguous on a closed polyline.
        auto first_segment_middle = [&piece]() { return Vec2d(0.5 * (piece.points[0].cast<double>() + piece.points[1].cast<double>())); };
        PolylinePosition position = closest_position(polyline, first_segment_middle());
        if ((piece.points[1] - piece.points[0]).cast<double>().dot(position.direction) < 0.) {
            piece.reverse();
            position = closest_position(polyline, first_segment_middle());
        }
        order.emplace_back(position.distance, i);
    }
    std::sort(order.begin(), order.end());
    Polylines out;
    out.reserve(order.size());
    for (const std::pair<double, size_t> &o : order)
        out.emplace_back(std::move(pieces[o.second]));
    return out;
}

bool was_clipped(const Polyline &source, const Polylines &pieces)
{
    return pieces.size() != 1 || pieces.front().length() < source.length() - SCALED_EPSILON;
}

double total_length(const ExtrusionPaths &paths)
{
    double length = 0.;
    for (const ExtrusionPath &path : paths)
        length += path.length();
    return length;
}

// Clips the paths of a multi-path or of a loop into the continuous runs left. Returns false if nothing was clipped.
bool clip_paths(const ExtrusionPaths &paths, bool loop, const Polygons &mask, std::vector<ExtrusionPaths> &runs)
{
    ExtrusionPaths pieces;
    bool           clipped = false;
    for (const ExtrusionPath &path : paths) {
        Polylines left = diff_pl_ordered(path.polyline, mask);
        clipped |= was_clipped(path.polyline, left);
        for (Polyline &polyline : left)
            pieces.emplace_back(std::move(polyline), path);
    }
    if (! clipped)
        return false;
    for (ExtrusionPath &piece : pieces) {
        if (runs.empty() || runs.back().back().last_point() != piece.first_point())
            runs.emplace_back();
        runs.back().emplace_back(std::move(piece));
    }
    // A loop not cut at its start point continues from its last run into its first one.
    if (loop && runs.size() > 1 && runs.back().back().last_point() == runs.front().front().first_point()) {
        append(runs.back(), std::move(runs.front()));
        runs.erase(runs.begin());
    }
    return true;
}

// Clips a path, a multi-path or a loop. Returns false if nothing was clipped, otherwise appends what is left to out.
bool clip_entity(const ExtrusionEntity &entity, const Polygons &mask, ExtrusionEntitiesPtr &out)
{
    if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        Polylines left = diff_pl_ordered(path->polyline, mask);
        if (! was_clipped(path->polyline, left))
            return false;
        for (Polyline &polyline : left)
            // Drop the slivers left at the edges of the cut.
            if (polyline.length() >= scale_(path->width)) {
                // Clone to keep the type of the path.
                auto *piece     = static_cast<ExtrusionPath*>(path->clone());
                piece->polyline = std::move(polyline);
                out.emplace_back(piece);
            }
        return true;
    }

    std::vector<ExtrusionPaths> runs;
    if (const auto *multi_path = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        if (! clip_paths(multi_path->paths, false, mask, runs))
            return false;
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        if (! clip_paths(loop->paths, true, mask, runs))
            return false;
    } else
        return false;

    for (ExtrusionPaths &run : runs) {
        if (total_length(run) < scale_(run.front().width))
            continue;
        ExtrusionEntity *piece;
        if (run.size() == 1)
            piece = new ExtrusionPath(std::move(run.front()));
        else {
            auto *multi_path  = new ExtrusionMultiPath();
            multi_path->paths = std::move(run);
            piece             = multi_path;
        }
        piece->set_customize_flag(entity.get_customize_flag());
        piece->set_cooling_node(entity.get_cooling_node());
        out.emplace_back(piece);
    }
    return true;
}

BoundingBox extrusion_extents(const ExtrusionEntity &entity)
{
    Points points;
    entity.collect_points(points);
    return points.empty() ? BoundingBox() : BoundingBox(points);
}

void clip_collection(ExtrusionEntityCollection &collection, const Polygons &mask, const BoundingBox &mask_bbox)
{
    ExtrusionEntitiesPtr out;
    out.reserve(collection.entities.size());
    for (ExtrusionEntity *entity : collection.entities) {
        if (! mask_bbox.overlap(extrusion_extents(*entity)))
            out.emplace_back(entity);
        else if (entity->is_collection()) {
            auto *sub_collection = static_cast<ExtrusionEntityCollection*>(entity);
            clip_collection(*sub_collection, mask, mask_bbox);
            if (sub_collection->entities.empty())
                delete sub_collection;
            else
                out.emplace_back(sub_collection);
        } else if (clip_entity(*entity, mask, out))
            delete entity;
        else
            out.emplace_back(entity);
    }
    collection.entities = std::move(out);
}

} // namespace

void clip_extrusions_by_cutaway(ExtrusionEntityCollection &extrusions, const Polygons &mask)
{
    if (! mask.empty())
        clip_collection(extrusions, mask, get_extents(mask));
}

} // namespace Slic3r
