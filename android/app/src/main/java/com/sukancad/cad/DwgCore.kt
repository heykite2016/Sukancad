package com.sukancad.cad

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** libredwg 解析导出层的 JNI 封装 */
object DwgCore {
    init {
        System.loadLibrary("cadcore")
    }

    /** 解析图纸并写出 SKDW 缓存；返回 0 成功 */
    external fun parseNative(src: String, cache: String): Int

    fun parse(src: File, cache: File): Boolean =
        parseNative(src.absolutePath, cache.absolutePath) == 0
}

/** SKDW 二进制格式的内存模型（v2：新增主体范围 mainBounds） */
class SkdwDocument private constructor(
    val minX: Double, val minY: Double,
    val maxX: Double, val maxY: Double,
    val mainMinX: Double, val mainMinY: Double,
    val mainMaxX: Double, val mainMaxY: Double,
    val layers: List<Layer>,
    val entities: List<Entity>,
) {
    class Layer(val name: String, val argb: Int, var visible: Boolean, val linewt: Int)

    sealed class Entity {
        abstract val layer: Int
        abstract val color: Int      // -1 = 随层
        abstract val bbox: DoubleArray
        class Line(override val layer: Int, override val color: Int,
                   override val bbox: DoubleArray,
                   val x1: Double, val y1: Double, val x2: Double, val y2: Double) : Entity()
        class Circle(override val layer: Int, override val color: Int,
                     override val bbox: DoubleArray,
                     val cx: Double, val cy: Double, val r: Double) : Entity()
        class Arc(override val layer: Int, override val color: Int,
                  override val bbox: DoubleArray,
                  val cx: Double, val cy: Double, val r: Double,
                  val a0: Double, val a1: Double) : Entity()
        class Polyline(override val layer: Int, override val color: Int,
                       override val bbox: DoubleArray,
                       val closed: Boolean,
                       val fill: Boolean,
                       val xs: DoubleArray, val ys: DoubleArray) : Entity()
        class Text(override val layer: Int, override val color: Int,
                   override val bbox: DoubleArray,
                   val x: Double, val y: Double, val height: Double,
                   val rotationDeg: Double, val widthScale: Double,
                   val hAlign: Int, val vAlign: Int,
                   val text: String) : Entity()
        /** 纸面视口：矩形 (cx,cy,w,h) + 模型→纸面仿射 (m11,m12,m21,m22,dx,dy) */
        class Viewport(override val layer: Int, override val color: Int,
                       override val bbox: DoubleArray,
                       val cx: Double, val cy: Double, val w: Double, val h: Double,
                       val m11: Double, val m12: Double,
                       val m21: Double, val m22: Double,
                       val dx: Double, val dy: Double) : Entity()
    }

    companion object {
        const val TYPE_LINE = 1
        const val TYPE_CIRCLE = 2
        const val TYPE_ARC = 3
        const val TYPE_POLYLINE = 4
        const val TYPE_TEXT = 5
        const val TYPE_VIEWPORT = 6
        const val VERSION = 14

        /** CAD 文本清理：剥离 MTEXT 控制码（与 C++ 端 cleanCadText 一致，GBK 文本延迟到此处处理） */
        private fun cleanCadText(text: String): String {
            val out = StringBuilder(text.length)
            var i = 0
            while (i < text.length) {
                val ch = text[i]
                if (ch == '{' || ch == '}') { i++; continue }
                if (ch != '\\') { out.append(ch); i++; continue }
                if (i + 1 >= text.length) break
                val code = text[++i]
                when {
                    code == 'P' -> out.append('\n')
                    code == '~' -> out.append(' ')
                    code == '\\' -> out.append('\\')
                    code == '{' -> out.append('{')
                    code == '}' -> out.append('}')
                    code == 'U' && i + 4 < text.length && text[i + 1] == '+' -> {
                        val hex = text.substring(i + 2, (i + 6).coerceAtMost(text.length))
                        val v = hex.toUIntOrNull(16)?.toInt() ?: 0
                        if (v in 1 until 0x110000) { out.append(v.toChar()); i += 4 }
                    }
                    code in 'A'..'Z' || code in 'a'..'z' -> {
                        while (i < text.length && text[i] != ';') i++
                    }
                }
                i++
            }
            return out.toString()
        }

        /** 解码 CAD 文本：gbk=true 时按 GB18030 解原始字节（含 mojibake 二次修复） */
        private fun decodeCadText(bytes: ByteArray, gbk: Boolean): String {
            if (!gbk) return cleanCadText(String(bytes, Charsets.UTF_8))
            val asGbk = try { String(bytes, charset("GB18030")) }
                        catch (_: Exception) { String(bytes, Charsets.UTF_8) }
            val hasCjk = asGbk.any {
                val c = it.code
                c in 0x3400..0x9FFF || c in 0xF900..0xFAFF
            }
            // GB18030 解不出中文 → 原字节其实是别的编码，退回 UTF-8
            val hasHighByte = bytes.any { it < 0 }
            return cleanCadText(if (hasCjk || !hasHighByte) asGbk
            else String(bytes, Charsets.UTF_8))
        }

        fun read(file: File): SkdwDocument? = try {
            val bytes = file.readBytes()
            parse(ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN))
        } catch (_: Exception) {
            null
        }

        class LayoutTab(val index: Int, val name: String)

        /** 读布局清单 <cache>.layouts：首行 N，每行 idx\tgbk\tname（原始字节可能为 GBK） */
        fun readLayouts(cache: File): List<LayoutTab> = try {
            val bytes = File(cache.absolutePath + ".layouts").readBytes()
            var pos = 0
            fun readLine(): String {
                val start = pos
                while (pos < bytes.size && bytes[pos] != '\n'.code.toByte()) pos++
                val line = ByteArray(pos - start) { bytes[start + it] }
                pos++   // skip \n
                return String(line, Charsets.ISO_8859_1)
            }
            val n = readLine().trim().toInt()
            (0 until n).mapNotNull { k ->
                val parts = readLine().split('\t')
                val idx = parts.getOrNull(0)?.trim()?.toIntOrNull() ?: return@mapNotNull null
                val gbk = parts.getOrNull(1) == "1"
                val raw = parts.drop(2).joinToString("\t")
                    .toByteArray(Charsets.ISO_8859_1)
                LayoutTab(idx, decodeCadText(raw, gbk))
            }
        } catch (_: Exception) {
            emptyList()
        }

        private fun parse(buf: ByteBuffer): SkdwDocument? {
            if (buf.remaining() < 4 + 4 + 64 + 8) return null
            val magic = ByteArray(4)
            buf.get(magic)
            if (magic[0] != 'S'.code.toByte() || magic[1] != 'K'.code.toByte() ||
                magic[2] != 'D'.code.toByte() || magic[3] != 'W'.code.toByte()
            ) return null
            val version = buf.int
            if (version != VERSION) return null   // 旧版本缓存失效，由调用方重新解析
            val minX = buf.double; val minY = buf.double
            val maxX = buf.double; val maxY = buf.double
            val mainMinX = buf.double; val mainMinY = buf.double
            val mainMaxX = buf.double; val mainMaxY = buf.double
            val layerCount = buf.int
            val entityCount = buf.int

            val layers = ArrayList<Layer>(layerCount.coerceAtMost(4096))
            repeat(layerCount) {
                val nameLen = buf.get().toInt() and 0xff
                val nameBytes = ByteArray(nameLen)
                buf.get(nameBytes)
                val nameGbk = buf.get().toInt() != 0   // v3
                val rgb = buf.int
                val visible = buf.get().toInt() != 0
                val linewt = buf.int
                layers.add(Layer(decodeCadText(nameBytes, nameGbk),
                    0xff000000.toInt() or (rgb and 0xffffff), visible, linewt))
            }

            val entities = ArrayList<Entity>(entityCount.coerceAtMost(1 shl 20))
            var guard = 0
            while (buf.remaining() > 1 && guard++ < entityCount) {
                val type = buf.get().toInt() and 0xff
                val layer = buf.int
                val color = buf.int
                val bbox = DoubleArray(4) { buf.double }
                when (type) {
                    TYPE_LINE -> {
                        if (buf.remaining() < 32) break
                        entities.add(Entity.Line(layer, color, bbox,
                            buf.double, buf.double, buf.double, buf.double))
                    }
                    TYPE_CIRCLE -> {
                        if (buf.remaining() < 24) break
                        entities.add(Entity.Circle(layer, color, bbox,
                            buf.double, buf.double, buf.double))
                    }
                    TYPE_ARC -> {
                        if (buf.remaining() < 40) break
                        entities.add(Entity.Arc(layer, color, bbox,
                            buf.double, buf.double, buf.double,
                            buf.double, buf.double))
                    }
                    TYPE_POLYLINE -> {
                        if (buf.remaining() < 6) break
                        val closed = buf.get().toInt() != 0
                        val fill = buf.get().toInt() != 0   // v4
                        val n = buf.int
                        if (n < 0 || buf.remaining() < n * 16) break
                        val xs = DoubleArray(n) { buf.double }
                        val ys = DoubleArray(n) { buf.double }
                        entities.add(Entity.Polyline(layer, color, bbox, closed, fill, xs, ys))
                    }
                    TYPE_TEXT -> {
                        // 最小载荷 = gbk1 + 5*8 + 2*4 + len4 = 53；文字内容长度另校验
                        // （此前写成 57+4，导致文件末尾是 1~3 字短文字时整个文档加载失败）
                        if (buf.remaining() < 53) break
                        val textGbk = buf.get().toInt() != 0   // v3
                        val x = buf.double; val y = buf.double
                        val h = buf.double; val rot = buf.double
                        val ws = buf.double
                        val ha = buf.int; val va = buf.int
                        val len = buf.int
                        if (len < 0 || len > 1 shl 20 || buf.remaining() < len) break
                        val textBytes = ByteArray(len)
                        buf.get(textBytes)
                        entities.add(Entity.Text(layer, color, bbox,
                            x, y, h, rot, ws, ha, va,
                            decodeCadText(textBytes, textGbk)))
                    }
                    TYPE_VIEWPORT -> {
                        if (buf.remaining() < 80) break
                        val cx = buf.double; val cy = buf.double
                        val w = buf.double; val h = buf.double
                        val m11 = buf.double; val m12 = buf.double
                        val m21 = buf.double; val m22 = buf.double
                        val dx = buf.double; val dy = buf.double
                        entities.add(Entity.Viewport(layer, color, bbox,
                            cx, cy, w, h, m11, m12, m21, m22, dx, dy))
                    }
                    else -> return null    // 未知类型：流已错位，整个文件作废
                }
            }
            // 尾部剩余 ≤53 字节（一条最小 TEXT 的一半）视为截断容忍：
            // 已读出的实体仍然有效，不再因此整篇作废触发无意义的重解析
            if (buf.remaining() > 56) return null   // 没读完 = 数据不完整
            return SkdwDocument(minX, minY, maxX, maxY,
                mainMinX, mainMinY, mainMaxX, mainMaxY, layers, entities)
        }
    }
}
