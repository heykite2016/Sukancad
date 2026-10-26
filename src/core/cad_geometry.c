#include "cad_geometry.h"

#include <math.h>

#define CAD_EPSILON 1.0e-12

double cad_distance(CadPoint2 a, CadPoint2 b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return sqrt(dx * dx + dy * dy);
}

double cad_polyline_length(const CadPoint2 *points, int count, int closed)
{
    double length = 0.0;
    int i;
    if (points == 0 || count < 2) {
        return 0.0;
    }
    for (i = 1; i < count; ++i) {
        length += cad_distance(points[i - 1], points[i]);
    }
    if (closed) {
        length += cad_distance(points[count - 1], points[0]);
    }
    return length;
}

double cad_signed_area(const CadPoint2 *points, int count)
{
    double twice_area = 0.0;
    int i;
    if (points == 0 || count < 3) {
        return 0.0;
    }
    for (i = 0; i < count; ++i) {
        const CadPoint2 a = points[i];
        const CadPoint2 b = points[(i + 1) % count];
        twice_area += a.x * b.y - b.x * a.y;
    }
    return twice_area * 0.5;
}

double cad_polygon_area(const CadPoint2 *points, int count)
{
    return fabs(cad_signed_area(points, count));
}

double cad_polygon_perimeter(const CadPoint2 *points, int count)
{
    return cad_polyline_length(points, count, 1);
}

int cad_point_in_polygon(CadPoint2 point, const CadPoint2 *points, int count)
{
    int inside = 0;
    int i;
    if (points == 0 || count < 3) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        const CadPoint2 a = points[i];
        const CadPoint2 b = points[(i + 1) % count];
        if (((a.y > point.y) != (b.y > point.y))
            && (point.x < (b.x - a.x) * (point.y - a.y)
                                  / (b.y - a.y + CAD_EPSILON)
                              + a.x)) {
            inside = !inside;
        }
    }
    return inside;
}

double cad_angle_between_degrees(CadPoint2 vertex, CadPoint2 a, CadPoint2 b)
{
    const double ax = a.x - vertex.x;
    const double ay = a.y - vertex.y;
    const double bx = b.x - vertex.x;
    const double by = b.y - vertex.y;
    const double denominator = sqrt((ax * ax + ay * ay) * (bx * bx + by * by));
    double cosine;
    if (denominator < CAD_EPSILON) {
        return 0.0;
    }
    cosine = (ax * bx + ay * by) / denominator;
    if (cosine > 1.0) {
        cosine = 1.0;
    } else if (cosine < -1.0) {
        cosine = -1.0;
    }
    return acos(cosine) * 180.0 / M_PI;
}

int cad_circle_from_three_points(CadPoint2 a, CadPoint2 b, CadPoint2 c,
                                 CadPoint2 *center, double *radius)
{
    const double d = 2.0 * (a.x * (b.y - c.y)
                            + b.x * (c.y - a.y)
                            + c.x * (a.y - b.y));
    double ux;
    double uy;
    if (fabs(d) < CAD_EPSILON || center == 0 || radius == 0) {
        return 0;
    }
    ux = ((a.x * a.x + a.y * a.y) * (b.y - c.y)
          + (b.x * b.x + b.y * b.y) * (c.y - a.y)
          + (c.x * c.x + c.y * c.y) * (a.y - b.y))
         / d;
    uy = ((a.x * a.x + a.y * a.y) * (c.x - b.x)
          + (b.x * b.x + b.y * b.y) * (a.x - c.x)
          + (c.x * c.x + c.y * c.y) * (b.x - a.x))
         / d;
    center->x = ux;
    center->y = uy;
    *radius = cad_distance(*center, a);
    return *radius > CAD_EPSILON;
}

double cad_arc_length_from_three_points(CadPoint2 a, CadPoint2 through,
                                        CadPoint2 b, int *valid)
{
    CadPoint2 center;
    double radius = 0.0;
    double start;
    double middle;
    double end;
    double sweep;
    if (!cad_circle_from_three_points(a, through, b, &center, &radius)) {
        if (valid != 0) {
            *valid = 0;
        }
        return 0.0;
    }
    start = atan2(a.y - center.y, a.x - center.x);
    middle = atan2(through.y - center.y, through.x - center.x);
    end = atan2(b.y - center.y, b.x - center.x);
    while (middle < start) {
        middle += 2.0 * M_PI;
    }
    while (end < start) {
        end += 2.0 * M_PI;
    }
    sweep = end - start;
    if (middle > end) {
        sweep = 2.0 * M_PI - sweep;
    }
    if (valid != 0) {
        *valid = sweep > CAD_EPSILON;
    }
    return radius * sweep;
}
