#include "CadModel.h"

#include "dx_iface.h"
#include "LibreDwgReader.h"
#include "PdfDocument.h"

#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QtMath>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Affine2D {
    double m11 = 1.0;
    double m12 = 0.0;
    double m21 = 0.0;
    double m22 = 1.0;
    double dx = 0.0;
    double dy = 0.0;

    QPointF map(const QPointF &point) const
    {
        return QPointF(m11 * point.x() + m21 * point.y() + dx,
                       m12 * point.x() + m22 * point.y() + dy);
    }

    Affine2D then(const Affine2D &outer) const
    {
        Affine2D result;
        result.m11 = outer.m11 * m11 + outer.m21 * m12;
        result.m21 = outer.m11 * m21 + outer.m21 * m22;
        result.dx = outer.m11 * dx + outer.m21 * dy + outer.dx;
        result.m12 = outer.m12 * m11 + outer.m22 * m12;
        result.m22 = outer.m12 * m21 + outer.m22 * m22;
        result.dy = outer.m12 * dx + outer.m22 * dy + outer.dy;
        return result;
    }
};

QPointF coord(const DRW_Coord &value)
{
    return QPointF(value.x, value.y);
}

double lineWeightMillimeters(int weight)
{
    static const double weights[] = {
        0.0, 0.05, 0.09, 0.13, 0.15, 0.18, 0.20, 0.25,
        0.30, 0.35, 0.40, 0.50, 0.53, 0.60, 0.70, 0.80,
        0.90, 1.00, 1.06, 1.20, 1.40, 1.58, 2.00, 2.11
    };
    if (weight >= 0 && weight <= 23) {
        return weights[weight];
    }
    return 0.25;
}

int dxfTransparencyAlpha(int transparency)
{
    if (transparency == 0) {
        return 255;
    }
    if (transparency == -1) {
        return 0;
    }
    if ((transparency & 0x02000000) != 0) {
        return qBound(0, transparency & 0xff, 255);
    }
    if (transparency >= 0 && transparency <= 100) {
        return qBound(
            0, 255 - int(std::lround(transparency * 255.0 / 100.0)),
            255);
    }
    return 255;
}

// MTEXT group 41 is the reference rectangle width (used for word wrap),
// NOT the character width factor. libdxfrw routes it through
// DRW_Text::parseCode() into widthscale, so using widthscale squeezes every
// MTEXT to 5% width. The real width factor is the \\W<factor>; inline code.
double mtextWidthScale(const QString &raw)
{
    static const QRegularExpression expression(
        QStringLiteral("\\\\W([0-9]+(?:\\.[0-9]+)?);"));
    const QRegularExpressionMatch match = expression.match(raw);
    if (match.hasMatch()) {
        return qBound(0.05, match.captured(1).toDouble(), 4.0);
    }
    return 1.0;
}

QString cleanMText(QString value)
{
    QString output;
    output.reserve(value.size());
    const int n = value.size();
    int i = 0;
    while (i < n) {
        const QChar ch = value[i];
        if (ch != QLatin1Char('\\')) {
            if (ch != QLatin1Char('{') && ch != QLatin1Char('}')) {
                output.append(ch);
            }
            ++i;
            continue;
        }
        if (i + 1 >= n) {
            break;
        }
        const QChar code = value[++i];
        if (code == QLatin1Char('P')) {
            output.append(QLatin1Char('\n'));
            ++i;
        } else if (code == QLatin1Char('~')) {
            output.append(QLatin1Char(' '));
            ++i;
        } else if (code == QLatin1Char('\\')
                   || code == QLatin1Char('{')
                   || code == QLatin1Char('}')) {
            output.append(code);
            ++i;
        } else if (code == QLatin1Char('U')
                   && i + 1 < n
                   && value[i + 1] == QLatin1Char('+')) {
            // \U+XXXX : BMP Unicode escape (4 hex digits).
            if (i + 5 < n) {
                bool ok = false;
                const ushort v =
                    value.mid(i + 2, 4).toUShort(&ok, 16);
                if (ok) {
                    output.append(QChar(v));
                    i += 6;
                } else {
                    ++i;
                }
            } else {
                ++i;
            }
        } else if (code == QLatin1Char('M')
                   && i + 1 < n
                   && value[i + 1] == QLatin1Char('+')) {
            // \M+XXXXXX : Unicode code point escape (1..6 hex digits),
            // used for characters beyond the BMP (e.g. CJK Ext B). Decode
            // straight to the code point; astral values become a surrogate
            // pair. Without this the raw "\M+5BEC6" text shows verbatim.
            int j = i + 2;
            QString hex;
            while (j < n && hex.size() < 6
                   && (value[j].isDigit()
                       || (value[j] >= QLatin1Char('A')
                           && value[j] <= QLatin1Char('F'))
                       || (value[j] >= QLatin1Char('a')
                           && value[j] <= QLatin1Char('f')))) {
                hex.append(value[j]);
                ++j;
            }
            bool ok = false;
            const uint codePoint = hex.toUInt(&ok, 16);
            if (ok && codePoint > 0) {
                if (codePoint <= 0xFFFF) {
                    output.append(QChar(ushort(codePoint)));
                } else {
                    const char32_t cps[1] = {char32_t(codePoint)};
                    output.append(QString::fromUcs4(cps, 1));
                }
                i = j;
            } else {
                ++i;
            }
        } else {
            // Any other control code \X...; : skip up to and including ';'.
            const int end = value.indexOf(QLatin1Char(';'), i);
            if (end >= 0) {
                i = end + 1;
            } else {
                ++i;
            }
        }
    }
    output.replace(QStringLiteral("%%d"), QStringLiteral("°"));
    output.replace(QStringLiteral("%%D"), QStringLiteral("°"));
    output.replace(QStringLiteral("%%c"), QStringLiteral("⌀"));
    output.replace(QStringLiteral("%%C"), QStringLiteral("⌀"));
    output.replace(QStringLiteral("%%p"), QStringLiteral("±"));
    output.replace(QStringLiteral("%%P"), QStringLiteral("±"));
    return output.trimmed();
}

// DXF TEXT/ATTRIB justification (DXF reference, AcDbText):
//   group 72 = horizontal 0 left, 1 center, 2 right, 3 aligned, 4 middle, 5 fit
//   group 73 (TEXT) / 74 (ATTRIB) = vertical 0 baseline, 1 bottom, 2 middle, 3 top
//   group 10/20 = insertion point, group 11/21 = alignment point.
// Whenever any justification is set, group 11/21 -- NOT the insertion point
// -- is the text anchor. For aligned/fit (3/5) the glyphs are stretched
// between the two points. Drawing at the insertion point instead shifts every
// dimension, room number and title-block label by up to a whole text box,
// which in a layout makes them appear in a neighbouring viewport.
void applyDxfTextJustification(CadEntity &out, const DRW_Text *text)
{
    const int horizontal = int(text->alignH);
    const int vertical = int(text->alignV);
    out.textHAlign = horizontal;
    out.textVAlign = vertical;
    const QPointF insertion = coord(text->basePoint);
    const QPointF alignment = coord(text->secPoint);
    if (horizontal == 3 || horizontal == 5) {
        out.textPosition = insertion;
        out.textAlignmentPoint = alignment;
        out.textFit = true;
        out.textRotation = qRadiansToDegrees(std::atan2(
            alignment.y() - insertion.y(),
            alignment.x() - insertion.x()));
    } else if ((horizontal != 0 || vertical != 0)
               && !alignment.isNull()) {
        out.textPosition = alignment;
    } else {
        out.textPosition = insertion;
    }
    out.points = {out.textPosition};
}

// MTEXT group 71 is the attachment point (1..9, matching DWG's attachment
// field). libdxfrw has no dedicated member: DRW_MText::parseCode() falls
// through to DRW_Text::parseCode(), which stores it in textgen (and
// dxfRW::writeMText writes it back out as group 71 the same way).
// The paragraph code \\A0; bottom / \\A1; center / \\A2; top overrides only
// the vertical part, so combine it with the attachment's column.
int mtextAttachment(const QString &raw, int storedAttachment)
{
    int attachment = storedAttachment;
    if (attachment < 1 || attachment > 9) {
        attachment = 1; // TopLeft, the DXF default
    }
    static const QRegularExpression alignmentCode(
        QStringLiteral("\\\\A([012]);"));
    const QRegularExpressionMatch match = alignmentCode.match(raw);
    if (match.hasMatch()) {
        const int row = 2 - match.captured(1).toInt();
        const int column = (attachment - 1) % 3;
        attachment = row * 3 + column + 1;
    }
    return attachment;
}

void appendBulgeArc(QVector<QPointF> &target, const QPointF &from,
                    const QPointF &to, double bulge)
{
    if (qAbs(bulge) < 1e-9) {
        target.append(to);
        return;
    }
    const QPointF delta = to - from;
    const double chord = std::hypot(delta.x(), delta.y());
    if (chord < 1e-12) {
        target.append(to);
        return;
    }
    const double theta = 4.0 * std::atan(bulge);
    const double half = theta * 0.5;
    const QPointF midpoint = (from + to) * 0.5;
    const QPointF leftNormal(-delta.y() / chord, delta.x() / chord);
    const double offset = chord * 0.5 / std::tan(half);
    const QPointF center = midpoint + leftNormal * offset;
    const double radius = std::hypot(from.x() - center.x(),
                                     from.y() - center.y());
    const double start = std::atan2(from.y() - center.y(),
                                    from.x() - center.x());
    const int steps = qBound(4, int(std::ceil(qAbs(theta) / (kPi / 24.0))),
                             256);
    for (int i = 1; i < steps; ++i) {
        const double angle = start + theta * double(i) / double(steps);
        target.append(center + QPointF(std::cos(angle), std::sin(angle))
                                  * radius);
    }
    target.append(to);
}

QVector<QPointF> sampleArc(const QPointF &center, double radius,
                           double start, double end, bool fullCircle = false)
{
    QVector<QPointF> result;
    if (radius <= 0.0) {
        return result;
    }
    if (fullCircle) {
        start = 0.0;
        end = 2.0 * kPi;
    } else {
        while (end <= start) {
            end += 2.0 * kPi;
        }
    }
    const int steps = qBound(fullCircle ? 32 : 8,
                             int(std::ceil(qAbs(end - start) / (kPi / 48.0))),
                             384);
    result.reserve(steps + (fullCircle ? 1 : 0));
    for (int i = 0; i <= steps; ++i) {
        const double angle = start + (end - start) * double(i) / double(steps);
        result.append(center + QPointF(std::cos(angle), std::sin(angle))
                                  * radius);
    }
    if (fullCircle && !result.isEmpty()) {
        result.removeLast();
    }
    return result;
}

struct CapturedLayout {
    int tabOrder = 0;
    QString name;
    std::uint32_t blockRecordHandle = 0;
};

class ModelBuilder {
public:
    explicit ModelBuilder(CadDocument &document) : m_document(document) {}

    void read(dx_data &data, dx_iface &importer, const QString &path,
              const QStringList &layouts,
              const QVector<CapturedLayout> &layoutRecords)
    {
        m_data = &data;
        m_document.path = path;
        m_document.fileName = QFileInfo(path).fileName();
        m_document.sourceFormat = QFileInfo(path).suffix().toUpper();
        m_document.sourceBackend = QStringLiteral("libdxfrw");
        m_document.formatVersion = versionLabel(data.sourceVersion);
        m_document.layouts = layouts;
        readLayers(data);
        readUnits(data);

        for (auto *block : data.blocks) {
            if (block) {
                m_blocks.insert(QString::fromStdString(block->name), block);
            }
        }
        QHash<std::uint32_t, int> layoutByBlockRecord;
        for (int i = 0; i < layoutRecords.size(); ++i) {
            if (layoutRecords[i].blockRecordHandle != 0) {
                layoutByBlockRecord.insert(
                    layoutRecords[i].blockRecordHandle, i);
            }
        }
        // Paper-space entities belong to exactly one layout; model-space
        // entities belong only to the model. The old loop put every paper
        // entity whose owner did not match a layout record into the model
        // (and then again into a layout fallback), which is one reason DXF
        // viewports showed geometry from unrelated sheets.
        QVector<QVector<CadEntity>> layoutContents(layoutRecords.size());
        QVector<CadEntity> unassignedPaper;
        if (data.mBlock) {
            for (auto *entity : data.mBlock->ent) {
                if (!entity) {
                    continue;
                }
                if (entity->space == DRW::PaperSpace) {
                    // VIEWPORT records are consumed by readViewports().
                    if (entity->eType == DRW::VIEWPORT) {
                        continue;
                    }
                    const int layoutIndex = layoutByBlockRecord.value(
                        entity->parentHandle, -1);
                    m_targetEntities =
                        layoutIndex >= 0 ? &layoutContents[layoutIndex]
                                         : &unassignedPaper;
                } else {
                    m_targetEntities = &m_document.entities;
                }
                visitEntity(entity, Affine2D{}, 0);
            }
        }
        if (!unassignedPaper.isEmpty() && !layoutContents.isEmpty()) {
            layoutContents[0].append(unassignedPaper);
        }
        for (int i = 0; i < layoutRecords.size(); ++i) {
            QVector<CadEntity> &content = layoutContents[i];
            if (content.isEmpty()) {
                continue;
            }
            m_document.layoutEntities.insert(layoutRecords[i].name, content);
            m_document.layoutBounds.insert(
                layoutRecords[i].name, calculateBounds(content));
        }
        readViewports(data, layoutRecords);
        (void)importer;
        finalize();
    }

private:
    void readLayers(dx_data &data)
    {
        for (const auto &source : data.layers) {
            CadLayer layer;
            layer.name = QString::fromStdString(source.name);
            if (layer.name.isEmpty()) {
                layer.name = QStringLiteral("0");
            }
            const int aci = qAbs(source.color);
            // Group 420 == 0 means "no true color" for several CAD
            // exporters; an index colour is still present in group 62.
            layer.color = source.color24 > 0
                ? QColor::fromRgb(source.color24 & 0x00ffffff)
                : aciColor(aci);
            layer.sourceColor = layer.color;
            layer.color = displayColorForDarkCanvas(layer.color);
            // DXF layer records use three independent states:
            //   negative ACI colour -> layer off
            //   flags bit 0         -> frozen
            //   flags bit 1         -> frozen in new viewports
            //   flags bit 2         -> locked (display only, never hidden)
            layer.off = source.color < 0;
            layer.frozen = (source.flags & 1) != 0;
            layer.frozenInNewViewport = (source.flags & 2) != 0;
            layer.visible = !layer.off && !layer.frozen;
            // AutoCAD AUDIT sweeps orphaned entities (furniture, plants,
            // fixtures) onto $TD_AUDIT_GENERATED_* layers, which are never
            // in any viewport freeze list, so they leak into every viewport.
            // Suppress them to match reference viewers (e.g. CAD快速看图).
            if (layer.name.startsWith(QStringLiteral("$TD_AUDIT_GENERATED_"))) {
                layer.visible = false;
            }
            layer.locked = (source.flags & 4) != 0;
            layer.lineWeight = static_cast<int>(source.lWeight);
            m_document.layers.append(layer);
            const int index = m_document.layers.size() - 1;
            m_layerLookup.insert(layer.name, index);
            m_layerHandleToIndex.insert(source.handle, index);
        }
        if (!m_layerLookup.contains(QStringLiteral("0"))) {
            CadLayer layer;
            layer.name = QStringLiteral("0");
            m_document.layers.prepend(layer);
            // Every recorded index shifts by one after the prepend.
            for (auto it = m_layerHandleToIndex.begin();
                 it != m_layerHandleToIndex.end(); ++it) {
                ++(*it);
            }
            rebuildLayerLookup();
        }
    }

    void rebuildLayerLookup()
    {
        m_layerLookup.clear();
        for (int i = 0; i < m_document.layers.size(); ++i) {
            m_layerLookup.insert(m_document.layers[i].name, i);
        }
    }

    int ensureLayer(const QString &name)
    {
        const QString normalized = name.isEmpty() ? QStringLiteral("0") : name;
        const auto found = m_layerLookup.constFind(normalized);
        if (found != m_layerLookup.cend()) {
            return found.value();
        }
        CadLayer layer;
        layer.name = normalized;
        layer.color = aciColor((m_document.layers.size() % 8) + 1);
        layer.sourceColor = layer.color;
        m_document.layers.append(layer);
        const int index = m_document.layers.size() - 1;
        m_layerLookup.insert(normalized, index);
        return index;
    }

    void readUnits(dx_data &data)
    {
        int units = 0;
        const auto unitsVar =
            data.headerC.vars.find(std::string("INSUNITS"));
        if (unitsVar != data.headerC.vars.end() && unitsVar->second
            && (unitsVar->second->type() == DRW_Variant::INTEGER
                || unitsVar->second->type() == DRW_Variant::INTEGER64)) {
            units = int(unitsVar->second->i_val());
        }
        m_document.sourceUnit = measuringUnitFromInsUnits(units);
        m_document.metersPerDrawingUnit =
            measuringUnitMeters(m_document.sourceUnit);
        if (!std::isfinite(m_document.metersPerDrawingUnit)
            || m_document.metersPerDrawingUnit <= 0.0) {
            m_document.sourceUnit = MeasuringUnit::Millimeter;
            m_document.metersPerDrawingUnit = 0.001;
        }
        const auto readCoord = [&data](const char *name, QPointF *point) {
            auto found = data.headerC.vars.find(std::string(name));
            if (found == data.headerC.vars.end() || !found->second
                || found->second->type() != DRW_Variant::COORD
                || !found->second->coord()) {
                return false;
            }
            *point = QPointF(found->second->coord()->x,
                             found->second->coord()->y);
            return true;
        };
        QPointF minimum;
        QPointF maximum;
        if (readCoord("EXTMIN", &minimum)
            && readCoord("EXTMAX", &maximum)) {
            const QRectF extents(minimum, maximum);
            if (extents.isValid() && extents.width() > 0.0
                && extents.height() > 0.0) {
                m_document.declaredBounds = extents.normalized();
            }
        }
    }

    // DXF stores layout viewports as VIEWPORT entities in paper space. The
    // libdxfrw import callback keeps them in the model block, but the old
    // ModelBuilder simply dropped them, leaving every DXF layout viewport
    // empty (layoutViewports was always empty). Build the same
    // CadViewportDefinition the LibreDWG reader produces so the shared
    // layout compositor can map + clip model geometry into each viewport.
    void readViewports(dx_data &data,
                       const QVector<CapturedLayout> &layoutRecords)
    {
        if (!data.mBlock || layoutRecords.isEmpty()) {
            return;
        }
        struct RawViewport {
            std::uint32_t owner = 0;
            int id = 0;
            CadViewportDefinition definition;
        };
        QVector<RawViewport> collected;
        for (const DRW_Entity *entity : data.mBlock->ent) {
            if (!entity || entity->eType != DRW::VIEWPORT) {
                continue;
            }
            const auto *viewport =
                dynamic_cast<const DRW_Viewport *>(entity);
            if (!viewport || viewport->pswidth <= 1.0e-9
                || viewport->psheight <= 1.0e-9
                || viewport->viewHeight <= 1.0e-9) {
                continue;
            }
            const QPointF paperCenter = coord(viewport->basePoint);
            QPointF modelCenter(viewport->centerPX, viewport->centerPY);
            // DXF group 51 is degrees; the compositor works in radians.
            const double twist = qDegreesToRadians(viewport->twistAngle);
            // DXF 12/22 are DCS coordinates; rotate them back by -twist so
            // a twisted viewport looks at the right model region.
            if (qAbs(twist) > 1.0e-12) {
                const double theta = -twist;
                const double cosine = std::cos(theta);
                const double sine = std::sin(theta);
                modelCenter = QPointF(
                    modelCenter.x() * cosine - modelCenter.y() * sine,
                    modelCenter.x() * sine + modelCenter.y() * cosine);
            }
            // DXF viewport IDs are not guaranteed to be unique. A normal
            // model viewport can also report id 1; only the 1:1 paper window
            // should suppress model content.
            const bool paperViewport = isOverallCadViewport(
                paperCenter, modelCenter, viewport->viewHeight,
                viewport->psheight);
            QSet<QString> frozenNames;
            QSet<int> frozenIndices;
            for (const std::uint32_t handle :
                 viewport->frozenLayerHandles) {
                const auto found = m_layerHandleToIndex.constFind(handle);
                if (found != m_layerHandleToIndex.cend()
                    && found.value() >= 0
                    && found.value() < m_document.layers.size()) {
                    frozenIndices.insert(found.value());
                    frozenNames.insert(
                        m_document.layers[found.value()].name);
                }
            }
            // DXF group 68: 0 is off, -1 is on but not the active editing
            // viewport, and positive values are on and active. Non-active
            // viewports still render their model contents in paper space.
            const bool viewportOn = viewport->vpstatus != 0;
            CadViewportDefinition definition;
            definition.id = int(viewport->vpID);
            definition.status = int(viewport->vpstatus);
            definition.on = viewportOn;
            definition.overallPaperViewport = paperViewport;
            definition.center = paperCenter;
            definition.windowSize =
                QSizeF(viewport->pswidth, viewport->psheight);
            definition.modelCenter = modelCenter;
            definition.viewHeight = viewport->viewHeight;
            definition.twist = twist;
            definition.revealsModel = viewportOn && !paperViewport;
            definition.frozenLayers = frozenNames;
            definition.frozenLayerIndices = frozenIndices;
            if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
                std::fprintf(
                    stderr,
                    "[dxfvp] id=%d status=%d paperVp=%d "
                    "reveals=%d frozen=%lld win=%g x %g vh=%g "
                    "mc=%g,%g pc=%g,%g\n",
                    int(viewport->vpID), int(viewport->vpstatus),
                    int(paperViewport),
                    int(viewportOn && !paperViewport),
                    static_cast<long long>(frozenIndices.size()),
                    viewport->pswidth, viewport->psheight,
                    viewport->viewHeight, modelCenter.x(),
                    modelCenter.y(), paperCenter.x(),
                    paperCenter.y());
                if (!frozenNames.isEmpty()) {
                    std::fprintf(stderr, "   frozenNames=%s\n",
                        frozenNames.values().join(QLatin1Char('|'))
                            .toUtf8().constData());
                }
            }
            collected.append(
                {entity->parentHandle, viewport->vpID, definition});
        }
        std::stable_sort(collected.begin(), collected.end(),
                         [](const RawViewport &a, const RawViewport &b) {
                             return a.id < b.id;
                         });
        QVector<QVector<CadViewportDefinition>> perLayout(
            layoutRecords.size());
        int ownerMatches = 0;
        for (const RawViewport &raw : std::as_const(collected)) {
            int target = -1;
            for (int i = 0; i < layoutRecords.size(); ++i) {
                if (layoutRecords[i].blockRecordHandle != 0
                    && layoutRecords[i].blockRecordHandle == raw.owner) {
                    target = i;
                    break;
                }
            }
            if (target >= 0) {
                ++ownerMatches;
            } else {
                // Most DXF files have a single layout; its VIEWPORT owner
                // is the paper-space block record rather than the layout
                // block record handle. Attribute those to the first layout.
                target = 0;
            }
            perLayout[target].append(raw.definition);
        }
        for (int i = 0; i < layoutRecords.size(); ++i) {
            if (!perLayout[i].isEmpty()) {
                m_document.layoutViewports.insert(layoutRecords[i].name,
                                                  perLayout[i]);
            }
        }
        if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
            int total = 0;
            for (const auto &list : std::as_const(perLayout)) {
                total += list.size();
            }
            std::fprintf(stderr,
                         "[dxfvp] collected=%lld ownerMatches=%d layouts=%lld "
                         "viewports=%d\n",
                         static_cast<long long>(collected.size()),
                         ownerMatches,
                         static_cast<long long>(layoutRecords.size()), total);
        }
    }

    // DXF group 42 bulge on a hatch polyline boundary: bulge = tan(sweep/4),
    // positive = counter-clockwise. Sample the circular arc between two
    // vertices so door swings and rounded room corners keep their shape.
    static void appendBulgeArc(QVector<QPointF> &points, const QPointF &a,
                               const QPointF &b, double bulge)
    {
        const double length = QLineF(a, b).length();
        if (qAbs(bulge) <= 1.0e-12 || length <= 1.0e-12) {
            points.append(b);
            return;
        }
        const double theta = 4.0 * std::atan(bulge);
        const QPointF unit((b.x() - a.x()) / length,
                           (b.y() - a.y()) / length);
        const QPointF normal(-unit.y(), unit.x());
        const double cotHalf = (1.0 - bulge * bulge) / (2.0 * bulge);
        const QPointF center =
            (a + b) * 0.5 + normal * (length * 0.5 * cotHalf);
        const double radius = QLineF(center, a).length();
        const double start =
            std::atan2(a.y() - center.y(), a.x() - center.x());
        const int steps = qBound(
            2,
            int(std::ceil(qAbs(theta) / (2.0 * kPi) * 36.0)),
            100);
        for (int i = 1; i <= steps; ++i) {
            const double angle = start + theta * double(i) / double(steps);
            points.append(
                center
                + QPointF(std::cos(angle), std::sin(angle)) * radius);
        }
    }

    CadEntity baseEntity(const DRW_Entity *source, const Affine2D &transform)
    {
        CadEntity output;
        QString layerName = QString::fromStdString(source->layer);
        if (layerName.isEmpty()) {
            layerName = QStringLiteral("0");
        }
        // A block child on layer "0" takes the layer of its INSERT. This
        // is what makes VPLAYER layer freezes apply to block geometry and
        // is also where BYLAYER colour/lineweight come from.
        if (layerName == QStringLiteral("0") && !m_insertLayer.isEmpty()
            && m_insertLayer != QStringLiteral("0")) {
            layerName = m_insertLayer;
        }
        output.layer = layerName;
        const int layerIndex = ensureLayer(output.layer);
        output.layerIndex = layerIndex;
        applyBlockLayerContext(output);
        const CadLayer &cadLayer = m_document.layers[layerIndex];
        const QColor layerSourceColor =
            cadLayer.sourceColor.isValid() ? cadLayer.sourceColor
                                           : cadLayer.color;
        const int indexedColor = qAbs(source->color);
        // Group 420 takes precedence over group 62, but many exporters
        // write 420=0 as "no true colour" together with a real ACI in 62.
        // Treating that 0 as RGB black made whole layers render grey.
        const bool hasTrueColor = source->color24 > 0;
        const bool byLayer = indexedColor == DRW::ColorByLayer;
        const bool byBlock = indexedColor == DRW::ColorByBlock;
        // BYBLOCK resolves to the owning INSERT's colour; when the INSERT
        // itself is BYLAYER that is already the inherited layer colour.
        QColor resolved;
        if (hasTrueColor) {
            resolved = QColor::fromRgb(source->color24 & 0x00ffffff);
        } else if (byBlock) {
            resolved = m_insertSourceColor.isValid()
                           ? m_insertSourceColor : layerSourceColor;
        } else if (byLayer) {
            resolved = layerSourceColor;
        } else {
            resolved = aciColor(indexedColor, layerSourceColor);
        }
        resolved.setAlpha(dxfTransparencyAlpha(source->transparency));
        output.sourceColor = resolved;
        output.color = displayColorForDarkCanvas(output.sourceColor);
        output.lineWeight = source->lWeight == DRW_LW_Conv::widthByLayer
            ? cadLayer.lineWeight
            : static_cast<int>(source->lWeight);
        if (output.lineWeight < 0
            || output.lineWeight > static_cast<int>(
                DRW_LW_Conv::widthByBlock)) {
            output.lineWeight = 31;
        }
        m_document.layers[layerIndex].entityCount++;
        (void)transform;
        return output;
    }

    void applyBlockLayerContext(CadEntity &entity) const
    {
        if (m_blockLayerStack.isEmpty()) {
            return;
        }
        QSet<int> uniqueIndices;
        for (int index : m_blockLayerStack) {
            if (index >= 0 && index < m_document.layers.size()) {
                uniqueIndices.insert(index);
            }
        }
        entity.blockLayerIndices = uniqueIndices.values();
        std::sort(entity.blockLayerIndices.begin(),
                  entity.blockLayerIndices.end());
        entity.blockLayerNames.clear();
        entity.blockLayerNames.reserve(entity.blockLayerIndices.size());
        for (int index : entity.blockLayerIndices) {
            entity.blockLayerNames.append(m_document.layers[index].name);
        }
    }

    void appendEntity(CadEntity entity, const Affine2D &transform)
    {
        const double scaleX = std::hypot(transform.m11, transform.m12);
        const double scaleY = std::hypot(transform.m21, transform.m22);
        const double uniformScale = (scaleX + scaleY) * 0.5;
        for (QPointF &point : entity.points) {
            point = transform.map(point);
        }
        for (CadDimensionEdge &edge : entity.dimensionEdges) {
            edge.midpoint = transform.map(edge.midpoint);
            edge.length *= uniformScale;
        }
        entity.center = transform.map(entity.center);
        entity.textPosition = transform.map(entity.textPosition);
        entity.textAlignmentPoint =
            transform.map(entity.textAlignmentPoint);
        entity.radius *= uniformScale;
        entity.textHeight *= uniformScale;
        if (scaleX > 1.0e-12) {
            entity.textWidthScale *= scaleY / scaleX;
        }
        entity.textRotation +=
            qRadiansToDegrees(std::atan2(transform.m12, transform.m11));
        entity.updateBounds();
        if (!entity.bounds.isValid() || entity.points.isEmpty()
            && entity.type != CadEntityType::Text) {
            return;
        }
        m_targetEntities->append(std::move(entity));
    }

    void visitEntity(const DRW_Entity *source, const Affine2D &transform,
                     int depth)
    {
        if (!source || depth > 24 || !source->visible) {
            return;
        }
        ++m_typeHistogram[int(source->eType)];
        // Attribute definitions inside a block are templates, not visible
        // geometry. Their displayed values are carried by the owning
        // INSERT's attlist and emitted there; drawing the block copy too
        // is what made the same number/text appear many times over.
        if (depth > 0 && (source->eType == DRW::ATTRIB
                          || source->eType == DRW::ATTDEF)) {
            return;
        }
        switch (source->eType) {
        case DRW::LINE:
        case DRW::THREEDLINE: {
            const auto *line = dynamic_cast<const DRW_Line *>(source);
            if (!line) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Segment;
            out.points = {coord(line->basePoint), coord(line->secPoint)};
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::RAY:
        case DRW::XLINE: {
            // Construction rays have no finite model extent. Rendering them
            // with an arbitrary huge length corrupts zoom-to-extents for real
            // drawings, so they are intentionally omitted from this viewer.
            break;
        }
        case DRW::CIRCLE: {
            const auto *circle = dynamic_cast<const DRW_Circle *>(source);
            if (!circle) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Circle;
            out.center = coord(circle->basePoint);
            out.radius = circle->radious;
            out.points = sampleArc(out.center, out.radius, 0.0, 2.0 * kPi,
                                   true);
            out.closed = true;
            if (out.radius > 0.0) {
                out.dimensionEdges.append(
                    {out.center + QPointF(out.radius, 0.0),
                     2.0 * kPi * out.radius, true});
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::ARC: {
            const auto *arc = dynamic_cast<const DRW_Arc *>(source);
            if (!arc) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Arc;
            out.center = coord(arc->basePoint);
            out.radius = arc->radious;
            out.startAngle = arc->staangle;
            out.endAngle = arc->endangle;
            out.points = sampleArc(out.center, out.radius, out.startAngle,
                                   out.endAngle);
            double sweep = out.endAngle - out.startAngle;
            while (sweep <= 0.0) {
                sweep += 2.0 * kPi;
            }
            if (out.radius > 0.0) {
                const double middle =
                    out.startAngle + sweep * 0.5;
                out.dimensionEdges.append(
                    {out.center
                         + QPointF(std::cos(middle), std::sin(middle))
                             * out.radius,
                     out.radius * sweep, true});
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::ELLIPSE: {
            const auto *ellipse = dynamic_cast<const DRW_Ellipse *>(source);
            if (!ellipse) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Ellipse;
            const QPointF center = coord(ellipse->basePoint);
            const QPointF major = coord(ellipse->secPoint);
            const QPointF minor(-major.y() * ellipse->ratio,
                                major.x() * ellipse->ratio);
            double start = ellipse->staparam;
            double end = ellipse->endparam;
            if (qAbs(end - start) < 1e-10) {
                start = 0.0;
                end = 2.0 * kPi;
                out.closed = true;
            } else {
                while (end <= start) {
                    end += 2.0 * kPi;
                }
            }
            const int steps = qBound(16, int(std::ceil(
                qAbs(end - start) / (kPi / 48.0))), 384);
            for (int i = 0; i <= steps; ++i) {
                const double t = start + (end - start) * double(i) / steps;
                out.points.append(center + major * std::cos(t)
                                  + minor * std::sin(t));
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::LWPOLYLINE: {
            const auto *poly = dynamic_cast<const DRW_LWPolyline *>(source);
            if (!poly || poly->vertlist.empty()) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Polyline;
            out.closed = (poly->flags & 1) != 0;
            QPointF previous;
            bool havePrevious = false;
            double previousBulge = 0.0;
            for (const auto &vertex : poly->vertlist) {
                if (!vertex) {
                    continue;
                }
                const QPointF current(vertex->x, vertex->y + poly->elevation);
                if (!havePrevious) {
                    out.points.append(current);
                } else {
                    if (qAbs(previousBulge) > 1e-9) {
                        const QPointF delta = current - previous;
                        const double chord =
                            std::hypot(delta.x(), delta.y());
                        const double theta = 4.0 * std::atan(previousBulge);
                        const QPointF midpoint = (previous + current) * 0.5;
                        const QPointF normal(-delta.y() / chord,
                                             delta.x() / chord);
                        const QPointF center =
                            midpoint
                            + normal * (chord * 0.5
                                        / std::tan(theta * 0.5));
                        const double radius =
                            std::hypot(previous.x() - center.x(),
                                       previous.y() - center.y());
                        const double start = std::atan2(
                            previous.y() - center.y(),
                            previous.x() - center.x());
                        const double middle = start + theta * 0.5;
                        out.dimensionEdges.append(
                            {center
                                 + QPointF(std::cos(middle),
                                           std::sin(middle))
                                     * radius,
                             radius * qAbs(theta), true});
                    } else {
                        out.dimensionEdges.append(
                            {(previous + current) * 0.5,
                             QLineF(previous, current).length(), false});
                    }
                    appendBulgeArc(out.points, previous, current,
                                   previousBulge);
                }
                previous = current;
                previousBulge = vertex->bulge;
                havePrevious = true;
            }
            if (out.closed && out.points.size() > 2) {
                const double closingBulge =
                    poly->vertlist.back() ? poly->vertlist.back()->bulge : 0.0;
                if (!out.points.isEmpty()) {
                    if (qAbs(closingBulge) > 1e-9) {
                        const QPointF delta =
                            out.points.first() - out.points.last();
                        const double chord =
                            std::hypot(delta.x(), delta.y());
                        const double theta =
                            4.0 * std::atan(closingBulge);
                        const QPointF midpoint =
                            (out.points.last() + out.points.first())
                            * 0.5;
                        const QPointF normal(-delta.y() / chord,
                                             delta.x() / chord);
                        const QPointF center =
                            midpoint
                            + normal * (chord * 0.5
                                        / std::tan(theta * 0.5));
                        const double radius =
                            std::hypot(out.points.last().x() - center.x(),
                                       out.points.last().y() - center.y());
                        const double start = std::atan2(
                            out.points.last().y() - center.y(),
                            out.points.last().x() - center.x());
                        const double middle = start + theta * 0.5;
                        out.dimensionEdges.append(
                            {center
                                 + QPointF(std::cos(middle),
                                           std::sin(middle))
                                     * radius,
                             radius * qAbs(theta), true});
                    } else {
                        out.dimensionEdges.append(
                            {(out.points.last() + out.points.first())
                                 * 0.5,
                             QLineF(out.points.last(), out.points.first())
                                 .length(),
                             false});
                    }
                }
                if (qAbs(closingBulge) > 1e-9) {
                    appendBulgeArc(out.points, out.points.back(),
                                   out.points.first(), closingBulge);
                }
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::POLYLINE: {
            const auto *poly = dynamic_cast<const DRW_Polyline *>(source);
            if (!poly || poly->vertlist.empty()) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Polyline;
            out.closed = (poly->flags & 1) != 0;
            QPointF previous;
            bool havePrevious = false;
            for (const auto &vertex : poly->vertlist) {
                if (!vertex) {
                    continue;
                }
                const QPointF current = coord(vertex->basePoint);
                if (!havePrevious) {
                    out.points.append(current);
                } else {
                    appendBulgeArc(out.points, previous, current,
                                   vertex->bulge);
                }
                previous = current;
                havePrevious = true;
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::SPLINE: {
            const auto *spline = dynamic_cast<const DRW_Spline *>(source);
            if (!spline) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Polyline;
            if (!spline->fitlist.empty()) {
                for (const auto &point : spline->fitlist) {
                    if (point) {
                        out.points.append(coord(*point));
                    }
                }
            } else {
                for (const auto &point : spline->controllist) {
                    if (point) {
                        out.points.append(coord(*point));
                    }
                }
            }
            out.closed = (spline->flags & 1) != 0;
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::POINT: {
            // POINT is commonly a dimension definition or block control
            // point. It is not drawing geometry unless the viewer implements
            // PDMODE/PDSIZE display semantics.
            break;
        }
        case DRW::TEXT:
        case DRW::ATTRIB: {
            const auto *text = dynamic_cast<const DRW_Text *>(source);
            if (!text) {
                break;
            }
            QString textValue = cleanMText(
                QString::fromStdString(text->text));
            double widthScale = qMax(0.05, text->widthscale);
            if (source->eType == DRW::ATTRIB) {
                const auto *attrib =
                    dynamic_cast<const DRW_Attrib *>(source);
                if (!attrib || (attrib->attribFlags & 1) != 0) {
                    // Group 70 bit 1 = invisible attribute.
                    break;
                }
                if (attrib->mtext
                    && !attrib->mtext->text.empty()) {
                    const QString raw =
                        QString::fromStdString(attrib->mtext->text);
                    textValue = cleanMText(raw);
                    widthScale = mtextWidthScale(raw);
                }
            }
            if (textValue.isEmpty()) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Text;
            out.text = textValue;
            out.textHeight = qMax(0.1, text->height);
            out.textRotation = text->angle;
            out.textWidthScale = widthScale;
            applyDxfTextJustification(out, text);
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::MTEXT: {
            const auto *text = dynamic_cast<const DRW_MText *>(source);
            if (!text) {
                break;
            }
            const QString raw = QString::fromStdString(text->text);
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Text;
            out.text = cleanMText(raw);
            out.textPosition = coord(text->basePoint);
            out.points = {out.textPosition};
            out.textHeight = qMax(0.1, text->height);
            // MTEXT group 11/21 is the X-axis direction vector, not an
            // alignment point; libdxfrw's updateAngle() has already folded
            // it into angle via atan2(secPoint.y, secPoint.x).
            out.textRotation = text->angle;
            out.textWidthScale = mtextWidthScale(raw);
            out.textAttachment = mtextAttachment(raw, text->textgen);
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::SOLID:
        case DRW::E3DFACE:
        case DRW::DXF_TRACE: {
            const auto *face = dynamic_cast<const DRW_Trace *>(source);
            if (!face) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Solid;
            out.filled = true;
            out.closed = true;
            out.points = {
                coord(face->basePoint), coord(face->secPoint),
                coord(face->fourPoint), coord(face->thirdPoint)
            };
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::HATCH:
        case DRW::MPOLYGON: {
            const auto *hatch = dynamic_cast<const DRW_Hatch *>(source);
            if (!hatch) {
                break;
            }
            QVector<QVector<QPointF>> loops;
            for (const auto &loop : hatch->looplist) {
                if (!loop) {
                    continue;
                }
                // Group 92 bit 0x20 marks an open boundary path: it is not
                // part of the fill region.
                const bool openLoop = (loop->type & 0x20) != 0;
                QVector<QPointF> loopPoints;
                for (const auto &edge : loop->objlist) {
                    if (!edge) {
                        continue;
                    }
                    if (const auto *line =
                            dynamic_cast<const DRW_Line *>(edge.get())) {
                        if (loopPoints.isEmpty()
                            || QLineF(loopPoints.last(),
                                      coord(line->basePoint)).length()
                                > 1.0e-9) {
                            loopPoints.append(coord(line->basePoint));
                        }
                        loopPoints.append(coord(line->secPoint));
                    } else if (const auto *arc =
                                   dynamic_cast<const DRW_Arc *>(edge.get())) {
                        // HATCH boundary arcs with isccw=0 are measured in
                        // the clockwise direction; negate their angles to
                        // get true mathematical angles (same rule as the
                        // LibreDWG reader, verified on the DXF sibling of
                        // the same drawing). Otherwise the sampled arc does
                        // not meet its neighbouring edges and the loop
                        // fills as a blob.
                        const bool ccw = arc->isccw != 0;
                        double start =
                            ccw ? arc->staangle : -arc->staangle;
                        double end = ccw ? arc->endangle : -arc->endangle;
                        double sweep = end - start;
                        if (ccw) {
                            while (sweep <= 0.0) {
                                sweep += 2.0 * kPi;
                            }
                        } else {
                            while (sweep >= 0.0) {
                                sweep -= 2.0 * kPi;
                            }
                        }
                        const QPointF center = coord(arc->basePoint);
                        const int steps = qBound(
                            4, int(std::ceil(qAbs(sweep) / (kPi / 24.0))),
                            256);
                        for (int i = 0; i <= steps; ++i) {
                            const double angle =
                                start + sweep * double(i) / double(steps);
                            const QPointF point =
                                center
                                + QPointF(std::cos(angle), std::sin(angle))
                                      * arc->radious;
                            if (loopPoints.isEmpty()
                                || QLineF(loopPoints.last(), point).length()
                                    > 1.0e-9) {
                                loopPoints.append(point);
                            }
                        }
                    } else if (const auto *ellipse =
                                   dynamic_cast<const DRW_Ellipse *>(
                                       edge.get())) {
                        const QPointF center = coord(ellipse->basePoint);
                        const QPointF major = coord(ellipse->secPoint);
                        const QPointF minor(
                            -major.y() * ellipse->ratio,
                            major.x() * ellipse->ratio);
                        double start = ellipse->staparam;
                        double end = ellipse->endparam;
                        if (qAbs(end - start) < 1.0e-10) {
                            start = 0.0;
                            end = 2.0 * kPi;
                        } else {
                            while (end <= start) {
                                end += 2.0 * kPi;
                            }
                        }
                        const int steps = qBound(
                            16, int(std::ceil((end - start)
                                              / (kPi / 48.0))),
                            384);
                        for (int i = 0; i <= steps; ++i) {
                            const double t =
                                start + (end - start) * double(i)
                                / double(steps);
                            loopPoints.append(
                                center + major * std::cos(t)
                                + minor * std::sin(t));
                        }
                    } else if (const auto *polyline =
                                   dynamic_cast<const DRW_LWPolyline *>(
                                       edge.get())) {
                        bool first = true;
                        QPointF previous;
                        double previousBulge = 0.0;
                        for (const auto &vertex : polyline->vertlist) {
                            if (!vertex) {
                                continue;
                            }
                            const QPointF current(vertex->x, vertex->y);
                            if (first) {
                                if (loopPoints.isEmpty()
                                    || QLineF(loopPoints.last(), current)
                                           .length()
                                        > 1.0e-9) {
                                    loopPoints.append(current);
                                }
                                first = false;
                            } else {
                                appendBulgeArc(loopPoints, previous, current,
                                               previousBulge);
                            }
                            previous = current;
                            previousBulge = vertex->bulge;
                        }
                        if (!first && (polyline->flags & 1) != 0
                            && qAbs(previousBulge) > 1.0e-10
                            && !polyline->vertlist.empty()) {
                            const auto &firstVertex =
                                polyline->vertlist.front();
                            if (firstVertex) {
                                appendBulgeArc(
                                    loopPoints, previous,
                                    QPointF(firstVertex->x, firstVertex->y),
                                    previousBulge);
                            }
                        }
                    }
                }
                if (!openLoop && loopPoints.size() > 2) {
                    if (QLineF(loopPoints.first(), loopPoints.last()).length()
                        < 1.0e-9) {
                        loopPoints.removeLast();
                    }
                    loops.append(loopPoints);
                }
            }
            if (loops.isEmpty()) {
                break;
            }
            std::sort(loops.begin(), loops.end(),
                      [](const QVector<QPointF> &a,
                         const QVector<QPointF> &b) {
                          QRectF ba;
                          QRectF bb;
                          for (const QPointF &point : a) {
                              ba = ba.united(QRectF(point, point));
                          }
                          for (const QPointF &point : b) {
                              bb = bb.united(QRectF(point, point));
                          }
                          return ba.width() * ba.height()
                              > bb.width() * bb.height();
                      });
            // DXF HATCH pattern definition lines (group 53/43/44/45/46/49)
            // are already expanded into absolute drawing coordinates: the
            // line angle includes group 52 and the offset already includes
            // group 41 scale/rotation. Use them as-is, like libredwg's
            // deflines on the DWG path.
            auto pattern = std::make_shared<CadHatchPattern>();
            pattern->name = QString::fromStdString(hatch->name);
            pattern->solid = hatch->solid != 0;
            pattern->gradient = hatch->isGradient != 0;
            pattern->style = hatch->hstyle;
            pattern->lines.reserve(
                qMin<qsizetype>(hatch->patternLines.size(), 65536));
            for (const auto &definition : hatch->patternLines) {
                CadHatchPatternLine line;
                line.origin =
                    QPointF(definition.baseX, definition.baseY);
                const double angle =
                    qDegreesToRadians(definition.angle);
                line.direction =
                    QPointF(std::cos(angle), std::sin(angle));
                line.offset =
                    QPointF(definition.offsetX, definition.offsetY);
                for (double dash : definition.dashList) {
                    line.dashes.append(dash);
                }
                pattern->lines.append(std::move(line));
                if (pattern->lines.size() >= 65536) {
                    break;
                }
            }
            // Every closed boundary loop of one HATCH shares an odd-even
            // fill group, so islands cut holes and the pattern is clipped
            // once against the whole region (instead of filling only the
            // largest loop with an opaque colour).
            const int hatchGroup = m_nextHatchGroup++;
            for (const QVector<QPointF> &loopPoints :
                 std::as_const(loops)) {
                CadEntity out = baseEntity(source, transform);
                out.type = CadEntityType::Solid;
                out.closed = true;
                out.filled = pattern->solid;
                out.hatchGroup = hatchGroup;
                out.hatchPattern = pattern;
                out.points = loopPoints;
                appendEntity(std::move(out), transform);
            }
            break;
        }
        case DRW::DIMENSION:
        case DRW::DIMALIGNED:
        case DRW::DIMLINEAR:
        case DRW::DIMRADIAL:
        case DRW::DIMDIAMETRIC:
        case DRW::DIMANGULAR:
        case DRW::DIMANGULAR3P:
        case DRW::DIMORDINATE:
        case DRW::DIMARC: {
            // A DXF DIMENSION keeps its drawn geometry (extension lines,
            // arrows, text) in an anonymous block named by group 2. Expand
            // that block in place; without it every dimension in a drawing
            // silently disappears.
            const auto *dimension =
                dynamic_cast<const DRW_Dimension *>(source);
            if (!dimension || depth > 20) {
                break;
            }
            const QString blockName = QString::fromStdString(
                const_cast<DRW_Dimension *>(dimension)->getName());
            if (blockName.isEmpty()) {
                break;
            }
            const auto blockIt = m_blocks.constFind(blockName);
            if (blockIt == m_blocks.cend() || !blockIt.value()) {
                ++m_dimensionBlocksMissing;
                break;
            }
            ++m_dimensionBlocksExpanded;
            for (auto *child : blockIt.value()->ent) {
                visitEntity(child, transform, depth + 1);
            }
            break;
        }
        case DRW::LEADER: {
            const auto *leader =
                dynamic_cast<const DRW_Leader *>(source);
            if (!leader || leader->vertexlist.size() < 2) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Polyline;
            for (const auto &vertex : leader->vertexlist) {
                if (vertex) {
                    out.points.append(coord(*vertex));
                }
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::INSERT: {
            const auto *insert = dynamic_cast<const DRW_Insert *>(source);
            if (!insert) {
                break;
            }
            // CAD layer/colour inheritance for block references. A child on
            // layer "0" takes this INSERT's layer, and a BYBLOCK child takes
            // this INSERT's colour. getLayer/ByLayer semantics are otherwise
            // impossible to reproduce and VPLAYER freezes silently fail.
            struct InsertContextGuard {
                QString *layer;
                QColor *color;
                QVector<int> *blockLayerStack;
                QString savedLayer;
                QColor savedColor;
                qsizetype savedStackSize;
                ~InsertContextGuard() {
                    *layer = savedLayer;
                    *color = savedColor;
                    blockLayerStack->resize(savedStackSize);
                }
            };
            const QString rawLayer = QString::fromStdString(insert->layer);
            const QString inheritedLayer =
                (rawLayer.isEmpty() || rawLayer == QStringLiteral("0"))
                    ? m_insertLayer : rawLayer;
            const QString effectiveLayer = inheritedLayer.isEmpty()
                ? QStringLiteral("0") : inheritedLayer;
            const int insertLayerIndex = ensureLayer(effectiveLayer);
            const CadLayer &insertCadLayer =
                m_document.layers[insertLayerIndex];
            QColor insertColor;
            if (insert->color24 > 0) {
                insertColor = QColor::fromRgb(insert->color24 & 0x00ffffff);
            } else {
                const int aci = qAbs(insert->color);
                if (aci == DRW::ColorByLayer || aci == DRW::ColorByBlock) {
                    insertColor = insertCadLayer.sourceColor.isValid()
                        ? insertCadLayer.sourceColor : insertCadLayer.color;
                } else {
                    insertColor = aciColor(aci);
                }
            }
            InsertContextGuard guard{
                &m_insertLayer, &m_insertSourceColor,
                &m_blockLayerStack, m_insertLayer,
                m_insertSourceColor, m_blockLayerStack.size()};
            m_insertLayer = effectiveLayer;
            m_insertSourceColor = insertColor;
            m_blockLayerStack.append(insertLayerIndex);
            if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")
                && insert->attlist.size() >= 8) {
                int ones = 0;
                for (const auto &candidate : insert->attlist) {
                    if (candidate && candidate->text == "01") {
                        ++ones;
                    }
                }
                std::fprintf(
                    stderr,
                    "[insert] name='%s' layer='%s' attrs=%lld ones=%d "
                    "pos=(%.1f,%.1f) cols=%d rows=%d\n",
                    insert->name.c_str(), insert->layer.c_str(),
                    static_cast<long long>(insert->attlist.size()), ones,
                    insert->basePoint.x, insert->basePoint.y,
                    insert->colcount, insert->rowcount);
            }
            // DXF keeps ATTRIB values on their owning INSERT (attlist), not
            // as standalone entities. These are the room numbers, labels
            // and title-block texts; dropping them leaves a drawing with
            // almost no text. Their coordinates are in the INSERT's parent
            // space, so use the incoming transform, not the block transform.
            QSet<QString> emittedAttributes;
            for (const auto &attribute : insert->attlist) {
                if (!attribute || (attribute->attribFlags & 1) != 0) {
                    continue;
                }
                QString value = QString::fromStdString(attribute->text);
                // An ATTRIB with an embedded MTEXT has its group 41 routed
                // through DRW_Text too, where it is the rectangle width.
                // The ATTRIB's own width factor is only trustworthy when
                // there is no embedded MTEXT.
                double widthScale = qMax(0.05, attribute->widthscale);
                if (attribute->mtext
                    && !attribute->mtext->text.empty()) {
                    const QString raw = QString::fromStdString(
                        attribute->mtext->text);
                    value = cleanMText(raw);
                    widthScale = mtextWidthScale(raw);
                }
                if (value.isEmpty()) {
                    continue;
                }
                CadEntity out = baseEntity(attribute.get(), transform);
                out.type = CadEntityType::Text;
                out.text = value;
                out.textHeight = qMax(0.1, attribute->height);
                out.textRotation = attribute->angle;
                out.textWidthScale = widthScale;
                // ATTRIB group 72=4 (middle) with group 11/21 alignment
                // point is the normal case for title-block and room labels.
                // Honour it exactly like a standalone TEXT entity.
                applyDxfTextJustification(out, attribute.get());
                // Tianzheng/AUDIT exports can repeat the same visible
                // attribute value with different tags (20 copies of "01"
                // observed). Deduplicate on the FINAL drawn anchor: once
                // justification is honoured, two attributes with different
                // insertion points can resolve to the same alignment point,
                // so keying on basePoint would let the glyphs overlap.
                const QString identity =
                    value + QLatin1Char('|')
                    + QString::number(out.textPosition.x(), 'f', 3)
                    + QLatin1Char('|')
                    + QString::number(out.textPosition.y(), 'f', 3)
                    + QLatin1Char('|')
                    + QString::number(out.textHeight, 'f', 3)
                    + QLatin1Char('|')
                    + QString::number(out.textRotation, 'f', 3);
                if (emittedAttributes.contains(identity)) {
                    continue;
                }
                emittedAttributes.insert(identity);
                appendEntity(std::move(out), transform);
            }
            const auto blockIt =
                m_blocks.constFind(QString::fromStdString(insert->name));
            if (blockIt == m_blocks.cend() || !blockIt.value()) {
                break;
            }
            const auto *block = blockIt.value();
            const double cosine = std::cos(insert->angle);
            const double sine = std::sin(insert->angle);
            Affine2D local;
            local.m11 = cosine * insert->xscale;
            local.m12 = sine * insert->xscale;
            local.m21 = -sine * insert->yscale;
            local.m22 = cosine * insert->yscale;
            local.dx = insert->basePoint.x
                - local.m11 * block->basePoint.x
                - local.m21 * block->basePoint.y;
            local.dy = insert->basePoint.y
                - local.m12 * block->basePoint.x
                - local.m22 * block->basePoint.y;
            const Affine2D combined = local.then(transform);
            const int columns = qBound(1, insert->colcount, 256);
            const int rows = qBound(1, insert->rowcount, 256);
            for (int row = 0; row < rows; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const double localX = column * insert->colspace;
                    const double localY = row * insert->rowspace;
                    Affine2D arrayOffset;
                    arrayOffset.dx = cosine * localX - sine * localY;
                    arrayOffset.dy = sine * localX + cosine * localY;
                    const Affine2D arrayTransform = arrayOffset.then(combined);
                    for (auto *child : block->ent) {
                        visitEntity(child, arrayTransform, depth + 1);
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }

    void finalize()
    {
        m_document.sanitizeRenderableEntities();
        QRectF bounds = m_document.declaredBounds.isValid()
            ? m_document.declaredBounds
            : calculateBounds(m_document.entities);
        if (!bounds.isValid() || bounds.width() < 1e-9
            || bounds.height() < 1e-9) {
            const double side = qMax(1.0, bounds.width());
            bounds = QRectF(bounds.center().x() - side,
                            bounds.center().y() - side, side * 2.0,
                            side * 2.0);
        }
        m_document.bounds = bounds;
        m_document.recountLayers();
        if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
            QVector<QPair<int, int>> histogram;
            histogram.reserve(m_typeHistogram.size());
            for (auto it = m_typeHistogram.cbegin();
                 it != m_typeHistogram.cend(); ++it) {
                histogram.append({it.key(), it.value()});
            }
            std::sort(histogram.begin(), histogram.end(),
                      [](const QPair<int, int> &a,
                         const QPair<int, int> &b) {
                          return a.second > b.second;
                      });
            QString types;
            for (int i = 0; i < qMin(histogram.size(), 24); ++i) {
                types += QStringLiteral("%1:%2 ")
                             .arg(histogram[i].first)
                             .arg(histogram[i].second);
            }
            std::fprintf(stderr,
                         "[dxftypes] %s dimensionBlocks=%lld/%lld\n",
                         types.toUtf8().constData(),
                         static_cast<long long>(m_dimensionBlocksExpanded),
                         static_cast<long long>(m_dimensionBlocksMissing));
        }
    }

    static QRectF calculateBounds(const QVector<CadEntity> &entities)
    {
        QRectF bounds;
        for (const CadEntity &entity : entities) {
            if (!entity.bounds.isValid()) {
                continue;
            }
            bounds = bounds.isNull() ? entity.bounds
                                     : bounds.united(entity.bounds);
        }
        if (!bounds.isValid()) {
            return bounds;
        }
        return bounds.adjusted(-bounds.width() * 0.03,
                               -bounds.height() * 0.03,
                               bounds.width() * 0.03,
                               bounds.height() * 0.03);
    }

    static QString versionLabel(DRW::Version version)
    {
        for (const auto &entry : DRW::dwgVersionStrings) {
            if (entry.second == version && entry.first) {
                return QString::fromLatin1(entry.first);
            }
        }
        return QStringLiteral("AutoCAD");
    }

    CadDocument &m_document;
    QVector<CadEntity> *m_targetEntities = &m_document.entities;
    dx_data *m_data = nullptr;
    QHash<QString, dx_ifaceBlock *> m_blocks;
    QHash<QString, int> m_layerLookup;
    QHash<std::uint32_t, int> m_layerHandleToIndex;
    QHash<int, int> m_typeHistogram;
    qsizetype m_dimensionBlocksExpanded = 0;
    qsizetype m_dimensionBlocksMissing = 0;
    int m_nextHatchGroup = 0;
    // CAD semantics: entities on layer "0" inside a block inherit the
    // layer (and therefore the colour, lineweight and VPLAYER visibility)
    // of the INSERT that references the block. Without this every block
    // child lands on layer "0", so viewport-specific layer freezes miss
    // almost all of them and geometry leaks into the wrong viewports.
    QString m_insertLayer;
    QColor m_insertSourceColor;
    QVector<int> m_blockLayerStack;
};

class LayoutAwareInterface : public dx_iface {
public:
    void addBlock(const DRW_Block &block) override
    {
        dx_iface::addBlock(block);
        if (currentBlock) {
            blocksByHandle.insert(static_cast<int>(block.handle),
                                  currentBlock);
        }
    }

    void setBlock(const int handle) override
    {
        const auto found = blocksByHandle.constFind(handle);
        if (found != blocksByHandle.cend()) {
            currentBlock = found.value();
        } else {
            currentBlock = cData ? cData->mBlock : nullptr;
        }
    }

    void addLayout(const DRW_Layout &layout) override
    {
        const QString name = QString::fromStdString(layout.name).trimmed();
        if (!name.isEmpty() && name.compare(QStringLiteral("Model"),
                                           Qt::CaseInsensitive)
                != 0
            && std::none_of(layoutRecords.cbegin(), layoutRecords.cend(),
                            [&name](const CapturedLayout &record) {
                                return record.name == name;
                            })) {
            layoutRecords.append(
                {layout.tabOrder, name, layout.paperSpaceBlockRecordHandle.ref});
        }
    }

    void finishLayouts()
    {
        std::sort(layoutRecords.begin(), layoutRecords.end(),
                  [](const auto &a, const auto &b) {
                      if (a.tabOrder != b.tabOrder) {
                          return a.tabOrder < b.tabOrder;
                      }
                      return a.name.size() < b.name.size();
                  });
        layouts.clear();
        for (const auto &record : layoutRecords) {
            layouts.append(record.name);
        }
    }

    QStringList layouts;
    QVector<CapturedLayout> layoutRecords;
    QHash<int, dx_ifaceBlock *> blocksByHandle;
};

// Returns true only when the buffer contains non-ASCII bytes that form a
// strictly valid UTF-8 stream. Pure ASCII and legacy DBCS (GBK/Big5/...)
// buffers both return false.
bool isNonAsciiUtf8(const QByteArray &bytes)
{
    const auto *data =
        reinterpret_cast<const unsigned char *>(bytes.constData());
    const int size = bytes.size();
    bool nonAscii = false;
    for (int i = 0; i < size;) {
        const unsigned char lead = data[i];
        if (lead < 0x80) {
            ++i;
            continue;
        }
        nonAscii = true;
        int extra = 0;
        unsigned int code = 0;
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            code = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            code = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            code = lead & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= size) {
            return false;
        }
        for (int k = 1; k <= extra; ++k) {
            const unsigned char continuation = data[i + k];
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (continuation & 0x3Fu);
        }
        if ((extra == 1 && code < 0x80u)
            || (extra == 2 && code < 0x800u)
            || (extra == 3 && code < 0x10000u)
            || code > 0x10FFFFu
            || (code >= 0xD800u && code <= 0xDFFFu)) {
            return false;
        }
        i += extra + 1;
    }
    return nonAscii;
}

// DXF R2007+ (AC1021 and newer) stores every string as UTF-8 by spec, but
// exporters routinely leave $DWGCODEPAGE at ANSI_936. libdxfrw obeys the
// header unconditionally and decodes already-UTF-8 bytes as GBK, turning
// "布局1" into "甯冨眬1" for layer names, layout names and all text entities.
// Feed the parser a temporary copy whose header declares UTF-8 in exactly
// that mismatch case. A genuine GBK drawing is left alone: its bytes are not
// valid UTF-8, so the declared codepage is the correct decoder for it.
QString normalizedDxfPath(const QString &path, QTemporaryFile *scratch)
{
    if (!scratch) {
        return path;
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        return path;
    }
    const QByteArray bytes = source.readAll();
    source.close();
    if (bytes.size() < 64 || bytes.size() > 256 * 1024 * 1024
        || !isNonAsciiUtf8(bytes)) {
        return path;
    }
    const QByteArray prefix(bytes.constData(), qMin(bytes.size(), 262144));
    if (!prefix.contains("AC1021") && !prefix.contains("AC1024")
        && !prefix.contains("AC1027") && !prefix.contains("AC1032")) {
        return path;
    }
    const int key = prefix.indexOf("$DWGCODEPAGE");
    if (key < 0) {
        return path;
    }
    // HEADER layout: "  9\n$DWGCODEPAGE\n  3\nANSI_936\n". The value is two
    // lines below the variable name.
    const int nameEnd = bytes.indexOf('\n', key);
    const int codeEnd = bytes.indexOf('\n', nameEnd + 1);
    if (nameEnd < 0 || codeEnd < 0) {
        return path;
    }
    int valueStart = codeEnd + 1;
    while (valueStart < bytes.size()
           && (bytes.at(valueStart) == ' '
               || bytes.at(valueStart) == '\t')) {
        ++valueStart;
    }
    const int valueEnd = bytes.indexOf('\n', valueStart);
    if (valueEnd <= valueStart) {
        return path;
    }
    const QByteArray value =
        bytes.mid(valueStart, valueEnd - valueStart).trimmed();
    if (value.startsWith("UTF-8") || !value.startsWith("ANSI_")) {
        return path;
    }
    QByteArray patched = bytes;
    patched.remove(valueStart, valueEnd - valueStart);
    patched.insert(valueStart, "UTF-8");
    scratch->setFileTemplate(
        QDir::tempPath() + QStringLiteral("/cadlens-dxf-XXXXXX.dxf"));
    if (!scratch->open()) {
        return path;
    }
    if (scratch->write(patched) != patched.size() || !scratch->flush()) {
        return path;
    }
    // Release the handle before libdxfrw opens the path (Windows opens
    // QTemporaryFile exclusively). The object stays alive and removes the
    // file when the load scope ends.
    scratch->close();
    return scratch->fileName();
}

// Last-resort fallback for DXF files whose BLOCKS section contains one
// malformed entity that makes libdxfrw abort before reaching ENTITIES.
// Build a copy with an empty BLOCKS section so the model-space geometry is
// still readable. Block references will not expand, but a viewer showing
// most of the drawing beats an outright "cannot open".
QString dxfWithoutBlocksPath(const QString &path, QTemporaryFile *scratch)
{
    if (!scratch) {
        return {};
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = source.readAll();
    source.close();
    const QList<QByteArray> lines = bytes.split('\n');
    QByteArray output;
    output.reserve(bytes.size());
    bool skipping = false;
    bool replaced = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i);
        const QByteArray token = line.trimmed();
        if (!skipping) {
            if (!replaced && token == "2" && i + 1 < lines.size()
                && lines.at(i + 1).trimmed() == "BLOCKS"
                && i >= 2 && lines.at(i - 1).trimmed() == "SECTION"
                && lines.at(i - 2).trimmed() == "0") {
                output += token;
                output += "\nBLOCKS\n  0\nENDSEC\n";
                ++i;
                skipping = true;
                replaced = true;
                continue;
            }
            output += line;
            if (i + 1 < lines.size()) {
                output += '\n';
            }
        } else if (token == "0" && i + 1 < lines.size()
                   && lines.at(i + 1).trimmed() == "ENDSEC") {
            ++i; // consume the original ENDSEC pair
            skipping = false;
        }
    }
    if (!replaced) {
        return {};
    }
    scratch->setFileTemplate(
        QDir::tempPath() + QStringLiteral("/cadlens-dxf-noblk-XXXXXX.dxf"));
    if (!scratch->open()) {
        return {};
    }
    if (scratch->write(output) != output.size() || !scratch->flush()) {
        return {};
    }
    scratch->close();
    return scratch->fileName();
}

// DGN-based exporters can put tens of millions of OBJECTS records
// (dictionaries, XRECORDs) into a DXF. A viewer needs none of them: all
// geometry is in BLOCKS/ENTITIES and all viewport definitions are VIEWPORT
// entities. Parsing that metadata can take a minute and can exhaust parser
// ceilings before the LAYOUT objects at the end of the section are reached,
// which silently drops every layout tab. Return a copy whose OBJECTS section
// contains only the raw LAYOUT objects, so libdxfrw still decodes their
// names/codepage normally but skips the metadata entirely. Only very large
// OBJECTS sections are rewritten so ordinary drawings keep the exact old
// import path.
QString dxfTrimmedObjectsPath(const QString &path, QTemporaryFile *scratch)
{
    if (!scratch) {
        return {};
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        return {};
    }
    const qint64 size = source.size();
    if (size < 16 * 1024 * 1024 || size > 1024LL * 1024 * 1024) {
        return {};
    }
    uchar *mapped = source.map(0, size);
    if (!mapped) {
        return {};
    }
    const QByteArray bytes = QByteArray::fromRawData(
        reinterpret_cast<const char *>(mapped),
        static_cast<qsizetype>(size));
    const QByteArray eol = bytes.left(65536).contains("\r\n") ? "\r\n" : "\n";
    const QByteArray sectionMark = "\nSECTION" + eol;
    const QByteArray endsecMark = "\nENDSEC" + eol;
    const QByteArray layoutMark = "\nLAYOUT" + eol;
    const QByteArray code0Padded = "\n  0" + eol;
    const QByteArray code0Plain = "\n0" + eol;

    const auto lineStartBefore = [&bytes](qint64 pos) {
        return bytes.lastIndexOf('\n', pos - 1) + 1;
    };
    const auto trimmedLine = [&bytes, size](qint64 pos) {
        qint64 end = bytes.indexOf('\n', pos);
        if (end < 0) {
            end = size;
        }
        return QByteArray::fromRawData(bytes.constData() + pos,
                                       static_cast<qsizetype>(end - pos))
            .trimmed();
    };

    qint64 objectsStart = -1;
    qint64 objectsBody = -1;
    qint64 objectsEnd = -1;
    for (qint64 search = 0; search < size;) {
        const qint64 hit = bytes.indexOf(sectionMark, search);
        if (hit < 0) {
            break;
        }
        search = hit + sectionMark.size();
        const qint64 codeLine = lineStartBefore(hit);
        if (codeLine < 0 || trimmedLine(codeLine) != "0") {
            continue;
        }
        const qint64 nameCodeStart = search;
        const qint64 nameCodeEnd = bytes.indexOf('\n', nameCodeStart);
        if (nameCodeEnd < 0) {
            break;
        }
        const qint64 nameValueStart = nameCodeEnd + 1;
        const qint64 nameValueEnd = bytes.indexOf('\n', nameValueStart);
        if (nameValueEnd < 0 || trimmedLine(nameCodeStart) != "2"
            || trimmedLine(nameValueStart) != "OBJECTS") {
            continue;
        }
        objectsStart = codeLine;
        objectsBody = nameValueEnd + 1;
        const qint64 endHit = bytes.indexOf(endsecMark, objectsBody);
        if (endHit < 0) {
            break;
        }
        const qint64 endCodeLine = lineStartBefore(endHit);
        if (endCodeLine < 0 || trimmedLine(endCodeLine) != "0") {
            break;
        }
        objectsEnd = endCodeLine;
        break;
    }
    if (objectsStart < 0 || objectsEnd <= objectsBody
        || objectsEnd - objectsBody < 4 * 1024 * 1024) {
        source.unmap(mapped);
        return {};
    }

    QByteArray layoutBlocks;
    for (qint64 cursor = objectsBody; cursor < objectsEnd;) {
        const qint64 hit = bytes.indexOf(layoutMark, cursor);
        if (hit < 0 || hit >= objectsEnd) {
            break;
        }
        const qint64 blockStart = lineStartBefore(hit);
        cursor = hit + layoutMark.size();
        if (blockStart < 0 || trimmedLine(blockStart) != "0") {
            continue;
        }
        qint64 next = bytes.indexOf(code0Padded, cursor);
        // An unpadded "0" line can also be a numeric VALUE (e.g. group 70).
        // Only accept it when the following line is a real object/entity name.
        qint64 plain = bytes.indexOf(code0Plain, cursor);
        while (plain >= 0 && plain < objectsEnd) {
            const qint64 valueStart = bytes.indexOf('\n', plain + 1) + 1;
            const qint64 valueEnd = bytes.indexOf('\n', valueStart);
            if (valueEnd > valueStart) {
                const QByteArray value = trimmedLine(valueStart);
                if (!value.isEmpty() && std::any_of(
                        value.cbegin(), value.cend(), [](char ch) {
                            return std::isalpha(static_cast<unsigned char>(ch));
                        })) {
                    break;
                }
            }
            plain = bytes.indexOf(code0Plain, plain + code0Plain.size());
        }
        if (plain >= objectsEnd) {
            plain = -1;
        }
        if (plain >= 0 && (next < 0 || plain < next)) {
            next = plain;
        }
        const qint64 blockEnd =
            (next < 0 || next >= objectsEnd) ? objectsEnd : next + 1;
        layoutBlocks.append(bytes.constData() + blockStart,
                            static_cast<qsizetype>(blockEnd - blockStart));
        cursor = blockEnd;
    }
    if (layoutBlocks.isEmpty()) {
        source.unmap(mapped);
        return {};
    }

    scratch->setFileTemplate(
        QDir::tempPath() + QStringLiteral("/cadlens-dxf-trim-XXXXXX.dxf"));
    if (!scratch->open()) {
        source.unmap(mapped);
        return {};
    }
    const QByteArray header = "  0" + eol + "SECTION" + eol + "  2" + eol
        + "OBJECTS" + eol;
    const QByteArray tail = "  0" + eol + "ENDSEC" + eol + "  0" + eol
        + "EOF" + eol;
    bool written = scratch->write(bytes.constData(), objectsStart)
                       == objectsStart
                   && scratch->write(header) == header.size()
                   && scratch->write(layoutBlocks) == layoutBlocks.size()
                   && scratch->write(tail) == tail.size();
    written = written && scratch->flush();
    const qint64 trimmedSize = scratch->size();
    source.unmap(mapped);
    scratch->close();
    if (!written) {
        return {};
    }
    if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
        std::fprintf(stderr,
                     "[dxf] trimmed OBJECTS: %lld -> %lld bytes, "
                     "%lld bytes of LAYOUT objects kept\n",
                     static_cast<long long>(size),
                     static_cast<long long>(trimmedSize),
                     static_cast<long long>(layoutBlocks.size()));
    }
    return scratch->fileName();
}

} // namespace

// AutoCAD Color Index (ACI) 1..255. Byte-identical to libredwg's
// dwg_rgb_palette_index() table, so the DXF (libdxfrw) and DWG (libredwg)
// back-ends resolve group code 62 to exactly the same RGB values.
static const QRgb kAciPalette[256] = {
    0x000000, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0xFF00FF, 0xFFFFFF,
    0x414141, 0x808080, 0xFF0000, 0xFFAAAA, 0xBD0000, 0xBD7E7E, 0x810000, 0x815656,
    0x680000, 0x684545, 0x4F0000, 0x4F3535, 0xFF3F00, 0xFFBFAA, 0xBD2E00, 0xBD8D7E,
    0x811F00, 0x816056, 0x681900, 0x684E45, 0x4F1300, 0x4F3B35, 0xFF7F00, 0xFFD4AA,
    0xBD5E00, 0xBD9D7E, 0x814000, 0x816B56, 0x683400, 0x685645, 0x4F2700, 0x4F4235,
    0xFFBF00, 0xFFEAAA, 0xBD8D00, 0xBDAD7E, 0x816000, 0x817656, 0x684E00, 0x685F45,
    0x4F3B00, 0x4F4935, 0xFFFF00, 0xFFFFAA, 0xBDBD00, 0xBDBD7E, 0x818100, 0x818156,
    0x686800, 0x686845, 0x4F4F00, 0x4F4F35, 0xBFFF00, 0xEAFFAA, 0x8DBD00, 0xADBD7E,
    0x608100, 0x768156, 0x4E6800, 0x5F6845, 0x3B4F00, 0x494F35, 0x7FFF00, 0xD4FFAA,
    0x5EBD00, 0x9DBD7E, 0x408100, 0x6B8156, 0x346800, 0x566845, 0x274F00, 0x424F35,
    0x3FFF00, 0xBFFFAA, 0x2EBD00, 0x8DBD7E, 0x1F8100, 0x608156, 0x196800, 0x4E6845,
    0x134F00, 0x3B4F35, 0x00FF00, 0xAAFFAA, 0x00BD00, 0x7EBD7E, 0x008100, 0x568156,
    0x006800, 0x456845, 0x004F00, 0x354F35, 0x00FF3F, 0xAAFFBF, 0x00BD2E, 0x7EBD8D,
    0x00811F, 0x568160, 0x006819, 0x45684E, 0x004F13, 0x354F3B, 0x00FF7F, 0xAAFFD4,
    0x00BD5E, 0x7EBD9D, 0x008140, 0x56816B, 0x006834, 0x456856, 0x004F27, 0x354F42,
    0x00FFBF, 0xAAFFEA, 0x00BD8D, 0x7EBDAD, 0x008160, 0x568176, 0x00684E, 0x45685F,
    0x004F3B, 0x354F49, 0x00FFFF, 0xAAFFFF, 0x00BDBD, 0x7EBDBD, 0x008181, 0x568181,
    0x006868, 0x456868, 0x004F4F, 0x354F4F, 0x00BFFF, 0xAAEAFF, 0x008DBD, 0x7EADBD,
    0x006081, 0x567681, 0x004E68, 0x455F68, 0x003B4F, 0x35494F, 0x007FFF, 0xAAD4FF,
    0x005EBD, 0x7E9DBD, 0x004081, 0x566B81, 0x003468, 0x455668, 0x00274F, 0x35424F,
    0x003FFF, 0xAABFFF, 0x002EBD, 0x7E8DBD, 0x001F81, 0x566081, 0x001968, 0x454E68,
    0x00134F, 0x353B4F, 0x0000FF, 0xAAAAFF, 0x0000BD, 0x7E7EBD, 0x000081, 0x565681,
    0x000068, 0x454568, 0x00004F, 0x35354F, 0x3F00FF, 0xBFAAFF, 0x2E00BD, 0x8D7EBD,
    0x1F0081, 0x605681, 0x190068, 0x4E4568, 0x13004F, 0x3B354F, 0x7F00FF, 0xD4AAFF,
    0x5E00BD, 0x9D7EBD, 0x400081, 0x6B5681, 0x340068, 0x564568, 0x27004F, 0x42354F,
    0xBF00FF, 0xEAAAFF, 0x8D00BD, 0xAD7EBD, 0x600081, 0x765681, 0x4E0068, 0x5F4568,
    0x3B004F, 0x49354F, 0xFF00FF, 0xFFAAFF, 0xBD00BD, 0xBD7EBD, 0x810081, 0x815681,
    0x680068, 0x684568, 0x4F004F, 0x4F354F, 0xFF00BF, 0xFFAAEA, 0xBD008D, 0xBD7EAD,
    0x810060, 0x815676, 0x68004E, 0x68455F, 0x4F003B, 0x4F3549, 0xFF007F, 0xFFAAD4,
    0xBD005E, 0xBD7E9D, 0x810040, 0x81566B, 0x680034, 0x684556, 0x4F0027, 0x4F3542,
    0xFF003F, 0xFFAABF, 0xBD002E, 0xBD7E8D, 0x81001F, 0x815660, 0x680019, 0x68454E,
    0x4F0013, 0x4F353B, 0x333333, 0x505050, 0x696969, 0x828282, 0xBEBEBE, 0xFFFFFF,
};

QColor aciColor(int index, const QColor &fallback)
{
    if (index >= 0 && index < 256) {
        return QColor::fromRgb(kAciPalette[index]);
    }
    return fallback;
}

QColor displayColorForDarkCanvas(const QColor &source)
{
    if (!source.isValid()) {
        return QColor(QStringLiteral("#f2f4f5"));
    }
    const auto linear = [](double value) {
        value /= 255.0;
        return value <= 0.04045
            ? value / 12.92
            : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const auto luminance = [&linear](const QColor &color) {
        return 0.2126 * linear(color.red())
            + 0.7152 * linear(color.green())
            + 0.0722 * linear(color.blue());
    };
    if (luminance(source) >= 0.12) {
        return source;
    }
    // Preserve hue and saturation for chromatic CAD/ACI colours. Blending
    // dark red/blue/green directly toward white produces the washed-out
    // pastel appearance common to generic dark-theme converters.
    const QColor hsv = source.toHsv();
    if (hsv.hsvSaturation() >= 32 && hsv.hsvHue() >= 0) {
        for (int value = qMax(64, hsv.value());
             value <= 255; value += 8) {
            QColor vivid;
            vivid.setHsv(
                hsv.hsvHue(), hsv.hsvSaturation(),
                value, source.alpha());
            if (luminance(vivid) >= 0.18) {
                return vivid;
            }
        }
        QColor vivid;
        vivid.setHsv(
            hsv.hsvHue(), hsv.hsvSaturation(),
            255, source.alpha());
        return vivid;
    }
    // Neutral black and grays have no hue worth preserving; raise them
    // toward light gray for visibility on the black canvas.
    QColor adjusted = source;
    for (double amount = 0.2; amount <= 0.75; amount += 0.05) {
        adjusted.setRed(qRound(source.red() * (1.0 - amount) + 255 * amount));
        adjusted.setGreen(
            qRound(source.green() * (1.0 - amount) + 255 * amount));
        adjusted.setBlue(
            qRound(source.blue() * (1.0 - amount) + 255 * amount));
        if (luminance(adjusted) >= 0.18) {
            adjusted.setAlpha(source.alpha());
            return adjusted;
        }
    }
    adjusted.setAlpha(source.alpha());
    return adjusted;
}

void CadEntity::updateBounds()
{
    segmentCacheValid = false;
    bounds = QRectF();
    if (type == CadEntityType::Text) {
        const int lines = qMax(1, text.count(QLatin1Char('\n')) + 1);
        double width = qMax(1, text.size()) * textHeight * 0.62
            * textWidthScale;
        if (textFit) {
            width = qMax(
                width,
                QLineF(textPosition, textAlignmentPoint).length());
        }
        const double height = textHeight * lines * 1.2;
        double left = textPosition.x();
        double top = textPosition.y() - height * 0.2;
        if (textAttachment >= 1 && textAttachment <= 9) {
            const int column = (textAttachment - 1) % 3;
            const int row = (textAttachment - 1) / 3;
            left -= column == 1 ? width * 0.5
                    : column == 2 ? width : 0.0;
            top = row == 0 ? textPosition.y() - height
                  : row == 1 ? textPosition.y() - height * 0.5
                             : textPosition.y();
        } else {
            left -= textHAlign == 1 || textHAlign == 4
                ? width * 0.5
                : textHAlign == 2 ? width : 0.0;
            top = textHAlign == 4 || textVAlign == 2
                ? textPosition.y() - height * 0.5
                : textVAlign == 3 ? textPosition.y() - height
                : textVAlign == 1 ? textPosition.y()
                                  : textPosition.y() - height * 0.2;
        }
        bounds = QRectF(left, top, width, height);
        return;
    }
    if (points.isEmpty()) {
        return;
    }
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (const QPointF &point : points) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            continue;
        }
        minX = qMin(minX, point.x());
        minY = qMin(minY, point.y());
        maxX = qMax(maxX, point.x());
        maxY = qMax(maxY, point.y());
    }
    if (minX <= maxX && minY <= maxY) {
        bounds = QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
        const double epsilon =
            qMax(1.0e-9, qMax(bounds.width(), bounds.height()) * 1.0e-9);
        if (bounds.width() < epsilon) {
            bounds.setLeft(bounds.left() - epsilon);
            bounds.setRight(bounds.right() + epsilon);
        }
        if (bounds.height() < epsilon) {
            bounds.setTop(bounds.top() - epsilon);
            bounds.setBottom(bounds.bottom() + epsilon);
        }
    }
}

const QVector<QLineF> &CadEntity::segments() const
{
    if (segmentCacheValid) {
        return segmentCache;
    }
    segmentCache.clear();
    segmentCacheValid = true;
    if (points.size() < 2) {
        return segmentCache;
    }
    if (type == CadEntityType::Text || type == CadEntityType::Point) {
        return segmentCache;
    }
    segmentCache.reserve(points.size() + (closed ? 1 : 0));
    for (int i = 1; i < points.size(); ++i) {
        segmentCache.append(QLineF(points[i - 1], points[i]));
    }
    if (closed) {
        segmentCache.append(QLineF(points.last(), points.first()));
    }
    return segmentCache;
}

QRectF cadInitialViewBounds(const QVector<CadEntity> &entities,
                           const QRectF &drawingBounds)
{
    // This is only a camera hint. The complete drawing bounds remain intact
    // so show-all continues to include every renderable entity.
    const QRectF fullBounds = drawingBounds.normalized();
    constexpr int gridSize = 32;
    constexpr int minimumEntityCount = 64;
    constexpr int cellCount = gridSize * gridSize;
    if (!fullBounds.isValid() || fullBounds.width() <= 0.0
        || fullBounds.height() <= 0.0
        || entities.size() < minimumEntityCount) {
        return fullBounds;
    }

    QVector<quint32> cellCounts(cellCount, 0);
    int validEntityCount = 0;
    for (const CadEntity &entity : entities) {
        if (!entity.bounds.isValid()) {
            continue;
        }
        const QPointF center = entity.bounds.center();
        if (!std::isfinite(center.x()) || !std::isfinite(center.y())) {
            continue;
        }
        if (!fullBounds.contains(center)) {
            continue;
        }
        const int column = qBound(
            0, int(qBound(0.0,
                          (center.x() - fullBounds.left())
                              / fullBounds.width(),
                          1.0)
                       * gridSize),
            gridSize - 1);
        const int row = qBound(
            0, int(qBound(0.0,
                          (center.y() - fullBounds.top())
                              / fullBounds.height(),
                          1.0)
                       * gridSize),
            gridSize - 1);
        ++cellCounts[row * gridSize + column];
        ++validEntityCount;
    }
    if (validEntityCount < minimumEntityCount) {
        return fullBounds;
    }

    QVector<quint8> visited(cellCount, 0);
    qint64 bestCount = 0;
    int bestMinimumColumn = 0;
    int bestMaximumColumn = 0;
    int bestMinimumRow = 0;
    int bestMaximumRow = 0;
    static constexpr int neighborOffsets[8][2] = {
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
        {1, 0}, {-1, 1}, {0, 1}, {1, 1}
    };
    for (int start = 0; start < cellCount; ++start) {
        if (cellCounts[start] == 0 || visited[start]) {
            continue;
        }
        QVector<int> pending = {start};
        visited[start] = 1;
        qint64 componentCount = 0;
        int minimumColumn = gridSize;
        int maximumColumn = -1;
        int minimumRow = gridSize;
        int maximumRow = -1;
        for (qsizetype index = 0; index < pending.size(); ++index) {
            const int cell = pending[index];
            const int column = cell % gridSize;
            const int row = cell / gridSize;
            componentCount += cellCounts[cell];
            minimumColumn = qMin(minimumColumn, column);
            maximumColumn = qMax(maximumColumn, column);
            minimumRow = qMin(minimumRow, row);
            maximumRow = qMax(maximumRow, row);
            for (const auto &offset : neighborOffsets) {
                const int nextColumn = column + offset[0];
                const int nextRow = row + offset[1];
                if (nextColumn < 0 || nextColumn >= gridSize
                    || nextRow < 0 || nextRow >= gridSize) {
                    continue;
                }
                const int next = nextRow * gridSize + nextColumn;
                if (!visited[next] && cellCounts[next] > 0) {
                    visited[next] = 1;
                    pending.append(next);
                }
            }
        }
        if (componentCount > bestCount) {
            bestCount = componentCount;
            bestMinimumColumn = minimumColumn;
            bestMaximumColumn = maximumColumn;
            bestMinimumRow = minimumRow;
            bestMaximumRow = maximumRow;
        }
    }
    if (bestCount <= 0) {
        return fullBounds;
    }

    const double cellWidth = fullBounds.width() / gridSize;
    const double cellHeight = fullBounds.height() / gridSize;
    QRectF focusBounds(
        fullBounds.left() + bestMinimumColumn * cellWidth,
        fullBounds.top() + bestMinimumRow * cellHeight,
        (bestMaximumColumn - bestMinimumColumn + 1) * cellWidth,
        (bestMaximumRow - bestMinimumRow + 1) * cellHeight);
    const double horizontalMargin = focusBounds.width() * 0.06;
    const double verticalMargin = focusBounds.height() * 0.06;
    focusBounds.adjust(-horizontalMargin, -verticalMargin,
                       horizontalMargin, verticalMargin);
    return focusBounds.intersected(fullBounds);
}

bool isOverallCadViewport(const QPointF &paperCenter,
                          const QPointF &modelCenter,
                          double viewHeight,
                          double paperHeight)
{
    if (viewHeight <= 1.0e-9 || paperHeight <= 1.0e-9) {
        return false;
    }
    return QLineF(paperCenter, modelCenter).length()
               <= qMax(1.0, viewHeight) * 1.0e-9
           && qAbs(viewHeight - paperHeight)
               <= qMax(1.0, paperHeight) * 1.0e-9;
}

namespace {

constexpr quint32 kParsedCacheMagic = 0x4341444cU;
constexpr quint32 kParsedCacheVersion = 15;

QDataStream &operator<<(QDataStream &stream,
                        const CadDimensionEdge &edge);
QDataStream &operator>>(QDataStream &stream, CadDimensionEdge &edge);
QDataStream &operator<<(QDataStream &stream,
                        const CadHatchPatternLine &line);
QDataStream &operator>>(QDataStream &stream, CadHatchPatternLine &line);
QDataStream &operator<<(QDataStream &stream, const CadLayer &layer);
QDataStream &operator>>(QDataStream &stream, CadLayer &layer);
QDataStream &operator<<(QDataStream &stream, const CadEntity &entity);
QDataStream &operator>>(QDataStream &stream, CadEntity &entity);
QDataStream &operator<<(QDataStream &stream,
                        const CadViewportDefinition &viewport);
QDataStream &operator>>(QDataStream &stream,
                        CadViewportDefinition &viewport);
QDataStream &operator<<(QDataStream &stream,
                        const CadExternalReference &reference);
QDataStream &operator>>(QDataStream &stream,
                        CadExternalReference &reference);

template <typename T>
void writeVector(QDataStream &stream, const QVector<T> &values)
{
    stream << quint32(values.size());
    for (const T &value : values) {
        stream << value;
    }
}

template <typename T>
bool readVector(QDataStream &stream, QVector<T> &values)
{
    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok || count > 10000000U) {
        return false;
    }
    values.clear();
    values.reserve(qsizetype(count));
    for (quint32 i = 0; i < count; ++i) {
        T value;
        stream >> value;
        if (stream.status() != QDataStream::Ok) {
            return false;
        }
        values.append(std::move(value));
    }
    return true;
}

template <typename T>
void writeStringVectorMap(
    QDataStream &stream,
    const QHash<QString, QVector<T>> &values)
{
    stream << quint32(values.size());
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        stream << it.key();
        writeVector(stream, it.value());
    }
}

template <typename T>
bool readStringVectorMap(
    QDataStream &stream,
    QHash<QString, QVector<T>> &values)
{
    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok || count > 100000U) {
        return false;
    }
    values.clear();
    for (quint32 i = 0; i < count; ++i) {
        QString key;
        QVector<T> value;
        stream >> key;
        if (!readVector(stream, value)) {
            return false;
        }
        values.insert(std::move(key), std::move(value));
    }
    return true;
}

template <typename T>
void writeStringMap(QDataStream &stream,
                    const QHash<QString, T> &values)
{
    stream << quint32(values.size());
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        stream << it.key() << it.value();
    }
}

template <typename T>
bool readStringMap(QDataStream &stream, QHash<QString, T> &values)
{
    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok || count > 100000U) {
        return false;
    }
    values.clear();
    for (quint32 i = 0; i < count; ++i) {
        QString key;
        T value;
        stream >> key >> value;
        if (stream.status() != QDataStream::Ok) {
            return false;
        }
        values.insert(std::move(key), std::move(value));
    }
    return true;
}

QString parsedCachePath(const QString &drawingPath)
{
    const QFileInfo info(drawingPath);
    const QString stablePath =
        info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    const QByteArray fingerprint =
        QFile::encodeName(stablePath)
        + '|' + QByteArray::number(info.size())
        + '|' + QByteArray::number(
            info.lastModified().toUTC().toSecsSinceEpoch())
        + '|' + QByteArray::number(qint64(kParsedCacheVersion));
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(fingerprint,
                                 QCryptographicHash::Sha256)
            .toHex());
    QString directory =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (directory.isEmpty()) {
        directory = QDir::tempPath() + QStringLiteral("/CadLens");
    }
    return directory + QStringLiteral("/parsed/") + hash
        + QStringLiteral(".cadcache");
}

QDataStream &operator<<(QDataStream &stream,
                        const CadDimensionEdge &edge)
{
    return stream << edge.midpoint << edge.length << edge.arc;
}

QDataStream &operator>>(QDataStream &stream, CadDimensionEdge &edge)
{
    return stream >> edge.midpoint >> edge.length >> edge.arc;
}

QDataStream &operator<<(QDataStream &stream,
                        const CadHatchPatternLine &line)
{
    return stream << line.origin << line.direction << line.offset
                  << line.dashes;
}

QDataStream &operator>>(QDataStream &stream, CadHatchPatternLine &line)
{
    return stream >> line.origin >> line.direction >> line.offset
                  >> line.dashes;
}

QDataStream &operator<<(QDataStream &stream,
                        const CadHatchPattern &pattern)
{
    stream << pattern.name << pattern.solid << pattern.gradient
           << qint32(pattern.style)
           << qint32(pattern.defaultArcSegments)
           << qint32(pattern.maximumArcSegments)
           << pattern.toleranceFactor
           << qint32(pattern.maximumBoundarySegments)
           << qint32(pattern.maximumPatternSegments);
    writeVector(stream, pattern.lines);
    return stream;
}

QDataStream &operator>>(QDataStream &stream, CadHatchPattern &pattern)
{
    qint32 style = 0;
    qint32 defaultArcSegments = 36;
    qint32 maximumArcSegments = 100;
    qint32 maximumBoundarySegments = 100000;
    qint32 maximumPatternSegments = 1000000;
    stream >> pattern.name >> pattern.solid >> pattern.gradient
        >> style >> defaultArcSegments >> maximumArcSegments
        >> pattern.toleranceFactor >> maximumBoundarySegments
        >> maximumPatternSegments;
    pattern.style = qBound(0, int(style), 2);
    pattern.defaultArcSegments = qBound(1, int(defaultArcSegments), 4096);
    pattern.maximumArcSegments = qBound(
        pattern.defaultArcSegments, int(maximumArcSegments), 4096);
    pattern.maximumBoundarySegments = qBound(
        1, int(maximumBoundarySegments), 10000000);
    pattern.maximumPatternSegments = qBound(
        1, int(maximumPatternSegments), 10000000);
    readVector(stream, pattern.lines);
    return stream;
}

QDataStream &operator<<(QDataStream &stream, const CadLayer &layer)
{
    return stream << layer.name << layer.color << layer.sourceColor
                  << layer.off << layer.frozen << layer.visible
                  << layer.locked << layer.frozenInNewViewport
                  << layer.lineWeight
                  << layer.entityCount;
}

QDataStream &operator>>(QDataStream &stream, CadLayer &layer)
{
    return stream >> layer.name >> layer.color >> layer.sourceColor
                  >> layer.off >> layer.frozen >> layer.visible
                  >> layer.locked >> layer.frozenInNewViewport
                  >> layer.lineWeight
                  >> layer.entityCount;
}

QDataStream &operator<<(QDataStream &stream, const CadEntity &entity)
{
    stream << qint32(entity.type) << quint64(entity.sourceHandle)
           << entity.layer << qint32(entity.layerIndex);
    stream << qint32(entity.blockLayerIndices.size());
    for (int index : entity.blockLayerIndices) {
        stream << qint32(index);
    }
    stream << entity.blockLayerNames;
    stream << entity.color << entity.sourceColor
           << entity.lineWeight
           << entity.closed << entity.filled
           << entity.center << entity.radius << entity.startAngle
           << entity.endAngle << entity.text << entity.textPosition
           << entity.textAlignmentPoint << entity.textFit
           << entity.textHeight << entity.textRotation
           << entity.textWidthScale << entity.textHAlign
           << entity.textVAlign << entity.textAttachment
           << entity.textMirrored << entity.hatchGroup
           << bool(entity.hatchPattern);
    writeVector(stream, entity.points);
    if (entity.hatchPattern) {
        stream << *entity.hatchPattern;
    }
    writeVector(stream, entity.dimensionEdges);
    return stream << entity.clipRect;
}

QDataStream &operator>>(QDataStream &stream, CadEntity &entity)
{
    qint32 type = 0;
    qint32 layerIndex = -1;
    qint32 blockLayerCount = 0;
    quint64 handle = 0;
    bool hasPattern = false;
    stream >> type >> handle >> entity.layer >> layerIndex;
    entity.layerIndex = qMax(-1, int(layerIndex));
    stream >> blockLayerCount;
    blockLayerCount = qBound(0, int(blockLayerCount), 100000);
    entity.blockLayerIndices.clear();
    entity.blockLayerIndices.reserve(blockLayerCount);
    for (int i = 0; i < blockLayerCount; ++i) {
        qint32 index = -1;
        stream >> index;
        entity.blockLayerIndices.append(qMax(-1, int(index)));
    }
    stream >> entity.blockLayerNames >> entity.color
        >> entity.sourceColor >> entity.lineWeight >> entity.closed
        >> entity.filled >> entity.center >> entity.radius
        >> entity.startAngle >> entity.endAngle >> entity.text
        >> entity.textPosition >> entity.textAlignmentPoint
        >> entity.textFit >> entity.textHeight >> entity.textRotation
        >> entity.textWidthScale >> entity.textHAlign
        >> entity.textVAlign >> entity.textAttachment
        >> entity.textMirrored >> entity.hatchGroup >> hasPattern;
    readVector(stream, entity.points);
    entity.type = CadEntityType(qBound(0, int(type), 7));
    entity.sourceHandle = handle;
    if (hasPattern) {
        entity.hatchPattern = std::make_shared<CadHatchPattern>();
        stream >> *entity.hatchPattern;
    }
    readVector(stream, entity.dimensionEdges);
    stream >> entity.clipRect;
    entity.updateBounds();
    return stream;
}

QDataStream &operator<<(QDataStream &stream,
                        const CadViewportDefinition &viewport)
{
    stream << qint32(viewport.id) << qint32(viewport.status)
           << viewport.on << viewport.overallPaperViewport
           << viewport.center << viewport.windowSize
           << viewport.modelCenter << viewport.viewHeight
           << viewport.twist << viewport.revealsModel
           << qint32(viewport.frozenLayers.size());
    for (const QString &layer : viewport.frozenLayers) {
        stream << layer;
    }
    stream << qint32(viewport.frozenLayerIndices.size());
    for (int index : viewport.frozenLayerIndices) {
        stream << qint32(index);
    }
    return stream;
}

QDataStream &operator>>(QDataStream &stream,
                        CadViewportDefinition &viewport)
{
    qint32 frozenCount = 0;
    qint32 viewportId = 0;
    qint32 viewportStatus = 0;
    stream >> viewportId >> viewportStatus
           >> viewport.on >> viewport.overallPaperViewport
           >> viewport.center >> viewport.windowSize
        >> viewport.modelCenter >> viewport.viewHeight
        >> viewport.twist >> viewport.revealsModel >> frozenCount;
    viewport.id = int(viewportId);
    viewport.status = int(viewportStatus);
    frozenCount = qBound(0, int(frozenCount), 100000);
    for (int i = 0; i < frozenCount; ++i) {
        QString layer;
        stream >> layer;
        viewport.frozenLayers.insert(layer);
    }
    qint32 frozenIndexCount = 0;
    stream >> frozenIndexCount;
    frozenIndexCount = qBound(0, int(frozenIndexCount), 100000);
    viewport.frozenLayerIndices.clear();
    for (int i = 0; i < frozenIndexCount; ++i) {
        qint32 index = -1;
        stream >> index;
        if (index >= 0) {
            viewport.frozenLayerIndices.insert(int(index));
        }
    }
    return stream;
}

QDataStream &operator<<(QDataStream &stream,
                        const CadExternalReference &reference)
{
    stream << reference.name << reference.savedPath
           << reference.resolvedPath << reference.fileFound
           << reference.loaded;
    return stream;
}

QDataStream &operator>>(QDataStream &stream,
                        CadExternalReference &reference)
{
    stream >> reference.name >> reference.savedPath
        >> reference.resolvedPath >> reference.fileFound
        >> reference.loaded;
    return stream;
}

void writeParsedDocumentCache(const CadDocument &document)
{
    const QString cachePath = parsedCachePath(document.path);
    QDir().mkpath(QFileInfo(cachePath).absolutePath());
    QSaveFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_8);
    stream << kParsedCacheMagic << kParsedCacheVersion
           << document.path << document.fileName
           << document.formatVersion << document.sourceFormat
           << document.sourceBackend;
    writeVector(stream, document.entities);
    writeVector(stream, document.layers);
    writeVector(stream, document.layouts);
    writeStringVectorMap(stream, document.layoutEntities);
    writeStringVectorMap(stream, document.layoutViewports);
    writeStringMap(stream, document.layoutBounds);
    writeVector(stream, document.externalReferences);
    stream << document.computedBounds
           << document.declaredBounds << document.bounds
           << document.savedViewCenter << document.savedViewHeight
           << document.savedViewWidth << document.savedViewTwist
           << document.hasSavedView << qint32(document.attributeMode)
           << document.sourceAttributeCount
           << document.sourceVisibleAttributeCount
           << document.sourceHiddenAttributeCount
           << document.renderedAttributeVisits
           << document.suppressedAttributeVisits
           << document.overriddenHiddenAttributeVisits
           << qint32(document.sourceUnit)
           << document.metersPerDrawingUnit;
    if (stream.status() == QDataStream::Ok) {
        file.commit();
    }
}

bool readParsedDocumentCache(const QString &drawingPath,
                             CadDocument &document)
{
    QFile file(parsedCachePath(drawingPath));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_8);
    quint32 magic = 0;
    quint32 version = 0;
    stream >> magic >> version;
    if (magic != kParsedCacheMagic || version != kParsedCacheVersion) {
        return false;
    }
    CadDocument cached;
    qint32 attributeMode = -1;
    qint32 sourceUnit = int(MeasuringUnit::Millimeter);
    stream >> cached.path >> cached.fileName >> cached.formatVersion
        >> cached.sourceFormat >> cached.sourceBackend;
    if (!readVector(stream, cached.entities)
        || !readVector(stream, cached.layers)
        || !readVector(stream, cached.layouts)
        || !readStringVectorMap(stream, cached.layoutEntities)
        || !readStringVectorMap(stream, cached.layoutViewports)
        || !readStringMap(stream, cached.layoutBounds)
        || !readVector(stream, cached.externalReferences)) {
        return false;
    }
    stream >> cached.computedBounds
        >> cached.declaredBounds >> cached.bounds
        >> cached.savedViewCenter >> cached.savedViewHeight
        >> cached.savedViewWidth >> cached.savedViewTwist
        >> cached.hasSavedView >> attributeMode
        >> cached.sourceAttributeCount
        >> cached.sourceVisibleAttributeCount
        >> cached.sourceHiddenAttributeCount
        >> cached.renderedAttributeVisits
        >> cached.suppressedAttributeVisits
        >> cached.overriddenHiddenAttributeVisits
        >> sourceUnit >> cached.metersPerDrawingUnit;
    if (stream.status() != QDataStream::Ok
        || cached.entities.isEmpty() || cached.layers.isEmpty()
        || !cached.bounds.isValid()) {
        return false;
    }
    cached.attributeMode = attributeMode;
    cached.sourceUnit = MeasuringUnit(sourceUnit);
    cached.calibrated = false;
    cached.sanitizeRenderableEntities();
    document = std::move(cached);
    return true;
}

} // namespace

QString drawingParsedCachePath(const QString &drawingPath)
{
    return parsedCachePath(drawingPath);
}

namespace {

bool finiteCadPoint(const QPointF &point)
{
    constexpr double maximumCoordinate = 1.0e15;
    return std::isfinite(point.x()) && std::isfinite(point.y())
        && qAbs(point.x()) <= maximumCoordinate
        && qAbs(point.y()) <= maximumCoordinate;
}

void sanitizeCadEntity(CadEntity &entity)
{
    entity.points.erase(
        std::remove_if(
            entity.points.begin(), entity.points.end(),
            [](const QPointF &point) {
                return !finiteCadPoint(point);
            }),
        entity.points.end());
    if (!entity.clipRect.isNull() && !entity.clipRect.isValid()) {
        entity.clipRect = QRectF();
    }
    if (entity.type == CadEntityType::Text
        && entity.text.trimmed().isEmpty()) {
        entity.text.clear();
    }
    entity.updateBounds();
}

} // namespace

bool isRenderableCadEntity(const CadEntity &entity)
{
    if (entity.type == CadEntityType::Text) {
        return !entity.text.trimmed().isEmpty()
            && std::isfinite(entity.textHeight)
            && entity.textHeight > 1.0e-9
            && entity.textHeight < 1.0e9
            && finiteCadPoint(entity.textPosition);
    }
    if (entity.points.size() < 2) {
        return false;
    }
    if (!std::all_of(
            entity.points.cbegin(), entity.points.cend(),
            finiteCadPoint)) {
        return false;
    }
    if ((entity.type == CadEntityType::Circle
         || entity.type == CadEntityType::Arc)
        && (!std::isfinite(entity.radius)
            || entity.radius <= 1.0e-12
            || !finiteCadPoint(entity.center))) {
        return false;
    }
    if (!entity.bounds.isValid()
        || !std::isfinite(entity.bounds.width())
        || !std::isfinite(entity.bounds.height())
        || entity.bounds.width() > 1.0e15
        || entity.bounds.height() > 1.0e15) {
        return false;
    }
    if (entity.type == CadEntityType::Segment) {
        return QLineF(entity.points.first(), entity.points.last()).length()
            > 1.0e-12;
    }
    if (entity.type == CadEntityType::Solid) {
        return entity.points.size() >= 3;
    }
    return true;
}

void CadDocument::sanitizeRenderableEntities()
{
    const auto sanitizeVector = [](QVector<CadEntity> &values) {
        QVector<CadEntity> clean;
        clean.reserve(values.size());
        for (CadEntity &entity : values) {
            sanitizeCadEntity(entity);
            if (isRenderableCadEntity(entity)) {
                clean.append(std::move(entity));
            }
        }
        values = std::move(clean);
    };
    sanitizeVector(entities);
    for (auto iterator = layoutEntities.begin();
         iterator != layoutEntities.end(); ++iterator) {
        sanitizeVector(iterator.value());
    }
    recountLayers();
}

namespace {

// A stale VPORT can survive in the file long after the model moved, so a
// saved view that does not even touch the computed bounds would open the
// sheet on an empty region. So would one that only grazes the drawing:
// real drawings concentrate their entities, so a useful saved view must
// cover a meaningful share of them. Drop useless views and let the
// fit-to-drawing path decide.
void discardStaleSavedView(CadDocument &document)
{
    if (!document.hasSavedView || document.savedViewHeight <= 0.0
        || document.savedViewWidth <= 0.0
        || !document.bounds.isValid()) {
        return;
    }
    const QRectF view(
        document.savedViewCenter
            - QPointF(document.savedViewWidth / 2.0,
                      document.savedViewHeight / 2.0),
        QSizeF(document.savedViewWidth, document.savedViewHeight));
    bool stale = !view.intersects(document.bounds);
    if (!stale && document.entities.size() >= 64) {
        // Coverage test: sample entity centers against the saved view.
        // Editors move content around without refreshing the saved view,
        // which leaves it pointing at an empty offset region.
        constexpr int kSampleLimit = 20000;
        const int stride = qMax(1, document.entities.size()
                                       / kSampleLimit);
        qint64 sampled = 0;
        qint64 covered = 0;
        for (qsizetype i = 0; i < document.entities.size(); i += stride) {
            const CadEntity &entity = document.entities[i];
            if (!entity.bounds.isValid()) {
                continue;
            }
            const QPointF center = entity.bounds.center();
            if (!std::isfinite(center.x()) || !std::isfinite(center.y())) {
                continue;
            }
            ++sampled;
            if (view.contains(center)) {
                ++covered;
            }
        }
        if (sampled >= 16
            && covered * 4 < sampled) {
            stale = true;
        }
    }
    if (stale) {
        document.hasSavedView = false;
        document.savedViewCenter = QPointF();
        document.savedViewWidth = 0.0;
        document.savedViewHeight = 0.0;
        document.savedViewTwist = 0.0;
    }
}

} // namespace

bool CadDocument::load(const QString &filePath, QString *errorMessage,
                       const QHash<QString, QString> &externalReferencePaths)
{
    QElapsedTimer timer;
    timer.start();
    entities.clear();
    layers.clear();
    layerIndices.clear();
    layouts.clear();
    layoutEntities.clear();
    layoutViewports.clear();
    layoutBounds.clear();
    pdfPages.clear();
    declaredBounds = QRectF();
    computedBounds = QRectF();
    bounds = QRectF();
    savedViewCenter = QPointF();
    savedViewHeight = 0.0;
    savedViewWidth = 0.0;
    savedViewTwist = 0.0;
    hasSavedView = false;
    attributeMode = -1;
    sourceAttributeCount = 0;
    sourceVisibleAttributeCount = 0;
    sourceHiddenAttributeCount = 0;
    renderedAttributeVisits = 0;
    suppressedAttributeVisits = 0;
    overriddenHiddenAttributeVisits = 0;
    calibrated = false;

    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("文件不存在：%1").arg(filePath);
        }
        return false;
    }
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("pdf")) {
        if (!loadPdfDocument(filePath, *this, errorMessage)) {
            return false;
        }
        loadMilliseconds = timer.elapsed();
        return !pdfPages.isEmpty();
    }
    if (suffix != QStringLiteral("dwg") && suffix != QStringLiteral("dxf")) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("仅支持 DWG、DXF 与 PDF 图纸。");
        }
        return false;
    }

    if (suffix == QStringLiteral("dwg")
        && !qEnvironmentVariableIsSet("CADLENS_FORCE_LIBDXFRW")
        && !qEnvironmentVariableIsSet("CADLENS_DISABLE_PARSE_CACHE")
        && externalReferencePaths.isEmpty()
        && readParsedDocumentCache(filePath, *this)) {
        loadMilliseconds = timer.elapsed();
        if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_DWG")) {
            std::fprintf(stderr, "parsed-cache hit %lldms\n",
                         static_cast<long long>(loadMilliseconds));
        }
        // A document whose only entities are POINT/WIPEOUT/VIEWPORT (all
        // intentionally not drawn) parses fine but yields no drawable
        // geometry. That is a valid drawing, not a load failure.
        return true;
    }

    if (suffix == QStringLiteral("dwg")
        && !qEnvironmentVariableIsSet("CADLENS_FORCE_LIBDXFRW")) {
        QString libreError;
        if (loadDwgWithLibreDwg(filePath, *this, &libreError,
                                externalReferencePaths)) {
            if (externalReferencePaths.isEmpty()
                && !qEnvironmentVariableIsSet(
                    "CADLENS_DISABLE_PARSE_CACHE")) {
                discardStaleSavedView(*this);
                writeParsedDocumentCache(*this);
            } else {
                discardStaleSavedView(*this);
            }
            loadMilliseconds = timer.elapsed();
            // Same rule as the cache path: a successful decode with no
            // drawable entities is an empty sheet, not an error.
            return true;
        }
        // Keep the older direct reader as a compatibility fallback for DWG
        // revisions which the primary backend cannot complete.
        entities.clear();
        layers.clear();
        layouts.clear();
        layoutEntities.clear();
        layoutBounds.clear();
        declaredBounds = QRectF();
    }

    // Keep the temporary file alive for the whole import: the parser reads
    // from its path, and it is deleted when this scope ends.
    QTemporaryFile normalizedDxf;
    QTemporaryFile trimmedObjectsDxf;
    QTemporaryFile blocksStrippedDxf;
    QString importPath = filePath;
    if (suffix == QStringLiteral("dxf")) {
        // Trim oversized non-graphical OBJECTS metadata first, then patch
        // the codepage on the (much smaller) result.
        const QString trimmed =
            dxfTrimmedObjectsPath(filePath, &trimmedObjectsDxf);
        importPath = normalizedDxfPath(
            trimmed.isEmpty() ? filePath : trimmed, &normalizedDxf);
    }
    try {
        const bool debug =
            qEnvironmentVariableIsSet("CADLENS_DXF_DEBUG");
        auto data = std::make_unique<dx_data>();
        auto importer = std::make_unique<LayoutAwareInterface>();
        const auto countEntities = [](const dx_data &candidate) {
            qsizetype count = 0;
            if (candidate.mBlock) {
                for (const auto *entity : candidate.mBlock->ent) {
                    if (entity) {
                        ++count;
                    }
                }
            }
            return count;
        };
        const bool success = importer->fileImport(
            importPath.toStdString(), data.get(), debug);
        qsizetype parsedEntities = countEntities(*data);
        // A parser failure very late in the file still leaves all the
        // geometry intact; keep it rather than refusing the drawing.
        if (!success && parsedEntities > 0
            && qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
            std::fprintf(stderr,
                         "[dxf] partial import after parser error: "
                         "%lld entities kept\n",
                         static_cast<long long>(parsedEntities));
        }
        if (!success && suffix == QStringLiteral("dxf")) {
            // The abort happened before ENTITIES (typically a malformed
            // BLOCKS entry). Retry with an empty BLOCKS section so the
            // model-space geometry can still be read. This also recovers
            // partial imports where the parser kept only a small prefix.
            // Do not retry a file that parsed cleanly: an empty ENTITIES
            // section is legal and stripping BLOCKS would throw away its
            // block definitions.
            const QString stripped = dxfWithoutBlocksPath(
                importPath, &blocksStrippedDxf);
            if (!stripped.isEmpty()) {
                auto retryData = std::make_unique<dx_data>();
                auto retryImporter =
                    std::make_unique<LayoutAwareInterface>();
                retryImporter->fileImport(stripped.toStdString(),
                                          retryData.get(), debug);
                const qsizetype retryCount = countEntities(*retryData);
                if (retryCount > parsedEntities) {
                    data = std::move(retryData);
                    importer = std::move(retryImporter);
                    parsedEntities = retryCount;
                    if (qEnvironmentVariableIsSet(
                            "CADLENS_VERBOSE_RENDER")) {
                        std::fprintf(stderr,
                                     "[dxf] BLOCKS section skipped, "
                                     "%lld entities recovered\n",
                                     static_cast<long long>(retryCount));
                    }
                }
            }
        }
        // A DXF may legitimately have an empty ENTITIES section: cover,
        // catalog and material-table sheets sometimes keep every entity in
        // BLOCKS and insert nothing in model or paper space. Opening such a
        // sheet must succeed (as it already does for the DWG cache path).
        // Only refuse when nothing at all could be decoded.
        const auto hasDecodedStructure = [](const dx_data &candidate) {
            return candidate.sourceVersion != DRW::UNKNOWNV
                || !candidate.headerC.vars.empty()
                || !candidate.layers.empty()
                || !candidate.blocks.empty()
                || !candidate.lineTypes.empty()
                || !candidate.textStyles.empty()
                || !candidate.VPorts.empty();
        };
        if (parsedEntities == 0 && !hasDecodedStructure(*data)
            && importer->layoutRecords.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "图纸解析失败。文件可能损坏、加密，或使用了当前开源内核"
                    "尚未覆盖的 DWG 版本。");
            }
            return false;
        }
        if (parsedEntities == 0
            && qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
            std::fprintf(stderr,
                         "[dxf] accepted empty sheet: version=%d "
                         "blocks=%lld layers=%lld layouts=%lld\n",
                         static_cast<int>(data->sourceVersion),
                         static_cast<long long>(data->blocks.size()),
                         static_cast<long long>(data->layers.size()),
                         static_cast<long long>(
                             importer->layoutRecords.size()));
        }
        entities.reserve(1024);
        importer->finishLayouts();
        ModelBuilder builder(*this);
        builder.read(*data, *importer, filePath, importer->layouts,
                     importer->layoutRecords);
    } catch (const std::exception &exception) {
        if (errorMessage) {
            *errorMessage = QString::fromUtf8(exception.what());
        }
        return false;
    } catch (...) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("解析图纸时发生未知错误。");
        }
        return false;
    }
    loadMilliseconds = timer.elapsed();
    discardStaleSavedView(*this);
    return true;
}

const CadPdfPage *CadDocument::pdfPage(const QString &name) const
{
    for (const CadPdfPage &page : pdfPages) {
        if (page.name == name) {
            return &page;
        }
    }
    return nullptr;
}

CadPdfPage *CadDocument::pdfPage(const QString &name)
{
    for (CadPdfPage &page : pdfPages) {
        if (page.name == name) {
            return &page;
        }
    }
    return nullptr;
}

int CadDocument::layerIndex(const QString &name) const
{
    const auto found = layerIndices.constFind(name);
    if (found != layerIndices.cend()) {
        return found.value();
    }
    for (int i = 0; i < layers.size(); ++i) {
        if (layers[i].name == name) {
            return i;
        }
    }
    return -1;
}

const CadLayer *CadDocument::layer(const QString &name) const
{
    const int index = layerIndex(name);
    return index >= 0 ? &layers[index] : nullptr;
}

CadLayer *CadDocument::layer(const QString &name)
{
    const int index = layerIndex(name);
    return index >= 0 ? &layers[index] : nullptr;
}

const CadLayer *CadDocument::layerOf(const CadEntity &entity) const
{
    if (entity.layerIndex >= 0
        && entity.layerIndex < layers.size()) {
        return &layers[entity.layerIndex];
    }
    return layer(entity.layer);
}

void CadDocument::recountLayers()
{
    layerIndices.clear();
    layerIndices.reserve(layers.size());
    for (int i = 0; i < layers.size(); ++i) {
        layers[i].entityCount = 0;
        layerIndices.insert(layers[i].name, i);
    }
    for (const CadEntity &entity : entities) {
        const int index = entity.layerIndex >= 0
            && entity.layerIndex < layers.size()
                ? entity.layerIndex
                : layerIndex(entity.layer);
        if (index >= 0 && index < layers.size()) {
            ++layers[index].entityCount;
        }
    }
}

QString measuringUnitLabel(MeasuringUnit unit)
{
    switch (unit) {
    case MeasuringUnit::Millimeter:
        return QStringLiteral("毫米 (mm)");
    case MeasuringUnit::Centimeter:
        return QStringLiteral("厘米 (cm)");
    case MeasuringUnit::Meter:
        return QStringLiteral("米 (m)");
    case MeasuringUnit::Kilometer:
        return QStringLiteral("千米 (km)");
    case MeasuringUnit::Inch:
        return QStringLiteral("英寸 (in)");
    case MeasuringUnit::Foot:
        return QStringLiteral("英尺 (ft)");
    case MeasuringUnit::None:
        break;
    }
    return QStringLiteral("图纸单位");
}

QString measuringUnitSuffix(MeasuringUnit unit, bool squared)
{
    QString suffix;
    switch (unit) {
    case MeasuringUnit::Millimeter:
        suffix = QStringLiteral("mm");
        break;
    case MeasuringUnit::Centimeter:
        suffix = QStringLiteral("cm");
        break;
    case MeasuringUnit::Meter:
        suffix = QStringLiteral("m");
        break;
    case MeasuringUnit::Kilometer:
        suffix = QStringLiteral("km");
        break;
    case MeasuringUnit::Inch:
        suffix = QStringLiteral("in");
        break;
    case MeasuringUnit::Foot:
        suffix = QStringLiteral("ft");
        break;
    case MeasuringUnit::None:
        suffix = QStringLiteral("u");
        break;
    }
    return squared ? suffix + QStringLiteral("²") : suffix;
}

double measuringUnitMeters(MeasuringUnit unit)
{
    switch (unit) {
    case MeasuringUnit::Millimeter:
        return 0.001;
    case MeasuringUnit::Centimeter:
        return 0.01;
    case MeasuringUnit::Meter:
        return 1.0;
    case MeasuringUnit::Kilometer:
        return 1000.0;
    case MeasuringUnit::Inch:
        return 0.0254;
    case MeasuringUnit::Foot:
        return 0.3048;
    case MeasuringUnit::None:
        return 1.0;
    }
    return 1.0;
}

MeasuringUnit measuringUnitFromInsUnits(int insUnits)
{
    switch (insUnits) {
    case 1:
        return MeasuringUnit::Inch;
    case 2:
        return MeasuringUnit::Foot;
    case 4:
        return MeasuringUnit::Millimeter;
    case 5:
        return MeasuringUnit::Centimeter;
    case 6:
        return MeasuringUnit::Meter;
    case 7:
        return MeasuringUnit::Kilometer;
    default:
        return MeasuringUnit::Millimeter;
    }
}

QString formatMeasuredLength(double drawingLength, const CadDocument &document,
                             MeasuringUnit displayUnit, int precision)
{
    const double value = drawingLength * document.metersPerDrawingUnit
        / measuringUnitMeters(displayUnit);
    return QStringLiteral("%1 %2")
        .arg(value, 0, 'f', precision)
        .arg(measuringUnitSuffix(displayUnit));
}

QString formatDimensionValue(double drawingLength,
                             const CadDocument &document,
                             MeasuringUnit displayUnit, int precision)
{
    const double value = drawingLength * document.metersPerDrawingUnit
        / measuringUnitMeters(displayUnit);
    return QString::number(value, 'f', qBound(0, precision, 6));
}

QString formatMeasuredArea(double drawingArea, const CadDocument &document,
                           MeasuringUnit displayUnit, int precision)
{
    const double linearFactor = document.metersPerDrawingUnit
        / measuringUnitMeters(displayUnit);
    return QStringLiteral("%1 %2")
        .arg(drawingArea * linearFactor * linearFactor, 0, 'f', precision)
        .arg(measuringUnitSuffix(displayUnit, true));
}

QString formatAreaValue(double drawingArea, const CadDocument &document,
                        MeasuringUnit displayUnit, int precision)
{
    const double linearFactor = document.metersPerDrawingUnit
        / measuringUnitMeters(displayUnit);
    return QString::number(drawingArea * linearFactor * linearFactor,
                           'f', qBound(0, precision, 6));
}

void appendConnectedPolylinePiece(
    QVector<QVector<QPointF>> &paths, QVector<QPointF> piece,
    double tolerance)
{
    if (piece.isEmpty()) {
        return;
    }
    const auto pointsMatch = [tolerance](const QPointF &a,
                                         const QPointF &b) {
        return QLineF(a, b).length() <= tolerance;
    };
    if (paths.isEmpty()) {
        paths.append(std::move(piece));
        return;
    }

    QVector<QPointF> &path = paths.last();
    if (!path.isEmpty() && pointsMatch(path.last(), piece.first())) {
        for (qsizetype i = 1; i < piece.size(); ++i) {
            path.append(piece[i]);
        }
        return;
    }
    if (!path.isEmpty() && pointsMatch(path.last(), piece.last())) {
        for (qsizetype i = piece.size() - 2; i >= 0; --i) {
            path.append(piece[i]);
        }
        return;
    }
    paths.append(std::move(piece));
}
