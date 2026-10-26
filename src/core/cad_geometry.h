#ifndef CADLENS_CAD_GEOMETRY_H
#define CADLENS_CAD_GEOMETRY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CadPoint2 {
    double x;
    double y;
} CadPoint2;

double cad_distance(CadPoint2 a, CadPoint2 b);
double cad_polyline_length(const CadPoint2 *points, int count, int closed);
double cad_signed_area(const CadPoint2 *points, int count);
double cad_polygon_area(const CadPoint2 *points, int count);
double cad_polygon_perimeter(const CadPoint2 *points, int count);
int cad_point_in_polygon(CadPoint2 point, const CadPoint2 *points, int count);

double cad_angle_between_degrees(CadPoint2 vertex, CadPoint2 a, CadPoint2 b);
int cad_circle_from_three_points(CadPoint2 a, CadPoint2 b, CadPoint2 c,
                                 CadPoint2 *center, double *radius);
double cad_arc_length_from_three_points(CadPoint2 a, CadPoint2 through,
                                        CadPoint2 b, int *valid);

#ifdef __cplusplus
}
#endif

#endif
