package com.sukancad.cad

import android.annotation.SuppressLint
import android.app.Dialog
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.bottomsheet.BottomSheetBehavior
import com.google.android.material.button.MaterialButton
import java.io.File
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/** 图纸查看页：解析 → 渲染 → 测量 */
class DrawingActivity : AppCompatActivity() {

    private lateinit var drawingView: DrawingView
    private lateinit var infoPanel: TextView
    private lateinit var loadingPanel: TextView
    private lateinit var loadingOverlay: FrameLayout
    private val parseExecutor = Executors.newSingleThreadExecutor()

    /** 待关联照片的标注点（照片选择回来后用） */
    private var pendingPhotoPoint: Pair<Double, Double>? = null

    // 布局切换：0 = 模型，1..N = 图纸空间布局
    private data class Space(val name: String, val file: File?)
    private val spaces = ArrayList<Space>()
    private var currentSpace = 0
    private val spaceButtons = ArrayList<Button>()
    private var modelDoc: SkdwDocument? = null
    private val docCache = HashMap<Int, SkdwDocument>()
    private val measurementsBySpace = HashMap<Int, MutableList<Measurement>>()

    // 图层可见性状态（与 modelDoc.layers 对齐；布局文档复用同一图层表）
    private var layerVisible: BooleanArray? = null
    private lateinit var source: File

    // 视图记忆（重开图纸回到上次看的位置/布局）
    private var pendingRestore: Triple<Double, Double, Double>? = null
    private var pendingRestoreSpace = 0

    // 底部导航 + 分类工具条（对标竞品结构）
    private lateinit var titleView: TextView
    private lateinit var spaceButton: Button
    private lateinit var actionBar: LinearLayout
    private lateinit var actionStatus: TextView
    private lateinit var toolDock: CadToolDockView
    private lateinit var toolRowScroll: android.widget.HorizontalScrollView
    private var toolRowView: CadToolRowView? = null
    private var currentToolLabel = ""
    private var currentToolHint = ""
    private var activeTab = -1
    private var initialLayerVisible: BooleanArray? = null
    private var lastInteractionResult = ""

    @SuppressLint("SetTextI18n")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val path = intent.getStringExtra("path") ?: run { finish(); return }
        source = File(path)
        title = source.name
        DrawingLibrary.recordOpen(this, source.absolutePath)

        val root = FrameLayout(this).apply {
            setBackgroundColor(0xFF101318.toInt())
        }

        drawingView = DrawingView(this)
        root.addView(drawingView, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        val topBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(10), dp(8), dp(10), dp(8))
            background = rounded(0xE61A1E24.toInt(), 16f)
        }
        val back = compactButton("‹", 24f) { finish() }
        topBar.addView(back)
        val titleColumn = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8), 0, dp(8), 0)
        }
        titleView = TextView(this).apply {
            text = source.name
            textSize = 15f
            setTextColor(0xFFF3F6F8.toInt())
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.MIDDLE
        }
        titleColumn.addView(titleView)
        spaceButton = compactButton("模型 ▾", 12f) { showSpaceMenu() }
        spaceButton.visibility = View.GONE
        titleColumn.addView(spaceButton)
        topBar.addView(titleColumn, LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        topBar.addView(compactButton("全图", 12f) {
            drawingView.fitToView(true)
        })
        topBar.addView(compactButton("分享", 12f) { shareScreenshot() })

        infoPanel = TextView(this).apply {
            textSize = 14f
            setTextColor(0xFFEAEFF3.toInt())
            setLineSpacing(dp(3).toFloat(), 1f)
            setPadding(dp(14), dp(10), dp(14), dp(10))
            background = rounded(0xEE202832.toInt(), 14f)
            visibility = View.GONE
        }

        actionStatus = TextView(this).apply {
            textSize = 12f
            setTextColor(0xFFB9C4CE.toInt())
            setPadding(dp(12), 0, dp(8), 0)
            maxLines = 2
        }
        actionBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(4), dp(4), dp(6), dp(4))
            background = rounded(0xEE1A2027.toInt(), 14f)
            visibility = View.GONE
        }
        actionBar.addView(actionStatus, LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        actionBar.addView(compactButton("退一步", 12f) {
            drawingView.undoPendingPoint()
        })
        actionBar.addView(compactButton("完成", 12f) {
            drawingView.finishInteraction()
        })
        actionBar.addView(compactButton("取消", 12f) {
            drawingView.cancelInteraction()
            activeTab = 0
            refreshDock()
            actionBar.visibility = View.GONE
            lastInteractionResult = ""
            drawingView.refreshInfoPanel()
        })

        toolDock = CadToolDockView(
            this,
            listOf("浏览", "测量", "标注", "图层", "更多"),
            ::selectDock,
        )

        val topLayout = FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP,
        ).apply { setMargins(dp(10), dp(8), dp(10), 0) }
        root.addView(topBar, topLayout)

        val bottomStack = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(10), 0, dp(10), dp(8))
        }
        bottomStack.addView(actionBar, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { bottomMargin = dp(6) })
        bottomStack.addView(infoPanel, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { bottomMargin = dp(6) })
        toolRowScroll = android.widget.HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            visibility = View.GONE
            background = rounded(0xEE202832.toInt(), 14f)
        }
        bottomStack.addView(toolRowScroll, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { bottomMargin = dp(6) })
        bottomStack.addView(toolDock, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ))
        root.addView(bottomStack, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM,
        ))

        val loadingWrap = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(dp(28), dp(22), dp(28), dp(22))
            background = rounded(0xF21A2027.toInt(), 18f)
            addView(android.widget.ProgressBar(this@DrawingActivity))
        }
        loadingPanel = TextView(this).apply {
            text = "正在解析图纸…"
            setTextColor(0xFFF3F6F8.toInt())
            textSize = 15f
            gravity = Gravity.CENTER
            setPadding(0, dp(12), 0, 0)
        }
        loadingWrap.addView(loadingPanel)
        loadingOverlay = FrameLayout(this).apply {
            setBackgroundColor(0x66000000)
            addView(loadingWrap, FrameLayout.LayoutParams(
                dp(260), ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER,
            ))
        }
        root.addView(loadingOverlay, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT,
        ))
        setContentView(root)
        applySettings()
        selectDock(0)

        drawingView.onTextInput = { kind, pos ->
            runOnUiThread { showAnnotationTextInput(kind, pos) }
        }
        drawingView.onMeasurementChanged = { text ->
            runOnUiThread {
                lastInteractionResult = text
                if (drawingView.mode == MeasureMode.NONE && !drawingView.isEditingObject) {
                    infoPanel.text = text
                    infoPanel.visibility =
                        if (text.isEmpty()) View.GONE else View.VISIBLE
                } else {
                    infoPanel.visibility = View.GONE
                    if (text.isNotBlank()) {
                        actionStatus.text = if (drawingView.isEditingObject) {
                            "编辑标注 · " + text.replace("\n", "，")
                        } else if (drawingView.pendingPointCount == 0 ||
                            drawingView.activeInteractionHint.isNotBlank()) {
                            "$currentToolLabel · " + text.replace("\n", "，")
                        } else {
                            actionStatus.text
                        }
                    }
                }
            }
        }
        drawingView.onCalibrationRequested = { rawLength ->
            runOnUiThread { showManualCalibrationInput(rawLength) }
        }
        drawingView.onEntityPicked = { text ->
            runOnUiThread {
                if (text.isNullOrEmpty()) {
                    drawingView.refreshInfoPanel()
                } else {
                    infoPanel.text = text
                    infoPanel.visibility = View.VISIBLE
                }
            }
        }
        drawingView.onInteractionStateChanged = { currentMode, count ->
            runOnUiThread {
                if (currentMode == MeasureMode.NONE) {
                    actionBar.visibility = View.GONE
                } else {
                    activeTab = when (currentMode) {
                        MeasureMode.TEXT, MeasureMode.LINE, MeasureMode.SKETCH,
                        MeasureMode.CLOUD, MeasureMode.LEADER,
                        MeasureMode.PIN, MeasureMode.PHOTO -> 2
                        else -> 1
                    }
                    refreshDock()
                    actionBar.visibility = View.VISIBLE
                    infoPanel.visibility = View.GONE
                    actionStatus.text = if (drawingView.activeInteractionHint.isNotBlank()) {
                        "$currentToolLabel · " +
                                drawingView.activeInteractionHint.replace("\n", "，")
                    } else if (count == 0 && lastInteractionResult.isNotBlank()) {
                        "$currentToolLabel · " +
                                lastInteractionResult.replace("\n", "，")
                    } else if (count == 0) {
                        "$currentToolLabel · $currentToolHint"
                    } else {
                        "$currentToolLabel · 已选 $count 点；拖动控制柄可调整"
                    }
                }
            }
        }
        drawingView.onPinAdded = { pin ->
            runOnUiThread { showPinNoteDialog(pin) }
        }
        drawingView.onPinTapped = { pin ->
            runOnUiThread { showPinDetail(pin) }
        }
        drawingView.onMeasurementTapped = { measurement ->
            runOnUiThread { showMeasurementActions(measurement) }
        }
        drawingView.onAnnotationTapped = { annotation ->
            runOnUiThread { showAnnotationActions(annotation) }
        }
        drawingView.onObjectEditStateChanged = { editing ->
            runOnUiThread {
                if (editing) {
                    activeTab = 0
                    refreshDock()
                    infoPanel.visibility = View.GONE
                    actionBar.visibility = View.VISIBLE
                    actionStatus.text = "编辑标注 · 拖动控制点，实时重算尺寸"
                } else {
                    actionBar.visibility = View.GONE
                    activeTab = 0
                    refreshDock()
                    drawingView.refreshInfoPanel()
                }
            }
        }
        drawingView.onPhotoRequested = { world ->
            pendingPhotoPoint = world
            val intent = android.content.Intent(
                android.content.Intent.ACTION_GET_CONTENT).apply {
                type = "image/*"
                addCategory(android.content.Intent.CATEGORY_OPENABLE)
            }
            startActivityForResult(
                android.content.Intent.createChooser(intent, "选择照片"), 4201)
        }

        // 解析（有缓存直接读缓存；缓存版本不符/损坏时重新解析）
        loadingOverlay.visibility = View.VISIBLE
        thread {
            val cache = File(source.absolutePath + ".skdw")
            var doc: SkdwDocument? = null
            var reparsed = false
            if (cache.isFile && cache.lastModified() >= source.lastModified() &&
                cache.length() > 0) {
                doc = SkdwDocument.read(cache)   // 版本不符时返回 null → 触发重解析
            }
            if (doc == null) {
                reparsed = true
                val ok = DwgCore.parse(source, cache)
                if (ok) doc = SkdwDocument.read(cache)
            }
            runOnUiThread {
                loadingOverlay.visibility = View.GONE
                if (doc == null) {
                    Toast.makeText(this, "解析失败：${source.name}", Toast.LENGTH_LONG).show()
                    finish()
                } else {
                    modelDoc = doc
                    docCache[0] = doc
                    layerVisible = BooleanArray(doc.layers.size) { doc.layers[it].visible }
                    initialLayerVisible = layerVisible?.copyOf()
                    restoreLayerState()
                    // 布局清单（缓存同目录 .layouts）
                    val tabs = SkdwDocument.readLayouts(cache)
                    spaces.clear()
                    spaces.add(Space("模型", cache))
                    for (t in tabs) {
                        spaces.add(Space(t.name,
                            File(cache.absolutePath + ".l" + t.index + ".skdw")))
                    }
                    if (spaces.size > 1) buildSpaceButtons()
                    // 视图记忆：有记录时直接回到上次的布局和位置。
                    // 重新解析过的图纸（坐标范围可能变化）不恢复旧视图，重新适配全图
                    if (reparsed) {
                        pendingRestore = null
                        pendingRestoreSpace = 0
                    } else {
                        readViewState()
                    }
                    loadPins()
                    // 模型空间为空但布局有内容（封面/目录等图纸空间出图）→ 直接打开布局
                    if (doc.entities.isEmpty() && spaces.size > 1
                        && pendingRestoreSpace == 0) {
                        pendingRestoreSpace = 1
                        pendingRestore = null
                        Toast.makeText(this, "模型空间为空，已打开${spaces[1].name}",
                            Toast.LENGTH_LONG).show()
                    }
                    switchSpace(pendingRestoreSpace)
                    Toast.makeText(this,
                        "${doc.entities.size} 个图元 · ${doc.layers.size} 个图层" +
                                (if (spaces.size > 1) " · ${spaces.size - 1} 个布局" else ""),
                        Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    /** 布局切换按钮（解析完成后刷新，顶栏「模型/布局」按钮用） */
    private fun buildSpaceButtons() {
        spaceButtons.clear()
        for ((i, space) in spaces.withIndex()) {
            spaceButtons.add(toolButton(space.name) { switchSpace(i) })
        }
        refreshSpaceBar()
        if (spaces.size > 1) {
            spaceButton.visibility = View.VISIBLE
            spaceButton.text = spaces.getOrNull(currentSpace)?.name ?: "模型"
        } else {
            spaceButton.visibility = View.GONE
        }
    }

    private fun refreshSpaceBar() {
        for ((i, btn) in spaceButtons.withIndex()) {
            btn.setTextColor(if (i == currentSpace) 0xFF31D2A0.toInt()
            else 0xFFC9D2DC.toInt())
        }
        spaceButton.text = spaces.getOrNull(currentSpace)?.name ?: "模型"
    }

    // ---- 底部导航分类面板 ----

    private fun toolButton(label: String, color: Int = 0xFFC9D2DC.toInt(),
                           onClick: () -> Unit): Button =
        Button(this).apply {
            text = label
            textSize = 13f
            isAllCaps = false
            setTextColor(color)
            setBackgroundColor(Color.TRANSPARENT)
            setOnClickListener { onClick() }
        }

    private fun dp(value: Int): Int =
        (value * resources.displayMetrics.density).toInt()

    private fun rounded(color: Int, radiusDp: Float): GradientDrawable =
        GradientDrawable().apply {
            setColor(color)
            cornerRadius = dp(radiusDp.toInt()).toFloat()
        }

    private fun compactButton(label: String, sizeSp: Float,
                              onClick: () -> Unit): Button =
        Button(this).apply {
            text = label
            textSize = sizeSp
            isAllCaps = false
            minWidth = 0
            minimumWidth = 0
            minHeight = 0
            minimumHeight = 0
            setPadding(dp(8), dp(4), dp(8), dp(4))
            setTextColor(0xFFE9EEF2.toInt())
            setBackgroundColor(Color.TRANSPARENT)
            setOnClickListener { onClick() }
        }

    private fun refreshDock() {
        if (::toolDock.isInitialized) toolDock.selectedIndex = activeTab
    }

    private fun selectDock(tab: Int) {
        if (tab in setOf(1, 2, 4) && tab == activeTab &&
            toolRowScroll.visibility == View.VISIBLE) {
            hideToolRow()
            return
        }
        activeTab = tab
        refreshDock()
        when (tab) {
            0 -> {
                currentToolLabel = "浏览"
                currentToolHint = ""
                drawingView.cancelInteraction()
                actionBar.visibility = View.GONE
                hideToolRow()
            }
            1 -> showMeasurePanel()
            2 -> showDrawPanel()
            3 -> showLayerDialog()
            4 -> showToolPanel()
        }
    }

    private fun activateTool(label: String, hint: String, mode: MeasureMode) {
        currentToolLabel = label
        currentToolHint = hint
        lastInteractionResult = ""
        drawingView.setMode(mode)
        actionBar.visibility = View.VISIBLE
        actionStatus.text = "$label · $hint"
    }

    private fun showToolSheet(
        title: String,
        subtitle: String,
        items: List<Pair<String, () -> Unit>>,
    ) {
        toolRowScroll.removeAllViews()
        val view = CadToolRowView(this, items.map { it.first }) { index ->
            hideToolRow()
            items.getOrNull(index)?.second?.invoke()
        }
        toolRowView = view
        toolRowScroll.addView(view, ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ))
        toolRowScroll.visibility = View.VISIBLE
        toolRowScroll.post { toolRowScroll.scrollTo(0, 0) }
        view.contentDescription = "$title：$subtitle"
    }

    private fun hideToolRow() {
        if (::toolRowScroll.isInitialized) toolRowScroll.visibility = View.GONE
    }

    private fun showMeasurePanel() {
        showToolSheet(
            "测量",
            "单指拖动准星选点，松开确认；双指平移/缩放；靠近已选点可拖动调整。",
            listOf(
                "长度" to {
                    activateTool("长度", "选择两个点", MeasureMode.LENGTH)
                },
                "连续" to {
                    activateTool("连续", "连续选点，点“完成”结束", MeasureMode.CONTINUOUS)
                },
                "面积" to {
                    activateTool("面积", "沿边界选点，点“完成”闭合", MeasureMode.AREA)
                },
                "角度" to {
                    activateTool("角度", "依次点两条线段，再点文字落点", MeasureMode.ANGLE)
                },
                "弧长" to {
                    activateTool("弧长", "点击目标圆弧", MeasureMode.ARC)
                },
                "半径" to {
                    activateTool("半径", "点击目标圆或圆弧", MeasureMode.RADIUS)
                },
                "比例校准" to { showScaleDialog() },
            ),
        )
    }

    /** 移动端只保留文字标注和标注管理。 */
    private fun showDrawPanel() {
        showToolSheet(
            "标注",
            "只保留文字标注；测量和文字标注统一在标注管理中维护。",
            listOf(
                "文字标注" to {
                    activateTool("文字标注", "拖动准星定位，松开后输入文字", MeasureMode.TEXT)
                },
                "标注管理" to { showPinListDialog() },
            ),
        )
    }

    private fun showToolPanel() {
        val bgDark = getSharedPreferences("skdw_settings",
            Context.MODE_PRIVATE).getBoolean("bg_dark", true)
        showToolSheet(
            "更多",
            "按需使用；测量颜色和背景会立即应用。",
            listOf(
                "文字查找" to { showTextFindDialog() },
                "提取文字" to { showExtractTextDialog() },
                (if (bgDark) "白底显示" else "黑底显示") to {
                    getSharedPreferences("skdw_settings", Context.MODE_PRIVATE)
                        .edit().putBoolean("bg_dark", !bgDark).apply()
                    drawingView.lightBackground = bgDark
                    showToolPanel()
                },
                "测量颜色" to { showMeasureColorSheet() },
                "分享截图" to { shareScreenshot() },
                "清除测量" to {
                    AlertDialog.Builder(this)
                        .setTitle("清除测量")
                        .setMessage("删除当前空间的所有测量结果？")
                        .setPositiveButton("清除") { _, _ ->
                            drawingView.clearMeasurements()
                            captureCurrentMeasurements()
                            savePins()
                            drawingView.cancelInteraction()
                            actionBar.visibility = View.GONE
                        }
                        .setNegativeButton("取消", null)
                        .show()
                },
            ),
        )
    }

    private fun showMeasureColorSheet() {
        val colors = listOf(
            "绿色" to 0xFF31D2A0.toInt(),
            "黄色" to 0xFFFFC94D.toInt(),
            "青色" to 0xFF54D7FF.toInt(),
            "橙色" to 0xFFFF9B58.toInt(),
            "红色" to 0xFFFF6B6B.toInt(),
            "白色" to 0xFFF2F5F7.toInt(),
        )
        showToolSheet(
            "测量颜色",
            "同时应用于尺寸线、结果文字和点控制柄。",
            colors.map { (name, color) ->
                name to {
                    drawingView.measureColor = color
                    getSharedPreferences("skdw_settings", Context.MODE_PRIVATE)
                        .edit().putInt("measure_color", color).apply()
                }
            },
        )
    }

    /** 模型/布局切换菜单（顶栏按钮） */
    private fun showSpaceMenu() {
        if (spaces.size <= 1) {
            Toast.makeText(this, "此图纸没有布局", Toast.LENGTH_SHORT).show()
            return
        }
        val labels = spaces.map {
            (if (spaces.indexOf(it) == currentSpace) "● " else "") + it.name
        }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("模型/布局")
            .setItems(labels) { _, which -> switchSpace(which) }
            .show()
    }

    /** 文字/引线内容输入（标注绘制用） */
    private fun showAnnotationTextInput(kind: MeasureMode, pos: Pair<Double, Double>) {
        val et = android.widget.EditText(this).apply {
            hint = "输入文字内容"
            setSingleLine(true)
            setPadding(48, 40, 48, 0)
        }
        AlertDialog.Builder(this)
            .setTitle(if (kind == MeasureMode.LEADER) "引线文字" else "添加文字")
            .setView(et)
            .setPositiveButton("确定") { _, _ ->
                val t = et.text.toString().trim()
                when (kind) {
                    MeasureMode.TEXT -> {
                        if (t.isNotEmpty()) {
                            drawingView.addAnnotation(MeasureMode.TEXT, listOf(pos), t)
                            saveAnnotations()
                        }
                    }
                    MeasureMode.LEADER -> {
                        // 引线已在图上落点生成，此处回填文字；取消则删除
                        val a = drawingView.pendingLeader
                        drawingView.pendingLeader = null
                        if (a != null) {
                            if (t.isEmpty()) drawingView.removeAnnotation(a)
                            else { a.text = t; saveAnnotations() }
                        }
                    }
                    else -> {}
                }
                drawingView.setMode(MeasureMode.NONE)
            }
            .setNegativeButton("取消") { _, _ ->
                if (kind == MeasureMode.LEADER) {
                    drawingView.pendingLeader?.let { drawingView.removeAnnotation(it) }
                    drawingView.pendingLeader = null
                }
                drawingView.setMode(MeasureMode.NONE)
            }
            .show()
    }

    /** 文字查找：输入关键词 → 列出匹配文字 → 点击定位 */
    private fun showTextFindDialog() {
        val input = android.widget.EditText(this).apply {
            hint = "输入文字关键词"
            setSingleLine(true)
            setPadding(48, 40, 48, 0)
        }
        AlertDialog.Builder(this)
            .setTitle("文字查找")
            .setView(input)
            .setPositiveButton("查找") { _, _ ->
                val q = input.text.toString().trim()
                if (q.isNotEmpty()) {
                    val hits = drawingView.findTexts(q)
                    if (hits.isEmpty()) {
                        Toast.makeText(this, "没有找到「$q」", Toast.LENGTH_SHORT).show()
                    } else {
                        val labels = hits.map { it.first.take(30) }.toTypedArray()
                        AlertDialog.Builder(this)
                            .setTitle("找到 ${hits.size} 处")
                            .setItems(labels) { _, which ->
                                val (_, pos) = hits[which]
                                drawingView.centerOn(pos.first, pos.second)
                            }
                            .setNegativeButton("取消", null)
                            .show()
                    }
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /** 自动识别，或按 PC 端方式拾取已知长度的两个端点。 */
    private fun showScaleDialog() {
        val cur = drawingView.unitScale
        val opts = listOf("自动识别图纸比例", "手动拾取两点校准", "恢复 1:1")
        AlertDialog.Builder(this)
            .setTitle("比例校准（当前 ${if (cur == 1.0) "1:1" else "×$cur"}）")
            .setItems(opts.toTypedArray()) { _, which ->
                when (which) {
                    0 -> performAutoCalibration()
                    1 -> activateTool(
                        "校准比例",
                        "选择已知长度的两个端点",
                        MeasureMode.CALIBRATE,
                    )
                    else -> setScale(1.0)
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun performAutoCalibration() {
        loadingPanel.text = "正在识别原图尺寸…"
        loadingOverlay.visibility = View.VISIBLE
        parseExecutor.execute {
            val result = drawingView.inferAutoCalibration()
            runOnUiThread {
                loadingOverlay.visibility = View.GONE
                if (result == null) {
                    Toast.makeText(
                        this,
                        "未找到足够一致的尺寸样本，请使用手动校准",
                        Toast.LENGTH_LONG,
                    ).show()
                } else {
                    setScale(result.scale)
                    Toast.makeText(
                        this,
                        "自动校准完成：×%.4f（%d 个尺寸样本）"
                            .format(result.scale, result.samples),
                        Toast.LENGTH_LONG,
                    ).show()
                }
            }
        }
    }

    private fun showManualCalibrationInput(rawLength: Double) {
        val input = android.widget.EditText(this).apply {
            hint = "输入图纸上标注的已知长度"
            setSingleLine(true)
            inputType = android.text.InputType.TYPE_CLASS_NUMBER or
                    android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL
            setPadding(dp(14), dp(10), dp(14), dp(10))
        }
        AlertDialog.Builder(this)
            .setTitle("手动校准")
            .setMessage("拾取长度：%.3f\n请输入原图标注的已知长度：".format(rawLength))
            .setView(input)
            .setPositiveButton("应用") { _, _ ->
                val known = input.text.toString().toDoubleOrNull()
                if (known != null && known > 0.0) {
                    setScale(known / rawLength)
                } else {
                    Toast.makeText(this, "请输入大于 0 的长度", Toast.LENGTH_SHORT).show()
                }
                drawingView.setMode(MeasureMode.NONE)
                actionBar.visibility = View.GONE
                activeTab = 0
                refreshDock()
            }
            .setNegativeButton("取消") { _, _ ->
                drawingView.cancelInteraction()
                actionBar.visibility = View.GONE
                activeTab = 0
                refreshDock()
            }
            .show()
    }

    private fun setScale(v: Double) {
        drawingView.unitScale = v
        viewPrefs().edit().putString("sc:" + source.absolutePath, v.toString()).apply()
        Toast.makeText(this, "比例已设置 ×$v", Toast.LENGTH_SHORT).show()
    }

    /** 新建编号标注：输入备注（可空） */
    private fun showPinNoteDialog(pin: NotePin) {
        val et = android.widget.EditText(this).apply {
            hint = "备注（可留空）"
            setSingleLine(true)
            setPadding(48, 40, 48, 0)
            setText(pin.note)
        }
        AlertDialog.Builder(this)
            .setTitle("标注 #${pin.id}")
            .setView(et)
            .setPositiveButton("保存") { _, _ ->
                pin.note = et.text.toString().trim()
                savePins()
                drawingView.invalidate()
            }
            .setNegativeButton("删除标注") { _, _ ->
                drawingView.removePin(pin)
                savePins()
            }
            .show()
    }

    /** 点到已有标注：查看/改备注/删除 */
    private fun showPinDetail(pin: NotePin) {
        val opts = ArrayList<String>()
        if (pin.note.isNotEmpty()) opts.add("备注：${pin.note.take(24)}")
        if (pin.photoPath != null) opts.add("查看照片")
        opts.add("修改备注")
        opts.add("删除标注")
        AlertDialog.Builder(this)
            .setTitle("标注 #${pin.id}")
            .setItems(opts.toTypedArray()) { _, which ->
                when (opts[which]) {
                    "查看照片" -> showPhoto(pin)
                    "修改备注" -> showPinNoteDialog(pin)
                    "删除标注" -> { drawingView.removePin(pin); savePins() }
                }
            }
            .setNegativeButton("关闭", null)
            .show()
    }

    private fun showMeasurementActions(measurement: Measurement) {
        val actions = arrayOf("修改端点", "删除标注")
        AlertDialog.Builder(this)
            .setTitle(measurement.label(drawingView.unitScale).replace("\n", "，"))
            .setItems(actions) { _, which ->
                when (which) {
                    0 -> drawingView.beginMeasurementEdit(measurement)
                    else -> confirmDeleteMeasurement(measurement)
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun showAnnotationActions(annotation: Annotation) {
        val kind = when (annotation.kind) {
            MeasureMode.TEXT -> "文字"
            MeasureMode.LINE -> "直线"
            MeasureMode.SKETCH -> "任意线"
            MeasureMode.CLOUD -> "云线"
            MeasureMode.LEADER -> "引线"
            else -> "标注"
        }
        val editAction = if (annotation.kind == MeasureMode.TEXT) "修改位置" else "修改端点"
        val actions = arrayOf(editAction, "删除标注")
        AlertDialog.Builder(this)
            .setTitle("$kind${annotation.text.takeIf { it.isNotBlank() }?.let { " · $it" } ?: ""}")
            .setItems(actions) { _, which ->
                when (which) {
                    0 -> drawingView.beginAnnotationEdit(annotation)
                    else -> confirmDeleteAnnotation(annotation)
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun confirmDeleteMeasurement(measurement: Measurement) {
        AlertDialog.Builder(this)
            .setTitle("删除测量")
            .setMessage(measurement.label(drawingView.unitScale).replace("\n", "，") +
                    "\n\n删除后不可恢复。")
            .setPositiveButton("删除") { _, _ ->
                drawingView.removeMeasurement(measurement)
                captureCurrentMeasurements()
                savePins()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun confirmDeleteAnnotation(annotation: Annotation) {
        AlertDialog.Builder(this)
            .setTitle("删除标注")
            .setMessage("删除后不可恢复。")
            .setPositiveButton("删除") { _, _ ->
                drawingView.removeAnnotation(annotation)
                savePins()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /** 标注明细：统一管理测量、文字/手绘标注和编号/照片标注。 */
    private fun showPinListDialog() {
        data class Entry(
            val label: String,
            val locate: () -> Unit,
            val detail: (() -> Unit)?,
            val delete: () -> Unit,
        )
        val entries = ArrayList<Entry>()
        for (m in drawingView.measurements.asReversed()) {
            entries.add(Entry(
                "测量 · " + m.label(drawingView.unitScale).replace("\n", "，"),
                { m.points.firstOrNull()?.let { drawingView.centerOn(it.first, it.second) } },
                null,
                { drawingView.removeMeasurement(m) },
            ))
        }
        for (a in drawingView.annotations.asReversed()) {
            val kind = when (a.kind) {
                MeasureMode.TEXT -> "文字"
                MeasureMode.LINE -> "直线"
                MeasureMode.SKETCH -> "任意线"
                MeasureMode.CLOUD -> "云线"
                MeasureMode.LEADER -> "引线"
                else -> "标注"
            }
            val summary = a.text.take(24).ifBlank { kind }
            entries.add(Entry(
                "$kind · $summary",
                { a.points.firstOrNull()?.let { drawingView.centerOn(it.first, it.second) } },
                null,
                { drawingView.removeAnnotation(a) },
            ))
        }
        for (p in drawingView.pins.asReversed()) {
            entries.add(Entry(
                "#${p.id} ${if (p.photoPath != null) "照片 " else ""}${p.note.take(22)}",
                { drawingView.centerOnPin(p) },
                { showPinDetail(p) },
                { drawingView.removePin(p) },
            ))
        }
        if (entries.isEmpty()) {
            Toast.makeText(this, "暂无测量或标注", Toast.LENGTH_SHORT).show()
            return
        }
        val labels = entries.map { it.label }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("标注明细（${entries.size}）")
            .setItems(labels) { _, which ->
                val entry = entries[which]
                val actions = if (entry.detail != null) {
                    arrayOf("查看详情", "定位", "删除")
                } else {
                    arrayOf("定位", "删除")
                }
                AlertDialog.Builder(this)
                    .setTitle(entry.label)
                    .setItems(actions) { _, action ->
                        when (actions[action]) {
                            "查看详情" -> entry.detail?.invoke()
                            "定位" -> entry.locate()
                            "删除" -> {
                                entry.delete()
                                captureCurrentMeasurements()
                                savePins()
                            }
                        }
                    }
                    .setNegativeButton("取消", null)
                    .show()
            }
            .setPositiveButton("全部清除") { _, _ ->
                AlertDialog.Builder(this)
                    .setTitle("全部清除")
                    .setMessage("清除当前图纸的测量、文字/手绘标注和编号/照片标注？")
                    .setPositiveButton("清除") { _, _ ->
                        drawingView.clearMeasurements()
                        drawingView.clearAnnotations()
                        drawingView.clearPins()
                        captureCurrentMeasurements()
                        savePins()
                    }
                    .setNegativeButton("取消", null)
                    .show()
            }
            .setNeutralButton("关闭", null)
            .show()
    }

    /** 查看标注照片 */
    private fun showPhoto(pin: NotePin) {
        val path = pin.photoPath ?: return
        val bmp = decodeScaled(path, 2048)
        if (bmp == null) {
            Toast.makeText(this, "照片已失效", Toast.LENGTH_SHORT).show()
            return
        }
        val dialog = Dialog(this)
        val frame = FrameLayout(this).apply { setBackgroundColor(0xEE101318.toInt()) }
        val iv = android.widget.ImageView(this).apply { setImageBitmap(bmp) }
        frame.addView(iv, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
            Gravity.CENTER))
        dialog.setContentView(frame)
        dialog.window?.apply {
            setBackgroundDrawableResource(android.R.color.transparent)
            setLayout(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT)
        }
        dialog.setCanceledOnTouchOutside(true)
        frame.setOnClickListener { dialog.dismiss() }
        dialog.show()
    }

    private fun decodeScaled(path: String, maxDim: Int): android.graphics.Bitmap? {
        return try {
            val bounds = android.graphics.BitmapFactory.Options().apply {
                inJustDecodeBounds = true
            }
            android.graphics.BitmapFactory.decodeFile(path, bounds)
            var sample = 1
            while (bounds.outWidth / sample > maxDim ||
                bounds.outHeight / sample > maxDim) sample *= 2
            android.graphics.BitmapFactory.decodeFile(path,
                android.graphics.BitmapFactory.Options().apply { inSampleSize = sample })
        } catch (_: Exception) { null }
    }

    /** 提取文字：列出图上全部文字，可复制 */
    private fun showExtractTextDialog() {
        val doc = modelDoc ?: return
        thread {
            val texts = drawingView.allTexts(doc)
            runOnUiThread {
                if (texts.isEmpty()) {
                    Toast.makeText(this, "图上没有文字", Toast.LENGTH_SHORT).show()
                    return@runOnUiThread
                }
                val sb = StringBuilder()
                for (t in texts) sb.append(t).append('\n')
                val et = android.widget.EditText(this).apply {
                    setText(sb.toString())
                    gravity = Gravity.TOP
                    minLines = 6
                }
                val scroll = android.widget.ScrollView(this)
                scroll.addView(et)
                AlertDialog.Builder(this)
                    .setTitle("提取文字（${texts.size} 条）")
                    .setView(scroll)
                    .setPositiveButton("复制") { _, _ ->
                        val cm = getSystemService(Context.CLIPBOARD_SERVICE)
                                as android.content.ClipboardManager
                        cm.setPrimaryClip(
                            android.content.ClipData.newPlainText("text", sb.toString()))
                        Toast.makeText(this, "已复制全部文字", Toast.LENGTH_SHORT).show()
                    }
                    .setNegativeButton("关闭", null)
                    .show()
            }
        }
    }

    // ---- 编号/照片标注持久化（按图纸路径，JSON 文件） ----

    private fun notesFile(): File {
        val dir = File(filesDir, "notes"); dir.mkdirs()
        val key = android.util.Base64.encodeToString(
            source.absolutePath.toByteArray(),
            android.util.Base64.NO_WRAP or android.util.Base64.URL_SAFE)
        return File(dir, key.filter { it.isLetterOrDigit() }.takeLast(60) + ".json")
    }

    private fun savePins() {
        try {
            captureCurrentMeasurements()
            val arr = org.json.JSONArray()
            for (p in drawingView.pins) arr.put(org.json.JSONObject().apply {
                put("id", p.id); put("x", p.x); put("y", p.y)
                put("note", p.note); put("photo", p.photoPath)
            })
            for (a in drawingView.annotations) arr.put(org.json.JSONObject().apply {
                put("anno", 1); put("id", a.id); put("kind", a.kind.name)
                put("text", a.text)
                val pts = org.json.JSONArray()
                for (pt in a.points) { pts.put(pt.first); pts.put(pt.second) }
                put("pts", pts)
            })
            for ((space, items) in measurementsBySpace.toSortedMap()) {
                for (m in items) arr.put(org.json.JSONObject().apply {
                    put("measure", 1)
                    put("space", space)
                    put("kind", m.kind.name)
                    val pts = org.json.JSONArray()
                    for (pt in m.points) { pts.put(pt.first); pts.put(pt.second) }
                    put("pts", pts)
                    val edges = org.json.JSONArray()
                    for (edge in m.edgeLengths) edges.put(edge)
                    put("edges", edges)
                    put("total", m.total)
                    put("area", m.area)
                    put("perimeter", m.perimeter)
                    put("radius", m.radius)
                    put("extra", m.extra)
                })
            }
            notesFile().writeText(arr.toString())
        } catch (_: Exception) {}
    }

    private fun loadPins() {
        try {
            val f = notesFile()
            if (!f.isFile) return
            val arr = org.json.JSONArray(f.readText())
            drawingView.pins.clear()
            drawingView.annotations.clear()
            measurementsBySpace.clear()
            var maxId = 0
            var maxAnnoId = 0
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                if (o.optInt("measure") == 1) {
                    val kind = try { MeasureMode.valueOf(o.getString("kind")) }
                               catch (_: Exception) { MeasureMode.LENGTH }
                    fun readPoints(key: String = "pts"): List<Pair<Double, Double>> {
                        val values = o.optJSONArray(key) ?: return emptyList()
                        val result = ArrayList<Pair<Double, Double>>()
                        var k = 0
                        while (k + 1 < values.length()) {
                            result.add(Pair(values.optDouble(k), values.optDouble(k + 1)))
                            k += 2
                        }
                        return result
                    }
                    val edgesArr = o.optJSONArray("edges")
                    val edges = ArrayList<Double>()
                    if (edgesArr != null) {
                        for (k in 0 until edgesArr.length()) {
                            edges.add(edgesArr.optDouble(k))
                        }
                    }
                    val measurement = Measurement(
                        kind,
                        readPoints(),
                        edges,
                        o.optDouble("total"),
                        o.optDouble("area"),
                        o.optDouble("perimeter"),
                        o.optDouble("radius"),
                        o.optString("extra"),
                    )
                    val space = o.optInt("space", 0).coerceAtLeast(0)
                    measurementsBySpace.getOrPut(space) { ArrayList() }.add(measurement)
                } else if (o.optInt("anno") == 1) {
                    val id = o.optInt("id", i + 1)
                    if (id > maxAnnoId) maxAnnoId = id
                    val kind = try { MeasureMode.valueOf(o.getString("kind")) }
                               catch (_: Exception) { MeasureMode.TEXT }
                    val ptsArr = o.optJSONArray("pts")
                    val pts = ArrayList<Pair<Double, Double>>()
                    if (ptsArr != null) {
                        var k = 0
                        while (k + 1 < ptsArr.length()) {
                            pts.add(Pair(ptsArr.optDouble(k), ptsArr.optDouble(k + 1)))
                            k += 2
                        }
                    }
                    drawingView.annotations.add(
                        Annotation(id, kind, pts, o.optString("text")))
                } else {
                    val id = o.optInt("id", i + 1)
                    if (id > maxId) maxId = id
                    drawingView.pins.add(NotePin(id, o.getDouble("x"), o.getDouble("y"),
                        o.optString("note"),
                        o.optString("photo").takeIf { it.isNotEmpty() }))
                }
            }
            drawingView.setNextPinId(maxId + 1)
            drawingView.setNextAnnoId(maxAnnoId + 1)
            if (currentSpace == 0) {
                drawingView.setMeasurements(
                    measurementsBySpace[0]?.toList().orEmpty())
            }
            drawingView.invalidate()
        } catch (_: Exception) {}
    }

    private fun saveAnnotations() = savePins()

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int,
                                  data: android.content.Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 4201) return
        val pt = pendingPhotoPoint
        pendingPhotoPoint = null
        val uri = data?.data
        if (resultCode != RESULT_OK || pt == null || uri == null) return
        thread {
            try {
                val dir = File(filesDir, "photos"); dir.mkdirs()
                val outFile = File(dir, "ph_${System.currentTimeMillis()}.jpg")
                contentResolver.openInputStream(uri)?.use { inp ->
                    outFile.outputStream().use { inp.copyTo(it) }
                }
                runOnUiThread {
                    drawingView.addPin(pt.first, pt.second, "", outFile.absolutePath)
                        .let { drawingView.centerOnPin(it) }
                    savePins()
                    Toast.makeText(this, "已添加照片标注", Toast.LENGTH_SHORT).show()
                }
            } catch (e: Exception) {
                runOnUiThread {
                    Toast.makeText(this, "照片保存失败：${e.message}",
                        Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    private fun applySettings() {
        val prefs = getSharedPreferences("skdw_settings", Context.MODE_PRIVATE)
        drawingView.lightBackground = !prefs.getBoolean("bg_dark", true)
        drawingView.measureColor =
            prefs.getInt("measure_color", 0xFF31D2A0.toInt())
        // 测量比例记忆（按图纸）
        viewPrefs().getString("sc:" + source.absolutePath, null)
            ?.toDoubleOrNull()?.let { drawingView.unitScale = it }
    }

    /** 切换模型/布局；布局文档懒加载 */
    private fun switchSpace(index: Int) {
        if (index < 0 || index >= spaces.size) return
        if (index != currentSpace && drawingView.document != null) {
            captureCurrentMeasurements()
        }
        currentSpace = index
        refreshSpaceBar()
        val cached = docCache[index]
        if (cached != null) {
            applySpace(index, cached)
            return
        }
        val file = spaces[index].file ?: return
        loadingPanel.text = "加载${spaces[index].name}…"
        loadingOverlay.visibility = View.VISIBLE
        thread {
            val doc = SkdwDocument.read(file)
            runOnUiThread {
                loadingOverlay.visibility = View.GONE
                if (doc != null) {
                    docCache[index] = doc
                    if (currentSpace == index) applySpace(index, doc)
                } else {
                    Toast.makeText(this, "${spaces[index].name} 加载失败",
                        Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    private fun applySpace(index: Int, doc: SkdwDocument) {
        applyLayerState(doc)
        drawingView.load(doc)
        // 布局页把模型空间文档设为视口内容来源；模型页清空
        drawingView.setOverlay(if (index == 0) null else modelDoc)
        drawingView.setMeasurements(
            measurementsBySpace[index]?.toList().orEmpty())
        if (index == pendingRestoreSpace) {
            pendingRestore?.let { drawingView.restoreView(it.first, it.second, it.third) }
            pendingRestore = null
            pendingRestoreSpace = -1
        }
    }

    /** 模型空间和每张布局分别保存自己的测量记录。 */
    private fun captureCurrentMeasurements() {
        if (drawingView.document == null) return
        measurementsBySpace[currentSpace] =
            ArrayList(drawingView.measurements)
    }

    // ---- 视图记忆 ----

    private fun viewPrefs() =
        getSharedPreferences("skdw_view_state", Context.MODE_PRIVATE)

    private fun readViewState() {
        pendingRestore = null
        pendingRestoreSpace = 0
        val saved = viewPrefs().getString("v:" + source.absolutePath, null) ?: return
        val parts = saved.split(",")
        val sp = parts.getOrNull(0)?.toIntOrNull() ?: return
        val cx = parts.getOrNull(1)?.toDoubleOrNull() ?: return
        val cy = parts.getOrNull(2)?.toDoubleOrNull() ?: return
        val sc = parts.getOrNull(3)?.toDoubleOrNull() ?: return
        // 记录保存时的图纸范围：与当前文档不一致（重新解析过/版本升级）
        // 说明旧视图坐标已失效，丢弃，重新适配全图（旧格式无范围同样丢弃）
        val bx0 = parts.getOrNull(4)?.toDoubleOrNull() ?: return
        val bx1 = parts.getOrNull(5)?.toDoubleOrNull() ?: return
        if (modelDoc != null) {
            val tol = 1e-6 * (Math.abs(bx0) + Math.abs(bx1) + 1.0)
            if (Math.abs(bx0 - modelDoc!!.minX) > tol ||
                Math.abs(bx1 - modelDoc!!.maxX) > tol) return
        }
        if (sp < 0 || sp >= spaces.size) return
        pendingRestoreSpace = sp
        pendingRestore = Triple(cx, cy, sc)
    }

    override fun onPause() {
        super.onPause()
        savePins()   // 标注/测量相关 pin 与手绘标注落盘
        val vs = drawingView.viewState() ?: return
        val b = modelDoc
        val bounds = if (b != null) ",${b.minX},${b.maxX}" else ""
        viewPrefs().edit()
            .putString("v:" + source.absolutePath,
                "$currentSpace,${vs.first},${vs.second},${vs.third}$bounds")
            .apply()
    }

    /** 把当前图层可见性状态应用到文档（懒加载的布局文档也保持一致） */
    private fun applyLayerState(doc: SkdwDocument) {
        val lv = layerVisible ?: return
        for (i in doc.layers.indices) if (i < lv.size) doc.layers[i].visible = lv[i]
    }

    private fun syncLayerVisibility() {
        val lv = layerVisible ?: return
        for (doc in docCache.values) applyLayerState(doc)
        saveLayerState()
        drawingView.invalidate()
    }

    // ---- 图层状态持久化（按图纸路径记忆显隐） ----

    private fun layerPrefs() =
        getSharedPreferences("skdw_layer_state", Context.MODE_PRIVATE)

    private fun layerStateKey() = "lv:" + source.absolutePath

    private fun saveLayerState() {
        val lv = layerVisible ?: return
        layerPrefs().edit()
            .putString(layerStateKey(), lv.joinToString("") { if (it) "1" else "0" })
            .apply()
    }

    private fun restoreLayerState() {
        val lv = layerVisible ?: return
        val saved = layerPrefs().getString(layerStateKey(), null) ?: return
        if (saved.length != lv.size) return
        for (i in lv.indices) lv[i] = saved[i] == '1'
        for (doc in docCache.values) applyLayerState(doc)
    }

    // ---- 截图保存/分享 ----

    private fun shareScreenshot() {
        try {
            val bmp = android.graphics.Bitmap.createBitmap(
                drawingView.width.coerceAtLeast(1),
                drawingView.height.coerceAtLeast(1),
                android.graphics.Bitmap.Config.ARGB_8888)
            drawingView.draw(android.graphics.Canvas(bmp))
            val ts = java.text.SimpleDateFormat("yyyyMMdd_HHmmss",
                java.util.Locale.CHINA).format(java.util.Date())
            val name = "Sukancad_$ts.png"

            // 保存到相册 Pictures/Sukancad（API 29+ 走 MediaStore，无需权限）
            var albumOk = false
            if (android.os.Build.VERSION.SDK_INT >= 29) {
                val values = android.content.ContentValues().apply {
                    put(android.provider.MediaStore.Images.Media.DISPLAY_NAME, name)
                    put(android.provider.MediaStore.Images.Media.MIME_TYPE, "image/png")
                    put(android.provider.MediaStore.Images.Media.RELATIVE_PATH,
                        "Pictures/Sukancad")
                }
                val uri = contentResolver.insert(
                    android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                if (uri != null) {
                    contentResolver.openOutputStream(uri)?.use { out ->
                        bmp.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, out)
                    }
                    albumOk = true
                }
            }

            // 分享面板（FileProvider）
            val shareDir = File(cacheDir, "share")
            shareDir.mkdirs()
            val shareFile = File(shareDir, name)
            shareFile.outputStream().use { out ->
                bmp.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, out)
            }
            val shareUri = androidx.core.content.FileProvider.getUriForFile(
                this, "$packageName.files", shareFile)
            val intent = android.content.Intent(android.content.Intent.ACTION_SEND).apply {
                type = "image/png"
                putExtra(android.content.Intent.EXTRA_STREAM, shareUri)
                addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            startActivity(android.content.Intent.createChooser(
                intent, "分享图纸截图"))
            bmp.recycle()
            Toast.makeText(this, if (albumOk) "已保存到相册"
            else "截图已生成", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            Toast.makeText(this, "截图失败：${e.message}", Toast.LENGTH_SHORT).show()
        }
    }

    /** 图层面板：点击切换显隐，长按仅显示该层 */
    @SuppressLint("SetTextI18n")
    @Suppress("unused")
    private fun showLegacyLayerDialog() {
        val doc = modelDoc ?: return
        val lv = layerVisible ?: return
        val counts = IntArray(doc.layers.size)
        for (e in doc.entities) if (e.layer in counts.indices) counts[e.layer]++

        val dialog = Dialog(this)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(40, 32, 40, 32)
            background = android.graphics.drawable.GradientDrawable().apply {
                setColor(0xF51A1E24.toInt())
                cornerRadius = 28f
            }
        }
        val titleRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        titleRow.addView(TextView(this).apply {
            text = "图层（${doc.layers.size}）"
            setTextColor(0xFFE6ECF5.toInt())
            textSize = 16f
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        titleRow.addView(Button(this).apply {
            text = "全开"
            textSize = 12f
            isAllCaps = false
            setTextColor(0xFF31D2A0.toInt())
            setBackgroundColor(Color.TRANSPARENT)
            setOnClickListener {
                for (i in lv.indices) lv[i] = true
                syncLayerVisibility()
                dialog.dismiss()
                Toast.makeText(this@DrawingActivity, "已打开所有图层", Toast.LENGTH_SHORT).show()
            }
        })
        titleRow.addView(Button(this).apply {
            text = "全关"
            textSize = 12f
            isAllCaps = false
            setTextColor(0xFFC9D2DC.toInt())
            setBackgroundColor(Color.TRANSPARENT)
            setOnClickListener {
                for (i in lv.indices) lv[i] = false
                syncLayerVisibility()
                dialog.dismiss()
                Toast.makeText(this@DrawingActivity, "已关闭所有图层", Toast.LENGTH_SHORT).show()
            }
        })
        titleRow.addView(Button(this).apply {
            text = "恢复初始"
            textSize = 12f
            isAllCaps = false
            setTextColor(0xFFFFC94D.toInt())
            setBackgroundColor(Color.TRANSPARENT)
            setOnClickListener {
                initialLayerVisible?.let { init ->
                    for (i in lv.indices) lv[i] = init[i]
                }
                syncLayerVisibility()
                dialog.dismiss()
                Toast.makeText(this@DrawingActivity, "图层显示已恢复到初始状态",
                    Toast.LENGTH_SHORT).show()
            }
        })
        root.addView(titleRow, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        val list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        for (i in doc.layers.indices) {
            val layer = doc.layers[i]
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(8, 20, 8, 20)
            }
            val dot = TextView(this).apply {
                text = "●"
                textSize = 15f
                setTextColor(if (layer.argb == 0xFF000000.toInt()) 0xFF9AA3AE.toInt()
                             else layer.argb)
            }
            row.addView(dot)
            row.addView(TextView(this).apply {
                text = " ${layer.name}"
                setTextColor(0xFFC9D2DC.toInt())
                textSize = 14f
                maxWidth = (resources.displayMetrics.widthPixels * 0.4f).toInt()
                ellipsize = android.text.TextUtils.TruncateAt.END
                setSingleLine(true)
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(TextView(this).apply {
                text = "${counts[i]} "
                setTextColor(0xFF77808B.toInt())
                textSize = 12f
            })
            val state = TextView(this).apply {
                textSize = 13f
                text = if (lv[i]) "显示" else "隐藏"
                setTextColor(if (lv[i]) 0xFF31D2A0.toInt() else 0xFFFF8A8A.toInt())
            }
            row.addView(state)
            fun refresh() {
                state.text = if (lv[i]) "显示" else "隐藏"
                state.setTextColor(if (lv[i]) 0xFF31D2A0.toInt() else 0xFFFF8A8A.toInt())
                row.alpha = if (lv[i]) 1f else 0.45f
            }
            refresh()
            row.setOnClickListener {
                lv[i] = !lv[i]
                syncLayerVisibility()
                refresh()
            }
            row.setOnLongClickListener {
                for (j in lv.indices) lv[j] = j == i
                syncLayerVisibility()
                dialog.dismiss()
                Toast.makeText(this, "仅显示「${layer.name}」", Toast.LENGTH_SHORT).show()
                true
            }
            list.addView(row)
        }
        val scroll = android.widget.ScrollView(this)
        scroll.addView(list)
        root.addView(scroll, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            (resources.displayMetrics.heightPixels * 0.5f).toInt()))
        dialog.setContentView(root)
        dialog.window?.apply {
            setBackgroundDrawableResource(android.R.color.transparent)
            setLayout((resources.displayMetrics.widthPixels * 0.7f).toInt(),
                ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        dialog.setCanceledOnTouchOutside(true)
        dialog.show()
    }

    private fun showLayerDialog() {
        val doc = modelDoc ?: return
        val lv = layerVisible ?: return
        val counts = IntArray(doc.layers.size)
        for (entity in doc.entities) {
            if (entity.layer in counts.indices) counts[entity.layer]++
        }

        val dialog = BottomSheetDialog(this)
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(10), dp(18), dp(24))
            background = rounded(0xFFF7F9FA.toInt(), 20f)
        }
        panel.addView(View(this).apply {
            background = rounded(0xFFD2D9DE.toInt(), 3f)
        }, LinearLayout.LayoutParams(dp(42), dp(5)).apply {
            gravity = Gravity.CENTER_HORIZONTAL
            bottomMargin = dp(14)
        })
        panel.addView(TextView(this).apply {
            text = "图层"
            textSize = 20f
            setTextColor(0xFF17232B.toInt())
        })
        panel.addView(TextView(this).apply {
            text = "点击开关显隐；长按图层名称可仅显示该层。"
            textSize = 13f
            setTextColor(0xFF6C7A84.toInt())
            setPadding(0, dp(4), 0, dp(12))
        })

        val actions = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
        }
        fun actionButton(label: String, action: () -> Unit): MaterialButton =
            MaterialButton(this).apply {
                text = label
                textSize = 13f
                isAllCaps = false
                cornerRadius = dp(10)
                minHeight = dp(42)
                setBackgroundColor(0xFFE8F1F4.toInt())
                setTextColor(0xFF164256.toInt())
                setOnClickListener {
                    action()
                    dialog.dismiss()
                }
            }
        actions.addView(actionButton("全开") {
            for (i in lv.indices) lv[i] = true
            syncLayerVisibility()
        }, LinearLayout.LayoutParams(0, dp(44), 1f).apply {
            marginEnd = dp(6)
        })
        actions.addView(actionButton("全关") {
            for (i in lv.indices) lv[i] = false
            syncLayerVisibility()
        }, LinearLayout.LayoutParams(0, dp(44), 1f).apply {
            marginEnd = dp(6)
        })
        actions.addView(actionButton("恢复初始") {
            initialLayerVisible?.let { initial ->
                for (i in lv.indices) lv[i] = initial[i]
            }
            syncLayerVisibility()
        }, LinearLayout.LayoutParams(0, dp(44), 1f))
        panel.addView(actions)

        val list = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(8), 0, 0)
        }
        for (i in doc.layers.indices) {
            val layer = doc.layers[i]
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dp(8), dp(9), dp(8), dp(9))
            }
            row.addView(TextView(this).apply {
                text = "●"
                textSize = 14f
                val r = (layer.argb shr 16) and 0xff
                val g = (layer.argb shr 8) and 0xff
                val b = layer.argb and 0xff
                setTextColor(
                    if (r > 225 && g > 225 && b > 225) 0xFF7D8992.toInt()
                    else layer.argb)
            })
            row.addView(TextView(this).apply {
                text = layer.name
                textSize = 14f
                maxLines = 1
                ellipsize = android.text.TextUtils.TruncateAt.END
                setTextColor(if (lv[i]) 0xFF263640.toInt() else 0xFF9AA4AB.toInt())
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = dp(8)
            })
            row.addView(TextView(this).apply {
                text = counts[i].toString()
                textSize = 11f
                setTextColor(0xFF8C99A2.toInt())
                setPadding(dp(6), 0, dp(8), 0)
            })
            row.addView(Switch(this).apply {
                isChecked = lv[i]
                setOnCheckedChangeListener { _, checked ->
                    lv[i] = checked
                    syncLayerVisibility()
                }
            })
            row.setOnLongClickListener {
                for (j in lv.indices) lv[j] = j == i
                syncLayerVisibility()
                dialog.dismiss()
                Toast.makeText(this, "仅显示「${layer.name}」", Toast.LENGTH_SHORT).show()
                true
            }
            list.addView(row)
        }
        val scroll = ScrollView(this).apply { addView(list) }
        panel.addView(scroll, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            minOf(
                dp(520),
                dp(56) * doc.layers.size + dp(16),
                (resources.displayMetrics.heightPixels * 0.58f).toInt(),
            ),
        ))
        dialog.setContentView(panel)
        dialog.setOnShowListener {
            dialog.behavior.state = BottomSheetBehavior.STATE_EXPANDED
            dialog.behavior.skipCollapsed = true
        }
        dialog.show()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (drawingView.mode != MeasureMode.NONE) {
            drawingView.cancelInteraction()
            actionBar.visibility = View.GONE
            activeTab = 0
            refreshDock()
            drawingView.refreshInfoPanel()
            return
        }
        super.onBackPressed()
    }

    override fun onDestroy() {
        super.onDestroy()
        parseExecutor.shutdown()
    }
}
