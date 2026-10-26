#include "LibreDwgReader.h"

#include "dwg.h"
#include "dwg_api.h"

#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iconv.h>
#include <limits>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Affine {
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

    Affine then(const Affine &outer) const
    {
        Affine result;
        result.m11 = outer.m11 * m11 + outer.m21 * m12;
        result.m21 = outer.m11 * m21 + outer.m21 * m22;
        result.dx = outer.m11 * dx + outer.m21 * dy + outer.dx;
        result.m12 = outer.m12 * m11 + outer.m22 * m12;
        result.m22 = outer.m12 * m21 + outer.m22 * m22;
        result.dy = outer.m12 * dx + outer.m22 * dy + outer.dy;
        return result;
    }
};

QString decodeWithIconv(const QByteArray &input, const char *encoding)
{
    iconv_t converter = iconv_open("UTF-8", encoding);
    if (converter == reinterpret_cast<iconv_t>(-1)) {
        return {};
    }
    std::vector<char> output(std::max<std::size_t>(
        32, std::size_t(input.size()) * 4 + 8));
    char *inputPtr = const_cast<char *>(input.constData());
    char *outputPtr = output.data();
    std::size_t inputLeft = std::size_t(input.size());
    std::size_t outputLeft = output.size();
    const bool converted =
        iconv(converter, &inputPtr, &inputLeft, &outputPtr, &outputLeft)
            != std::size_t(-1)
        && inputLeft == 0;
    iconv_close(converter);
    return converted
        ? QString::fromUtf8(output.data(),
                            int(outputPtr - output.data()))
        : QString();
}

QString repairMojibake(const QString &value)
{
    bool hasCjk = false;
    bool suspicious = false;
    for (const QChar ch : value) {
        const ushort code = ch.unicode();
        hasCjk = hasCjk
            || (code >= 0x3400 && code <= 0x9fff)
            || (code >= 0xf900 && code <= 0xfaff);
        suspicious = suspicious || (code >= 0x80 && code < 0x0500);
    }
    if (hasCjk || !suspicious) {
        return value;
    }
    const QString repaired =
        decodeWithIconv(value.toUtf8(), "GB18030");
    if (repaired.isEmpty()) {
        return value;
    }
    for (const QChar ch : repaired) {
        const ushort code = ch.unicode();
        if ((code >= 0x3400 && code <= 0x9fff)
            || (code >= 0xf900 && code <= 0xfaff)) {
            return repaired;
        }
    }
    return value;
}

QString utf8Field(void *value, const char *type, const char *field)
{
    char *text = nullptr;
    int isNew = 0;
    if (!value
        || !dwg_dynapi_entity_utf8text(value, type, field, &text, &isNew,
                                      nullptr)
        || !text) {
        return {};
    }
    QString result = QString::fromUtf8(text);
    if (result.contains(QChar::ReplacementCharacter)) {
        const QString decoded = decodeWithIconv(
            QByteArray(text, int(std::strlen(text))), "GB18030");
        if (!decoded.isEmpty()) {
            result = decoded;
        }
    }
    result = repairMojibake(result);
    if (isNew) {
        std::free(text);
    }
    return result;
}

QString cleanCadText(QString text)
{
    QString output;
    output.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text[i];
        if (ch == QLatin1Char('{') || ch == QLatin1Char('}')) {
            continue;
        }
        if (ch != QLatin1Char('\\')) {
            output.append(ch);
            continue;
        }
        if (i + 1 >= text.size()) {
            break;
        }
        const QChar code = text[++i];
        if (code == QLatin1Char('P')) {
            output.append(QLatin1Char('\n'));
        } else if (code == QLatin1Char('~')) {
            output.append(QLatin1Char(' '));
        } else if (code == QLatin1Char('\\')
                   || code == QLatin1Char('{')
                   || code == QLatin1Char('}')) {
            output.append(code);
        } else if (code == QLatin1Char('U')
                   && i + 5 < text.size()
                   && text[i + 1] == QLatin1Char('+')) {
            bool ok = false;
            const ushort value = text.mid(i + 2, 4).toUShort(&ok, 16);
            if (ok) {
                output.append(QChar(value));
                i += 5;
            }
        } else if (code == QLatin1Char('S')) {
            int end = text.indexOf(QLatin1Char(';'), i + 1);
            if (end < 0) {
                break;
            }
            QString stacked = text.mid(i + 1, end - i - 1);
            stacked.replace(QLatin1Char('^'), QLatin1Char('/'));
            output.append(stacked);
            i = end;
        } else if (code == QLatin1Char('L')
                   || code == QLatin1Char('l')
                   || code == QLatin1Char('O')
                   || code == QLatin1Char('o')
                   || code == QLatin1Char('K')
                   || code == QLatin1Char('k')) {
            continue;
        } else {
            const int end = text.indexOf(QLatin1Char(';'), i + 1);
            if (end >= 0) {
                i = end;
            }
        }
    }
    output.replace(QStringLiteral("%%d"), QStringLiteral("°"));
    output.replace(QStringLiteral("%%D"), QStringLiteral("°"));
    output.replace(QStringLiteral("%%c"), QStringLiteral("⌀"));
    output.replace(QStringLiteral("%%C"), QStringLiteral("⌀"));
    output.replace(QStringLiteral("%%p"), QStringLiteral("±"));
    output.replace(QStringLiteral("%%P"), QStringLiteral("±"));
    return output;
}

QColor aciColor(int index, const QColor &fallback = QColor("#f2f4f5"))
{
    if (index >= 0 && index < 256) {
        return QColor::fromRgb(dwg_rgb_palette_index(index) & 0x00ffffff);
    }
    return fallback;
}

QColor visibleColor(const QColor &source)
{
    return displayColorForDarkCanvas(source);
}

QColor cmcColor(const Dwg_Color &source,
                const QColor &fallback = QColor("#f2f4f5"))
{
    const int method = int(source.rgb >> 24) & 0xff;
    const int payload = int(source.rgb & 0x00ffffff);
    if (method == DWG_COLOR_METHOD_TRUECOLOR && payload <= 0xff) {
        return aciColor(payload, fallback);
    }
    if (method == DWG_COLOR_METHOD_ACI
        && source.index >= 0 && source.index < 256) {
        return aciColor(source.index, fallback);
    }
    if (payload > 0xff) {
        return QColor::fromRgb(payload);
    }
    if (source.index >= 0 && source.index < 256) {
        return aciColor(source.index, fallback);
    }
    return fallback;
}

QPointF point(const BITCODE_2RD &p)
{
    return QPointF(p.x, p.y);
}

QPointF point(const BITCODE_3BD &p)
{
    return QPointF(p.x, p.y);
}

QPointF point(const BITCODE_2DPOINT &p)
{
    return QPointF(p.x, p.y);
}

QPointF ocsPoint(const QPointF &p, const BITCODE_BE &normal)
{
    const double nx = normal.x;
    const double ny = normal.y;
    const double nz = normal.z;
    if (qAbs(nx) < 1.0e-12 && qAbs(ny) < 1.0e-12
        && nz >= 0.0) {
        return p;
    }
    QPointF xAxis;
    if (qAbs(nx) < 0.015625 && qAbs(ny) < 0.015625) {
        xAxis = QPointF(nz, 0.0);
    } else {
        xAxis = QPointF(-ny, nx);
    }
    const double xLength = std::hypot(xAxis.x(), xAxis.y());
    if (xLength < 1.0e-12) {
        return p;
    }
    xAxis /= xLength;
    const QPointF yAxis(-xAxis.y() * nz, nz * xAxis.x());
    return QPointF(xAxis.x() * p.x() + yAxis.x() * p.y(),
                   xAxis.y() * p.x() + yAxis.y() * p.y());
}

QPointF ocsPoint(const QPointF &p, double elevation,
                 const BITCODE_BE &normal)
{
    const double nx = normal.x;
    const double ny = normal.y;
    const double nz = normal.z;
    if (qAbs(nx) < 1.0e-12 && qAbs(ny) < 1.0e-12
        && nz >= 0.0) {
        return p;
    }
    QPointF xAxis;
    if (qAbs(nx) < 0.015625 && qAbs(ny) < 0.015625) {
        xAxis = QPointF(nz, 0.0);
    } else {
        xAxis = QPointF(-ny, nx);
    }
    const double length = std::hypot(xAxis.x(), xAxis.y());
    if (length < 1.0e-12) {
        return p;
    }
    xAxis /= length;
    const QPointF yAxis(-xAxis.y() * nz, nz * xAxis.x());
    return QPointF(
        xAxis.x() * p.x() + yAxis.x() * p.y() + nx * elevation,
        xAxis.y() * p.x() + yAxis.y() * p.y() + ny * elevation);
}

Affine ocsAffine(const BITCODE_BE &normal)
{
    const double nx = normal.x;
    const double ny = normal.y;
    const double nz = normal.z;
    if (qAbs(nx) < 1.0e-12 && qAbs(ny) < 1.0e-12 && nz >= 0.0) {
        return {};
    }
    QPointF xAxis;
    if (qAbs(nx) < 0.015625 && qAbs(ny) < 0.015625) {
        xAxis = QPointF(nz, 0.0);
    } else {
        xAxis = QPointF(-ny, nx);
    }
    const double length = std::hypot(xAxis.x(), xAxis.y());
    if (length < 1.0e-12) {
        return {};
    }
    xAxis /= length;
    const QPointF yAxis(-xAxis.y() * nz, nz * xAxis.x());
    Affine result;
    result.m11 = xAxis.x();
    result.m12 = xAxis.y();
    result.m21 = yAxis.x();
    result.m22 = yAxis.y();
    return result;
}

QVector<QPointF> sampleArc(const QPointF &center, double radius,
                           double start, double end, bool full = false)
{
    QVector<QPointF> result;
    if (radius <= 0.0 || !std::isfinite(radius)) {
        return result;
    }
    if (full) {
        start = 0.0;
        end = 2.0 * kPi;
    } else {
        while (end <= start) {
            end += 2.0 * kPi;
        }
    }
    const int count = qBound(full ? 36 : 8,
                             int(std::ceil(qAbs(end - start)
                                           / (kPi / 48.0))),
                             512);
    result.reserve(count);
    for (int i = 0; i < count; ++i) {
        const double angle =
            start + (end - start) * double(i) / double(count);
        result.append(center
                      + QPointF(std::cos(angle), std::sin(angle))
                          * radius);
    }
    return result;
}

void appendBulge(QVector<QPointF> &result, const QPointF &a,
                 const QPointF &b, double bulge)
{
    if (qAbs(bulge) < 1.0e-10) {
        result.append(b);
        return;
    }
    const QPointF delta = b - a;
    const double chord = std::hypot(delta.x(), delta.y());
    if (chord < 1.0e-12) {
        result.append(b);
        return;
    }
    const double theta = 4.0 * std::atan(bulge);
    const QPointF normal(-delta.y() / chord, delta.x() / chord);
    const QPointF center =
        (a + b) * 0.5
        + normal * (chord * 0.5 / std::tan(theta * 0.5));
    const double radius =
        std::hypot(a.x() - center.x(), a.y() - center.y());
    const double start =
        std::atan2(a.y() - center.y(), a.x() - center.x());
    const int count =
        qBound(4, int(std::ceil(qAbs(theta) / (kPi / 24.0))), 256);
    for (int i = 1; i <= count; ++i) {
        const double angle =
            start + theta * double(i) / double(count);
        result.append(center
                      + QPointF(std::cos(angle), std::sin(angle))
                          * radius);
    }
}

QString versionName(Dwg_Version_Type version)
{
    switch (version) {
    case R_11:
        return QStringLiteral("AC1009");
    case R_13:
        return QStringLiteral("AC1012");
    case R_14:
        return QStringLiteral("AC1014");
    case R_2000:
        return QStringLiteral("AC1015");
    case R_2004:
        return QStringLiteral("AC1018");
    case R_2007:
        return QStringLiteral("AC1021");
    case R_2010:
        return QStringLiteral("AC1024");
    case R_2013:
        return QStringLiteral("AC1027");
    case R_2018:
        return QStringLiteral("AC1032");
    default:
        return QStringLiteral("DWG");
    }
}

class LibreBuilder {
public:
    LibreBuilder(Dwg_Data &dwg, CadDocument &document)
        : m_dwg(dwg), m_document(document)
    {
    }

    bool build(QString *error)
    {
        QElapsedTimer stageTimer;
        stageTimer.start();
        m_document.path = m_path;
        m_document.fileName = QFileInfo(m_path).fileName();
        m_document.sourceFormat = QStringLiteral("DWG");
        m_document.sourceBackend = QStringLiteral("LibreDWG");
        m_document.formatVersion = versionName(m_dwg.header.version);
        m_document.sourceUnit =
            measuringUnitFromInsUnits(m_dwg.header_vars.INSUNITS);
        m_document.metersPerDrawingUnit =
            measuringUnitMeters(m_document.sourceUnit);
        m_attMode = int(m_dwg.header_vars.ATTMODE);
        m_document.attributeMode = m_attMode;
        m_mirrText = bool(m_dwg.header_vars.MIRRTEXT);
        if (m_document.sourceUnit == MeasuringUnit::None
            || m_document.metersPerDrawingUnit <= 0.0) {
            m_document.sourceUnit = MeasuringUnit::Millimeter;
            m_document.metersPerDrawingUnit = 0.001;
        }
        indexOwnedAttributes();
        const qint64 attributeMilliseconds = stageTimer.restart();
        readLayers();
        readSavedModelView();
        const qint64 layerMilliseconds = stageTimer.restart();

        Dwg_Object *modelHeaderObject =
            dwg_model_space_object(&m_dwg);
        if (!modelHeaderObject) {
            if (error) {
                *error = QStringLiteral("DWG 中未找到模型空间。");
            }
            return false;
        }
        m_target = &m_document.entities;
        visitBlockHeader(modelHeaderObject, Affine{}, 0);
        const qint64 modelMilliseconds = stageTimer.restart();

        QVector<Dwg_Object *> layouts;
        for (BITCODE_BL i = 0; i < m_dwg.num_objects; ++i) {
            Dwg_Object *object = &m_dwg.object[i];
            if (object->fixedtype == DWG_TYPE_LAYOUT) {
                layouts.append(object);
            }
        }
        std::sort(layouts.begin(), layouts.end(),
                  [](Dwg_Object *a, Dwg_Object *b) {
                      auto *la = dwg_object_to_LAYOUT(a);
                      auto *lb = dwg_object_to_LAYOUT(b);
                      return la && lb && la->tab_order < lb->tab_order;
                  });
        for (Dwg_Object *layoutObject : layouts) {
            Dwg_Object_LAYOUT *layout =
                dwg_object_to_LAYOUT(layoutObject);
            if (!layout) {
                continue;
            }
            const QString name = utf8Field(
                layout, "LAYOUT", "layout_name");
            if (name.isEmpty()
                || name.compare(QStringLiteral("Model"),
                                Qt::CaseInsensitive) == 0
                || !layout->block_header) {
                continue;
            }
            int refError = 0;
            Dwg_Object *blockHeader =
                dwg_ref_get_object(layout->block_header, &refError);
            if (!blockHeader || refError) {
                continue;
            }
            QVector<CadEntity> content;
            m_target = &content;
            visitBlockHeader(blockHeader, Affine{}, 0);
            Dwg_Object_BLOCK_HEADER *layoutBlock =
                dwg_object_to_BLOCK_HEADER(blockHeader);
            if (layoutBlock && layoutBlock->entities) {
                const BITCODE_BL count =
                    qMax(layoutBlock->num_owned,
                         BITCODE_BL(0));
                for (BITCODE_BL i = 0; i < count; ++i) {
                    if (!layoutBlock->entities[i]) {
                        continue;
                    }
                    int itemError = 0;
                    Dwg_Object *layoutEntity = dwg_ref_get_object(
                        layoutBlock->entities[i], &itemError);
                    if (layoutEntity && !itemError
                        && layoutEntity->fixedtype == DWG_TYPE_VIEWPORT) {
                        recordViewport(
                            name,
                            dwg_object_to_VIEWPORT(layoutEntity));
                    }
                }
            }
            m_document.layouts.append(name);
            m_document.layoutEntities.insert(name, content);
            const QRectF layoutExtents(
                QPointF(layout->EXTMIN.x, layout->EXTMIN.y),
                QPointF(layout->EXTMAX.x, layout->EXTMAX.y));
            QRectF contentBounds = layoutExtents.isValid()
                ? layoutExtents.normalized()
                : calculateBounds(content);
            if (contentBounds.isValid()) {
                contentBounds = contentBounds.adjusted(
                    -contentBounds.width() * 0.03,
                    -contentBounds.height() * 0.03,
                    contentBounds.width() * 0.03,
                    contentBounds.height() * 0.03);
            }
            m_document.layoutBounds.insert(name, contentBounds);
        }
        const qint64 layoutMilliseconds = stageTimer.restart();
        m_target = &m_document.entities;
        m_document.computedBounds =
            robustBounds(m_document.entities);
        m_document.renderedAttributeVisits =
            int(qMin<quint64>(
                m_attributeRenderedVisits,
                std::numeric_limits<int>::max()));
        m_document.suppressedAttributeVisits =
            int(qMin<quint64>(
                m_attributeSuppressedVisits,
                std::numeric_limits<int>::max()));
        m_document.overriddenHiddenAttributeVisits =
            int(qMin<quint64>(
                m_attributeOverriddenHiddenVisits,
                std::numeric_limits<int>::max()));
        m_document.bounds = m_document.computedBounds;
        const QRectF declared(
            QPointF(m_dwg.header_vars.EXTMIN.x,
                    m_dwg.header_vars.EXTMIN.y),
            QPointF(m_dwg.header_vars.EXTMAX.x,
                    m_dwg.header_vars.EXTMAX.y));
        if (declared.isValid() && declared.width() > 0.0
            && declared.height() > 0.0) {
            m_document.declaredBounds = declared.normalized();
        }
        if (!m_document.bounds.isValid()) {
            m_document.bounds = QRectF(0.0, 0.0, 1000.0, 1000.0);
        }
        m_document.recountLayers();
        if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_DWG")) {
            std::fprintf(
                stderr,
                "builder-stage attributes=%lldms layers/view=%lldms "
                "model=%lldms layouts=%lldms bounds/audit=%lldms\n",
                static_cast<long long>(attributeMilliseconds),
                static_cast<long long>(layerMilliseconds),
                static_cast<long long>(modelMilliseconds),
                static_cast<long long>(layoutMilliseconds),
                static_cast<long long>(stageTimer.elapsed()));
            std::fprintf(
                stderr,
                "LibreDWG blocks=%llu inserts=%llu unresolved=%llu "
                "maxDepth=%d entities=%lld\n",
                static_cast<unsigned long long>(m_blocksVisited),
                static_cast<unsigned long long>(m_insertsVisited),
                static_cast<unsigned long long>(m_unresolvedInserts),
                m_maxDepth,
                static_cast<long long>(m_document.entities.size()));
            std::fprintf(stderr,
                         "insertSwitch=%llu blockArrays=%llu\n",
                         static_cast<unsigned long long>(
                             m_insertSwitchVisited),
                         static_cast<unsigned long long>(
                             m_blockArrayVisits));
            std::fprintf(stderr, "attributes indexed=%lld visited=%llu "
                                 "attMode=%d\n",
                         static_cast<long long>(m_attributesByOwner.size()),
                         static_cast<unsigned long long>(
                             m_attributeVisits),
                         m_attMode);
            QList<int> keys = m_typeHistogram.keys();
            std::sort(keys.begin(), keys.end());
            for (int key : keys) {
                std::fprintf(stderr, "type[%d]=%llu ", key,
                             static_cast<unsigned long long>(
                                 m_typeHistogram.value(key)));
            }
            std::fprintf(stderr, "\n");
        }
        return !m_document.entities.isEmpty();
    }

    void setPath(const QString &path) { m_path = path; }

private:
    void indexOwnedAttributes()
    {
        m_attributesByOwner.clear();
        m_document.sourceAttributeCount = 0;
        m_document.sourceVisibleAttributeCount = 0;
        m_document.sourceHiddenAttributeCount = 0;
        for (BITCODE_BL i = 0; i < m_dwg.num_objects; ++i) {
            Dwg_Object *object = &m_dwg.object[i];
            if (object->fixedtype != DWG_TYPE_ATTRIB) {
                continue;
            }
            ++m_document.sourceAttributeCount;
            if (object->tio.entity && object->tio.entity->invisible) {
                ++m_document.sourceHiddenAttributeCount;
            } else {
                ++m_document.sourceVisibleAttributeCount;
            }
            if (!object->tio.entity
                || !object->tio.entity->ownerhandle) {
                continue;
            }
            m_attributesByOwner[object->tio.entity->ownerhandle->absolute_ref]
                .append(object);
        }
    }

    void readSavedModelView()
    {
        Dwg_Object_VPORT *active = nullptr;
        Dwg_Object_VPORT *fallback = nullptr;
        for (BITCODE_BL i = 0; i < m_dwg.num_objects; ++i) {
            Dwg_Object *object = &m_dwg.object[i];
            if (object->fixedtype != DWG_TYPE_VPORT) {
                continue;
            }
            Dwg_Object_VPORT *viewport =
                dwg_object_to_VPORT(object);
            if (!viewport || viewport->VIEWSIZE <= 1.0e-9) {
                continue;
            }
            if (!fallback) {
                fallback = viewport;
            }
            const QString name =
                utf8Field(viewport, "VPORT", "name");
            if (name.compare(QStringLiteral("*Active"),
                             Qt::CaseInsensitive) == 0) {
                active = viewport;
                break;
            }
        }
        const Dwg_Object_VPORT *view = active ? active : fallback;
        if (!view) {
            return;
        }
        m_document.savedViewCenter =
            QPointF(view->VIEWCTR.x, view->VIEWCTR.y);
        m_document.savedViewHeight = view->VIEWSIZE;
        m_document.savedViewWidth =
            view->view_width > 1.0e-9
            ? view->view_width : view->VIEWSIZE;
        m_document.savedViewTwist = view->VIEWTWIST;
        m_document.hasSavedView = true;
    }

    void readLayers()
    {
        m_layerHandles.clear();
        for (BITCODE_BL i = 0; i < m_dwg.num_objects; ++i) {
            Dwg_Object *object = &m_dwg.object[i];
            if (object->fixedtype != DWG_TYPE_LAYER) {
                continue;
            }
            Dwg_Object_LAYER *source =
                dwg_object_to_LAYER(object);
            if (!source) {
                continue;
            }
            CadLayer layer;
            layer.name = utf8Field(source, "LAYER", "name");
            if (layer.name.isEmpty()) {
                layer.name = QStringLiteral("0");
            }
            layer.sourceColor = cmcColor(source->color);
            layer.color = layer.sourceColor;
            layer.color = visibleColor(layer.color);
            layer.visible = !source->off && !source->frozen;
            layer.locked = source->locked;
            layer.lineWeight = source->linewt;
            m_document.layers.append(layer);
            m_layerHandles.insert(object->handle.value,
                                  m_document.layers.size() - 1);
        }
        if (m_document.layers.isEmpty()) {
            CadLayer layer;
            layer.name = QStringLiteral("0");
            m_document.layers.append(layer);
        }
    }

    CadEntity commonEntity(Dwg_Object *object,
                           const QString &insertLayer = {}) const
    {
        CadEntity result;
        result.sourceHandle = object ? object->handle.value : 0;
        const auto *common = object->tio.entity;
        const auto layerFound =
            common && common->layer
                ? m_layerHandles.constFind(common->layer->absolute_ref)
                : m_layerHandles.cend();
        if (layerFound != m_layerHandles.cend()
            && layerFound.value() < m_document.layers.size()) {
            const CadLayer &layer =
                m_document.layers[layerFound.value()];
            result.layer = layer.name;
            result.color = layer.color;
            result.sourceColor =
                layer.sourceColor.isValid()
                ? layer.sourceColor : layer.color;
            result.lineWeight = layer.lineWeight;
        } else {
            result.layer = QStringLiteral("0");
            if (!m_document.layers.isEmpty()) {
                result.color = m_document.layers.first().color;
                result.sourceColor =
                    m_document.layers.first().sourceColor.isValid()
                    ? m_document.layers.first().sourceColor
                    : m_document.layers.first().color;
                result.lineWeight =
                    m_document.layers.first().lineWeight;
            }
        }
        if (result.layer == QStringLiteral("0")
            && !insertLayer.isEmpty()) {
            result.layer = insertLayer;
            const auto inherited =
                std::find_if(m_document.layers.cbegin(),
                             m_document.layers.cend(),
                             [&insertLayer](const CadLayer &layer) {
                                 return layer.name == insertLayer;
                             });
            if (inherited != m_document.layers.cend()) {
                result.color = inherited->color;
                    result.sourceColor =
                        inherited->sourceColor.isValid()
                        ? inherited->sourceColor : inherited->color;
                result.lineWeight = inherited->lineWeight;
            }
        }
        if (common) {
            int colorError = 0;
            const Dwg_Color *color =
                dwg_ent_get_color(const_cast<Dwg_Object_Entity *>(common),
                                  &colorError);
            if (color && color->index != 256) {
                result.sourceColor =
                    cmcColor(*color, result.sourceColor);
                result.color = result.sourceColor;
            }
            result.lineWeight = common->linewt;
        }
        result.color = visibleColor(result.color);
        return result;
    }

    void append(CadEntity entity, const Affine &transform)
    {
        const double scaleX =
            std::hypot(transform.m11, transform.m12);
        const double scaleY =
            std::hypot(transform.m21, transform.m22);
        const double uniformScale = (scaleX + scaleY) * 0.5;
        const auto mapVector = [&transform](const QPointF &vector) {
            return QPointF(
                transform.m11 * vector.x() + transform.m21 * vector.y(),
                transform.m12 * vector.x() + transform.m22 * vector.y());
        };
        for (QPointF &p : entity.points) {
            p = transform.map(p);
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
        if (entity.type == CadEntityType::Text) {
            const double sourceAngle =
                qDegreesToRadians(entity.textRotation);
            const QPointF xAxis = mapVector(
                QPointF(std::cos(sourceAngle),
                        std::sin(sourceAngle)));
            const QPointF yAxis = mapVector(
                QPointF(-std::sin(sourceAngle),
                        std::cos(sourceAngle)));
            const double xLength =
                std::hypot(xAxis.x(), xAxis.y());
            const double yLength =
                std::hypot(yAxis.x(), yAxis.y());
            const double frameDeterminant =
                xAxis.x() * yAxis.y() - xAxis.y() * yAxis.x();
            const bool reflected = frameDeterminant < 0.0;
            double rotation = qRadiansToDegrees(
                std::atan2(xAxis.y(), xAxis.x()));
            if (reflected && !m_mirrText) {
                while (rotation > 90.0) {
                    rotation -= 180.0;
                }
                while (rotation <= -90.0) {
                    rotation += 180.0;
                }
            }
            entity.textRotation = rotation;
            entity.textMirrored = reflected && m_mirrText;
            entity.textHeight *= yLength;
            if (xLength > 1.0e-12) {
                entity.textWidthScale *= yLength / xLength;
            }
            if (entity.textFit) {
                const QPointF direction =
                    entity.textAlignmentPoint - entity.textPosition;
                if (std::hypot(direction.x(), direction.y())
                    > 1.0e-12) {
                    double fitRotation = qRadiansToDegrees(
                        std::atan2(direction.y(), direction.x()));
                    if (reflected && !m_mirrText) {
                        while (fitRotation > 90.0) {
                            fitRotation -= 180.0;
                        }
                        while (fitRotation <= -90.0) {
                            fitRotation += 180.0;
                        }
                    }
                    entity.textRotation = fitRotation;
                }
            }
        } else {
            entity.textHeight *= uniformScale;
            entity.textRotation += qRadiansToDegrees(
                std::atan2(transform.m12, transform.m11));
        }
        if (entity.hatchPattern) {
            auto pattern =
                std::make_shared<CadHatchPattern>(*entity.hatchPattern);
            for (CadHatchPatternLine &line : pattern->lines) {
                line.origin = transform.map(line.origin);
                line.direction = mapVector(line.direction);
                line.offset = mapVector(line.offset);
            }
            entity.hatchPattern = std::move(pattern);
        }
        entity.updateBounds();
        if (entity.type != CadEntityType::Text
            && entity.points.isEmpty()) {
            return;
        }
        m_target->append(std::move(entity));
    }

    void visitBlockHeader(Dwg_Object *headerObject,
                          const Affine &outer, int depth,
                          const QString &insertLayer = {})
    {
        if (!headerObject || depth > 32) {
            return;
        }
        ++m_blocksVisited;
        m_maxDepth = qMax(m_maxDepth, depth);
        Dwg_Object_BLOCK_HEADER *block =
            dwg_object_to_BLOCK_HEADER(headerObject);
        if (block && block->entities && block->num_owned > 0) {
            ++m_blockArrayVisits;
            for (BITCODE_BL i = 0; i < block->num_owned; ++i) {
                if (!block->entities[i]) {
                    continue;
                }
                int error = 0;
                Dwg_Object *entity = dwg_ref_get_object(
                    block->entities[i], &error);
                if (entity && !error) {
                    visitEntity(entity, outer, depth, insertLayer);
                }
            }
            return;
        }
        Dwg_Object *entity =
            get_first_owned_entity(headerObject);
        int guard = 0;
        while (entity && guard++ < 1000000) {
            visitEntity(entity, outer, depth, insertLayer);
            entity = get_next_owned_entity(headerObject, entity);
        }
    }

    void visitInsert(Dwg_Object *object, Dwg_Entity_INSERT *insert,
                     const Affine &outer, int depth,
                     const QString &insertLayer)
    {
        ++m_insertsVisited;
        if (!insert || !insert->block_header || depth > 32) {
            ++m_unresolvedInserts;
            return;
        }
        int error = 0;
        Dwg_Object *header =
            dwg_ref_get_object(insert->block_header, &error);
        if (!header || error) {
            ++m_unresolvedInserts;
            return;
        }
        Dwg_Object_BLOCK_HEADER *block =
            dwg_object_to_BLOCK_HEADER(header);
        if (!block) {
            ++m_unresolvedInserts;
            return;
        }
        const double sx = qAbs(insert->scale.x) > 1.0e-12
            ? insert->scale.x : 1.0;
        const double sy = qAbs(insert->scale.y) > 1.0e-12
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
        const int columns = qBound(1, int(insert->num_cols), 256);
        const int rows = qBound(1, int(insert->num_rows), 256);
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                const double x = column * insert->col_spacing;
                const double y = row * insert->row_spacing;
                Affine arrayLocal = local;
                arrayLocal.dx += cosine * x - sine * y;
                arrayLocal.dy += sine * x + cosine * y;
                const Affine combined =
                    arrayLocal.then(ocsAffine(insert->extrusion))
                        .then(outer);
                const QString blockLayer =
                    commonEntity(object, insertLayer).layer;
                visitBlockHeader(header, combined, depth + 1,
                                 blockLayer);
            }
        }
        const QVector<Dwg_Object *> ownedAttributes =
            m_attributesByOwner.value(object->handle.value);
        if (!ownedAttributes.isEmpty()) {
            for (Dwg_Object *attribute : ownedAttributes) {
                ++m_attributeVisits;
                visitEntity(
                    attribute, outer, depth + 1,
                    commonEntity(object, insertLayer).layer);
            }
        } else if (insert->attribs && insert->num_owned > 0) {
            for (BITCODE_BL i = 0; i < insert->num_owned; ++i) {
                if (!insert->attribs[i]) {
                    continue;
                }
                int attributeError = 0;
                Dwg_Object *attribute = dwg_ref_get_object(
                    insert->attribs[i], &attributeError);
                if (attribute && !attributeError) {
                    visitEntity(
                        attribute, outer, depth + 1,
                        commonEntity(object, insertLayer).layer);
                }
            }
        }
        (void)object;
    }

    void recordViewport(const QString &layoutName,
                        Dwg_Entity_VIEWPORT *viewport)
    {
        if (!viewport || viewport->width <= 1.0e-9
            || viewport->height <= 1.0e-9
            || viewport->VIEWSIZE <= 1.0e-9) {
            return;
        }
        const QPointF paperCenter = point(viewport->center);
        const QPointF modelCenter = point(viewport->VIEWCTR);
        const bool paperViewport =
            QLineF(paperCenter, modelCenter).length()
                <= qMax(1.0, viewport->VIEWSIZE) * 1.0e-9
            && qAbs(viewport->VIEWSIZE - viewport->height)
                <= qMax(1.0, viewport->height) * 1.0e-9;
        QSet<QString> frozenLayers;
        if (!paperViewport && viewport->frozen_layers
            && viewport->num_frozen_layers > 0) {
            for (BITCODE_BL i = 0; i < viewport->num_frozen_layers; ++i) {
                const Dwg_Object_Ref *layerRef =
                    viewport->frozen_layers[i];
                if (!layerRef) {
                    continue;
                }
                const auto found =
                    m_layerHandles.constFind(layerRef->absolute_ref);
                if (found != m_layerHandles.cend()
                    && found.value() >= 0
                    && found.value() < m_document.layers.size()) {
                    frozenLayers.insert(
                        m_document.layers[found.value()].name);
                }
            }
        }
        m_document.layoutViewports[layoutName].append(
            {paperCenter, QSizeF(viewport->width, viewport->height),
             modelCenter, viewport->VIEWSIZE, viewport->VIEWTWIST,
             !paperViewport, frozenLayers});
    }

    void visitLegacyPolyline(Dwg_Object *object,
                             Dwg_Entity_POLYLINE_2D *polyline,
                             const Affine &transform,
                             const QString &insertLayer)
    {
        if (!polyline || !polyline->vertex) {
            return;
        }
        CadEntity out = commonEntity(object, insertLayer);
        out.type = CadEntityType::Polyline;
        out.closed = (polyline->flag & 1) != 0;
        QPointF previousOcs;
        bool havePrevious = false;
        for (BITCODE_BL i = 0; i < polyline->num_owned; ++i) {
            if (!polyline->vertex[i]) {
                continue;
            }
            int error = 0;
            Dwg_Object *vertexObject =
                dwg_ref_get_object(polyline->vertex[i], &error);
            if (!vertexObject || error) {
                continue;
            }
            Dwg_Entity_VERTEX_2D *vertex =
                dwg_object_to_VERTEX_2D(vertexObject);
            if (!vertex) {
                continue;
            }
            const QPointF currentOcs = point(vertex->point);
            const QPointF currentWcs =
                ocsPoint(currentOcs, polyline->elevation,
                         polyline->extrusion);
            if (!havePrevious) {
                out.points.append(currentWcs);
            } else {
                QVector<QPointF> local = {previousOcs};
                appendBulge(local, previousOcs, currentOcs,
                            vertex->bulge);
                const QPointF previousWcs =
                    ocsPoint(previousOcs, polyline->elevation,
                             polyline->extrusion);
                for (int j = 1; j < local.size(); ++j) {
                    out.points.append(
                        ocsPoint(local[j], polyline->elevation,
                                 polyline->extrusion));
                }
                const double chord =
                    QLineF(previousWcs, currentWcs).length();
                out.dimensionEdges.append(
                    {(previousWcs + currentWcs) * 0.5,
                     qAbs(vertex->bulge) > 1.0e-10
                         ? qAbs(vertex->bulge) * 4.0 * chord
                         : chord,
                     qAbs(vertex->bulge) > 1.0e-10});
            }
            previousOcs = currentOcs;
            havePrevious = true;
        }
        if (out.closed && !out.points.isEmpty() && havePrevious) {
            const double closingBulge = 0.0;
            if (qAbs(closingBulge) < 1.0e-10
                && QLineF(out.points.last(), out.points.first()).length()
                    > 1.0e-9) {
                out.dimensionEdges.append(
                    {(out.points.last() + out.points.first()) * 0.5,
                     QLineF(out.points.last(), out.points.first()).length(),
                     false});
            }
        }
        append(std::move(out), transform);
    }

    void visitPolyline3D(Dwg_Object *object,
                         Dwg_Entity_POLYLINE_3D *polyline,
                         const Affine &transform,
                         const QString &insertLayer)
    {
        if (!polyline || !polyline->vertex) {
            return;
        }
        CadEntity out = commonEntity(object, insertLayer);
        out.type = CadEntityType::Polyline;
        out.closed = (polyline->flag & 1) != 0;
        for (BITCODE_BL i = 0; i < polyline->num_owned; ++i) {
            if (!polyline->vertex[i]) {
                continue;
            }
            int error = 0;
            Dwg_Object *vertexObject =
                dwg_ref_get_object(polyline->vertex[i], &error);
            if (!vertexObject || error) {
                continue;
            }
            Dwg_Entity_VERTEX_3D *vertex =
                dwg_object_to_VERTEX_3D(vertexObject);
            if (vertex) {
                out.points.append(point(vertex->point));
            }
        }
        if (out.points.size() >= 2) {
            append(std::move(out), transform);
        }
    }

    void visitHatch(Dwg_Object *object, Dwg_Entity_HATCH *hatch,
                    const Affine &transform,
                    const QString &insertLayer)
    {
        if (!hatch || !hatch->paths) {
            return;
        }
        const int hatchGroup = m_nextHatchGroup++;
        auto pattern = std::make_shared<CadHatchPattern>();
        pattern->solid = hatch->is_solid_fill != 0;
        pattern->gradient = hatch->is_gradient_fill != 0;
        const QPointF ocsOrigin =
            ocsPoint(QPointF(0.0, 0.0), hatch->elevation,
                     hatch->extrusion);
        const auto ocsVector = [&](const QPointF &vector) {
            return ocsPoint(vector, hatch->elevation, hatch->extrusion)
                - ocsOrigin;
        };
        for (BITCODE_BS i = 0; i < hatch->num_deflines; ++i) {
            const Dwg_HATCH_DefLine &definition = hatch->deflines[i];
            CadHatchPatternLine line;
            line.origin = ocsPoint(
                QPointF(definition.pt0.x, definition.pt0.y),
                hatch->elevation, hatch->extrusion);
            line.direction = ocsVector(
                QPointF(std::cos(definition.angle),
                        std::sin(definition.angle)));
            line.offset = ocsVector(
                QPointF(definition.offset.x, definition.offset.y));
            if (definition.dashes && definition.num_dashes > 0) {
                line.dashes.reserve(definition.num_dashes);
                for (BITCODE_BS j = 0;
                     j < definition.num_dashes; ++j) {
                    line.dashes.append(definition.dashes[j]);
                }
            }
            pattern->lines.append(std::move(line));
        }
        for (BITCODE_BL pathIndex = 0;
             pathIndex < hatch->num_paths; ++pathIndex) {
            const Dwg_HATCH_Path &path = hatch->paths[pathIndex];
            QVector<QVector<QPointF>> chains;
            const bool openPath = (path.flag & 0x20) != 0;
            bool closedPath = !openPath;
            if ((path.flag & 2) != 0) {
                QVector<Dwg_HATCH_PolylinePath> vertices;
                vertices.reserve(path.num_segs_or_paths);
                for (BITCODE_BL i = 0; i < path.num_segs_or_paths; ++i) {
                    vertices.append(path.polyline_paths[i]);
                }
                QVector<QPointF> points;
                if (!vertices.isEmpty()) {
                    points.append(point(vertices.first().point));
                    for (qsizetype i = 1; i < vertices.size(); ++i) {
                        appendBulge(points, point(vertices[i - 1].point),
                                    point(vertices[i].point),
                                    vertices[i - 1].bulge);
                    }
                    closedPath = path.closed && !openPath;
                    if (closedPath && !points.isEmpty()
                        && qAbs(vertices.last().bulge) > 1.0e-10) {
                        appendBulge(points, point(vertices.last().point),
                                    point(vertices.first().point),
                                    vertices.last().bulge);
                    }
                }
                if (points.size() >= 2) {
                    chains.append(std::move(points));
                }
            } else {
                for (BITCODE_BL i = 0; i < path.num_segs_or_paths; ++i) {
                    const Dwg_HATCH_PathSeg &segment = path.segs[i];
                    QVector<QPointF> piece;
                    if (segment.curve_type == 1) {
                        piece = {point(segment.first_endpoint),
                                 point(segment.second_endpoint)};
                    } else if (segment.curve_type == 2
                               && segment.radius > 0.0) {
                        double sweep =
                            segment.end_angle - segment.start_angle;
                        if (segment.is_ccw) {
                            while (sweep <= 0.0) {
                                sweep += 2.0 * kPi;
                            }
                        } else {
                            while (sweep >= 0.0) {
                                sweep -= 2.0 * kPi;
                            }
                        }
                        const int count = qBound(
                            4,
                            int(std::ceil(qAbs(sweep) / (kPi / 24.0))),
                            256);
                        piece.reserve(count + 1);
                        for (int sample = 0; sample <= count; ++sample) {
                            const double angle =
                                segment.start_angle
                                + sweep * double(sample) / double(count);
                            piece.append(
                                point(segment.center)
                                + QPointF(std::cos(angle),
                                          std::sin(angle))
                                    * segment.radius);
                        }
                    } else if (segment.curve_type == 4) {
                        if (segment.fitpts && segment.num_fitpts > 1) {
                            piece.reserve(segment.num_fitpts);
                            for (BITCODE_BL j = 0;
                                 j < segment.num_fitpts; ++j) {
                                piece.append(point(segment.fitpts[j]));
                            }
                        } else if (segment.control_points
                                   && segment.num_control_points > 1) {
                            piece.reserve(segment.num_control_points);
                            for (BITCODE_BL j = 0;
                                 j < segment.num_control_points; ++j) {
                                piece.append(
                                    point(segment.control_points[j].point));
                            }
                        }
                    }
                    appendConnectedPolylinePiece(chains,
                                                 std::move(piece));
                }
            }
            for (qsizetype chainIndex = 0;
                 chainIndex < chains.size(); ++chainIndex) {
                QVector<QPointF> points = chains[chainIndex];
                if (points.size() < 2) {
                    continue;
                }
                for (QPointF &p : points) {
                    p = ocsPoint(p, hatch->elevation, hatch->extrusion);
                }
                CadEntity out = commonEntity(object, insertLayer);
                out.type = CadEntityType::Solid;
                // Open HATCH boundaries and disconnected edge groups must
                // remain open. Closing them invents long diagonal edges that
                // do not exist in the DWG.
                out.closed = closedPath && chains.size() == 1;
                out.filled = hatch->is_solid_fill && !openPath;
                if (closedPath && chains.size() == 1) {
                    out.hatchGroup = hatchGroup;
                    out.hatchPattern = pattern;
                }
                out.points = std::move(points);
                append(std::move(out), transform);
            }
        }
    }

    void visitEntity(Dwg_Object *object, const Affine &transform, int depth,
                     const QString &insertLayer = {})
    {
        if (object) {
            ++m_typeHistogram[static_cast<int>(object->fixedtype)];
        }
        const auto *common = object ? object->tio.entity : nullptr;
        const bool isAttribute =
            object && object->fixedtype == DWG_TYPE_ATTRIB;
        bool visible = common && !common->invisible;
        if (isAttribute) {
            // ATTMODE: 0 = all hidden, 1 = normal (honour each ATTRIB flag),
            // 2 = all visible.
            if (m_attMode == 0) {
                visible = false;
            } else if (m_attMode == 2) {
                visible = true;
            }
        }
        if (!visible) {
            if (isAttribute) {
                ++m_attributeSuppressedVisits;
            }
            return;
        }
        if (isAttribute) {
            ++m_attributeRenderedVisits;
            if (common->invisible && m_attMode == 2) {
                ++m_attributeOverriddenHiddenVisits;
            }
        }
        switch (object->fixedtype) {
        case DWG_TYPE_LINE: {
            auto *line = dwg_object_to_LINE(object);
            if (!line) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Segment;
            out.points = {point(line->start), point(line->end)};
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_CIRCLE: {
            auto *circle = dwg_object_to_CIRCLE(object);
            if (!circle) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Circle;
            const QPointF localCenter = point(circle->center);
            out.center =
                ocsPoint(localCenter, circle->extrusion);
            out.radius = circle->radius;
            out.points =
                sampleArc(localCenter, out.radius, 0.0, 2.0 * kPi, true);
            for (QPointF &p : out.points) {
                p = ocsPoint(p, circle->extrusion);
            }
            out.closed = true;
            out.dimensionEdges.append(
                {out.center + QPointF(out.radius, 0.0),
                 2.0 * kPi * out.radius, true});
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_ARC: {
            auto *arc = dwg_object_to_ARC(object);
            if (!arc) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Arc;
            const QPointF localCenter = point(arc->center);
            out.center =
                ocsPoint(localCenter, arc->extrusion);
            out.radius = arc->radius;
            out.startAngle = arc->start_angle;
            out.endAngle = arc->end_angle;
            out.points = sampleArc(localCenter, out.radius,
                                   out.startAngle, out.endAngle);
            for (QPointF &p : out.points) {
                p = ocsPoint(p, arc->extrusion);
            }
            double sweep = out.endAngle - out.startAngle;
            while (sweep <= 0.0) {
                sweep += 2.0 * kPi;
            }
            if (!out.points.isEmpty()) {
                out.dimensionEdges.append(
                    {out.points.at(out.points.size() / 2),
                     out.radius * sweep, true});
            }
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_LWPOLYLINE: {
            auto *polyline = dwg_object_to_LWPOLYLINE(object);
            if (!polyline || !polyline->points
                || polyline->num_points == 0) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Polyline;
            // LibreDWG stores the native DWG flag here (512 = closed).
            // DXF group code 70 exposes that bit as 1 when exporting.
            out.closed = (polyline->flag & 512) != 0;
            QPointF previousOcs;
            bool havePrevious = false;
            for (BITCODE_BL i = 0; i < polyline->num_points; ++i) {
                const QPointF currentOcs = point(polyline->points[i]);
                const QPointF currentWcs =
                    ocsPoint(currentOcs, polyline->elevation,
                             polyline->extrusion);
                if (!havePrevious) {
                    out.points.append(currentWcs);
                } else {
                    const double bulge =
                        i < polyline->num_bulges && polyline->bulges
                        ? polyline->bulges[i - 1] : 0.0;
                    QVector<QPointF> local = {previousOcs};
                    appendBulge(local, previousOcs, currentOcs, bulge);
                    const QPointF previousWcs =
                        ocsPoint(previousOcs, polyline->elevation,
                                 polyline->extrusion);
                    for (int j = 1; j < local.size(); ++j) {
                        out.points.append(
                            ocsPoint(local[j], polyline->elevation,
                                     polyline->extrusion));
                    }
                    const bool isArc = qAbs(bulge) > 1.0e-10;
                    const double chord =
                        QLineF(previousWcs, currentWcs).length();
                    out.dimensionEdges.append(
                        {(previousWcs + currentWcs) * 0.5,
                         isArc ? qAbs(bulge) * 4.0 * chord : chord,
                         isArc});
                }
                previousOcs = currentOcs;
                havePrevious = true;
            }
            if (out.closed && havePrevious && polyline->bulges
                && polyline->num_bulges >= polyline->num_points) {
                const double closingBulge =
                    polyline->bulges[polyline->num_points - 1];
                if (qAbs(closingBulge) > 1.0e-10) {
                    QVector<QPointF> local = {previousOcs};
                    appendBulge(
                        local, previousOcs,
                        point(polyline->points[0]), closingBulge);
                    for (int j = 1; j < local.size(); ++j) {
                        out.points.append(
                            ocsPoint(local[j], polyline->elevation,
                                     polyline->extrusion));
                    }
                }
            }
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_POLYLINE_2D: {
            auto *polyline = dwg_object_to_POLYLINE_2D(object);
            if (polyline) {
                visitLegacyPolyline(object, polyline, transform,
                                    insertLayer);
            }
            break;
        }
        case DWG_TYPE_POLYLINE_3D: {
            auto *polyline = dwg_object_to_POLYLINE_3D(object);
            if (polyline) {
                visitPolyline3D(object, polyline, transform,
                                insertLayer);
            }
            break;
        }
        case DWG_TYPE_TEXT: {
            auto *text = dwg_object_to_TEXT(object);
            if (!text) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Text;
            out.text =
                cleanCadText(utf8Field(text, "TEXT", "text_value"));
            const bool useAlignmentPoint =
                text->horiz_alignment == 4
                || text->vert_alignment > 0;
            out.textPosition =
                ocsPoint(point(useAlignmentPoint
                                   ? text->alignment_pt
                                   : text->ins_pt),
                         text->elevation,
                         text->extrusion);
            if (text->horiz_alignment == 3
                || text->horiz_alignment == 5) {
                out.textPosition =
                    ocsPoint(point(text->ins_pt), text->elevation,
                             text->extrusion);
                out.textAlignmentPoint =
                    ocsPoint(point(text->alignment_pt),
                             text->elevation, text->extrusion);
                out.textFit = true;
                out.textRotation = qRadiansToDegrees(std::atan2(
                    text->alignment_pt.y - text->ins_pt.y,
                    text->alignment_pt.x - text->ins_pt.x));
            }
            out.points = {out.textPosition};
            out.textHeight = qMax(0.1, text->height);
            out.textRotation = qRadiansToDegrees(text->rotation);
            out.textWidthScale = qMax(0.05, text->width_factor);
            out.textHAlign = text->horiz_alignment;
            out.textVAlign = text->vert_alignment;
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_MTEXT: {
            auto *text = dwg_object_to_MTEXT(object);
            if (!text) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Text;
            out.text =
                cleanCadText(utf8Field(text, "MTEXT", "text"));
            // MTEXT insertion and X-axis direction are WCS vectors. Applying
            // OCS a second time mirrors text across the drawing for the common
            // negative-Z extrusion used by exploded plan details.
            out.textPosition = point(text->ins_pt);
            out.points = {out.textPosition};
            out.textHeight = qMax(0.1, text->text_height);
            out.textRotation = qRadiansToDegrees(
                std::atan2(text->x_axis_dir.y, text->x_axis_dir.x));
            out.textWidthScale = 1.0;
            out.textAttachment = text->attachment;
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_ATTRIB: {
            auto *text = dwg_object_to_ATTRIB(object);
            if (!text) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Text;
            out.text = cleanCadText(
                utf8Field(text, "ATTRIB", "text_value"));
            const bool useAlignmentPoint =
                text->horiz_alignment == 4
                || text->vert_alignment > 0;
            out.textPosition =
                ocsPoint(point(useAlignmentPoint
                                   ? text->alignment_pt
                                   : text->ins_pt),
                         text->elevation, text->extrusion);
            if (text->horiz_alignment == 3
                || text->horiz_alignment == 5) {
                out.textPosition =
                    ocsPoint(point(text->ins_pt), text->elevation,
                             text->extrusion);
                out.textAlignmentPoint =
                    ocsPoint(point(text->alignment_pt),
                             text->elevation, text->extrusion);
                out.textFit = true;
                out.textRotation = qRadiansToDegrees(std::atan2(
                    text->alignment_pt.y - text->ins_pt.y,
                    text->alignment_pt.x - text->ins_pt.x));
            }
            out.points = {out.textPosition};
            out.textHeight = qMax(0.1, text->height);
            out.textRotation = qRadiansToDegrees(text->rotation);
            out.textWidthScale = qMax(0.05, text->width_factor);
            out.textHAlign = text->horiz_alignment;
            out.textVAlign = text->vert_alignment;
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_LEADER: {
            auto *leader = dwg_object_to_LEADER(object);
            if (!leader || !leader->points || leader->num_points < 2) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Polyline;
            out.points.reserve(leader->num_points);
            for (BITCODE_BL i = 0; i < leader->num_points; ++i) {
                out.points.append(point(leader->points[i]));
            }
            if (leader->arrowhead_on && leader->num_points >= 2) {
                double arrowSize = 1.2;
                Dwg_Object *arrowHeader = nullptr;
                if (leader->dimstyle) {
                    int dimStyleError = 0;
                    Dwg_Object *styleObject = dwg_ref_get_object(
                        leader->dimstyle, &dimStyleError);
                    Dwg_Object_DIMSTYLE *style =
                        styleObject && !dimStyleError
                        ? dwg_object_to_DIMSTYLE(styleObject) : nullptr;
                    if (style && style->DIMASZ > 1.0e-9) {
                        arrowSize = style->DIMASZ
                            * qMax(1.0, style->DIMSCALE);
                    }
                    if (style && style->DIMLDRBLK) {
                        int arrowError = 0;
                        Dwg_Object *candidate = dwg_ref_get_object(
                            style->DIMLDRBLK, &arrowError);
                        if (candidate && !arrowError
                            && dwg_object_to_BLOCK_HEADER(candidate)) {
                            arrowHeader = candidate;
                        }
                    }
                }
                const QPointF tip = out.points[0];
                const QPointF direction = out.points[1] - tip;
                const double length =
                    std::hypot(direction.x(), direction.y());
                if (length > 1.0e-9) {
                    if (arrowHeader) {
                        const double angle =
                            std::atan2(direction.y(), direction.x())
                            + kPi;
                        const double cosine = std::cos(angle);
                        const double sine = std::sin(angle);
                        Affine arrowTransform;
                        arrowTransform.m11 = cosine * arrowSize;
                        arrowTransform.m12 = sine * arrowSize;
                        arrowTransform.m21 = -sine * arrowSize;
                        arrowTransform.m22 = cosine * arrowSize;
                        arrowTransform.dx = tip.x();
                        arrowTransform.dy = tip.y();
                        visitBlockHeader(
                            arrowHeader, arrowTransform.then(transform),
                            depth + 1,
                            commonEntity(object, insertLayer).layer);
                    }
                }
            }
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_INSERT: {
            ++m_insertSwitchVisited;
            visitInsert(object, dwg_object_to_INSERT(object),
                        transform, depth, insertLayer);
            break;
        }
        case DWG_TYPE_MINSERT: {
            auto *insert = dwg_object_to_MINSERT(object);
            if (insert) {
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
                compatible.num_owned = insert->num_owned;
                compatible.attribs = insert->attribs;
                visitInsert(object, &compatible, transform, depth,
                            insertLayer);
            }
            break;
        }
        case DWG_TYPE_ELLIPSE: {
            auto *ellipse = dwg_object_to_ELLIPSE(object);
            if (!ellipse) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Ellipse;
            // ELLIPSE center and major axis are WCS values. The extrusion
            // vector defines the minor-axis direction and parameter winding.
            const QPointF center = point(ellipse->center);
            const QPointF major = point(ellipse->sm_axis);
            const double normalX = ellipse->extrusion.x;
            const double normalY = ellipse->extrusion.y;
            const double normalZ = ellipse->extrusion.z;
            const QPointF minor(
                (-normalZ * major.y() + normalY * 0.0)
                    * ellipse->axis_ratio,
                (normalZ * major.x() - normalX * 0.0)
                    * ellipse->axis_ratio);
            double start = ellipse->start_angle;
            double end = ellipse->end_angle;
            out.closed = qAbs(end - start) < 1.0e-10;
            if (out.closed) {
                start = 0.0;
                end = 2.0 * kPi;
            } else {
                while (end <= start) {
                    end += 2.0 * kPi;
                }
            }
            const int count = 128;
            for (int i = 0; i < count; ++i) {
                const double t =
                    start + (end - start) * double(i) / count;
                out.points.append(center + major * std::cos(t)
                                  + minor * std::sin(t));
            }
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_VIEWPORT: {
            // Viewport frames are paper-space controls, not drawing geometry.
            // Their model contents are expanded lazily by CadView.
            break;
        }
        case DWG_TYPE_HATCH:
        case DWG_TYPE_MPOLYGON: {
            auto *hatch = dwg_object_to_HATCH(object);
            if (hatch) {
                visitHatch(object, hatch, transform, insertLayer);
            }
            break;
        }
        case DWG_TYPE_SPLINE: {
            auto *spline = dwg_object_to_SPLINE(object);
            if (!spline) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Polyline;
            out.closed = spline->closed_b || spline->periodic;
            const int count = int(spline->num_ctrl_pts);
            const int degree = qBound(
                1, int(spline->degree), qMax(1, count - 1));
            const bool hasNurbs =
                spline->ctrl_pts && spline->knots && count >= 2
                && int(spline->num_knots) >= count + degree + 1;
            if (hasNurbs) {
                struct WeightedPoint {
                    double x;
                    double y;
                    double w;
                };
                const auto evaluate = [&](double u, int span) {
                    std::vector<WeightedPoint> work;
                    work.reserve(degree + 1);
                    for (int j = 0; j <= degree; ++j) {
                        const int index =
                            qBound(0, span - degree + j, count - 1);
                        const Dwg_SPLINE_control_point &control =
                            spline->ctrl_pts[index];
                        const double weight =
                            spline->weighted && qAbs(control.w) > 1.0e-12
                            ? control.w : 1.0;
                        work.push_back({control.x * weight,
                                        control.y * weight, weight});
                    }
                    for (int r = 1; r <= degree; ++r) {
                        for (int j = degree; j >= r; --j) {
                            const int knotIndex = span - degree + j;
                            const double lower = spline->knots[knotIndex];
                            const double upper =
                                spline->knots[span + 1 + j - r];
                            const double denominator = upper - lower;
                            const double alpha =
                                qAbs(denominator) > 1.0e-15
                                ? (u - lower) / denominator : 0.0;
                            work[j].x =
                                (1.0 - alpha) * work[j - 1].x
                                + alpha * work[j].x;
                            work[j].y =
                                (1.0 - alpha) * work[j - 1].y
                                + alpha * work[j].y;
                            work[j].w =
                                (1.0 - alpha) * work[j - 1].w
                                + alpha * work[j].w;
                        }
                    }
                    const double weight = qAbs(work[degree].w) > 1.0e-15
                        ? work[degree].w : 1.0;
                    return QPointF(work[degree].x / weight,
                                   work[degree].y / weight);
                };
                for (int span = degree; span < count; ++span) {
                    const double start = spline->knots[span];
                    const double end = spline->knots[span + 1];
                    if (!std::isfinite(start) || !std::isfinite(end)
                        || end <= start) {
                        continue;
                    }
                    constexpr int samplesPerSpan = 12;
                    const int first = out.points.isEmpty() ? 0 : 1;
                    for (int sample = first;
                         sample <= samplesPerSpan; ++sample) {
                        const double u =
                            start + (end - start)
                                * double(sample) / samplesPerSpan;
                        out.points.append(evaluate(
                            sample == samplesPerSpan
                                ? std::nextafter(end, start) : u,
                            span));
                    }
                }
            } else if (spline->fit_pts && spline->num_fit_pts > 1) {
                QVector<QPointF> fit;
                fit.reserve(spline->num_fit_pts);
                for (BITCODE_BS i = 0; i < spline->num_fit_pts; ++i) {
                    fit.append(point(spline->fit_pts[i]));
                }
                constexpr int samplesPerSpan = 10;
                for (int i = 0; i + 1 < fit.size(); ++i) {
                    const QPointF p0 = fit[qMax(0, i - 1)];
                    const QPointF p1 = fit[i];
                    const QPointF p2 = fit[i + 1];
                    const QPointF p3 =
                        fit[qMin(fit.size() - 1, i + 2)];
                    const int first = out.points.isEmpty() ? 0 : 1;
                    for (int sample = first;
                         sample <= samplesPerSpan; ++sample) {
                        const double t =
                            double(sample) / samplesPerSpan;
                        const double t2 = t * t;
                        const double t3 = t2 * t;
                        out.points.append(
                            0.5
                            * (2.0 * p1
                               + (-p0 + p2) * t
                               + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3)
                                   * t2
                               + (-p0 + 3.0 * p1 - 3.0 * p2 + p3)
                                   * t3));
                    }
                }
            } else if (spline->ctrl_pts && count > 1) {
                for (int i = 0; i < count; ++i) {
                    out.points.append(
                        QPointF(spline->ctrl_pts[i].x,
                                spline->ctrl_pts[i].y));
                }
            }
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_REGION:
        case DWG_TYPE_BODY:
        case DWG_TYPE__3DSOLID: {
            auto *solid = dwg_object_to__3DSOLID(object);
            if (!solid || !solid->wires) {
                break;
            }
            for (BITCODE_BL wireIndex = 0;
                 wireIndex < solid->num_wires; ++wireIndex) {
                const Dwg_3DSOLID_wire &wire =
                    solid->wires[wireIndex];
                if (!wire.points || wire.num_points < 2) {
                    continue;
                }
                CadEntity out = commonEntity(object, insertLayer);
                out.type = CadEntityType::Polyline;
                out.closed = true;
                out.points.reserve(wire.num_points);
                for (BITCODE_BL i = 0; i < wire.num_points; ++i) {
                    out.points.append(point(wire.points[i]));
                }
                append(std::move(out), transform);
            }
            break;
        }
        case DWG_TYPE_SOLID: {
            Dwg_Entity_SOLID *solid =
                dwg_object_to_SOLID(object);
            if (!solid) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Solid;
            out.filled = true;
            out.closed = true;
            out.points = {
                ocsPoint(point(solid->corner1), solid->extrusion),
                ocsPoint(point(solid->corner2), solid->extrusion),
                ocsPoint(point(solid->corner4), solid->extrusion),
                ocsPoint(point(solid->corner3), solid->extrusion)
            };
            append(std::move(out), transform);
            break;
        }
        case DWG_TYPE_TRACE: {
            Dwg_Entity_TRACE *trace =
                dwg_object_to_TRACE(object);
            if (!trace) {
                break;
            }
            CadEntity out = commonEntity(object, insertLayer);
            out.type = CadEntityType::Solid;
            out.filled = true;
            out.closed = true;
            out.points = {
                ocsPoint(point(trace->corner1), trace->extrusion),
                ocsPoint(point(trace->corner2), trace->extrusion),
                ocsPoint(point(trace->corner4), trace->extrusion),
                ocsPoint(point(trace->corner3), trace->extrusion)
            };
            append(std::move(out), transform);
            break;
        }
        default: {
            const bool dimension =
                object->fixedtype >= DWG_TYPE_DIMENSION_ORDINATE
                && object->fixedtype <= DWG_TYPE_DIMENSION_ANG2LN;
            if (dimension) {
                Dwg_DIMENSION_common *dim =
                    dwg_object_to_DIMENSION(object);
                if (dim && dim->block) {
                    int error = 0;
                    Dwg_Object *header =
                        dwg_ref_get_object(dim->block, &error);
                    if (header && !error) {
                        visitBlockHeader(
                            header, transform, depth + 1,
                            commonEntity(object, insertLayer).layer);
                    }
                }
            }
            break;
        }
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
        if (bounds.isValid()) {
            bounds = bounds.adjusted(-bounds.width() * 0.03,
                                     -bounds.height() * 0.03,
                                     bounds.width() * 0.03,
                                     bounds.height() * 0.03);
        }
        return bounds;
    }

    static QRectF robustBounds(const QVector<CadEntity> &entities)
    {
        if (entities.size() < 20) {
            return calculateBounds(entities);
        }
        QVector<double> xs;
        QVector<double> ys;
        xs.reserve(entities.size());
        ys.reserve(entities.size());
        for (const CadEntity &entity : entities) {
            if (entity.bounds.isValid()) {
                xs.append(entity.bounds.center().x());
                ys.append(entity.bounds.center().y());
            }
        }
        if (xs.size() < 20) {
            return calculateBounds(entities);
        }
        std::sort(xs.begin(), xs.end());
        std::sort(ys.begin(), ys.end());
        const int low = xs.size() * 2 / 100;
        const int high = xs.size() * 98 / 100;
        const double centralWidth = xs[high] - xs[low];
        const double centralHeight = ys[high] - ys[low];
        const double minX = xs[low] - qMax(1.0, centralWidth * 0.08);
        const double maxX = xs[high] + qMax(1.0, centralWidth * 0.08);
        const double minY = ys[low] - qMax(1.0, centralHeight * 0.08);
        const double maxY = ys[high] + qMax(1.0, centralHeight * 0.08);
        const double maxDimension =
            qMax(1.0, qMax(centralWidth, centralHeight) * 8.0);
        QRectF result;
        for (const CadEntity &entity : entities) {
            if (!entity.bounds.isValid()) {
                continue;
            }
            const QPointF center = entity.bounds.center();
            if (center.x() < minX || center.x() > maxX
                || center.y() < minY || center.y() > maxY
                || entity.bounds.width() > maxDimension
                || entity.bounds.height() > maxDimension) {
                continue;
            }
            result = result.isNull() ? entity.bounds
                                     : result.united(entity.bounds);
        }
        if (!result.isValid()) {
            return calculateBounds(entities);
        }
        return result.adjusted(-result.width() * 0.03,
                               -result.height() * 0.03,
                               result.width() * 0.03,
                               result.height() * 0.03);
    }

    Dwg_Data &m_dwg;
    CadDocument &m_document;
    QString m_path;
    QVector<CadEntity> *m_target = &m_document.entities;
    QHash<uint32_t, int> m_layerHandles;
    QHash<quint64, QVector<Dwg_Object *>> m_attributesByOwner;
    QHash<int, quint64> m_typeHistogram;
    quint64 m_blocksVisited = 0;
    quint64 m_insertsVisited = 0;
    quint64 m_unresolvedInserts = 0;
    quint64 m_insertSwitchVisited = 0;
    quint64 m_blockArrayVisits = 0;
    quint64 m_attributeVisits = 0;
    quint64 m_attributeRenderedVisits = 0;
    quint64 m_attributeSuppressedVisits = 0;
    quint64 m_attributeOverriddenHiddenVisits = 0;
    int m_maxDepth = 0;
    int m_attMode = 0;
    bool m_mirrText = false;
    int m_nextHatchGroup = 0;
};

} // namespace

QString cleanDwgTextForDisplay(const QString &text)
{
    return cleanCadText(text);
}

bool loadDwgWithLibreDwg(const QString &path, CadDocument &document,
                         QString *errorMessage)
{
    QElapsedTimer loadTimer;
    loadTimer.start();
    Dwg_Data dwg;
    std::memset(&dwg, 0, sizeof(dwg));
    const QByteArray nativePath = QFile::encodeName(path);
    const int result = dwg_read_file(nativePath.constData(), &dwg);
    const qint64 decodeMilliseconds = loadTimer.restart();
    if (result >= DWG_ERR_CRITICAL) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("LibreDWG 无法读取该 DWG（错误码 %1）。")
                    .arg(result);
        }
        dwg_free(&dwg);
        return false;
    }
    LibreBuilder builder(dwg, document);
    builder.setPath(path);
    QString buildError;
    const bool success = builder.build(&buildError);
    const qint64 buildMilliseconds = loadTimer.elapsed();
    if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_DWG")) {
        std::fprintf(stderr, "reader-stage decode=%lldms build=%lldms\n",
                     static_cast<long long>(decodeMilliseconds),
                     static_cast<long long>(buildMilliseconds));
    }
    dwg_free(&dwg);
    if (!success && errorMessage) {
        *errorMessage = buildError.isEmpty()
            ? QStringLiteral("DWG 中没有可渲染的模型实体。")
            : buildError;
    }
    return success;
}
