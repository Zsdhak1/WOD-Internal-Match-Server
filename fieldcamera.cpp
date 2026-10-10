#include "fieldcamera.h"

#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <utility>

namespace {
class QtFieldCameraBackend final : public FieldCameraBackend
{
public:
    explicit QtFieldCameraBackend(QObject *parent = nullptr) : FieldCameraBackend(parent)
    {
        auto *media = new QMediaDevices(this);
        connect(media, &QMediaDevices::videoInputsChanged, this,
                &FieldCameraBackend::devicesChanged);
    }
    QVector<FieldCameraDevice> devices() const override
    {
        QVector<FieldCameraDevice> result;
        for (const auto &device : QMediaDevices::videoInputs())
            result.append({device.id(), device.description()});
        return result;
    }
    void start(const QByteArray &id) override
    {
        stop();
        QCameraDevice selected;
        for (const auto &device : QMediaDevices::videoInputs()) {
            if (device.id() == id) { selected = device; break; }
        }
        if (selected.isNull()) {
            emit captureError(tr("所选摄像头已断开"));
            return;
        }
        m_session = new QMediaCaptureSession(this);
        m_camera = new QCamera(selected, this);
        m_sink = new QVideoSink(this);
        m_session->setCamera(m_camera);
        m_session->setVideoSink(m_sink);
        // Prefer 720p near 30 FPS rather than max FPS (USB bandwidth / CPU).
        // Other resolutions remain supported; no advertised format is assumed.
        QCameraFormat best;
        double bestScore = -1e20;
        for (const auto &format : selected.videoFormats()) {
            if (format.maxFrameRate() <= 0 || format.resolution().isEmpty()) continue;
            const QSize size = format.resolution();
            const double distance = qAbs(size.width() - 1280) + qAbs(size.height() - 720);
            const double fpsDistance = format.minFrameRate() <= 30 && format.maxFrameRate() >= 30
                                           ? 0 : qMin(qAbs(format.minFrameRate() - 30),
                                                      qAbs(format.maxFrameRate() - 30));
            const double score = -distance - fpsDistance * 40;
            if (best.isNull() || score > bestScore) { best = format; bestScore = score; }
        }
        if (!best.isNull()) m_camera->setCameraFormat(best);
        const auto generation = m_generation;
        connect(m_sink, &QVideoSink::videoFrameChanged, this,
                [this, generation](const QVideoFrame &frame) {
            if (generation != m_generation || !frame.isValid()) return;
            const QImage image = frame.toImage();
            if (!image.isNull()) emit frameReady(image);
        });
        connect(m_camera, &QCamera::errorOccurred, this,
                [this, generation](QCamera::Error, const QString &message) {
            if (generation == m_generation) emit captureError(message);
        });
        m_camera->start();
    }
    void stop() override
    {
        ++m_generation;
        if (m_sink) m_sink->disconnect(this);
        if (m_camera) { m_camera->disconnect(this); m_camera->stop(); }
        if (m_session) { m_session->setCamera(nullptr); m_session->setVideoSink(nullptr); }
        // deleteLater is safe even when recovery originates inside an error signal.
        if (m_camera) m_camera->deleteLater();
        if (m_sink) m_sink->deleteLater();
        if (m_session) m_session->deleteLater();
        m_camera = nullptr; m_sink = nullptr; m_session = nullptr;
    }
private:
    QCamera *m_camera = nullptr;
    QVideoSink *m_sink = nullptr;
    QMediaCaptureSession *m_session = nullptr;
    quint64 m_generation = 0;
};
}

FieldCamera::FieldCamera(QObject *parent)
    : FieldCamera(new QtFieldCameraBackend, parent) {}

FieldCamera::FieldCamera(FieldCameraBackend *backend, QObject *parent)
    : QObject(parent), m_backend(backend)
{
    m_backend->setParent(this);
    m_retryTimer = new QTimer(this);
    m_retryTimer->setObjectName(QStringLiteral("fieldCameraRetry"));
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &FieldCamera::attemptStart);
    m_watchdog = new QTimer(this);
    m_watchdog->setObjectName(QStringLiteral("fieldCameraWatchdog"));
    m_watchdog->setInterval(1000);
    connect(m_watchdog, &QTimer::timeout, this, &FieldCamera::watchdog);
    connect(m_backend, &FieldCameraBackend::devicesChanged, this, &FieldCamera::refreshDevices);
    connect(m_backend, &FieldCameraBackend::frameReady, this, &FieldCamera::onFrame);
    connect(m_backend, &FieldCameraBackend::captureError, this, &FieldCamera::onError);
    m_status = tr("摄像头未启动（仅记忆设备，不自动采集）");
}
FieldCamera::~FieldCamera() { m_backend->stop(); }
QVector<FieldCameraDevice> FieldCamera::devices() const { return m_backend->devices(); }
bool FieldCamera::devicePresent() const
{
    for (const auto &device : devices()) if (device.id == m_selected) return true;
    return false;
}
void FieldCamera::setStatus(const QString &message)
{
    m_status = message;
    emit stateChanged();
}
void FieldCamera::closeCapture()
{
    m_acceptFrames = false;
    m_watchdog->stop();
    m_backend->stop();
    m_live = false;
    m_frameClock.invalidate();
    m_stableClock.invalidate();
    emit frameReady(QImage());
}
void FieldCamera::selectDevice(const QByteArray &id)
{
    if (id == m_selected) return;
    const bool restart = m_requested;
    stop();
    m_selected = id;
    setStatus(id.isEmpty() ? tr("请选择摄像头") : tr("摄像头已选择，点击启动采集"));
    if (restart) start();
}
void FieldCamera::start()
{
    m_retryTimer->stop();
    closeCapture();
    m_failures = 0;
    m_requested = !m_selected.isEmpty();
    if (!m_requested) { setStatus(tr("请选择摄像头后再启动")); return; }
    attemptStart();
}
void FieldCamera::stop()
{
    m_requested = false;
    m_retryTimer->stop();
    closeCapture();
    setStatus(tr("摄像头已停止，设备已释放"));
}
void FieldCamera::attemptStart()
{
    if (!m_requested) return;
    m_retryTimer->stop();
    closeCapture();
    if (!devicePresent()) {
        setStatus(tr("摄像头已断开，等待同一设备重连；可点击停止取消"));
        return;
    }
    m_acceptFrames = true;
    m_frameClock.start();
    m_watchdog->start();
    setStatus(tr("正在打开摄像头，等待首帧…"));
    m_backend->start(m_selected);
}
void FieldCamera::refreshDevices()
{
    emit devicesChanged();
    if (!m_requested) return;
    if (!devicePresent()) {
        m_retryTimer->stop();
        closeCapture();
        setStatus(tr("摄像头已断开，等待同一设备重连；可点击停止取消"));
    } else if (!m_acceptFrames && !m_retryTimer->isActive()) {
        m_failures = 0;
        m_retryTimer->start(300);
    }
}
void FieldCamera::retry(const QString &reason)
{
    closeCapture();
    emit logMessage(tr("[本地摄像头] %1").arg(reason));
    if (++m_failures > 3) {
        m_requested = false;
        setStatus(tr("%1；重试失败，已停止。请检查 Windows 摄像头权限、占用和 USB 连接后重新启动").arg(reason));
        return;
    }
    const int delay = 1000 * (1 << (m_failures - 1));
    setStatus(tr("%1；%2 秒后重试（%3/3）").arg(reason).arg(delay / 1000).arg(m_failures));
    m_retryTimer->start(delay);
}
void FieldCamera::onError(const QString &message)
{
    if (m_requested && m_acceptFrames) retry(tr("摄像头错误：%1").arg(message));
}
void FieldCamera::onFrame(const QImage &frame)
{
    if (!m_requested || !m_acceptFrames || frame.isNull()) return;
    m_frameClock.restart();
    if (!m_live) {
        m_live = true;
        m_stableClock.start();
        setStatus(tr("采集中 · %1 × %2 · 本地全场视角").arg(frame.width()).arg(frame.height()));
        emit logMessage(tr("[本地摄像头] 已收到首帧 %1 × %2").arg(frame.width()).arg(frame.height()));
    } else if (m_stableClock.elapsed() >= 10000) m_failures = 0;
    emit frameReady(frame);
}
void FieldCamera::watchdog()
{
    if (m_requested && m_acceptFrames && m_frameClock.isValid() && m_frameClock.elapsed() > 6000)
        retry(tr("6 秒未收到视频帧（设备占用、权限受限或采集停滞）"));
}
