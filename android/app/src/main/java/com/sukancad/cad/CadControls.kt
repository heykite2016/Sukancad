package com.sukancad.cad

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

private const val INK = 0xFFD6DFE6.toInt()
private const val ACCENT = 0xFF31D2A0.toInt()
private const val CARD = 0xFF202832.toInt()
private const val TILE = 0xFF2A323B.toInt()

/** Canvas 自绘的底栏，避免原生 Button 在不同厂商 ROM 上的内边距差异。 */
class CadToolDockView(
    context: Context,
    private val labels: List<String>,
    private val onSelect: (Int) -> Unit,
) : View(context) {
    private val density = resources.displayMetrics.density
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val round = RectF()
    private var downX = 0f
    private var downY = 0f
    private var downIndex = -1

    var selectedIndex = -1
        set(value) {
            field = value
            contentDescription = labels.getOrNull(value)
            invalidate()
        }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val width = MeasureSpec.getSize(widthMeasureSpec)
        setMeasuredDimension(width, (58 * density).toInt())
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        round.set(0f, (3 * density), width.toFloat(), height - (2 * density))
        paint.style = Paint.Style.FILL
        paint.color = CARD
        canvas.drawRoundRect(round, 18 * density, 18 * density, paint)
        val itemWidth = width.toFloat() / labels.size.coerceAtLeast(1)
        for (index in labels.indices) {
            val selected = index == selectedIndex
            val cx = itemWidth * (index + 0.5f)
            val cy = 21f * density
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = 2f * density
            paint.strokeCap = Paint.Cap.ROUND
            paint.color = if (selected) ACCENT else INK
            drawIcon(canvas, index, cx, cy, 12f * density)
            paint.style = Paint.Style.FILL
            paint.color = if (selected) ACCENT else INK
            paint.textAlign = Paint.Align.CENTER
            paint.textSize = 12f * resources.displayMetrics.scaledDensity
            canvas.drawText(labels[index], cx, 47f * density, paint)
        }
    }

    private fun drawIcon(canvas: Canvas, index: Int, cx: Float, cy: Float, r: Float) {
        val path = Path()
        when (index) {
            0 -> {
                canvas.drawCircle(cx, cy, r * 0.72f, paint)
                canvas.drawLine(cx - r, cy, cx + r, cy, paint)
                canvas.drawLine(cx, cy - r, cx, cy + r, paint)
            }
            1 -> {
                canvas.drawLine(cx - r, cy + r * 0.4f, cx + r, cy + r * 0.4f, paint)
                canvas.drawLine(cx - r * 0.7f, cy - r * 0.45f,
                    cx - r * 0.7f, cy + r * 0.7f, paint)
                canvas.drawLine(cx + r * 0.7f, cy - r * 0.45f,
                    cx + r * 0.7f, cy + r * 0.7f, paint)
            }
            2 -> {
                path.moveTo(cx - r, cy + r * 0.7f)
                path.lineTo(cx + r * 0.55f, cy + r * 0.7f)
                path.lineTo(cx + r, cy - r * 0.65f)
                path.lineTo(cx - r * 0.8f, cy - r * 0.25f)
                canvas.drawPath(path, paint)
            }
            3 -> {
                path.moveTo(cx - r, cy + r * 0.65f)
                path.lineTo(cx - r, cy - r * 0.65f)
                path.lineTo(cx + r * 0.15f, cy - r * 0.65f)
                path.lineTo(cx + r * 0.15f, cy + r * 0.65f)
                path.lineTo(cx + r, cy + r * 0.65f)
                canvas.drawPath(path, paint)
            }
            else -> {
                repeat(3) { i ->
                    canvas.drawCircle(cx + (i - 1) * r * 0.7f, cy, 1.7f * density, paint)
                }
            }
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x
                downY = event.y
                downIndex = indexAt(event.x)
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (abs(event.x - downX) <= touchSlop &&
                    abs(event.y - downY) <= touchSlop &&
                    downIndex >= 0 && downIndex == indexAt(event.x)) {
                    selectedIndex = downIndex
                    onSelect(downIndex)
                    performClick()
                }
                return true
            }
            MotionEvent.ACTION_CANCEL -> downIndex = -1
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    private fun indexAt(x: Float): Int {
        if (labels.isEmpty() || x < 0f || x > width) return -1
        return min(labels.lastIndex, (x / (width.toFloat() / labels.size)).toInt())
    }
}

/** 单排横向工具，Canvas 绘制图标和文字。 */
class CadToolRowView(
    context: Context,
    private val labels: List<String>,
    private val onSelect: (Int) -> Unit,
) : View(context) {
    private val density = resources.displayMetrics.density
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val tile = RectF()
    private var downX = 0f
    private var downY = 0f
    private var downIndex = -1
    private val itemWidth = 100f * density
    private var originX = 0f

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val desired = max(
            (labels.size * itemWidth).toInt().coerceAtLeast(1),
            suggestedMinimumWidth,
        )
        setMeasuredDimension(resolveSize(desired, widthMeasureSpec), (52 * density).toInt())
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val totalWidth = labels.size * itemWidth
        originX = ((width - totalWidth) / 2f).coerceAtLeast(0f)
        for (index in labels.indices) {
            val left = originX + index * itemWidth + 3 * density
            val right = originX + (index + 1) * itemWidth - 3 * density
            tile.set(left, 2 * density, right, height - 2 * density)
            paint.style = Paint.Style.FILL
            paint.color = TILE
            canvas.drawRoundRect(tile, 11 * density, 11 * density, paint)
            paint.color = INK
            paint.textAlign = Paint.Align.CENTER
            paint.textSize = 15f * resources.displayMetrics.scaledDensity
            canvas.drawText(iconFor(labels[index]),
                (left + right) / 2f, 22 * density, paint)
            paint.textSize = 11.5f * resources.displayMetrics.scaledDensity
            canvas.drawText(labels[index],
                (left + right) / 2f, 42 * density, paint)
        }
    }

    private fun iconFor(label: String): String = when {
        "长度" in label -> "↔"
        "连续" in label -> "⌁"
        "面积" in label -> "▱"
        "角度" in label -> "∠"
        "弧长" in label -> "◠"
        "半径" in label -> "◯"
        "比例" in label || "校准" in label -> "↗"
        "文字" in label -> "T"
        "管理" in label || "列表" in label -> "☰"
        "颜色" in label -> "●"
        "白底" in label || "黑底" in label -> "◐"
        "查找" in label -> "⌕"
        "提取" in label || "复制" in label -> "T⁻¹"
        "分享" in label || "截图" in label -> "⇧"
        "清除" in label -> "×"
        else -> "•"
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x
                downY = event.y
                downIndex = indexAt(event.x)
                parent?.requestDisallowInterceptTouchEvent(true)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (abs(event.x - downX) > touchSlop) {
                    parent?.requestDisallowInterceptTouchEvent(false)
                }
            }
            MotionEvent.ACTION_UP -> {
                parent?.requestDisallowInterceptTouchEvent(false)
                if (abs(event.x - downX) <= touchSlop &&
                    abs(event.y - downY) <= touchSlop &&
                    downIndex >= 0 && downIndex == indexAt(event.x)) {
                    onSelect(downIndex)
                    performClick()
                }
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                parent?.requestDisallowInterceptTouchEvent(false)
                downIndex = -1
            }
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    private fun indexAt(x: Float): Int {
        if (labels.isEmpty()) return -1
        val totalWidth = labels.size * itemWidth
        val start = ((width - totalWidth) / 2f).coerceAtLeast(0f)
        if (x < start || x > start + totalWidth) return -1
        return ((x - start) / itemWidth).toInt().coerceIn(0, labels.lastIndex)
    }
}
