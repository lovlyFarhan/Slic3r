#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include <algorithm>

namespace Slic3r {

BridgeDetector::BridgeDetector(
    ExPolygon                   _expolygon,
    const ExPolygonCollection  &_lower_slices, 
    coord_t                     _spacing) :
    // The original infill polygon, not inflated.
    expolygons(expolygons_owned),
    // All surfaces of the object supporting this region.
    lower_slices(_lower_slices),
    spacing(_spacing)
{
    this->expolygons_owned.push_back(STDMOVE(_expolygon));
    initialize();
}

BridgeDetector::BridgeDetector(
    const ExPolygons           &_expolygons,
    const ExPolygonCollection  &_lower_slices,
    coord_t                     _spacing) : 
    // The original infill polygon, not inflated.
    expolygons(_expolygons),
    // All surfaces of the object supporting this region.
    lower_slices(_lower_slices),
    spacing(_spacing)
{
    initialize();
}

void BridgeDetector::initialize()
{
    // 5 degrees stepping
    this->resolution = PI/36.0; 
    // output angle not known
    this->angle = -1.;

    // Outset our bridge by an arbitrary amout; we'll use this outer margin for detecting anchors.
    Polygons grown = offset(to_polygons(this->expolygons), float(this->spacing));
    
    // Detect possible anchoring edges of this bridging region.
    // Detect what edges lie on lower slices by turning bridge contour and holes
    // into polylines and then clipping them with each lower slice's contour.
    // Currently _edges are only used to set a candidate direction of the bridge (see bridge_direction_candidates()).
    this->_edges = intersection_pl(to_polylines(grown), this->lower_slices.contours());
    
    #ifdef SLIC3R_DEBUG
    printf("  bridge has " PRINTF_ZU " support(s)\n", this->_edges.size());
    #endif
    
    // detect anchors as intersection between our bridge expolygon and the lower slices
    // safety offset required to avoid Clipper from detecting empty intersection while Boost actually found some edges
    this->_anchor_regions = intersection_ex(grown, to_polygons(this->lower_slices.expolygons), true);
    
    /*
    if (0) {
        require "Slic3r/SVG.pm";
        Slic3r::SVG::output("bridge.svg",
            expolygons      => [ $self->expolygon ],
            red_expolygons  => $self->lower_slices,
            polylines       => $self->_edges,
        );
    }
    */
}

bool
BridgeDetector::detect_angle()
{
    if (this->_edges.empty() || this->_anchor_regions.empty()) 
        // The bridging region is completely in the air, there are no anchors available at the layer below.
        return false;

    std::vector<BridgeDirection> candidates;
    {
        std::vector<double> angles = bridge_direction_candidates();
        candidates.reserve(angles.size());
        for (size_t i = 0; i < angles.size(); ++ i)
            candidates.push_back(BridgeDirection(angles[i]));
    }
    
    /*  Outset the bridge expolygon by half the amount we used for detecting anchors;
        we'll use this one to clip our test lines and be sure that their endpoints
        are inside the anchors and not on their contours leading to false negatives. */
    Polygons clip_area = offset(this->expolygons, 0.5f * float(this->spacing));
    
    /*  we'll now try several directions using a rudimentary visibility check:
        bridge in several directions and then sum the length of lines having both
        endpoints within anchors */
        
    bool have_coverage = false;
    for (size_t i_angle = 0; i_angle < candidates.size(); ++ i_angle)
    {
        const double angle = candidates[i_angle].angle;

        Lines lines;
        {
            // Get an oriented bounding box around _anchor_regions.
            BoundingBox bbox = get_extents_rotated(this->_anchor_regions, - angle);
            // Cover the region with line segments.
            lines.reserve((bbox.max.y - bbox.min.y + this->spacing) / this->spacing);
            double s = sin(angle);
            double c = cos(angle);
            //FIXME Vojtech: The lines shall be spaced half the line width from the edge, but then 
            // some of the test cases fail. Need to adjust the test cases then?
//            for (coord_t y = bbox.min.y + this->spacing / 2; y <= bbox.max.y; y += this->spacing)
            for (coord_t y = bbox.min.y; y <= bbox.max.y; y += this->spacing)
                lines.push_back(Line(
                    Point((coord_t)round(c * bbox.min.x - s * y), (coord_t)round(c * y + s * bbox.min.x)),
                    Point((coord_t)round(c * bbox.max.x - s * y), (coord_t)round(c * y + s * bbox.max.x))));
        }

        double total_length = 0;
        double max_length = 0;
        {
            Lines clipped_lines = intersection_ln(lines, clip_area);
            for (size_t i = 0; i < clipped_lines.size(); ++i) {
                const Line &line = clipped_lines[i];
                if (expolygons_contain(this->_anchor_regions, line.a) && expolygons_contain(this->_anchor_regions, line.b)) {
                    // This line could be anchored.
                    double len = line.length();
                    total_length += len;
                    max_length = std::max(max_length, len);
                }
            }        
        }
        if (total_length == 0.)
            continue;

        have_coverage = true;
        // Sum length of bridged lines.
        candidates[i_angle].coverage = total_length;
        /*  The following produces more correct results in some cases and more broken in others.
            TODO: investigate, as it looks more reliable than line clipping. */
        // $directions_coverage{$angle} = sum(map $_->area, @{$self->coverage($angle)}) // 0;
        // max length of bridged lines
        candidates[i_angle].max_length = max_length;
    }

    // if no direction produced coverage, then there's no bridge direction
    if (! have_coverage)
        return false;
    
    // sort directions by coverage - most coverage first
    std::sort(candidates.begin(), candidates.end());
    
    // if any other direction is within extrusion width of coverage, prefer it if shorter
    // TODO: There are two options here - within width of the angle with most coverage, or within width of the currently perferred?
    size_t i_best = 0;
    for (size_t i = 1; i < candidates.size() && candidates[i_best].coverage - candidates[i].coverage < this->spacing; ++ i)
        if (candidates[i].max_length < candidates[i_best].max_length)
            i_best = i;

    this->angle = candidates[i_best].angle;
    if (this->angle >= PI)
        this->angle -= PI;
    
    #ifdef SLIC3R_DEBUG
    printf("  Optimal infill angle is %d degrees\n", (int)Slic3r::Geometry::rad2deg(this->angle));
    #endif
    
    return true;
}

std::vector<double> BridgeDetector::bridge_direction_candidates() const
{
    // we test angles according to configured resolution
    std::vector<double> angles;
    for (int i = 0; i <= PI/this->resolution; ++i)
        angles.push_back(i * this->resolution);
    
    // we also test angles of each bridge contour
    {
        Lines lines = to_lines(this->expolygons);
        for (Lines::const_iterator line = lines.begin(); line != lines.end(); ++line)
            angles.push_back(line->direction());
    }
    
    /*  we also test angles of each open supporting edge
        (this finds the optimal angle for C-shaped supports) */
    for (Polylines::const_iterator edge = this->_edges.begin(); edge != this->_edges.end(); ++edge)
        if (! edge->first_point().coincides_with(edge->last_point()))
            angles.push_back(Line(edge->first_point(), edge->last_point()).direction());
    
    // remove duplicates
    double min_resolution = PI/180.0;  // 1 degree
    std::sort(angles.begin(), angles.end());
    for (size_t i = 1; i < angles.size(); ++i) {
        if (Slic3r::Geometry::directions_parallel(angles[i], angles[i-1], min_resolution)) {
            angles.erase(angles.begin() + i);
            --i;
        }
    }
    /*  compare first value with last one and remove the greatest one (PI) 
        in case they are parallel (PI, 0) */
    if (Slic3r::Geometry::directions_parallel(angles.front(), angles.back(), min_resolution))
        angles.pop_back();

    return angles;
}

Polygons BridgeDetector::coverage(double angle) const
{
    if (angle == -1)
        angle = this->angle;

    Polygons covered;

    if (angle != -1) {

        // Get anchors, convert them to Polygons and rotate them.
        Polygons anchors = to_polygons(this->_anchor_regions);
        polygons_rotate(anchors, PI/2.0 - angle);
        
        for (ExPolygons::const_iterator it_expoly = this->expolygons.begin(); it_expoly != this->expolygons.end(); ++ it_expoly)
        {
            // Clone our expolygon and rotate it so that we work with vertical lines.
            ExPolygon expolygon = *it_expoly;
            expolygon.rotate(PI/2.0 - angle);
            
            /*  Outset the bridge expolygon by half the amount we used for detecting anchors;
                we'll use this one to generate our trapezoids and be sure that their vertices
                are inside the anchors and not on their contours leading to false negatives. */
            ExPolygons grown = offset_ex(expolygon, 0.5f * float(this->spacing));
            
            // Compute trapezoids according to a vertical orientation
            Polygons trapezoids;
            for (ExPolygons::const_iterator it = grown.begin(); it != grown.end(); ++it)
                it->get_trapezoids2(&trapezoids, PI/2.0);
            
            for (Polygons::iterator trapezoid = trapezoids.begin(); trapezoid != trapezoids.end(); ++trapezoid) {
                Lines supported = intersection_ln(trapezoid->lines(), anchors);
                size_t n_supported = 0;
                // not nice, we need a more robust non-numeric check
                for (size_t i = 0; i < supported.size(); ++i)
                    if (supported[i].length() >= this->spacing)
                        ++ n_supported;
                if (n_supported >= 2) 
                    covered.push_back(STDMOVE(*trapezoid));
            }
        }

        // Unite the trapezoids before rotation, as the rotation creates tiny gaps and intersections between the trapezoids
        // instead of exact overlaps.
        covered = union_(covered);

        // Intersect trapezoids with actual bridge area to remove extra margins and append it to result.
        polygons_rotate(covered, -(PI/2.0 - angle));
    	covered = intersection(covered, to_polygons(this->expolygons));

        /*
        if (0) {
            my @lines = map @{$_->lines}, @$trapezoids;
            $_->rotate(-(PI/2 - $angle), [0,0]) for @lines;
            
            require "Slic3r/SVG.pm";
            Slic3r::SVG::output(
                "coverage_" . rad2deg($angle) . ".svg",
                expolygons          => [$self->expolygon],
                green_expolygons    => $self->_anchor_regions,
                red_expolygons      => $coverage,
                lines               => \@lines,
            );
        }
        */
    }
    return covered;
}

/*  This method returns the bridge edges (as polylines) that are not supported
    but would allow the entire bridge area to be bridged with detected angle
    if supported too */
void
BridgeDetector::unsupported_edges(double angle, Polylines* unsupported) const
{
    if (angle == -1) angle = this->angle;
    if (angle == -1) return;

    Polygons grown_lower = offset(this->lower_slices.expolygons, float(this->spacing));

    for (ExPolygons::const_iterator it_expoly = this->expolygons.begin(); it_expoly != this->expolygons.end(); ++ it_expoly) {    
        // get unsupported bridge edges (both contour and holes)
        Lines unsupported_lines = to_lines(diff_pl(to_polylines(*it_expoly), grown_lower));
        /*  Split into individual segments and filter out edges parallel to the bridging angle
            TODO: angle tolerance should probably be based on segment length and flow width,
            so that we build supports whenever there's a chance that at least one or two bridge
            extrusions would be anchored within such length (i.e. a slightly non-parallel bridging
            direction might still benefit from anchors if long enough)
            double angle_tolerance = PI / 180.0 * 5.0; */
        for (Lines::const_iterator line = unsupported_lines.begin(); line != unsupported_lines.end(); ++line) {
            if (!Slic3r::Geometry::directions_parallel(line->direction(), angle))
                unsupported->push_back(*line);
        }
    }
    
    /*
    if (0) {
        require "Slic3r/SVG.pm";
        Slic3r::SVG::output(
            "unsupported_" . rad2deg($angle) . ".svg",
            expolygons          => [$self->expolygon],
            green_expolygons    => $self->_anchor_regions,
            red_expolygons      => union_ex($grown_lower),
            no_arrows           => 1,
            polylines           => \@bridge_edges,
            red_polylines       => $unsupported,
        );
    }
    */
}

Polylines
BridgeDetector::unsupported_edges(double angle) const
{
    Polylines pp;
    this->unsupported_edges(angle, &pp);
    return pp;
}

}
