package com.sukancad.cad

import android.annotation.SuppressLint
import android.app.Dialog
import android.content.Context
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/** 图纸查看页：解析 → 渲染 → 测量 */
class DrawingActivity : AppCompatActivity() {

    private lateinit var drawingView: DrawingView
    private lateinit var infoPanel: TextView
    private lateinit var loadingPanel: TextView
    private val parseExecutor = Executors.newSingleThreadExecutor()

    private val modes = listOf(
        "长度" to MeasureMode.LENGTH,
        "连续" to MeasureMode.CONTINUOUS,
        "面积" to MeasureMode.AREA,
        "角度" to MeasureMode.ANGLE,
        "弧长" to MeasureMode.ARC,
        "半径" to MeasureMode.RADIUS,
        "坐标" to MeasureMode.COORD,
    )
    private val drawTools = listOf(
        "文字" to MeasureMode.TEXT,
        "直线" to MeasureMode.LINE,
        "任意线" to MeasureMode.SKETCH,
        "云线" to MeasureMode.CLOUD,
        "引线" to MeasureMode.LEADER,
        "编号" to MeasureMode.PIN,
        "照片" to MeasureMode.PHOTO,
    )
    private val modeButtons = ArrayList<Button>()
    /** 待关联照片的标注点（照片选择回来后用） */
    private var pendingPhotoPoint: Pair<Double, Double>? = null

    // 布局切换：0 = 模型，1..N = 图纸空间布局
    private data class Space(val name: String, val file: File?)
    private val spaces = ArrayList<Space>()
    private var currentSpace = 0
    private val spaceButtons = ArrayList<Button>()
    private var modelDoc: SkdwDocument? = null
    private val docCache = HashMap<Int, SkdwDocument>()

    // 图层可见性状态（与 modelDoc.layers 对齐；布局文档复用同一图层表）
    private var layerVisible: BooleanArray? = null
    private lateinit var source: File

    // 视图记忆（重开图纸回到上次看的位置/布局）
    private var pendingRestore: Triple<Double, Double, Double>? = null
    private var pendingRestoreSpace = 0

    // 底部导航 + 分类工具条（对标竞品结构）
    private lateinit var titleView: TextView
    private lateinit var spaceButton: Button
    private lateinit var toolScroll: android.widget.HorizontalScrollView
    private lateinit var toolPanel: LinearLayout
    private var activeTab = -1
    private var initialLayerVisible: BooleanArray? = null
    private val navViews = ArrayList<TextView>()

    @SuppressLint("SetTextI18n")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val path = intent.getStringExtra("path") ?: run { finish(); return }
        source = File(path)
        title = source.name
        DrawingLibrary.recordOpen(this, source.absolutePath)

        val root = FrameLayout(this)

        drawingView = DrawingView(this)
        root.addView(drawingView, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        loadingPanel = TextView(this).apply {
            text = "解析中…"
            setTextColor(0xFFE6ECF5.toInt())
            textSize = 15f
            gravity = Gravity.CENTER
            setBackgroundColor(0xCC101318.toInt())
            visibility = View.GONE
        }

        // 文字/引线输入回调（标注绘制）
        drawingView.onTextInput = { kind, pos ->
            runOnUiThread { showAnnotationTextInput(kind, pos) }
        }

        // 顶栏：返回 | 模型/布局 | 文件名 | 分享（对标竞品）
        val topBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(0xE61E2227.toInt())
            gravity = Gravity.CENTER_VERTICAL
            setPadding(16, 12, 16, 12)
        }
        val back = Button(this).apply {
            text = "‹"
            textSize = 16f
            setTextColor(0xFFE6ECF5.toInt())
            setBackgroundColor(Color.TRANSPARENT)
            setOnClickListener { finish() }
        }
        topBar.addView(back, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        spaceButton = Button(this).apply {
            textSize = 13f
            isAllCaps = false
            setTextColor(0xFF31D2A0.toInt())
            setBackgroundColor(Color.TRANSPARENT)
            visibility = View.GONE
            setOnClickListener { showSpaceMenu() }
        }
        topBar.addView(spaceButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        titleView = TextView(this).apply {
            text = source.name
            textSize = 15f
            setTextColor(0xFFE6ECF5.toInt())
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.MIDDLE
        }
        topBar.addView(titleView, LinearLayout.LayoutParams(
            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        val share = Button(this).apply {
            text = "分享"
            textSize = 13f
            isAllCaps = false
            setTextColor(0xFFC9D2DC.toInt())
            setBackgroundColor(Color.TRANSPARENT)
            setOnClickListener { shareScreenshot() }
        }
        topBar.addView(share, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        // 底部信息面板
        infoPanel = TextView(this).apply {
            text = ""
            setTextColor(0xFF31D2A0.toInt())
            textSize = 14f
            setLineSpacing(6f, 1f)
            setBackgroundColor(0xB3101318.toInt())
            setPadding(28, 18, 28, 22)
            visibility = View.GONE
        }
        drawingView.onMeasurementChanged = { text ->
            runOnUiThread {
                infoPanel.text = text
                infoPanel.visibility =
                    if (text.isEmpty()) View.GONE else View.VISIBLE
            }
        }
        // 选择模式点图元 → 显示属性
        drawingView.onEntityPicked = { text ->
            runOnUiThread {
                if (text.isNullOrEmpty()) {
                    drawingView.refreshInfoPanel()   // 恢复测量信息
                } else {
                    infoPanel.text = text
                    infoPanel.visibility = View.VISIBLE
                }
            }
        }
        // 定距标定已按竞品移除（比例在测量面板「比例」里设置）
        // 编号/照片标注
        drawingView.onPinAdded = { pin ->
            runOnUiThread { showPinNoteDialog(pin) }
        }
        drawingView.onPinTapped = { pin ->
            runOnUiThread { showPinDetail(pin) }
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

        // 分类工具条（底部导航上方，按 tab 切换内容）
        toolPanel = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(0xE6161A1F.toInt())
            setPadding(16, 6, 16, 6)
            gravity = Gravity.CENTER_VERTICAL
        }
        toolScroll = android.widget.HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            visibility = View.GONE
        }
        toolScroll.addView(toolPanel, ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        // 底部导航（对标竞品五键）：全图 / 测量 / 标注 / 图层 / 工具箱
        val navItems = listOf("全图" to 0, "测量" to 1, "标注" to 2, "图层" to 3, "工具箱" to 4)
        val bottomNav = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(0xF21E2227.toInt())
            setPadding(12, 16, 12, 20)
        }
        for ((label, tab) in navItems) {
            val tv = TextView(this).apply {
                text = label
                textSize = 15f
                gravity = Gravity.CENTER
                setPadding(0, 12, 0, 12)
                setOnClickListener { switchTab(tab) }
            }
            navViews.add(tv)
            bottomNav.addView(tv, LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        }

        val topStack = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        topStack.addView(topBar)
        root.addView(topStack, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.TOP))
        val bottomStack = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        bottomStack.addView(infoPanel)
        bottomStack.addView(toolScroll)
        bottomStack.addView(bottomNav)
        root.addView(bottomStack, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM))
        root.addView(loadingPanel, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        setContentView(root)
        applySettings()
        toolScroll.visibility = View.GONE   // 默认收起面板（浏览状态）

        // 解析（有缓存直接读缓存；缓存版本不符/损坏时重新解析）
        loadingPanel.visibility = View.VISIBLE
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
                loadingPanel.visibility = View.GONE
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

    private fun switchTab(tab: Int) {
        if (tab == 0) {
            // 全图：适配整个图纸范围（含散落图元），再点一次回主体
            activeTab = -1
            refreshNavStyles()
            toolScroll.visibility = View.GONE
            drawingView.fitToView(!drawingView.isFittedFull)
            return
        }
        activeTab = if (activeTab == tab) -1 else tab
        refreshNavStyles()
        when (activeTab) {
            1 -> showMeasurePanel()
            2 -> showDrawPanel()
            3 -> {
                toolScroll.visibility = View.GONE
                showLayerDialog()
            }
            4 -> showToolPanel()
            else -> toolScroll.visibility = View.GONE
        }
    }

    private fun refreshNavStyles() {
        for ((i, tv) in navViews.withIndex()) {
            tv.setTextColor(if (i == activeTab) 0xFF31D2A0.toInt()
            else 0xFFC9D2DC.toInt())
        }
    }

    private fun swapPanel(buttons: List<Button>) {
        toolPanel.removeAllViews()
        for (b in buttons) toolPanel.addView(b, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        toolScroll.visibility = View.VISIBLE
    }

    private fun showMeasurePanel() {
        modeButtons.clear()
        val buttons = ArrayList<Button>()
        for ((label, mode) in modes) {
            val btn = toolButton(label) {
                drawingView.setMode(mode)
                refreshToolbar()
            }
            modeButtons.add(btn)
            buttons.add(btn)
        }
        buttons.add(toolButton("比例") { showScaleDialog() })
        swapPanel(buttons)
        refreshToolbar()
    }

    /** 标注绘制面板：文字/直线/任意线/云线/引线/编号/照片 */
    private fun showDrawPanel() {
        val buttons = ArrayList<Button>()
        for ((label, mode) in drawTools) {
            buttons.add(toolButton(label) {
                drawingView.setMode(mode)
                refreshToolbar()
                Toast.makeText(this, when (mode) {
                    MeasureMode.TEXT -> "点击图纸放置文字"
                    MeasureMode.LINE -> "点击两点绘制直线"
                    MeasureMode.SKETCH -> "按住拖动绘制任意线"
                    MeasureMode.CLOUD -> "点击多个点绘制云线，靠近起点闭合"
                    MeasureMode.LEADER -> "点击箭头端和文字端"
                    MeasureMode.PIN -> "点击图纸放置编号标注"
                    MeasureMode.PHOTO -> "点击图纸位置，再选照片"
                    else -> ""
                }, Toast.LENGTH_SHORT).show()
            })
        }
        swapPanel(buttons)
        refreshToolbar()
    }

    private fun showToolPanel() {
        val bgDark = getSharedPreferences("skdw_settings",
            Context.MODE_PRIVATE).getBoolean("bg_dark", true)
        swapPanel(listOf(
            toolButton("文字查找") { showTextFindDialog() },
            toolButton("提取文字") { showExtractTextDialog() },
            toolButton(if (bgDark) "白底显示" else "黑底显示") {
                val prefs = getSharedPreferences("skdw_settings", Context.MODE_PRIVATE)
                prefs.edit().putBoolean("bg_dark", !bgDark).apply()
                drawingView.lightBackground = bgDark
                showToolPanel()   // 刷新按钮文字
            },
            toolButton("标注列表") { showPinListDialog() },
            toolButton("清除标注", 0xFFFF8A8A.toInt()) {
                drawingView.clearMeasurements()
                drawingView.setMode(MeasureMode.NONE)
                Toast.makeText(this, "已清除测量标注", Toast.LENGTH_SHORT).show()
            }
        ))
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

    /** 测量比例设置（1:1 / 1:10 / 1:100 / 自定义，按图纸记忆） */
    private fun showScaleDialog() {
        val cur = drawingView.unitScale
        val opts = listOf("1:1（原始单位）", "1:10", "1:100", "1:500", "1:1000", "自定义…")
        val vals = doubleArrayOf(1.0, 10.0, 100.0, 500.0, 1000.0)
        AlertDialog.Builder(this)
            .setTitle("测量比例（当前 ${if (cur == 1.0) "1:1" else "×${cur}"}）")
            .setItems(opts.toTypedArray()) { _, which ->
                if (which == 5) {
                    val et = android.widget.EditText(this).apply {
                        hint = "输入比例系数，如 100 或 0.001"
                        setSingleLine(true)
                        inputType = android.text.InputType.TYPE_CLASS_NUMBER or
                                android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL
                        setPadding(48, 40, 48, 0)
                    }
                    AlertDialog.Builder(this)
                        .setTitle("自定义比例")
                        .setView(et)
                        .setPositiveButton("确定") { _, _ ->
                            et.text.toString().toDoubleOrNull()
                                ?.takeIf { it > 0 }?.let { setScale(it) }
                        }
                        .setNegativeButton("取消", null)
                        .show()
                } else setScale(vals[which])
            }
            .setNegativeButton("取消", null)
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

    /** 标注列表：点击定位，长按详情 */
    private fun showPinListDialog() {
        val pins = drawingView.pins
        if (pins.isEmpty()) {
            Toast.makeText(this, "暂无编号/照片标注", Toast.LENGTH_SHORT).show()
            return
        }
        val labels = pins.map {
            "#${it.id} ${if (it.photoPath != null) "📷 " else ""}${it.note.take(20)}"
        }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("标注（${pins.size}）")
            .setItems(labels) { _, which -> drawingView.centerOnPin(pins[which]) }
            .setPositiveButton("全部清除") { _, _ ->
                drawingView.clearPins()
                savePins()
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
            var maxId = 0
            var maxAnnoId = 0
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                if (o.optInt("anno") == 1) {
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
        currentSpace = index
        refreshSpaceBar()
        val cached = docCache[index]
        if (cached != null) {
            applySpace(index, cached)
            return
        }
        val file = spaces[index].file ?: return
        loadingPanel.text = "加载${spaces[index].name}…"
        loadingPanel.visibility = View.VISIBLE
        thread {
            val doc = SkdwDocument.read(file)
            runOnUiThread {
                loadingPanel.visibility = View.GONE
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
        if (index == pendingRestoreSpace) {
            pendingRestore?.let { drawingView.restoreView(it.first, it.second, it.third) }
            pendingRestore = null
            pendingRestoreSpace = -1
        }
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
    private fun showLayerDialog() {
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

    private fun refreshToolbar() {
        for ((i, btn) in modeButtons.withIndex()) {
            val active = modes.getOrNull(i)?.second == drawingView.mode
            btn.setTextColor(
                if (active) 0xFF31D2A0.toInt() else 0xFFC9D2DC.toInt())
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        parseExecutor.shutdown()
    }
}
