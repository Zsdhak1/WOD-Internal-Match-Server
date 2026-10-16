#ifndef VIDEOFORMATSELECT_H
#define VIDEOFORMATSELECT_H

#include <QCameraFormat>
#include <QList>
#include <QSize>

#include "videostreamsize.h"

// Capture-format policy shared by the server field camera and the player client.
// The broadcast output and the player's full-screen view use the source's
// native resolution; only previews are downscaled (to at most 720p).
namespace videoformat {

constexpr int kMinSmoothFps = 30;              // "native" = best resolution that still reaches 30 FPS

inline bool reachesSmoothFps(const QCameraFormat &f)
{ return f.maxFrameRate() >= kMinSmoothFps - 0.5f; }

// Highest resolution among formats that reach 30 FPS. If none reaches 30,
// fall back to the highest frame rate (then resolution). Ties at the same
// resolution prefer ARGB (no conversion), then highest FPS when preferHighFps,
// otherwise the rate closest to 30.
inline QCameraFormat chooseNative(const QList<QCameraFormat> &formats, bool preferHighFps)
{
    QCameraFormat best;
    const auto area = [](const QCameraFormat &f) {
        return qint64(f.resolution().width()) * f.resolution().height();
    };
    const auto better = [&](const QCameraFormat &a, const QCameraFormat &b) {
        if (b.isNull()) return true;
        if (reachesSmoothFps(a) != reachesSmoothFps(b)) return reachesSmoothFps(a);
        if (!reachesSmoothFps(a) && a.maxFrameRate() != b.maxFrameRate())
            return a.maxFrameRate() > b.maxFrameRate();
        if (area(a) != area(b)) return area(a) > area(b);
        const bool argbA = a.pixelFormat() == QVideoFrameFormat::Format_ARGB8888;
        const bool argbB = b.pixelFormat() == QVideoFrameFormat::Format_ARGB8888;
        if (argbA != argbB) return argbA;
        if (preferHighFps) return a.maxFrameRate() > b.maxFrameRate();
        return qAbs(a.maxFrameRate() - kMinSmoothFps) < qAbs(b.maxFrameRate() - kMinSmoothFps);
    };
    for (const QCameraFormat &f : formats) {
        const QSize s = f.resolution();
        if (s.isEmpty() || f.maxFrameRate() <= 0) continue;
        if (s.width() > kMaxCaptureSize.width() || s.height() > kMaxCaptureSize.height()) continue;
        if (better(f, best)) best = f;
    }
    return best;
}

} // namespace videoformat

#endif // VIDEOFORMATSELECT_H
