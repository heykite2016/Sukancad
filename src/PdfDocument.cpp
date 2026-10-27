#include "PdfDocument.h"

#include "CadModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QLibrary>

#include <algorithm>
#include <cmath>

#if defined(Q_OS_WIN)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "fpdfview.h"

namespace {

QString pageName(int index)
{
    return QStringLiteral("第%1页").arg(index + 1);
}

QString pdfiumErrorText(unsigned long error)
{
    switch (error) {
    case FPDF_ERR_SUCCESS:
        return QStringLiteral("PDFium 未返回错误信息。");
    case FPDF_ERR_FILE:
        return QStringLiteral("PDF 文件无法读取。");
    case FPDF_ERR_FORMAT:
        return QStringLiteral("PDF 文件格式无效或已损坏。");
    case FPDF_ERR_PASSWORD:
        return QStringLiteral("PDF 文件受密码保护。");
    case FPDF_ERR_SECURITY:
        return QStringLiteral("PDF 使用了不受支持的安全机制。");
    case FPDF_ERR_PAGE:
        return QStringLiteral("PDF 页面不存在或无法读取。");
    default:
        return QStringLiteral("PDF 渲染失败（错误码 %1）。").arg(error);
    }
}

class PdfiumApi {
public:
    bool initialize(QString *errorMessage)
    {
        if (m_ready) {
            return true;
        }
        if (!m_attempted) {
            m_attempted = true;
            load();
        }
        if (!m_ready && errorMessage) {
            *errorMessage = m_error;
        }
        return m_ready;
    }

    QLibrary library;
    decltype(&FPDF_InitLibrary) initLibrary = nullptr;
    decltype(&FPDF_DestroyLibrary) destroyLibrary = nullptr;
    decltype(&FPDF_GetLastError) getLastError = nullptr;
    decltype(&FPDF_LoadDocument) loadDocument = nullptr;
    decltype(&FPDF_CloseDocument) closeDocument = nullptr;
    decltype(&FPDF_GetPageCount) getPageCount = nullptr;
    decltype(&FPDF_LoadPage) loadPage = nullptr;
    decltype(&FPDF_ClosePage) closePage = nullptr;
    decltype(&FPDF_GetPageWidthF) getPageWidth = nullptr;
    decltype(&FPDF_GetPageHeightF) getPageHeight = nullptr;
    decltype(&FPDFBitmap_CreateEx) createBitmap = nullptr;
    decltype(&FPDFBitmap_FillRect) fillBitmap = nullptr;
    decltype(&FPDFBitmap_Destroy) destroyBitmap = nullptr;
    decltype(&FPDF_RenderPageBitmap) renderPage = nullptr;

private:
    template <typename Function>
    bool resolve(Function *function, const char *name)
    {
        const QFunctionPointer address = library.resolve(name);
        if (!address) {
            m_error = QStringLiteral(
                "PDF 运行库缺少入口 %1。").arg(QString::fromLatin1(name));
            return false;
        }
        *function = reinterpret_cast<Function>(address);
        return true;
    }

    void load()
    {
        const QString libraryPath =
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("pdfium.dll"));
        library.setFileName(libraryPath);
        if (!library.load()) {
            m_error = QStringLiteral(
                "无法加载 PDF 运行库：%1").arg(library.errorString());
            return;
        }
        if (!resolve(&initLibrary, "FPDF_InitLibrary")
            || !resolve(&destroyLibrary, "FPDF_DestroyLibrary")
            || !resolve(&getLastError, "FPDF_GetLastError")
            || !resolve(&loadDocument, "FPDF_LoadDocument")
            || !resolve(&closeDocument, "FPDF_CloseDocument")
            || !resolve(&getPageCount, "FPDF_GetPageCount")
            || !resolve(&loadPage, "FPDF_LoadPage")
            || !resolve(&closePage, "FPDF_ClosePage")
            || !resolve(&getPageWidth, "FPDF_GetPageWidthF")
            || !resolve(&getPageHeight, "FPDF_GetPageHeightF")
            || !resolve(&createBitmap, "FPDFBitmap_CreateEx")
            || !resolve(&fillBitmap, "FPDFBitmap_FillRect")
            || !resolve(&destroyBitmap, "FPDFBitmap_Destroy")
            || !resolve(&renderPage, "FPDF_RenderPageBitmap")) {
            library.unload();
            return;
        }
        initLibrary();
        m_ready = true;
    }

    bool m_attempted = false;
    bool m_ready = false;
    QString m_error;
};

PdfiumApi &pdfiumApi()
{
    static PdfiumApi api;
    return api;
}

QImage renderPage(PdfiumApi &api, FPDF_DOCUMENT document,
                  int pageIndex, int maximumPixelSize,
                  QString *errorMessage)
{
    FPDF_PAGE page = api.loadPage(document, pageIndex);
    if (!page) {
        if (errorMessage) {
            *errorMessage = pdfiumErrorText(api.getLastError());
        }
        return {};
    }
    const double pageWidth = api.getPageWidth(page);
    const double pageHeight = api.getPageHeight(page);
    if (!std::isfinite(pageWidth) || !std::isfinite(pageHeight)
        || pageWidth <= 0.0 || pageHeight <= 0.0) {
        api.closePage(page);
        if (errorMessage) {
            *errorMessage = QStringLiteral("PDF 页面尺寸无效。");
        }
        return {};
    }
    const double scale = qMin(
        16.0,
        qMax(1.0, double(maximumPixelSize)
                       / qMax(pageWidth, pageHeight)));
    const QSize pixelSize(
        qBound(1, qRound(pageWidth * scale), 16384),
        qBound(1, qRound(pageHeight * scale), 16384));
    QImage image(pixelSize, QImage::Format_ARGB32);
    image.fill(Qt::white);
    FPDF_BITMAP bitmap = api.createBitmap(
        pixelSize.width(), pixelSize.height(),
        FPDFBitmap_BGRA, image.bits(), image.bytesPerLine());
    if (!bitmap) {
        api.closePage(page);
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法创建 PDF 渲染缓冲区。");
        }
        return {};
    }
    api.fillBitmap(
        bitmap, 0, 0, pixelSize.width(), pixelSize.height(),
        0xffffffff);
    api.renderPage(
        bitmap, page, 0, 0,
        pixelSize.width(), pixelSize.height(),
        0, FPDF_ANNOT);
    api.destroyBitmap(bitmap);
    api.closePage(page);
    return image;
}

} // namespace
#endif

bool loadPdfDocument(const QString &filePath, CadDocument &document,
                     QString *errorMessage)
{
#if defined(Q_OS_WIN)
    PdfiumApi &api = pdfiumApi();
    if (!api.initialize(errorMessage)) {
        return false;
    }
    const QByteArray encodedPath = filePath.toUtf8();
    FPDF_DOCUMENT pdf = api.loadDocument(
        encodedPath.constData(), nullptr);
    if (!pdf) {
        if (errorMessage) {
            *errorMessage = pdfiumErrorText(api.getLastError());
        }
        return false;
    }
    const int pageCount = api.getPageCount(pdf);
    if (pageCount <= 0) {
        api.closeDocument(pdf);
        if (errorMessage) {
            *errorMessage = QStringLiteral("PDF 中没有可显示的页面。");
        }
        return false;
    }

    document.path = filePath;
    document.fileName = QFileInfo(filePath).fileName();
    document.sourceFormat = QStringLiteral("PDF");
    document.sourceBackend = QStringLiteral("PDFium");
    document.formatVersion = QStringLiteral("PDF");
    document.sourceUnit = MeasuringUnit::Millimeter;
    document.metersPerDrawingUnit = 0.001;

    CadLayer pdfLayer;
    pdfLayer.name = QStringLiteral("PDF 页面");
    pdfLayer.color = QColor(Qt::white);
    pdfLayer.sourceColor = pdfLayer.color;
    document.layers.append(pdfLayer);

    for (int index = 0; index < pageCount; ++index) {
        FPDF_PAGE page = api.loadPage(pdf, index);
        if (!page) {
            continue;
        }
        const double width = api.getPageWidth(page);
        const double height = api.getPageHeight(page);
        api.closePage(page);
        if (!std::isfinite(width) || !std::isfinite(height)
            || width <= 0.0 || height <= 0.0) {
            continue;
        }
        CadPdfPage output;
        output.name = pageName(index);
        output.pageRect = QRectF(0.0, 0.0, width, height);
        if (index == 0) {
            output.image = renderPage(
                api, pdf, index, 3200, nullptr);
        }
        document.pdfPages.append(output);
        document.layouts.append(output.name);
        document.layoutEntities.insert(output.name, {});
        document.layoutBounds.insert(output.name, output.pageRect);
    }
    api.closeDocument(pdf);
    if (document.pdfPages.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("PDF 中没有可显示的页面。");
        }
        return false;
    }
    const CadPdfPage &firstPage = document.pdfPages.first();
    document.bounds = firstPage.pageRect;
    document.computedBounds = firstPage.pageRect;
    document.declaredBounds = firstPage.pageRect;
    return true;
#else
    Q_UNUSED(filePath);
    Q_UNUSED(document);
    if (errorMessage) {
        *errorMessage = QStringLiteral(
            "当前平台构建尚未启用 PDF 渲染后端。");
    }
    return false;
#endif
}

bool renderPdfPage(const QString &filePath, int pageIndex,
                   int maximumPixelSize, QImage *image,
                   QString *errorMessage)
{
#if defined(Q_OS_WIN)
    if (!image || pageIndex < 0 || maximumPixelSize < 256) {
        return false;
    }
    PdfiumApi &api = pdfiumApi();
    if (!api.initialize(errorMessage)) {
        return false;
    }
    const QByteArray encodedPath = filePath.toUtf8();
    FPDF_DOCUMENT pdf = api.loadDocument(
        encodedPath.constData(), nullptr);
    if (!pdf) {
        if (errorMessage) {
            *errorMessage = pdfiumErrorText(api.getLastError());
        }
        return false;
    }
    if (pageIndex >= api.getPageCount(pdf)) {
        api.closeDocument(pdf);
        if (errorMessage) {
            *errorMessage = QStringLiteral("PDF 页面不存在。");
        }
        return false;
    }
    *image = renderPage(
        api, pdf, pageIndex, maximumPixelSize, errorMessage);
    api.closeDocument(pdf);
    if (image->isNull()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("PDF 页面渲染失败。");
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(filePath);
    Q_UNUSED(pageIndex);
    Q_UNUSED(maximumPixelSize);
    Q_UNUSED(image);
    if (errorMessage) {
        *errorMessage = QStringLiteral(
            "当前平台构建尚未启用 PDF 渲染后端。");
    }
    return false;
#endif
}
