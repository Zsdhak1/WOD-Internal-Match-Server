#include "videocanvas.h"

#include <QPainter>

VideoCanvas::VideoCanvas(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAutoFillBackground(false);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
}

void VideoCanvas::setFrame(const QImage &frame)
{
    if (frame.isNull()) {
        clearFrame();
        return;
    }
    m_frame = frame;
    update();
}

void VideoCanvas::clearFrame()
{
    m_frame = QImage();
    update();
}

void VideoCanvas::initializeGL()
{
}

void VideoCanvas::paintGL()
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (m_frame.isNull() || width() <= 0 || height() <= 0)
        return;

    // Match KeepAspectRatioByExpanding: fill the viewport and crop the excess.
    const qreal scale = qMax(qreal(width()) / m_frame.width(),
                             qreal(height()) / m_frame.height());
    const QSizeF visible(qreal(width()) / scale, qreal(height()) / scale);
    const QRectF source((m_frame.width() - visible.width()) / 2.0,
                        (m_frame.height() - visible.height()) / 2.0,
                        visible.width(), visible.height());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(rect(), m_frame, source);
}
