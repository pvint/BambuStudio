#include <algorithm>
#include <limits>

#include "../AABBTreeLines.hpp"
#include "../ClipperUtils.hpp"
#include "../ClipperZUtils.hpp"
#include "../ExPolygon.hpp"
#include "../VariableWidth.hpp"
#include "Arachne/WallToolPaths.hpp"
#include "Arachne/utils/SparsePointGrid.hpp"

#include "FillConcentricInsideOut.hpp"

namespace Slic3r {

namespace {

// A line to print: a whole Arachne line, or a part of one that was cut off by the outline of the fill area.
struct Piece
{
    ThickPolyline path;
    size_t        inset_idx;
    bool          closed;
};

coordf_t width_at(const ThickPolyline &path, size_t point_idx)
{
    return point_idx + 1 < path.points.size() ? path.width[2 * point_idx] : path.width.back();
}

// The fill area grown past where it reaches the outline of the object rather than the land, so far that Arachne's
// loops starting from the outline of the grown area meet the shore loops outside the object: inside the fill area,
// all loops then follow the shore. Empty if there is nothing to grow: no land, or the fill area doesn't reach the
// outline of the object.
ExPolygons grow_past_object_outline(const ExPolygon &expolygon, const ExPolygons &object_slices, const ExPolygons &land, coord_t spacing)
{
    if (land.empty() || object_slices.empty())
        return {};
    const AABBTreeLines::LinesDistancer<Line> to_land(to_lines(land));
    const AABBTreeLines::LinesDistancer<Line> to_fill_area(to_lines(expolygon));

    // How far the fill area keeps off the land, i.e. the walls along the shore: the median distance of its vertices
    // from the land, leaving out those far from any land.
    std::vector<double> shore_distances;
    auto add_shore_distances = [&](const Points &points) {
        for (const Point &p : points)
            if (double distance = to_land.distance_from_lines<false>(p); distance < 10. * spacing)
                shore_distances.emplace_back(distance);
    };
    add_shore_distances(expolygon.contour.points);
    for (const Polygon &hole : expolygon.holes)
        add_shore_distances(hole.points);
    if (shore_distances.empty())
        return {};
    std::nth_element(shore_distances.begin(), shore_distances.begin() + shore_distances.size() / 2, shore_distances.end());
    // The walls along the outline of the object are as wide as those along the shore.
    const coord_t edge_band = coord_t(shore_distances[shore_distances.size() / 2]) + spacing + spacing / 2;

    // The farthest from the land that the outline of the object gets next to the fill area.
    double max_edge_distance = -1.;
    for (const ExPolygon &slice : object_slices)
        for (const Line &line : to_lines(slice)) {
            const Vec2d  v     = (line.b - line.a).cast<double>();
            const size_t steps = size_t(v.norm() / spacing) + 1;
            for (size_t s = 0; s < steps; ++ s) {
                const Point q = line.a + (v * (double(s) / double(steps))).cast<coord_t>();
                if (to_fill_area.distance_from_lines<false>(q) <= edge_band)
                    max_edge_distance = std::max(max_edge_distance, to_land.distance_from_lines<false>(q));
            }
        }
    if (max_edge_distance < 0.)
        return {};
    // Loops from the outline of the grown area meet the shore loops halfway, beyond the outline of the object.
    const coord_t reach = coord_t(max_edge_distance) + 2 * spacing;

    // Only the neighbourhood of the fill area, but far enough away that the cut can't start loops in the fill area:
    // nothing in there is farther from the land than its size plus the walls.
    BoundingBox bbox = get_extents(expolygon);
    bbox.offset(coordf_t(reach) + (bbox.max - bbox.min).cast<double>().norm() + 10. * spacing);
    ExPolygons outside    = intersection_ex(diff_ex(offset_ex(object_slices, float(reach)), object_slices), Polygons{ bbox.polygon() });
    // The walls along the outline of the object, connecting the fill area to the outside.
    ExPolygons edge_walls = diff_ex(diff_ex(offset_ex(expolygon, float(edge_band)), land), offset_ex(object_slices, - float(edge_band)));

    ExPolygons all { expolygon };
    append(all, std::move(outside));
    append(all, std::move(edge_walls));
    ExPolygons grown;
    for (ExPolygon &part : union_ex(all))
        if (! intersection_ex(ExPolygons{ part }, ExPolygons{ expolygon }).empty())
            grown.emplace_back(std::move(part));
    return grown;
}

// The parts of the lines inside the fill area, with their widths.
std::vector<Piece> cut_to_fill_area(const std::vector<const Arachne::ExtrusionLine*> &lines, const ExPolygon &fill_area, coord_t spacing)
{
    using ClipperZUtils::ZPoint;
    using ClipperZUtils::ZPath;
    using ClipperZUtils::ZPaths;

    // Z is 32 bit, too small to carry a line and a width. Instead the vertices of the lines carry the index of their
    // junction + 1, those of the fill area 0, and the points where a line is cut -1 - the index of the cut.
    struct Junction
    {
        coordf_t w;
        size_t   line_idx;
    };
    struct Cut
    {
        coord_t from;
        coord_t to;
        double  t;
    };
    std::vector<Junction> junctions(1);
    std::vector<Cut>      cuts;

    ZPaths subject;
    subject.reserve(lines.size());
    for (size_t line_idx = 0; line_idx < lines.size(); ++ line_idx) {
        ZPath &path = subject.emplace_back();
        for (const Arachne::ExtrusionJunction &junction : *lines[line_idx]) {
            path.emplace_back(junction.p.x(), junction.p.y(), coord_t(junctions.size()));
            junctions.push_back({ coordf_t(junction.w), line_idx });
        }
        if (lines[line_idx]->is_closed && (path.front().x() != path.back().x() || path.front().y() != path.back().y()))
            path.emplace_back(path.front());
    }
    // A little slack, so that the lines along the shore, which lie on the outline of the fill area, are kept whole.
    ZPaths clip = ClipperZUtils::expolygons_to_zpaths_with_same_z<false>(offset_ex(fill_area, float(spacing) / 8.f), coord_t(0));

    ClipperLib_Z::Clipper clipper;
    clipper.ZFillFunction([&cuts](const ZPoint &e1bot, const ZPoint &e1top, const ZPoint &e2bot, const ZPoint &e2top, ZPoint &pt) {
        // Where along the edge of the line the cut is. The other edge is usually one of the fill area.
        const bool    e1_is_line = e1bot.z() > 0 && e1top.z() > 0;
        const ZPoint &start      = e1_is_line ? e1bot : e2bot;
        const ZPoint &end        = e1_is_line ? e1top : e2top;
        if (start.z() <= 0 || end.z() <= 0) {
            pt.z() = 0;
            return;
        }
        const double dx      = double(end.x() - start.x());
        const double dy      = double(end.y() - start.y());
        const double length2 = dx * dx + dy * dy;
        const double t       = length2 > 0. ? std::clamp((double(pt.x() - start.x()) * dx + double(pt.y() - start.y()) * dy) / length2, 0., 1.) : 0.;
        cuts.push_back({ start.z(), end.z(), t });
        pt.z() = - coord_t(cuts.size());
    });
    clipper.AddPaths(subject, ClipperLib_Z::ptSubject, false);
    clipper.AddPaths(clip, ClipperLib_Z::ptClip, true);
    ClipperLib_Z::PolyTree tree;
    clipper.Execute(ClipperLib_Z::ctIntersection, tree, ClipperLib_Z::pftNonZero, ClipperLib_Z::pftNonZero);
    ZPaths clipped;
    ClipperLib_Z::PolyTreeToPaths(tree, clipped);

    auto line_of  = [&](const ZPoint &p) { return junctions[p.z() > 0 ? p.z() : cuts[- p.z() - 1].from].line_idx; };
    auto width_of = [&](const ZPoint &p) {
        if (p.z() > 0)
            return junctions[p.z()].w;
        const Cut &cut = cuts[- p.z() - 1];
        return junctions[cut.from].w + (junctions[cut.to].w - junctions[cut.from].w) * cut.t;
    };
    std::vector<std::vector<ZPath>> by_line(lines.size());
    for (ZPath &path : clipped) {
        // Vertices of the fill area (Z = 0) turn up where a line runs along its outline: give them the Z of a neighbour.
        for (size_t i = 1; i < path.size(); ++ i)
            if (path[i].z() == 0)
                path[i].z() = path[i - 1].z();
        for (size_t i = path.size(); i > 1; -- i)
            if (path[i - 2].z() == 0)
                path[i - 2].z() = path[i - 1].z();
        if (path.size() >= 2 && path.front().z() != 0)
            by_line[line_of(path.front())].emplace_back(std::move(path));
    }

    // Clipper may split a line where it wasn't cut: join up the pieces of a line that meet end to end.
    auto same_xy = [](const ZPoint &a, const ZPoint &b) { return a.x() == b.x() && a.y() == b.y(); };
    auto join    = [&same_xy](ZPath &a, ZPath &b) {
        if (same_xy(a.front(), a.back()))
            return false;
        if (same_xy(a.front(), b.front()) || same_xy(a.front(), b.back()))
            std::reverse(a.begin(), a.end());
        if (same_xy(a.back(), b.back()))
            std::reverse(b.begin(), b.end());
        if (! same_xy(a.back(), b.front()))
            return false;
        a.insert(a.end(), b.begin() + 1, b.end());
        return true;
    };
    std::vector<Piece> pieces;
    for (size_t line_idx = 0; line_idx < lines.size(); ++ line_idx) {
        std::vector<ZPath> &paths = by_line[line_idx];
        for (size_t i = 0; i < paths.size(); ++ i)
            for (size_t j = i + 1; j < paths.size();)
                if (join(paths[i], paths[j])) {
                    paths.erase(paths.begin() + j);
                    j = i + 1;
                } else
                    ++ j;
        for (const ZPath &path : paths) {
            Piece piece;
            piece.inset_idx = lines[line_idx]->inset_idx;
            piece.closed    = lines[line_idx]->is_closed && same_xy(path.front(), path.back());
            for (size_t i = 0; i < path.size(); ++ i) {
                piece.path.points.emplace_back(path[i].x(), path[i].y());
                if (i > 0) {
                    piece.path.width.emplace_back(width_of(path[i - 1]));
                    piece.path.width.emplace_back(width_of(path[i]));
                }
            }
            // Slivers where a line just grazes the fill area.
            if (piece.path.length() >= spacing / 2)
                pieces.emplace_back(std::move(piece));
        }
    }
    return pieces;
}

std::vector<Piece> whole_lines(const std::vector<const Arachne::ExtrusionLine*> &lines)
{
    std::vector<Piece> pieces;
    pieces.reserve(lines.size());
    for (const Arachne::ExtrusionLine *line : lines) {
        Piece &piece    = pieces.emplace_back();
        piece.path      = Arachne::to_thick_polyline(*line);
        piece.inset_idx = line->inset_idx;
        piece.closed    = line->is_closed && piece.path.points.front() == piece.path.points.back();
    }
    return pieces;
}

// Order constraints between neighbouring pieces, as (inner, outer) index pairs: the inner piece, whose inset index is
// one higher, has to be printed first. Same neighbour test as Arachne::WallToolPaths::getRegionOrder(), which can't
// be used here because it always puts the odd middle lines last, and those are the lines that have to go first.
std::vector<std::pair<size_t, size_t>> inside_out_constraints(const std::vector<Piece> &pieces)
{
    std::vector<std::pair<size_t, size_t>> constraints;

    coordf_t max_line_w = 0;
    for (const Piece &piece : pieces)
        for (coordf_t w : piece.path.width)
            max_line_w = std::max(max_line_w, w);
    if (max_line_w <= 0)
        return constraints;

    struct LineLoc
    {
        Point   p;
        coord_t w;
        size_t  piece_idx;
    };
    struct Locator
    {
        Point operator()(const LineLoc &elem) { return elem.p; }
    };

    // Sample the pieces at least every max_line_w: two neighbouring loops along a long straight stretch
    // may have no vertices close to each other.
    std::vector<LineLoc> samples;
    for (size_t piece_idx = 0; piece_idx < pieces.size(); ++ piece_idx) {
        const ThickPolyline &path = pieces[piece_idx].path;
        for (size_t i = 0; i < path.points.size(); ++ i) {
            const Point   &a  = path.points[i];
            const coordf_t wa = width_at(path, i);
            samples.push_back({ a, coord_t(wa), piece_idx });
            if (i + 1 == path.points.size())
                break;
            const Point   &b     = path.points[i + 1];
            const coordf_t wb    = width_at(path, i + 1);
            const Vec2d    v     = (b - a).cast<double>();
            const int      steps = int(v.norm() / max_line_w);
            for (int s = 1; s <= steps; ++ s) {
                const double t = double(s) / double(steps + 1);
                samples.push_back({ Point(coord_t(a.x() + t * v.x()), coord_t(a.y() + t * v.y())), coord_t(wa + t * (wb - wa)), piece_idx });
            }
        }
    }

    constexpr double diagonal_extension = 1.9;
    const coord_t    searching_radius   = coord_t(max_line_w * diagonal_extension);
    Arachne::SparsePointGrid<LineLoc, Locator> grid(searching_radius, samples.size());
    for (const LineLoc &sample : samples)
        grid.insert(sample);

    for (const LineLoc &here : samples) {
        const size_t inset_here = pieces[here.piece_idx].inset_idx;
        for (const LineLoc &nearby : grid.getNearby(here.p, searching_radius))
            // Look only from the inner piece, so that each pair is recorded one way round.
            if (pieces[nearby.piece_idx].inset_idx + 1 == inset_here &&
                shorter_then(here.p - nearby.p, coord_t((here.w + nearby.w) / 2 * diagonal_extension)))
                constraints.emplace_back(here.piece_idx, nearby.piece_idx);
    }
    sort_remove_duplicates(constraints);
    return constraints;
}

} // namespace

void FillConcentricInsideOut::_fill_surface_single(
    const FillParams                &params,
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction,
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    // FillConcentric chains its loops from the outermost to the innermost.
    // Walking that chain backwards prints the same loops inside out, with the same travel moves.
    Polylines polylines;
    FillConcentric::_fill_surface_single(params, thickness_layers, direction, std::move(expolygon), polylines);
    for (auto it = polylines.rbegin(); it != polylines.rend(); ++ it) {
        it->reverse();
        polylines_out.emplace_back(std::move(*it));
    }
}

void FillConcentricInsideOut::_fill_surface_single(const FillParams& params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point>& direction,
    ExPolygon                      expolygon,
    ThickPolylines& thick_polylines_out)
{
    assert(params.use_arachne);
    assert(this->print_config != nullptr && this->print_object_config != nullptr);

    // no rotation is supported for this infill pattern
    coord_t min_spacing = scaled<coord_t>(this->spacing);

    if (params.density > 0.9999f && !params.dont_adjust) {
        // Arachne runs on the fill area grown past the outline of the object, and its lines are cut back to the fill area.
        ExPolygons grown;
        if (this->object_slices != nullptr)
            grown = grow_past_object_outline(expolygon, *this->object_slices, this->land, min_spacing);
        const bool cut = ! grown.empty();
        if (! cut)
            grown = { expolygon };

        // Same Arachne setup as FillConcentric.
        Point                  bbox_size = get_extents(grown).size();
        coord_t                loops_count = std::max(bbox_size.x(), bbox_size.y()) / min_spacing + 1;
        Polygons               polygons = offset(grown, float(min_spacing) / 2.f);

        double min_nozzle_diameter = *std::min_element(print_config->nozzle_diameter.values.begin(), print_config->nozzle_diameter.values.end());
        Arachne::WallToolPathsParams input_params;
        input_params.min_bead_width = 0.85 * min_nozzle_diameter;
        input_params.min_feature_size = 0.25 * min_nozzle_diameter;
        input_params.wall_transition_length = 1.0 * min_nozzle_diameter;
        input_params.wall_transition_angle = 10;
        input_params.wall_transition_filter_deviation = 0.25 * min_nozzle_diameter;
        input_params.wall_distribution_count = 1;

        Arachne::WallToolPaths wallToolPaths(polygons, min_spacing, min_spacing, loops_count, 0, params.layer_height, input_params);

        std::vector<Arachne::VariableWidthLines>    loops = wallToolPaths.getToolPaths();
        std::vector<const Arachne::ExtrusionLine*> all_extrusions;
        for (Arachne::VariableWidthLines& loop : loops)
            for (const Arachne::ExtrusionLine& wall : loop)
                if (wall.size() >= 2)
                    all_extrusions.emplace_back(&wall);

        std::vector<Piece> pieces = cut ? cut_to_fill_area(all_extrusions, expolygon, min_spacing) : whole_lines(all_extrusions);

        // A piece becomes available once all its inner neighbours have been printed.
        std::vector<size_t>              blocked(pieces.size(), 0);
        std::vector<std::vector<size_t>> blocking(pieces.size());
        for (const auto &[inner, outer] : inside_out_constraints(pieces)) {
            ++ blocked[outer];
            blocking[inner].emplace_back(outer);
        }
        std::vector<size_t> available;
        for (size_t idx = 0; idx < pieces.size(); ++ idx)
            if (blocked[idx] == 0)
                available.emplace_back(idx);

        std::vector<BoundingBox> bboxes;
        bboxes.reserve(pieces.size());
        for (const Piece& piece : pieces)
            bboxes.emplace_back(piece.path.points);
        // Lower bound of entry_distance2(), to skip the vertex scan for pieces that are far away.
        auto bbox_distance2 = [&bboxes](size_t idx, const Point& pos) {
            const BoundingBox& bbox = bboxes[idx];
            double dx = std::max({ double(bbox.min.x() - pos.x()), 0., double(pos.x() - bbox.max.x()) });
            double dy = std::max({ double(bbox.min.y() - pos.y()), 0., double(pos.y() - bbox.max.y()) });
            return dx * dx + dy * dy;
        };
        // Squared distance from pos to where a piece would be entered: its nearest vertex if closed, otherwise its nearer end.
        auto entry_distance2 = [&pieces](size_t idx, const Point& pos) {
            const Piece& piece = pieces[idx];
            if (! piece.closed)
                return std::min((piece.path.first_point() - pos).cast<double>().squaredNorm(), (piece.path.last_point() - pos).cast<double>().squaredNorm());
            double best = std::numeric_limits<double>::max();
            for (const Point& p : piece.path.points)
                best = std::min(best, (p - pos).cast<double>().squaredNorm());
            return best;
        };

        size_t            first_poly_idx = thick_polylines_out.size();
        std::vector<bool> closed_out;
        Point             last_pos(0, 0);
        bool              first = true;
        while (! available.empty()) {
            size_t best_i = 0;
            if (first) {
                // Start in the middle of the widest part of the region.
                for (size_t i = 1; i < available.size(); ++ i)
                    if (pieces[available[i]].inset_idx > pieces[available[best_i]].inset_idx)
                        best_i = i;
                first = false;
            } else {
                // Then always the nearest available piece.
                double best_distance2 = std::numeric_limits<double>::max();
                for (size_t i = 0; i < available.size(); ++ i)
                    if (bbox_distance2(available[i], last_pos) < best_distance2) {
                        double distance2 = entry_distance2(available[i], last_pos);
                        if (distance2 < best_distance2) {
                            best_distance2 = distance2;
                            best_i         = i;
                        }
                    }
            }
            const size_t best = available[best_i];
            available[best_i] = available.back();
            available.pop_back();
            for (size_t unblocked : blocking[best])
                if (-- blocked[unblocked] == 0)
                    available.emplace_back(unblocked);

            ThickPolyline& thick_polyline = pieces[best].path;
            const bool     closed         = pieces[best].closed;
            if (closed && thick_polyline.points.front() == thick_polyline.points.back() && thick_polyline.width.front() == thick_polyline.width.back()) {
                thick_polyline.points.pop_back();
                assert(thick_polyline.points.size() * 2 == thick_polyline.width.size());
                int nearest_idx = last_pos.nearest_point_index(thick_polyline.points);
                std::rotate(thick_polyline.points.begin(), thick_polyline.points.begin() + nearest_idx, thick_polyline.points.end());
                std::rotate(thick_polyline.width.begin(), thick_polyline.width.begin() + 2 * nearest_idx, thick_polyline.width.end());
                thick_polyline.points.emplace_back(thick_polyline.points.front());
            } else if (! closed &&
                       (thick_polyline.last_point() - last_pos).cast<double>().squaredNorm() < (thick_polyline.first_point() - last_pos).cast<double>().squaredNorm()) {
                thick_polyline.reverse();
            }
            thick_polylines_out.emplace_back(std::move(thick_polyline));
            closed_out.emplace_back(closed);
            last_pos = thick_polylines_out.back().last_point();
        }
        // The constraints always point from a higher inset index to a lower one, so they can't form a cycle.
        assert(thick_polylines_out.size() - first_poly_idx == pieces.size());

        // clip the loops to prevent the extruder from getting exactly on the first point of the loop.
        // Lines cut off at the outline of the object are left alone. Keep valid paths only.
        size_t j = first_poly_idx;
        for (size_t i = first_poly_idx; i < thick_polylines_out.size(); ++i) {
            if (closed_out[i - first_poly_idx])
                thick_polylines_out[i].clip_end(this->loop_clipping);
            if (thick_polylines_out[i].is_valid()) {
                if (j < i)
                    thick_polylines_out[j] = std::move(thick_polylines_out[i]);
                ++j;
            }
        }
        if (j < thick_polylines_out.size())
            thick_polylines_out.erase(thick_polylines_out.begin() + int(j), thick_polylines_out.end());
    }
    else {
        Polylines polylines;
        this->_fill_surface_single(params, thickness_layers, direction, expolygon, polylines);
        append(thick_polylines_out, to_thick_polylines(std::move(polylines), min_spacing));
    }
}

} // namespace Slic3r
