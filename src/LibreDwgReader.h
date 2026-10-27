#ifndef CADLENS_LIBREDWGREADER_H
#define CADLENS_LIBREDWGREADER_H

#include "CadModel.h"

bool loadDwgWithLibreDwg(const QString &path, CadDocument &document,
                         QString *errorMessage);
QString cleanDwgTextForDisplay(const QString &text);

#endif
