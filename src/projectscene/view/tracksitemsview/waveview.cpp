/*
* Audacity: A Digital Audio Editor
*/
#include "waveview.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QSGFlatColorMaterial>
#include <QTimer>

#include <cstring>
#include <QSGGeometry>
#include <QSGVertexColorMaterial>

#include "global/types/color.h"
#include "global/log.h"

#include "au3/wavepainterutils.h"
#include "au3/samplespainterutils.h"
#include "au3/WaveformPainter.h"
#include "view/timeline/timelinecontext.h"

#include "au3wrap/internal/domaccessor.h"
#include "au3-track/PendingTracks.h"

using namespace au::projectscene;

static const QColor BACKGROUND_COLOR = QColor(255, 255, 255);
static const QColor SAMPLES_BASE_COLOR = QColor(0, 0, 0);
static const QColor SAMPLES_HIGHLIGHT_COLOR = QColor(255, 255, 255);
static const QColor RMS_BASE_COLOR = QColor(255, 255, 255);
static const QColor RMS_SELECTED_COLOR = QColor(255, 255, 255); // TODO: This need update
static const QColor CLIPPING_SOLID_COLOR = QColor(239, 71, 111);
static const QColor CENTER_LINE_COLOR = QColor(0, 0, 0);
static const QColor SAMPLE_HEAD_COLOR = QColor(0, 0, 0);
static const QColor SAMPLE_STALK_COLOR = QColor(0, 0, 0);

// AU3 colors from au3/libraries/au3-theme-resources/light/Components/Colors.txt
static const QColor CLASSIC_BACKGROUND_COLOR = QColor(240, 243, 255);               // Unselected: #f0f3ff
static const QColor CLASSIC_BACKGROUND_SELECTED_COLOR = QColor(170, 195, 242);      // Selected: #aac3f2
static const QColor CLASSIC_SAMPLES_BASE_COLOR = QColor(100, 100, 211);             // Sample: #6464D3
static const QColor CLASSIC_SAMPLES_BASE_SELECTED_COLOR = QColor(103, 124, 228);    // SelSample: #677ce4
static const QColor CLASSIC_RMS_COLOR = QColor(151, 151, 253);                      // Rms: #9797FD
static const QColor CLASSIC_RMS_SELECTED_COLOR = QColor(151, 151, 253);             // Rms: #9797FD // TODO: This need update
static const QColor CLASSIC_CLIPPING_COLOR = QColor(239, 71, 111);                  // Clipped: #ef476f

static const float SAMPLE_HEAD_DEFAULT_ALPHA= 0.6;
static const float SAMPLE_HEAD_CLIP_SELECTED_ALPHA = 0.8;
static const float SAMPLE_HEAD_DATA_SELECTED_ALPHA = 0.9;
static const float SAMPLE_STALK_DEFAULT_ALPHA = 0.4;
static const float SAMPLE_STALK_CLIP_SELECTED_ALPHA = 0.6;
static const float SAMPLE_STALK_DATA_SELECTED_ALPHA = 0.7;

// ============================================================
// PaintedFallback — internal QQuickPaintedItem child for
// QPainter-based rendering (ConnectingDots, Samples, and
// MinMaxRMS until scene graph path is complete)
// ============================================================
class WaveView::PaintedFallback : public QQuickPaintedItem
{
public:
    explicit PaintedFallback(WaveView* owner)
        : QQuickPaintedItem(owner)
        , m_owner(owner)
    {
    }

    void paint(QPainter* painter) override
    {
        m_owner->paintFallback(painter);
    }

private:
    WaveView* m_owner = nullptr;
};

WaveView::WaveView(QQuickItem* parent)
    : QQuickItem(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    setFlag(QQuickItem::ItemHasContents, true);

    m_fallback = new PaintedFallback(this);
    m_fallback->setSize(QSizeF(width(), height()));

    //! NOTE: Push history state after edit is completed to avoid multiple unecessary calls.
    connect(this, &WaveView::isIsolationModeChanged, [this]() {
        if (!m_isIsolationMode) {
            pushProjectHistorySampleEdit();
        }
    });

    connect(this, &WaveView::visibleChanged, [this]() {
        emit isNearSampleChanged();
    });

    connect(this, &WaveView::multiSampleEditChanged, [this]() {
        if (!m_multiSampleEdit) {
            pushProjectHistorySampleEdit();
        }
    });

    configuration()->isRMSInWaveformVisibleChanged().onReceive(this, [this](bool) {
        scheduleRepaint();
    });

    configuration()->isClippingInWaveformVisibleChanged().onReceive(this, [this](bool) {
        scheduleRepaint();
    });
}

WaveView::~WaveView()
{
}

void WaveView::scheduleRepaint()
{
    //! NOTE: polish() coalesces all repaint requests of a frame (clip time,
    //! geometry, zoom, selection...) into a single updatePolish() call right
    //! before the scene graph is synced.
    polish();
}

void WaveView::updatePolish()
{
    prepareSceneGraphData();

    if (m_useSceneGraph) {
        // Scene graph path — hide fallback, trigger updatePaintNode
        if (m_fallback->isVisible()) {
            m_fallback->setVisible(false);
        }
        update();
    } else {
        // Fallback QPainter path — show fallback, trigger its paint()
        if (!m_fallback->isVisible()) {
            m_fallback->setVisible(true);
        }
        m_fallback->update();
    }
}

void WaveView::forceRepaint()
{
    scheduleRepaint();
}

void WaveView::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);

    if (m_fallback) {
        m_fallback->setSize(newGeometry.size());
    }

    // A view laid out after its properties were set (or resized later) must
    // re-prepare: a preparation that ran at zero height yields no vertices and
    // would leave the view stuck on the fallback renderer.
    if (newGeometry.size() != oldGeometry.size()) {
        scheduleRepaint();
    }
}

void WaveView::setClipKey(const ClipKey& newClipKey)
{
    m_clipKey = newClipKey;
    emit clipKeyChanged();

    scheduleRepaint();
}

IWavePainter::Params WaveView::getWavePainterParams() const
{
    IWavePainter::Params params;
    params.geometry.height = height();
    params.geometry.width = width();
    params.geometry.left = 0.0;

    params.zoom = m_context->zoom();
    params.fromTime = (m_clipTime.itemStartTime - m_clipTime.startTime);
    params.toTime = params.fromTime + (m_clipTime.itemEndTime - m_clipTime.itemStartTime);
    params.selectionStartTime = m_clipTime.selectionStartTime;
    params.selectionEndTime = m_clipTime.selectionEndTime;
    params.channelHeightRatio = m_channelHeightRatio;
    params.showRMS = configuration()->isRMSInWaveformVisible();
    params.showClipping = configuration()->isClippingInWaveformVisible();
    params.isLinear = m_isLinear;
    params.dbRange = m_dbRange;
    params.displayBounds = m_displayBounds;

    projectscene::ClipStyles::Style clipStyle = configuration()->clipStyle();
    if (clipStyle == projectscene::ClipStyles::Style::COLORFUL) {
        applyColorfulStyle(params, m_clipColor, m_clipSelectedColor, m_clipSelected);
    } else {
        applyClassicStyle(params, m_clipSelected);
    }

    return params;
}

void WaveView::applyColorfulStyle(IWavePainter::Params& params,
                                  const QColor& clipColor,
                                  const QColor& clipSelectedColor,
                                  bool selected) const
{
    float normalBgAlpha = 0.8;
    if (selected) {
        params.style.blankBrush = clipSelectedColor;
        params.style.normalBackground = clipSelectedColor;
        params.style.selectedBackground = clipSelectedColor;
        params.style.envelopeBackground = clipSelectedColor;
        params.style.selectedEnvelopeBackground = clipSelectedColor;
    } else {
        params.style.blankBrush = muse::blendQColors(BACKGROUND_COLOR, clipColor, 0.9);
        params.style.normalBackground = muse::blendQColors(BACKGROUND_COLOR, clipColor, normalBgAlpha);
        params.style.selectedBackground = clipSelectedColor;
        params.style.envelopeBackground = muse::blendQColors(BACKGROUND_COLOR, clipColor, normalBgAlpha);
        params.style.selectedEnvelopeBackground = clipSelectedColor;
    }

    params.style.samplePen = muse::blendQColors(params.style.blankBrush, SAMPLES_BASE_COLOR, 0.8);
    params.style.selectedSamplePen = muse::blendQColors(params.style.blankBrush,
                                                        selected ? SAMPLES_HIGHLIGHT_COLOR : SAMPLES_BASE_COLOR,
                                                        0.75);
    params.style.rmsPen = muse::blendQColors(params.style.samplePen, RMS_BASE_COLOR, 0.25);
    params.style.rmsSelectedPen = muse::blendQColors(params.style.selectedSamplePen, RMS_BASE_COLOR, 0.6); // TODO: use RMS_SELECTED_COLOR
    params.style.clippedPen = CLIPPING_SOLID_COLOR;
    params.style.centerLine = muse::blendQColors(params.style.samplePen, CENTER_LINE_COLOR, 0.2);

    float headAlpha = selected ? SAMPLE_HEAD_CLIP_SELECTED_ALPHA : SAMPLE_HEAD_DEFAULT_ALPHA;
    float stalkAlpha = selected ? SAMPLE_STALK_CLIP_SELECTED_ALPHA : SAMPLE_STALK_DEFAULT_ALPHA;

    params.style.sampleHead = muse::blendQColors(params.style.samplePen, SAMPLE_HEAD_COLOR, headAlpha);
    params.style.sampleStalk = muse::blendQColors(params.style.samplePen, SAMPLE_STALK_COLOR, stalkAlpha);

    if (!selected) {
        params.style.sampleHeadSelection = muse::blendQColors(params.style.samplePen, SAMPLE_HEAD_COLOR,
                                                              SAMPLE_HEAD_DATA_SELECTED_ALPHA);
        params.style.sampleStalkSelection
            = muse::blendQColors(params.style.samplePen, SAMPLE_STALK_COLOR, SAMPLE_STALK_DATA_SELECTED_ALPHA);
    }
}

void WaveView::applyClassicStyle(IWavePainter::Params& params, bool selected) const
{
    params.style.blankBrush = selected ? CLASSIC_BACKGROUND_SELECTED_COLOR : CLASSIC_BACKGROUND_COLOR;
    params.style.normalBackground = params.style.blankBrush;
    params.style.selectedBackground = selected ? transformColor(CLASSIC_BACKGROUND_SELECTED_COLOR) : CLASSIC_BACKGROUND_SELECTED_COLOR;

    params.style.envelopeBackground = params.style.blankBrush;
    params.style.selectedEnvelopeBackground
        = selected ? transformColor(CLASSIC_BACKGROUND_SELECTED_COLOR) : CLASSIC_BACKGROUND_SELECTED_COLOR;

    QColor baseSampleColor = selected ? CLASSIC_SAMPLES_BASE_SELECTED_COLOR : CLASSIC_SAMPLES_BASE_COLOR;
    params.style.samplePen = baseSampleColor;
    params.style.selectedSamplePen = CLASSIC_SAMPLES_BASE_SELECTED_COLOR;
    params.style.rmsPen = CLASSIC_RMS_COLOR;
    params.style.rmsSelectedPen = muse::blendQColors(params.style.selectedSamplePen, CLASSIC_RMS_COLOR, 0.6); // TODO: use CLASSIC_RMS_SELECTED_COLOR
    params.style.clippedPen = CLASSIC_CLIPPING_COLOR;
    params.style.centerLine = baseSampleColor;
    params.style.sampleHead = baseSampleColor;
    params.style.sampleStalk = baseSampleColor;

    if (!selected) {
        params.style.sampleHeadSelection = baseSampleColor;
        params.style.sampleStalkSelection = baseSampleColor;
    }
}

void WaveView::paintFallback(QPainter* painter)
{
    QElapsedTimer timer;
    timer.start();

    IWavePainter::Params params = getWavePainterParams();
    IWavePainter::PlotType pType = wavepainterutils::getPlotType(globalContext()->currentProject(), m_clipKey.key, params.zoom);

    bool isStemPlot = pType == IWavePainter::PlotType::Stem;

    setIsStemPlot(isStemPlot);
    m_fallback->setAntialiasing(isStemPlot);

    wavePainter()->paint(*painter, m_clipKey.key, params, pType);

    LOGD() << "[WaveView::paintFallback] clip=" << m_clipKey.key.itemId
           << " plotType=" << static_cast<int>(pType)
           << " elapsed=" << timer.nsecsElapsed() / 1000 << "us";
}

void WaveView::prepareSceneGraphData()
{
    QElapsedTimer timer;
    timer.start();

    const std::vector<SGVertexData> prevVertices = std::move(m_sgVertices);
    m_sgVertices.clear();
    m_useSceneGraph = false;

    if (!m_context || m_clipKey.key.itemId == -1) {
        return;
    }

    IWavePainter::Params params = getWavePainterParams();
    IWavePainter::PlotType pType = wavepainterutils::getPlotType(
        globalContext()->currentProject(), m_clipKey.key, params.zoom);

    if (pType != IWavePainter::PlotType::MinMaxRMS) {
        return;
    }

    au::au3::Au3Project* au3Project
        =reinterpret_cast<au::au3::Au3Project*>(globalContext()->currentProject()->au3ProjectPtr());
    WaveTrack* track = au::au3::DomAccessor::findWaveTrack(*au3Project, TrackId(m_clipKey.key.trackId));
    if (!track) {
        return;
    }

    std::shared_ptr<WaveClip> waveClip = au::au3::DomAccessor::findWaveClip(track, m_clipKey.key.itemId);
    if (!waveClip) {
        return;
    }

    auto& waveformPainter = WaveformPainter::Get(*waveClip);
    waveformPainter.EnsureClip(*waveClip);

    const std::vector<double> channelHeight {
        params.geometry.height * params.channelHeightRatio,
        params.geometry.height * (1 - params.channelHeightRatio),
    };

    const float dBRange = std::abs(params.dbRange);
    const bool dB = !params.isLinear;

    auto waveMetrics = wavepainterutils::getWaveMetrics(
        globalContext()->currentProject(), m_clipKey.key, params);

    // The data cache is indexed from the untrimmed sequence start; place the
    // requested window relative to the sequence origin. The origin
    // (playStart - trimLeft) is invariant under trimming, so the window stays
    // consistent with the container position from m_clipTime even when a
    // repaint runs between the clip trim and the view-state update.
    const double sequenceStartTime = waveClip->GetPlayStartTime() - waveClip->GetTrimLeft();
    waveMetrics.fromTime = m_clipTime.itemStartTime - sequenceStartTime;
    waveMetrics.toTime = waveMetrics.fromTime + (m_clipTime.itemEndTime - m_clipTime.itemStartTime);

    // The clip container is positioned at integer x (see TrackClipsListModel);
    // start the content at the sub-pixel residual so the waveform stays
    // anchored to the timeline rather than to the rounded container.
    const double containerExactX = m_context->timeToPosition(m_clipTime.itemStartTime);
    waveMetrics.left = containerExactX - std::floor(0.5 + containerExactX);

    const float zoomMin = params.displayBounds.first;
    const float zoomMax = params.displayBounds.second;

    m_sgBackgroundColor = params.style.normalBackground;
    m_sgSelectedBackgroundColor = params.style.selectedBackground;
    m_sgZeroLineColor = params.style.centerLine;
    m_sgShowRMS = params.showRMS;

    QColor sampleColor = params.style.samplePen;
    QColor selSampleColor = params.style.selectedSamplePen;
    QColor rmsColor = params.style.rmsPen;
    QColor selRmsColor = params.style.rmsSelectedPen;
    QColor clipColor = params.style.clippedPen;

    double topOffset = 0.0;

    // Track selection pixel range for overlay rect
    m_sgHasSelection = false;
    float selMinX = std::numeric_limits<float>::max();
    float selMaxX = std::numeric_limits<float>::lowest();

    m_sgChannelSplitIndex = 0;
    m_sgZeroLineYs.clear();
    bool allDataComplete = true;

    for (size_t ch = 0; ch < waveClip->NChannels(); ++ch) {
        waveMetrics.height = channelHeight[ch];
        waveMetrics.top = topOffset;

        // Build paint params per channel (height changes per channel for stereo)
        WavePaintParameters paintParams;
        paintParams
        .SetDisplayParameters(
            waveMetrics.height, zoomMin, zoomMax, params.showClipping)
        .SetDBParameters(dBRange, dB)
        .SetShowRMS(params.showRMS);
        paintParams.SetEnvelope(waveClip->GetEnvelope());

        bool channelDataComplete = true;
        auto columnData = waveformPainter.GetColumnData(ch, paintParams, waveMetrics, &channelDataComplete);
        allDataComplete = allDataComplete && channelDataComplete;

        if (ch > 0) {
            m_sgChannelSplitIndex = m_sgVertices.size();
        }

        const float channelTop = static_cast<float>(topOffset);
        const float channelBottom = channelTop + static_cast<float>(channelHeight[ch]);

        for (const auto& col : columnData) {
            SGVertexData v;
            v.x = col.x;

            if (col.clipping) {
                // Full-height clipping bar matches bitmap renderer's behavior
                v.maxY = channelTop;
                v.minY = channelBottom;
                v.rmsMaxY = channelTop;
                v.rmsMinY = channelBottom;

                v.r = clipColor.red();
                v.g = clipColor.green();
                v.b = clipColor.blue();
                v.a = 255;

                v.rmsR = clipColor.red();
                v.rmsG = clipColor.green();
                v.rmsB = clipColor.blue();
                v.rmsA = 255;
            } else {
                v.maxY = channelTop + col.maxY;
                v.minY = channelTop + col.minY;
                v.rmsMaxY = channelTop + col.rmsMaxY;
                v.rmsMinY = channelTop + col.rmsMinY;

                QColor sc = col.selected ? selSampleColor : sampleColor;
                v.r = sc.red();
                v.g = sc.green();
                v.b = sc.blue();
                v.a = 255;

                QColor rc = col.selected ? selRmsColor : rmsColor;
                v.rmsR = rc.red();
                v.rmsG = rc.green();
                v.rmsB = rc.blue();
                v.rmsA = 255;
            }

            if (col.selected && ch == 0) {
                selMinX = std::min(selMinX, col.x);
                selMaxX = std::max(selMaxX, col.x + 1.0f);
                m_sgHasSelection = true;
            }

            m_sgVertices.push_back(v);
        }

        // Compute zero line Y for this channel
        float normalized = (params.displayBounds.second - 0.0f)
                           / (params.displayBounds.second - params.displayBounds.first);
        m_sgZeroLineYs.push_back(channelTop + normalized * (static_cast<float>(channelHeight[ch]) - 1));

        topOffset += channelHeight[ch];
    }

    // Incomplete cache elements are recomputed by the cache on the next lookup;
    // ask again shortly so the snapshot does not stay partial until the next
    // user-triggered repaint. Only retry while retries make progress, otherwise
    // a clip whose data cannot complete would retry forever.
    const bool madeProgress = m_sgVertices.size() != prevVertices.size()
                              || (!m_sgVertices.empty()
                                  && memcmp(m_sgVertices.data(), prevVertices.data(),
                                            m_sgVertices.size() * sizeof(SGVertexData)) != 0);
    if (!allDataComplete && madeProgress && !m_sgRetryPending) {
        m_sgRetryPending = true;
        QTimer::singleShot(80, this, [this]() {
            m_sgRetryPending = false;
            scheduleRepaint();
        });
    }

    if (m_sgHasSelection) {
        m_sgSelectionLeft = selMinX;
        m_sgSelectionRight = selMaxX;
    }

    m_sgHeight = static_cast<float>(params.geometry.height);
    m_useSceneGraph = !m_sgVertices.empty();
    m_sgDirty = true;

    LOGD() << "[WaveView::prepareSG] clip=" << m_clipKey.key.itemId
           << " cols=" << m_sgVertices.size()
           << " elapsed=" << timer.nsecsElapsed() / 1000 << "us";
}

QSGNode* WaveView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    QElapsedTimer timer;
    timer.start();

    if (!m_useSceneGraph) {
        delete oldNode;
        return nullptr;
    }

    // Node structure (fixed order, child index matters for pointer recovery):
    //   root (QSGNode)
    //   ├── [0] background       (flat color rect)
    //   ├── [1] selection bg     (flat color rect, empty when no selection)
    //   ├── [2] waveform body    (vertex-colored triangle strip)
    //   ├── [3] rms band         (vertex-colored triangle strip, empty when showRMS=false)
    //   └── [4] zero line        (flat color rect)

    constexpr int IDX_BG = 0;
    constexpr int IDX_SEL_BG = 1;
    constexpr int IDX_WAVE = 2;
    constexpr int IDX_RMS = 3;
    constexpr int IDX_ZERO = 4;

    QSGNode* root = oldNode;

    auto makeFlatNode = [](int vertexCount) {
        auto* node = new QSGGeometryNode();
        auto* geo = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), vertexCount);
        geo->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geo);
        node->setFlag(QSGNode::OwnsGeometry);
        auto* mat = new QSGFlatColorMaterial();
        node->setMaterial(mat);
        node->setFlag(QSGNode::OwnsMaterial);
        return node;
    };

    auto makeColoredNode = []() {
        auto* node = new QSGGeometryNode();
        auto* geo = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geo->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geo);
        node->setFlag(QSGNode::OwnsGeometry);
        auto* mat = new QSGVertexColorMaterial();
        node->setMaterial(mat);
        node->setFlag(QSGNode::OwnsMaterial);
        return node;
    };

    if (!root) {
        root = new QSGNode();
        root->appendChildNode(makeFlatNode(4));     // background
        root->appendChildNode(makeFlatNode(4));     // selection background
        root->appendChildNode(makeColoredNode());   // waveform
        root->appendChildNode(makeColoredNode());   // rms
        root->appendChildNode(makeFlatNode(4));     // zero line
    }

    auto* bgNode = static_cast<QSGGeometryNode*>(root->childAtIndex(IDX_BG));
    auto* selBgNode = static_cast<QSGGeometryNode*>(root->childAtIndex(IDX_SEL_BG));
    auto* waveformNode = static_cast<QSGGeometryNode*>(root->childAtIndex(IDX_WAVE));
    auto* rmsNode = static_cast<QSGGeometryNode*>(root->childAtIndex(IDX_RMS));
    auto* zeroLineNode = static_cast<QSGGeometryNode*>(root->childAtIndex(IDX_ZERO));

    const int N = static_cast<int>(m_sgVertices.size());
    const float w = static_cast<float>(width());

    // Background (full clip area)
    {
        auto* geo = bgNode->geometry();
        auto* v = geo->vertexDataAsPoint2D();
        v[0].set(0, 0);
        v[1].set(w, 0);
        v[2].set(0, m_sgHeight);
        v[3].set(w, m_sgHeight);
        static_cast<QSGFlatColorMaterial*>(bgNode->material())->setColor(m_sgBackgroundColor);
        bgNode->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    }

    // Selection background overlay (only visible when a selection exists)
    {
        auto* geo = selBgNode->geometry();
        auto* v = geo->vertexDataAsPoint2D();
        if (m_sgHasSelection) {
            v[0].set(m_sgSelectionLeft, 0);
            v[1].set(m_sgSelectionRight, 0);
            v[2].set(m_sgSelectionLeft, m_sgHeight);
            v[3].set(m_sgSelectionRight, m_sgHeight);
        } else {
            // Degenerate quad — renders nothing
            v[0].set(0, 0);
            v[1].set(0, 0);
            v[2].set(0, 0);
            v[3].set(0, 0);
        }
        static_cast<QSGFlatColorMaterial*>(selBgNode->material())->setColor(m_sgSelectedBackgroundColor);
        selBgNode->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    }

    // Two extra degenerate vertices break the strip between stereo channels so
    // no triangles bridge the end of one channel to the start of the next.
    const bool hasSplit = m_sgChannelSplitIndex > 0 && m_sgChannelSplitIndex < m_sgVertices.size();
    const int splitIndex = static_cast<int>(m_sgChannelSplitIndex);

    // Waveform body — triangle strip: (x, maxY), (x, minY) pairs per column
    {
        auto* geo = waveformNode->geometry();
        geo->allocate(N * 2 + (hasSplit ? 2 : 0));
        auto* v = geo->vertexDataAsColoredPoint2D();
        int vi = 0;
        for (int i = 0; i < N; ++i) {
            const auto& col = m_sgVertices[i];
            if (hasSplit && i == splitIndex) {
                const auto& prev = m_sgVertices[i - 1];
                v[vi++].set(prev.x, prev.minY, prev.r, prev.g, prev.b, prev.a);
                v[vi++].set(col.x, col.maxY, col.r, col.g, col.b, col.a);
            }
            v[vi++].set(col.x, col.maxY, col.r, col.g, col.b, col.a);
            v[vi++].set(col.x, col.minY, col.r, col.g, col.b, col.a);
        }
        waveformNode->markDirty(QSGNode::DirtyGeometry);
    }

    // RMS band — only allocate vertices when enabled
    {
        auto* geo = rmsNode->geometry();
        if (m_sgShowRMS) {
            geo->allocate(N * 2 + (hasSplit ? 2 : 0));
            auto* v = geo->vertexDataAsColoredPoint2D();
            int vi = 0;
            for (int i = 0; i < N; ++i) {
                const auto& col = m_sgVertices[i];
                if (hasSplit && i == splitIndex) {
                    const auto& prev = m_sgVertices[i - 1];
                    v[vi++].set(prev.x, prev.rmsMinY, prev.rmsR, prev.rmsG, prev.rmsB, prev.rmsA);
                    v[vi++].set(col.x, col.rmsMaxY, col.rmsR, col.rmsG, col.rmsB, col.rmsA);
                }
                v[vi++].set(col.x, col.rmsMaxY, col.rmsR, col.rmsG, col.rmsB, col.rmsA);
                v[vi++].set(col.x, col.rmsMinY, col.rmsR, col.rmsG, col.rmsB, col.rmsA);
            }
        } else {
            geo->allocate(0);
        }
        rmsNode->markDirty(QSGNode::DirtyGeometry);
    }

    // Zero line — one 1px line per channel
    {
        auto* geo = zeroLineNode->geometry();
        geo->setDrawingMode(QSGGeometry::DrawTriangles);
        geo->allocate(static_cast<int>(m_sgZeroLineYs.size()) * 6);
        auto* v = geo->vertexDataAsPoint2D();
        int vi = 0;
        for (float y : m_sgZeroLineYs) {
            v[vi++].set(0, y);
            v[vi++].set(w, y);
            v[vi++].set(0, y + 1);
            v[vi++].set(w, y);
            v[vi++].set(w, y + 1);
            v[vi++].set(0, y + 1);
        }
        static_cast<QSGFlatColorMaterial*>(zeroLineNode->material())->setColor(m_sgZeroLineColor);
        zeroLineNode->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    }

    m_sgDirty = false;

    LOGD() << "[WaveView::updatePaintNode] clip=" << m_clipKey.key.itemId
           << " verts=" << (m_sgVertices.size() * 2)
           << " elapsed=" << timer.nsecsElapsed() / 1000 << "us";

    return root;
}

ClipKey WaveView::clipKey() const
{
    return m_clipKey;
}

TimelineContext* WaveView::timelineContext() const
{
    return m_context;
}

void WaveView::setTimelineContext(TimelineContext* newContext)
{
    if (m_context == newContext) {
        return;
    }

    if (m_context) {
        disconnect(m_context, nullptr, this, nullptr);
    }

    m_context = newContext;

    if (m_context) {
        connect(m_context, &TimelineContext::frameTimeChanged, this, &WaveView::updateView);
        connect(m_context, &TimelineContext::selectionStartTimeChanged, this, &WaveView::updateView);
        connect(m_context, &TimelineContext::selectionEndTimeChanged, this, &WaveView::updateView);
        connect(m_context, &TimelineContext::zoomChanged, this, &WaveView::onWaveZoomChanged);

        onWaveZoomChanged();
    }

    emit timelineContextChanged();
}

void WaveView::updateView()
{
    scheduleRepaint();
}

QColor WaveView::clipColor() const
{
    return m_clipColor;
}

void WaveView::setClipColor(const QColor& newClipColor)
{
    if (m_clipColor == newClipColor) {
        return;
    }
    m_clipColor = newClipColor;
    emit clipColorChanged();

    scheduleRepaint();
}

QColor WaveView::clipSelectedColor() const
{
    return m_clipSelectedColor;
}

void WaveView::setClipSelectedColor(const QColor& newClipSelectedColor)
{
    if (m_clipSelectedColor == newClipSelectedColor) {
        return;
    }
    m_clipSelectedColor = newClipSelectedColor;
    emit clipSelectedColorChanged();

    scheduleRepaint();
}

bool WaveView::clipSelected() const
{
    return m_clipSelected;
}

void WaveView::setClipSelected(bool newClipSelected)
{
    if (m_clipSelected == newClipSelected) {
        return;
    }
    m_clipSelected = newClipSelected;
    emit clipSelectedChanged();

    scheduleRepaint();
}

ClipTime WaveView::clipTime() const
{
    return m_clipTime;
}

void WaveView::setClipTime(const ClipTime& newClipTime)
{
    if (m_clipTime == newClipTime) {
        return;
    }
    m_clipTime = newClipTime;
    emit clipTimeChanged();

    scheduleRepaint();
}

double WaveView::channelHeightRatio() const
{
    return m_channelHeightRatio;
}

void WaveView::setChannelHeightRatio(double channelHeightRatio)
{
    m_channelHeightRatio = channelHeightRatio;
    emit channelHeightRatioChanged();
    scheduleRepaint();
}

bool WaveView::isNearSample() const
{
    return isVisible() && m_isNearSample;
}

void WaveView::setIsNearSample(bool isNearSample)
{
    if (m_isNearSample == isNearSample) {
        return;
    }

    m_isNearSample = isNearSample;
    emit isNearSampleChanged();
}

bool WaveView::isStemPlot() const
{
    return m_isStemPlot;
}

void WaveView::setIsStemPlot(bool isStemPlot)
{
    if (m_isStemPlot == isStemPlot) {
        return;
    }

    m_isStemPlot = isStemPlot;
    emit isStemPlotChanged();
}

int WaveView::currentChannel() const
{
    return m_currentChannel.value_or(0);
}

void WaveView::setCurrentChannel(int currentChannel)
{
    m_currentChannel = currentChannel;
}

bool WaveView::isIsolationMode() const
{
    return m_isIsolationMode;
}

void WaveView::setIsIsolationMode(bool isIsolationMode)
{
    if (m_isIsolationMode == isIsolationMode) {
        return;
    }

    m_isIsolationMode = isIsolationMode;
    emit isIsolationModeChanged();
}

void WaveView::setMultiSampleEdit(bool multiSampleEdit)
{
    if (m_multiSampleEdit == multiSampleEdit) {
        return;
    }

    m_multiSampleEdit = multiSampleEdit;
    emit multiSampleEditChanged();
}

bool WaveView::multiSampleEdit() const
{
    return m_multiSampleEdit;
}

void WaveView::setIsBrush(bool isBrush)
{
    if (m_isBrush == isBrush) {
        return;
    }

    m_isBrush = isBrush;
    emit isBrushChanged();
}

bool WaveView::isBrush() const
{
    return m_isBrush;
}

bool WaveView::isLinear() const
{
    return m_isLinear;
}

void WaveView::setIsLinear(bool isLinear)
{
    if (m_isLinear == isLinear) {
        return;
    }

    m_isLinear = isLinear;
    scheduleRepaint();
}

double WaveView::dbRange() const
{
    return m_dbRange;
}

void WaveView::setDbRange(double dbRange)
{
    if (m_dbRange == dbRange) {
        return;
    }

    m_dbRange = dbRange;
    scheduleRepaint();
}

QVariant WaveView::displayBounds() const
{
    QMap<QString, float> bounds;
    bounds["min"] = m_displayBounds.first;
    bounds["max"] = m_displayBounds.second;
    return QVariant::fromValue(bounds);
}

void WaveView::setDisplayBounds(const QVariant& displayBounds)
{
    float minBound = displayBounds.toMap().value("min", -1.0f).toFloat();
    float maxBound = displayBounds.toMap().value("max", 1.0f).toFloat();

    if (m_displayBounds.first == minBound && m_displayBounds.second == maxBound) {
        return;
    }

    m_displayBounds.first = minBound;
    m_displayBounds.second = maxBound;

    scheduleRepaint();
}

QColor WaveView::transformColor(const QColor& originalColor) const
{
    int r = originalColor.red();
    int g = originalColor.green();
    int b = originalColor.blue();

    int deltaRed = (r < 240) ? 51 : (255 - r);
    int deltaGreen = (g < 240) ? 69 : (255 - g);
    int deltaBlue = 77;

    int newRed = qBound(0, r + deltaRed, 255);
    int newGreen = qBound(0, g + deltaGreen, 255);
    int newBlue = qBound(0, b + deltaBlue, 255);

    return QColor(newRed, newGreen, newBlue);
}

void WaveView::setLastMousePos(const unsigned int x, const unsigned int y)
{
    if (wavepainterutils::getPlotType(globalContext()->currentProject(), m_clipKey.key,
                                      m_context->zoom()) != IWavePainter::PlotType::Stem) {
        return;
    }

    const auto params = getWavePainterParams();
    m_currentChannel =  samplespainterutils::hitNearestSampleChannelIndex(globalContext()->currentProject(), m_clipKey.key, QPoint(x,
                                                                                                                                   y),
                                                                          params);
    setIsNearSample(m_currentChannel.has_value());
}

void WaveView::setLastClickPos(const unsigned lastX, const unsigned lastY, const unsigned int x, const unsigned int y)
{
    if (wavepainterutils::getPlotType(globalContext()->currentProject(), m_clipKey.key,
                                      m_context->zoom()) != IWavePainter::PlotType::Stem) {
        return;
    }

    // Prevent sample editing during playback
    if (playbackState()->isPlaying()) {
        return;
    }

    const auto currentPosition = QPoint(x, y);
    const auto lastPosition = QPoint(lastX, lastY);

    const auto params = getWavePainterParams();

    if (!m_currentChannel.has_value()) {
        m_currentChannel = samplespainterutils::hitNearestSampleChannelIndex(
            globalContext()->currentProject(), m_clipKey.key, currentPosition, params);
        return;
    }

    samplespainterutils::setLastClickPos(
        m_currentChannel.value(),
        globalContext()->currentProject(), m_clipKey.key, lastPosition, currentPosition, params);

    m_lastClickedPoint = currentPosition;
}

void WaveView::smoothLastClickPos(unsigned int x, const unsigned int y)
{
    if (!m_isStemPlot) {
        return;
    }

    // Prevent sample editing during playback
    if (playbackState()->isPlaying()) {
        return;
    }

    const auto currentPosition = QPoint(x, y);
    const auto params = getWavePainterParams();

    auto channel = samplespainterutils::hitChannelIndex(globalContext()->currentProject(), m_clipKey.key, currentPosition, params);

    if (!channel) {
        return;
    }

    samplespainterutils::smoothLastClickPos(
        channel.value(),
        globalContext()->currentProject(), m_clipKey.key, currentPosition, params);

    //! NOTE: History state is only pushed when data is actually changed.
    // For smooth edition there is no data change on button press or release
    // just on mouse click.
    pushProjectHistorySampleEdit();
}

void WaveView::setIsolatedPoint(const unsigned int x, const unsigned int y)
{
    if (!m_isStemPlot) {
        return;
    }

    if (!m_isIsolationMode) {
        return;
    }

    // Prevent sample editing during playback
    if (playbackState()->isPlaying()) {
        return;
    }

    if (!m_lastClickedPoint.has_value()) {
        return;
    }

    const auto currentPosition = QPoint(x, y);
    const auto params = getWavePainterParams();

    if (!m_currentChannel.has_value()) {
        m_currentChannel = samplespainterutils::hitNearestSampleChannelIndex(
            globalContext()->currentProject(), m_clipKey.key, currentPosition, params);
        return;
    }

    samplespainterutils::setIsolatedPoint(
        m_currentChannel.value(),
        m_clipKey.key, globalContext()->currentProject(), m_lastClickedPoint.value(), currentPosition, params);
}

void WaveView::onWaveZoomChanged()
{
    const IWavePainter::PlotType currentPlotType = wavepainterutils::getPlotType(globalContext()->currentProject(), m_clipKey.key,
                                                                                 m_context->zoom());
    const bool wasStemPlot = m_isStemPlot;
    const bool isStemPlot = currentPlotType == IWavePainter::PlotType::Stem;

    if (wasStemPlot != isStemPlot) {
        setIsStemPlot(isStemPlot);
        if (!isStemPlot && m_isNearSample) {
            // force isNearSample to false when transitioning away from stem plot mode
            setIsNearSample(false);
        }
        // Note: When transitioning TO stem plot mode, ClipItem.qml onIsStemPlotChanged
        // will trigger mouse position update to force isNearSample to be set correctly
    }

    scheduleRepaint();
}

void WaveView::pushProjectHistorySampleEdit()
{
    projectHistory()->pushHistoryState("Moved Samples", "Sample Edit", trackedit::UndoPushType::CONSOLIDATE);
}

au::context::IPlaybackStatePtr WaveView::playbackState() const
{
    return globalContext()->playbackState();
}
