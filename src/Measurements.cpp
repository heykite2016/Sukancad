#include "Measurements.h"

#include "core/cad_geometry.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <cmath>

namespace {

QString drawingCacheIdentity(const QString &drawingPath)
{
    const QFileInfo info(drawingPath);
    const QString stablePath =
        info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    const QByteArray fingerprint =
        QFile::encodeName(stablePath)
        + '|' + QByteArray::number(info.size())
        + '|' + QByteArray::number(
            info.lastModified().toUTC().toSecsSinceEpoch());
    return QString::fromLatin1(
        QCryptographicHash::hash(fingerprint,
                                 QCryptographicHash::Sha256)
            .toHex());
}

QString annotationCacheDirectory()
{
    QString directory =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (directory.isEmpty()) {
        directory = QDir::tempPath() + QStringLiteral("/CadLens");
    }
    return directory + QStringLiteral("/drawings");
}

CadPoint2 toC(const QPointF &point)
{
    return CadPoint2{point.x(), point.y()};
}

QJsonArray pointsToJson(const QVector<QPointF> &points)
{
    QJsonArray array;
    for (const QPointF &point : points) {
        array.append(QJsonArray{point.x(), point.y()});
    }
    return array;
}

QVector<QPointF> pointsFromJson(const QJsonArray &array)
{
    QVector<QPointF> points;
    points.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QJsonArray pair = value.toArray();
        if (pair.size() == 2) {
            points.append(QPointF(pair[0].toDouble(), pair[1].toDouble()));
        }
    }
    return points;
}

QString typeKey(MeasurementType type)
{
    switch (type) {
    case MeasurementType::Aligned:
        return QStringLiteral("aligned");
    case MeasurementType::Linear:
        return QStringLiteral("linear");
    case MeasurementType::Continuous:
        return QStringLiteral("continuous");
    case MeasurementType::Batch:
        return QStringLiteral("batch");
    case MeasurementType::Rectangle:
        return QStringLiteral("rectangle");
    case MeasurementType::Polygon:
        return QStringLiteral("polygon");
    case MeasurementType::CircleArea:
        return QStringLiteral("circle_area");
    case MeasurementType::FilledArea:
        return QStringLiteral("filled_area");
    case MeasurementType::SideArea:
        return QStringLiteral("side_area");
    case MeasurementType::AreaOffset:
        return QStringLiteral("area_offset");
    case MeasurementType::Radius:
        return QStringLiteral("radius");
    case MeasurementType::Diameter:
        return QStringLiteral("diameter");
    case MeasurementType::Angle:
        return QStringLiteral("angle");
    case MeasurementType::ArcLength:
        return QStringLiteral("arc_length");
    case MeasurementType::Coordinate:
        return QStringLiteral("coordinate");
    case MeasurementType::PointToLine:
        return QStringLiteral("point_to_line");
    case MeasurementType::FixedDistance:
        return QStringLiteral("fixed_distance");
    case MeasurementType::Text:
        return QStringLiteral("text");
    }
    return QStringLiteral("linear");
}

MeasurementType typeFromKey(const QString &key)
{
    if (key == QStringLiteral("aligned")) {
        return MeasurementType::Aligned;
    }
    if (key == QStringLiteral("continuous")) {
        return MeasurementType::Continuous;
    }
    if (key == QStringLiteral("batch")) {
        return MeasurementType::Batch;
    }
    if (key == QStringLiteral("rectangle")) {
        return MeasurementType::Rectangle;
    }
    if (key == QStringLiteral("polygon")) {
        return MeasurementType::Polygon;
    }
    if (key == QStringLiteral("circle_area")) {
        return MeasurementType::CircleArea;
    }
    if (key == QStringLiteral("filled_area")) {
        return MeasurementType::FilledArea;
    }
    if (key == QStringLiteral("side_area")) {
        return MeasurementType::SideArea;
    }
    if (key == QStringLiteral("area_offset")) {
        return MeasurementType::AreaOffset;
    }
    if (key == QStringLiteral("radius")) {
        return MeasurementType::Radius;
    }
    if (key == QStringLiteral("diameter")) {
        return MeasurementType::Diameter;
    }
    if (key == QStringLiteral("angle")) {
        return MeasurementType::Angle;
    }
    if (key == QStringLiteral("arc_length")) {
        return MeasurementType::ArcLength;
    }
    if (key == QStringLiteral("coordinate")) {
        return MeasurementType::Coordinate;
    }
    if (key == QStringLiteral("point_to_line")) {
        return MeasurementType::PointToLine;
    }
    if (key == QStringLiteral("fixed_distance")) {
        return MeasurementType::FixedDistance;
    }
    if (key == QStringLiteral("text")) {
        return MeasurementType::Text;
    }
    return MeasurementType::Linear;
}

} // namespace

QString defaultMeasurementId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

QString measurementTypeLabel(MeasurementType type)
{
    switch (type) {
    case MeasurementType::Aligned:
        return QStringLiteral("对齐尺寸");
    case MeasurementType::Linear:
        return QStringLiteral("线性尺寸");
    case MeasurementType::Continuous:
        return QStringLiteral("连续测量");
    case MeasurementType::Batch:
        return QStringLiteral("批量尺寸");
    case MeasurementType::Rectangle:
        return QStringLiteral("矩形面积");
    case MeasurementType::Polygon:
        return QStringLiteral("多边形面积");
    case MeasurementType::CircleArea:
        return QStringLiteral("圆面积");
    case MeasurementType::FilledArea:
        return QStringLiteral("填充面积");
    case MeasurementType::SideArea:
        return QStringLiteral("侧面积");
    case MeasurementType::AreaOffset:
        return QStringLiteral("偏移面积");
    case MeasurementType::Radius:
        return QStringLiteral("半径");
    case MeasurementType::Diameter:
        return QStringLiteral("直径");
    case MeasurementType::Angle:
        return QStringLiteral("角度");
    case MeasurementType::ArcLength:
        return QStringLiteral("弧长");
    case MeasurementType::Coordinate:
        return QStringLiteral("坐标");
    case MeasurementType::PointToLine:
        return QStringLiteral("点到直线");
    case MeasurementType::FixedDistance:
        return QStringLiteral("定距");
    case MeasurementType::Text:
        return QStringLiteral("文字注释");
    }
    return QStringLiteral("测量");
}

QStringList annotationStyleLabels()
{
    return {
        QStringLiteral("ISO 标准"),
        QStringLiteral("建筑斜线"),
        QStringLiteral("机械 ISO-25"),
        QStringLiteral("工程开口箭头"),
        QStringLiteral("小数圆点"),
        QStringLiteral("紧凑端线")
    };
}

QString annotationStyleKey(AnnotationStyle style)
{
    switch (style) {
    case AnnotationStyle::Standard:
        return QStringLiteral("standard");
    case AnnotationStyle::Architectural:
        return QStringLiteral("architectural");
    case AnnotationStyle::Mechanical:
        return QStringLiteral("mechanical");
    case AnnotationStyle::Engineering:
        return QStringLiteral("engineering");
    case AnnotationStyle::Decimal:
        return QStringLiteral("decimal");
    case AnnotationStyle::Compact:
        return QStringLiteral("compact");
    }
    return QStringLiteral("standard");
}

AnnotationStyle annotationStyleFromKey(const QString &key,
                                       AnnotationStyle fallback)
{
    if (key == QStringLiteral("standard")) {
        return AnnotationStyle::Standard;
    }
    if (key == QStringLiteral("architectural")) {
        return AnnotationStyle::Architectural;
    }
    if (key == QStringLiteral("mechanical")) {
        return AnnotationStyle::Mechanical;
    }
    if (key == QStringLiteral("engineering")) {
        return AnnotationStyle::Engineering;
    }
    if (key == QStringLiteral("decimal")) {
        return AnnotationStyle::Decimal;
    }
    if (key == QStringLiteral("compact")) {
        return AnnotationStyle::Compact;
    }
    return fallback;
}

AnnotationStyleMetrics annotationStyleMetrics(AnnotationStyle style)
{
    AnnotationStyleMetrics metrics;
    switch (style) {
    case AnnotationStyle::Standard:
        metrics.textSize = 13.0;
        metrics.arrowSize = 8.4;
        metrics.lineWidth = 1.35;
        metrics.extensionOffset = 4.0;
        metrics.extensionOverrun = 11.0;
        metrics.textGap = 11.0;
        metrics.marker = AnnotationMarker::FilledArrow;
        break;
    case AnnotationStyle::Architectural:
        metrics.textSize = 13.0;
        metrics.arrowSize = 7.0;
        metrics.lineWidth = 1.15;
        metrics.extensionOffset = 4.8;
        metrics.extensionOverrun = 12.5;
        metrics.textGap = 12.0;
        metrics.marker = AnnotationMarker::ArchitecturalTick;
        break;
    case AnnotationStyle::Mechanical:
        metrics.textSize = 13.0;
        metrics.arrowSize = 6.8;
        metrics.lineWidth = 1.1;
        metrics.extensionOffset = 3.0;
        metrics.extensionOverrun = 9.5;
        metrics.textGap = 9.0;
        metrics.marker = AnnotationMarker::NarrowFilledArrow;
        break;
    case AnnotationStyle::Engineering:
        metrics.textSize = 13.0;
        metrics.arrowSize = 10.0;
        metrics.lineWidth = 1.3;
        metrics.extensionOffset = 4.5;
        metrics.extensionOverrun = 13.0;
        metrics.textGap = 12.0;
        metrics.marker = AnnotationMarker::OpenArrow;
        break;
    case AnnotationStyle::Decimal:
        metrics.textSize = 13.0;
        metrics.arrowSize = 6.0;
        metrics.lineWidth = 1.2;
        metrics.extensionOffset = 3.8;
        metrics.extensionOverrun = 10.5;
        metrics.textGap = 10.0;
        metrics.marker = AnnotationMarker::Dot;
        break;
    case AnnotationStyle::Compact:
        metrics.textSize = 11.5;
        metrics.arrowSize = 5.8;
        metrics.lineWidth = 0.95;
        metrics.extensionOffset = 2.5;
        metrics.extensionOverrun = 8.0;
        metrics.textGap = 7.5;
        metrics.marker = AnnotationMarker::EndBar;
        break;
    }
    return metrics;
}

QString Measurement::typeLabel() const
{
    return measurementTypeLabel(type);
}

double Measurement::rawLength() const
{
    if (points.size() < 2) {
        return 0.0;
    }
    if (type == MeasurementType::FixedDistance && auxiliaryValue > 0.0) {
        return auxiliaryValue;
    }
    if (type == MeasurementType::Linear
        && (auxiliaryValue == 1.0 || auxiliaryValue == 2.0)) {
        return auxiliaryValue == 1.0
            ? qAbs(points[1].x() - points[0].x())
            : qAbs(points[1].y() - points[0].y());
    }
    if (type == MeasurementType::CircleArea && points.size() >= 2) {
        return cad_distance(toC(points[0]), toC(points[1])) * 2.0
            * 3.14159265358979323846;
    }
    if (type == MeasurementType::Rectangle
        || type == MeasurementType::Polygon
        || type == MeasurementType::FilledArea
        || type == MeasurementType::AreaOffset) {
        QVector<CadPoint2> converted;
        converted.reserve(points.size());
        for (const QPointF &point : points) {
            converted.append(toC(point));
        }
        return cad_polyline_length(converted.constData(), converted.size(), 1);
    }
    if (type == MeasurementType::Continuous) {
        QVector<CadPoint2> converted;
        converted.reserve(points.size());
        for (const QPointF &point : points) {
            converted.append(toC(point));
        }
        return cad_polyline_length(converted.constData(), converted.size(), 0);
    }
    if (type == MeasurementType::Radius) {
        return cad_distance(toC(points[0]), toC(points[1]));
    }
    if (type == MeasurementType::Diameter) {
        return cad_distance(toC(points[0]), toC(points[1])) * 2.0;
    }
    if (type == MeasurementType::ArcLength && points.size() >= 3) {
        int valid = 0;
        return cad_arc_length_from_three_points(
            toC(points[0]), toC(points[1]), toC(points[2]), &valid);
    }
    if (type == MeasurementType::PointToLine && points.size() >= 3) {
        const QPointF a = points[1];
        const QPointF b = points[2];
        const QPointF delta = b - a;
        const double length = std::hypot(delta.x(), delta.y());
        if (length <= 1.0e-12) {
            return 0.0;
        }
        return qAbs(delta.x() * (a.y() - points[0].y())
                    - (a.x() - points[0].x()) * delta.y())
            / length;
    }
    return cad_distance(toC(points[0]), toC(points[1]));
}

double Measurement::rawArea() const
{
    if (type == MeasurementType::CircleArea && points.size() >= 2) {
        const double radius = cad_distance(toC(points[0]), toC(points[1]));
        return 3.14159265358979323846 * radius * radius;
    }
    if (type == MeasurementType::SideArea) {
        return rawLength() * auxiliaryValue;
    }
    if (type != MeasurementType::Rectangle
        && type != MeasurementType::Polygon
        && type != MeasurementType::FilledArea
        && type != MeasurementType::AreaOffset) {
        return 0.0;
    }
    QVector<CadPoint2> converted;
    converted.reserve(points.size());
    for (const QPointF &point : points) {
        converted.append(toC(point));
    }
    return cad_polygon_area(converted.constData(), converted.size());
}

QString Measurement::primaryValue(const CadDocument &document,
                                  const DisplaySettings &settings) const
{
    switch (type) {
    case MeasurementType::Rectangle:
    case MeasurementType::Polygon:
    case MeasurementType::FilledArea:
    case MeasurementType::AreaOffset:
        return QStringLiteral("面积：%1\n周长：%2")
            .arg(formatAreaValue(rawArea(), document, settings.areaUnit,
                                 settings.areaPrecision),
                 formatDimensionValue(rawLength(), document,
                                      settings.lengthUnit,
                                      settings.lengthPrecision));
    case MeasurementType::CircleArea:
        if (points.size() >= 2) {
            const double radius =
                QLineF(points[0], points[1]).length();
            const QString centerX = formatDimensionValue(
                points[0].x(), document, settings.coordinateUnit,
                settings.coordinatePrecision);
            const QString centerY = formatDimensionValue(
                points[0].y(), document, settings.coordinateUnit,
                settings.coordinatePrecision);
            return QStringLiteral(
                       "圆心：（%1，%2）\n半径：%3\n面积：%4\n周长：%5")
                .arg(centerX, centerY,
                     formatDimensionValue(
                         radius, document, settings.lengthUnit,
                         settings.lengthPrecision),
                     formatAreaValue(rawArea(), document,
                                     settings.areaUnit,
                                     settings.areaPrecision),
                     formatDimensionValue(
                         rawLength(), document, settings.lengthUnit,
                         settings.lengthPrecision));
        }
        break;
    case MeasurementType::SideArea:
        return QStringLiteral("面积：%1\n长度：%2")
            .arg(formatAreaValue(rawArea(), document, settings.areaUnit,
                                 settings.areaPrecision),
                 formatDimensionValue(rawLength(), document,
                                      settings.lengthUnit,
                                      settings.lengthPrecision));
    case MeasurementType::Angle:
        if (points.size() >= 3) {
            return QStringLiteral("∠%1°").arg(
                cad_angle_between_degrees(toC(points[0]), toC(points[1]),
                                          toC(points[2])),
                0, 'f', 2);
        }
        break;
    case MeasurementType::Radius:
        return QStringLiteral("R%1")
            .arg(formatDimensionValue(rawLength(), document,
                                      settings.lengthUnit,
                                      settings.lengthPrecision));
    case MeasurementType::Diameter:
        return QStringLiteral("⌀%1")
            .arg(formatDimensionValue(rawLength(), document,
                                      settings.lengthUnit,
                                      settings.lengthPrecision));
    case MeasurementType::ArcLength:
        return QStringLiteral("弧长：%1")
            .arg(formatDimensionValue(rawLength(), document,
                                      settings.lengthUnit,
                                      settings.lengthPrecision));
    case MeasurementType::PointToLine:
        return QStringLiteral("距离：%1")
            .arg(formatDimensionValue(rawLength(), document,
                                      settings.lengthUnit,
                                      settings.lengthPrecision));
    case MeasurementType::FixedDistance:
        return QStringLiteral("定距：%1")
            .arg(formatDimensionValue(rawLength(), document,
                                      settings.lengthUnit,
                                      settings.lengthPrecision));
    case MeasurementType::Text:
        return note.isEmpty() ? QStringLiteral("文字注释") : note;
    case MeasurementType::Coordinate:
        if (!points.isEmpty()) {
            const QPointF point = points.first();
            const double factor = document.metersPerDrawingUnit
                / measuringUnitMeters(settings.coordinateUnit);
            return QStringLiteral("X=%1  Y=%2")
                .arg(point.x() * factor, 0, 'f',
                     settings.coordinatePrecision)
                .arg(point.y() * factor, 0, 'f',
                     settings.coordinatePrecision);
        }
        break;
    case MeasurementType::Continuous:
        return QStringLiteral("总长：%1（%2 段）")
            .arg(formatDimensionValue(rawLength(), document,
                                      settings.lengthUnit,
                                      settings.lengthPrecision))
            .arg(qMax(0, points.size() - 1));
    default:
        break;
    }
    return formatDimensionValue(rawLength(), document, settings.lengthUnit,
                                settings.lengthPrecision);
}

QJsonObject Measurement::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("space"), space);
    object.insert(QStringLiteral("type"), typeKey(type));
    object.insert(QStringLiteral("points"), pointsToJson(points));
    object.insert(QStringLiteral("label"),
                  QJsonArray{labelPosition.x(), labelPosition.y()});
    object.insert(QStringLiteral("note"), note);
    QJsonArray edgeArcLengths;
    for (double length : this->edgeArcLengths) {
        edgeArcLengths.append(length);
    }
    object.insert(QStringLiteral("edgeArcLengths"), edgeArcLengths);
    QJsonArray edgeLengths;
    for (double length : this->edgeLengths) {
        edgeLengths.append(length);
    }
    object.insert(QStringLiteral("edgeLengths"), edgeLengths);
    object.insert(QStringLiteral("edgeMidpoints"),
                  pointsToJson(edgeMidpoints));
    object.insert(QStringLiteral("color"), color.name(QColor::HexArgb));
    object.insert(QStringLiteral("visible"), visible);
    object.insert(QStringLiteral("auxiliaryValue"), auxiliaryValue);
    return object;
}

Measurement Measurement::fromJson(const QJsonObject &object)
{
    Measurement measurement;
    measurement.id = object.value(QStringLiteral("id")).toString();
    if (measurement.id.isEmpty()) {
        measurement.id = defaultMeasurementId();
    }
    measurement.space =
        object.value(QStringLiteral("space"))
            .toString(QStringLiteral("模型"));
    measurement.type =
        typeFromKey(object.value(QStringLiteral("type")).toString());
    measurement.points =
        pointsFromJson(object.value(QStringLiteral("points")).toArray());
    const QJsonArray label = object.value(QStringLiteral("label")).toArray();
    if (label.size() == 2) {
        measurement.labelPosition =
            QPointF(label[0].toDouble(), label[1].toDouble());
    } else if (!measurement.points.isEmpty()) {
        measurement.labelPosition = measurement.points.first();
    }
    measurement.note = object.value(QStringLiteral("note")).toString();
    for (const QJsonValue &value :
         object.value(QStringLiteral("edgeArcLengths")).toArray()) {
        measurement.edgeArcLengths.append(value.toDouble());
    }
    for (const QJsonValue &value :
         object.value(QStringLiteral("edgeLengths")).toArray()) {
        measurement.edgeLengths.append(value.toDouble());
    }
    measurement.edgeMidpoints = pointsFromJson(
        object.value(QStringLiteral("edgeMidpoints")).toArray());
    const QColor savedColor(
        object.value(QStringLiteral("color")).toString());
    if (savedColor.isValid()) {
        measurement.color = savedColor;
    }
    measurement.visible =
        object.value(QStringLiteral("visible")).toBool(true);
    measurement.auxiliaryValue =
        object.value(QStringLiteral("auxiliaryValue")).toDouble();
    return measurement;
}

QString annotationSidecarPath(const QString &drawingPath)
{
    return annotationCacheDirectory() + QLatin1Char('/')
        + drawingCacheIdentity(drawingPath)
        + QStringLiteral(".cadlens.json");
}

QString annotationAutosavePath(const QString &drawingPath)
{
    return annotationCacheDirectory() + QLatin1Char('/')
        + drawingCacheIdentity(drawingPath)
        + QStringLiteral(".cadlens.autosave.json");
}

namespace {

bool writeAnnotationFile(const QString &drawingPath,
                         const QString &annotationPath,
                         const CadDocument &document,
                         const DisplaySettings &settings,
                         const QVector<Measurement> &measurements,
                         QString *errorMessage)
{
    QJsonArray annotations;
    for (const Measurement &measurement : measurements) {
        annotations.append(measurement.toJson());
    }
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("cadlens.annotations"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("drawing"),
                QFileInfo(drawingPath).fileName());
    root.insert(QStringLiteral("annotationStyle"),
                annotationStyleKey(settings.annotationStyle));
    root.insert(QStringLiteral("metersPerDrawingUnit"),
                document.metersPerDrawingUnit);
    root.insert(QStringLiteral("calibrated"), document.calibrated);
    root.insert(QStringLiteral("displayUnit"),
                static_cast<int>(settings.lengthUnit));
    root.insert(QStringLiteral("lengthUnit"),
                static_cast<int>(settings.lengthUnit));
    root.insert(QStringLiteral("areaUnit"),
                static_cast<int>(settings.areaUnit));
    root.insert(QStringLiteral("coordinateUnit"),
                static_cast<int>(settings.coordinateUnit));
    root.insert(QStringLiteral("lengthPrecision"),
                settings.lengthPrecision);
    root.insert(QStringLiteral("areaPrecision"), settings.areaPrecision);
    root.insert(QStringLiteral("coordinatePrecision"),
                settings.coordinatePrecision);
    root.insert(QStringLiteral("annotationColor"),
                settings.annotationColor.name(QColor::HexArgb));
    root.insert(QStringLiteral("measurements"), annotations);

    if (!QDir().mkpath(QFileInfo(annotationPath).absolutePath())) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("无法创建系统缓存目录。");
        }
        return false;
    }
    QSaveFile file(annotationPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented))
        < 0 || !file.commit()) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    return true;
}

} // namespace

bool saveAnnotations(const QString &drawingPath, const CadDocument &document,
                     const DisplaySettings &settings,
                     const QVector<Measurement> &measurements,
                     QString *errorMessage)
{
    return writeAnnotationFile(
        drawingPath, annotationSidecarPath(drawingPath), document, settings,
        measurements, errorMessage);
}

bool saveAnnotationsAutosave(
    const QString &drawingPath, const CadDocument &document,
    const DisplaySettings &settings,
    const QVector<Measurement> &measurements,
    QString *errorMessage)
{
    return writeAnnotationFile(
        drawingPath, annotationAutosavePath(drawingPath), document, settings,
        measurements, errorMessage);
}

bool loadAnnotations(const QString &drawingPath, CadDocument &document,
                     DisplaySettings *settings,
                     QVector<Measurement> *measurements,
                     QString *errorMessage)
{
    QString sourcePath = annotationAutosavePath(drawingPath);
    if (!QFileInfo::exists(sourcePath)) {
        sourcePath = annotationSidecarPath(drawingPath);
    }
    bool legacySource = false;
    if (!QFileInfo::exists(sourcePath)) {
        sourcePath =
            drawingPath + QStringLiteral(".cadlens.autosave.json");
        legacySource = QFileInfo::exists(sourcePath);
    }
    if (!legacySource && !QFileInfo::exists(sourcePath)) {
        sourcePath = drawingPath + QStringLiteral(".cadlens.json");
        legacySource = QFileInfo::exists(sourcePath);
    }
    QFile file(sourcePath);
    if (!file.exists()) {
        measurements->clear();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(),
                                                       &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        if (errorMessage) {
            *errorMessage = parseError.errorString();
        }
        return false;
    }
    const QJsonObject root = json.object();
    const double meters =
        root.value(QStringLiteral("metersPerDrawingUnit"))
            .toDouble(document.metersPerDrawingUnit);
    if (meters > 0.0 && std::isfinite(meters)) {
        document.metersPerDrawingUnit = meters;
    }
    document.calibrated =
        root.value(QStringLiteral("calibrated")).toBool(document.calibrated);
    if (settings) {
        const int legacyUnit =
            root.value(QStringLiteral("displayUnit"))
                .toInt(static_cast<int>(settings->lengthUnit));
        const int lengthUnit =
            root.value(QStringLiteral("lengthUnit")).toInt(legacyUnit);
        const int areaUnit =
            root.value(QStringLiteral("areaUnit"))
                .toInt(static_cast<int>(MeasuringUnit::Meter));
        const int coordinateUnit =
            root.value(QStringLiteral("coordinateUnit")).toInt(legacyUnit);
        settings->annotationStyle = annotationStyleFromKey(
            root.value(QStringLiteral("annotationStyle"))
                .toString(),
            settings->annotationStyle);
        const auto validUnit = [](int unit) {
            return unit >= static_cast<int>(MeasuringUnit::Millimeter)
                && unit <= static_cast<int>(MeasuringUnit::Foot);
        };
        if (validUnit(lengthUnit)) {
            settings->lengthUnit = static_cast<MeasuringUnit>(lengthUnit);
        }
        if (validUnit(areaUnit)) {
            settings->areaUnit = static_cast<MeasuringUnit>(areaUnit);
        }
        if (validUnit(coordinateUnit)) {
            settings->coordinateUnit =
                static_cast<MeasuringUnit>(coordinateUnit);
        }
        settings->lengthPrecision = qBound(
            0, root.value(QStringLiteral("lengthPrecision"))
                   .toInt(settings->lengthPrecision),
            6);
        settings->areaPrecision = qBound(
            0, root.value(QStringLiteral("areaPrecision"))
                   .toInt(settings->areaPrecision),
            6);
        settings->coordinatePrecision = qBound(
            0, root.value(QStringLiteral("coordinatePrecision"))
                   .toInt(settings->coordinatePrecision),
            6);
        const QColor color(
            root.value(QStringLiteral("annotationColor")).toString());
        if (color.isValid()) {
            settings->annotationColor = color;
        }
    }
    measurements->clear();
    for (const QJsonValue &value :
         root.value(QStringLiteral("measurements")).toArray()) {
        if (value.isObject()) {
            measurements->append(Measurement::fromJson(value.toObject()));
        }
    }
    if (legacySource) {
        const DisplaySettings cachedDefaults;
        writeAnnotationFile(
            drawingPath, annotationAutosavePath(drawingPath), document,
            settings ? *settings : cachedDefaults, *measurements,
            nullptr);
    }
    return true;
}
