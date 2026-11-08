package com.sukancad.cad

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Typeface
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** 图元类型常量（与 SKDW 一致） */
private const val T_LINE = 1
private const val T_CIRCLE = 2
private const val T_ARC = 3
private const val T_POLYLINE = 4
private const val T_TEXT = 5

// 测量模式（对标竞品）：长度/连续/面积/角度/弧长/半径/坐标
// 标注绘制模式：文字/直线/任意线/云线/引线；编号与照片沿用 NotePin
enum class MeasureMode {
    NONE, LENGTH, CONTINUOUS, AREA, ANGLE, ARC, RADIUS, COORD,
    TEXT, LINE, SKETCH, CLOUD, LEADER, PIN, PHOTO
}

/** 图上编号/照片标注（持久化到图纸备注文件） */
class NotePin(val id: Int, val x: Double, val y: Double,
              var note: String = "", var photoPath: String? = null)

/** 用户绘制的标注（持久化，与测量同文件） */
class Annotation(val id: Int, val kind: MeasureMode,
                 val points: MutableList<Pair<Double, Double>>,
                 var text: String = "")

data class Measurement(
    val kind: MeasureMode,
    val points: List<Pair<Double, Double>>,
    val edgeLengths: List<Double>,
    val total: Double,
    val area: Double,          // 面积（mm²），仅面积类
    val perimeter: Double,     // 周长，仅面积类
    val radius: Double,        // 圆测量
    val extra: String = "",    // 坐标标注等附加文本
) {
    /** 带换算比例的显示文本（s=1 时为模型单位原值） */
    fun label(s: Double): String = when (kind) {
        MeasureMode.LENGTH -> "长度 %.2f".format(total * s)
        MeasureMode.CONTINUOUS -> "总长 %.2f".format(total * s)
        MeasureMode.AREA ->
            "面积 %.2f m²\n周长 %.2f".format(area * s * s / 1e6, perimeter * s)
        MeasureMode.RADIUS ->
            "R %.2f\n⌀ %.2f".format(radius * s, radius * 2 * s)
        MeasureMode.COORD -> extra
        MeasureMode.ARC ->
            "弧长 %.2f\nR %.2f  ∠ ${extra}°".format(total * s, radius * s)
        MeasureMode.ANGLE -> {
            if (points.size < 3) ""
            else "∠ ${"%.1f".format(angleAt(points[0], points[1], points[2]))}°"
        }
        MeasureMode.NONE, MeasureMode.PIN, MeasureMode.PHOTO,
        MeasureMode.TEXT, MeasureMode.LINE, MeasureMode.SKETCH,
        MeasureMode.CLOUD, MeasureMode.LEADER -> ""
    }

    val label: String
        get() = label(1.0)

    companion object {
        /** 顶点 v 处 a、b 两边的夹角（度，0~180） */
        fun angleAt(v: Pair<Double, Double>, a: Pair<Double, Double>,
                    b: Pair<Double, Double>): Double {
            return GeometryCore.angleDegrees(
                v.first, v.second, a.first, a.second, b.first, b.second)
        }
    }
}

/**
 * CAD 图纸自绘视图：黑底渲染 + 单指平移/双指缩放 + 测量标注。
 * 坐标系：世界 Y 向上；屏幕 Y 向下。
 */
class DrawingView(context: Context) : android.view.View(context) {

    var document: SkdwDocument? = null
        private set

    // 视口
    private var centerX = 0.0
    private var centerY = 0.0
    private var scale = 1.0
    private var fitted = false

    // 空间网格索引：大图纸（10 万+图元）逐帧全量遍历会卡，按格子分桶后只绘可见桶
    private var grid: GridIndex? = null

    // 布局模式的模型空间叠加文档（视口内容从这里取）
    private var overlayDoc: SkdwDocument? = null
    private var overlayGrid: GridIndex? = null

    /** 均匀网格索引（约每格 6 个图元，上限 160×160 格） */
    private class GridIndex(doc: SkdwDocument) {
        val cols: Int
        val rows: Int
        val cellW: Double
        val cellH: Double
        val originX: Double
        val originY: Double
        val cells: List<MutableList<SkdwDocument.Entity>>
        val big: MutableList<SkdwDocument.Entity>

        init {
            val n = doc.entities.size
            val side = min(160, max(24, Math.sqrt(n / 6.0).toInt()))
            cols = side; rows = side
            cellW = (doc.maxX - doc.minX) / side
            cellH = (doc.maxY - doc.minY) / side
            originX = doc.minX; originY = doc.minY
            val list = ArrayList<MutableList<SkdwDocument.Entity>>(side * side)
            repeat(side * side) { list.add(ArrayList()) }
            cells = list
            big = ArrayList()
            val maxSpan = 8   // 跨格超过 8×8 的算大图元，单独列表
            for (e in doc.entities) {
                // bbox 布局 [minX, minY, maxX, maxY]；行号随 y 增大
                val c0 = cellX(e.bbox[0]); val c1 = cellX(e.bbox[2])
                val r0 = cellY(e.bbox[1]); val r1 = cellY(e.bbox[3])
                val span = (c1 - c0 + 1) * (r1 - r0 + 1)
                if (span > maxSpan) { big.add(e); continue }
                for (r in r0..r1) for (c in c0..c1) cells[r * cols + c].add(e)
            }
        }

        private fun cellX(x: Double): Int =
            ((x - originX) / cellW).toInt().coerceIn(0, cols - 1)
        private fun cellY(y: Double): Int =
            ((y - originY) / cellH).toInt().coerceIn(0, rows - 1)

        /** 与矩形相交的候选桶范围 */
        fun range(minX: Double, minY: Double, maxX: Double, maxY: Double)
                : Pair<Pair<Int, Int>, Pair<Int, Int>> =
            Pair(Pair(cellX(minX), cellX(maxX)), Pair(cellY(minY), cellY(maxY)))
    }

    private fun buildIndex(doc: SkdwDocument?): GridIndex? {
        if (doc == null || doc.entities.isEmpty()) return null
        if (doc.entities.size < 600) return null   // 小图直接全遍历更快
        val w = doc.maxX - doc.minX
        val h = doc.maxY - doc.minY
        if (w <= 0 || h <= 0) return null
        return GridIndex(doc)
    }

    // 测量
    var mode = MeasureMode.NONE
        private set
    private val pending = ArrayList<Pair<Double, Double>>()
    /** 布局模式：每个待定点对应的模型空间坐标（视口外为 null），与 pending 一一对应 */
    private val pendingModel = ArrayList<Pair<Double, Double>?>()
    /** 一次性提示（如"未点到圆弧"），下一次点击时清除 */
    private var tapHint: String? = null
    val measurements = ArrayList<Measurement>()
    var onMeasurementChanged: ((String) -> Unit)? = null

    // 图元拾取（选择模式点图元看属性）
    var onEntityPicked: ((String?) -> Unit)? = null
    private var picked: SkdwDocument.Entity? = null

    // 编号/照片标注
    val pins = ArrayList<NotePin>()
    private var nextPinId = 1
    /** 新增 pin 后回调（宿主页弹备注输入框） */
    var onPinAdded: ((NotePin) -> Unit)? = null
    /** 选择模式点到 pin 回调 */
    var onPinTapped: ((NotePin) -> Unit)? = null
    /** 照片标注：用户在 world 点请求选照片 */
    var onPhotoRequested: ((Pair<Double, Double>) -> Unit)? = null

    // 用户绘制标注（文字/直线/任意线/云线/引线）
    val annotations = ArrayList<Annotation>()
    private var nextAnnoId = 1
    /** 文字/引线需要输入内容时回调（第二参数为位置，返回后调 addAnnotation） */
    var onTextInput: ((MeasureMode, Pair<Double, Double>) -> Unit)? = null
    /** 手绘/多点绘制进行中的点（屏幕坐标世界值） */
    private val sketchPts = ArrayList<Pair<Double, Double>>()
    /** 引线待输入文字的标注（输完由宿主回填 text） */
    var pendingLeader: Annotation? = null

    /** 测量换算比例（定距标定/比例设置），显示值 = 模型值 × unitScale */
    var unitScale = 1.0
        set(value) { field = value; updateResultPanel(); invalidate() }

    fun addPin(x: Double, y: Double, note: String, photo: String? = null): NotePin {
        val p = NotePin(nextPinId++, x, y, note, photo)
        pins.add(p)
        invalidate()
        return p
    }

    fun removePin(pin: NotePin) { pins.remove(pin); invalidate() }

    fun clearPins() { pins.clear(); invalidate() }

    fun setNextPinId(v: Int) { nextPinId = v }

    fun addAnnotation(kind: MeasureMode, points: List<Pair<Double, Double>>,
                      text: String = ""): Annotation {
        val a = Annotation(nextAnnoId++, kind, ArrayList(points), text)
        annotations.add(a)
        updateResultPanel()
        invalidate()
        return a
    }

    fun removeAnnotation(a: Annotation) { annotations.remove(a); invalidate() }

    fun clearAnnotations() { annotations.clear(); invalidate() }

    fun setNextAnnoId(v: Int) { nextAnnoId = v }

    fun centerOnPin(pin: NotePin) { centerOn(pin.x, pin.y) }

    /** 选择模式命中 pin（屏幕 34px 内） */
    fun pinAt(world: Pair<Double, Double>): NotePin? {
        val px = worldToScreenX(world.first)
        val py = worldToScreenY(world.second)
        for (p in pins.asReversed()) {
            if (hypot(px - worldToScreenX(p.x), py - worldToScreenY(p.y)) <= 34.0) return p
        }
        return null
    }

    // 显示设置（设置面板可调）
    /** 白底模式：背景纯白，图元用原色不做提亮 */
    var lightBackground = false
        set(value) { field = value; invalidate() }
    /** 标注/测量颜色 */
    var measureColor = 0xFF31D2A0.toInt()
        set(value) {
            field = value
            measurePaint.color = value
            measureTextPaint.color = value
            measureFill.color = (value and 0x00FFFFFF) or 0x2E000000
            invalidate()
        }

    // 画笔
    private val linePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.NORMAL)
    }
    private val measurePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = 0xFF31D2A0.toInt()
    }
    private val measureTextPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF31D2A0.toInt()
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.NORMAL)
    }
    private val measureFill = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = 0x2E31D2A0
    }
    private val snapPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = 0xFFFFC94D.toInt()
    }
    private val infoPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF9AA3AE.toInt()
        textSize = 28f
    }
    private val pinPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val pinStroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2.5f
        color = 0xFFFFFFFF.toInt()
    }
    private val pinTextPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFFFFFFF.toInt()
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
    }

    // 手势
    private var scaleFactor = 1.15f
    private lateinit var scaleDetector: ScaleGestureDetector
    private lateinit var gestureDetector: GestureDetector
    private var snapPoint: Pair<Double, Double>? = null

    init {
        scaleDetector = ScaleGestureDetector(context,
            object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
                override fun onScale(detector: ScaleGestureDetector): Boolean {
                    val focus = screenToWorld(detector.focusX, detector.focusY)
                    scale = (scale * detector.scaleFactor).coerceIn(minScale(), maxScale())
                    // 保持焦点世界坐标不动
                    val after = screenToWorld(detector.focusX, detector.focusY)
                    centerX += focus.first - after.first
                    centerY += focus.second - after.second
                    invalidate()
                    return true
                }

                override fun onScaleBegin(detector: ScaleGestureDetector) = true
            }).apply {
            // 关闭「双击后拖动缩放」：与双击适配全图冲突，容易误触
            isQuickScaleEnabled = false
        }
        gestureDetector = GestureDetector(context,
            object : GestureDetector.SimpleOnGestureListener() {
                override fun onDown(e: MotionEvent) = true

                override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                    onTap(screenToWorld(e.x, e.y))
                    return true
                }

                override fun onDoubleTap(e: MotionEvent): Boolean {
                    if (mode == MeasureMode.NONE) fitToView(!isFittedFull)
                    return true
                }
            })
    }

    // ---------------------------------------------------------- 数据装载

    fun load(doc: SkdwDocument?) {
        document = doc
        fitted = false
        measurements.clear()
        pending.clear()
        pendingModel.clear()
        picked = null
        grid = buildIndex(doc)
        android.util.Log.i("SkdwRender",
            "load ents=${doc?.entities?.size} layers=${doc?.layers?.size} " +
            "full=[${doc?.minX},${doc?.minY}~${doc?.maxX},${doc?.maxY}] " +
            "main=[${doc?.mainMinX},${doc?.mainMinY}~${doc?.mainMaxX},${doc?.mainMaxY}] " +
            "grid=${grid != null}")
        invalidate()
    }

    /** 布局模式：设置模型空间叠加文档（视口内容来源） */
    fun setOverlay(doc: SkdwDocument?) {
        overlayDoc = doc
        overlayGrid = buildIndex(doc)
        invalidate()
    }

    fun setMode(m: MeasureMode) {
        mode = m
        pending.clear()
        pendingModel.clear()
        sketchPts.clear()
        tapHint = null
        picked = null
        updateResultPanel()
        invalidate()
    }

    fun clearMeasurements() {
        measurements.clear()
        pending.clear()
        pendingModel.clear()
        updateResultPanel()
        invalidate()
    }

    /** 选择模式点图元：拾取最近图元，回调属性描述（null=没点到，恢复测量信息面板） */
    private fun pickEntity(world: Pair<Double, Double>) {
        val doc = document
        if (doc == null) { onEntityPicked?.invoke(null); return }
        val radiusW = 22.0 / scale
        var best: SkdwDocument.Entity? = null
        var bestD = radiusW
        var bestDoc: SkdwDocument = doc

        fun geometryDist(e: SkdwDocument.Entity, wx: Double, wy: Double): Double? {
            if (wx < e.bbox[0] - radiusW || wx > e.bbox[2] + radiusW ||
                wy < e.bbox[1] - radiusW || wy > e.bbox[3] + radiusW) return null
            fun segDist(x1: Double, y1: Double, x2: Double, y2: Double): Double {
                val dx = x2 - x1; val dy = y2 - y1
                val len2 = dx * dx + dy * dy
                val t = if (len2 < 1e-18) 0.0
                        else ((wx - x1) * dx + (wy - y1) * dy).coerceIn(0.0, 1.0) / len2
                return hypot(wx - (x1 + t * dx), wy - (y1 + t * dy))
            }
            return when (e) {
                is SkdwDocument.Entity.Line ->
                    segDist(e.x1, e.y1, e.x2, e.y2)
                is SkdwDocument.Entity.Circle ->
                    abs(hypot(wx - e.cx, wy - e.cy) - e.r)
                is SkdwDocument.Entity.Arc ->
                    abs(hypot(wx - e.cx, wy - e.cy) - e.r)
                is SkdwDocument.Entity.Polyline -> {
                    var d = Double.MAX_VALUE
                    val n = e.xs.size
                    for (i in 0 until n) {
                        val j = if (e.closed) (i + 1) % n else i + 1
                        if (j >= n && !e.closed) break
                        d = min(d, segDist(e.xs[i], e.ys[i], e.xs[j], e.ys[j]))
                    }
                    if (n == 1) hypot(wx - e.xs[0], wy - e.ys[0]) else d
                }
                is SkdwDocument.Entity.Text -> {
                    // 文字按 bbox 距离
                    val dx = max(max(e.bbox[0] - wx, wx - e.bbox[2]), 0.0)
                    val dy = max(max(e.bbox[1] - wy, wy - e.bbox[3]), 0.0)
                    hypot(dx, dy)
                }
                else -> null
            }
        }

        fun search(doc2: SkdwDocument, grid2: GridIndex?, wx: Double, wy: Double) {
            val cand = ArrayList<SkdwDocument.Entity>()
            if (grid2 != null) {
                val (cs, rs) = grid2.range(wx - radiusW, wy - radiusW,
                                           wx + radiusW, wy + radiusW)
                for (r in rs.first..rs.second) {
                    val base = r * grid2.cols
                    for (c in cs.first..cs.second)
                        cand.addAll(grid2.cells[base + c])
                }
                cand.addAll(grid2.big)   // 跨格大图元单独列表
            } else {
                cand.addAll(doc2.entities)
            }
            for (e in cand) {
                val d = geometryDist(e, wx, wy) ?: continue
                if (d < bestD) { bestD = d; best = e; bestDoc = doc2 }
            }
        }

        // 布局模式：视口内优先拾取模型图元（映射到模型坐标再找），其次纸面图元
        val model = overlayDoc
        if (model != null) {
            val mp = paperPointToModel(world.first, world.second)
            if (mp != null) search(model, overlayGrid, mp.first, mp.second)
            search(doc, grid, world.first, world.second)
        } else {
            search(doc, grid, world.first, world.second)
        }

        picked = best
        val pickedDoc: SkdwDocument = bestDoc
        onEntityPicked?.invoke(best?.let { describeEntity(it, pickedDoc) })
        invalidate()
    }

    /** 图元属性描述（选择面板用） */
    private fun describeEntity(e: SkdwDocument.Entity, doc: SkdwDocument): String {
        val layerName = doc.layers.getOrNull(e.layer)?.name ?: "?"
        val type = when (e) {
            is SkdwDocument.Entity.Line -> "直线"
            is SkdwDocument.Entity.Circle -> "圆"
            is SkdwDocument.Entity.Arc -> "圆弧"
            is SkdwDocument.Entity.Polyline ->
                if (e.closed) "多段线（闭合）" else "多段线"
            is SkdwDocument.Entity.Text -> "文字"
            is SkdwDocument.Entity.Viewport -> "视口"
        }
        val sb = StringBuilder(type)
        when (e) {
            is SkdwDocument.Entity.Line ->
                sb.append("｜长度 ").append("%.2f".format(
                    hypot(e.x2 - e.x1, e.y2 - e.y1)))
            is SkdwDocument.Entity.Circle ->
                sb.append("｜R ").append("%.2f".format(e.r))
                    .append("｜⌀ ").append("%.2f".format(e.r * 2))
            is SkdwDocument.Entity.Arc -> {
                var sweep = e.a1 - e.a0
                while (sweep <= 0) sweep += 2 * Math.PI
                sb.append("｜R ").append("%.2f".format(e.r))
                    .append("｜弧长 ").append("%.2f".format(e.r * sweep))
            }
            is SkdwDocument.Entity.Polyline -> {
                var len = 0.0
                val n = e.xs.size
                for (i in 0 until n) {
                    val j = if (e.closed) (i + 1) % n else i + 1
                    if (j >= n && !e.closed) break
                    len += hypot(e.xs[j] - e.xs[i], e.ys[j] - e.ys[i])
                }
                sb.append("｜顶点 ").append(n).append("｜长度 ").append("%.2f".format(len))
            }
            is SkdwDocument.Entity.Text ->
                sb.append("｜「").append(e.text.replace("\n", " / ")).append("」")
            is SkdwDocument.Entity.Viewport -> {}
        }
        val colorStr = if (e.color >= 0) "%06X".format(e.color and 0xffffff)
                       else "随层"
        sb.append("｜颜色 ").append(colorStr)
        sb.append("｜图层 ").append(layerName)
        return sb.toString()
    }

    /** 最近的圆弧图元（弧长测量用） */
    private fun nearestArc(doc: SkdwDocument, wx: Double, wy: Double)
            : SkdwDocument.Entity.Arc? {
        val radiusW = 22.0 / scale
        var best: SkdwDocument.Entity.Arc? = null
        var bestD = radiusW
        for (e in doc.entities) {
            if (e !is SkdwDocument.Entity.Arc) continue
            if (wx < e.bbox[0] - radiusW || wx > e.bbox[2] + radiusW ||
                wy < e.bbox[1] - radiusW || wy > e.bbox[3] + radiusW) continue
            val d = abs(hypot(wx - e.cx, wy - e.cy) - e.r)
            if (d < bestD) { bestD = d; best = e }
        }
        return best
    }

    /** 弧长测量：记录圆心/起止点/中点 + 弧长/半径/角度 */
    private fun commitArc(a: SkdwDocument.Entity.Arc, fromModel: Boolean) {
        var sweep = a.a1 - a.a0
        while (sweep <= 0) sweep += 2 * Math.PI
        val mid = a.a0 + sweep / 2
        fun pt(ang: Double) =
            Pair(a.cx + a.r * Math.cos(ang), a.cy + a.r * Math.sin(ang))
        var pts = listOf(Pair(a.cx, a.cy), pt(a.a0), pt(a.a1), pt(mid))
        if (fromModel) {
            // 布局页从视口内选到的模型弧：映射回纸面坐标绘制
            pts = pts.map { modelPointToPaper(it.first, it.second) ?: it }
        }
        measurements.add(Measurement(MeasureMode.ARC, pts, emptyList(),
            a.r * sweep, 0.0, 0.0, a.r,
            extra = "%.1f".format(Math.toDegrees(sweep))))
        updateResultPanel()
        invalidate()
    }

    /** 布局模式：模型空间点 → 纸面坐标（优先取包含该点的视口） */
    fun modelPointToPaper(mx: Double, my: Double): Pair<Double, Double>? {
        val doc = document ?: return null
        val model = overlayDoc ?: return null
        var fallback: Pair<Double, Double>? = null
        for (e in doc.entities.asReversed()) {
            if (e !is SkdwDocument.Entity.Viewport) continue
            val t = effectiveVpXform(model, e)
            if (abs(t.det) < 1e-12) continue
            val px = t.m11 * mx + t.m21 * my + t.dx
            val py = t.m12 * mx + t.m22 * my + t.dy
            if (px in e.bbox[0]..e.bbox[2] && py in e.bbox[1]..e.bbox[3])
                return Pair(px, py)
            if (fallback == null) fallback = Pair(px, py)
        }
        return fallback
    }

    /** full=false 适配主体范围（默认，打开即见主图）；full=true 适配全图含杂散图元 */
    var isFittedFull = false
        private set

    fun fitToView(full: Boolean = false) {
        val doc = document ?: return
        val useMain = !full && doc.mainMaxX > doc.mainMinX && doc.mainMaxY > doc.mainMinY
        val x0 = if (useMain) doc.mainMinX else doc.minX
        val y0 = if (useMain) doc.mainMinY else doc.minY
        val x1 = if (useMain) doc.mainMaxX else doc.maxX
        val y1 = if (useMain) doc.mainMaxY else doc.maxY
        val w = x1 - x0
        val h = y1 - y0
        if (w <= 0 || h <= 0) return
        centerX = (x0 + x1) / 2
        centerY = (y0 + y1) / 2
        scale = min(width / w, height / h) * 0.96
        fitted = true
        isFittedFull = full
        android.util.Log.i("SkdwRender",
            "fit full=$full scale=$scale center=($centerX,$centerY) view=${width}x$height")
        invalidate()
    }

    private fun maxScale() = max(4.0, baseFitScale() * 400)
    private fun minScale() = baseFitScale() / 8

    /** 以整张图纸（全图）适配屏幕的缩放为基准：缩放上下限按它计算。
     *  之前用「上次适配值」当基准，主体适配后按全图时需要的缩放
     *  比基准的 1/8 还小，被 minScale 顶住 → 全图后只剩一小块内容 */
    private fun baseFitScale(): Double {
        val doc = document ?: return 1.0
        val w = max(doc.maxX - doc.minX, 1.0)
        val h = max(doc.maxY - doc.minY, 1.0)
        if (width <= 0 || height <= 0) return 1.0
        return min(width / w, height / h)
    }

    // ---------------------------------------------------------- 坐标变换

    private fun worldToScreenX(wx: Double) = (wx - centerX) * scale + width / 2.0
    private fun worldToScreenY(wy: Double) = height / 2.0 - (wy - centerY) * scale

    /** 白纸模式：近白色（ACI 7）反转为黑色，其余颜色保持原色 */
    private fun onPaperColor(argb: Int): Int {
        val r = (argb shr 16) and 0xff
        val g = (argb shr 8) and 0xff
        val b = argb and 0xff
        return if (r > 235 && g > 235 && b > 235) 0xFF1A1A1A.toInt() else argb
    }
    private fun screenToWorld(sx: Float, sy: Float): Pair<Double, Double> =
        Pair((sx - width / 2.0) / scale + centerX,
             (height / 2.0 - sy) / scale + centerY)

    // ---------------------------------------------------------- 测量交互

    private fun onTap(world: Pair<Double, Double>) {
        tapHint = null
        if (mode == MeasureMode.NONE) {
            val pin = pinAt(world)
            if (pin != null) onPinTapped?.invoke(pin)
            else pickEntity(world)
            return
        }
        val snapped = findSnap(world) ?: world
        // 布局模式：纸面点映射到模型空间（测量取真实图上尺寸）
        val modelPt = if (overlayDoc != null)
            paperPointToModel(snapped.first, snapped.second) else null
        when (mode) {
            MeasureMode.LENGTH -> {
                pending.add(snapped); pendingModel.add(modelPt)
                if (pending.size == 2) commit(MeasureMode.LENGTH)
            }
            MeasureMode.ANGLE -> {
                // 第1点=顶点，第2/3点=两边端点（角度与比例无关，无需模型映射）
                pending.add(snapped)
                if (pending.size == 3) commit(MeasureMode.ANGLE)
            }
            MeasureMode.ARC -> {
                // 点选最近的圆弧图元
                val target = overlayDoc
                val arc = if (target != null) {
                    val wp = paperPointToModel(world.first, world.second)
                    nearestArc(target, wp?.first ?: world.first, wp?.second ?: world.second)
                        ?: document?.let { nearestArc(it, world.first, world.second) }
                } else {
                    document?.let { nearestArc(it, world.first, world.second) }
                }
                if (arc != null) commitArc(arc, target != null)
                else tapHint = "未点到圆弧，请靠近弧线点击"
            }
            MeasureMode.RADIUS -> {
                // 点选圆或圆弧，测半径
                val doc = document
                if (doc != null) {
                    val target = overlayDoc
                    var hitR = 0.0
                    var hitC: Pair<Double, Double>? = null
                    fun findNear(dd: SkdwDocument, wx: Double, wy: Double) {
                        val rw = 22.0 / scale
                        for (e in dd.entities) {
                            if (e !is SkdwDocument.Entity.Circle &&
                                e !is SkdwDocument.Entity.Arc) continue
                            val r = if (e is SkdwDocument.Entity.Circle) e.r else (e as SkdwDocument.Entity.Arc).r
                            val cx = if (e is SkdwDocument.Entity.Circle) e.cx else (e as SkdwDocument.Entity.Arc).cx
                            val cy = if (e is SkdwDocument.Entity.Circle) e.cy else (e as SkdwDocument.Entity.Arc).cy
                            if (wx < e.bbox[0] - rw || wx > e.bbox[2] + rw ||
                                wy < e.bbox[1] - rw || wy > e.bbox[3] + rw) continue
                            if (abs(hypot(wx - cx, wy - cy) - r) <= rw) {
                                hitR = r; hitC = Pair(cx, cy); return
                            }
                        }
                    }
                    if (target != null) {
                        val wp = paperPointToModel(world.first, world.second)
                        findNear(target, wp?.first ?: world.first, wp?.second ?: world.second)
                        if (hitC == null) findNear(doc, world.first, world.second)
                    } else {
                        findNear(doc, world.first, world.second)
                    }
                    val c = hitC
                    if (c != null && hitR > 0) {
                        val draw = if (target != null)
                            modelPointToPaper(c.first, c.second) ?: c
                        else c
                        measurements.add(Measurement(MeasureMode.RADIUS,
                            listOf(draw, Pair(draw.first + hitR, draw.second)),
                            emptyList(), 0.0, 0.0, 0.0, hitR))
                        updateResultPanel()
                    } else tapHint = "未点到圆或圆弧"
                }
            }
            MeasureMode.PIN -> {
                addPin(snapped.first, snapped.second, "")
                    .let { onPinAdded?.invoke(it) }
            }
            MeasureMode.PHOTO -> {
                onPhotoRequested?.invoke(snapped)
            }
            MeasureMode.COORD -> {
                pending.add(snapped); pendingModel.add(modelPt)
                commit(MeasureMode.COORD)
            }
            MeasureMode.AREA, MeasureMode.CONTINUOUS -> {
                // 靠近起点自动闭合（连续测量同样以闭合结束）
                if (pending.size >= 3) {
                    val first = pending.first()
                    val sx = worldToScreenX(first.first).toFloat()
                    val sy = worldToScreenY(first.second).toFloat()
                    val tx = worldToScreenX(snapped.first).toFloat()
                    val ty = worldToScreenY(snapped.second).toFloat()
                    if (hypot(tx - sx, ty - sy) < 40f) {
                        commit(mode)
                        return
                    }
                }
                pending.add(snapped)
                pendingModel.add(modelPt)
            }
            MeasureMode.LINE -> {
                pending.add(snapped)
                if (pending.size == 2) {
                    annotations.add(Annotation(nextAnnoId++,
                        MeasureMode.LINE, ArrayList(pending)))
                    pending.clear()
                    updateResultPanel()
                }
            }
            MeasureMode.CLOUD -> {
                // 云线：多点绘制，双击/靠近起点结束
                if (pending.size >= 3) {
                    val first = pending.first()
                    val sx = worldToScreenX(first.first).toFloat()
                    val sy = worldToScreenY(first.second).toFloat()
                    val tx = worldToScreenX(snapped.first).toFloat()
                    val ty = worldToScreenY(snapped.second).toFloat()
                    if (hypot(tx - sx, ty - sy) < 40f) {
                        annotations.add(Annotation(nextAnnoId++,
                            MeasureMode.CLOUD, ArrayList(pending)))
                        pending.clear()
                        updateResultPanel()
                        invalidate()
                        return
                    }
                }
                pending.add(snapped)
            }
            MeasureMode.SKETCH -> {}   // 手绘走 onTouchEvent 的 MOVE 采样
            MeasureMode.TEXT -> {
                onTextInput?.invoke(MeasureMode.TEXT, snapped)
            }
            MeasureMode.LEADER -> {
                // 第一点=箭头端，第二点=文字端；先落标注再弹输入
                pending.add(snapped)
                if (pending.size == 2) {
                    val a = Annotation(nextAnnoId++,
                        MeasureMode.LEADER, ArrayList(pending))
                    annotations.add(a)
                    pendingLeader = a
                    onTextInput?.invoke(MeasureMode.LEADER, pending[1])
                    pending.clear()
                }
            }
            MeasureMode.NONE -> {}
        }
        updateResultPanel()
        invalidate()
    }

    private fun commit(kind: MeasureMode) {
        val pts = ArrayList(pending)
        val mpts = ArrayList<Pair<Double, Double>?>(pendingModel)
        pending.clear()
        pendingModel.clear()
        if (pts.size < 2 && kind != MeasureMode.COORD) return
        /** 两点距离：布局模式两点都在视口内取模型空间距离（真实尺寸），否则纸面距离 */
        fun dist(i: Int, j: Int): Double {
            val a = mpts.getOrNull(i); val b = mpts.getOrNull(j)
            val pa = pts[i]; val pb = pts[j]
            return if (a != null && b != null)
                hypot(b.first - a.first, b.second - a.second)
            else hypot(pb.first - pa.first, pb.second - pa.second)
        }
        val edges = ArrayList<Double>()
        val n = pts.size
        var perimeter = 0.0
        var closed = kind == MeasureMode.AREA
        var total = 0.0
        var area = 0.0
        var radius = 0.0
        when (kind) {
            MeasureMode.ANGLE -> {
                // 角度与比例无关，标注绘制时由三点计算
            }
            MeasureMode.COORD -> {
                // 坐标值优先取模型空间（布局页视口内为图面真实坐标）
                val p = mpts.getOrNull(0) ?: pts[0]
                measurements.add(Measurement(MeasureMode.COORD, pts, edges,
                    0.0, 0.0, 0.0, 0.0,
                    extra = "X %.1f\nY %.1f".format(p.first, p.second)))
                updateResultPanel()
                invalidate()
                return
            }
            MeasureMode.LENGTH -> {
                val len = dist(1, 0)
                edges.add(len); total = len
            }
            MeasureMode.CONTINUOUS -> {
                for (i in 1 until n) {
                    val len = dist(i, i - 1)
                    edges.add(len); total += len
                }
            }
            MeasureMode.AREA -> {
                for (i in 1 until n) {
                    val len = dist(i, i - 1)
                    edges.add(len); total += len
                }
                // 闭合边
                val closing = dist(0, n - 1)
                edges.add(closing); total += closing; perimeter = total
                // 使用桌面端共享的 C 几何核心；布局模式优先取模型坐标。
                val useModel = mpts.size == n && mpts.all { it != null }
                val source = if (useModel) mpts.map { it!! } else pts
                val xs = DoubleArray(n) { source[it].first }
                val ys = DoubleArray(n) { source[it].second }
                area = GeometryCore.polygonArea(xs, ys)
                perimeter = GeometryCore.polygonPerimeter(xs, ys)
            }
            MeasureMode.NONE, MeasureMode.ARC, MeasureMode.RADIUS,
            MeasureMode.PIN, MeasureMode.PHOTO,
            MeasureMode.TEXT, MeasureMode.LINE, MeasureMode.SKETCH,
            MeasureMode.CLOUD, MeasureMode.LEADER -> {}
        }
        measurements.add(Measurement(kind, pts, edges, total, area, perimeter, radius))
        updateResultPanel()
        invalidate()
    }

    private fun updateResultPanel() {
        val sb = StringBuilder()
        tapHint?.let { sb.append(it) }
        if (pending.isNotEmpty()) {
            if (sb.isNotEmpty()) sb.append("\n")
            sb.append("进行中：").append(pending.size).append(" 点（靠起点闭合）")
        }
        measurements.takeLast(3).reversed().forEach { m ->
            if (sb.isNotEmpty()) sb.append("\n")
            sb.append(m.label(unitScale).replace("\n", "，"))
        }
        onMeasurementChanged?.invoke(sb.toString())
    }

    /** 删除最后一条指定类型的测量（定距标定取消/完成后清理） */
    fun removeLastMeasurement(kind: MeasureMode) {
        for (i in measurements.indices.reversed())
            if (measurements[i].kind == kind) { measurements.removeAt(i); break }
        updateResultPanel()
        invalidate()
    }

    /** 供宿主页在拾取取消后恢复测量信息面板 */
    fun refreshInfoPanel() = updateResultPanel()

    /** 当前视图状态（centerX, centerY, scale）；未适配过时 null */
    fun viewState(): Triple<Double, Double, Double>? =
        if (fitted) Triple(centerX, centerY, scale) else null

    /** 恢复上次浏览位置（按图纸记忆） */
    fun restoreView(cx: Double, cy: Double, s: Double) {
        centerX = cx
        centerY = cy
        scale = s.coerceIn(minScale(), maxScale())
        fitted = true
        invalidate()
    }

    /** 把世界坐标点移到屏幕中心（文字查找定位等） */
    fun centerOn(x: Double, y: Double) {
        if (!fitted) fitToView()
        centerX = x
        centerY = y
        invalidate()
    }

    /** 当前文档中名称含关键词的文字图元（最多返回 limit 个） */
    fun findTexts(query: String, limit: Int = 50)
            : List<Pair<String, Pair<Double, Double>>> {
        val doc = document ?: return emptyList()
        val q = query.lowercase()
        if (q.isEmpty()) return emptyList()
        val out = ArrayList<Pair<String, Pair<Double, Double>>>()
        for (e in doc.entities) {
            if (e is SkdwDocument.Entity.Text && e.text.lowercase().contains(q)) {
                out.add(Pair(e.text.replace("\n", " / "),
                    Pair(e.x, e.y)))
                if (out.size >= limit) break
            }
        }
        return out
    }

    /** 全部文字内容（提取文字用，按绘制顺序去空去重） */
    fun allTexts(doc: SkdwDocument? = document, limit: Int = 3000): List<String> {
        val d = doc ?: return emptyList()
        val seen = HashSet<String>()
        val out = ArrayList<String>()
        for (e in d.entities) {
            if (e !is SkdwDocument.Entity.Text) continue
            val t = e.text.trim()
            if (t.isEmpty() || !seen.add(t)) continue
            out.add(t)
            if (out.size >= limit) break
        }
        return out
    }

    /** 端点/顶点吸附：像素半径内找最近的特征点 */
    private fun findSnap(world: Pair<Double, Double>): Pair<Double, Double>? {
        val doc = document ?: return null
        val model = overlayDoc
        if (model != null) {
            // 布局模式：优先吸附所在视口内的模型特征点（映射回纸面坐标）
            for (e in doc.entities.asReversed()) {
                if (e !is SkdwDocument.Entity.Viewport) continue
                if (world.first < e.bbox[0] || world.first > e.bbox[2] ||
                    world.second < e.bbox[1] || world.second > e.bbox[3]) continue
                val t = effectiveVpXform(model, e)
                val det = t.det
                if (abs(det) < 1e-12) continue
                val invx = (t.m22 * (world.first - t.dx) -
                            t.m21 * (world.second - t.dy)) / det
                val invy = (-t.m12 * (world.first - t.dx) +
                            t.m11 * (world.second - t.dy)) / det
                val s = hypot(t.m11, t.m12)
                val rModel = 22.0 / (scale * max(s, 1e-9))
                var best: Pair<Double, Double>? = null
                var bestD = rModel
                fun consider(mx: Double, my: Double) {
                    val d = hypot(mx - invx, my - invy)
                    if (d < bestD) { bestD = d; best = Pair(mx, my) }
                }
                val cand = ArrayList<SkdwDocument.Entity>()
                val idx = overlayGrid
                if (idx != null) {
                    val (cs, rs) = idx.range(invx - rModel, invy - rModel,
                                             invx + rModel, invy + rModel)
                    for (r in rs.first..rs.second) {
                        val base = r * idx.cols
                        for (c in cs.first..cs.second)
                            cand.addAll(idx.cells[base + c])
                    }
                    cand.addAll(idx.big)
                } else {
                    cand.addAll(model.entities)
                }
                for (me in cand) {
                    if (me.bbox[2] < invx - rModel || me.bbox[0] > invx + rModel ||
                        me.bbox[3] < invy - rModel || me.bbox[1] > invy + rModel) continue
                    when (me) {
                        is SkdwDocument.Entity.Line -> {
                            consider(me.x1, me.y1); consider(me.x2, me.y2)
                        }
                        is SkdwDocument.Entity.Polyline -> {
                            for (i in me.xs.indices) consider(me.xs[i], me.ys[i])
                        }
                        is SkdwDocument.Entity.Circle -> consider(me.cx, me.cy)
                        is SkdwDocument.Entity.Arc -> consider(me.cx, me.cy)
                        else -> {}
                    }
                }
                if (best != null) {
                    val (mx, my) = best!!
                    return Pair(t.m11 * mx + t.m21 * my + t.dx,
                                t.m12 * mx + t.m22 * my + t.dy)
                }
            }
            return findSnapIn(doc, world)
        }
        return findSnapIn(doc, world)
    }

    /** 模型空间文档的特征点吸附（普通模型模式） */
    private fun findSnapIn(doc: SkdwDocument,
                           world: Pair<Double, Double>): Pair<Double, Double>? {
        val radiusPx = 22.0
        var best: Pair<Double, Double>? = null
        var bestDist = radiusPx / scale
        val wx = world.first; val wy = world.second
        fun consider(px: Double, py: Double) {
            val d = hypot(px - wx, py - wy)
            if (d < bestDist) { bestDist = d; best = Pair(px, py) }
        }
        for (e in doc.entities) {
            // 快速剔除：bbox 距离
            if (wx < e.bbox[0] - bestDist || wx > e.bbox[2] + bestDist ||
                wy < e.bbox[1] - bestDist || wy > e.bbox[3] + bestDist) continue
            when (e) {
                is SkdwDocument.Entity.Line -> {
                    consider(e.x1, e.y1); consider(e.x2, e.y2)
                }
                is SkdwDocument.Entity.Polyline -> {
                    for (i in e.xs.indices) consider(e.xs[i], e.ys[i])
                }
                is SkdwDocument.Entity.Circle -> consider(e.cx, e.cy)
                is SkdwDocument.Entity.Arc -> consider(e.cx, e.cy)
                else -> {}
            }
        }
        return best
    }

    // ---------------------------------------------------------- 绘制

    // 拖动（1:1 跟手，无惯性——手指动多少图动多少，松手即停）
    private var lastTouchX = 0f

    private var lastTouchY = 0f

    /** 手指拖动中：用更宽松的 LOD 保证跟手 */
    private var dragging = false

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(if (lightBackground) 0xFFFFFFFF.toInt() else 0xFF101318.toInt())
        val doc = document ?: run {
            canvas.drawText("图纸未加载", 40f, height - 60f, infoPaint)
            return
        }
        if (!fitted) fitToView()

        val visible = visibleWorldRect()
        val strokeW = (scale * 0.18).toFloat().coerceIn(1f, 6f)
        linePaint.strokeWidth = strokeW

        val path = Path()
        val cells = grid
        if (cells != null) {
            // 网格索引：只遍历视口覆盖的桶
            val (cs, rs) = cells.range(visible.first, visible.third,
                visible.second, visible.fourth)
            val (c0, c1) = cs
            val (r0, r1) = rs
            for (r in r0..r1) {
                val base = r * cells.cols
                for (c in c0..c1) {
                    for (e in cells.cells[base + c]) {
                        drawEntity(canvas, path, doc, e, visible)
                    }
                }
            }
            for (e in cells.big) drawEntity(canvas, path, doc, e, visible)
        } else {
            for (e in doc.entities) drawEntity(canvas, path, doc, e, visible)
        }
        drawMeasurements(canvas)
        drawAnnotations(canvas)
        drawPins(canvas)
        // 拾取高亮：黄色虚线框出选中图元 bbox
        picked?.let {
            snapPaint.color = 0xFFFFC94D.toInt()
            val pad = 4f
            canvas.drawRect(
                worldToScreenX(it.bbox[0]).toFloat() - pad,
                worldToScreenY(it.bbox[3]).toFloat() - pad,
                worldToScreenX(it.bbox[2]).toFloat() + pad,
                worldToScreenY(it.bbox[1]).toFloat() + pad, snapPaint)
            snapPaint.color = 0xFFFFC94D.toInt()
        }
        snapPoint?.let {
            val sx = worldToScreenX(it.first).toFloat()
            val sy = worldToScreenY(it.second).toFloat()
            canvas.drawRect(sx - 8, sy - 8, sx + 8, sy + 8, snapPaint)
        }
    }

    private fun drawEntity(canvas: Canvas, path: Path, doc: SkdwDocument,
                           e: SkdwDocument.Entity,
                           visible: Quad<Double, Double, Double, Double>) {
        if (e.bbox[2] < visible.first || e.bbox[0] > visible.second ||
            e.bbox[3] < visible.third || e.bbox[1] > visible.fourth) return
        // LOD：屏幕上小于阈值的图元跳过（拖动/惯性中放宽，保证跟手）
        val sx0 = worldToScreenX(e.bbox[0])
        val sx1 = worldToScreenX(e.bbox[2])
        val sy0 = worldToScreenY(e.bbox[3])
        val sy1 = worldToScreenY(e.bbox[1])
        // 全图/远景（scale 接近全图适配值）时阈值降到 0.25px：一面墙只有
        // 0.5~1px，按 0.8 阈值会把整张图所有线条剔光 = “图形不全”
        val lod = when {
            dragging -> 2.2
            scale <= baseFitScale() * 1.05 -> 0.25
            else -> 0.8
        }
        // 文字不做包围盒 LOD：缩小后仍按最小字号绘制（轴线号/房间名保持可见）
        if (e !is SkdwDocument.Entity.Text && sx1 - sx0 < lod && sy1 - sy0 < lod) return
        val layerIdx = e.layer
        if (layerIdx >= 0 && layerIdx < doc.layers.size &&
            !doc.layers[layerIdx].visible) return
        // 图元自带颜色是 24 位 RGB（无 alpha），必须补不透明位，
        // 否则 Paint 收到 0x00RRGGBB = 全透明，这些图元在屏幕上一个像素都不画
        val argb = if (e.color >= 0) (e.color or 0xFF000000.toInt())
        else if (layerIdx >= 0 && layerIdx < doc.layers.size)
            doc.layers[layerIdx].argb else 0xFFE6ECF5.toInt()
        // 白底模式：ACI 7（白色）反转为黑色；黑底模式对过暗颜色提亮
        // 布局页与模型页配色统一（不再铺白纸反色）
        linePaint.color = if (lightBackground) onPaperColor(argb)
        else displayColor(argb)

        when (e) {
            is SkdwDocument.Entity.Line -> {
                canvas.drawLine(worldToScreenX(e.x1).toFloat(),
                    worldToScreenY(e.y1).toFloat(),
                    worldToScreenX(e.x2).toFloat(),
                    worldToScreenY(e.y2).toFloat(), linePaint)
            }
            is SkdwDocument.Entity.Circle -> {
                val cx = worldToScreenX(e.cx).toFloat()
                val cy = worldToScreenY(e.cy).toFloat()
                val r = (e.r * scale).toFloat()
                if (r > 0.5f) canvas.drawCircle(cx, cy, r, linePaint)
            }
            is SkdwDocument.Entity.Arc -> {
                val cx = worldToScreenX(e.cx).toFloat()
                val cy = worldToScreenY(e.cy).toFloat()
                val r = (e.r * scale).toFloat()
                if (r > 0.5f) {
                    var sweep = e.a1 - e.a0
                    while (sweep <= 0) sweep += 2 * Math.PI
                    // 屏幕 Y 向下 → 起始角取 -a1
                    canvas.drawArc(cx - r, cy - r, cx + r, cy + r,
                        (-e.a1 * 180 / Math.PI).toFloat(),
                        (sweep * 180 / Math.PI).toFloat(), false, linePaint)
                }
            }
            is SkdwDocument.Entity.Polyline -> {
                path.reset()
                val n = e.xs.size
                if (n >= 2) {
                    path.moveTo(worldToScreenX(e.xs[0]).toFloat(),
                        worldToScreenY(e.ys[0]).toFloat())
                    for (i in 1 until n) {
                        path.lineTo(worldToScreenX(e.xs[i]).toFloat(),
                            worldToScreenY(e.ys[i]).toFloat())
                    }
                    if (e.closed) path.close()
                    if (e.fill && e.closed) {
                        // 实心填充（SOLID/HATCH solid）：半透明填充+描边
                        fillPaint.color = linePaint.color and 0x00FFFFFF or 0xB0000000.toInt()
                        canvas.drawPath(path, fillPaint)
                    }
                    canvas.drawPath(path, linePaint)
                }
            }
            is SkdwDocument.Entity.Text -> {
                val hPx = e.height * scale
                // 远景不画小字：屏幕高度不足 3.5px 的文字画出来只是黑糊糊一片
                // （几千个房间名/尺寸/编号叠在一起 = 用户说的"文字密集"），
                // 放大到能看清时自然出现。
                if (hPx < 3.5) return
                if (dragging && hPx < 6) return
                textPaint.textSize = hPx.toFloat()
                // 白底模式文字反转黑；黑底模式跟背景（布局页与模型页统一）
                textPaint.color = if (lightBackground) onPaperColor(argb)
                else displayColor(argb)
                val x = worldToScreenX(e.x).toFloat()
                val y = worldToScreenY(e.y).toFloat()
                if (x < -2000 || x > width + 2000 || y < -2000 || y > height + 2000) return
                textPaint.textAlign = when (e.hAlign) {
                    1, 4 -> Paint.Align.CENTER
                    2 -> Paint.Align.RIGHT
                    else -> Paint.Align.LEFT
                }
                val baselineOffset = when (e.vAlign) {
                    2 -> -hPx.toFloat() * 0.35f
                    3 -> -hPx.toFloat() * 0.9f
                    else -> 0f
                }
                if (abs(e.rotationDeg) < 0.01) {
                    drawTextLines(canvas, e.text, x, y + baselineOffset, hPx.toFloat())
                } else {
                    canvas.save()
                    canvas.rotate((-e.rotationDeg).toFloat(), x, y)
                    drawTextLines(canvas, e.text, x, y + baselineOffset, hPx.toFloat())
                    canvas.restore()
                }
            }
            is SkdwDocument.Entity.Viewport -> {
                drawViewport(canvas, path, doc, e)
            }
        }
    }

    /** 单行/多行文字：MTEXT 的 \P 换行按行绘制（此前整串一次画，
     *  换行和后续行全部丢失 = 标注文字不全） */
    private fun drawTextLines(canvas: Canvas, text: String, x: Float, y: Float,
                              hPx: Float) {
        if (text.indexOf('\n') < 0) {
            canvas.drawText(text, x, y, textPaint)
            return
        }
        var base = y
        for (line in text.split('\n')) {
            if (line.isNotEmpty()) canvas.drawText(line, x, base, textPaint)
            base += hPx * 1.35f
        }
    }

    /** 纸面视口：视口内容裁剪到视口矩形内（缩小后模型内容不再溢出到纸面外），
     *  再画边框 */
    private fun drawViewport(canvas: Canvas, path: Path, doc: SkdwDocument,
                             vp: SkdwDocument.Entity.Viewport) {
        val left = worldToScreenX(vp.bbox[0]).toFloat()
        val right = worldToScreenX(vp.bbox[2]).toFloat()
        val bottom = worldToScreenY(vp.bbox[1]).toFloat()
        val top = worldToScreenY(vp.bbox[3]).toFloat()
        val model = overlayDoc
        if (model != null) {
            val sw = linePaint.strokeWidth
            linePaint.strokeWidth = 1f
            val save = canvas.save()
            // 裁剪到视口矩形：视口外的模型图元（跑飞线/相邻图内容）不画
            canvas.clipRect(left, top, right, bottom)
            drawViewportContent(canvas, path, model, vp)
            canvas.restoreToCount(save)
            linePaint.strokeWidth = sw
        }
        // 视口边框（浅色虚线感，识别范围用）
        linePaint.color = if (lightBackground) 0xFFB8BEC8.toInt()
        else 0xFF5A6472.toInt()
        canvas.drawLine(left, top, right, top, linePaint)
        canvas.drawLine(right, top, right, bottom, linePaint)
        canvas.drawLine(right, bottom, left, bottom, linePaint)
        canvas.drawLine(left, bottom, left, top, linePaint)
    }

    /** 视口的有效仿射：反推区域与模型内容完全无交集时才判为坏视口（auto-fit 兜底）。
     *  特写视口（只放大某房间，不含主体中心）必须保留原仿射，否则多视口内容重复。 */
    private fun effectiveVpXform(model: SkdwDocument,
                                 vp: SkdwDocument.Entity.Viewport): VpXform {
        var m11 = vp.m11; var m12 = vp.m12
        var m21 = vp.m21; var m22 = vp.m22
        var dx = vp.dx; var dy = vp.dy
        val det = m11 * m22 - m21 * m12
        if (abs(det) > 1e-12) {
            // 反推视口显示的模型区域，与模型 full bounds 求交集
            fun invX(px: Double, py: Double) =
                (m22 * (px - dx) - m21 * (py - dy)) / det
            fun invY(px: Double, py: Double) =
                (-m12 * (px - dx) + m11 * (py - dy)) / det
            var mnx = Double.MAX_VALUE; var mxx = -Double.MAX_VALUE
            var mny = Double.MAX_VALUE; var mxy = -Double.MAX_VALUE
            for ((px, py) in listOf(
                vp.bbox[0] to vp.bbox[1], vp.bbox[2] to vp.bbox[1],
                vp.bbox[2] to vp.bbox[3], vp.bbox[0] to vp.bbox[3])) {
                val mx = invX(px, py); val my = invY(px, py)
                mnx = min(mnx, mx); mxx = max(mxx, mx)
                mny = min(mny, my); mxy = max(mxy, my)
            }
            // 交集面积近似为 0 → 视口参数完全偏离内容（VIEWCTR 失真）→ 自适应兜底
            val ix = min(mxx, model.maxX) - max(mnx, model.minX)
            val iy = min(mxy, model.maxY) - max(mny, model.minY)
            val modelArea = max(model.maxX - model.minX, 1e-9) *
                    max(model.maxY - model.minY, 1e-9)
            if (ix <= 0 || iy <= 0 || ix * iy < modelArea * 1e-6) {
                val mw = model.mainMaxX - model.mainMinX
                val mh = model.mainMaxY - model.mainMinY
                if (mw > 0 && mh > 0 && vp.w > 0 && vp.h > 0) {
                    val s2 = min(vp.w / mw, vp.h / mh)
                    m11 = s2; m22 = s2; m12 = 0.0; m21 = 0.0
                    dx = vp.cx - s2 * (model.mainMinX + model.mainMaxX) / 2
                    dy = vp.cy - s2 * (model.mainMinY + model.mainMaxY) / 2
                }
            }
        }
        return VpXform(m11, m12, m21, m22, dx, dy)
    }

    /** 布局模式：纸面坐标 → 模型空间坐标（点在某视口内时），否则 null */
    fun paperPointToModel(px: Double, py: Double): Pair<Double, Double>? {
        val doc = document ?: return null
        val model = overlayDoc ?: return null
        for (e in doc.entities.asReversed()) {
            if (e !is SkdwDocument.Entity.Viewport) continue
            if (px < e.bbox[0] || px > e.bbox[2] ||
                py < e.bbox[1] || py > e.bbox[3]) continue
            val t = effectiveVpXform(model, e)
            val det = t.det
            if (abs(det) < 1e-12) continue
            val mx = (t.m22 * (px - t.dx) - t.m21 * (py - t.dy)) / det
            val my = (-t.m12 * (px - t.dx) + t.m11 * (py - t.dy)) / det
            return Pair(mx, my)
        }
        return null
    }

    /** 视口内容：视口矩形反推模型范围，从叠加索引取图元按仿射绘制 */
    private fun drawViewportContent(canvas: Canvas, path: Path,
                                    model: SkdwDocument,
                                    vp: SkdwDocument.Entity.Viewport) {
        val idx = overlayGrid
        val xf = effectiveVpXform(model, vp)
        if (abs(xf.det) < 1e-12) return
        val s = hypot(xf.m11, xf.m12)

        // 反推模型范围（供索引查询）
        val det2 = xf.det
        fun invX(px: Double, py: Double) =
            (xf.m22 * (px - xf.dx) - xf.m21 * (py - xf.dy)) / det2
        fun invY(px: Double, py: Double) =
            (-xf.m12 * (px - xf.dx) + xf.m11 * (py - xf.dy)) / det2
        var mnx = Double.MAX_VALUE; var mxx = -Double.MAX_VALUE
        var mny = Double.MAX_VALUE; var mxy = -Double.MAX_VALUE
        for ((px, py) in listOf(
            vp.bbox[0] to vp.bbox[1], vp.bbox[2] to vp.bbox[1],
            vp.bbox[2] to vp.bbox[3], vp.bbox[0] to vp.bbox[3])) {
            val mx = invX(px, py); val my = invY(px, py)
            mnx = min(mnx, mx); mxx = max(mxx, mx)
            mny = min(mny, my); mxy = max(mxy, my)
        }
        fun draw(e: SkdwDocument.Entity) {
            drawEntityInViewport(canvas, path, model, e, xf, s)
        }
        if (idx != null) {
            val (cs, rs) = idx.range(mnx, mny, mxx, mxy)
            for (r in rs.first..rs.second) {
                val base = r * idx.cols
                for (c in cs.first..cs.second)
                    for (e in idx.cells[base + c]) draw(e)
            }
            for (e in idx.big) draw(e)
        } else {
            for (e in model.entities) draw(e)
        }
    }

    private fun docMainCenterX(doc: SkdwDocument) =
        (doc.mainMinX + doc.mainMaxX) / 2
    private fun docMainCenterY(doc: SkdwDocument) =
        (doc.mainMinY + doc.mainMaxY) / 2

    private class VpXform(val m11: Double, val m12: Double,
                          val m21: Double, val m22: Double,
                          val dx: Double, val dy: Double) {
        val det: Double get() = m11 * m22 - m21 * m12
    }

    /** 视口内的图元绘制：模型坐标经仿射映射到纸面再上屏 */
    private fun drawEntityInViewport(canvas: Canvas, path: Path,
                                     doc: SkdwDocument,
                                     e: SkdwDocument.Entity,
                                     t: VpXform,
                                     s: Double) {
        // 屏幕 LOD：bbox 角点映射后跨度小于阈值跳过（拖动中放宽保证跟手）
        val sx0 = worldToScreenX(t.m11 * e.bbox[0] + t.m21 * e.bbox[1] + t.dx)
        val sx1 = worldToScreenX(t.m11 * e.bbox[2] + t.m21 * e.bbox[3] + t.dx)
        val sy0 = worldToScreenY(t.m12 * e.bbox[0] + t.m22 * e.bbox[1] + t.dy)
        val sy1 = worldToScreenY(t.m12 * e.bbox[2] + t.m22 * e.bbox[3] + t.dy)
        // 全图/远景（scale 接近全图适配值）时阈值降到 0.25px：一面墙只有
        // 0.5~1px，按 0.8 阈值会把整张图所有线条剔光 = “图形不全”
        val lod = when {
            dragging -> 2.2
            scale <= baseFitScale() * 1.05 -> 0.25
            else -> 0.8
        }
        if (abs(sx1 - sx0) < lod && abs(sy1 - sy0) < lod) return
        val layerIdx = e.layer
        if (layerIdx >= 0 && layerIdx < doc.layers.size &&
            !doc.layers[layerIdx].visible) return
        // 同上：24 位 RGB 必须补不透明 alpha，否则视口内图元全透明不绘制
        val argb = if (e.color >= 0) (e.color or 0xFF000000.toInt())
        else if (layerIdx >= 0 && layerIdx < doc.layers.size)
            doc.layers[layerIdx].argb else 0xFF222222.toInt()
        // 视口内的图元绘制：模型坐标经仿射映射到纸面再上屏
        // 配色与模型页统一：白底反转黑，黑底提亮
        linePaint.color = if (lightBackground) onPaperColor(argb)
        else displayColor(argb)

        fun px(x: Double, y: Double) =
            worldToScreenX(t.m11 * x + t.m21 * y + t.dx).toFloat()
        fun py(x: Double, y: Double) =
            worldToScreenY(t.m12 * x + t.m22 * y + t.dy).toFloat()
        when (e) {
            is SkdwDocument.Entity.Line ->
                canvas.drawLine(px(e.x1, e.y1), py(e.x1, e.y1),
                    px(e.x2, e.y2), py(e.x2, e.y2), linePaint)
            is SkdwDocument.Entity.Circle -> {
                val cx = px(e.cx, e.cy); val cy = py(e.cx, e.cy)
                val r = (e.r * s * scale).toFloat()
                if (r > 0.5f) canvas.drawCircle(cx, cy, r, linePaint)
            }
            is SkdwDocument.Entity.Arc -> {
                val cx = px(e.cx, e.cy); val cy = py(e.cx, e.cy)
                val r = (e.r * s * scale).toFloat()
                if (r > 0.5f) {
                    // 角度跟随仿射旋转；镜像时区间翻转
                    val rot = atan2(t.m12, t.m11)
                    val det = t.det
                    var a0: Double; var a1: Double
                    if (det < 0) { a0 = rot - e.a1; a1 = rot - e.a0 }
                    else { a0 = e.a0 + rot; a1 = e.a1 + rot }
                    while (a1 <= a0) a1 += 2 * Math.PI
                    val sweep = a1 - a0
                    canvas.drawArc(cx - r, cy - r, cx + r, cy + r,
                        (-a1 * 180 / Math.PI).toFloat(),
                        (sweep * 180 / Math.PI).toFloat(), false, linePaint)
                }
            }
            is SkdwDocument.Entity.Polyline -> {
                val n = e.xs.size
                if (n >= 2) {
                    path.reset()
                    path.moveTo(px(e.xs[0], e.ys[0]), py(e.xs[0], e.ys[0]))
                    for (i in 1 until n)
                        path.lineTo(px(e.xs[i], e.ys[i]), py(e.xs[i], e.ys[i]))
                    if (e.closed) path.close()
                    if (e.fill && e.closed) {
                        fillPaint.color = linePaint.color and 0x00FFFFFF or 0xB0000000.toInt()
                        canvas.drawPath(path, fillPaint)
                    }
                    canvas.drawPath(path, linePaint)
                }
            }
            is SkdwDocument.Entity.Text -> {
                val hPx = e.height * s * scale
                // 与模型页一致：远景不画小字，避免文字叠成一片
                if (hPx < 3.5) return
                if (dragging && hPx < 6) return
                textPaint.textSize = hPx.toFloat()
                textPaint.color = if (lightBackground)
                    onPaperColor(argb) else displayColor(argb)
                val x = px(e.x, e.y); val y = py(e.x, e.y)
                if (x < -2000 || x > width + 2000 || y < -2000 || y > height + 2000) return
                textPaint.textAlign = when (e.hAlign) {
                    1, 4 -> Paint.Align.CENTER
                    2 -> Paint.Align.RIGHT
                    else -> Paint.Align.LEFT
                }
                val baselineOffset = when (e.vAlign) {
                    2 -> -hPx.toFloat() * 0.35f
                    3 -> -hPx.toFloat() * 0.9f
                    else -> 0f
                }
                val rotDeg = e.rotationDeg +
                    atan2(t.m12, t.m11) * 180.0 / Math.PI
                if (abs(rotDeg) < 0.01) {
                    canvas.drawText(e.text, x, y + baselineOffset, textPaint)
                } else {
                    canvas.save()
                    canvas.rotate((-rotDeg).toFloat(), x, y)
                    canvas.drawText(e.text, x, y + baselineOffset, textPaint)
                    canvas.restore()
                }
            }
            is SkdwDocument.Entity.Viewport -> {}
        }
    }

    private fun visibleWorldRect(): Quad<Double, Double, Double, Double> {
        // 顶边(y=0)与底边(y=height)各取一次世界 y，否则 y 范围退化为一个点
        val (a, yTop) = screenToWorld(0f, 0f)
        val (b, _) = screenToWorld(width.toFloat(), 0f)
        val (_, yBot) = screenToWorld(0f, height.toFloat())
        return Quad(min(a, b), max(a, b), min(yTop, yBot), max(yTop, yBot))
    }

    /** 暗底显示：严格使用图纸设定的颜色（不再自作提亮，用户要求与图纸一致） */
    private fun displayColor(argb: Int): Int = argb or 0xFF000000.toInt()

    // ---------------------------------------------------------- 标注绘制

    /** 编号/照片标注绘制 */
    /** 用户绘制标注（文字/直线/任意线/云线/引线） */
    private fun drawAnnotations(canvas: Canvas) {
        for (a in annotations) {
            val pts = a.points
            if (pts.isEmpty()) continue
            when (a.kind) {
                MeasureMode.TEXT -> {
                    val x = worldToScreenX(pts[0].first).toFloat()
                    val y = worldToScreenY(pts[0].second).toFloat()
                    measureTextPaint.textSize = 30f
                    measureTextPaint.textAlign = Paint.Align.LEFT
                    canvas.drawText(a.text, x + 8f, y - 8f, measureTextPaint)
                    measureTextPaint.textAlign = Paint.Align.CENTER
                }
                MeasureMode.LINE -> {
                    if (pts.size < 2) continue
                    canvas.drawLine(worldToScreenX(pts[0].first).toFloat(),
                        worldToScreenY(pts[0].second).toFloat(),
                        worldToScreenX(pts[1].first).toFloat(),
                        worldToScreenY(pts[1].second).toFloat(), measurePaint)
                }
                MeasureMode.SKETCH, MeasureMode.CLOUD -> {
                    if (pts.size < 2) continue
                    if (a.kind == MeasureMode.CLOUD) {
                        // 云线：每段用半圆弧串成
                        for (i in 1 until pts.size) {
                            val ax = worldToScreenX(pts[i - 1].first).toFloat()
                            val ay = worldToScreenY(pts[i - 1].second).toFloat()
                            val bx = worldToScreenX(pts[i].first).toFloat()
                            val by = worldToScreenY(pts[i].second).toFloat()
                            drawCloudSegment(canvas, ax, ay, bx, by)
                        }
                    } else {
                        val path = Path()
                        path.moveTo(worldToScreenX(pts[0].first).toFloat(),
                            worldToScreenY(pts[0].second).toFloat())
                        for (i in 1 until pts.size) {
                            path.lineTo(worldToScreenX(pts[i].first).toFloat(),
                                worldToScreenY(pts[i].second).toFloat())
                        }
                        canvas.drawPath(path, measurePaint)
                    }
                }
                MeasureMode.LEADER -> {
                    if (pts.isEmpty()) continue
                    val x = worldToScreenX(pts[0].first).toFloat()
                    val y = worldToScreenY(pts[0].second).toFloat()
                    val ex = if (pts.size > 1) worldToScreenX(pts[1].first).toFloat() else x + 60f
                    val ey = if (pts.size > 1) worldToScreenY(pts[1].second).toFloat() else y - 60f
                    canvas.drawLine(x, y, ex, ey, measurePaint)
                    // 箭头
                    val ang = atan2((ey - y).toDouble(), (ex - x).toDouble()).toFloat()
                    for (d in floatArrayOf(0.5f, -0.5f)) {
                        canvas.drawLine(ex, ey,
                            ex - 16f * kotlin.math.cos(ang + d),
                            ey - 16f * kotlin.math.sin(ang + d), measurePaint)
                    }
                    if (a.text.isNotEmpty()) {
                        measureTextPaint.textSize = 26f
                        measureTextPaint.textAlign = Paint.Align.LEFT
                        val right = ex + if (ex >= x) 12f else -measureTextPaint.measureText(a.text) - 12f
                        canvas.drawText(a.text, right,
                            ey + if (ey <= y) -12f else 30f, measureTextPaint)
                        measureTextPaint.textAlign = Paint.Align.CENTER
                    }
                }
                else -> {}
            }
        }
        // 手绘进行中预览
        if (sketchPts.size >= 2) {
            val path = Path()
            path.moveTo(worldToScreenX(sketchPts[0].first).toFloat(),
                worldToScreenY(sketchPts[0].second).toFloat())
            for (i in 1 until sketchPts.size) {
                path.lineTo(worldToScreenX(sketchPts[i].first).toFloat(),
                    worldToScreenY(sketchPts[i].second).toFloat())
            }
            measurePaint.alpha = 170
            canvas.drawPath(path, measurePaint)
            measurePaint.alpha = 255
        }
        // 进行中云线预览（pending）
        if (mode == MeasureMode.CLOUD && pending.size >= 2) {
            for (i in 1 until pending.size) {
                drawCloudSegment(canvas,
                    worldToScreenX(pending[i - 1].first).toFloat(),
                    worldToScreenY(pending[i - 1].second).toFloat(),
                    worldToScreenX(pending[i].first).toFloat(),
                    worldToScreenY(pending[i].second).toFloat())
            }
        }
    }

    /** 云线一段：沿线段方向排布的半圆弧（凸向行进方向左侧） */
    private fun drawCloudSegment(canvas: Canvas, ax: Float, ay: Float,
                                 bx: Float, by: Float) {
        val len = hypot(bx - ax, by - ay)
        if (len < 2f) return
        val n = max(1, (len / 22f).toInt())
        val ux = (bx - ax) / len; val uy = (by - ay) / len
        for (i in 0 until n) {
            val cx = ax + (bx - ax) * (i + 0.5f) / n
            val cy = ay + (by - ay) * (i + 0.5f) / n
            val r = len / n * 0.62f
            val base = Math.toDegrees(
                atan2((by - ay).toDouble(), (bx - ax).toDouble())).toFloat()
            canvas.drawArc(cx - r, cy - r, cx + r, cy + r,
                base + 90f, 180f, false, measurePaint)
        }
    }

    private fun drawPins(canvas: Canvas) {
        for (p in pins) {
            val sx = worldToScreenX(p.x).toFloat()
            val sy = worldToScreenY(p.y).toFloat()
            if (sx < -80 || sx > width + 80 || sy < -80 || sy > height + 80) continue
            pinPaint.color = measureColor
            canvas.drawCircle(sx, sy, 21f, pinPaint)
            canvas.drawCircle(sx, sy, 21f, pinStroke)
            pinTextPaint.textSize = 20f
            canvas.drawText(p.id.toString(), sx, sy + 7f, pinTextPaint)
            if (p.photoPath != null) {
                // 有照片：右上白色小圆点提示
                pinPaint.color = 0xFFFFFFFF.toInt()
                canvas.drawCircle(sx + 18f, sy - 18f, 7f, pinPaint)
                pinPaint.color = measureColor
                canvas.drawCircle(sx + 18f, sy - 18f, 3.5f, pinPaint)
            }
            if (p.note.isNotEmpty()) drawLabel(canvas, sx, sy + 30f, p.note.take(16))
        }
    }

    private fun drawMeasurements(canvas: Canvas) {
        val all = ArrayList<Measurement>(measurements.size + 1)
        all.addAll(measurements)
        if (pending.size >= 2) {
            // 进行中的预览线
            measurePaint.color = 0x8831D2A0.toInt()
            for (i in 1 until pending.size) {
                canvas.drawLine(worldToScreenX(pending[i - 1].first).toFloat(),
                    worldToScreenY(pending[i - 1].second).toFloat(),
                    worldToScreenX(pending[i].first).toFloat(),
                    worldToScreenY(pending[i].second).toFloat(), measurePaint)
            }
        }
        measurePaint.color = 0xFF31D2A0.toInt()
        for (m in measurements) drawOne(canvas, m)
        // 进行中的点
        for (p in pending) {
            val sx = worldToScreenX(p.first).toFloat()
            val sy = worldToScreenY(p.second).toFloat()
            canvas.drawCircle(sx, sy, 5f, measureFill)
            canvas.drawCircle(sx, sy, 5f, measurePaint)
        }
    }

    private fun drawOne(canvas: Canvas, m: Measurement) {
        val pts = m.points
        if (pts.isEmpty()) return
        when (m.kind) {
            MeasureMode.RADIUS -> {
                val cx = worldToScreenX(pts[0].first).toFloat()
                val cy = worldToScreenY(pts[0].second).toFloat()
                val r = (m.radius * scale).toFloat()
                canvas.drawCircle(cx, cy, r, measurePaint)
                canvas.drawLine(cx, cy,
                    cx + r * 0.7071f, cy - r * 0.7071f, measurePaint)
                drawLabel(canvas, cx + r * 0.5f, cy - r * 0.5f,
                    "R %.2f  ⌀ %.2f".format(m.radius * unitScale,
                        m.radius * 2 * unitScale))
            }
            MeasureMode.ANGLE -> {
                if (pts.size < 3) return
                val vx = worldToScreenX(pts[0].first).toFloat()
                val vy = worldToScreenY(pts[0].second).toFloat()
                val ax = worldToScreenX(pts[1].first).toFloat()
                val ay = worldToScreenY(pts[1].second).toFloat()
                val bx = worldToScreenX(pts[2].first).toFloat()
                val by = worldToScreenY(pts[2].second).toFloat()
                canvas.drawLine(vx, vy, ax, ay, measurePaint)
                canvas.drawLine(vx, vy, bx, by, measurePaint)
                // 顶点处圆弧标记
                val a1 = atan2((ay - vy).toDouble(), (ax - vx).toDouble())
                var sweep = atan2((by - vy).toDouble(), (bx - vx).toDouble()) - a1
                while (sweep <= -Math.PI) sweep += 2 * Math.PI
                while (sweep > Math.PI) sweep -= 2 * Math.PI
                val rPx = 52f
                canvas.drawArc(vx - rPx, vy - rPx, vx + rPx, vy + rPx,
                    Math.toDegrees(a1).toFloat(), Math.toDegrees(sweep).toFloat(),
                    false, measurePaint)
                drawLabel(canvas, vx, vy, m.label)
            }
            MeasureMode.COORD -> {
                val x = worldToScreenX(pts[0].first).toFloat()
                val y = worldToScreenY(pts[0].second).toFloat()
                val r = 10f
                canvas.drawLine(x - r, y, x + r, y, measurePaint)
                canvas.drawLine(x, y - r, x, y + r, measurePaint)
                canvas.drawCircle(x, y, r * 0.55f, measurePaint)
                drawLabel(canvas, x, y, m.label)
            }
            MeasureMode.ARC -> {
                if (pts.size < 4) return
                val cx = worldToScreenX(pts[0].first).toFloat()
                val cy = worldToScreenY(pts[0].second).toFloat()
                val r = (m.radius * scale).toFloat()
                if (r > 0.5f) {
                    val a0 = atan2((pts[1].second - pts[0].second).toDouble(),
                        (pts[1].first - pts[0].first).toDouble())
                    val a1 = atan2((pts[2].second - pts[0].second).toDouble(),
                        (pts[2].first - pts[0].first).toDouble())
                    var sweep = a1 - a0
                    while (sweep <= 0) sweep += 2 * Math.PI
                    canvas.drawArc(cx - r, cy - r, cx + r, cy + r,
                        (-a1 * 180 / Math.PI).toFloat(),
                        (sweep * 180 / Math.PI).toFloat(), false, measurePaint)
                }
                drawLabel(canvas, worldToScreenX(pts[3].first).toFloat(),
                    worldToScreenY(pts[3].second).toFloat(), m.label(unitScale))
            }
            MeasureMode.LENGTH, MeasureMode.CONTINUOUS -> {
                // 尺寸线压在线段上 + 45° 起止斜线 + 数字在线上方（建筑制图风格）
                var i = 0
                while (i + 1 < pts.size) {
                    val a = pts[i]; val b = pts[i + 1]
                    val ax = worldToScreenX(a.first).toFloat()
                    val ay = worldToScreenY(a.second).toFloat()
                    val bx = worldToScreenX(b.first).toFloat()
                    val by = worldToScreenY(b.second).toFloat()
                    canvas.drawLine(ax, ay, bx, by, measurePaint)
                    tick(canvas, ax, ay, bx, by)
                    tick(canvas, bx, by, ax, ay)
                    val len = if (i < m.edgeLengths.size) m.edgeLengths[i] else 0.0
                    val text = if (m.kind == MeasureMode.LENGTH) m.label(unitScale)
                    else "%.0f".format(len * unitScale)
                    drawLabel(canvas, (ax + bx) / 2, (ay + by) / 2, text,
                        atan2(by - ay, bx - ax))
                    i++
                }
                // 连续测量闭合时画回边
                if (m.kind == MeasureMode.CONTINUOUS && pts.size > 2) {
                    val a = pts.last(); val b = pts.first()
                    canvas.drawLine(worldToScreenX(a.first).toFloat(),
                        worldToScreenY(a.second).toFloat(),
                        worldToScreenX(b.first).toFloat(),
                        worldToScreenY(b.second).toFloat(), measurePaint)
                }
            }
            MeasureMode.AREA -> {
                val path = Path()
                path.moveTo(worldToScreenX(pts[0].first).toFloat(),
                    worldToScreenY(pts[0].second).toFloat())
                for (i in 1 until pts.size) {
                    path.lineTo(worldToScreenX(pts[i].first).toFloat(),
                        worldToScreenY(pts[i].second).toFloat())
                }
                path.close()
                canvas.drawPath(path, measureFill)
                canvas.drawPath(path, measurePaint)
                // 形心
                var cx = 0.0; var cy = 0.0
                for (p in pts) { cx += p.first; cy += p.second }
                drawLabel(canvas,
                    worldToScreenX(cx / pts.size).toFloat(),
                    worldToScreenY(cy / pts.size).toFloat(), m.label(unitScale))
            }
            MeasureMode.NONE, MeasureMode.PIN, MeasureMode.PHOTO,
            MeasureMode.TEXT, MeasureMode.LINE, MeasureMode.SKETCH,
            MeasureMode.CLOUD, MeasureMode.LEADER -> {}
        }
    }

    /** 45° 中粗斜线起止符号（建筑制图规范风格） */
    private fun tick(canvas: Canvas, x: Float, y: Float, towardX: Float, towardY: Float) {
        val dx = towardX - x; val dy = towardY - y
        val len = hypot(dx, dy)
        if (len < 1f) return
        val ux = dx / len; val uy = dy / len
        // 相对线方向 45°
        val size = 12f
        val mx = x - ux * size * 0.5f
        val my = y - uy * size * 0.5f
        val tx = mx + (ux + uy) * size * 0.7f
        val ty = my + (uy - ux) * size * 0.7f
        canvas.drawLine(mx - (tx - mx), my - (ty - my), tx, ty, measurePaint)
    }

    private fun drawLabel(canvas: Canvas, x: Float, y: Float, text: String,
                          angle: Float = 0f) {
        measureTextPaint.textSize = 26f
        val lines = text.split("\n")
        canvas.save()
        if (abs(angle) > 0.01f && abs(abs(angle) - Math.PI.toFloat()) > 0.01f) {
            var a = angle
            if (a > Math.PI / 2) a -= Math.PI.toFloat()
            if (a < -Math.PI / 2) a += Math.PI.toFloat()
            canvas.rotate(-a, x, y)
        }
        var base = y - lines.size * 15f - 8f
        for (line in lines) {
            canvas.drawText(line, x, base, measureTextPaint)
            base += 30f
        }
        canvas.restore()
    }

    // ---------------------------------------------------------- 触摸分发

    override fun onTouchEvent(event: MotionEvent): Boolean {
        // 先喂手势检测器：下面的分支无论是否 return，捏合缩放/单击/双击
        // 都必须先收到事件（之前放在末尾，双指分支 return 后缩放完全失效）
        scaleDetector.onTouchEvent(event)
        gestureDetector.onTouchEvent(event)

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastTouchX = event.x
                lastTouchY = event.y
                dragging = true
                // 手绘：按下开始采点
                if (mode == MeasureMode.SKETCH) {
                    sketchPts.clear()
                    sketchPts.add(screenToWorld(event.x, event.y))
                }
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                // 双指进入：平移基准取两指中点，避免第二根手指落下瞬间跳位
                val mid = pointerMid(event, -1)
                lastTouchX = mid.first
                lastTouchY = mid.second
            }
            MotionEvent.ACTION_POINTER_UP -> {
                // 一根手指抬起：平移基准切到剩余手指中点，避免抬指瞬间跳位
                val mid = pointerMid(event, event.actionIndex)
                lastTouchX = mid.first
                lastTouchY = mid.second
            }
            MotionEvent.ACTION_MOVE -> {
                // 手绘模式：采点不拖图纸
                if (mode == MeasureMode.SKETCH) {
                    val w = screenToWorld(event.x, event.y)
                    val last = sketchPts.lastOrNull()
                    val ls = last?.let {
                        Pair(worldToScreenX(it.first).toFloat(),
                             worldToScreenY(it.second).toFloat())
                    }
                    if (ls == null || hypot(ls.first - event.x, ls.second - event.y) > 6f) {
                        sketchPts.add(w)
                        invalidate()
                    }
                    return true
                }
                // 双指：按两指中点位移做 1:1 跟手平移；缩放由 ScaleGestureDetector
                // 单独处理（它以两指中点为中心保持焦点不动），两者互不重复位移
                if (event.pointerCount > 1) {
                    val mid = pointerMid(event, -1)
                    val dx = mid.first - lastTouchX
                    val dy = mid.second - lastTouchY
                    lastTouchX = mid.first
                    lastTouchY = mid.second
                    if (dx.isFinite() && dy.isFinite()) {
                        centerX -= dx / scale
                        centerY += dy / scale
                        invalidate()
                    }
                    return true
                }
                val dx = event.x - lastTouchX
                val dy = event.y - lastTouchY
                lastTouchX = event.x
                lastTouchY = event.y
                centerX -= dx / scale
                centerY += dy / scale
                invalidate()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                dragging = false
                snapPoint = null
                // 手绘：抬起提交（至少 2 点且有一定长度）
                if (mode == MeasureMode.SKETCH && event.actionMasked == MotionEvent.ACTION_UP) {
                    if (sketchPts.size >= 2) {
                        annotations.add(Annotation(nextAnnoId++,
                            MeasureMode.SKETCH, ArrayList(sketchPts)))
                        updateResultPanel()
                    }
                    sketchPts.clear()
                }
                invalidate()
            }
        }
        return true
    }

    /** 多指中点（skipIndex = 要排除的手指，-1 表示全部参与） */
    private fun pointerMid(event: MotionEvent, skipIndex: Int): Pair<Float, Float> {
        var sx = 0f; var sy = 0f; var n = 0
        for (i in 0 until event.pointerCount) {
            if (i == skipIndex) continue
            sx += event.getX(i); sy += event.getY(i); n++
        }
        return if (n <= 0) Pair(event.x, event.y) else Pair(sx / n, sy / n)
    }

    private data class Quad<A, B, C, D>(val first: A, val second: B,
                                        val third: C, val fourth: D)
}
