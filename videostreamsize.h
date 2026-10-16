#ifndef VIDEOSTREAMSIZE_H
#define VIDEOSTREAMSIZE_H

#include <QSize>

// Stream-size policy shared by the client encoder and the server decoder.
// Kept free of Qt Multimedia so network-only targets can use it.
namespace videoformat {

constexpr QSize kMaxCaptureSize(3840, 2160);   // larger sensors exceed the encoder/pipe budget
constexpr QSize kPreviewSize(1280, 720);

// H.264 bitrate (kbit/s) by stream height: 720p 4 Mbps, 1080p 8 Mbps, above 12 Mbps.
inline int streamBitrateKbps(const QSize &size)
{
    if (size.height() <= 720) return 4000;
    if (size.height() <= 1080) return 8000;
    return 12000;
}

// Even, bounded stream size accepted by both ends (yuv420p needs even sizes).
inline bool validStreamSize(const QSize &s)
{
    return s.width() >= 160 && s.height() >= 90 && s.width() <= kMaxCaptureSize.width()
           && s.height() <= kMaxCaptureSize.height() && s.width() % 2 == 0 && s.height() % 2 == 0;
}

// Preview frames are never larger than 720p.
inline QSize previewBound(const QSize &frame)
{
    return frame.boundedTo(frame.scaled(kPreviewSize, Qt::KeepAspectRatio));
}

} // namespace videoformat

#endif // VIDEOSTREAMSIZE_H
