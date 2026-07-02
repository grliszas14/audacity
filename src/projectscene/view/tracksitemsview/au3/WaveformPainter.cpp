#include <QImage>
#include <cmath>

#include "WaveformPainter.h"
#include "au3-screen-geometry/ZoomInfo.h"
#include "au3-mixer/Envelope.h"
#include "au3-utility/MemoryX.h"

namespace au::projectscene {
struct WaveBitmapCacheElementQt final : public WaveBitmapCacheElement
{
    uint8_t* Allocate(size_t width, size_t height) override
    {
        mWidth = width;
        mHeight = height;

        mBytes.resize(std::max(mWidth * mHeight * 3, mBytes.size()));
        return mBytes.data();
    }

    QImage GetImage() const
    {
        return QImage((const uchar*)mBytes.data(), mWidth, mHeight, mWidth * 3, QImage::Format_RGB888);
    }

    size_t Width() const override
    {
        return mWidth;
    }

    size_t Height() const override
    {
        return mHeight;
    }

private:
    size_t mWidth{};
    size_t mHeight{};
    std::vector<uint8_t> mBytes;
};

static au::au3::Au3WaveClip::Attachments::RegisteredFactory sKeyW{ [](au::au3::Au3WaveClip&) {
        return std::make_unique<WaveformPainter>();
    } };

WaveformPainter& WaveformPainter::Get(const au::au3::Au3WaveClip& clip)
{
    return const_cast< au::au3::Au3WaveClip& >(clip)   // Consider it mutable data
           .Attachments::Get<WaveformPainter>(sKeyW).EnsureClip(clip);
}

WaveformPainter& WaveformPainter::EnsureClip(const au::au3::Au3WaveClip& clip)
{
    const auto changed = mChanged.exchange(false);
    if (&clip != mWaveClip || changed) {
        mChannelCaches.clear();
    }

    const auto nChannels = clip.NChannels();

    if (mChannelCaches.size() == nChannels) {
        return *this;
    }

    mWaveClip = &clip;

    mChannelCaches.reserve(nChannels);

    for (size_t channelIndex = 0; channelIndex < nChannels; ++channelIndex) {
        auto dataCache = std::make_shared<WaveDataCache>(clip, channelIndex);

        auto bitmapCache = std::make_unique<WaveBitmapCache>(
            clip, dataCache,
            [] { return std::make_unique<WaveBitmapCacheElementQt>(); });

        mChannelCaches.push_back({ std::move(dataCache), std::move(bitmapCache) });
    }

    return *this;
}

void WaveformPainter::Draw(size_t channelIndex,
                           QPainter& painter,
                           const WavePaintParameters& params,
                           const au::projectscene::WaveMetrics& metrics)
{
    assert(channelIndex < mChannelCaches.size());
    if (channelIndex >= mChannelCaches.size()) {
        return;
    }

    auto& bitmapCache = mChannelCaches[channelIndex].BitmapCache;
    bitmapCache->SetPaintParameters(params);

    const ZoomInfo zoomInfo(0.0, metrics.zoom);
    bitmapCache->SetSelection(zoomInfo, metrics.selectionStartTime, metrics.selectionEndTime, true);

    auto range = bitmapCache->PerformLookup(zoomInfo, metrics.fromTime, metrics.toTime);

    double left = metrics.left;
    int height = metrics.height;

    for (auto it = range.begin(); it != range.end(); ++it) {
        const auto elementLeftOffset = it.GetLeftOffset();
        const auto elementRightOffset = it.GetRightOffset();

        const auto width = WaveBitmapCache::CacheElementWidth - elementLeftOffset - elementRightOffset;

        const auto drawableWidth = std::min<int32_t>(width, it->Width() - elementLeftOffset);

        const auto image = static_cast<const WaveBitmapCacheElementQt&>(*it).GetImage();
        painter.drawImage(
            QRectF(left, metrics.top, drawableWidth, height),
            image,
            QRectF(
                elementLeftOffset,
                0,
                std::clamp(drawableWidth, 0, image.width() - static_cast<int>(elementLeftOffset)),
                std::clamp(height, 0, image.height())
                )
            );

        left += width;
    }
}

std::vector<WaveColumnVertex> WaveformPainter::GetColumnData(
    size_t channelIndex,
    const WavePaintParameters& params,
    const au::projectscene::WaveMetrics& metrics,
    bool* dataComplete)
{
    std::vector<WaveColumnVertex> result;

    if (dataComplete) {
        *dataComplete = true;
    }

    if (channelIndex >= mChannelCaches.size() || metrics.height <= 0) {
        return result;
    }

    auto& dataCache = mChannelCaches[channelIndex].DataCache;
    dataCache->UpdateViewportWidth(static_cast<int64_t>(metrics.width));

    const ZoomInfo zoomInfo(0.0, metrics.zoom);
    auto range = dataCache->PerformLookup(zoomInfo, metrics.fromTime, metrics.toTime);

    const auto height = static_cast<float>(metrics.height);
    const auto displayMin = static_cast<float>(params.Min);
    const auto displayMax = static_cast<float>(params.Max);
    const bool dbScale = params.DBScale;
    const float dbRange = static_cast<float>(params.DBRange);
    const bool showClipping = params.ShowClipping;

    auto valueToY = [displayMin, displayMax, height](float value) -> float {
        if (displayMax == displayMin) {
            return height * 0.5f;
        }
        float normalized = (displayMax - value) / (displayMax - displayMin);
        return normalized * (height - 1);
    };

    auto applyDB = [dbRange](float value) -> float {
        float sign = (value >= 0 ? 1.0f : -1.0f);
        if (value != 0.0f) {
            float db = static_cast<float>(LINEAR_TO_DB(fabs(value)));
            value = (db + dbRange) / dbRange;
            if (value < 0.0f) {
                value = 0.0f;
            }
            value *= sign;
        }
        return value;
    };

    // Compute selection pixel range
    const int64_t selFirst = zoomInfo.TimeToPosition(metrics.selectionStartTime);
    const int64_t selLast = std::max(zoomInfo.TimeToPosition(metrics.selectionEndTime), selFirst + 1);

    // Get envelope if present
    const Envelope* envelope = params.AttachedEnvelope;
    const bool hasEnvelope = envelope != nullptr
                             && (envelope->GetNumberOfPoints() > 0
                                 || envelope->GetDefaultValue() != 1.0);

    if (dataComplete) {
        *dataComplete = !(range.begin() == range.end() && metrics.width > 0);
    }

    constexpr size_t tileWidth = GraphicsDataCacheBase::CacheElementWidth;

    // PerformLookup snaps fromTime to a whole column; carry the discarded
    // sub-pixel fraction into the vertex positions so the waveform does not
    // wobble against the smoothly-moving clip edge while trimming/scrolling.
    const double exactStartPixel = metrics.fromTime * metrics.zoom;
    const int64_t snappedStartPixel = zoomInfo.TimeToPosition(metrics.fromTime);
    float tileStartX = static_cast<float>(metrics.left + (snappedStartPixel - exactStartPixel));

    for (auto it = range.begin(); it != range.end(); ++it) {
        const auto& element = *it;
        const size_t leftOffset = it.GetLeftOffset();
        const size_t rightOffset = it.GetRightOffset();

        //! NOTE: x always advances by the tile's nominal width, like the bitmap
        //! renderer: an incomplete tile must leave a gap, not shift the rest.
        const size_t nominalWidth = tileWidth - leftOffset - rightOffset;
        const size_t endCol = std::min<size_t>(element.AvailableColumns, tileWidth - rightOffset);

        // The final element of a clip is never IsComplete (it nominally spans
        // past the clip's end); only report incompleteness when columns are
        // missing within the requested window.
        if (dataComplete && !element.IsComplete
            && element.AvailableColumns < tileWidth - rightOffset) {
            *dataComplete = false;
        }

        const size_t emitCount = endCol > leftOffset ? endCol - leftOffset : 0;

        // Fetch envelope values for the emitted columns of this tile
        std::array<double, tileWidth> envValues {};
        if (hasEnvelope && emitCount > 0) {
            double colTime = metrics.fromTime + (tileStartX - metrics.left) / metrics.zoom;
            envelope->GetValues(
                envValues.data(), static_cast<int>(emitCount),
                colTime + envelope->GetOffset(),
                1.0 / metrics.zoom);
        }

        for (size_t col = leftOffset; col < endCol; ++col) {
            auto columnData = element.Data[col];
            const float x = tileStartX + static_cast<float>(col - leftOffset);

            // Apply envelope
            if (hasEnvelope) {
                float envVal = static_cast<float>(envValues[col - leftOffset]);
                columnData.min *= envVal;
                columnData.max *= envVal;
                columnData.rms *= envVal;
            }

            // Apply dB scale
            if (dbScale) {
                columnData.min = applyDB(columnData.min);
                columnData.max = applyDB(columnData.max);
                columnData.rms = applyDB(columnData.rms);
            }

            // Determine selection state
            double colTimeSec = metrics.fromTime + (x - metrics.left) / metrics.zoom;
            int64_t pixelPos = static_cast<int64_t>(colTimeSec * metrics.zoom + 0.5);
            bool selected = pixelPos >= selFirst && pixelPos < selLast;

            bool clipping = showClipping
                            && (columnData.min <= static_cast<float>(-MAX_AUDIO)
                                || columnData.max >= static_cast<float>(MAX_AUDIO));

            WaveColumnVertex v;
            v.x = x;
            v.maxY = valueToY(columnData.max);
            v.minY = valueToY(columnData.min);
            v.rmsMaxY = valueToY(std::min(columnData.rms, columnData.max));
            v.rmsMinY = valueToY(std::max(-columnData.rms, columnData.min));
            v.selected = selected;
            v.clipping = clipping;

            result.push_back(v);
        }

        tileStartX += static_cast<float>(nominalWidth);
    }

    return result;
}

void WaveformPainter::MarkChanged() noexcept
{
    mChanged.store(true);
}

void WaveformPainter::Invalidate()
{
    for (auto& channelCache : mChannelCaches) {
        channelCache.DataCache->Invalidate();
        channelCache.BitmapCache->Invalidate();
    }
}

std::unique_ptr<WaveClipListener> WaveformPainter::Clone() const
{
    return std::make_unique<WaveformPainter>();
}
}
