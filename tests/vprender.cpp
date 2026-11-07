// Offscreen viewport renderer for diagnosis: loads a DWG, renders every
// layout viewport (and model space) to PNG files so rendering defects can
// be inspected without the GUI.
#include "CadView.h"
#include "CadModel.h"
#include "LibreDwgReader.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <cstdio>
#include <memory>
#include <algorithm>

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: vprender file.dwg outPrefix [vpIndex ...]\n");
        return 1;
    }
    auto document = std::make_shared<CadDocument>();
    QString error;
    const QString path = QString::fromUtf8(argv[1]);
    // DWG uses the primary LibreDWG reader directly (stable benchmark
    // baseline); DXF must go through CadDocument::load so it takes the
    // libdxfrw backend, exactly like the application does.
    const bool isDxf =
        path.endsWith(QStringLiteral(".dxf"), Qt::CaseInsensitive);
    const bool loaded =
        isDxf ? document->load(path, &error)
              : loadDwgWithLibreDwg(path, *document, &error);
    if (!loaded) {
        std::fprintf(stderr, "load failed: %s\n",
                     error.toUtf8().constData());
        return 1;
    }
    if (qEnvironmentVariableIsSet("VPRENDER_DOC_STATS")) {
        std::fprintf(stderr,
                     "doc: backend=%s format=%s layouts=%lld entities=%lld "
                     "layoutViewports=%lld loadMs=%lld\n",
                     document->sourceBackend.toUtf8().constData(),
                     document->sourceFormat.toUtf8().constData(),
                     static_cast<long long>(document->layouts.size()),
                     static_cast<long long>(document->entities.size()),
                     static_cast<long long>(
                         document->layoutViewports.size()),
                     static_cast<long long>(document->loadMilliseconds));
    }
    // Diagnostic: text entity counts, duplicate (text, position) pairs and
    // the effective character-width factor. Duplicates reveal an ATTRIB
    // being emitted both as a block child and as an INSERT attribute, and
    // a width factor of ~0.05 means a rectangular MTEXT width leaked into
    // DRW_Text::widthscale via DXF group 41.
    if (qEnvironmentVariableIsSet("VPRENDER_TEXT_STATS")) {
        const auto dumpTexts = [](const char *label,
                                  const QVector<CadEntity> &list) {
            QHash<QString, int> duplicates;
            QHash<int, int> stretch;
            QHash<int, int> attachments;
            QHash<int, int> hAligns;
            QHash<int, int> vAligns;
            int texts = 0;
            int fitted = 0;
            int alignedPoints = 0;
            for (const CadEntity &entity : list) {
                if (entity.type != CadEntityType::Text) {
                    continue;
                }
                ++texts;
                ++attachments[entity.textAttachment];
                ++hAligns[entity.textHAlign];
                ++vAligns[entity.textVAlign];
                if (entity.textFit) {
                    ++fitted;
                }
                if (!entity.textAlignmentPoint.isNull()) {
                    ++alignedPoints;
                }
                ++stretch[qBound(0, int(entity.textWidthScale * 100.0),
                                 400)];
                const QString key =
                    entity.text + QLatin1Char('|')
                    + QString::number(entity.textPosition.x(), 'f', 1)
                    + QLatin1Char('|')
                    + QString::number(entity.textPosition.y(), 'f', 1);
                ++duplicates[key];
            }
            int duplicateGroups = 0;
            int extraDraws = 0;
            QVector<QPair<int, QString>> top;
            for (auto it = duplicates.constBegin();
                 it != duplicates.constEnd(); ++it) {
                if (it.value() > 1) {
                    ++duplicateGroups;
                    extraDraws += it.value() - 1;
                    top.append({it.value(), it.key()});
                }
            }
            std::sort(top.begin(), top.end(),
                      [](const QPair<int, QString> &a,
                         const QPair<int, QString> &b) {
                          return a.first > b.first;
                      });
            QVector<QPair<int, int>> stretchTop;
            for (auto it = stretch.constBegin(); it != stretch.constEnd();
                 ++it) {
                stretchTop.append({it.key(), it.value()});
            }
            std::sort(stretchTop.begin(), stretchTop.end(),
                      [](const QPair<int, int> &a,
                         const QPair<int, int> &b) {
                          return a.second > b.second;
                      });
            std::fprintf(stderr,
                         "[text] %s entities=%lld texts=%d dupGroups=%d "
                         "extraDraws=%d\n",
                         label, static_cast<long long>(list.size()), texts,
                         duplicateGroups, extraDraws);
            std::fprintf(stderr, "[text]   widthScale:");
            for (int i = 0; i < qMin(8, stretchTop.size()); ++i) {
                std::fprintf(stderr, " %d%%x%d", stretchTop[i].first,
                             stretchTop[i].second);
            }
            std::fprintf(stderr, "\n");
            const auto dumpHistogram = [](const char *label,
                                          const QHash<int, int> &values) {
                QVector<QPair<int, int>> sorted;
                for (auto it = values.constBegin(); it != values.constEnd();
                     ++it) {
                    sorted.append({it.key(), it.value()});
                }
                std::sort(sorted.begin(), sorted.end(),
                          [](const QPair<int, int> &a,
                             const QPair<int, int> &b) {
                              return a.second > b.second;
                          });
                std::fprintf(stderr, "[text]   %s:", label);
                for (int i = 0; i < qMin(8, sorted.size()); ++i) {
                    std::fprintf(stderr, " %d x%d", sorted[i].first,
                                 sorted[i].second);
                }
                std::fprintf(stderr, "\n");
            };
            dumpHistogram("attachment", attachments);
            dumpHistogram("hAlign", hAligns);
            dumpHistogram("vAlign", vAligns);
            std::fprintf(stderr, "[text]   fitted=%d alignmentPoints=%d\n",
                         fitted, alignedPoints);
            for (int i = 0; i < qMin(5, top.size()); ++i) {
                std::fprintf(stderr, "[text]   dup x%d %s\n", top[i].first,
                             top[i].second.toUtf8().constData());
            }
        };
        dumpTexts("model", document->entities);
        for (auto it = document->layoutEntities.constBegin();
             it != document->layoutEntities.constEnd(); ++it) {
            dumpTexts(it.key().toUtf8().constData(), it.value());
        }
        for (auto it = document->layoutViewports.constBegin();
             it != document->layoutViewports.constEnd(); ++it) {
            std::fprintf(stderr, "[vp] layout='%s' viewports=%lld",
                         it.key().toUtf8().constData(),
                         static_cast<long long>(it.value().size()));
            for (int i = 0; i < it.value().size(); ++i) {
                std::fprintf(stderr, " |%d:frozen=%lld/%lld,model=%d",
                             i,
                             static_cast<long long>(
                                 it.value()[i].frozenLayerIndices.size()),
                             static_cast<long long>(
                                 it.value()[i].frozenLayers.size()),
                             it.value()[i].revealsModel ? 1 : 0);
            }
            std::fprintf(stderr, "\n");
            for (int i = 0; i < it.value().size(); ++i) {
                QStringList names;
                for (const QString &name :
                     std::as_const(it.value()[i].frozenLayers)) {
                    names.append(name);
                }
                std::sort(names.begin(), names.end());
                QStringList indices;
                for (int index :
                     std::as_const(it.value()[i].frozenLayerIndices)) {
                    const QString resolved =
                        index >= 0 && index < document->layers.size()
                            ? document->layers[index].name
                            : QStringLiteral("?");
                    indices.append(QString::number(index)
                                   + QLatin1Char(':') + resolved);
                }
                std::sort(indices.begin(), indices.end());
                std::fprintf(stderr,
                             "[vp]   %d names=[%s] indices=[%s]\n", i,
                             names.join(QStringLiteral(", "))
                                 .toUtf8().constData(),
                             indices.join(QStringLiteral(", "))
                                 .toUtf8().constData());
            }
        }
        QHash<int, int> layerHistogram;
        for (const CadEntity &entity :
             std::as_const(document->entities)) {
            ++layerHistogram[entity.layerIndex];
        }
        QVector<QPair<int, int>> layerTop;
        for (auto it = layerHistogram.constBegin();
             it != layerHistogram.constEnd(); ++it) {
            layerTop.append({it.key(), it.value()});
        }
        std::sort(layerTop.begin(), layerTop.end(),
                  [](const QPair<int, int> &a, const QPair<int, int> &b) {
                      return a.second > b.second;
                  });
        std::fprintf(stderr, "[layer] model layerIndex top:");
        for (int i = 0; i < qMin(12, layerTop.size()); ++i) {
            const int index = layerTop[i].first;
            const QString name =
                index >= 0 && index < document->layers.size()
                    ? document->layers[index].name
                    : QStringLiteral("?");
            std::fprintf(stderr, " %d:%s=%d", index,
                         name.toUtf8().constData(), layerTop[i].second);
        }
        std::fprintf(stderr, "\n");
    }
    // VPRENDER_LOAD_ONLY checks that the file parses without rendering any
    // viewports: used to sweep a whole folder of drawings quickly.
    if (qEnvironmentVariableIsSet("VPRENDER_LOAD_ONLY")) {
        return 0;
    }
    const QString prefix = QString::fromUtf8(argv[2]);

    CadView view;
    view.resize(1600, 1000);
    view.setDocument(document);

    // Render the whole layout page (all viewports at once) so viewport
    // placement and cross-viewport leakage can be inspected directly.
    if (qEnvironmentVariableIsSet("VPRENDER_LAYOUT_OVERVIEW")) {
        const int ovW = qgetenv("VPRENDER_OVERVIEW_W").toInt();
        const int ovH = qgetenv("VPRENDER_OVERVIEW_H").toInt();
        const QSize ovSize(ovW > 0 ? ovW : 2400, ovH > 0 ? ovH : 1600);
        for (const QString &layout : document->layouts) {
            view.setActiveSpace(layout);
            std::fprintf(stderr,
                         "[ov] layout='%s' bounds=(%.1f,%.1f)-(%.1f,%.1f) "
                         "img=%dx%d\n",
                         layout.toUtf8().constData(),
                         document->bounds.left(), document->bounds.top(),
                         document->bounds.right(), document->bounds.bottom(),
                         ovSize.width(), ovSize.height());
            const QImage image = view.renderToImage(ovSize);
            const QString path = prefix + QStringLiteral("_overview_")
                                 + layout + QStringLiteral(".png");
            image.save(path);
            std::fprintf(stderr, "saved %s\n",
                         path.toUtf8().constData());
        }
        return 0;
    }

    // VPRENDER_COMPARE_CLIP=1 renders one viewport twice in the SAME
    // process (hash seeds identical), toggling only the hatch clip path,
    // so the two PNGs can be compared byte-for-byte without QHash ordering
    // noise from separate runs.
    if (qEnvironmentVariableIsSet("VPRENDER_COMPARE_CLIP")) {
        for (auto it = document->layoutViewports.constBegin();
             it != document->layoutViewports.constEnd(); ++it) {
            const QString layout = it.key();
            if (!document->layoutEntities.contains(layout)) {
                continue;
            }
            view.setActiveSpace(layout);
            const QVector<CadViewportDefinition> &viewports = it.value();
            for (int i = 0; i < viewports.size(); ++i) {
                if (!viewports[i].revealsModel || i != 11) {
                    continue;
                }
                const QRectF paper(
                    viewports[i].center
                        - QPointF(viewports[i].windowSize.width() * 0.5,
                                  viewports[i].windowSize.height() * 0.5),
                    viewports[i].windowSize);
                qunsetenv("CADLENS_HATCH_FORCE_PATH_CLIP");
                view.invalidateDrawingCache();
                const QImage fast =
                    view.renderWorldRectToImage(paper, QSize(1400, 900));
                qputenv("CADLENS_HATCH_FORCE_PATH_CLIP", "1");
                view.invalidateDrawingCache();
                const QImage exact =
                    view.renderWorldRectToImage(paper, QSize(1400, 900));
                fast.save(prefix + QStringLiteral("_clip_fast.png"));
                exact.save(prefix + QStringLiteral("_clip_exact.png"));
                std::fprintf(stderr, "saved clip comparison for vp11\n");
                return 0;
            }
        }
        return 1;
    }

    // VPRENDER_BENCH=N forces a full geometry rebuild for up to N layout
    // viewports (the real views a user pans/zooms in) and reports the stage
    // profile. Without invalidating, renderWorldRectToImage would hit the
    // cached bitmap on every call and measure nothing.
    const QByteArray bench = qgetenv("VPRENDER_BENCH");
    if (!bench.isEmpty()) {
        const int limit = qMax(1, bench.toInt());
        const QByteArray benchVp = qgetenv("VPRENDER_BENCH_VP");
        const int onlyVp = benchVp.isEmpty() ? -1 : benchVp.toInt();
        QElapsedTimer timer;
        int measured = 0;
        for (auto it = document->layoutViewports.constBegin();
             it != document->layoutViewports.constEnd() && measured < limit;
             ++it) {
            const QString layout = it.key();
            if (!document->layoutEntities.contains(layout)) {
                continue;
            }
            view.setActiveSpace(layout);
            const QVector<CadViewportDefinition> &viewports = it.value();
            for (int i = 0; i < viewports.size() && measured < limit; ++i) {
                const CadViewportDefinition &vp = viewports[i];
                if (!vp.revealsModel || (onlyVp >= 0 && i != onlyVp)) {
                    continue;
                }
                const QRectF paper(
                    vp.center
                        - QPointF(vp.windowSize.width() * 0.5,
                                  vp.windowSize.height() * 0.5),
                    vp.windowSize);
                view.invalidateDrawingCache();
                timer.restart();
                const QImage image =
                    view.renderWorldRectToImage(paper, QSize(1400, 900));
                std::fprintf(stderr,
                             "bench layout='%s' vp=%d total=%.1fms\n",
                             layout.toUtf8().constData(), i,
                             timer.nsecsElapsed() / 1.0e6);
                Q_UNUSED(image)
                ++measured;
            }
        }
        return 0;
    }

    // Model space overview
    view.setActiveSpace(QStringLiteral("模型"));
    QImage model = view.renderToImage(QSize(1600, 1000));
    model.save(prefix + QStringLiteral("_model.png"));
    std::fprintf(stderr, "saved %s_model.png\n", prefix.toUtf8().constData());

    for (auto it = document->layoutViewports.constBegin();
         it != document->layoutViewports.constEnd(); ++it) {
        const QString layout = it.key();
        if (!document->layoutEntities.contains(layout)) {
            continue;
        }
        view.setActiveSpace(layout);
        const QVector<CadViewportDefinition> &viewports = it.value();
        for (int i = 0; i < viewports.size(); ++i) {
            const CadViewportDefinition &viewport = viewports[i];
            if (!viewport.revealsModel) {
                continue;
            }
            if (qEnvironmentVariableIsSet("VPRENDER_DEBUG")) {
                std::fprintf(stderr, "vp%d frozen(%lld):", i,
                             static_cast<long long>(
                                 viewport.frozenLayers.size()));
                for (const QString &name : viewport.frozenLayers) {
                    std::fprintf(stderr, " '%s'",
                                 name.toUtf8().constData());
                }
                std::fprintf(stderr, "\n");
            }
            const QRectF paper(
                viewport.center
                    - QPointF(viewport.windowSize.width() * 0.5,
                              viewport.windowSize.height() * 0.5),
                viewport.windowSize);
            const QImage image =
                view.renderWorldRectToImage(paper, QSize(1400, 900),
                                            false);
            const QString path = QStringLiteral("%1_%2_vp%3.png")
                                     .arg(prefix,
                                          layout.isEmpty()
                                              ? QStringLiteral("layout")
                                              : layout)
                                     .arg(i);
            image.save(path);
            std::fprintf(stderr,
                         "saved %s (frozen=%lld viewHeight=%.1f)\n",
                         path.toUtf8().constData(),
                         static_cast<long long>(viewport.frozenLayers.size()),
                         viewport.viewHeight);
        }
    }
    return 0;
}
