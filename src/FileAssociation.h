#ifndef CADLENS_FILEASSOCIATION_H
#define CADLENS_FILEASSOCIATION_H

#include <QString>

namespace FileAssociation {

enum class DocumentType {
    Dwg,
    Dxf,
    Pdf
};

struct ApplyResult {
    bool success = true;
    QString message;
};

// Registers the current executable as a candidate in the system's
// "Open With" list. Existing default applications are not changed.
void registerOpenWith(QString *errorMessage = nullptr);

// Sets or clears this application as the default handler. PDF is deliberately
// not supported here; it is registered only as an available alternative.
ApplyResult setDefault(DocumentType type, bool enabled);

bool isDefault(DocumentType type);

// Opens the operating system's default-app settings when automatic changes
// are blocked by the OS.
void openSystemDefaultAppsSettings();

QString documentTypeName(DocumentType type);

} // namespace FileAssociation

#endif // CADLENS_FILEASSOCIATION_H
