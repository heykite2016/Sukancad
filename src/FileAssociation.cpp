#include "FileAssociation.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#if defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#elif defined(Q_OS_WIN)
#include <QStandardPaths>
#include <shlobj.h>
#include <shlwapi.h>
#include <windows.h>
#endif

#include <array>
#include <string>

namespace {

#if defined(Q_OS_WIN)
QString nativeApplicationPath()
{
    const QFileInfo executable(QCoreApplication::applicationFilePath());
    return QDir::toNativeSeparators(executable.canonicalFilePath());
}
#endif

#if defined(Q_OS_MACOS)
QSettings applicationSettings()
{
    QString directory = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
    if (directory.isEmpty()) {
        directory = QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);
    }
    if (directory.isEmpty()) {
        directory = QDir::tempPath() + QStringLiteral("/CadLens/config");
    }
    QDir().mkpath(directory);
    return QSettings(directory + QStringLiteral("/settings.ini"),
                     QSettings::IniFormat);
}
#endif

#if defined(Q_OS_WIN)
QString progId(FileAssociation::DocumentType type)
{
    switch (type) {
    case FileAssociation::DocumentType::Dwg:
        return QStringLiteral("Sukancad.DWG");
    case FileAssociation::DocumentType::Dxf:
        return QStringLiteral("Sukancad.DXF");
    case FileAssociation::DocumentType::Pdf:
        return QStringLiteral("Sukancad.PDF");
    }
    return QString();
}

QString extension(FileAssociation::DocumentType type)
{
    switch (type) {
    case FileAssociation::DocumentType::Dwg:
        return QStringLiteral(".dwg");
    case FileAssociation::DocumentType::Dxf:
        return QStringLiteral(".dxf");
    case FileAssociation::DocumentType::Pdf:
        return QStringLiteral(".pdf");
    }
    return QString();
}

QString commandForCurrentExecutable()
{
    return QStringLiteral("\"%1\" \"%2\"")
        .arg(nativeApplicationPath(), QStringLiteral("%1"));
}

void registerProgId(QSettings &classes,
                    FileAssociation::DocumentType type)
{
    const QString id = progId(type);
    const QString typeName =
        FileAssociation::documentTypeName(type) + QStringLiteral(" 图纸");
    classes.setValue(id + QStringLiteral("/Application/ApplicationName"),
                     QStringLiteral("速看CAD"));
    classes.setValue(id + QStringLiteral("/DefaultIcon/."),
                     QStringLiteral("\"%1\",%2")
                         .arg(nativeApplicationPath(),
                              type == FileAssociation::DocumentType::Dwg
                                  ? QStringLiteral("2")
                                  : type == FileAssociation::DocumentType::Dxf
                                      ? QStringLiteral("3")
                                      : QStringLiteral("0")));
    classes.setValue(id + QStringLiteral("/shell/open/command/."),
                     commandForCurrentExecutable());
    classes.setValue(id + QStringLiteral("/FriendlyTypeName"),
                     QStringLiteral("速看CAD ") + typeName);
}

bool effectiveHandlerIsCurrentApplication(const QString &suffix)
{
    std::array<wchar_t, MAX_PATH * 4> handlerPath = {};
    DWORD size = DWORD(handlerPath.size());
    const std::wstring extension = suffix.toStdWString();
    const HRESULT result = AssocQueryStringW(
        ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, extension.c_str(),
        L"open", handlerPath.data(), &size);
    if (FAILED(result)) {
        return false;
    }
    const QFileInfo handler(QString::fromWCharArray(handlerPath.data()));
    return handler.canonicalFilePath().compare(
               QCoreApplication::applicationFilePath(),
               Qt::CaseInsensitive)
        == 0;
}

void notifyShell()
{
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
#endif

#if defined(Q_OS_MACOS)
QString macContentType(FileAssociation::DocumentType type)
{
    switch (type) {
    case FileAssociation::DocumentType::Dwg:
        return QStringLiteral("com.sukancad.dwg");
    case FileAssociation::DocumentType::Dxf:
        return QStringLiteral("com.sukancad.dxf");
    case FileAssociation::DocumentType::Pdf:
        return QStringLiteral("com.adobe.pdf");
    }
    return QString();
}

QString currentBundleIdentifier()
{
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (!bundle) {
        return QStringLiteral("com.cadlens.desktop");
    }
    CFStringRef identifier = CFBundleGetIdentifier(bundle);
    if (!identifier) {
        return QStringLiteral("com.cadlens.desktop");
    }
    char buffer[256] = {};
    if (!CFStringGetCString(identifier, buffer, sizeof(buffer),
                            kCFStringEncodingUTF8)) {
        return QStringLiteral("com.cadlens.desktop");
    }
    return QString::fromUtf8(buffer);
}

QString defaultHandler(FileAssociation::DocumentType type)
{
    CFStringRef contentType = CFStringCreateWithCString(
        kCFAllocatorDefault, macContentType(type).toUtf8().constData(),
        kCFStringEncodingUTF8);
    if (!contentType) {
        return QString();
    }
    CFStringRef handler =
        LSCopyDefaultRoleHandlerForContentType(contentType, kLSRolesAll);
    CFRelease(contentType);
    if (!handler) {
        return QString();
    }
    char buffer[512] = {};
    const bool converted = CFStringGetCString(
        handler, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(handler);
    return converted ? QString::fromUtf8(buffer) : QString();
}

bool setMacDefaultHandler(FileAssociation::DocumentType type,
                          const QString &bundleId)
{
    CFStringRef contentType = CFStringCreateWithCString(
        kCFAllocatorDefault, macContentType(type).toUtf8().constData(),
        kCFStringEncodingUTF8);
    CFStringRef handler = CFStringCreateWithCString(
        kCFAllocatorDefault, bundleId.toUtf8().constData(),
        kCFStringEncodingUTF8);
    if (!contentType || !handler) {
        if (contentType) {
            CFRelease(contentType);
        }
        if (handler) {
            CFRelease(handler);
        }
        return false;
    }
    const OSStatus result = LSSetDefaultRoleHandlerForContentType(
        contentType, kLSRolesAll, handler);
    CFRelease(contentType);
    CFRelease(handler);
    return result == noErr;
}

QString previousHandlerKey(FileAssociation::DocumentType type)
{
    return type == FileAssociation::DocumentType::Dwg
        ? QStringLiteral("fileAssociation/previousDwgHandler")
        : QStringLiteral("fileAssociation/previousDxfHandler");
}
#endif

} // namespace

namespace FileAssociation {

QString documentTypeName(DocumentType type)
{
    switch (type) {
    case DocumentType::Dwg:
        return QStringLiteral("DWG");
    case DocumentType::Dxf:
        return QStringLiteral("DXF");
    case DocumentType::Pdf:
        return QStringLiteral("PDF");
    }
    return QString();
}

void registerOpenWith(QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
#if defined(Q_OS_WIN)
    QSettings classes(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
        QSettings::NativeFormat);
    QSettings software(
        QStringLiteral("HKEY_CURRENT_USER\\Software"),
        QSettings::NativeFormat);

    for (DocumentType type : {DocumentType::Dwg, DocumentType::Dxf,
                              DocumentType::Pdf}) {
        const QString suffix = extension(type);
        const QString id = progId(type);
        registerProgId(classes, type);
        classes.setValue(suffix + QStringLiteral("/OpenWithProgids/") + id,
                         QString());
        classes.setValue(suffix + QStringLiteral("/OpenWithList/CadLens.exe"),
                         QString());
    }

    const QString executableName = QStringLiteral("CadLens.exe");
    const QString applicationKey =
        QStringLiteral("Applications/") + executableName;
    classes.setValue(applicationKey + QStringLiteral("/ApplicationName"),
                     QStringLiteral("速看CAD"));
    classes.setValue(applicationKey + QStringLiteral("/shell/open/command/."),
                     commandForCurrentExecutable());
    for (DocumentType type : {DocumentType::Dwg, DocumentType::Dxf,
                              DocumentType::Pdf}) {
        classes.setValue(
            applicationKey + QStringLiteral("/SupportedTypes/")
                + extension(type),
            QString());
    }

    software.setValue(
        QStringLiteral("CADLens/Capabilities/ApplicationName"),
        QStringLiteral("速看CAD"));
    software.setValue(
        QStringLiteral("CADLens/Capabilities/ApplicationDescription"),
        QStringLiteral("DWG、DXF、PDF 工程施工看图与测量"));
    software.setValue(
        QStringLiteral("CADLens/Capabilities/ApplicationIcon"),
        QStringLiteral("\"%1\",0").arg(nativeApplicationPath()));
    software.setValue(
        QStringLiteral("CADLens/Capabilities/FileAssociations/.dwg"),
        progId(DocumentType::Dwg));
    software.setValue(
        QStringLiteral("CADLens/Capabilities/FileAssociations/.dxf"),
        progId(DocumentType::Dxf));
    software.setValue(
        QStringLiteral("CADLens/Capabilities/FileAssociations/.pdf"),
        progId(DocumentType::Pdf));
    software.setValue(
        QStringLiteral("RegisteredApplications/速看CAD"),
        QStringLiteral("Software\\CADLens\\Capabilities"));

    classes.sync();
    software.sync();
    notifyShell();
    if (classes.status() != QSettings::NoError
        || software.status() != QSettings::NoError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法写入 Windows 文件关联注册表。");
        }
    }
#elif defined(Q_OS_MACOS)
    // Launch Services registers the bundle and document types from Info.plist.
    // Refresh it immediately so the app appears in "Open With" without a
    // logout or a manually forced Launch Services database rebuild.
    CFBundleRef bundle = CFBundleGetMainBundle();
    CFURLRef bundleUrl = bundle ? CFBundleCopyBundleURL(bundle) : nullptr;
    if (bundleUrl) {
        LSRegisterURL(bundleUrl, true);
        CFRelease(bundleUrl);
    }
#else
    if (errorMessage) {
        *errorMessage = QStringLiteral("当前平台暂不支持自动注册文件关联。");
    }
#endif
}

ApplyResult setDefault(DocumentType type, bool enabled)
{
    if (type == DocumentType::Pdf) {
        return {false, QStringLiteral("PDF 仅加入打开方式，不修改默认程序。")};
    }

#if defined(Q_OS_WIN)
    QSettings classes(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
        QSettings::NativeFormat);
    const QString suffix = extension(type);
    const QString id = progId(type);
    if (enabled) {
        if (classes.value(suffix + QStringLiteral("/Default")).toString()
            != id) {
            classes.setValue(suffix + QStringLiteral("/Default"), id);
        }
    } else if (classes.value(suffix + QStringLiteral("/Default")).toString()
               == id) {
        classes.remove(suffix + QStringLiteral("/Default"));
    }
    classes.sync();
    notifyShell();

    const bool effective = effectiveHandlerIsCurrentApplication(suffix);
    if (effective == enabled) {
        return {true, QString()};
    }
    if (classes.status() != QSettings::NoError) {
        return {false, QStringLiteral("数据库写入失败，请检查当前用户权限。")};
    }
    return {
        false,
        QStringLiteral("Windows 默认应用保护未允许静默修改。请使用“打开"
                       "系统默认应用设置”为 %1 选择速看CAD。")
            .arg(documentTypeName(type))
    };
#elif defined(Q_OS_MACOS)
    QSettings settings = applicationSettings();
    const QString key = previousHandlerKey(type);
    const QString ours = currentBundleIdentifier();
    if (enabled) {
        const QString current = defaultHandler(type);
        if (!current.isEmpty() && current != ours) {
            settings.setValue(key, current);
        }
        const bool changed = setMacDefaultHandler(type, ours)
            && defaultHandler(type) == ours;
        settings.sync();
        return changed
            ? ApplyResult{true, QString()}
            : ApplyResult{false,
                          QStringLiteral("macOS 未允许修改 %1 的默认程序。")
                              .arg(documentTypeName(type))};
    }

    const QString previous = settings.value(key).toString();
    bool changed = false;
    if (!previous.isEmpty() && previous != ours) {
        changed = setMacDefaultHandler(type, previous)
            && defaultHandler(type) == previous;
    } else {
        changed = defaultHandler(type) != ours;
    }
    if (changed) {
        settings.remove(key);
        settings.sync();
        return {true, QString()};
    }
    return {
        false,
        QStringLiteral("请在 Finder 中右键一个 %1 文件，选择“显示简介”，"
                       "再选择其他“打开方式”并点击“全部更改”。")
            .arg(documentTypeName(type))
    };
#else
    Q_UNUSED(enabled)
    return {false, QStringLiteral("当前平台暂不支持设置默认程序。")};
#endif
}

bool isDefault(DocumentType type)
{
    if (type == DocumentType::Pdf) {
        return false;
    }
#if defined(Q_OS_WIN)
    return effectiveHandlerIsCurrentApplication(extension(type));
#elif defined(Q_OS_MACOS)
    return defaultHandler(type) == currentBundleIdentifier();
#else
    return false;
#endif
}

void openSystemDefaultAppsSettings()
{
#if defined(Q_OS_WIN)
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("ms-settings:defaultapps")));
#elif defined(Q_OS_MACOS)
    // macOS does not expose a stable URL for a single content type. Open the
    // standard Extensions settings page; Finder's Get Info remains the
    // authoritative per-file-type choice.
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("x-apple.systempreferences:com.apple.ExtensionsPreferences")));
#else
    QDesktopServices::openUrl(QUrl(QStringLiteral("settings://")));
#endif
}

} // namespace FileAssociation
