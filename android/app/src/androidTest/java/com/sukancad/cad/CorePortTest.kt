package com.sukancad.cad

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.core.content.FileProvider
import android.os.SystemClock
import android.view.MotionEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

@RunWith(AndroidJUnit4::class)
class CorePortTest {
    private fun clickVirtualMouseAt(view: DrawingView, x: Float, y: Float) {
        val cursor = view.virtualMouseCursorForTest()
        val button = view.virtualMouseButtonForTest()
        val dx = x - cursor.first
        val dy = y - cursor.second
        val now = SystemClock.uptimeMillis()
        view.dispatchTouchEvent(
            MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN,
                button.first, button.second, 0))
        if (dx != 0f || dy != 0f) {
            view.dispatchTouchEvent(
                MotionEvent.obtain(now, now + 20, MotionEvent.ACTION_MOVE,
                    button.first + dx, button.second + dy, 0))
        }
        view.dispatchTouchEvent(
            MotionEvent.obtain(now, now + 40, MotionEvent.ACTION_UP,
                button.first + dx, button.second + dy, 0))

        val movedButton = view.virtualMouseButtonForTest()
        view.dispatchTouchEvent(
            MotionEvent.obtain(now, now + 60, MotionEvent.ACTION_DOWN,
                movedButton.first, movedButton.second, 0))
        view.dispatchTouchEvent(
            MotionEvent.obtain(now, now + 80, MotionEvent.ACTION_UP,
                movedButton.first, movedButton.second, 0))
    }

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

    @Test
    fun zipImportExtractsDrawingsAndRemovesArchiveFromLibrary() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val testRoot = instrumentation.context
        val shareDir = File(context.cacheDir, "share").apply { mkdirs() }
        val base = "wechat-core-test-${System.currentTimeMillis()}"
        val archive = File(shareDir, "$base.zip")
        ZipOutputStream(archive.outputStream().buffered()).use { zip ->
            zip.putNextEntry(ZipEntry("wechat-plan.dxf"))
            testRoot.assets.open("core-lw.dxf").use { it.copyTo(zip) }
            zip.closeEntry()
        }

        val uri = FileProvider.getUriForFile(
            context, "com.sukancad.cad.files", archive)
        val message = DrawingLibrary.importFromUri(context, uri)
        assertTrue(message, message.contains("已解压"))

        val library = DrawingLibrary.libraryDir(context)
        assertTrue(
            "ZIP archive must not remain in drawing library",
            library.listFiles().orEmpty().none {
                it.isFile && it.name.startsWith(base) &&
                    it.extension.equals("zip", ignoreCase = true)
            },
        )
        val importedFolder = File(library, base)
        assertTrue(importedFolder.isDirectory)
        assertTrue(DrawingLibrary.countDrawings(importedFolder) > 0)

        importedFolder.deleteRecursively()
        archive.delete()
    }

    @Test
    fun cacheCleanupPreservesDrawings() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(context.cacheDir, "cache-cleanup-${System.currentTimeMillis()}")
        root.mkdirs()
        val drawing = File(root, "drawing.dxf").apply { writeText("test") }
        File(root, "drawing.dxf.skdw").writeText("cache")
        File(root, "drawing.dxf.skdw.layouts").writeText("cache")

        assertEquals(2, DrawingLibrary.cacheInfo(context, root).files)
        DrawingLibrary.clearParsedCaches(context, root)
        assertTrue("drawing must survive cache cleanup", drawing.isFile)
        assertEquals(0, DrawingLibrary.cacheInfo(context, root).files)
        root.deleteRecursively()
    }

    @Test
    fun recentRemovalKeepsDrawingFile() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val drawing = File(context.cacheDir, "recent-remove-${System.currentTimeMillis()}.dxf")
        drawing.writeText("drawing")
        DrawingLibrary.recordOpen(context, drawing.absolutePath)
        assertTrue(DrawingLibrary.recent(context).any { it.file == drawing })

        DrawingLibrary.removeRecent(context, drawing.absolutePath)
        assertTrue("drawing file must survive recent-list removal", drawing.isFile)
        assertTrue(DrawingLibrary.recent(context).none { it.file == drawing })
        drawing.delete()
    }

    @Test
    fun touchSelectionAndHandleAdjustmentKeepEditablePoint() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        lateinit var view: DrawingView
        instrumentation.runOnMainSync {
            view = DrawingView(instrumentation.targetContext).apply {
                layout(0, 0, 1000, 800)
                setMode(MeasureMode.LENGTH)
            }
            clickVirtualMouseAt(view, 500f, 500f)
        }
        assertEquals(1, view.pendingPointCount)
        val original = view.pendingPointsSnapshot().single()

        instrumentation.runOnMainSync {
            val (handleX, handleY) = view.virtualMouseCursorForTest()
            val start = SystemClock.uptimeMillis()
            view.dispatchTouchEvent(
                MotionEvent.obtain(start, start, MotionEvent.ACTION_DOWN,
                    handleX, handleY, 0))
            view.dispatchTouchEvent(
                MotionEvent.obtain(start, start + 20, MotionEvent.ACTION_MOVE,
                    700f, handleY, 0))
            view.dispatchTouchEvent(
                MotionEvent.obtain(start, start + 40, MotionEvent.ACTION_UP,
                    700f, handleY, 0))
        }
        assertEquals(1, view.pendingPointCount)
        val adjusted = view.pendingPointsSnapshot().single()
        assertTrue(
            "dragging an existing handle must update its coordinates",
            adjusted.first != original.first,
        )
        assertEquals(
            "grabbing or dragging a handle must not make it jump vertically",
            original.second,
            adjusted.second,
            1e-9,
        )
    }

    @Test
    fun drawingManagementRenamesAndDeletesWithCache() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(context.cacheDir, "drawing-manage-${System.currentTimeMillis()}")
        root.mkdirs()
        val original = File(root, "alpha.dxf").apply { writeText("drawing") }
        File(root, "alpha.dxf.skdw").writeText("cache")

        assertTrue(DrawingLibrary.rename(context, DrawingLibrary.Item(original), "beta.dxf"))
        val renamed = File(root, "beta.dxf")
        assertTrue(renamed.isFile)
        assertTrue(File(root, "beta.dxf.skdw").isFile)

        DrawingLibrary.delete(context, DrawingLibrary.Item(renamed))
        assertTrue(!renamed.exists())
        assertTrue(!File(root, "beta.dxf.skdw").exists())
        root.deleteRecursively()
    }

    @Test
    fun continuousMeasurementCanFinishFromTouchActionBar() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        lateinit var view: DrawingView
        instrumentation.runOnMainSync {
            view = DrawingView(instrumentation.targetContext).apply {
                layout(0, 0, 1000, 800)
                setMode(MeasureMode.CONTINUOUS)
            }
            val points = arrayOf(300f to 300f, 500f to 300f, 500f to 500f)
            points.forEach { point ->
                clickVirtualMouseAt(view, point.first, point.second)
            }
            assertEquals(3, view.pendingPointCount)
            assertEquals(0, view.measurements.size)
            view.finishInteraction()
        }
        assertEquals(0, view.pendingPointCount)
        assertEquals(1, view.measurements.size)
        assertEquals(MeasureMode.CONTINUOUS, view.measurements.single().kind)
    }

    @Test
    fun existingMeasurementEndpointCanBeEditedAndCancelled() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        lateinit var view: DrawingView
        instrumentation.runOnMainSync {
            view = DrawingView(instrumentation.targetContext)
            view.layout(0, 0, 1000, 800)
            view.setMode(MeasureMode.LENGTH)
            for (point in arrayOf(300f to 500f, 500f to 500f)) {
                clickVirtualMouseAt(view, point.first, point.second)
            }
        }
        val original = view.measurements.single()
        assertEquals(200.0, original.total, 1e-9)

        instrumentation.runOnMainSync {
            view.beginMeasurementEdit(original)
            val first = original.points.first()
            val sx = (view.width / 2.0 + first.first).toFloat()
            val sy = (view.height / 2.0 - first.second).toFloat()
            val start = SystemClock.uptimeMillis()
            view.dispatchTouchEvent(
                MotionEvent.obtain(start, start, MotionEvent.ACTION_DOWN, sx, sy, 0))
            view.dispatchTouchEvent(
                MotionEvent.obtain(start, start + 20, MotionEvent.ACTION_MOVE,
                    sx + 100f, sy, 0))
            view.dispatchTouchEvent(
                MotionEvent.obtain(start, start + 40, MotionEvent.ACTION_UP,
                    sx + 100f, sy, 0))
        }
        assertEquals(100.0, view.measurements.single().total, 1e-9)
        assertTrue(view.isEditingObject)

        instrumentation.runOnMainSync {
            view.finishObjectEdit()
        }
        assertTrue(!view.isEditingObject)
        assertEquals(100.0, view.measurements.single().total, 1e-9)
    }

    @Test
    fun virtualMouseSelectsPointAndDimensionCanBeDeletedFromSelection() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        lateinit var view: DrawingView
        instrumentation.runOnMainSync {
            view = DrawingView(instrumentation.targetContext).apply {
                layout(0, 0, 1000, 800)
                setMode(MeasureMode.LENGTH)
            }
            val now = SystemClock.uptimeMillis()
            // 测量态下直接点图纸不再落下选点，必须操作浮动鼠标。
            view.dispatchTouchEvent(
                MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN,
                    420f, 420f, 0))
            view.dispatchTouchEvent(
                MotionEvent.obtain(now, now + 20, MotionEvent.ACTION_UP,
                    420f, 420f, 0))
            assertEquals(0, view.pendingPointCount)

            clickVirtualMouseAt(view, 300f, 450f)
            clickVirtualMouseAt(view, 600f, 450f)
            assertEquals(1, view.measurements.size)
            assertNotNull(view.hitMeasurementAtScreenForTest(450f, 450f))
            assertNotNull(view.hitMeasurementAtScreenForTest(300f, 450f))
            assertNotNull(view.hitMeasurementAtScreenForTest(450f, 370f))

            view.beginMeasurementEdit(view.measurements.single())
            assertTrue(view.isEditingObject)
            assertTrue(view.deleteEditingObject())
            assertTrue(view.measurements.isEmpty())
        }
    }

    @Test
    fun angleMeasurementUsesTwoLineSegmentsAndLabelTap() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val source = File(context.cacheDir, "angle-two-lines.dxf")
        val cache = File(context.cacheDir, "angle-two-lines.skdw")
        instrumentation.context.assets.open("core-lw.dxf").use { input ->
            source.outputStream().use(input::copyTo)
        }
        assertTrue(DwgCore.parse(source, cache))
        val document = SkdwDocument.read(cache)
        assertNotNull(document)

        lateinit var view: DrawingView
        instrumentation.runOnMainSync {
            view = DrawingView(context)
            view.layout(0, 0, 1000, 800)
            view.load(document)
            view.fitToView(true)
            val fit = view.viewState()!!
            view.restoreView(fit.first, fit.second, fit.third * 0.45)
            view.setMode(MeasureMode.ANGLE)
            val polyline = document!!.entities
                .filterIsInstance<SkdwDocument.Entity.Polyline>().single()
            val first = view.worldToScreenForTest(
                (polyline.xs[0] + polyline.xs[1]) / 2.0,
                (polyline.ys[0] + polyline.ys[1]) / 2.0,
            )
            val second = view.worldToScreenForTest(
                (polyline.xs[1] + polyline.xs[2]) / 2.0,
                (polyline.ys[1] + polyline.ys[2]) / 2.0,
            )
            clickVirtualMouseAt(view, first.first, first.second)
            assertEquals(view.activeInteractionHint, 1, view.pendingPointCount)
            clickVirtualMouseAt(view, second.first, second.second)
            assertEquals(view.activeInteractionHint, 3, view.pendingPointCount)
            clickVirtualMouseAt(view, 400f, 300f)
        }
        assertEquals(1, view.measurements.size)
        val measurement = view.measurements.single()
        assertEquals(MeasureMode.ANGLE, measurement.kind)
        assertEquals(4, measurement.points.size)
    }

    @Test
    fun autoCalibrationInfersScaleFromDimensionTextAndSegments() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val source = File(context.cacheDir, "auto-calibration.dxf")
        val cache = File(context.cacheDir, "auto-calibration.skdw")
        val base = instrumentation.context.assets.open("core-lw.dxf")
            .bufferedReader().use { it.readText() }
            .replace(
                "  9\n\$HANDSEED\n  5\n40",
                "  9\n\$HANDSEED\n  5\nFF",
            )
        val marker = "  0\nENDSEC\n  0\nSECTION\n  2\nOBJECTS"
        assertTrue(base.contains(marker))
        fun textEntity(handle: Int, x: Double, y: Double, value: String): String =
            """
            0
            TEXT
            5
            $handle
            330
            1
            100
            AcDbEntity
            8
            0
            100
            AcDbText
            10
            $x
            20
            $y
            30
            0.0
            40
            0.2
            1
            $value
            """.trimIndent() + "\n"
        val texts = textEntity(50, 2.0, 0.2, "4") +
                textEntity(51, 3.8, 1.5, "3") +
                textEntity(52, 2.0, 2.8, "4") +
                textEntity(53, 0.2, 1.5, "3") +
                textEntity(54, 1.0, 0.2, "4") +
                textEntity(55, 3.8, 1.0, "3")
        source.writeText(base.replace(marker, texts + marker))
        assertTrue(DwgCore.parse(source, cache))
        val document = SkdwDocument.read(cache)
        assertNotNull(document)
        val parsedTexts = document!!.entities.filterIsInstance<SkdwDocument.Entity.Text>()
        assertEquals(6, parsedTexts.size)

        lateinit var view: DrawingView
        instrumentation.runOnMainSync {
            view = DrawingView(context)
            view.layout(0, 0, 1000, 800)
            view.load(document)
        }
        val result = view.inferAutoCalibration()
        assertNotNull(result)
        assertEquals(1.0, result!!.scale, 0.02)
        assertTrue(result.samples >= 3)
    }
}
