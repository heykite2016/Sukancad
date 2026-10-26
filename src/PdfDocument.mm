#include "PdfDocument.h"

#include "CadModel.h"

#include <QFileInfo>
#include <QImage>
#include <QtMath>

#import <PDFKit/PDFKit.h>

#include <algorithm>

namespace {

QString pageName(int index)
{
    return QStringLiteral("第%1页").arg(index + 1);
}

QSize displayedPageSize(PDFPage *page)
{
    NSRect bounds = [page boundsForBox:kPDFDisplayBoxCropBox];
    if (NSIsEmptyRect(bounds)) {
        bounds = [page boundsForBox:kPDFDisplayBoxMediaBox];
    }
    double width = NSWidth(bounds);
    double height = NSHeight(bounds);
    const int rotation = (([page rotation] % 360) + 360) % 360;
    if (rotation == 90 || rotation == 270) {
        std::swap(width, height);
    }
    return QSize(qMax(1, qCeil(width)), qMax(1, qCeil(height)));
}

QImage renderPage(PDFPage *page, PDFDocument *document, int pageIndex,
                  int maximumPixelSize)
{
    const QSize pageSize = displayedPageSize(page);
    if (!pageSize.isValid()) {
        return {};
    }
    const double scale = qMin(
        3.0,
        qMax(1.0, double(maximumPixelSize)
                       / qMax(1, qMax(pageSize.width(),
                                      pageSize.height()))));
    const QSize pixelSize(
        qMax(1, qRound(pageSize.width() * scale)),
        qMax(1, qRound(pageSize.height() * scale)));
    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        image.bits(), pixelSize.width(), pixelSize.height(), 8,
        image.bytesPerLine(), colorSpace,
        CGBitmapInfo(kCGBitmapByteOrder32Little)
            | CGBitmapInfo(kCGImageAlphaPremultipliedFirst));
    CGColorSpaceRelease(colorSpace);
    if (!context) {
        return {};
    }
    CGContextSetRGBFillColor(context, 1.0, 1.0, 1.0, 1.0);
    CGContextFillRect(
        context, CGRectMake(0.0, 0.0, pixelSize.width(),
                            pixelSize.height()));
    CGContextSaveGState(context);
    CGContextScaleCTM(context, scale, scale);
    [page drawWithBox:kPDFDisplayBoxCropBox toContext:context];
    Q_UNUSED(document);
    Q_UNUSED(pageIndex);
    CGContextRestoreGState(context);
    CGContextRelease(context);
    return image;
}

} // namespace

bool loadPdfDocument(const QString &filePath, CadDocument &document,
                     QString *errorMessage)
{
    @autoreleasepool {
        PDFDocument *pdf = [[PDFDocument alloc]
            initWithURL:[NSURL fileURLWithPath:
                filePath.toNSString()]];
        if (!pdf || pdf.pageCount <= 0) {
            if (errorMessage) {
                *errorMessage =
                    QStringLiteral("无法读取 PDF，文件可能损坏或受密码保护。");
            }
            return false;
        }

        document.path = filePath;
        document.fileName = QFileInfo(filePath).fileName();
        document.sourceFormat = QStringLiteral("PDF");
        document.sourceBackend = QStringLiteral("PDFKit");
        document.formatVersion = QStringLiteral("PDF");
        document.sourceUnit = MeasuringUnit::Millimeter;
        document.metersPerDrawingUnit = 0.001;

        CadLayer pdfLayer;
        pdfLayer.name = QStringLiteral("PDF 页面");
        pdfLayer.color = QColor(Qt::white);
        document.layers.append(pdfLayer);

        for (NSInteger index = 0; index < pdf.pageCount; ++index) {
            PDFPage *page = [pdf pageAtIndex:index];
            if (!page) {
                continue;
            }
            const QSize size = displayedPageSize(page);
            CadPdfPage output;
            output.name = pageName(int(index));
            output.pageRect = QRectF(0.0, 0.0, size.width(), size.height());
            if (index == 0) {
                output.image =
                    renderPage(page, pdf, int(index), 3200);
            }
            document.pdfPages.append(output);
            document.layouts.append(output.name);
            document.layoutEntities.insert(output.name, {});
            document.layoutBounds.insert(output.name, output.pageRect);
        }
    }

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
}

bool renderPdfPage(const QString &filePath, int pageIndex,
                   int maximumPixelSize, QImage *image,
                   QString *errorMessage)
{
    if (!image || pageIndex < 0 || maximumPixelSize < 256) {
        return false;
    }
    @autoreleasepool {
        PDFDocument *pdf = [[PDFDocument alloc]
            initWithURL:[NSURL fileURLWithPath:
                filePath.toNSString()]];
        if (!pdf || pageIndex >= pdf.pageCount) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("PDF 页面不存在。");
            }
            return false;
        }
        PDFPage *page = [pdf pageAtIndex:pageIndex];
        if (!page) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("PDF 页面无法读取。");
            }
            return false;
        }
        *image = renderPage(page, pdf, pageIndex, maximumPixelSize);
    }
    if (image->isNull()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("PDF 页面渲染失败。");
        }
        return false;
    }
    return true;
}
