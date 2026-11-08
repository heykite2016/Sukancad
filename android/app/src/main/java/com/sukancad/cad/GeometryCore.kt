package com.sukancad.cad

/** 与桌面端共享的 src/core/cad_geometry.c */
object GeometryCore {
    init {
        System.loadLibrary("cadcore")
    }

    external fun polygonArea(xs: DoubleArray, ys: DoubleArray): Double
    external fun polygonPerimeter(xs: DoubleArray, ys: DoubleArray): Double
    external fun polylineLength(
        xs: DoubleArray,
        ys: DoubleArray,
        closed: Boolean,
    ): Double

    external fun pointInPolygon(
        x: Double,
        y: Double,
        xs: DoubleArray,
        ys: DoubleArray,
    ): Boolean

    external fun angleDegrees(
        vx: Double,
        vy: Double,
        ax: Double,
        ay: Double,
        bx: Double,
        by: Double,
    ): Double

    /** 返回 [centerX, centerY, radius]，三点共线时为空。 */
    external fun circleFromThreePoints(
        ax: Double,
        ay: Double,
        bx: Double,
        by: Double,
        cx: Double,
        cy: Double,
    ): DoubleArray

    /** 返回 NaN 表示三点无法确定有效圆弧。 */
    external fun arcLengthFromThreePoints(
        ax: Double,
        ay: Double,
        mx: Double,
        my: Double,
        bx: Double,
        by: Double,
    ): Double
}
