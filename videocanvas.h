#ifndef VIDEOCANVAS_H
#define VIDEOCANVAS_H

#include <QImage>
#include <QOpenGLWidget>

// Lightweight GPU-backed frame surface used by the broadcast/client window.
// It avoids rebuilding a full-screen QPixmap on the GUI thread for every frame.
class VideoCanvas final : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit VideoCanvas(QWidget *parent = nullptr);

    void setFrame(const QImage &frame);
    void clearFrame();

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    QImage m_frame;
};

#endif // VIDEOCANVAS_H
