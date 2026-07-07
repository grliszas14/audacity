#pragma once

#include <QPainter>
#include <vector>

#include "au3-wave-track/WaveClip.h"
#include "au3-wave-track-paint/waveform/WavePaintParameters.h"
#include "au3-wave-track-paint/waveform/WaveDataCache.h"
#include "au3-wave-track-paint/waveform/WaveBitmapCache.h"

#include "au3wrap/au3types.h"
#include "WaveMetrics.h"

namespace au::projectscene {
//! Per-column waveform data ready for vertex generation
struct WaveColumnVertex
{
    float x;           //!< Pixel x-coordinate within the clip
    float maxY;        //!< Top of waveform (pixel y)
    float minY;        //!< Bottom of waveform (pixel y)
    float rmsMaxY;     //!< Top of RMS band (pixel y)
    float rmsMinY;     //!< Bottom of RMS band (pixel y)
    bool selected;     //!< Whether this column is in the selection range
    bool clipping;     //!< Whether this column clips
};

class WaveformPainter final : public WaveClipListener
{
public:

    static WaveformPainter& Get(const au::au3::Au3WaveClip& cache);

    WaveformPainter& EnsureClip(const au::au3::Au3WaveClip& clip);
    void Draw(size_t channelIndex, QPainter& painter, const WavePaintParameters& params, const au::projectscene::WaveMetrics& metrics);

    //! Build vertex-ready column data from the WaveDataCache directly (bypassing bitmap cache).
    //! dataComplete (optional) is set to false when any cache element is still being
    //! computed — the caller should request the data again later and repaint.
    std::vector<WaveColumnVertex> GetColumnData(
        size_t channelIndex, const WavePaintParameters& params, const au::projectscene::WaveMetrics& metrics, bool* dataComplete = nullptr);

    void MarkChanged() noexcept override;
    void Invalidate() override;
    std::unique_ptr<WaveClipListener> Clone() const override;

private:
    const au::au3::Au3WaveClip* mWaveClip {};

    struct ChannelCaches final
    {
        std::shared_ptr<WaveDataCache> DataCache;
        std::unique_ptr<WaveBitmapCache> BitmapCache;
    };

    std::vector<ChannelCaches> mChannelCaches;
    std::atomic<bool> mChanged = false;
};
}
