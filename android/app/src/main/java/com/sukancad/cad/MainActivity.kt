package com.sukancad.cad

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import kotlin.concurrent.thread

/** 图纸库列表页（支持文件夹浏览）+ 微信等外部「用其他应用打开」的导入入口 */
class MainActivity : AppCompatActivity() {

    private lateinit var listView: ListView
    private lateinit var emptyView: TextView
    private lateinit var btnBack: TextView
    private lateinit var titleView: TextView
    private lateinit var searchInput: android.widget.EditText
    private var items: List<DrawingLibrary.Item> = emptyList()
    private var query = ""
    /** 0=本地图纸 1=最近打开 */
    private var tab = 0
    private val tabViews = ArrayList<TextView>()

    /** 文件夹导航栈，根目录时为空 */
    private val pathStack = ArrayDeque<java.io.File>()

    private fun currentDir() = pathStack.lastOrNull() ?: DrawingLibrary.libraryDir(this)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 顶栏：返回按钮 + 当前位置标题
        btnBack = TextView(this).apply {
            text = "‹ 返回"
            textSize = 15f
            setTextColor(Color.WHITE)
            setPadding(36, 0, 36, 0)
            gravity = Gravity.CENTER_VERTICAL
            setOnClickListener { navigateUp() }
            visibility = View.GONE
        }
        titleView = TextView(this).apply {
            text = "图纸库"
            textSize = 17f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER_VERTICAL
        }
        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(0xFF1F6FD6.toInt())
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, 30, 0, 30)
            addView(btnBack, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT))
            addView(titleView, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT))
        }

        // 页签：最近打开 / 本地图纸（对标竞品首页结构）
        val tabBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.WHITE)
        }
        for ((label, t) in listOf("本地图纸" to 0, "最近打开" to 1)) {
            val tv = TextView(this).apply {
                text = label
                textSize = 15f
                gravity = Gravity.CENTER
                setPadding(0, 26, 0, 26)
                setOnClickListener { switchTab(t) }
            }
            tabViews.add(tv)
            tabBar.addView(tv, LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        }

        // 搜索框：递归搜索当前目录子树内的图纸
        searchInput = android.widget.EditText(this).apply {
            hint = "搜索图纸名称（含子文件夹）"
            textSize = 14f
            setTextColor(0xFF333B44.toInt())
            setHintTextColor(0xFF9AA3AE.toInt())
            setBackgroundColor(Color.WHITE)
            setPadding(36, 26, 36, 26)
            addTextChangedListener(object : android.text.TextWatcher {
                override fun afterTextChanged(s: android.text.Editable?) {
                    query = s?.toString()?.trim() ?: ""
                    refresh()
                }
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            })
        }

        listView = ListView(this).apply {
            divider = null
            setBackgroundColor(0xFFF3F4F6.toInt())
        }
        emptyView = TextView(this).apply {
            text = "图纸库还是空的\n\n在微信里点开图纸 → 右上角「…」→「用其他应用打开」→ 选择 速看CAD\n支持 dwg / dxf / zip / rar"
            setTextColor(0xFF9AA3AE.toInt())
            textSize = 14f
            gravity = Gravity.CENTER
            setPadding(64, 120, 64, 0)
            setLineSpacing(8f, 1f)
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(header, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(tabBar, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(searchInput, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(listView, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
            addView(emptyView, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(root)
        switchTab(0)

        listView.setOnItemClickListener { _, _, position, _ ->
            val item = items.getOrNull(position) ?: return@setOnItemClickListener
            when {
                item.isFolder -> {
                    pathStack.addLast(item.file)
                    refresh()
                }
                item.file.extension.lowercase() in setOf("zip", "rar") -> {
                    // 失败保留的压缩包：点一下重试解压
                    Toast.makeText(this, "解压中…", Toast.LENGTH_SHORT).show()
                    thread {
                        val message = DrawingLibrary.extractLocal(this, item.file)
                        runOnUiThread {
                            Toast.makeText(this, message, Toast.LENGTH_LONG).show()
                            refresh()
                        }
                    }
                }
                else -> startActivity(Intent(this, DrawingActivity::class.java)
                    .putExtra("path", item.file.absolutePath))
            }
        }
        listView.setOnItemLongClickListener { _, _, position, _ ->
            val item = items.getOrNull(position) ?: return@setOnItemLongClickListener true
            showItemMenu(item)
            true
        }

        handleIntent(intent)
        refresh()
    }

    /** 长按弹出菜单：重命名 / 删除 */
    private fun showItemMenu(item: DrawingLibrary.Item) {
        val options = arrayOf("重命名", "删除")
        AlertDialog.Builder(this)
            .setTitle(item.name)
            .setItems(options) { _, which ->
                when (which) {
                    0 -> showRenameDialog(item)
                    else -> confirmDelete(item)
                }
            }
            .show()
    }

    private fun showRenameDialog(item: DrawingLibrary.Item) {
        val input = android.widget.EditText(this).apply {
            setText(item.name)
            setSelection(item.name.length)
            setPadding(48, 40, 48, 0)
        }
        AlertDialog.Builder(this)
            .setTitle("重命名")
            .setView(input)
            .setPositiveButton("确定") { _, _ ->
                val newName = input.text.toString().trim()
                if (newName.isNotEmpty() && newName != item.name) {
                    val ok = DrawingLibrary.rename(this, item, newName)
                    Toast.makeText(this, if (ok) "已重命名" else "重命名失败（名字已存在？）",
                        Toast.LENGTH_SHORT).show()
                    refresh()
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun confirmDelete(item: DrawingLibrary.Item) {
        if (item.isFolder) {
            AlertDialog.Builder(this)
                .setTitle("删除文件夹")
                .setMessage("确定删除文件夹「${item.name}」？\n其中 ${item.drawingCount} 张图纸与解析缓存将一并删除。")
                .setPositiveButton("删除") { _, _ ->
                    DrawingLibrary.delete(this, item)
                    refresh()
                }
                .setNegativeButton("取消", null)
                .show()
        } else {
            AlertDialog.Builder(this)
                .setTitle("删除图纸")
                .setMessage("确定删除「${item.name}」？\n图纸与解析缓存将一并删除。")
                .setPositiveButton("删除") { _, _ ->
                    DrawingLibrary.delete(this, item)
                    refresh()
                }
                .setNegativeButton("取消", null)
                .show()
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
        refresh()
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (pathStack.isNotEmpty()) {
            navigateUp()
        } else {
            super.onBackPressed()
        }
    }

    private fun navigateUp() {
        if (pathStack.isNotEmpty()) {
            pathStack.removeLast()
            refresh()
        }
    }

    private fun handleIntent(intent: Intent?) {
        val uri = intent?.data ?: return
        if (intent.action != Intent.ACTION_VIEW) return
        // 后台导入：大压缩包拷贝/解压耗时，不能占主线程
        Toast.makeText(this, "正在导入…", Toast.LENGTH_SHORT).show()
        thread {
            val message = DrawingLibrary.importFromUri(this, uri)
            runOnUiThread {
                Toast.makeText(this, message, Toast.LENGTH_LONG).show()
                refresh()
            }
        }
    }

    private fun switchTab(t: Int) {
        tab = t
        for ((i, tv) in tabViews.withIndex()) {
            tv.setTextColor(if (i == tab) 0xFF1F6FD6.toInt() else 0xFF9AA3AE.toInt())
            // 简单下划线指示
            tv.paint.isUnderlineText = i == tab
        }
        refresh()
    }

    private fun refresh() {
        if (tab == 1) {
            // 最近打开
            items = DrawingLibrary.recent(this)
            btnBack.visibility = View.GONE
            titleView.text = "速看CAD"
            emptyView.text = "还没有打开过图纸"
            emptyView.visibility = if (items.isEmpty()) View.VISIBLE else View.GONE
            searchInput.visibility = View.GONE
            bindList()
            return
        }
        searchInput.visibility = View.VISIBLE
        val dir = currentDir()
        items = if (query.isEmpty()) DrawingLibrary.list(this, dir)
                else DrawingLibrary.search(dir, query)
        btnBack.visibility = if (pathStack.isEmpty()) View.GONE else View.VISIBLE
        titleView.text = when {
            query.isNotEmpty() -> "搜索「$query」· ${items.size} 个结果"
            pathStack.isEmpty() -> "图纸库"
            else -> dir.name
        }
        emptyView.text = if (query.isEmpty())
            "图纸库还是空的\n\n在微信里点开图纸 → 右上角「…」→「用其他应用打开」→ 选择 速看CAD\n支持 dwg / dxf / zip / rar"
        else "没有匹配「$query」的图纸"
        emptyView.visibility = if (items.isEmpty()) View.VISIBLE else View.GONE
        bindList()
    }

    private fun bindList() {
        listView.adapter = object : android.widget.BaseAdapter() {
            override fun getCount() = items.size
            override fun getItem(position: Int) = items[position]
            override fun getItemId(position: Int) = position.toLong()
            override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
                val view = convertView ?: layoutInflater.inflate(
                    R.layout.item_drawing, parent, false)
                val item = items[position]
                val name = view.findViewById<TextView>(R.id.itemName)
                val meta = view.findViewById<TextView>(R.id.itemMeta)
                name.text = item.name
                name.setCompoundDrawablesWithIntrinsicBounds(
                    if (item.isFolder) R.drawable.ic_folder else R.drawable.ic_drawing, 0, 0, 0)
                meta.text = when {
                    item.isFolder -> "${item.drawingCount} 张图纸 · ${formatTime(item.lastModified)}"
                    item.file.extension.lowercase() in setOf("zip", "rar") ->
                        "压缩包（点击重试解压） · ${formatSize(item.sizeBytes)} · ${formatTime(item.lastModified)}"
                    else -> "${formatSize(item.sizeBytes)} · ${formatTime(item.lastModified)}" +
                        if (item.hasCache) " · 已缓存" else ""
                }
                return view
            }
        }
    }

    private fun formatSize(bytes: Long): String = when {
        bytes >= 1 shl 20 -> "%.1f MB".format(bytes / 1048576.0)
        bytes >= 1 shl 10 -> "%d KB".format(bytes / 1024)
        else -> "$bytes B"
    }

    private fun formatTime(millis: Long): String =
        java.text.SimpleDateFormat("MM-dd HH:mm", java.util.Locale.CHINA)
            .format(java.util.Date(millis))
}
