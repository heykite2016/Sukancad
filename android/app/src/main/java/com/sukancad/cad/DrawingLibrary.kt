package com.sukancad.cad

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import android.util.Log
import java.io.File
import java.util.zip.ZipInputStream

/**
 * 图纸库：应用专属目录 getExternalFilesDir("图纸库")，
 * 导入的 zip/rar 自动解压并删除压缩包，递归登记 dwg/dxf。
 *
 * 类型按文件头嗅探（微信常给 application/octet-stream，扩展名可能丢失）：
 *   PK\x03\x04        → zip
 *   Rar!\x1a\x07\x00  → rar4（junrar）
 *   Rar!\x1a\x07\x01  → rar5（commons-compress）
 * 解压失败时压缩包保留在图纸库，不丢数据。
 */
object DrawingLibrary {

    private const val TAG = "SukancadImport"

    data class CacheInfo(val files: Int, val bytes: Long)

    data class Item(val file: File) {
        val isFolder: Boolean get() = file.isDirectory
        val name: String get() = file.name
        val sizeBytes: Long get() = file.length()
        val lastModified: Long get() = file.lastModified()
        val cacheFile: File get() = File(file.parentFile, file.name + ".skdw")
        val hasCache: Boolean get() = !isFolder && cacheFile.isFile && cacheFile.length() > 0
        /** 文件夹内递归图纸数（列表展示用） */
        val drawingCount: Int get() = if (isFolder) countDrawings(file) else 0
    }

    fun libraryDir(context: Context): File {
        val dir = File(context.getExternalFilesDir(null) ?: context.filesDir, "图纸库")
        if (!dir.exists()) dir.mkdirs()
        return dir
    }

    // ---- 最近打开（对标竞品「最近打开」） ----

    fun recordOpen(context: Context, path: String) {
        val prefs = context.getSharedPreferences("skdw_settings", Context.MODE_PRIVATE)
        val cur = prefs.getString("recent", "")?.split("\n")
            ?.filter { it.isNotEmpty() && it != path } ?: emptyList()
        prefs.edit().putString("recent", (listOf(path) + cur).take(30).joinToString("\n")).apply()
    }

    /** 最近打开的图纸（存在文件才返回，最多 30 条） */
    fun recent(context: Context): List<Item> =
        context.getSharedPreferences("skdw_settings", Context.MODE_PRIVATE)
            .getString("recent", "")?.split("\n")
            ?.filter { it.isNotEmpty() }
            ?.map { File(it) }?.filter { it.isFile }?.map { Item(it) }
            ?: emptyList()

    fun clearRecent(context: Context) {
        context.getSharedPreferences("skdw_settings", Context.MODE_PRIVATE)
            .edit().remove("recent").apply()
    }

    /** 从最近列表移除，不触碰原图纸文件。 */
    fun removeRecent(context: Context, path: String) {
        val prefs = context.getSharedPreferences("skdw_settings", Context.MODE_PRIVATE)
        val next = prefs.getString("recent", "")
            ?.split("\n")
            ?.filter { it.isNotEmpty() && it != path }
            ?: emptyList()
        prefs.edit().putString("recent", next.joinToString("\n")).apply()
    }

    fun cacheInfo(context: Context, root: File = libraryDir(context)): CacheInfo {
        var files = 0
        var bytes = 0L
        val queue = ArrayDeque<File>()
        queue.add(root)
        while (queue.isNotEmpty()) {
            val dir = queue.removeFirst()
            dir.listFiles()?.forEach { file ->
                if (file.isDirectory) {
                    queue.add(file)
                } else if (file.name.contains(".skdw", ignoreCase = true)) {
                    files++
                    bytes += file.length()
                }
            }
        }
        return CacheInfo(files, bytes)
    }

    fun clearParsedCaches(context: Context,
                          root: File = libraryDir(context)): CacheInfo {
        val before = cacheInfo(context, root)
        val queue = ArrayDeque<File>()
        queue.add(root)
        while (queue.isNotEmpty()) {
            val dir = queue.removeFirst()
            dir.listFiles()?.forEach { file ->
                if (file.isDirectory) queue.add(file)
                else if (file.name.contains(".skdw", ignoreCase = true)) file.delete()
            }
        }
        context.cacheDir.listFiles()?.forEach {
            if (it.isFile && it.name.startsWith("import_")) it.delete()
        }
        return before
    }

    /** 浏览某个目录（root = 图纸库根）。文件夹在前，其余按时间倒序。 */
    fun list(context: Context, dir: File = libraryDir(context)): List<Item> {
        val ext = setOf("dwg", "dxf", "zip", "rar")
        val (folders, files) = (dir.listFiles()
            ?.filter { it.isDirectory || (it.isFile && it.extension.lowercase() in ext) }
            ?: emptyList()).partition { it.isDirectory }
        return folders.sortedByDescending { it.lastModified() }.map { Item(it) } +
            files.sortedByDescending { it.lastModified() }.map { Item(it) }
    }

    /** 递归统计目录里的 dwg/dxf 数量 */
    fun countDrawings(dir: File): Int {
        var n = 0
        val queue = ArrayDeque<File>()
        queue.add(dir)
        while (queue.isNotEmpty()) {
            val d = queue.removeFirst()
            d.listFiles()?.forEach { f ->
                if (f.isDirectory) queue.add(f)
                else if (f.extension.lowercase() in setOf("dwg", "dxf")) n++
            }
        }
        return n
    }

    /** 递归搜索目录子树内名称含关键词的图纸（不区分大小写，含子文件夹） */
    fun search(dir: File, query: String): List<Item> {
        val q = query.lowercase()
        if (q.isEmpty()) return emptyList()
        val out = ArrayList<Item>()
        val queue = ArrayDeque<File>()
        queue.add(dir)
        while (queue.isNotEmpty()) {
            val d = queue.removeFirst()
            d.listFiles()?.forEach { f ->
                if (f.isDirectory) queue.add(f)
                else if (f.extension.lowercase() in setOf("dwg", "dxf") &&
                    f.name.lowercase().contains(q)) out.add(Item(f))
            }
        }
        return out.sortedByDescending { it.lastModified }
    }

    fun delete(context: Context, item: Item) {
        if (item.isFolder) {
            item.file.deleteRecursively()
            Log.i(TAG, "deleted folder ${item.file.name}")
            return
        }
        item.file.delete()
        item.cacheFile.delete()
        // 清理残留的空文件夹（如 zip 里只有目录结构没图纸）
        val dir = libraryDir(context)
        dir.listFiles()?.filter { it.isDirectory && it.listFiles()?.isEmpty() == true }
            ?.forEach { it.delete() }
    }

    /** 重命名文件/文件夹（非法字符替换为 _） */
    fun rename(context: Context, item: Item, newName: String): Boolean {
        val safe = newName.replace(Regex("[\\\\/:*?\"<>|]"), "_").trim()
        if (safe.isEmpty()) return false
        val target = File(item.file.parentFile, safe)
        if (target.exists()) return false
        val ok = item.file.renameTo(target)
        if (ok && !item.isFolder) {
            // 图纸的解析缓存跟着改名
            val cache = item.cacheFile
            if (cache.isFile) cache.renameTo(File(target.parentFile, target.name + ".skdw"))
        }
        Log.i(TAG, "renamed ${item.name} -> $safe: $ok")
        return ok
    }

    /** 处理外部打开请求。返回导入说明（供 Toast）。 */
    fun importFromUri(context: Context, uri: Uri): String {
        val resolver = context.contentResolver
        val displayName = queryDisplayName(resolver, uri) ?: "import.bin"
        val safeName = displayName.replace(Regex("[\\\\/:*?\"<>|]"), "_").ifBlank { "import.bin" }
        Log.i(TAG, "import uri=$uri name=$safeName")

        val temp = File(context.cacheDir, "import_$safeName")
        try {
            resolver.openInputStream(uri)?.use { input ->
                temp.outputStream().use { output -> input.copyTo(output) }
            } ?: return importFailed(context, temp, safeName, "无法读取文件内容")
        } catch (e: Exception) {
            return importFailed(context, temp, safeName, "读取失败：${e.message}")
        }

        return try {
            val lower = safeName.lowercase()
            when (sniffType(temp) ?: extType(lower)) {
                "zip" -> extractZip(context, temp, safeName)
                "rar4", "rar5" -> extractRar(context, temp, safeName)
                "dwg" -> {
                    val target = uniqueTarget(context, safeName)
                    if (!temp.renameTo(target)) {
                        temp.copyTo(target, overwrite = true)
                        temp.delete()
                    }
                    Log.i(TAG, "imported drawing ${target.name}")
                    "已导入「${target.name}」"
                }
                else -> importFailed(context, temp, safeName,
                    "不支持的文件类型（仅支持 dwg/dxf/zip/rar）")
            }
        } catch (e: Exception) {
            Log.e(TAG, "import failed", e)
            importFailed(context, temp, safeName, "导入失败：${e.message}")
        }
    }

    /** 失败时把文件保留到图纸库，便于重试/排查 */
    private fun importFailed(context: Context, temp: File, name: String, reason: String): String {
        try {
            if (temp.isFile) {
                val kept = uniqueTarget(context, name)
                temp.copyTo(kept, overwrite = true)
                temp.delete()
                Log.w(TAG, "kept original as ${kept.name}: $reason")
            }
        } catch (_: Exception) {
        }
        return "$reason（原文件已保留在图纸库）"
    }

    private fun extType(lower: String): String? = when {
        lower.endsWith(".zip") -> "zip"
        lower.endsWith(".rar") -> "rar" // 再细分 rar4/rar5
        lower.endsWith(".dwg") || lower.endsWith(".dxf") -> "dwg"
        else -> null
    }

    private fun sniffType(file: File): String? {
        if (!file.isFile || file.length() < 8) return null
        val head = ByteArray(8)
        file.inputStream().use { val n = it.read(head); if (n < 4) return null }
        return when {
            head[0] == 'P'.code.toByte() && head[1] == 'K'.code.toByte() &&
                head[2] == 3.toByte() && head[3] == 4.toByte() -> "zip"
            head[0] == 'R'.code.toByte() && head[1] == 'a'.code.toByte() &&
                head[2] == 'r'.code.toByte() && head[3] == '!'.code.toByte() &&
                head[4] == 0x1a.toByte() && head[5] == 7.toByte() &&
                head[6] == 0.toByte() -> "rar4"
            head[0] == 'R'.code.toByte() && head[1] == 'a'.code.toByte() &&
                head[2] == 'r'.code.toByte() && head[3] == '!'.code.toByte() &&
                head[4] == 0x1a.toByte() && head[5] == 7.toByte() &&
                head[6] == 1.toByte() -> "rar5"
            else -> null
        }
    }

    private fun queryDisplayName(resolver: android.content.ContentResolver, uri: Uri): String? {
        resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0) return cursor.getString(idx)
            }
        }
        return uri.lastPathSegment
    }

    private fun uniqueTarget(context: Context, name: String): File {
        var target = File(libraryDir(context), name)
        if (!target.exists()) return target
        val base = name.substringBeforeLast('.')
        val ext = name.substringAfterLast('.', "bin")
        var i = 2
        while (true) {
            target = File(libraryDir(context), "$base($i).$ext")
            if (!target.exists()) return target
            i++
        }
    }

    private fun extractDir(context: Context, archiveName: String): File {
        val dir = File(libraryDir(context), archiveName.substringBeforeLast('.'))
        dir.mkdirs()
        return dir
    }

    private fun isDrawing(file: File) =
        file.extension.lowercase() in setOf("dwg", "dxf")

    /** zip 解压 → 登记图纸 → 删除压缩包 */
    private fun extractZip(context: Context, archive: File, name: String): String {
        val dir = extractDir(context, name)
        var count = 0
        ZipInputStream(archive.inputStream().buffered()).use { zip ->
            val buffer = ByteArray(1 shl 16)
            while (true) {
                val entry = zip.nextEntry ?: break
                val outFile = safeFile(dir, entry.name) ?: continue
                if (entry.isDirectory) {
                    outFile.mkdirs()
                } else {
                    outFile.parentFile?.mkdirs()
                    outFile.outputStream().use { output ->
                        while (true) {
                            val read = zip.read(buffer)
                            if (read < 0) break
                            output.write(buffer, 0, read)
                        }
                    }
                    if (isDrawing(outFile)) count++
                }
                zip.closeEntry()
            }
        }
        archive.delete()
        Log.i(TAG, "zip extracted drawings=$count")
        return if (count > 0) "已解压 $count 张图纸（压缩包已删除）"
        else "压缩包里没有 dwg/dxf 图纸"
    }

    /** rar（官方 unrar 静态库，RAR4/RAR5 通吃） */
    private fun extractRar(context: Context, archive: File, name: String): String {
        val dir = extractDir(context, name)
        val rc = RarBridge.extractAll(archive.absolutePath, dir.absolutePath)
        return when {
            rc > 0 -> {
                archive.delete()
                Log.i(TAG, "rar extracted drawings=$rc")
                "已解压 $rc 张图纸（压缩包已删除）"
            }
            rc == 0 -> {
                archive.delete()
                "压缩包里没有 dwg/dxf 图纸"
            }
            rc <= -122 -> "压缩包有密码，请先解密后再发送"
            else -> "RAR 解压失败（错误码 ${-rc}），原文件已保留"
        }
    }

    /** 从图纸库内直接重试解压（点击列表里的 zip/rar） */
    fun extractLocal(context: Context, archive: File): String = try {
        when (sniffType(archive) ?: extType(archive.name.lowercase())) {
            "zip" -> extractZip(context, archive, archive.name)
            "rar4", "rar5" -> extractRar(context, archive, archive.name)
            else -> "无法识别的压缩包格式"
        }
    } catch (e: Exception) {
        Log.e(TAG, "extractLocal failed", e)
        "解压失败：${e.message}"
    }

    /** 防 zip-slip：解出路径必须仍在目标目录内 */
    private fun safeFile(dir: File, entryName: String): File? {
        val cleaned = entryName.replace('\\', '/').trimStart('/')
        val candidate = File(dir, cleaned)
        val canonicalDir = dir.canonicalPath + File.separator
        val canonical = try { candidate.canonicalPath } catch (_: Exception) { return null }
        return if (canonical.startsWith(canonicalDir) || canonical == dir.canonicalPath) candidate else null
    }
}
