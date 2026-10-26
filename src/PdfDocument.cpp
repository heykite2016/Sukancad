#include "PdfDocument.h"

#include "CadModel.h"

bool loadPdfDocument(const QString &, CadDocument &, QString *errorMessage)
{
    if (errorMessage) {
        *errorMessage = QStringLiteral(
            "当前平台构建尚未启用 PDF 渲染后端。");
    }
    return false;
}

bool renderPdfPage(const QString &, int, int, QImage *,
                   QString *errorMessage)
{
    if (errorMessage) {
        *errorMessage = QStringLiteral(
            "当前平台构建尚未启用 PDF 渲染后端。");
    }
    return false;
}
