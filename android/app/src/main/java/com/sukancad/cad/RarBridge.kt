package com.sukancad.cad

/** 官方 unrar 静态库的 JNI 桥（RAR4/RAR5 通吃） */
object RarBridge {
    init {
        System.loadLibrary("cadcore")
    }

    /**
     * 解压整个压缩包到目标目录。
     * 返回提取出的 dwg/dxf 数量；负数表示失败：
     *   -N   打开失败（unrar ERAR 错误码）
     *   -122 缺密码 / -124 密码错误
     */
    external fun extractAll(archivePath: String, destDir: String): Int
}
