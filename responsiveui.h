#ifndef RESPONSIVEUI_H
#define RESPONSIVEUI_H

#include <QByteArray>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLayout>
#include <QMargins>
#include <QPointer>
#include <QScreen>
#include <QSize>
#include <QVector>
#include <QWidget>

inline QSize responsiveWindowSizeFromEnvironment(const char *environmentName,
                                                  const QSize &fallback)
{
    const QByteArray value = qgetenv(environmentName).trimmed().toLower();
    const int separator = value.indexOf('x');
    if (separator <= 0)
        return fallback;

    bool widthOk = false;
    bool heightOk = false;
    const int width = value.left(separator).toInt(&widthOk);
    const int height = value.mid(separator + 1).toInt(&heightOk);
    return widthOk && heightOk && width >= 320 && height >= 240
               ? QSize(width, height)
               : fallback;
}

inline QScreen *responsiveScreenFromEnvironment(const char *environmentName)
{
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty())
        return QGuiApplication::primaryScreen();

    const QByteArray requested = qgetenv(environmentName).trimmed().toLower();
    if (requested == QByteArrayLiteral("secondary")
        || requested == QByteArrayLiteral("non-primary")) {
        const QScreen *primary = QGuiApplication::primaryScreen();
        for (QScreen *screen : screens) {
            if (screen != primary)
                return screen;
        }
    }

    bool ok = false;
    const int index = requested.toInt(&ok);
    if (ok && index >= 0 && index < screens.size())
        return screens.at(index);
    return QGuiApplication::primaryScreen();
}

// Scales widget chrome authored at a reference window size. Qt layouts remain
// responsible for positioning; this class scales the metrics that layouts do
// not scale themselves (fonts, margins, spacing and explicit size limits).
// Keeping this in one helper avoids each window growing a separate collection
// of pixel constants and also keeps resize behaviour deterministic in tests.
class ResponsiveUiScaler final
{
public:
    void capture(QWidget *root, const QSize &referenceSize,
                 qreal minimumScale = 0.55, qreal maximumScale = 2.5)
    {
        m_root = root;
        m_referenceSize = referenceSize;
        m_minimumScale = minimumScale;
        m_maximumScale = maximumScale;
        m_widgets.clear();
        m_layouts.clear();
        m_scale = -1.0;

        if (!root)
            return;

        QList<QWidget *> widgets = root->findChildren<QWidget *>();
        widgets.prepend(root);
        m_widgets.reserve(widgets.size());
        for (QWidget *widget : widgets) {
            WidgetMetrics metrics;
            metrics.widget = widget;
            metrics.font = widget->font();
            metrics.minimumSize = widget->minimumSize();
            metrics.maximumSize = widget->maximumSize();
            m_widgets.append(metrics);
        }

        const QList<QLayout *> layouts = root->findChildren<QLayout *>();
        m_layouts.reserve(layouts.size());
        for (QLayout *layout : layouts) {
            LayoutMetrics metrics;
            metrics.layout = layout;
            metrics.margins = layout->contentsMargins();
            metrics.spacing = layout->spacing();
            if (auto *grid = qobject_cast<QGridLayout *>(layout)) {
                metrics.horizontalSpacing = grid->horizontalSpacing();
                metrics.verticalSpacing = grid->verticalSpacing();
            } else if (auto *form = qobject_cast<QFormLayout *>(layout)) {
                metrics.horizontalSpacing = form->horizontalSpacing();
                metrics.verticalSpacing = form->verticalSpacing();
            }
            m_layouts.append(metrics);
        }
    }

    qreal apply(const QSize &windowSize)
    {
        if (!m_root || !m_referenceSize.isValid() || !windowSize.isValid())
            return 1.0;

        const qreal widthScale = qreal(windowSize.width()) / m_referenceSize.width();
        const qreal heightScale = qreal(windowSize.height()) / m_referenceSize.height();
        const qreal nextScale = qBound(m_minimumScale,
                                       qMin(widthScale, heightScale),
                                       m_maximumScale);
        if (qAbs(nextScale - m_scale) < 0.005)
            return m_scale;
        m_scale = nextScale;

        for (const WidgetMetrics &metrics : qAsConst(m_widgets)) {
            QWidget *widget = metrics.widget.data();
            if (!widget)
                continue;

            QFont font = metrics.font;
            if (font.pointSizeF() > 0.0)
                font.setPointSizeF(qMax(6.0, font.pointSizeF() * m_scale));
            else if (font.pixelSize() > 0)
                font.setPixelSize(qMax(7, qRound(font.pixelSize() * m_scale)));
            widget->setFont(font);

            widget->setMinimumSize(scaleMinimum(metrics.minimumSize));
            widget->setMaximumSize(scaleMaximum(metrics.maximumSize));
        }

        for (const LayoutMetrics &metrics : qAsConst(m_layouts)) {
            QLayout *layout = metrics.layout.data();
            if (!layout)
                continue;
            layout->setContentsMargins(scale(metrics.margins.left()),
                                       scale(metrics.margins.top()),
                                       scale(metrics.margins.right()),
                                       scale(metrics.margins.bottom()));
            if (metrics.spacing >= 0)
                layout->setSpacing(scale(metrics.spacing));
            if (auto *grid = qobject_cast<QGridLayout *>(layout)) {
                if (metrics.horizontalSpacing >= 0)
                    grid->setHorizontalSpacing(scale(metrics.horizontalSpacing));
                if (metrics.verticalSpacing >= 0)
                    grid->setVerticalSpacing(scale(metrics.verticalSpacing));
            } else if (auto *form = qobject_cast<QFormLayout *>(layout)) {
                if (metrics.horizontalSpacing >= 0)
                    form->setHorizontalSpacing(scale(metrics.horizontalSpacing));
                if (metrics.verticalSpacing >= 0)
                    form->setVerticalSpacing(scale(metrics.verticalSpacing));
            }
            layout->invalidate();
        }
        m_root->updateGeometry();
        return m_scale;
    }

    qreal scaleFactor() const { return m_scale > 0.0 ? m_scale : 1.0; }
    int scaled(int value) const { return scale(value); }

private:
    struct WidgetMetrics {
        QPointer<QWidget> widget;
        QFont font;
        QSize minimumSize;
        QSize maximumSize;
    };

    struct LayoutMetrics {
        QPointer<QLayout> layout;
        QMargins margins;
        int spacing = -1;
        int horizontalSpacing = -1;
        int verticalSpacing = -1;
    };

    int scale(int value) const
    {
        if (value == 0)
            return 0;
        return qMax(1, qRound(value * scaleFactor()));
    }

    QSize scaleMinimum(const QSize &size) const
    {
        return QSize(scale(size.width()), scale(size.height()));
    }

    QSize scaleMaximum(const QSize &size) const
    {
        constexpr int widgetSizeMax = 16777215;
        return QSize(size.width() >= widgetSizeMax ? widgetSizeMax : scale(size.width()),
                     size.height() >= widgetSizeMax ? widgetSizeMax : scale(size.height()));
    }

    QPointer<QWidget> m_root;
    QSize m_referenceSize;
    qreal m_minimumScale = 0.55;
    qreal m_maximumScale = 2.5;
    qreal m_scale = -1.0;
    QVector<WidgetMetrics> m_widgets;
    QVector<LayoutMetrics> m_layouts;
};

#endif // RESPONSIVEUI_H
