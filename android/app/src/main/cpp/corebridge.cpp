#include "cad_geometry.h"

#include <jni.h>

#include <cmath>
#include <cstddef>
#include <vector>

namespace {

std::vector<CadPoint2> pointsFromArrays(JNIEnv *env, jdoubleArray xs,
                                        jdoubleArray ys)
{
    if (!xs || !ys) {
        return {};
    }
    const jsize count = env->GetArrayLength(xs);
    if (count != env->GetArrayLength(ys) || count <= 0) {
        return {};
    }
    std::vector<double> x(static_cast<size_t>(count));
    std::vector<double> y(static_cast<size_t>(count));
    env->GetDoubleArrayRegion(xs, 0, count, x.data());
    env->GetDoubleArrayRegion(ys, 0, count, y.data());
    std::vector<CadPoint2> points(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        points[static_cast<size_t>(i)] = {x[static_cast<size_t>(i)],
                                          y[static_cast<size_t>(i)]};
    }
    return points;
}

} // namespace

extern "C" JNIEXPORT jdouble JNICALL
Java_com_sukancad_cad_GeometryCore_polygonArea(JNIEnv *env, jclass,
                                                jdoubleArray xs,
                                                jdoubleArray ys)
{
    const std::vector<CadPoint2> points = pointsFromArrays(env, xs, ys);
    return points.size() >= 3
        ? cad_polygon_area(points.data(), static_cast<int>(points.size()))
        : 0.0;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_sukancad_cad_GeometryCore_polygonPerimeter(JNIEnv *env, jclass,
                                                     jdoubleArray xs,
                                                     jdoubleArray ys)
{
    const std::vector<CadPoint2> points = pointsFromArrays(env, xs, ys);
    return points.size() >= 3
        ? cad_polygon_perimeter(points.data(),
                                static_cast<int>(points.size()))
        : 0.0;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_sukancad_cad_GeometryCore_polylineLength(JNIEnv *env, jclass,
                                                   jdoubleArray xs,
                                                   jdoubleArray ys,
                                                   jboolean closed)
{
    const std::vector<CadPoint2> points = pointsFromArrays(env, xs, ys);
    return points.size() >= 2
        ? cad_polyline_length(points.data(), static_cast<int>(points.size()),
                              closed ? 1 : 0)
        : 0.0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_sukancad_cad_GeometryCore_pointInPolygon(JNIEnv *env, jclass,
                                                   jdouble x, jdouble y,
                                                   jdoubleArray xs,
                                                   jdoubleArray ys)
{
    const std::vector<CadPoint2> points = pointsFromArrays(env, xs, ys);
    return points.size() >= 3
        && cad_point_in_polygon({x, y}, points.data(),
                                static_cast<int>(points.size()))
        ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_sukancad_cad_GeometryCore_angleDegrees(
    JNIEnv *, jclass, jdouble vx, jdouble vy, jdouble ax, jdouble ay,
    jdouble bx, jdouble by)
{
    return cad_angle_between_degrees({vx, vy}, {ax, ay}, {bx, by});
}

extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_sukancad_cad_GeometryCore_circleFromThreePoints(
    JNIEnv *env, jclass, jdouble ax, jdouble ay, jdouble bx, jdouble by,
    jdouble cx, jdouble cy)
{
    CadPoint2 center{};
    double radius = 0.0;
    if (!cad_circle_from_three_points({ax, ay}, {bx, by}, {cx, cy},
                                      &center, &radius)) {
        return env->NewDoubleArray(0);
    }
    const double values[] = {center.x, center.y, radius};
    jdoubleArray result = env->NewDoubleArray(3);
    if (result) {
        env->SetDoubleArrayRegion(result, 0, 3, values);
    }
    return result;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_sukancad_cad_GeometryCore_arcLengthFromThreePoints(
    JNIEnv *, jclass, jdouble ax, jdouble ay, jdouble mx, jdouble my,
    jdouble bx, jdouble by)
{
    int valid = 0;
    const double length = cad_arc_length_from_three_points(
        {ax, ay}, {mx, my}, {bx, by}, &valid);
    return valid ? length : std::nan("");
}
