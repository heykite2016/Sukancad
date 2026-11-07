#include "cad_geometry.h"

#include <cmath>
#include <cstdio>

namespace {
bool near(double a, double b, double tolerance = 1e-7)
{
    return std::fabs(a - b) <= tolerance;
}
}

int main()
{
    const CadPoint2 square[] = {
        {0.0, 0.0}, {10.0, 0.0}, {10.0, 5.0}, {0.0, 5.0}
    };
    const CadPoint2 arcStart = {10.0, 0.0};
    const CadPoint2 arcMiddle = {0.0, 10.0};
    const CadPoint2 arcEnd = {-10.0, 0.0};
    int arcValid = 0;

    if (!near(cad_polyline_length(square, 4, 1), 30.0)
        || !near(cad_polygon_area(square, 4), 50.0)
        || !near(cad_polygon_perimeter(square, 4), 30.0)
        || cad_point_in_polygon({5.0, 2.0}, square, 4) != 1
        || cad_point_in_polygon({15.0, 2.0}, square, 4) != 0
        || !near(cad_angle_between_degrees({0.0, 0.0}, {1.0, 0.0},
                                           {0.0, 1.0}), 90.0)
        || !near(cad_arc_length_from_three_points(
                     arcStart, arcMiddle, arcEnd, &arcValid),
                 10.0 * M_PI)
        || arcValid != 1) {
        std::fprintf(stderr, "cad_geometry_tests failed\n");
        return 1;
    }
    std::puts("cad_geometry_tests passed");
    return 0;
}
