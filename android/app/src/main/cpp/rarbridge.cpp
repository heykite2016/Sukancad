// unrar JNI 桥：RAR4/RAR5 统一用官方 unrar 库解压
#include <jni.h>
#include <string>
#include <vector>
#include <cstdint>

#include "unrar/rar.hpp"   // 提供 UNRARCALLBACK/LPARAM 等基础类型
#include "unrar/dll.hpp"

#include <sys/stat.h>
#include <cstdlib>

namespace {

std::string utf16ToUtf8(const wchar_t *w) {
    if (!w) return {};
    std::string out;
    for (const wchar_t *p = w; *p; ++p) {
        uint32_t c = uint32_t(*p);
        if (c < 0x80) {
            out.push_back(char(c));
        } else if (c < 0x800) {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(char(0xE0 | (c >> 12)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (c >> 18)));
            out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::wstring utf8ToUtf16(const char *s) {
    std::wstring out;
    if (!s) return out;
    size_t i = 0;
    const size_t n = std::string(s).size();
    const auto *u = reinterpret_cast<const uint8_t *>(s);
    while (i < n) {
        uint32_t c = u[i];
        if (c < 0x80) { i += 1; }
        else if ((c >> 5) == 6 && i + 1 < n) {
            c = ((c & 0x1F) << 6) | (u[i + 1] & 0x3F); i += 2;
        } else if ((c >> 4) == 14 && i + 2 < n) {
            c = ((c & 0x0F) << 12) | ((u[i + 1] & 0x3F) << 6) | (u[i + 2] & 0x3F); i += 3;
        } else if ((c >> 3) == 30 && i + 3 < n) {
            c = ((c & 0x07) << 18) | ((u[i + 1] & 0x3F) << 12)
                | ((u[i + 2] & 0x3F) << 6) | (u[i + 3] & 0x3F); i += 4;
        } else { i += 1; c = 0xFFFD; }
        if (c >= 0x10000 && c <= 0x10FFFF) {
            c -= 0x10000;
            out.push_back(wchar_t(0xD800 + (c >> 10)));
            out.push_back(wchar_t(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(wchar_t(c));
        }
    }
    return out;
}

} // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_sukancad_cad_RarBridge_extractAll(JNIEnv *env, jclass,
                                           jstring jArc, jstring jDest) {
    const char *arc = env->GetStringUTFChars(jArc, nullptr);
    const char *dest = env->GetStringUTFChars(jDest, nullptr);
    const std::wstring arcW = utf8ToUtf16(arc);
    const std::wstring destW = utf8ToUtf16(dest);

    RAROpenArchiveDataEx open{};
    open.ArcNameW = const_cast<wchar_t *>(arcW.c_str());
    open.OpenMode = RAR_OM_EXTRACT;
    HANDLE handle = RAROpenArchiveEx(&open);
    jint result;
    if (!handle || open.OpenResult != ERAR_SUCCESS) {
        result = -(open.OpenResult > 0 ? open.OpenResult : 1);
    } else {
        result = 0;
        RARHeaderDataEx header{};
        while (RARReadHeaderEx(handle, &header) == ERAR_SUCCESS) {
            const std::string name = header.FileNameW[0]
                ? utf16ToUtf8(header.FileNameW)
                : std::string(header.FileName);
            if (name.find("..") != std::string::npos) {
                // 拒绝路径穿越
                RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr);
                continue;
            }
            const std::wstring outPath = destW + L"/" + utf8ToUtf16(name.c_str());
            const int rc = RARProcessFileW(handle, RAR_EXTRACT,
                                           const_cast<wchar_t *>(destW.c_str()),
                                           const_cast<wchar_t *>(outPath.c_str()));
            // 只统计图纸数量（与 zip 路径语义一致）
            const bool drawing = name.size() >= 4 && [&name] {
                const std::string lower = [&name] {
                    std::string s = name;
                    for (char &c : s)
                        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
                    return s;
                }();
                return lower.size() >= 4
                    && (lower.compare(lower.size() - 4, 4, ".dwg") == 0
                        || lower.compare(lower.size() - 4, 4, ".dxf") == 0);
            }();
            if (rc == ERAR_SUCCESS) {
                if (drawing) ++result;
            } else if (rc == ERAR_MISSING_PASSWORD || rc == ERAR_BAD_PASSWORD) {
                result = -100 - rc;
                break;
            }
        }
        RARCloseArchive(handle);
    }
    env->ReleaseStringUTFChars(jArc, arc);
    env->ReleaseStringUTFChars(jDest, dest);
    return result;
}
