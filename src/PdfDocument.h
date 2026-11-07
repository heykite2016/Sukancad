#ifndef CADLENS_PDFDOCUMENT_H
#define CADLENS_PDFDOCUMENT_H

#include <QString>

struct CadDocument;
class QImage;

bool loadPdfDocument(const QString &filePath, CadDocument &document,
                     QString *errorMessage);
bool renderPdfPage(const QString &filePath, int pageIndex,
                   int maximumPixelSize, QImage *image,
                   QString *errorMessage);

#endif
