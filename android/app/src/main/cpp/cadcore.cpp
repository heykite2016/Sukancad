// 速看CAD 安卓版 —— DWG/DXF 解析导出层（纯 C++，无 Qt 依赖）
//
// 围绕 LibreDWG 把模型空间展平成紧凑二进制（SKDW 格式），
// 桌面版 LibreDwgReader 的关键逻辑在此移植：
//   - INSERT 递归变换（含 MINSERT 阵列）
//   - SPATIAL_FILTER (XCLIP) 裁剪（Liang-Barsky 段裁剪）
//   - LWPOLYLINE/POLYLINE_2D bulge 弧采样
//   - OCS→WCS（挤出向量）变换
//   - DIMENSION 展开匿名标注块
//   - ACI 颜色与暗底显示
//
// 输出格式（小端）：
//   "SKDW" u32 version=1
//   f64 bounds[4]  u32 layerCount  u32 entityCount
//   Layer: u8 nameLen utf8[nameLen] s32 argb u8 visible s32 linewt
//   Ent:   u8 type s32 layer s32 colorOverride(-1=随层) f64 bbox[4] payload...
//     1 LINE     f64 x1 y1 x2 y2
//     2 CIRCLE   f64 cx cy r
//     3 ARC      f64 cx cy r a0 a1
//     4 POLYLINE u8 closed u32 n f64 x[n] f64 y[n]
//     5 TEXT     f64 x y h rot wScale s32 hAlign s32 vAlign u32 len utf8[len]

#include "dwg.h"
#include "dwg_api.h"

#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#define LOG_TAG "cadcore"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- 基础类型

struct P2 {
    double x = 0.0;
    double y = 0.0;
};

struct Affine {
    double m11 = 1.0, m12 = 0.0, m21 = 0.0, m22 = 1.0, dx = 0.0, dy = 0.0;

    P2 map(const P2 &p) const {
        return {m11 * p.x + m21 * p.y + dx, m12 * p.x + m22 * p.y + dy};
    }
    Affine then(const Affine &outer) const {
        Affine r;
        r.m11 = outer.m11 * m11 + outer.m21 * m12;
        r.m21 = outer.m11 * m21 + outer.m21 * m22;
        r.dx = outer.m11 * dx + outer.m21 * dy + outer.dx;
        r.m12 = outer.m12 * m11 + outer.m22 * m12;
        r.m22 = outer.m12 * m21 + outer.m22 * m22;
        r.dy = outer.m12 * dx + outer.m22 * dy + outer.dy;
        return r;
    }
};

// ---------------------------------------------------------------- 颜色

uint32_t hsvToRgb(int hueDeg, double sat, double val) {
    const double h = (hueDeg % 360) / 60.0;
    const double c = val * sat;
    const double x = c * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;
    switch (int(h) % 6) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    const double m = val - c;
    auto to8 = [](double v) { return uint32_t(std::round(v * 255.0)) & 0xff; };
    return (to8(r + m) << 16) | (to8(g + m) << 8) | to8(b + m);
}

uint32_t aciColor(int index) {
    // 标准 AutoCAD ACI 256 色（与图纸「对象颜色/图层颜色」一致）
    // 1-9：基本色；10-249：24 个色相 × 10 档明度/饱和；250-255：灰阶
    static const uint32_t basic[9] = {
        0xff0000, 0xffff00, 0x00ff00, 0x00ffff, 0x0000ff,
        0xff00ff, 0xffffff, 0x808080, 0xc0c0c0,
    };
    static const uint32_t grays[6] = {
        0x333333, 0x505050, 0x696969, 0x828282, 0xbebebe, 0xffffff,
    };
    if (index >= 1 && index <= 9) return basic[index - 1];
    if (index >= 250 && index <= 255) return grays[index - 250];
    if (index >= 10 && index <= 249) {
        const int n = index - 10;
        const double hue = double(n / 10) * 15.0;
        const int light = n % 10;
        // 每 10 档：偶数档全饱和，奇数档 1/3 饱和；明度 1.0 / 0.741 / 0.506 / 0.408 / 0.31
        static const double value[10] = {
            1.000, 1.000, 0.741, 0.741, 0.506,
            0.506, 0.408, 0.408, 0.310, 0.310,
        };
        const double sat = (light % 2 == 0) ? 1.0 : 0.33;
        return hsvToRgb(hue, sat, value[light]);
    }
    return 0xe6ecf5;
}

// ---------------------------------------------------------------- OCS

P2 ocsPoint(const P2 &p, const BITCODE_BE &normal) {
    const double nx = normal.x, ny = normal.y, nz = normal.z;
    if (std::abs(nx) < 1e-12 && std::abs(ny) < 1e-12 && nz >= 0.0) return p;
    P2 xAxis;
    if (std::abs(nx) < 0.015625 && std::abs(ny) < 0.015625) xAxis = {nz, 0.0};
    else xAxis = {-ny, nx};
    const double len = std::hypot(xAxis.x, xAxis.y);
    if (len < 1e-12) return p;
    xAxis.x /= len; xAxis.y /= len;
    const P2 yAxis{-xAxis.y * nz, nz * xAxis.x};
    return {xAxis.x * p.x + yAxis.x * p.y, xAxis.y * p.x + yAxis.y * p.y};
}

P2 ocsPoint(const P2 &p, double elevation, const BITCODE_BE &normal) {
    const double nx = normal.x, ny = normal.y, nz = normal.z;
    if (std::abs(nx) < 1e-12 && std::abs(ny) < 1e-12 && nz >= 0.0) return p;
    P2 xAxis;
    if (std::abs(nx) < 0.015625 && std::abs(ny) < 0.015625) xAxis = {nz, 0.0};
    else xAxis = {-ny, nx};
    const double len = std::hypot(xAxis.x, xAxis.y);
    if (len < 1e-12) return p;
    xAxis.x /= len; xAxis.y /= len;
    const P2 yAxis{-xAxis.y * nz, nz * xAxis.x};
    return {xAxis.x * p.x + yAxis.x * p.y + nx * elevation,
            xAxis.y * p.x + yAxis.y * p.y + ny * elevation};
}

Affine ocsAffine(const BITCODE_BE &normal) {
    const double nx = normal.x, ny = normal.y, nz = normal.z;
    if (std::abs(nx) < 1e-12 && std::abs(ny) < 1e-12 && nz >= 0.0) return {};
    P2 xAxis;
    if (std::abs(nx) < 0.015625 && std::abs(ny) < 0.015625) xAxis = {nz, 0.0};
    else xAxis = {-ny, nx};
    const double len = std::hypot(xAxis.x, xAxis.y);
    if (len < 1e-12) return {};
    xAxis.x /= len; xAxis.y /= len;
    const P2 yAxis{-xAxis.y * nz, nz * xAxis.x};
    return {xAxis.x, xAxis.y, yAxis.x, yAxis.y, 0.0, 0.0};
}

// ---------------------------------------------------------------- 几何采样

std::vector<P2> sampleArc(const P2 &center, double radius,
                          double start, double end, bool full = false) {
    std::vector<P2> result;
    if (radius <= 0.0 || !std::isfinite(radius)) return result;
    if (full) { start = 0.0; end = 2.0 * kPi; }
    else { while (end <= start) end += 2.0 * kPi; }
    const int count = std::min(512, std::max(full ? 36 : 8,
        int(std::ceil(std::abs(end - start) / (kPi / 48.0)))));
    const int steps = full ? count : count + 1;
    result.reserve(size_t(steps));
    for (int i = 0; i < steps; ++i) {
        const double angle = start + (end - start) * double(i) / double(count);
        result.push_back({center.x + std::cos(angle) * radius,
                          center.y + std::sin(angle) * radius});
    }
    return result;
}

// de Boor 求值：B 样条在参数 u 处的点（SPLINE 实体采样用）
P2 splinePoint(const std::vector<P2> &cp, const std::vector<double> &kn,
               int p, double u) {
    const int n = int(cp.size()) - 1;
    int k = n;
    if (u < kn[size_t(n + 1)]) {
        for (int i = p; i <= n; ++i) {
            if (u >= kn[size_t(i)] && u < kn[size_t(i + 1)]) { k = i; break; }
        }
    }
    if (k < p) k = p;
    std::vector<P2> d(size_t(p) + 1);
    for (int j = 0; j <= p; ++j) d[size_t(j)] = cp[size_t(k - p + j)];
    for (int r = 1; r <= p; ++r) {
        for (int j = p; j >= r; --j) {
            const int i = k - p + j;
            const double den = kn[size_t(i + p - r + 1)] - kn[size_t(i)];
            const double a = den > 1e-12 ? (u - kn[size_t(i)]) / den : 0.0;
            d[size_t(j)] = {
                (1.0 - a) * d[size_t(j - 1)].x + a * d[size_t(j)].x,
                (1.0 - a) * d[size_t(j - 1)].y + a * d[size_t(j)].y};
        }
    }
    return d[size_t(p)];
}

void appendBulge(std::vector<P2> &result, const P2 &a, const P2 &b,
                 double bulge) {
    // 弧的圆心在 chord 外侧 chord/2/tan(theta/2) 处：
    // bulge 极小（但非 0）时 tan→0，圆心会被推到千万量级，
    // 采样点飞到图外，把整图包围盒撑爆（随后适配缩放趋近 0，
    // 屏幕上所有线条/文字都小于 LOD 阈值被剔除 = "图纸缺线条"）。
    // 因此：|bulge| 很小按直线处理；半径/圆心异常也退回直线。
    if (!std::isfinite(bulge) || std::abs(bulge) < 1e-4) {
        result.push_back(b);
        return;
    }
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double chord = std::hypot(dx, dy);
    if (chord < 1e-12) { result.push_back(b); return; }
    const double theta = 4.0 * std::atan(bulge);
    const double halfTan = std::tan(theta * 0.5);
    if (!std::isfinite(halfTan) || std::abs(halfTan) < 1e-6) {
        result.push_back(b);
        return;
    }
    const P2 normal{-dy / chord, dx / chord};
    const double offset = chord * 0.5 / halfTan;
    const P2 center{(a.x + b.x) * 0.5 + normal.x * offset,
                    (a.y + b.y) * 0.5 + normal.y * offset};
    const double radius = std::hypot(a.x - center.x, a.y - center.y);
    // 半径不应远超弦长（半圆时 radius = chord/2，最大合理值约 chord）
    if (!std::isfinite(radius) || radius > chord * 1e4) {
        result.push_back(b);
        return;
    }
    const double start = std::atan2(a.y - center.y, a.x - center.x);
    const int count = std::min(256, std::max(4,
        int(std::ceil(std::abs(theta) / (kPi / 48.0)))));
    for (int i = 1; i <= count; ++i) {
        const double angle = start + theta * double(i) / double(count);
        result.push_back({center.x + std::cos(angle) * radius,
                          center.y + std::sin(angle) * radius});
    }
}

// ---------------------------------------------------------------- 文本处理

// NDK 无 iconv，也没有 GB18030 码表；libredwg 已按图纸码表转出 UTF-8，
// 兜底仅剔除无法解码的 U+FFFD，避免渲染出乱码方块
// NDK 无 iconv，也没有 GB18030 码表；文本编码判定在此，重解码交给 Java 层
// （Java/Android 的 ICU 内置 GB18030）。
// 返回 true = 需要按 GB18030 解码原始字节：
//   a) 字节流不是合法 UTF-8 → 老图纸原始 GBK 字节
//   b) 合法 UTF-8 但出现 Latin 补充区字符（U+0080..U+04FF）→ mojibake，
//      把它的 UTF-8 字节再按 GB18030 解一次（与桌面版 repairMojibake 一致）
static bool needsGbkDecode(const std::string &s) {
    bool validUtf8 = true;
    bool suspicious = false;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { ++i; continue; }
        size_t len = 0;
        if ((c & 0xE0) == 0xC0 && c >= 0xC2) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) len = 4;
        if (len == 0 || i + len > s.size()) { validUtf8 = false; break; }
        unsigned cp = c & (len == 2 ? 0x1Fu : (len == 3 ? 0x0Fu : 0x07u));
        bool cont = true;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { cont = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!cont || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            validUtf8 = false; break;
        }
        if (cp >= 0x80 && cp < 0x0500) suspicious = true;
        i += len;
    }
    if (!validUtf8) return true;
    return suspicious;
}

// 前置声明（prepareText 在 cleanCadText 之前定义）
std::string cleanCadText(const std::string &text);
std::string cleanCadTextGbk(const std::string &text);

// 统一文本出口：判定编码 + 清理控制码。
// GBK 文本必须在 GBK 字节层面清理：GBK 双字节的尾字节可能是 0x5C（\），
// 若按 UTF-8 逻辑逐字节扫会被误当转义符、把整串中文咬坏。
static std::string prepareText(std::string raw, bool &gbk) {
    gbk = needsGbkDecode(raw);
    return gbk ? cleanCadTextGbk(raw) : cleanCadText(raw);
}

std::string utf8Field(void *value, const char *type, const char *field) {
    char *text = nullptr;
    int isNew = 0;
    if (!value
        || !dwg_dynapi_entity_utf8text(value, type, field, &text, &isNew, nullptr)
        || !text) {
        return {};
    }
    std::string result(text);
    if (isNew) std::free(text);
    return result;
}

// 清理 MTEXT/TEXT 控制码（移植 cleanCadText）
std::string cleanCadText(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '{' || ch == '}') continue;
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (i + 1 >= text.size()) break;
        const char code = text[++i];
        if (code == 'P') out.push_back('\n');
        else if (code == '~') out.push_back(' ');
        else if (code == '\\') out.push_back('\\');
        else if (code == '{') out.push_back('{');
        else if (code == '}') out.push_back('}');
        else if (code == 'U' && i + 4 < text.size()
                 && text[i + 1] == '+' ) {
            // \U+XXXX → Unicode
            char hex[5] = {0, 0, 0, 0, 0};
            for (int k = 0; k < 4 && i + 2 + k < text.size(); ++k)
                hex[k] = text[i + 2 + k];
            const unsigned long value = std::strtoul(hex, nullptr, 16);
            i += 4;
            if (value > 0 && value < 0x110000) {
                if (value < 0x80) out.push_back(char(value));
                else if (value < 0x800) {
                    out.push_back(char(0xC0 | (value >> 6)));
                    out.push_back(char(0x80 | (value & 0x3F)));
                } else {
                    out.push_back(char(0xE0 | (value >> 12)));
                    out.push_back(char(0x80 | ((value >> 6) & 0x3F)));
                    out.push_back(char(0x80 | (value & 0x3F)));
                }
            }
        } else if ((code >= 'A' && code <= 'Z')
                   || (code >= 'a' && code <= 'z')) {
            // 字体/颜色等代码 \f...\; \C1; \H..; —— 跳到分号
            while (i < text.size() && text[i] != ';') ++i;
        }
        // 其它（如 \- 等）丢弃
    }
    return out;
}

// GBK 文本控制码清理：双字节字符整体拷贝（尾字节可能是 0x5C），
// ASCII 段按 CAD 控制码规则清理。此前 GBK 文本直接原样返回，
// \fSimSun|b0;、\H2.5x;、\P 等全部随文字画出 = 标注/房间名乱码、缺行。
std::string cleanCadTextGbk(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char ch = (unsigned char)text[i];
        if (ch >= 0x80) {
            out.push_back(char(ch));
            if (i + 1 < text.size()) out.push_back(text[++i]);
            continue;
        }
        if (ch == '{' || ch == '}') continue;
        if (ch != '\\') { out.push_back(char(ch)); continue; }
        if (i + 1 >= text.size()) break;
        const char code = text[++i];
        if (code == 'P') out.push_back('\n');
        else if (code == '~') out.push_back(' ');
        else if (code == '\\') out.push_back('\\');
        else if (code == '{' || code == '}') out.push_back(code);
        else if (code == 'U' && i + 4 < text.size() && text[i + 1] == '+') {
            char hex[5] = {0, 0, 0, 0, 0};
            for (int k = 0; k < 4 && i + 2 + k < text.size(); ++k)
                hex[k] = text[i + 2 + k];
            const unsigned long value = std::strtoul(hex, nullptr, 16);
            i += 4;
            // GBK 串里混 UTF-8 会破坏解码，非 ASCII 的 \U+ 用 '?' 占位
            if (value > 0x1F && value < 0x7F) out.push_back(char(value));
            else out.push_back('?');
        } else if ((code >= 'A' && code <= 'Z')
                   || (code >= 'a' && code <= 'z')) {
            // 字体/颜色等代码 \f...\; \C1; \H..; —— 跳到分号
            while (i < text.size() && text[i] != ';') ++i;
        }
    }
    return out;
}

// ---------------------------------------------------------------- 导出实体

enum EntType : uint8_t {
    kLine = 1, kCircle = 2, kArc = 3, kPolyline = 4, kText = 5,
    kViewport = 6,
};

struct Ent {
    uint8_t type = kLine;
    int32_t layer = 0;
    int32_t color = -1;      // -1 = 随层
    int32_t linewt = -3;     // BYLAYER
    bool closed = false;
    bool fill = false;       // v4: 实心填充（SOLID/HATCH solid）
    bool gbk = false;        // 文本需按 GB18030 解码（Java 端处理）
    bool xref = false;       // 来自外部参照合并的图元（不参与跑飞剔除）
    double bbox[4] = {0, 0, 0, 0};
    std::vector<double> num; // payload 数字
    std::string text;        // 文本内容（type=5）

    void updateBounds() {
        bbox[0] = bbox[1] = 1e308;
        bbox[2] = bbox[3] = -1e308;
        const auto take = [this](double x, double y) {
            bbox[0] = std::min(bbox[0], x); bbox[1] = std::min(bbox[1], y);
            bbox[2] = std::max(bbox[2], x); bbox[3] = std::max(bbox[3], y);
        };
        switch (type) {
        case kLine:
            take(num[0], num[1]); take(num[2], num[3]); break;
        case kCircle:
            take(num[0] - num[2], num[1] - num[2]);
            take(num[0] + num[2], num[1] + num[2]); break;
        case kArc: {
            const double cx = num[0], cy = num[1], r = num[2];
            double a0 = num[3], a1 = num[4];
            while (a1 <= a0) a1 += 2 * kPi;
            // 包围盒必须按实际弧段计算：整圆 bbox 会把 40 万半径的
            // 微小圆弧角(fillet)算成横跨全图的矩形，撑爆主范围
            take(cx + std::cos(a0) * r, cy + std::sin(a0) * r);
            take(cx + std::cos(a1) * r, cy + std::sin(a1) * r);
            for (int q = 0; q < 4; ++q) {
                const double bound = q * kPi / 2.0;
                double t = bound;
                while (t < a0) t += 2 * kPi;
                if (t <= a1) {
                    take(cx + std::cos(t) * r, cy + std::sin(t) * r);
                }
            }
            break;
        }
        case kPolyline: {
            const size_t n = num.size() / 2;
            for (size_t i = 0; i < n; ++i)
                take(num[i], num[n + i]);
            break;
        }
        case kText: {
            const double h = num[2];
            const double rot = num[3] * kPi / 180.0;
            const double width = std::max(h * 0.6 * num[4], h * 0.6)
                * double(text.size());
            take(num[0], num[1]);
            take(num[0] + std::cos(rot) * width,
                 num[1] + std::sin(rot) * width);
            break;
        }
        }
    }
};

struct LayerRec {
    std::string name;
    bool gbk = false;        // 名称需按 GB18030 解码
    uint32_t color = 0xffffff;
    bool visible = true;
    int32_t linewt = -3;
};

// ---------------------------------------------------------------- 导出器

struct Bounds {
    double minX = 1e308, minY = 1e308, maxX = -1e308, maxY = -1e308;
    bool valid() const { return maxX > minX && maxY > minY; }
    void take(double x, double y) {
        minX = std::min(minX, x); minY = std::min(minY, y);
        maxX = std::max(maxX, x); maxY = std::max(maxY, y);
    }
    void unite(const Bounds &o) {
        minX = std::min(minX, o.minX); minY = std::min(minY, o.minY);
        maxX = std::max(maxX, o.maxX); maxY = std::max(maxY, o.maxY);
    }
};

bool clipSegmentToRect(P2 &a, P2 &b, double minX, double minY,
                       double maxX, double maxY) {
    double t0 = 0.0, t1 = 1.0;
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x - minX, maxX - a.x, a.y - minY, maxY - a.y};
    for (int k = 0; k < 4; ++k) {
        if (std::abs(p[k]) < 1e-12) {
            if (q[k] < 0.0) return false;
            continue;
        }
        const double r = q[k] / p[k];
        if (p[k] < 0.0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
    }
    const P2 s = a;
    a = {s.x + t0 * (b.x - s.x), s.y + t0 * (b.y - s.y)};
    b = {s.x + t1 * (b.x - s.x), s.y + t1 * (b.y - s.y)};
    return true;
}

class Exporter {
public:
    Dwg_Data dwg{};
    std::vector<Ent> entities;
    std::vector<LayerRec> layers;
    std::vector<std::pair<uint64_t, size_t>> layerByHandle;
    // 多段线顶点索引：owner handle → 顶点对象（避免每条 3D 多段线都全表扫）
    std::unordered_map<uint64_t, std::vector<Dwg_Object *>> vertexIndex;
    // 图案填充已生成的线条总数（全局预算）
    size_t patternLineCount = 0;
    bool mirrText = false;
    Bounds bounds;
    // 布局（图纸空间）导出结果
    struct LayoutEntry {
        std::string name;
        bool gbk = false;
        size_t entityCount = 0;
    };
    std::vector<LayoutEntry> paperLayouts;

    bool skipCull = false; // 宿主诊断用：跳过跑飞剔除
    bool isXrefSub = false;    // 本导出器是在解析外部参照子图（不再递归合并）
    std::string srcPath;       // 当前 DWG 源路径（外部参照按同目录文件名匹配）
    // 外部参照：块句柄 → 该块对应的外部文件实体（块空间坐标）。
    // 展开到图纸时用 INSERT 的变换绘制，图框才会落在正确位置。
    struct XrefBlock {
        uint64_t handle = 0;
        std::string name;   // 块名（已从 UTF-16LE 原始字节转 UTF-8）
        std::string file;
        std::vector<Ent> ents;
    };
    std::vector<XrefBlock> xrefBlocks;
    // BYBLOCK 颜色（ACI 0）的图元跟随宿主 INSERT 的颜色；-1 表示不在块内。
    int32_t curBlockColor = -1;

    // ---- 外部参照自动合并 --------------------------------------------

    static std::vector<uint8_t> readFileBytes(const std::string &path) {
        std::vector<uint8_t> data;
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) return data;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0 && size < (1L << 30)) {
            data.resize(size_t(size));
            if (std::fread(data.data(), 1, data.size(), f) != data.size())
                data.clear();
        }
        std::fclose(f);
        return data;
    }

    // UTF-8 -> UTF-16LE（DWG 里的字符串是 UTF-16LE，不能按字节补 0 糊弄，
    // 中文每个字符是 3 字节 UTF-8 / 2 字节 UTF-16，混着来必然搜不到）
    static void utf8ToUtf16le(const std::string &s,
                              std::vector<uint8_t> &out) {
        size_t i = 0;
        while (i < s.size()) {
            const unsigned char c = (unsigned char)s[i];
            uint32_t cp = 0;
            size_t n = 0;
            if (c < 0x80) { cp = c; n = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
            else { ++i; continue; }
            if (i + n > s.size()) break;
            bool ok = true;
            for (size_t k = 1; k < n; ++k) {
                const unsigned char cc = (unsigned char)s[i + k];
                if ((cc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (!ok) { ++i; continue; }
            i += n;
            if (cp < 0x10000) {
                out.push_back(uint8_t(cp & 0xFF));
                out.push_back(uint8_t((cp >> 8) & 0xFF));
            } else {
                cp -= 0x10000;
                const uint32_t hi = 0xD800 + (cp >> 10);
                const uint32_t lo = 0xDC00 + (cp & 0x3FF);
                out.push_back(uint8_t(hi & 0xFF));
                out.push_back(uint8_t((hi >> 8) & 0xFF));
                out.push_back(uint8_t(lo & 0xFF));
                out.push_back(uint8_t((lo >> 8) & 0xFF));
            }
        }
    }

    static std::vector<std::string> listSiblingDwgs(const std::string &src) {
        std::vector<std::string> out;
        const size_t slash = src.find_last_of('/');
        const std::string dir =
            slash == std::string::npos ? std::string(".") : src.substr(0, slash);
        DIR *d = opendir(dir.c_str());
        if (!d) return out;
        struct dirent *de = nullptr;
        while ((de = readdir(d)) != nullptr) {
            const std::string name = de->d_name;
            if (name.size() < 5) continue;
            std::string low = name;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) {
                               return char(std::tolower(c));
                           });
            if (low.compare(low.size() - 4, 4, ".dwg") == 0)
                out.push_back(dir + "/" + name);
        }
        closedir(d);
        return out;
    }

    int32_t layerIndexByName(const std::string &name) {
        for (size_t i = 0; i < layers.size(); ++i)
            if (layers[i].name == name) return int32_t(i);
        LayerRec rec;
        rec.name = name;
        rec.color = 0xe6ecf5;
        layers.push_back(std::move(rec));
        return int32_t(layers.size() - 1);
    }

    // 图纸引用的外部文件清单存在 DWG 的「文件依赖表」里，形如
    //   u16 count, 然后一串 UTF-16LE 键名（Acad:XRef / Acad:Text / ...）
    //   以及对应的路径（C:\...\图框.dwg）
    // 直接全文搜文件名会误命中标题栏里的文字说明，所以先定位 Acad:XRef
    // 标记，只在其后 64KB 窗口里找。
    static const size_t kXrefWindow = 64 * 1024;

    static void collectXrefWindows(const std::vector<uint8_t> &hay,
                                   std::vector<std::pair<size_t, size_t>> &out) {
        std::vector<uint8_t> marker;
        utf8ToUtf16le("Acad:XRef", marker);
        if (marker.empty()) return;
        size_t pos = 0;
        while (pos + marker.size() <= hay.size()) {
            auto it = std::search(hay.begin() + long(pos), hay.end(),
                                  marker.begin(), marker.end());
            if (it == hay.end()) break;
            const size_t at = size_t(it - hay.begin());
            out.emplace_back(at, std::min(hay.size(), at + kXrefWindow));
            pos = at + 1;
        }
    }

    // 在 [from,to) 内找「完整文件名」，且要求命中前后是路径边界。
    // 不能退回后缀匹配：A1 图框和 A2 图框文件名只差一个字符，
    // 后缀会把 A2 误判成被引用。
    static bool searchNameInRange(const std::vector<uint8_t> &hay, size_t from,
                                  size_t to, const std::string &name) {
        if (to > hay.size()) to = hay.size();
        if (from + 4 > to) return false;
        std::vector<uint8_t> pat;
        utf8ToUtf16le(name, pat);
        if (pat.size() < 8) return false;
        const auto b = hay.begin() + long(from);
        const auto e = hay.begin() + long(to);
        auto it = b;
        while (it != e) {
            it = std::search(it, e, pat.begin(), pat.end());
            if (it == e) return false;
            const size_t at = size_t(it - hay.begin());
            const size_t after = at + pat.size();
            // 命中之后必须是字符串结尾（NUL）或不可打印字符
            bool okEnd = true;
            if (after + 1 < hay.size()) {
                const unsigned v = unsigned(hay[after]) |
                                   (unsigned(hay[after + 1]) << 8);
                if (v >= 0x20) okEnd = false;
            }
            // 命中之前必须是路径分隔符（\ / :）或字符串开头
            bool okStart = at == 0;
            if (!okStart && at >= 2) {
                const unsigned v = unsigned(hay[at - 2]) |
                                   (unsigned(hay[at - 1]) << 8);
                if (v == '\\' || v == '/' || v == ':' || v < 0x20) okStart = true;
            }
            if (okEnd && okStart) return true;
            it += 2;
        }
        return false;
    }

    static bool referencedByBytes(const std::vector<uint8_t> &hay,
                                  const std::string &name,
                                  const std::vector<std::pair<size_t, size_t>>
                                      &windows) {
        if (windows.empty())   // 没有文件依赖表（老版本 DWG）：全文搜
            return searchNameInRange(hay, 0, hay.size(), name);
        for (const auto &w : windows)
            if (searchNameInRange(hay, w.first, w.second, name)) return true;
        return false;
    }

    // R2018 的块名/层名是 UTF-16LE 原始字节，strlen 会在首个 ASCII 字符的
    // 高位 0x00 处截断，这里按 u16 结束符取长度再转 UTF-8。
    static std::string tvRawToUtf8(const char *raw) {
        if (!raw) return {};
        size_t len = 0;
        for (; len < 1024; len += 2) {
            if (raw[len] == 0 && raw[len + 1] == 0) break;
        }
        bool u16 = len >= 2;
        for (size_t i = 1; i < len; i += 2)
            if (raw[i] != 0) { u16 = false; break; }
        if (!u16) return std::string(raw);   // 普通单字节/GBK
        std::string out;
        out.reserve(len / 2);
        for (size_t i = 0; i + 1 < len; i += 2) {
            unsigned cp = unsigned(uint8_t(raw[i]))
                        | (unsigned(uint8_t(raw[i + 1])) << 8);
            if (cp == 0) break;
            if (cp < 0x80) out.push_back(char(cp));
            else if (cp < 0x800) {
                out.push_back(char(0xC0 | (cp >> 6)));
                out.push_back(char(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(char(0xE0 | (cp >> 12)));
                out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(char(0x80 | (cp & 0x3F)));
            }
        }
        return out;
    }

    // 收集图纸里的外部参照块（BLOCK_HEADER.blkisxref=1），
    // 这些块本身没有几何，内容全在外部文件里。
    void collectXrefBlocks() {
        xrefBlocks.clear();
        for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
            Dwg_Object *o = &dwg.object[i];
            if (o->fixedtype != DWG_TYPE_BLOCK_HEADER) continue;
            auto *b = dwg_object_to_BLOCK_HEADER(o);
            if (!b || !b->blkisxref) continue;
            XrefBlock rec;
            rec.handle = o->handle.value;
            rec.name = tvRawToUtf8(b->name);
            xrefBlocks.push_back(std::move(rec));
        }
        if (!xrefBlocks.empty())
            ALOGI("xref blocks: %zu (name0=%s)", xrefBlocks.size(),
                  xrefBlocks.front().name.c_str());
    }

    void mergeExternalRefs() {
        const std::vector<uint8_t> self = readFileBytes(srcPath);
        if (self.empty()) { ALOGI("xref scan: cannot read %s", srcPath.c_str()); return; }
        std::vector<std::pair<size_t, size_t>> windows;
        collectXrefWindows(self, windows);
        const auto sibs = listSiblingDwgs(srcPath);
        ALOGI("xref scan: %zu sibling dwgs, bytes=%zu, xref windows=%zu",
              sibs.size(), self.size(), windows.size());
        const size_t slash = srcPath.find_last_of('/');
        const std::string selfName =
            slash == std::string::npos ? srcPath : srcPath.substr(slash + 1);
        std::vector<std::pair<std::string, std::vector<Ent>>> loaded;
        for (const std::string &sib : sibs) {
            const size_t s2 = sib.find_last_of('/');
            const std::string name = sib.substr(s2 + 1);
            if (name == selfName) continue;
            const bool hit = referencedByBytes(self, name, windows);
            ALOGI("xref candidate %s -> %s", name.c_str(), hit ? "HIT" : "no");
            if (!hit) continue;   // 未被本图引用
            Exporter sub;
            sub.isXrefSub = true;
            if (!sub.run(sib.c_str()) || sub.entities.empty()) continue;
            ALOGI("load xref %s: %zu entities", name.c_str(),
                  sub.entities.size());
            std::vector<Ent> ents;
            ents.reserve(sub.entities.size());
            for (Ent &e : sub.entities) {
                if (e.layer >= 0 && e.layer < int(sub.layers.size()))
                    e.layer = layerIndexByName(
                        sub.layers[size_t(e.layer)].name);
                e.xref = true;
                ents.push_back(std::move(e));
            }
            loaded.emplace_back(name, std::move(ents));
            if (loaded.size() >= 4) break;   // 外部参照数量上限
        }
        if (loaded.empty()) return;

        // 文件 → xref 块：一个文件时所有 xref 块都用它；多个文件时按块名匹配
        // （R2018 的块名是 UTF-16LE 原始字节，strlen 会在首个 ASCII 字符处截断，
        //  所以按 0x0000 结束符取长度）。
        auto norm = [](std::string s) {
            const size_t dot = s.find_last_of('.');
            if (dot != std::string::npos && s.compare(dot, 4, ".dwg") == 0)
                s = s.substr(0, dot);
            if (s.size() >= 6) {   // 去掉 "_x1_" 之类的参照前缀
                size_t i = 0;
                while (i < s.size() && s[i] == '_') ++i;
                if (i > 0) s = s.substr(i);
            }
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        };
        for (XrefBlock &blk : xrefBlocks) {
            size_t pick = 0;
            if (loaded.size() > 1) {
                const std::string want = norm(blk.name);
                for (size_t i = 0; i < loaded.size(); ++i) {
                    if (norm(loaded[i].first) == want) { pick = i; break; }
                }
            }
            blk.file = loaded[pick].first;
            blk.ents = loaded[pick].second;
        }
        int bound = 0;
        for (XrefBlock &blk : xrefBlocks)
            if (!blk.ents.empty()) ++bound;
        ALOGI("xref bound blocks: %d / %zu (files=%zu)", bound,
              xrefBlocks.size(), loaded.size());
    }

    int xrefBlockIndex(uint64_t handle) const {
        for (size_t i = 0; i < xrefBlocks.size(); ++i)
            if (xrefBlocks[i].handle == handle) return int(i);
        return -1;
    }

    // 把一个已解析实体套上块变换（外部参照内容按 INSERT 的位置/缩放/旋转落位）
    void transformEnt(Ent &e, const Affine &t) {
        const double det = t.m11 * t.m22 - t.m12 * t.m21;
        const double s = std::sqrt(std::abs(det));
        const double rot = std::atan2(t.m12, t.m11);
        switch (e.type) {
        case kLine: {
            const P2 a = t.map({e.num[0], e.num[1]});
            const P2 b = t.map({e.num[2], e.num[3]});
            e.num = {a.x, a.y, b.x, b.y};
            break;
        }
        case kCircle: {
            const P2 c = t.map({e.num[0], e.num[1]});
            e.num = {c.x, c.y, e.num[2] * s};
            break;
        }
        case kArc: {
            const P2 c = t.map({e.num[0], e.num[1]});
            e.num = {c.x, c.y, e.num[2] * s,
                     e.num[3] + rot * 180.0 / kPi,
                     e.num[4] + rot * 180.0 / kPi};
            break;
        }
        case kPolyline: {
            const size_t n = e.num.size() / 2;
            for (size_t i = 0; i < n; ++i) {
                const P2 p = t.map({e.num[i], e.num[n + i]});
                e.num[i] = p.x;
                e.num[n + i] = p.y;
            }
            break;
        }
        case kText:
            applyTextTransform(e, t);
            break;
        default:
            return;
        }
        e.updateBounds();
    }

    bool run(const char *path) {
        srcPath = path;
        const char *extension = std::strrchr(path, '.');
        const bool isDxf = extension && strcasecmp(extension, ".dxf") == 0;
        const int rc = isDxf
            ? dxf_read_file(path, &dwg)
            : dwg_read_file(path, &dwg);
        // 非致命警告（VALUEOUTOFBOUNDS 等）与桌面版一致地容忍
        if (rc >= DWG_ERR_CRITICAL) {
            ALOGE("%s(%s) = %d", isDxf ? "dxf_read_file" : "dwg_read_file",
                  path, rc);
            return false;
        }
        if (rc != 0) {
            ALOGI("%s(%s) = %d (non-critical, continue)",
                  isDxf ? "dxf_read_file" : "dwg_read_file", path, rc);
        }
        mirrText = dwg.header_vars.MIRRTEXT != 0;

        // 图层表
        for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
            Dwg_Object *object = &dwg.object[i];
            if (object->fixedtype != DWG_TYPE_LAYER) continue;
            auto *source = dwg_object_to_LAYER(object);
            if (!source) continue;
            LayerRec rec;
            rec.name = prepareText(utf8Field(source, "LAYER", "name"), rec.gbk);
            if (rec.name.empty()) rec.name = "0";
            rec.color = entityColor(source->color, 0xe6ecf5);
            rec.visible = !source->off && !source->frozen;
            rec.linewt = int32_t(source->linewt);
            layerByHandle.emplace_back(object->handle.value,
                                       layers.size());
            layers.push_back(std::move(rec));
        }

        // XCLIP 索引
        indexSpatialFilters();
        buildVertexIndex();

        Dwg_Object *model = dwg_model_space_object(&dwg);
        if (!model) {
            ALOGE("no model space");
            return false;
        }
        visitBlockHeader(model, Affine{}, 0, nullptr);

        // 外部参照（XREF）自动合并：图纸引用的外部 DWG（如图框）若与
        // 本文件同目录且文件名出现在本文件字节流中，则一并展平进输出。
        // libredwg 未暴露 xref 路径，这里直接在文件字节里找同目录 dwg 文件名
        // （DWG 文本为 UTF-16LE 编码）。
        if (!isXrefSub) {
            collectXrefBlocks();
            mergeExternalRefs();
        }

        bounds = computeBounds();
        if (!skipCull) cullStray();
        ALOGI("entities=%zu layers=%zu bounds=(%.1f,%.1f)-(%.1f,%.1f)",
              entities.size(), layers.size(),
              bounds.minX, bounds.minY, bounds.maxX, bounds.maxY);
        return true;
    }

    Bounds computeBounds() const {
        Bounds b;
        for (const Ent &e : entities) {
            if (e.bbox[0] <= e.bbox[2] && e.bbox[1] <= e.bbox[3]) {
                b.take(e.bbox[0], e.bbox[1]);
                b.take(e.bbox[2], e.bbox[3]);
            }
        }
        return b;
    }

    // -------------------------------------------------- 布局（图纸空间）

    // 导出所有布局为 <cache>.l<k>.skdw，清单写 <cache>.layouts。
    // 视口以记录形式输出，模型内容由 Kotlin 端叠加渲染。
    void exportLayouts(const char *cachePath) {
        paperLayouts.clear();
        std::vector<Ent> modelEntities = std::move(entities);
        const Bounds modelBounds = bounds;
        entities.clear();
        bounds = Bounds{};

        int k = 0;
        for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
            Dwg_Object *object = &dwg.object[i];
            if (object->fixedtype != DWG_TYPE_BLOCK_HEADER) continue;
            auto *block = dwg_object_to_BLOCK_HEADER(object);
            if (!block || !block->layout) continue;
            int err = 0;
            Dwg_Object *lo = dwg_ref_get_object(block->layout, &err);
            if (!lo || err || lo->fixedtype != DWG_TYPE_LAYOUT
                || !lo->tio.object) continue;
            auto *lay = lo->tio.object->tio.LAYOUT;
            if (!lay) continue;
            bool gbk = false;
            std::string name =
                prepareText(utf8Field(lay, "LAYOUT", "layout_name"), gbk);
            if (name.empty() || name == "Model") continue;

            entities.clear();
            bounds = Bounds{};
            patternLineCount = 0;
            // 纸面块里的 xref INSERT 会由 visitBlockHeader 按自身变换展开，
            // 不再额外叠一份（否则图框文字会画两遍）。
            visitBlockHeader(object, Affine{}, 0, nullptr);
            bounds = computeBounds();
            cullStray();
            ALOGI("layout[%d] \"%s\" entities=%zu", k, name.c_str(),
                  entities.size());
            if (entities.empty()) continue;

            std::string path = std::string(cachePath) + ".l"
                + std::to_string(k) + ".skdw";
            if (!serialize(path.c_str())) continue;
            LayoutEntry entry;
            entry.name = std::move(name);
            entry.gbk = gbk;
            entry.entityCount = entities.size();
            paperLayouts.push_back(std::move(entry));
            ++k;
        }
        ALOGI("layouts exported: %zu", paperLayouts.size());

        // 清单：<cache>.layouts，首行 N，每行 k\tgbk\tname
        std::string manifest = std::to_string(paperLayouts.size()) + "\n";
        for (size_t idx = 0; idx < paperLayouts.size(); ++idx) {
            manifest += std::to_string(idx);
            manifest += paperLayouts[idx].gbk ? "\t1\t" : "\t0\t";
            manifest += paperLayouts[idx].name;
            manifest += "\n";
        }
        std::string mpath = std::string(cachePath) + ".layouts";
        FILE *mf = std::fopen(mpath.c_str(), "wb");
        if (mf) {
            std::fwrite(manifest.data(), 1, manifest.size(), mf);
            std::fclose(mf);
        }

        entities = std::move(modelEntities);
        bounds = modelBounds;
    }

    // -------------------------------------------------- 颜色

    uint32_t entityColor(const Dwg_Color &source, uint32_t fallback) {
        const int method = int(source.rgb >> 24) & 0xff;
        const int payload = int(source.rgb & 0x00ffffff);
        if (method == DWG_COLOR_METHOD_TRUECOLOR && payload <= 0xff)
            return aciColor(payload);
        if (method == DWG_COLOR_METHOD_ACI
            && source.index >= 0 && source.index < 256)
            return aciColor(source.index);
        if (payload > 0xff) return payload & 0xffffff;
        if (source.index >= 0 && source.index < 256)
            return aciColor(source.index);
        return fallback;
    }

    // -------------------------------------------------- 图层

    int layerIndexOf(Dwg_Object_Entity *common) const {
        if (!common || !common->layer) return -1;
        for (const auto &entry : layerByHandle)
            if (entry.first == common->layer->absolute_ref)
                return int(entry.second);
        return -1;
    }

    uint32_t layerColorOf(int32_t idx) const {
        if (idx >= 0 && idx < int(layers.size())) return layers[size_t(idx)].color;
        return 0xe6ecf5;
    }

    // 图层名是否为 "0"（DWG 里 R2018 的层名可能是 UTF-16LE 原始字节，
    // 形如 '0' 0x00，两种都要认）
    bool isZeroLayerName(int32_t idx) const {
        if (idx < 0 || idx >= int(layers.size())) return true;
        const std::string &n = layers[size_t(idx)].name;
        if (n.empty()) return true;
        if (n[0] != '0') return false;
        return n.size() == 1 || n[1] == '\0';
    }

    // -------------------------------------------------- XCLIP

    std::vector<std::pair<uint64_t, std::vector<P2>>> clips;

    // 顶点对象按属主句柄建索引（POLYLINE_3D / POLYLINE_MESH 顶点数以千计，
    // 逐条全表扫描会退化成 O(n²)）
    void buildVertexIndex() {
        vertexIndex.clear();
        for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
            Dwg_Object *o = &dwg.object[i];
            const int t = int(o->fixedtype);
            if (t != DWG_TYPE_VERTEX_2D && t != DWG_TYPE_VERTEX_3D
                && t != DWG_TYPE_VERTEX_MESH) continue;
            if (!o->tio.entity || !o->tio.entity->ownerhandle) continue;
            vertexIndex[o->tio.entity->ownerhandle->absolute_ref]
                .push_back(o);
        }
    }

    void indexSpatialFilters() {
        clips.clear();
        for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
            Dwg_Object *object = &dwg.object[i];
            if (object->fixedtype != DWG_TYPE_SPATIAL_FILTER
                || !object->tio.object) continue;
            auto *filter = object->tio.object->tio.SPATIAL_FILTER;
            if (!filter || filter->num_clip_verts == 0
                || !filter->clip_verts) continue;
            Dwg_Object *owner = nullptr;
            if (object->tio.object->ownerhandle) {
                int err = 0;
                owner = dwg_ref_get_object(
                    object->tio.object->ownerhandle, &err);
            }
            int guard = 0;
            while (owner && owner->fixedtype == DWG_TYPE_DICTIONARY
                   && guard++ < 8) {
                if (!owner->tio.object
                    || !owner->tio.object->ownerhandle) { owner = nullptr; break; }
                int err = 0;
                owner = dwg_ref_get_object(
                    owner->tio.object->ownerhandle, &err);
                if (err) owner = nullptr;
            }
            if (!owner || owner->supertype != DWG_SUPERTYPE_ENTITY) continue;
            std::vector<P2> polygon;
            polygon.reserve(filter->num_clip_verts + 1);
            for (BITCODE_BL k = 0; k < filter->num_clip_verts; ++k) {
                const BITCODE_2RD &v = filter->clip_verts[k];
                P2 local{v.x, v.y};
                if (filter->inverse_transform) {
                    const double *t = filter->inverse_transform;
                    local = {t[0] * v.x + t[1] * v.y + t[3],
                             t[4] * v.x + t[5] * v.y + t[7]};
                }
                polygon.push_back(local);
            }
            if (filter->num_clip_verts == 2) {
                const P2 a = polygon.front(), b = polygon.back();
                polygon.clear();
                polygon.push_back(a);
                polygon.push_back({b.x, a.y});
                polygon.push_back(b);
                polygon.push_back({a.x, b.y});
                polygon.push_back(a);
            } else if (!(polygon.front().x == polygon.back().x
                         && polygon.front().y == polygon.back().y)) {
                polygon.push_back(polygon.front());
            }
            clips.emplace_back(owner->handle.value, std::move(polygon));
        }
    }

    std::vector<P2> clipFor(uint64_t handle) const {
        for (const auto &c : clips)
            if (c.first == handle) return c.second;
        return {};
    }

    // -------------------------------------------------- 遍历

    void visitBlockHeader(Dwg_Object *header, const Affine &outer,
                          int depth, const int32_t *inheritLayer) {
        if (!header || depth > 32) return;
        auto *block = dwg_object_to_BLOCK_HEADER(header);
        if (!block) return;
        if (block->blkisxref) {
            // 外部参照块：块内没有几何，内容来自同目录的外部文件。
            // 按本期 INSERT 的变换把外部图元搬进来（图框/标题栏才对得上位置）。
            const int xi = xrefBlockIndex(header->handle.value);
            if (xi >= 0 && !xrefBlocks[size_t(xi)].ents.empty()) {
                const std::vector<Ent> &src = xrefBlocks[size_t(xi)].ents;
                for (const Ent &s : src) {
                    Ent e = s;
                    if (e.type == kViewport) continue;
                    transformEnt(e, outer);
                    if (inheritLayer && isZeroLayerName(e.layer))
                        e.layer = *inheritLayer;
                    entities.push_back(std::move(e));
                }
            }
            return;
        }
        if (block->entities && block->num_owned > 0) {
            for (BITCODE_BL i = 0; i < block->num_owned; ++i) {
                if (!block->entities[i]) continue;
                int err = 0;
                Dwg_Object *entity =
                    dwg_ref_get_object(block->entities[i], &err);
                if (entity && !err) visitEntity(entity, outer, depth,
                                                inheritLayer);
            }
            return;
        }
        // 兜底：按 ownerhandle 扫描
        for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
            Dwg_Object *object = &dwg.object[i];
            if (object->supertype != DWG_SUPERTYPE_ENTITY) continue;
            Dwg_Object_Entity *common = object->tio.entity;
            if (!common || !common->ownerhandle) continue;
            if (common->ownerhandle->absolute_ref != header->handle.value)
                continue;
            if (object->fixedtype == DWG_TYPE_SEQEND) continue;
            visitEntity(object, outer, depth, inheritLayer);
        }
    }

    void visitEntity(Dwg_Object *object, const Affine &transform,
                     int depth, const int32_t *inheritLayer) {
        if (!object || object->supertype != DWG_SUPERTYPE_ENTITY) return;
        Dwg_Object_Entity *common = object->tio.entity;
        if (!common) return;
        // invisible=1 的实体（含 INSERT 引用）在 AutoCAD 里不绘制。
        // 此前忽略了该标志 —— 灯具符号块里被"关掉"的装饰线/填充全部画了出来，
        // 手机上就是"射灯变成一团乱麻""图纸上多出一堆不该有的线"。
        if (common->invisible) return;

        // 逐实体公共属性：图层 + 颜色 + 线宽
        int32_t layerIdx = layerIndexOf(common);
        // CAD 规则：块内位于 "0" 层的实体跟随 INSERT 所在图层显示（颜色也随之）。
        // 此前 inheritLayer 一路传下来却没用，块内 0 层实体全留在 0 层，
        // 于是"部分图元图层错、颜色也跟着错"。
        if (inheritLayer && isZeroLayerName(layerIdx)) layerIdx = *inheritLayer;
        uint32_t layerColor = 0xe6ecf5;
        if (layerIdx >= 0 && layerIdx < int(layers.size()))
            layerColor = layers[size_t(layerIdx)].color;
        uint32_t color = layerColor;
        int32_t colorOverride = -1;
        {
            int err = 0;
            const Dwg_Color *ec =
                dwg_ent_get_color(common, &err);
            // ACI 0 = BYBLOCK：颜色随宿主块引用（INSERT）的颜色。
            // 256 = BYLAYER：随图层，不算显式覆盖。
            if (ec && ec->index == 0 && curBlockColor >= 0) {
                colorOverride = curBlockColor;
            } else if (ec && ec->index != 256 && ec->index != 0) {
                color = entityColor(*ec, layerColor);
                colorOverride = int32_t(color);
            }
        }
        const int32_t linewt = int32_t(common->linewt);
        (void)color;

        switch (object->fixedtype) {
        case DWG_TYPE_LINE: {
            auto *line = dwg_object_to_LINE(object);
            if (!line) return;
            Ent e;
            e.type = kLine;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            const P2 a = ocsPoint({line->start.x, line->start.y},
                                  line->extrusion);
            const P2 b = ocsPoint({line->end.x, line->end.y},
                                  line->extrusion);
            const P2 wa = transform.map(a), wb = transform.map(b);
            e.num = {wa.x, wa.y, wb.x, wb.y};
            e.updateBounds();
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_CIRCLE: {
            auto *circle = dwg_object_to_CIRCLE(object);
            if (!circle) return;
            Ent e;
            e.type = kCircle;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            const P2 center = transform.map(
                ocsPoint({circle->center.x, circle->center.y},
                         circle->extrusion));
            const double sx = std::hypot(transform.m11, transform.m12);
            const double sy = std::hypot(transform.m21, transform.m22);
            // 非均匀缩放 → 椭圆，采样为折线（否则画错）
            if (std::abs(sx - sy) > 1e-9 * std::max(sx, sy)) {
                const auto pts = sampleArc(
                    ocsPoint({circle->center.x, circle->center.y},
                             circle->extrusion),
                    circle->radius, 0.0, 2.0 * kPi);
                e.type = kPolyline;
                e.closed = true; // emitPolylinePts 不写回 closed，需手动
                emitPolylinePts(e, pts, true, transform);
                e.updateBounds();
                entities.push_back(std::move(e));
                break;
            }
            double r = circle->radius * (sx + sy) * 0.5;
            if (r <= 0.0) return;
            e.num = {center.x, center.y, r};
            e.updateBounds();
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_ARC: {
            auto *arc = dwg_object_to_ARC(object);
            if (!arc) return;
            Ent e;
            e.type = kArc;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            const P2 center = transform.map(
                ocsPoint({arc->center.x, arc->center.y}, arc->extrusion));
            const double sx = std::hypot(transform.m11, transform.m12);
            const double sy = std::hypot(transform.m21, transform.m22);
            // 非均匀缩放 → 椭圆弧，采样为折线
            if (std::abs(sx - sy) > 1e-9 * std::max(sx, sy)) {
                const auto pts = sampleArc(
                    ocsPoint({arc->center.x, arc->center.y},
                             arc->extrusion),
                    arc->radius, arc->start_angle, arc->end_angle);
                e.type = kPolyline; // 折线布局，必须改类型（否则序列化错位）
                emitPolylinePts(e, pts, false, transform);
                e.updateBounds();
                entities.push_back(std::move(e));
                break;
            }
            double r = arc->radius * (sx + sy) * 0.5;
            // 旋转块中弧的角度必须跟随变换（筒灯等旋转插入的块此前全画错）：
            // det>0（旋转+均匀缩放）：角度整体加 rot；det<0（镜像）：区间翻转
            const double det = transform.m11 * transform.m22
                - transform.m21 * transform.m12;
            const double rot = std::atan2(transform.m12, transform.m11);
            double a0 = arc->start_angle;
            double a1 = arc->end_angle;
            if (det < 0) {
                a0 = rot - arc->end_angle;
                a1 = rot - arc->start_angle;
            } else {
                a0 = arc->start_angle + rot;
                a1 = arc->end_angle + rot;
            }
            while (a1 <= a0) a1 += 2.0 * kPi;   // 渲染端假定 a1 > a0
            if (r <= 0.0) return;
            e.num = {center.x, center.y, r, a0, a1};
            e.updateBounds();
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_LWPOLYLINE: {
            auto *polyline = dwg_object_to_LWPOLYLINE(object);
            if (!polyline || !polyline->points
                || polyline->num_points == 0) return;
            const bool closed = (polyline->flag & 512) != 0;
            std::vector<P2> pts;
            P2 previous;
            bool havePrevious = false;
            for (BITCODE_BL i = 0; i < polyline->num_points; ++i) {
                const P2 current{polyline->points[i].x,
                                 polyline->points[i].y};
                if (!havePrevious) {
                    pts.push_back(ocsPoint(current, polyline->elevation,
                                           polyline->extrusion));
                } else {
                    const double bulge =
                        i < polyline->num_bulges && polyline->bulges
                        ? polyline->bulges[i - 1] : 0.0;
                    std::vector<P2> local{previous};
                    appendBulge(local, previous, current, bulge);
                    for (size_t j = 1; j < local.size(); ++j) {
                        pts.push_back(ocsPoint(local[j],
                                               polyline->elevation,
                                               polyline->extrusion));
                    }
                }
                previous = current;
                havePrevious = true;
            }
            if (closed && havePrevious && polyline->bulges
                && polyline->num_bulges >= polyline->num_points) {
                const double closingBulge =
                    polyline->bulges[polyline->num_points - 1];
                if (std::abs(closingBulge) > 1e-10) {
                    std::vector<P2> local{previous};
                    appendBulge(local, previous,
                                {polyline->points[0].x,
                                 polyline->points[0].y},
                                closingBulge);
                    for (size_t j = 1; j < local.size(); ++j)
                        pts.push_back(ocsPoint(local[j],
                                               polyline->elevation,
                                               polyline->extrusion));
                }
            }
            if (pts.size() < 2) return;
            Ent e;
            e.type = kPolyline; e.closed = closed;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            emitPolylinePts(e, pts, closed, transform);
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_POLYLINE_2D: {
            auto *polyline = dwg_object_to_POLYLINE_2D(object);
            if (!polyline) return;
            const bool closed =
                (polyline->flag & 1) != 0 || (polyline->flag & 512) != 0;
            // 收集属主为本实体的 VERTEX_2D
            std::vector<const Dwg_Object *> vertices;
            for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
                const Dwg_Object *v = &dwg.object[i];
                if (v->fixedtype != DWG_TYPE_VERTEX_2D) continue;
                if (!v->tio.entity || !v->tio.entity->ownerhandle) continue;
                if (v->tio.entity->ownerhandle->absolute_ref
                    != object->handle.value) continue;
                vertices.push_back(v);
            }
            if (vertices.empty()) return;
            std::vector<P2> pts;
            P2 previous;
            bool havePrevious = false;
            for (size_t i = 0; i <= vertices.size(); ++i) {
                if (i == vertices.size() && !closed) break;
                const Dwg_Object *vobj =
                    vertices[i % vertices.size()];
                auto *vertex = dwg_object_to_VERTEX_2D(
                    const_cast<Dwg_Object *>(vobj));
                if (!vertex) continue;
                const P2 current{vertex->point.x, vertex->point.y};
                if (!havePrevious) {
                    pts.push_back(ocsPoint(current, polyline->elevation,
                                           polyline->extrusion));
                } else {
                    const double bulge = vertex->bulge;
                    std::vector<P2> local{previous};
                    appendBulge(local, previous, current, bulge);
                    for (size_t j = 1; j < local.size(); ++j)
                        pts.push_back(ocsPoint(local[j],
                                               polyline->elevation,
                                               polyline->extrusion));
                }
                previous = current;
                havePrevious = true;
            }
            if (pts.size() < 2) return;
            Ent e;
            e.type = kPolyline; e.closed = closed;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            emitPolylinePts(e, pts, closed, transform);
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_ELLIPSE: {
            auto *ellipse = dwg_object_to_ELLIPSE(object);
            if (!ellipse) return;
            const P2 center{ellipse->center.x, ellipse->center.y};
            const P2 major{ellipse->sm_axis.x, ellipse->sm_axis.y};
            const double nz = ellipse->extrusion.z;
            const P2 minor{(-nz * major.y) * ellipse->axis_ratio,
                (nz * major.x) * ellipse->axis_ratio};
            double start = ellipse->start_angle;
            double sweep = ellipse->end_angle - start;
            const bool full = std::isfinite(sweep)
                && std::abs(sweep) >= 2.0 * kPi - 1e-9;
            if (full) { start = 0.0; sweep = 2.0 * kPi; }
            else { while (sweep <= 0.0) sweep += 2.0 * kPi; }
            const int count = std::min(512, std::max(8,
                int(std::ceil(std::abs(sweep) / (kPi / 48.0)))));
            const int steps = full ? count : count + 1;
            std::vector<P2> pts;
            pts.reserve(size_t(steps));
            for (int i = 0; i < steps; ++i) {
                const double t = start + sweep * double(i) / double(count);
                pts.push_back({center.x + major.x * std::cos(t)
                                     + minor.x * std::sin(t),
                               center.y + major.y * std::cos(t)
                                     + minor.y * std::sin(t)});
            }
            Ent e;
            e.type = kPolyline; e.closed = full;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            emitPolylinePts(e, pts, full, transform);
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_SPLINE: {
            // 样条曲线（此前完全没画 = 图纸缺曲线）。用 de Boor 求值采样，
            // 无控制点时退化为拟合点折线。
            auto *spline = dwg_object_to_SPLINE(object);
            if (!spline) return;
            std::vector<P2> pts;
            if (spline->ctrl_pts && spline->num_ctrl_pts >= 2) {
                const int n = int(spline->num_ctrl_pts) - 1;
                int p = int(spline->degree);
                if (p < 1) p = 3;
                if (p > n) p = n;
                std::vector<P2> cp;
                cp.reserve(size_t(n) + 1);
                for (BITCODE_BL i = 0; i < spline->num_ctrl_pts; ++i)
                    cp.push_back({spline->ctrl_pts[i].x,
                                  spline->ctrl_pts[i].y});
                std::vector<double> kn;
                if (spline->knots
                    && int(spline->num_knots) >= n + p + 2) {
                    kn.assign(spline->knots,
                              spline->knots + spline->num_knots);
                } else {
                    kn.resize(size_t(n + p + 2));
                    for (int i = 0; i < n + p + 2; ++i)
                        kn[size_t(i)] = i <= p ? 0.0
                            : (i <= n ? double(i - p) : double(n - p + 1));
                }
                for (size_t i = 1; i < kn.size(); ++i)
                    if (kn[i] < kn[i - 1]) kn[i] = kn[i - 1];
                const double u0 = kn[size_t(p)];
                const double u1 = kn[size_t(n + 1)];
                if (u1 > u0) {
                    const int steps = std::min(800,
                        std::max(24, (n + 1) * 10));
                    pts.reserve(size_t(steps) + 1);
                    for (int s = 0; s <= steps; ++s)
                        pts.push_back(splinePoint(cp, kn, p,
                            u0 + (u1 - u0) * double(s) / double(steps)));
                }
            }
            if (pts.size() < 2 && spline->fit_pts) {
                for (BITCODE_BS i = 0; i < spline->num_fit_pts; ++i)
                    pts.push_back({spline->fit_pts[i].x,
                                   spline->fit_pts[i].y});
            }
            if (pts.size() < 2) return;
            Ent e;
            e.type = kPolyline;
            e.closed = (spline->flag & 1) != 0 || spline->closed_b != 0;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            emitPolylinePts(e, pts, e.closed, transform);
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_POLYLINE_3D: {
            // 3D 多段线（平面图里常作为屋面/轮廓线，此前完全没画）
            auto *poly = object->tio.entity
                ? object->tio.entity->tio.POLYLINE_3D : nullptr;
            if (!poly) return;
            auto found = vertexIndex.find(object->handle.value);
            if (found == vertexIndex.end() || found->second.size() < 2) return;
            std::vector<P2> pts;
            pts.reserve(found->second.size());
            for (Dwg_Object *vobj : found->second) {
                if (vobj->fixedtype != DWG_TYPE_VERTEX_3D
                    && vobj->fixedtype != DWG_TYPE_VERTEX_MESH) continue;
                auto *v = dwg_object_to_VERTEX_3D(vobj);
                if (!v) continue;
                pts.push_back({v->point.x, v->point.y});
            }
            if (pts.size() < 2) return;
            Ent e;
            e.type = kPolyline;
            e.closed = (poly->flag & 1) != 0;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            emitPolylinePts(e, pts, e.closed, transform);
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_TEXT: {
            auto *text = dwg_object_to_TEXT(object);
            if (!text) return;
            emitText(object, text, "TEXT", "text_value", transform,
                     layerIdx, colorOverride, linewt);
            break;
        }
        case DWG_TYPE_ATTRIB: {
            auto *attrib = dwg_object_to_ATTRIB(object);
            if (!attrib) return;
            Ent e;
            e.type = kText;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            e.text = prepareText(utf8Field(attrib, "ATTRIB", "text_value"), e.gbk);
            if (e.text.empty()) return;
            const bool useAlignment =
                attrib->horiz_alignment == 4 || attrib->vert_alignment > 0;
            const BITCODE_2DPOINT &pos =
                useAlignment ? attrib->alignment_pt : attrib->ins_pt;
            e.num = {pos.x, pos.y, std::max(0.1, attrib->height),
                     attrib->rotation * 180.0 / kPi,
                     std::max(0.05, attrib->width_factor),
                     double(attrib->horiz_alignment),
                     double(attrib->vert_alignment)};
            applyTextTransform(e, transform);
            e.updateBounds();
#ifdef CADCORE_TRACE_TEXT
            ALOGI("TXT %llu ATTRIB L%d %.2f %.2f h=%.2f a=%.1f |%s|",
                  (unsigned long long)object->handle.value, e.layer,
                  e.num[0], e.num[1], e.num[2], e.num[3], e.text.c_str());
#endif
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_MTEXT: {
            auto *text = dwg_object_to_MTEXT(object);
            if (!text) return;
            Ent e;
            e.type = kText;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            e.text = prepareText(utf8Field(text, "MTEXT", "text"), e.gbk);
            if (e.text.empty()) return;
            const P2 pos{text->ins_pt.x, text->ins_pt.y};
            const double rot = std::atan2(text->x_axis_dir.y,
                                          text->x_axis_dir.x);
            e.num = {pos.x, pos.y, std::max(0.1, text->text_height),
                     rot * 180.0 / kPi, 1.0, 0.0, 0.0};
            applyTextTransform(e, transform);
            e.updateBounds();
#ifdef CADCORE_TRACE_TEXT
            ALOGI("TXT %llu MTEXT L%d %.2f %.2f h=%.2f a=%.1f |%s|",
                  (unsigned long long)object->handle.value, e.layer,
                  e.num[0], e.num[1], e.num[2], e.num[3], e.text.c_str());
#endif
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_LEADER: {
            auto *leader = dwg_object_to_LEADER(object);
            if (!leader || !leader->points || leader->num_points < 2)
                return;
            std::vector<P2> pts;
            for (BITCODE_BL i = 0; i < leader->num_points; ++i)
                pts.push_back({leader->points[i].x, leader->points[i].y});
            Ent e;
            e.type = kPolyline;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            emitPolylinePts(e, pts, false, transform);
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_INSERT:
            visitInsert(object, dwg_object_to_INSERT(object), transform,
                        depth, layerIdx);
            break;
        case DWG_TYPE_MINSERT: {
            auto *insert = dwg_object_to_MINSERT(object);
            if (!insert) return;
            Dwg_Entity_INSERT compatible{};
            compatible.ins_pt = insert->ins_pt;
            compatible.scale = insert->scale;
            compatible.rotation = insert->rotation;
            compatible.extrusion = insert->extrusion;
            compatible.block_header = insert->block_header;
            compatible.num_cols = insert->num_cols;
            compatible.num_rows = insert->num_rows;
            compatible.col_spacing = insert->col_spacing;
            compatible.row_spacing = insert->row_spacing;
            visitInsert(object, &compatible, transform, depth, layerIdx);
            break;
        }
        case DWG_TYPE_SOLID:
        case DWG_TYPE_TRACE: {
            // 实心四边形：DXF 顶点顺序 1,2,4,3（3、4 互换了视觉顺序）
            auto *solid = object->fixedtype == DWG_TYPE_SOLID
                ? dwg_object_to_SOLID(object)
                : reinterpret_cast<Dwg_Entity_SOLID *>(
                      dwg_object_to_TRACE(object));
            if (!solid) return;
            const P2 raw[4] = {{solid->corner1.x, solid->corner1.y},
                               {solid->corner2.x, solid->corner2.y},
                               {solid->corner4.x, solid->corner4.y},
                               {solid->corner3.x, solid->corner3.y}};
            Ent e;
            e.type = kPolyline; e.closed = true; e.fill = true;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            std::vector<P2> pts;
            for (const P2 &p : raw)
                pts.push_back(ocsPoint(p, solid->elevation, solid->extrusion));
            emitPolylinePts(e, pts, true, transform);
            e.updateBounds();
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE__3DFACE: {
            auto *face = dwg_object_to__3DFACE(object);
            if (!face) return;
            const P2 raw[4] = {{face->corner1.x, face->corner1.y},
                               {face->corner2.x, face->corner2.y},
                               {face->corner3.x, face->corner3.y},
                               {face->corner4.x, face->corner4.y}};
            Ent e;
            e.type = kPolyline; e.closed = true;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            std::vector<P2> pts;
            for (const P2 &p : raw) pts.push_back(p);
            emitPolylinePts(e, pts, true, transform);
            e.updateBounds();
            entities.push_back(std::move(e));
            break;
        }
        case DWG_TYPE_HATCH: {
            // 填充：solid 实心 → 填充多边形；图案填充 → 边界 + 图案线
            auto *hatch = dwg_object_to_HATCH(object);
            if (!hatch || !hatch->paths) return;
            const bool solidFill = hatch->is_solid_fill != 0;
            const double elev = hatch->elevation;
            std::vector<std::vector<P2>> allLoops;
            for (BITCODE_BL p = 0; p < hatch->num_paths; ++p) {
                const Dwg_HATCH_Path &path = hatch->paths[p];
                std::vector<P2> pts;
                if (path.flag & 2) {          // 多段线路径
                    if (!path.polyline_paths) continue;
                    pts.reserve(path.num_segs_or_paths);
                    for (BITCODE_BL i = 0; i < path.num_segs_or_paths; ++i) {
                        const auto &pp = path.polyline_paths[i];
                        pts.push_back(ocsPoint({pp.point.x, pp.point.y},
                                               elev, hatch->extrusion));
                    }
                } else {                      // 边缘路径（线/弧/椭圆/样条）
                    if (!path.segs) continue;
                    for (BITCODE_BL i = 0; i < path.num_segs_or_paths; ++i) {
                        const Dwg_HATCH_PathSeg &seg = path.segs[i];
                        switch (seg.curve_type) {
                        case 1:  // 直线
                            pts.push_back(ocsPoint({seg.first_endpoint.x,
                                seg.first_endpoint.y}, elev, hatch->extrusion));
                            break;
                        case 2: { // 圆弧 → 采样
                            const auto arc = sampleArc(
                                {seg.center.x, seg.center.y}, seg.radius,
                                seg.start_angle, seg.end_angle);
                            for (const P2 &q : arc)
                                pts.push_back(ocsPoint(q, elev,
                                                       hatch->extrusion));
                            break;
                        }
                        case 3: { // 椭圆弧 → 采样
                            const P2 major{seg.endpoint.x, seg.endpoint.y};
                            const P2 minor{-major.y * seg.minor_major_ratio,
                                           major.x * seg.minor_major_ratio};
                            const int n = std::min(64, std::max(8,
                                int(std::ceil(std::abs(seg.end_angle
                                    - seg.start_angle) / (kPi / 32.0)))));
                            for (int k = 0; k <= n; ++k) {
                                const double t = seg.start_angle
                                    + (seg.end_angle - seg.start_angle)
                                      * double(k) / double(n);
                                pts.push_back(ocsPoint(
                                    {major.x * std::cos(t) + minor.x * std::sin(t),
                                     major.y * std::cos(t) + minor.y * std::sin(t)},
                                    elev, hatch->extrusion));
                            }
                            break;
                        }
                        case 4: { // 样条 → 控制点连成折线（近似）
                            if (seg.control_points) {
                                for (BITCODE_BL k = 0;
                                     k < seg.num_control_points; ++k)
                                    pts.push_back(ocsPoint(
                                        {seg.control_points[k].point.x,
                                         seg.control_points[k].point.y},
                                        elev, hatch->extrusion));
                            } else if (seg.fitpts) {
                                for (BITCODE_BL k = 0; k < seg.num_fitpts; ++k)
                                    pts.push_back(ocsPoint({seg.fitpts[k].x,
                                        seg.fitpts[k].y}, elev,
                                        hatch->extrusion));
                            }
                            break;
                        }
                        default: break;
                        }
                    }
                }
                const bool wantClosed = !(path.flag & 0x20);
                if (pts.size() < 3) continue;

                // 环拆分：有些图纸把外环和岛串在同一路径里，环与环之间
                // 会出现异常大跳变（连接线横穿图纸 = 用户看到的"异常线条"）。
                // 判据：某段长度远超其前后相邻段（>10 倍）即为跳变，
                // 拆成各自独立的闭合环（正常的长直边不会被误拆）。
                std::vector<std::vector<P2>> loops;
                {
                    const size_t m = pts.size();
                    std::vector<size_t> cuts;
                    for (size_t i = 0; i + 1 < m; ++i) {
                        const double d = std::hypot(pts[i+1].x - pts[i].x,
                                                    pts[i+1].y - pts[i].y);
                        const double prevD = i > 0
                            ? std::hypot(pts[i].x - pts[i-1].x,
                                         pts[i].y - pts[i-1].y) : d;
                        const double nextD = i + 2 < m
                            ? std::hypot(pts[i+2].x - pts[i+1].x,
                                         pts[i+2].y - pts[i+1].y) : d;
                        const double ref = std::max(prevD, nextD);
                        if (d > ref * 10.0) cuts.push_back(i + 1);
                    }
                    if (cuts.empty()) {
                        loops.push_back(pts);
                    } else {
                        size_t begin = 0;
                        for (size_t c : cuts) {
                            if (c - begin >= 3)
                                loops.emplace_back(pts.begin() + long(begin),
                                                   pts.begin() + long(c));
                            begin = c;
                        }
                        if (m - begin >= 3)
                            loops.emplace_back(pts.begin() + long(begin),
                                               pts.end());
                    }
                }
                for (const std::vector<P2> &loop : loops) {
                    if (loop.size() < 3) continue;
                    if (!solidFill) allLoops.push_back(loop);
                    // 图案填充的边界线在 AutoCAD 里是不可见的（只画填充图案），
                    // 而我们之前把边界当折线输出 —— 边界由多段环拼成的填充
                    // （天花/玻璃图案）就会在图纸上多出一堆横穿房间的斜线，
                    // 手机上看到的就是"图形多了一大堆"。实心填充才需要边界+填充。
                    if (!solidFill) continue;
                    Ent e;
                    e.type = kPolyline;
                    e.closed = wantClosed;
                    e.fill = solidFill && wantClosed;
                    e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
                    emitPolylinePts(e, loop, wantClosed, transform);
                    e.updateBounds();
                    entities.push_back(std::move(e));
                }
            }
            // 图案填充（ANSI31/AR-CONC 等）：与桌面版/竞品一致，不生成图案线。
            // 天花/地面图案按 deflines 铺出来的平行线在整图视角下就是
            // "满图斜线网络"（競品与桌面版都不画图案线，只画实心填充）。
            // 符号类小块更是直接变乱麻。需要图案质感时再按区域大小放开。
            (void)hatch;
            (void)allLoops;
            (void)layerIdx;
            (void)colorOverride;
            (void)linewt;
            (void)transform;
            break;
        }
        case DWG_TYPE_VIEWPORT: {
            // 纸面视口：输出视口记录（纸面矩形 + 模型→纸面仿射），
            // Kotlin 端把模型空间文档叠加渲染进该矩形（避免每布局复制模型内容）。
            auto *vp = dwg_object_to_VIEWPORT(object);
            if (!vp || vp->width <= 0.0 || vp->height <= 0.0) return;
            if (!std::isfinite(vp->VIEWSIZE) || vp->VIEWSIZE <= 1e-9) return;
            // id=1 是 AutoCAD 隐式布局视口：VIEWCTR/VIEWSIZE 存的是纸面
            // 坐标而非模型视图参数，渲染模型内容必然错误，直接跳过
            if (vp->id <= 1) return;
            ALOGI("VP id=%d on=%d w=%.3f h=%.3f VIEWSIZE=%.3f "
                  "VIEWCTR=(%.2f,%.2f) target=(%.2f,%.2f,%.2f) "
                  "dir=(%.2f,%.2f,%.2f) twist=%.4f center=(%.3f,%.3f)",
                  vp->id, vp->on_off, vp->width, vp->height, vp->VIEWSIZE,
                  vp->VIEWCTR.x, vp->VIEWCTR.y,
                  vp->view_target.x, vp->view_target.y, vp->view_target.z,
                  vp->VIEWDIR.x, vp->VIEWDIR.y, vp->VIEWDIR.z,
                  vp->VIEWTWIST, vp->center.x, vp->center.y);
            const double s = vp->height / vp->VIEWSIZE;
            const double th = vp->VIEWTWIST;
            Ent e;
            e.type = kViewport;
            e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
            const double m11 = std::cos(th) * s;
            const double m12 = -std::sin(th) * s;
            const double m21 = std::sin(th) * s;
            const double m22 = std::cos(th) * s;
            const double dx = vp->center.x
                - (m11 * vp->VIEWCTR.x + m21 * vp->VIEWCTR.y);
            const double dy = vp->center.y
                - (m12 * vp->VIEWCTR.x + m22 * vp->VIEWCTR.y);
            e.num = {vp->center.x, vp->center.y, vp->width, vp->height,
                     m11, m12, m21, m22, dx, dy};
            e.bbox[0] = vp->center.x - vp->width * 0.5;
            e.bbox[2] = vp->center.x + vp->width * 0.5;
            e.bbox[1] = vp->center.y - vp->height * 0.5;
            e.bbox[3] = vp->center.y + vp->height * 0.5;
            entities.push_back(std::move(e));
            return;
        }
        default: {
            const bool dimension =
                (object->fixedtype >= DWG_TYPE_DIMENSION_ORDINATE
                 && object->fixedtype <= DWG_TYPE_DIMENSION_ANG2LN)
                || object->fixedtype == DWG_TYPE_DIMENSION_RADIUS
                || object->fixedtype == DWG_TYPE_DIMENSION_DIAMETER
                || object->fixedtype == DWG_TYPE_LARGE_RADIAL_DIMENSION
                || object->fixedtype == DWG_TYPE_ARC_DIMENSION;
            if (dimension) {
                Dwg_DIMENSION_common *dim = nullptr;
                if (object->fixedtype == DWG_TYPE_LARGE_RADIAL_DIMENSION
                    && object->tio.entity) {
                    dim = (Dwg_DIMENSION_common *)object->tio.entity->tio
                              .LARGE_RADIAL_DIMENSION;
                } else if (object->fixedtype == DWG_TYPE_ARC_DIMENSION
                           && object->tio.entity) {
                    dim = (Dwg_DIMENSION_common *)object->tio.entity->tio
                              .ARC_DIMENSION;
                } else {
                    dim = dwg_object_to_DIMENSION(object);
                }
                bool expanded = false;
                if (dim && dim->block) {
                    int err = 0;
                    Dwg_Object *header =
                        dwg_ref_get_object(dim->block, &err);
                    if (header && !err) {
                        visitBlockHeader(header, transform, depth + 1,
                                         &layerIdx);
                        expanded = true;
                    }
                }
                // 匿名块缺失时标注几何丢失，但标注文字仍真实存在：
                // user_text 优先，否则用实测值
                if (dim && !expanded) {
                    std::string label;
                    bool gbk = false;
                    if (dim->user_text)
                        label = prepareText(std::string(dim->user_text), gbk);
                    if (label.empty()
                        && std::isfinite(dim->act_measurement)) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%.0f",
                                      dim->act_measurement);
                        label = buf;
                    }
                    if (!label.empty()) {
                        // 字高取标注样式 DIMTXT×DIMSCALE，缺失时退 2.5
                        double textH = 2.5;
                        if (dim->dimstyle) {
                            int stErr = 0;
                            Dwg_Object *styleObject =
                                dwg_ref_get_object(dim->dimstyle, &stErr);
                            Dwg_Object_DIMSTYLE *style =
                                styleObject && !stErr
                                ? dwg_object_to_DIMSTYLE(styleObject)
                                : nullptr;
                            if (style && style->DIMTXT > 1e-9)
                                textH = style->DIMTXT
                                    * std::max(1.0, style->DIMSCALE);
                        }
                        Ent e;
                        e.type = kText;
                        e.layer = layerIdx;
                        e.color = colorOverride;
                        e.linewt = linewt;
                        e.gbk = gbk;
                        e.text = label;
                        const P2 pos = ocsPoint(
                            {dim->text_midpt.x, dim->text_midpt.y},
                            dim->elevation, dim->extrusion);
                        const double rot =
                            std::fabs(dim->text_rotation) > 1e-12
                                ? dim->text_rotation : dim->horiz_dir;
                        e.num = {pos.x, pos.y, std::max(0.1, textH),
                                 rot * 180.0 / kPi, 1.0, 0.0, 0.0};
                        applyTextTransform(e, transform);
                        e.updateBounds();
                        entities.push_back(std::move(e));
                    }
                }
            }
            break;
        }
        }
    }

    void emitPolylinePts(Ent &e, const std::vector<P2> &pts, bool closed,
                         const Affine &transform) {
        e.num.reserve(pts.size() * 2 + 2);
        e.num.push_back(closed ? 1.0 : 0.0);
        e.num.push_back(double(pts.size()));
        for (const P2 &p : pts) {
            const P2 w = transform.map(p);
            e.num.push_back(w.x);
        }
        for (const P2 &p : pts) {
            const P2 w = transform.map(p);
            e.num.push_back(w.y);
        }
        // payload: closed 标志后面才是坐标 —— 调整：去头两个
        // （直接重排为 x[n], y[n]，closed 存在 e.closed）
        e.num.erase(e.num.begin(), e.num.begin() + 2);
        e.updateBounds();
    }

    // 图案填充的图案线：按每个图案定义（角度/间距）铺平行线，
    // 再用边界环做奇偶裁剪，得到落在填充区域内的线段。
    // 线条总数有预算上限，避免大图密集图案把实体数撑爆导致卡顿。
    void emitHatchPattern(Dwg_Entity_HATCH *hatch,
                          const std::vector<std::vector<P2>> &loops,
                          const Affine &transform, int32_t layerIdx,
                          int32_t colorOverride, int32_t linewt) {
        if (!hatch->deflines || hatch->num_deflines <= 0) return;
        if (patternLineCount >= 60000) return;
        double minX = 1e308, minY = 1e308, maxX = -1e308, maxY = -1e308;
        for (const auto &loop : loops) {
            for (const P2 &p : loop) {
                minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
            }
        }
        if (!(maxX > minX) || !(maxY > minY)) return;
        const P2 corners[4] = {{minX, minY}, {maxX, minY},
                               {maxX, maxY}, {minX, maxY}};
        const int perLineBudget = 70;   // 每个图案定义最多多少条线
        for (BITCODE_BS d = 0; d < hatch->num_deflines; ++d) {
            const Dwg_HATCH_DefLine &def = hatch->deflines[d];
            const double ang = def.angle;
            if (!std::isfinite(ang)) continue;
            const double dx = std::cos(ang), dy = std::sin(ang);
            const double nx = -dy, ny = dx;              // 法向
            double spacing = std::fabs(def.offset.x * nx + def.offset.y * ny);
            if (!(spacing > 1e-9))
                spacing = std::hypot(def.offset.x, def.offset.y);
            if (!(spacing > 1e-9)) continue;
            const double baseProj = def.pt0.x * nx + def.pt0.y * ny;
            double lo = 1e308, hi = -1e308;
            for (const P2 &c : corners) {
                const double pr = c.x * nx + c.y * ny;
                lo = std::min(lo, pr); hi = std::max(hi, pr);
            }
            if (!(hi > lo)) continue;
            const double wanted = (hi - lo) / spacing + 2.0;
            int step = int(std::ceil(wanted / double(perLineBudget)));
            if (step < 1) step = 1;
            const double gap = spacing * double(step);
            double pr = baseProj
                - std::floor((baseProj - lo) / gap) * gap;
            for (; pr <= hi; pr += gap) {
                const P2 origin{def.pt0.x + (pr - baseProj) * nx,
                                def.pt0.y + (pr - baseProj) * ny};
                std::vector<double> ts;
                ts.reserve(8);
                for (const auto &loop : loops) {
                    const size_t n = loop.size();
                    for (size_t i = 0; i < n; ++i) {
                        const P2 a = loop[i];
                        const P2 b = loop[(i + 1) % n];
                        const double ex = b.x - a.x, ey = b.y - a.y;
                        const double det = dx * ey - dy * ex;
                        if (std::fabs(det) < 1e-12) continue;
                        const double rx = a.x - origin.x, ry = a.y - origin.y;
                        const double t = (rx * ey - ry * ex) / det;
                        const double s = (rx * dy - ry * dx) / det;
                        if (s >= -1e-9 && s <= 1.0 + 1e-9) ts.push_back(t);
                    }
                }
                if (ts.size() < 2) continue;
                std::sort(ts.begin(), ts.end());
                for (size_t i = 0; i + 1 < ts.size(); i += 2) {
                    if (ts[i + 1] - ts[i] < 1e-9) continue;
                    const P2 w1 = transform.map(
                        {origin.x + dx * ts[i], origin.y + dy * ts[i]});
                    const P2 w2 = transform.map(
                        {origin.x + dx * ts[i + 1], origin.y + dy * ts[i + 1]});
                    Ent e;
                    e.type = kLine;
                    e.layer = layerIdx; e.color = colorOverride;
                    e.linewt = linewt;
                    e.num = {w1.x, w1.y, w2.x, w2.y};
                    e.updateBounds();
                    entities.push_back(std::move(e));
                    ++patternLineCount;
                    if (patternLineCount >= 60000) return;
                }
            }
        }
    }

    void emitText(Dwg_Object *object, Dwg_Entity_TEXT *text,
                  const char *type, const char *field,
                  const Affine &transform, int32_t layerIdx,
                  int32_t colorOverride, int32_t linewt) {
        (void)object;
        Ent e;
        e.type = kText;
        e.layer = layerIdx; e.color = colorOverride; e.linewt = linewt;
        e.text = prepareText(utf8Field(text, type, field), e.gbk);
        if (e.text.empty()) return;
        const bool useAlignment =
            text->horiz_alignment == 4 || text->vert_alignment > 0;
        const BITCODE_2DPOINT &pos =
            useAlignment ? text->alignment_pt : text->ins_pt;
        const P2 ocs = ocsPoint({pos.x, pos.y}, text->elevation,
                                text->extrusion);
        e.num = {ocs.x, ocs.y, std::max(0.1, text->height),
                 text->rotation * 180.0 / kPi,
                 std::max(0.05, text->width_factor),
                 double(text->horiz_alignment),
                 double(text->vert_alignment)};
        applyTextTransform(e, transform);
        e.updateBounds();
#ifdef CADCORE_TRACE_TEXT
        ALOGI("TXT %llu TEXT L%d %.2f %.2f h=%.2f a=%.1f |%s|",
              (unsigned long long)object->handle.value, e.layer,
              e.num[0], e.num[1], e.num[2], e.num[3], e.text.c_str());
#endif
        entities.push_back(std::move(e));
    }

    // 文本随块变换：位置映射、字高按 Y 缩放、旋转按 X 轴方向；
    // 镜像块里的文字不翻转（除非 MIRRTEXT），与桌面版 append() 一致
    void applyTextTransform(Ent &e, const Affine &transform) {
        const P2 mapped = transform.map({e.num[0], e.num[1]});
        e.num[0] = mapped.x;
        e.num[1] = mapped.y;
        const double sourceAngle = e.num[3] * kPi / 180.0;
        const double sx = transform.m11 * std::cos(sourceAngle)
            + transform.m21 * std::sin(sourceAngle);
        const double sy = transform.m12 * std::cos(sourceAngle)
            + transform.m22 * std::sin(sourceAngle);
        const double tx = -transform.m11 * std::sin(sourceAngle)
            + transform.m21 * std::cos(sourceAngle);
        const double ty = -transform.m12 * std::sin(sourceAngle)
            + transform.m22 * std::cos(sourceAngle);
        const double xLength = std::hypot(sx, sy);
        const double yLength = std::hypot(tx, ty);
        const bool reflected = sx * ty - sy * tx < 0.0;
        double rotation = std::atan2(sy, sx) * 180.0 / kPi;
        if (reflected && !mirrText) {
            while (rotation > 90.0) rotation -= 180.0;
            while (rotation <= -90.0) rotation += 180.0;
        }
        e.num[3] = rotation;
        e.num[2] *= yLength;
        if (xLength > 1e-12) e.num[4] *= yLength / xLength;
    }

    // -------------------------------------------------- INSERT

    void visitInsert(Dwg_Object *object, Dwg_Entity_INSERT *insert,
                     const Affine &outer, int depth,
                     int32_t insertLayerIdx) {
        if (!insert || !insert->block_header || depth > 32) return;
        int err = 0;
        Dwg_Object *header =
            dwg_ref_get_object(insert->block_header, &err);
        if (!header || err) return;
        auto *block = dwg_object_to_BLOCK_HEADER(header);
        if (!block) return;

        const double sx = std::abs(insert->scale.x) > 1e-12
            ? insert->scale.x : 1.0;
        const double sy = std::abs(insert->scale.y) > 1e-12
            ? insert->scale.y : 1.0;
        const double cosine = std::cos(insert->rotation);
        const double sine = std::sin(insert->rotation);
        Affine local;
        local.m11 = cosine * sx;
        local.m12 = sine * sx;
        local.m21 = -sine * sy;
        local.m22 = cosine * sy;
        local.dx = insert->ins_pt.x - local.m11 * block->base_pt.x
                 - local.m21 * block->base_pt.y;
        local.dy = insert->ins_pt.y - local.m12 * block->base_pt.x
                 - local.m22 * block->base_pt.y;
        const int columns = std::min(256, std::max(1, int(insert->num_cols)));
        const int rows = std::min(256, std::max(1, int(insert->num_rows)));
        const std::vector<P2> clipPolygon = clipFor(object->handle.value);

        // 块属性文字（轴号圈里的编号、房间名等 ATTRIB）：
        // ATTRIB 不在模型空间实体表里，只挂在 INSERT 上，此前从未输出。
        // 注意：ATTRIB 的坐标已是世界坐标（DXF 语义），不能再乘块插入变换，
        // 否则块缩放会把文字放大上千倍并甩到图外。
        const Affine attribBase;
        if (insert->attribs && insert->num_owned > 0) {
            const BITCODE_BL na = std::min<BITCODE_BL>(insert->num_owned, 4096);
            for (BITCODE_BL ai = 0; ai < na; ++ai) {
                int aerr = 0;
                Dwg_Object *ao = dwg_ref_get_object(insert->attribs[ai], &aerr);
                if (!ao || aerr || ao->fixedtype != DWG_TYPE_ATTRIB) continue;
                // 动态块的可见性状态靠 ATTRIB 的 invisible 标志实现：
                // 一个"材料/门窗编号"块里带 20 个候选属性，只有当前状态下那一个
                // 可见。不过滤的话 20 段文字全叠在同一位置 —— 用户看到的
                // "有的文字被重复绘制"。
                if (ao->tio.entity && ao->tio.entity->invisible) continue;
                auto *attrib = dwg_object_to_ATTRIB(ao);
                if (!attrib) continue;
                Ent e;
                e.type = kText;
                e.layer = ao->tio.entity
                    ? int32_t(layerIndexOf(ao->tio.entity)) : insertLayerIdx;
                if (e.layer < 0) e.layer = insertLayerIdx;
                e.color = -1; e.linewt = -3;
                e.text = prepareText(utf8Field(attrib, "ATTRIB", "text_value"),
                                     e.gbk);
                if (e.text.empty()) continue;
                const bool useAlignment =
                    attrib->horiz_alignment == 4 || attrib->vert_alignment > 0;
                const BITCODE_2DPOINT &pos =
                    useAlignment ? attrib->alignment_pt : attrib->ins_pt;
                e.num = {pos.x, pos.y, std::max(0.1, attrib->height),
                         attrib->rotation * 180.0 / kPi,
                         std::max(0.05, attrib->width_factor),
                         double(attrib->horiz_alignment),
                         double(attrib->vert_alignment)};
                applyTextTransform(e, attribBase);
                e.updateBounds();
#ifdef CADCORE_TRACE_TEXT
                ALOGI("TXT %llu ATTRIB-INS L%d %.2f %.2f h=%.2f a=%.1f ins=(%.2f,%.2f) ali=(%.2f,%.2f) ha=%d va=%d use=%d |%s|",
                      (unsigned long long)ao->handle.value, e.layer,
                      e.num[0], e.num[1], e.num[2], e.num[3],
                      attrib->ins_pt.x, attrib->ins_pt.y,
                      attrib->alignment_pt.x, attrib->alignment_pt.y,
                      (int)attrib->horiz_alignment, (int)attrib->vert_alignment,
                      int(useAlignment), e.text.c_str());
#endif
                entities.push_back(std::move(e));
            }
        }

        // 块内 BYBLOCK（ACI 0）图元的颜色 = 本 INSERT 的颜色：
        // 有显式色取显式，否则取 INSERT 所在图层色；嵌套时向内传递。
        int32_t insertColor = int32_t(layerColorOf(insertLayerIdx));
        {
            int cerr = 0;
            const Dwg_Color *ec = object->tio.entity
                ? dwg_ent_get_color(object->tio.entity, &cerr) : nullptr;
            if (ec && ec->index == 0) {
                if (curBlockColor >= 0) insertColor = curBlockColor;
            } else if (ec && ec->index != 256 && ec->index > 0) {
                insertColor = int32_t(entityColor(*ec, layerColorOf(insertLayerIdx)));
            }
        }
        const int32_t savedBlockColor = curBlockColor;
        curBlockColor = insertColor;

        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                const double ox = column * insert->col_spacing;
                const double oy = row * insert->row_spacing;
                Affine arrayLocal = local;
                arrayLocal.dx += cosine * ox - sine * oy;
                arrayLocal.dy += sine * ox + cosine * oy;
                const Affine combined =
                    arrayLocal.then(ocsAffine(insert->extrusion)).then(outer);
                const size_t cellBegin = entities.size();
                const int32_t blockLayer = insertLayerIdx;
                visitBlockHeader(header, combined, depth + 1, &blockLayer);
                if (!clipPolygon.empty()) {
                    double minX = 0, minY = 0, maxX = 0, maxY = 0;
                    bool have = false;
                    for (const P2 &v : clipPolygon) {
                        const P2 m = combined.map(v);
                        if (!have) {
                            minX = maxX = m.x; minY = maxY = m.y; have = true;
                        } else {
                            minX = std::min(minX, m.x);
                            minY = std::min(minY, m.y);
                            maxX = std::max(maxX, m.x);
                            maxY = std::max(maxY, m.y);
                        }
                    }
                    applyClipToRange(cellBegin, entities.size(),
                                     minX, minY, maxX, maxY);
                }
            }
        }
        curBlockColor = savedBlockColor;
    }

    void applyClipToRange(size_t begin, size_t end,
                          double minX, double minY,
                          double maxX, double maxY) {
        if (maxX <= minX || maxY <= minY) return;
        for (size_t i = begin; i < end;) {
            Ent &e = entities[i];
            if (e.bbox[0] > e.bbox[2] || e.bbox[1] > e.bbox[3]) { ++i; continue; }
            const bool noOverlap = e.bbox[2] < minX || e.bbox[0] > maxX
                || e.bbox[3] < minY || e.bbox[1] > maxY;
            if (noOverlap) {
                entities.erase(entities.begin() + long(i));
                --end;
                continue;
            }
            const bool inside = e.bbox[0] >= minX && e.bbox[2] <= maxX
                && e.bbox[1] >= minY && e.bbox[3] <= maxY;
            if (inside) { ++i; continue; }
            bool clipped = false;
            if (e.type == kLine) {
                P2 a{e.num[0], e.num[1]}, b{e.num[2], e.num[3]};
                if (clipSegmentToRect(a, b, minX, minY, maxX, maxY)) {
                    e.num = {a.x, a.y, b.x, b.y};
                    e.updateBounds();
                    clipped = true;
                }
            } else if (e.type == kPolyline) {
                const size_t n = e.num.size() / 2;
                std::vector<double> x, y;
                bool any = false;
                for (size_t p = 0; p + 1 < n; ++p) {
                    P2 a{e.num[p], e.num[n + p]};
                    P2 b{e.num[p + 1], e.num[n + p + 1]};
                    if (clipSegmentToRect(a, b, minX, minY, maxX, maxY)) {
                        if (x.empty() || x.back() != a.x || y.back() != a.y) {
                            x.push_back(a.x); y.push_back(a.y);
                        }
                        x.push_back(b.x); y.push_back(b.y);
                        any = true;
                    }
                }
                if (!any || x.size() < 2) {
                    entities.erase(entities.begin() + long(i));
                    --end;
                    continue;
                }
                e.num.clear();
                e.num.insert(e.num.end(), x.begin(), x.end());
                e.num.insert(e.num.end(), y.begin(), y.end());
                e.updateBounds();
                clipped = true;
            }
            if (!clipped) { ++i; continue; }
            ++i;
        }
    }

    // -------------------------------------------------- 序列化

    // 主体范围：杂散图元（跑飞的标注/辅助线）会把整体 bounds 撑大数倍，
    // 初始视野下主图缩成一点。两个候选：
    //   A. 中心坐标 1%/99% 分位框（对密集主图+少量跑飞图元很准）
    //   B. 64×64 网格密度：覆盖 98% 图元的高密度格范围
    // 各自统计框内图元率，取「覆盖率 ≥60% 且面积更小」者；都不达标用整体 bounds。
    Bounds mainBounds() const {
        std::vector<double> xs, ys;
        xs.reserve(entities.size());
        ys.reserve(entities.size());
        for (const Ent &e : entities) {
            if (e.bbox[0] > e.bbox[2] || e.bbox[1] > e.bbox[3]) continue;
            const double cx = (e.bbox[0] + e.bbox[2]) * 0.5;
            const double cy = (e.bbox[1] + e.bbox[3]) * 0.5;
            if (!std::isfinite(cx) || !std::isfinite(cy)) continue;
            if (std::fabs(cx) > 1e9 || std::fabs(cy) > 1e9) continue;
            xs.push_back(cx);
            ys.push_back(cy);
        }
        const size_t n = xs.size();
        if (n < 32 || !bounds.valid()) return bounds;

        auto coverage = [&](const Bounds &b) {
            if (!b.valid()) return 0.0;
            size_t in = 0;
            for (size_t i = 0; i < n; ++i)
                if (xs[i] >= b.minX && xs[i] <= b.maxX &&
                    ys[i] >= b.minY && ys[i] <= b.maxY) ++in;
            return double(in) / double(n);
        };

        // 候选 A：分位框（留 5% 边距）
        auto pct = [](std::vector<double> &v, double p) {
            const size_t k = size_t(v.size() * p);
            std::nth_element(v.begin(), v.begin() + long(k), v.end());
            return v[k];
        };
        const double ax0 = pct(xs, 0.01), ax1 = pct(xs, 0.99);
        const double ay0 = pct(ys, 0.01), ay1 = pct(ys, 0.99);
        Bounds candA;
        if (ax1 > ax0 && ay1 > ay0) {
            const double mx = (ax1 - ax0) * 0.05 + 1.0;
            const double my = (ay1 - ay0) * 0.05 + 1.0;
            candA.minX = ax0 - mx; candA.minY = ay0 - my;
            candA.maxX = ax1 + mx; candA.maxY = ay1 + my;
        }

        // 候选 B：网格密度框
        Bounds candB;
        {
            const int kGrid = 64;
            const double cw = (bounds.maxX - bounds.minX) / kGrid;
            const double ch = (bounds.maxY - bounds.minY) / kGrid;
            if (cw > 0 && ch > 0) {
                std::vector<uint32_t> cells(kGrid * kGrid, 0);
                for (size_t i = 0; i < n; ++i) {
                    int c = int((xs[i] - bounds.minX) / cw);
                    int r = int((ys[i] - bounds.minY) / ch);
                    if (c < 0) c = 0; if (c >= kGrid) c = kGrid - 1;
                    if (r < 0) r = 0; if (r >= kGrid) r = kGrid - 1;
                    cells[r * kGrid + c]++;
                }
                std::vector<uint32_t> order(cells);
                std::sort(order.begin(), order.end(), std::greater<uint32_t>());
                const uint64_t target = n * 98 / 100;
                uint64_t acc = 0;
                uint32_t threshold = 0;
                for (int i = 0; i < kGrid * kGrid; ++i) {
                    acc += order[i];
                    threshold = order[i];
                    if (acc >= target) break;
                }
                if (threshold > 0) {
                    int c0 = kGrid, c1 = -1, r0 = kGrid, r1 = -1;
                    for (int r = 0; r < kGrid; ++r)
                        for (int c = 0; c < kGrid; ++c)
                            if (cells[r * kGrid + c] >= threshold) {
                                c0 = std::min(c0, c); c1 = std::max(c1, c);
                                r0 = std::min(r0, r); r1 = std::max(r1, r);
                            }
                    if (c1 >= c0 && r1 >= r0) {
                        candB.minX = bounds.minX + c0 * cw;
                        candB.minY = bounds.minY + r0 * ch;
                        candB.maxX = bounds.minX + (c1 + 1) * cw;
                        candB.maxY = bounds.minY + (r1 + 1) * ch;
                    }
                }
            }
        }

        // 候选 D：最大密度簇生长框。分位框会被少量远处散点拉偏
        // （实测案例：90% 实体集中在 5 万宽的主图，约 10% 散点分布在
        // 500 万宽的范围，p1 落在散点区 → main 覆盖 186 万宽 →
        // 打开图纸后主图只占屏幕 3% = "图形不全"）。
        // 从最密格子按连通性生长，逐级放宽阈值，得到紧致的主图框。
        Bounds candD;
        {
            const int kGrid = 64;
            const double cw = (bounds.maxX - bounds.minX) / kGrid;
            const double ch = (bounds.maxY - bounds.minY) / kGrid;
            if (cw > 0 && ch > 0) {
                std::vector<uint32_t> cells(kGrid * kGrid, 0);
                for (size_t i = 0; i < n; ++i) {
                    int c = int((xs[i] - bounds.minX) / cw);
                    int r = int((ys[i] - bounds.minY) / ch);
                    if (c < 0) c = 0; if (c >= kGrid) c = kGrid - 1;
                    if (r < 0) r = 0; if (r >= kGrid) r = kGrid - 1;
                    cells[r * kGrid + c]++;
                }
                int sc = 0, sr = 0;
                uint32_t seed = 0;
                for (int r = 0; r < kGrid; ++r)
                    for (int c = 0; c < kGrid; ++c)
                        if (cells[r * kGrid + c] > seed) {
                            seed = cells[r * kGrid + c];
                            sr = r; sc = c;
                        }
                if (seed > 0) {
                    std::vector<char> used(kGrid * kGrid, 0);
                    std::vector<std::pair<int,int>> queue;
                    const uint32_t thresholds[4] = {
                        seed / 20, seed / 100, seed / 500, 1};
                    for (int ti = 0; ti < 4; ++ti) {
                        const uint32_t minCount = thresholds[ti];
                        std::fill(used.begin(), used.end(), 0);
                        queue.clear();
                        queue.push_back({sr, sc});
                        used[sr * kGrid + sc] = 1;
                        uint64_t acc = seed;
                        size_t head = 0;
                        while (head < queue.size()) {
                            const int r = queue[head].first;
                            const int c = queue[head].second;
                            ++head;
                            for (int dr = -1; dr <= 1; ++dr)
                                for (int dc = -1; dc <= 1; ++dc) {
                                    if (!dr && !dc) continue;
                                    const int r2 = r + dr, c2 = c + dc;
                                    if (r2 < 0 || r2 >= kGrid ||
                                        c2 < 0 || c2 >= kGrid) continue;
                                    if (used[r2 * kGrid + c2]) continue;
                                    if (cells[r2 * kGrid + c2] < minCount)
                                        continue;
                                    used[r2 * kGrid + c2] = 1;
                                    acc += cells[r2 * kGrid + c2];
                                    queue.push_back({r2, c2});
                                }
                        }
                        int c0 = kGrid, c1 = -1, r0 = kGrid, r1 = -1;
                        for (int r = 0; r < kGrid; ++r)
                            for (int c = 0; c < kGrid; ++c)
                                if (used[r * kGrid + c]) {
                                    c0 = std::min(c0, c); c1 = std::max(c1, c);
                                    r0 = std::min(r0, r); r1 = std::max(r1, r);
                                }
                        if (c1 >= c0 && r1 >= r0) {
                            candD.minX = bounds.minX + c0 * cw;
                            candD.minY = bounds.minY + r0 * ch;
                            candD.maxX = bounds.minX + (c1 + 1) * cw;
                            candD.maxY = bounds.minY + (r1 + 1) * ch;
                        }
                        if (acc * 10 >= n * 7) break;   // 已覆盖 ≥70%
                    }
                }
            }
        }

        const double covA = coverage(candA);
        const double covB = coverage(candB);
        const double areaA = candA.valid() ? (candA.maxX - candA.minX) * (candA.maxY - candA.minY) : 1e308;
        const double areaB = candB.valid() ? (candB.maxX - candB.minX) * (candB.maxY - candB.minY) : 1e308;
        const double covD = coverage(candD);
        const double areaD = candD.valid() ? (candD.maxX - candD.minX) * (candD.maxY - candD.minY) : 1e308;
        const double fullArea = (bounds.maxX - bounds.minX) * (bounds.maxY - bounds.minY);
        Bounds best = bounds;
        if (covD >= 0.70 && areaD < fullArea &&
            (covA < 0.60 || areaD <= areaA) &&
            (covB < 0.60 || areaD <= areaB)) best = candD;
        else if (covA >= 0.60 && areaA < fullArea &&
            (covB < 0.60 || areaA <= areaB)) best = candA;
        else if (covB >= 0.60 && areaB < fullArea) best = candB;
        if (!best.valid()) return bounds;

        // 分位框只看图元"中心"，长墙/长轴线的中心落在密集区，
        // 但端点远远伸出去——按完整 bbox 迭代扩张，直到稳定。
        for (int it = 0; it < 4; ++it) {
            Bounds nb;
            bool any = false;
            for (const Ent &e : entities) {
                if (e.bbox[0] > e.bbox[2] || e.bbox[1] > e.bbox[3]) continue;
                if (e.bbox[2] < best.minX || e.bbox[0] > best.maxX ||
                    e.bbox[3] < best.minY || e.bbox[1] > best.maxY) continue;
                if (!std::isfinite(e.bbox[0]) || !std::isfinite(e.bbox[3])) continue;
                if (!any) { nb.minX = e.bbox[0]; nb.minY = e.bbox[1];
                            nb.maxX = e.bbox[2]; nb.maxY = e.bbox[3]; any = true; }
                else {
                    nb.minX = std::min(nb.minX, e.bbox[0]);
                    nb.minY = std::min(nb.minY, e.bbox[1]);
                    nb.maxX = std::max(nb.maxX, e.bbox[2]);
                    nb.maxY = std::max(nb.maxY, e.bbox[3]);
                }
            }
            if (!any || !nb.valid()) break;
            const double prevArea =
                (best.maxX - best.minX) * (best.maxY - best.minY);
            best = nb;
            const double grown =
                (best.maxX - best.minX) * (best.maxY - best.minY);
            if (grown <= prevArea * 1.05) break;   // 收敛
            if (grown >= fullArea * 0.9) break;    // 已接近全图，到此为止
        }
        return best;
    }

    // v8: 剔除跑飞垃圾实体（远离主体的杂散块引用）。
    // 这些图元（如坐标 ±1500 万的孤立 INSERT）会把整体 bounds 撑大数百倍，
    // 「全图」适配后主图缩成一个点。以 mainBounds 为基准、四周留一倍主图范围作缓冲，
    // 完全落在缓冲区外的图元直接丢弃。
    void cullStray() {
        const Bounds main = mainBounds();
        if (!main.valid()) return;
        const double margin =
            std::max(main.maxX - main.minX, main.maxY - main.minY);
        if (!(margin > 0.0)) return;
        const double x0 = main.minX - margin, x1 = main.maxX + margin;
        const double y0 = main.minY - margin, y1 = main.maxY + margin;
        const size_t before = entities.size();
        entities.erase(std::remove_if(entities.begin(), entities.end(),
                                      [&](const Ent &e) {
            if (e.xref) return false;   // 外部参照图元不剔除（可能远离主体）
            if (e.bbox[0] > e.bbox[2] || e.bbox[1] > e.bbox[3]) return false;
            return e.bbox[2] < x0 || e.bbox[0] > x1 ||
                   e.bbox[3] < y0 || e.bbox[1] > y1;
        }), entities.end());
        if (entities.size() != before) {
            ALOGI("cullStray: %zu -> %zu", before, entities.size());
            bounds = computeBounds();
        }
    }

    bool serialize(const char *outPath) const {
        FILE *f = std::fopen(outPath, "wb");
        if (!f) {
            ALOGE("cannot open %s", outPath);
            return false;
        }
        auto w8 = [&](uint8_t v) { std::fwrite(&v, 1, 1, f); };
        auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wi32 = [&](int32_t v) { std::fwrite(&v, 4, 1, f); };
        auto w64 = [&](double v) { std::fwrite(&v, 8, 1, f); };

        const Bounds main = mainBounds();
        std::fwrite("SKDW", 1, 4, f);
        // v13: 同目录外部参照自动合并（图框等 xref 子图展平进模型/布局）
        // v12: 块内图案填充不再生成图案线（灯具/家具符号被铺成一团乱麻）+ 文字远景 LOD
        // v11: aciColor 换成标准 AutoCAD ACI 256 色表（颜色严格按图纸设定，
        //      不再用假色相调配）；旧缓存颜色错误，必须失效重解析
        // v10: mainBounds 改最大密度簇算法（散点不再把主体范围撑大数十倍）
        // v9: SPLINE（de Boor 采样）/ POLYLINE_3D 输出；图案填充生成图案线；
        //     GBK 文本控制码清理；MTEXT \P 换行按行绘制
        // v8: 剔除跑飞垃圾实体（模型空间与布局同样生效），全图适配不再被 ±千万级
        //     杂散块引用撑爆
        // v7: 输出 INSERT 的 ATTRIB 属性文字（轴号/门窗编号/房间名，世界坐标直出）
        // v6: 弧包围盒按实际弧段计算；HATCH 边界多环拆分（消除环间跳变斜线）
        // v5: mainBounds 修复——分位框后按完整 bbox 迭代扩张，长墙/轴线不再被裁掉
        // v4: POLYLINE 增加 fill 字节（SOLID/HATCH 实心填充）
        // v3: 文本带编码标志（1=按 GB18030 解码）；v2: TEXT 头 5 double+2 int32、主体范围
        // v14: 块内 0 层实体继承 INSERT 图层（图层/颜色错乱）、BYBLOCK 颜色、
        //      动态块隐藏 ATTRIB 不再输出（文字叠成重影）、xref 按 INSERT 变换落位
        w32(14);
        w64(bounds.minX); w64(bounds.minY);
        w64(bounds.maxX); w64(bounds.maxY);
        w64(main.minX); w64(main.minY);
        w64(main.maxX); w64(main.maxY);
        w32(uint32_t(layers.size()));
        w32(uint32_t(entities.size()));
        for (const LayerRec &layer : layers) {
            const auto len = uint8_t(std::min<size_t>(255, layer.name.size()));
            w8(len);
            std::fwrite(layer.name.data(), 1, len, f);
            w8(layer.gbk ? 1 : 0);
            wi32(int32_t(layer.color));
            w8(layer.visible ? 1 : 0);
            wi32(layer.linewt);
        }
        for (const Ent &e : entities) {
            // 防线：payload 长度必须与类型匹配，否则 Kotlin 端解码会错位。
            // 不合规的图元直接跳过（宁缺勿错）。
            size_t expect = 0;
            switch (e.type) {
            case kLine: expect = 4; break;
            case kCircle: expect = 3; break;
            case kArc: expect = 5; break;
            case kPolyline: expect = e.num.size() - e.num.size() % 2; break;
            case kText: expect = 7; break;
            case kViewport: expect = 10; break;
            default: continue;
            }
            if (e.type != kPolyline && e.num.size() != expect) {
                ALOGE("skip entity type=%u num=%zu expect=%zu",
                      e.type, e.num.size(), expect);
                continue;
            }
            w8(e.type);
            wi32(e.layer);
            wi32(e.color);
            w64(e.bbox[0]); w64(e.bbox[1]);
            w64(e.bbox[2]); w64(e.bbox[3]);
            switch (e.type) {
            case kLine:
                for (double v : e.num) w64(v);
                break;
            case kCircle:
                for (double v : e.num) w64(v);
                break;
            case kArc:
                for (double v : e.num) w64(v);
                break;
            case kViewport:
                for (double v : e.num) w64(v);
                break;
            case kPolyline: {
                const size_t n = e.num.size() / 2;
                w8(e.closed ? 1 : 0);
                w8(e.fill ? 1 : 0);
                w32(uint32_t(n));
                for (size_t i = 0; i < n; ++i) w64(e.num[i]);
                for (size_t i = 0; i < n; ++i) w64(e.num[n + i]);
                break;
            }
            case kText:
                // v2: x y h rot ws 为 double，hAlign/vAlign 为 int32
                // v3: 首字节为编码标志（1 = 按 GB18030 解码）
                w8(e.gbk ? 1 : 0);
                w64(e.num[0]); w64(e.num[1]); w64(e.num[2]);
                w64(e.num[3]); w64(e.num[4]);
                wi32(int32_t(e.num[5]));
                wi32(int32_t(e.num[6]));
                wi32(int32_t(e.text.size()));
                std::fwrite(e.text.data(), 1, e.text.size(), f);
                break;
            }
        }
        const bool ok = std::ferror(f) == 0;
        std::fclose(f);
        return ok;
    }
};

} // namespace

// ---------------------------------------------------------------- JNI

extern "C" JNIEXPORT jint JNICALL
Java_com_sukancad_cad_DwgCore_parseNative(JNIEnv *env, jclass,
                                          jstring jsrc, jstring jcache) {
    const char *src = env->GetStringUTFChars(jsrc, nullptr);
    const char *cache = env->GetStringUTFChars(jcache, nullptr);
    Exporter exporter;
    const bool loaded = exporter.run(src);
    if (!loaded) {
        ALOGE("parse FAILED run: ents=%zu layers=%zu",
              exporter.entities.size(), exporter.layers.size());
    }
    const bool wrote = loaded && exporter.serialize(cache);
    if (wrote) exporter.exportLayouts(cache);
    dwg_free(&exporter.dwg);
    env->ReleaseStringUTFChars(jsrc, src);
    env->ReleaseStringUTFChars(jcache, cache);
    if (!wrote) return -1;
    return 0;
}
