package com.sukancad.cad

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class CorePortTest {
    @Test
    fun sharedGeometryMatchesDesktopCore() {
        val xs = doubleArrayOf(0.0, 10.0, 10.0, 0.0)
        val ys = doubleArrayOf(0.0, 0.0, 5.0, 5.0)

        assertEquals(50.0, GeometryCore.polygonArea(xs, ys), 1e-9)
        assertEquals(30.0, GeometryCore.polygonPerimeter(xs, ys), 1e-9)
        assertEquals(
            90.0,
            GeometryCore.angleDegrees(0.0, 0.0, 1.0, 0.0, 0.0, 1.0),
            1e-9,
        )
        assertTrue(GeometryCore.pointInPolygon(5.0, 2.0, xs, ys))

        val circle = GeometryCore.circleFromThreePoints(
            10.0, 0.0,
            0.0, 10.0,
            -10.0, 0.0,
        )
        assertEquals(3, circle.size)
        assertEquals(10.0, circle[2], 1e-9)
        assertEquals(
            10.0 * Math.PI,
            GeometryCore.arcLengthFromThreePoints(
                10.0, 0.0,
                0.0, 10.0,
                -10.0, 0.0,
            ),
            1e-9,
        )
    }

    @Test
    fun parsesDxfThroughNativePipeline() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        context.cacheDir.mkdirs()
        val source = File(context.cacheDir, "core-lw.dxf")
        instrumentation.context.assets.open("core-lw.dxf").use { input ->
            source.outputStream().use(input::copyTo)
        }
        val cache = File(context.cacheDir, "core-lw.skdw")

        assertTrue(DwgCore.parse(source, cache))
        val document = SkdwDocument.read(cache)
        assertNotNull(document)
        assertEquals(1, document!!.layers.size)
        assertEquals(1, document.entities.size)
        assertTrue(document.entities.single() is SkdwDocument.Entity.Polyline)
    }
}
