package com.sukancad.cad

import android.content.Context
import android.graphics.drawable.GradientDrawable
import android.util.AttributeSet
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.widget.FrameLayout
import android.widget.TextView
import kotlin.math.abs
import kotlin.math.max

/** 原生列表行上的左滑操作区：内容随手指移动，右侧固定删除/移除按钮。 */
class SwipeRevealLayout @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : FrameLayout(context, attrs) {
    private lateinit var content: View
    private lateinit var action: TextView
    private val density = resources.displayMetrics.density
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private val actionWidth = (86 * density).toInt()
    private var downX = 0f
    private var downY = 0f
    private var dragging = false
    private var longPressed = false
    private var actionCallback: (() -> Unit)? = null
    private var tapCallback: (() -> Unit)? = null
    private var longPressCallback: (() -> Unit)? = null
    private val handler = Handler(Looper.getMainLooper())
    private val longPress = Runnable {
        if (!dragging) {
            longPressed = true
            longPressCallback?.invoke()
            performLongClick()
        }
    }

    var onOpenChanged: ((SwipeRevealLayout, Boolean) -> Unit)? = null

    override fun onFinishInflate() {
        super.onFinishInflate()
        check(childCount >= 2)
        content = getChildAt(0)
        action = getChildAt(1) as TextView
        content.bringToFront()
        action.setOnClickListener {
            close()
            actionCallback?.invoke()
        }
    }

    fun bind(
        label: String,
        color: Int,
        actionCallback: () -> Unit,
        tapCallback: () -> Unit,
        longPressCallback: () -> Unit,
    ) {
        this.actionCallback = actionCallback
        this.tapCallback = tapCallback
        this.longPressCallback = longPressCallback
        action.text = label
        action.contentDescription = label
        action.background = GradientDrawable().apply {
            setColor(color)
            cornerRadius = 10 * density
        }
        close(notify = false)
    }

    fun close(notify: Boolean = true) {
        content.animate().translationX(0f).setDuration(150).start()
        if (notify) onOpenChanged?.invoke(this, false)
    }

    private fun settle(open: Boolean) {
        content.animate()
            .translationX(if (open) -actionWidth.toFloat() else 0f)
            .setDuration(150)
            .start()
        onOpenChanged?.invoke(this, open)
    }

    override fun onInterceptTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x
                downY = event.y
                dragging = false
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = event.x - downX
                val dy = event.y - downY
                if (abs(dx) > touchSlop && abs(dx) > abs(dy)) {
                    dragging = true
                    handler.removeCallbacks(longPress)
                    parent?.requestDisallowInterceptTouchEvent(true)
                    return true
                }
            }
        }
        return false
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x
                downY = event.y
                dragging = false
                longPressed = false
                handler.postDelayed(longPress, ViewConfiguration.getLongPressTimeout().toLong())
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = event.x - downX
                val dy = event.y - downY
                if (!dragging &&
                    abs(dx) > touchSlop && abs(dx) > abs(dy)) {
                    dragging = true
                    parent?.requestDisallowInterceptTouchEvent(true)
                }
                if (dragging) {
                    val base = if (content.translationX < -actionWidth / 2f) -actionWidth else 0
                    content.translationX = (base + dx).coerceIn(-actionWidth.toFloat(), 0f)
                }
            }
            MotionEvent.ACTION_UP -> {
                parent?.requestDisallowInterceptTouchEvent(false)
                if (dragging) {
                    settle(content.translationX <= -actionWidth * 0.38f)
                } else if (!longPressed) {
                    if (content.translationX < -1f) close()
                    else tapCallback?.invoke()
                }
                handler.removeCallbacks(longPress)
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                handler.removeCallbacks(longPress)
                parent?.requestDisallowInterceptTouchEvent(false)
                settle(false)
            }
        }
        return dragging || super.onTouchEvent(event)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val width = MeasureSpec.getSize(widthMeasureSpec)
        val childHeightSpec = MeasureSpec.makeMeasureSpec(
            MeasureSpec.getSize(heightMeasureSpec),
            MeasureSpec.AT_MOST,
        )
        content.measure(
            MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
            childHeightSpec,
        )
        val height = content.measuredHeight
        action.measure(
            MeasureSpec.makeMeasureSpec(actionWidth, MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
        )
        setMeasuredDimension(width, max(height, suggestedMinimumHeight))
    }

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        val width = right - left
        val height = bottom - top
        content.layout(0, 0, width, height)
        action.layout(width - actionWidth, 0, width, height)
    }
}
