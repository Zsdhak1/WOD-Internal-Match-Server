#include "broadcastwindow.h"

#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEasingCurve>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QProcess>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWindow>

class TickerMarqueeWidget final : public QWidget
{
public:
    explicit TickerMarqueeWidget(QWidget *parent = nullptr)
        : QWidget(parent), m_timer(new QTimer(this))
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(m_timer, &QTimer::timeout, this, [this] {
            if (m_text.isEmpty() || width() <= 0)
                return;
            const QFontMetrics metrics(font());
            const int cycle = qMax(1, metrics.horizontalAdvance(m_text) + 96);
            m_offset -= 2;
            if (m_offset < -cycle)
                m_offset = width();
            update();
        });
    }

    void setText(const QString &text)
    {
        m_text = text.trimmed();
        m_offset = width();
        if (m_text.isEmpty())
            m_timer->stop();
        else if (isVisible())
            m_timer->start(30);
        update();
    }

    QString text() const { return m_text; }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (!m_text.isEmpty())
            m_timer->start(30);
    }

    void hideEvent(QHideEvent *event) override
    {
        m_timer->stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setPen(QColor(QStringLiteral("#f3fbff")));

        if (m_text.isEmpty())
            return;

        const QFontMetrics metrics(font());
        const int baseline = (height() - metrics.height()) / 2 + metrics.ascent();
        painter.save();
        painter.setClipRect(rect());
        const int cycle = qMax(1, metrics.horizontalAdvance(m_text) + 96);
        int x = m_offset;
        while (x < width()) {
            painter.drawText(x, baseline, m_text);
            x += cycle;
        }
        painter.restore();
    }

private:
    QString m_text;
    int m_offset = 0;
    QTimer *m_timer = nullptr;
};

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr quint8 kRedTeam = 1;
constexpr quint8 kBlueTeam = 2;
constexpr int kMatchDurationSeconds = 60;
constexpr int kMaxHp = 5000;
constexpr int kMaxHeat = 1000;
constexpr int kCenterHudWidth = 786;
constexpr int kCenterHudHeight = 225;
constexpr int kTeamHudWidth = 420;
constexpr int kTeamHudHeight = 199;
constexpr int kHudGap = 18;
constexpr int kCenterHudNativeYOffset = -34;
constexpr int kCenterTimerY = 40;
constexpr int kMatchTitleNativeYOffset = -15;
constexpr int kSettlementFrameWidth = 960;
constexpr int kSettlementFrameHeight = 540;
constexpr int kSettlementFrameBytes = kSettlementFrameWidth * kSettlementFrameHeight * 4;
constexpr int kSettlementFrameIntervalMs = 40;
constexpr int kSettlementFrameQueueLimit = 8;

QPoint legacyLayoutOffset(const QString &elementId)
{
    if (elementId == QStringLiteral("center_hud"))
        return QPoint(0, -40);
    if (elementId == QStringLiteral("left_score_panel"))
        return QPoint(140, 10);
    if (elementId == QStringLiteral("right_score_panel"))
        return QPoint(-140, 10);
    if (elementId == QStringLiteral("match_title"))
        return QPoint(0, -12);
    if (elementId == QStringLiteral("match_timer"))
        return QPoint(0, -50);
    return QPoint();
}

QPoint defaultLayoutOffset(const QString &elementId)
{
    // These values used to be applied with QWidget::move().  They are now
    // part of the stable layout geometry below, so a fresh layout starts at
    // the native coordinates instead of carrying hidden runtime offsets.
    if (elementId == QStringLiteral("center_hud"))
        return QPoint(0, 30);
    if (elementId == QStringLiteral("left_score_panel"))
        return QPoint(60, 20);
    if (elementId == QStringLiteral("right_score_panel"))
        return QPoint(-60, 20);
    if (elementId == QStringLiteral("match_title"))
        return QPoint(0, kMatchTitleNativeYOffset);
    return QPoint();
}

#ifdef Q_OS_WIN
void coverTaskbarOnScreen(QWidget *window, const QRect &geometry)
{
    if (!window)
        return;

    const HWND handle = reinterpret_cast<HWND>(window->winId());
    if (!handle)
        return;

    LONG_PTR extendedStyle = GetWindowLongPtr(handle, GWL_EXSTYLE);
    extendedStyle |= WS_EX_TOOLWINDOW;
    extendedStyle &= ~WS_EX_APPWINDOW;
    SetWindowLongPtr(handle, GWL_EXSTYLE, extendedStyle);
    SetWindowPos(handle,
                 HWND_TOPMOST,
                 geometry.x(),
                 geometry.y(),
                 geometry.width(),
                 geometry.height(),
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void releaseTaskbarCover(QWidget *window)
{
    if (!window || !window->winId())
        return;

    const HWND handle = reinterpret_cast<HWND>(window->winId());
    SetWindowPos(handle,
                 HWND_NOTOPMOST,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
#endif

QPixmap assetCrop(const QString &resource, const QRect &rect)
{
    const QPixmap atlas(resource);
    return atlas.isNull() ? QPixmap() : atlas.copy(rect);
}

QLabel *makeLabel(QWidget *parent, Qt::Alignment alignment = Qt::AlignLeft)
{
    auto *label = new QLabel(parent);
    label->setAlignment(alignment);
    return label;
}

void setTextColor(QLabel *label, const QColor &color, bool bold = false)
{
    label->setStyleSheet(QStringLiteral("color: %1;%2")
                             .arg(color.name(), bold ? QStringLiteral(" font-weight: 700;")
                                                     : QString()));
}

QString defaultTeamName(quint8 team)
{
    return team == kRedTeam ? QStringLiteral("红方") : QStringLiteral("蓝方");
}

class AtlasProgressBar final : public QProgressBar
{
public:
    AtlasProgressBar(const QPixmap &asset,
                     bool reverse,
                     bool mirrorAsset,
                     QWidget *parent = nullptr)
        : QProgressBar(parent),
          m_asset(mirrorAsset ? QPixmap::fromImage(asset.toImage().mirrored(true, false))
                              : asset),
          m_reverse(reverse)
    {
        setTextVisible(false);
    }

    void setDamageBufferValue(int bufferValue)
    {
        m_damageBufferValue = qBound(minimum(), bufferValue, maximum());
        update();
    }

    int damageBufferValue() const
    {
        return m_damageBufferValue >= minimum() ? m_damageBufferValue : value();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QPixmap scaled = m_asset.scaled(size(), Qt::IgnoreAspectRatio,
                                              Qt::SmoothTransformation);

        painter.setOpacity(0.22);
        painter.drawPixmap(rect(), scaled);

        const int span = maximum() - minimum();
        const qreal ratio = span > 0
                                ? qBound<qreal>(0.0, qreal(value() - minimum()) / span, 1.0)
                                : 0.0;
        const int fillWidth = qRound(width() * ratio);
        const QRect fillRect = m_reverse
                                   ? QRect(width() - fillWidth, 0, fillWidth, height())
                                   : QRect(0, 0, fillWidth, height());
        painter.save();
        painter.setClipRect(fillRect);
        painter.setOpacity(1.0);
        painter.drawPixmap(rect(), scaled);
        painter.restore();

        const int bufferValue = qMax(value(), damageBufferValue());
        const qreal bufferRatio = span > 0
                                      ? qBound<qreal>(0.0,
                                                     qreal(bufferValue - minimum()) / span,
                                                     1.0)
                                      : 0.0;
        const int bufferWidth = qRound(width() * bufferRatio);
        if (bufferWidth > fillWidth) {
            const QRect bufferRect = m_reverse
                                         ? QRect(width() - bufferWidth,
                                                 3,
                                                 bufferWidth - fillWidth,
                                                 qMax(1, height() - 6))
                                         : QRect(fillWidth,
                                                 3,
                                                 bufferWidth - fillWidth,
                                                 qMax(1, height() - 6));
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 228));
            painter.drawRect(bufferRect);
            painter.setBrush(QColor(255, 255, 255, 80));
            const QRect highlight = bufferRect.adjusted(0, 0, 0, -qMax(1, bufferRect.height() / 2));
            painter.drawRect(highlight);
        }

    }

private:
    QPixmap m_asset;
    bool m_reverse = false;
    int m_damageBufferValue = -1;
};

class CenterHudCanvas final : public QWidget
{
public:
    explicit CenterHudCanvas(QWidget *parent = nullptr)
        : QWidget(parent),
          m_n15(assetCrop(QStringLiteral(":/broadcast/notepad_atlas.png"),
                          QRect(7, 156, 400, 94))),
          m_n16Left(assetCrop(QStringLiteral(":/broadcast/notepad_atlas.png"),
                              QRect(421, 194, 82, 62))),
          m_n16Right(assetCrop(QStringLiteral(":/broadcast/notepad_atlas.png"),
                               QRect(421, 133, 82, 61)))
    {
        setObjectName(QStringLiteral("centerHud"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
        setMinimumSize(1, 1);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

        // The three atlas pieces are painted by this widget as one composition.
        // The score-panel widgets below are transparent geometry proxies only;
        // they carry the score labels without being able to replace or reorder
        // the N15/N16 artwork.
        m_leftBadgeLayer = new QWidget(this);
        m_rightBadgeLayer = new QWidget(this);
        for (QWidget *panel : {m_leftBadgeLayer, m_rightBadgeLayer}) {
            panel->setAttribute(Qt::WA_TranslucentBackground);
            panel->setAttribute(Qt::WA_NoSystemBackground);
            panel->setAttribute(Qt::WA_TransparentForMouseEvents);
            panel->setAutoFillBackground(false);
            panel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        }

        m_content = new QWidget(this);
        m_content->setAttribute(Qt::WA_TranslucentBackground);
        m_content->setAttribute(Qt::WA_NoSystemBackground);
        m_content->setAutoFillBackground(false);
        m_content->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        m_content->setAttribute(Qt::WA_TransparentForMouseEvents);

        for (QLabel **label : {&m_leftScore, &m_rightScore}) {
            *label = new QLabel(QStringLiteral("0"),
                                label == &m_leftScore ? m_leftBadgeLayer : m_rightBadgeLayer);
            (*label)->setAlignment(Qt::AlignCenter);
            QFont scoreFont = (*label)->font();
            scoreFont.setBold(true);
            scoreFont.setPointSize(34);
            (*label)->setFont(scoreFont);
            (*label)->setStyleSheet(QStringLiteral("color: #effcff; background: transparent;"));
            (*label)->setAttribute(Qt::WA_TransparentForMouseEvents);
        }

    }

    QSize sizeHint() const override { return QSize(kCenterHudWidth, kCenterHudHeight); }

    QWidget *contentWidget() const { return m_content; }
    QLabel *leftScoreLabel() const { return m_leftScore; }
    QLabel *rightScoreLabel() const { return m_rightScore; }
    QWidget *leftScorePanel() const { return m_leftBadgeLayer; }
    QWidget *rightScorePanel() const { return m_rightBadgeLayer; }

    void setElementOffset(const QString &elementId, const QPoint &offset)
    {
        m_elementOffsets.insert(elementId, offset);
        updateElementGeometry();
    }

    void setElementSize(const QString &elementId, const QSize &size)
    {
        if (size.isValid() && size.width() > 0 && size.height() > 0)
            m_elementSizes.insert(elementId, size);
        else
            m_elementSizes.remove(elementId);
        updateElementGeometry();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        updateElementGeometry();
        QWidget::resizeEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        // Paint order is the contract of this HUD: N15 is the timer base and
        // both N16 score boards are painted on top of it.
        if (!m_n15.isNull())
            painter.drawPixmap(m_panelRect, m_n15);
        if (!m_n16Left.isNull())
            painter.drawPixmap(m_leftBadgeRect, m_n16Left);
        if (!m_n16Right.isNull())
            painter.drawPixmap(m_rightBadgeRect, m_n16Right);
    }

private:
    QRect elementGeometry(const QString &elementId, const QRect &baseGeometry) const
    {
        const QSize size = m_elementSizes.value(elementId, baseGeometry.size());
        const QPoint offset = m_elementOffsets.value(elementId, QPoint());
        return QRect(baseGeometry.topLeft() + offset, size);
    }

    void updateElementGeometry()
    {
        const qreal scaleX = qMax<qreal>(0.01, width() / qreal(kCenterHudWidth));
        const qreal scaleY = qMax<qreal>(0.01, height() / qreal(kCenterHudHeight));
        const int panelWidth = qMax(1, qRound(600 * scaleX));
        const int panelHeight = qMax(1, qRound(141 * scaleY));
        const int badgeWidth = qMax(1, qRound(123 * scaleX));
        const int leftBadgeHeight = qMax(1, qRound(93 * scaleY));
        const int rightBadgeHeight = qMax(1, qRound(92 * scaleY));
        const int overlap = qMax(0, qRound(30 * scaleX));
        // The center HUD is the sole coordinate reference for the score
        // badges.  Do not derive their position from the outer window: that
        // made the result change when the surrounding layout was resized.
        const int panelLeft = qMax(0, (width() - panelWidth) / 2);
        const int leftBadgeOuterAnchor = panelLeft - badgeWidth + overlap;
        const int rightBadgeOuterAnchor = panelLeft + panelWidth - overlap;
        // The authored center HUD starts at the top of this local coordinate
        // system. The title may occupy the signed space above the panel, but
        // the artwork itself must not move when a label changes text.
        const int panelTop = 0;
        const int leftBadgeTop = qMax(0, qRound(10 * scaleY));
        const int rightBadgeTop = qMax(0, qRound(10 * scaleY));

        m_panelRect = QRect(panelLeft, panelTop, panelWidth, panelHeight);
        // The authored N16 positions are the outer anchors. Their final
        // positions come exclusively from layout offsets (60/-60, 20 by
        // default), so the layout file describes the actual requested move.
        const QRect leftBadgeBase(leftBadgeOuterAnchor,
                                  leftBadgeTop,
                                  badgeWidth,
                                  leftBadgeHeight);
        const QRect rightBadgeBase(rightBadgeOuterAnchor,
                                   rightBadgeTop,
                                   badgeWidth,
                                   rightBadgeHeight);
        m_leftBadgeRect = elementGeometry(QStringLiteral("left_score_panel"),
                                           leftBadgeBase);
        m_rightBadgeRect = elementGeometry(QStringLiteral("right_score_panel"),
                                            rightBadgeBase);

        m_leftBadgeLayer->setGeometry(m_leftBadgeRect);
        m_rightBadgeLayer->setGeometry(m_rightBadgeRect);
        m_leftScore->setGeometry(QRect(QPoint(), m_leftBadgeLayer->size()));
        m_rightScore->setGeometry(QRect(QPoint(), m_rightBadgeLayer->size()));
        // Content is a full-size local canvas. Title and timer now use
        // positions relative to the center artwork, never a global offset.
        m_content->setGeometry(0, 0, width(), height());
        m_content->raise();
    }

    QPixmap m_n15;
    QPixmap m_n16Left;
    QPixmap m_n16Right;
    QWidget *m_leftBadgeLayer = nullptr;
    QWidget *m_rightBadgeLayer = nullptr;
    QWidget *m_content = nullptr;
    QLabel *m_leftScore = nullptr;
    QLabel *m_rightScore = nullptr;
    QRect m_panelRect;
    QRect m_leftBadgeRect;
    QRect m_rightBadgeRect;
    QHash<QString, QPoint> m_elementOffsets;
    QHash<QString, QSize> m_elementSizes;
};

class HudElementLayout : public QLayout
{
public:
    explicit HudElementLayout(QWidget *parent = nullptr)
        : QLayout(parent)
    {
        setContentsMargins(0, 0, 0, 0);
    }

    ~HudElementLayout() override
    {
        while (QLayoutItem *item = takeAt(0))
            delete item;
    }

    void addElement(const QString &elementId, QWidget *widget)
    {
        if (elementId.isEmpty() || !widget)
            return;

        addChildWidget(widget);
        m_entries.append({elementId, new QWidgetItem(widget)});
        invalidate();
    }

    void addItem(QLayoutItem *item) override
    {
        if (!item)
            return;
        m_entries.append({QString(), item});
        invalidate();
    }

    void setElementOffset(const QString &elementId, const QPoint &offset)
    {
        m_offsets.insert(elementId, offset);
        invalidate();
    }

    void setElementSize(const QString &elementId, const QSize &size)
    {
        if (size.isValid() && size.width() > 0 && size.height() > 0)
            m_sizes.insert(elementId, size);
        else
            m_sizes.remove(elementId);
        invalidate();
    }

    bool hasElementSize(const QString &elementId) const
    {
        return m_sizes.contains(elementId);
    }

    QLayoutItem *itemAt(int index) const override
    {
        return index >= 0 && index < m_entries.size() ? m_entries.at(index).item : nullptr;
    }

    QLayoutItem *takeAt(int index) override
    {
        if (index < 0 || index >= m_entries.size())
            return nullptr;

        const Entry entry = m_entries.takeAt(index);
        m_offsets.remove(entry.elementId);
        m_sizes.remove(entry.elementId);
        return entry.item;
    }

    int count() const override { return m_entries.size(); }

protected:
    struct Entry {
        QString elementId;
        QLayoutItem *item = nullptr;
    };

    const Entry *entry(const QString &elementId) const
    {
        for (const Entry &candidate : m_entries) {
            if (candidate.elementId == elementId)
                return &candidate;
        }
        return nullptr;
    }

    QSize naturalSize(const QString &elementId) const
    {
        const Entry *candidate = entry(elementId);
        if (!candidate || !candidate->item)
            return QSize(1, 1);

        const QSize hint = candidate->item->sizeHint();
        const QSize minimum = candidate->item->minimumSize();
        return QSize(qMax(1, qMax(hint.width(), minimum.width())),
                     qMax(1, qMax(hint.height(), minimum.height())));
    }

    QSize minimumElementSize(const QString &elementId) const
    {
        const Entry *candidate = entry(elementId);
        if (!candidate || !candidate->item)
            return QSize(1, 1);
        const QSize minimum = candidate->item->minimumSize();
        return QSize(qMax(1, minimum.width()), qMax(1, minimum.height()));
    }

    QSize elementSize(const QString &elementId) const
    {
        return m_sizes.contains(elementId) ? m_sizes.value(elementId)
                                            : naturalSize(elementId);
    }

    QPoint elementOffset(const QString &elementId) const
    {
        return m_offsets.value(elementId, QPoint());
    }

    void setItemGeometry(const QString &elementId, const QRect &geometry)
    {
        const Entry *candidate = entry(elementId);
        if (!candidate || !candidate->item)
            return;

        // QWidgetItem clamps an item to the layout rect.  The original HUD
        // deliberately lets the center artwork extend above that rect, so
        // set the child geometry directly and retain the signed coordinates.
        if (QWidget *widget = candidate->item->widget())
            widget->setGeometry(geometry);
        else
            candidate->item->setGeometry(geometry);
    }

private:
    QVector<Entry> m_entries;
    QHash<QString, QPoint> m_offsets;
    QHash<QString, QSize> m_sizes;
};

class HudTopLayout final : public HudElementLayout
{
public:
    explicit HudTopLayout(QWidget *parent = nullptr)
        : HudElementLayout(parent)
    {
    }

    QSize sizeHint() const override
    {
        const QSize red = elementSize(QStringLiteral("red_team_panel"));
        const QSize center = elementSize(QStringLiteral("center_hud"));
        const QSize blue = elementSize(QStringLiteral("blue_team_panel"));
        return QSize(red.width() + center.width() + blue.width() + kHudGap * 2,
                     qMax(red.height(), qMax(center.height(), blue.height())));
    }

    QSize minimumSize() const override
    {
        const QSize red = hasElementSize(QStringLiteral("red_team_panel"))
                              ? elementSize(QStringLiteral("red_team_panel"))
                              : minimumElementSize(QStringLiteral("red_team_panel"));
        const QSize center = hasElementSize(QStringLiteral("center_hud"))
                                 ? elementSize(QStringLiteral("center_hud"))
                                 : minimumElementSize(QStringLiteral("center_hud"));
        const QSize blue = hasElementSize(QStringLiteral("blue_team_panel"))
                               ? elementSize(QStringLiteral("blue_team_panel"))
                               : minimumElementSize(QStringLiteral("blue_team_panel"));
        return QSize(red.width() + center.width() + blue.width() + kHudGap * 2,
                     qMax(red.height(), qMax(center.height(), blue.height())));
    }

    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }

protected:
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);

        const QString redId = QStringLiteral("red_team_panel");
        const QString centerId = QStringLiteral("center_hud");
        const QString blueId = QStringLiteral("blue_team_panel");
        QSize center = elementSize(centerId);
        const QSize redNatural = naturalSize(redId);
        const QSize blueNatural = naturalSize(blueId);
        QSize redSize = hasElementSize(redId)
                            ? elementSize(redId)
                            : QSize(kTeamHudWidth, qMax(kTeamHudHeight, redNatural.height()));
        QSize blueSize = hasElementSize(blueId)
                             ? elementSize(blueId)
                             : QSize(kTeamHudWidth, qMax(kTeamHudHeight, blueNatural.height()));
        int gap = kHudGap;

        // The reference HUD is authored for a wide output. Scale the complete
        // three-part header when the actual output is narrower, so the center
        // artwork remains centered and the team panels stay on-screen.
        const bool hasCustomSize = hasElementSize(redId) || hasElementSize(centerId)
                                   || hasElementSize(blueId);
        const int preferredWidth = kTeamHudWidth * 2 + kCenterHudWidth + kHudGap * 2;
        if (!hasCustomSize && rect.width() < preferredWidth) {
            const qreal scale = qBound<qreal>(0.01,
                                              rect.width() / qreal(preferredWidth),
                                              1.0);
            redSize = QSize(qMax(1, qRound(kTeamHudWidth * scale)),
                            qMax(1, qRound(kTeamHudHeight * scale)));
            blueSize = QSize(qMax(1, qRound(kTeamHudWidth * scale)),
                             qMax(1, qRound(kTeamHudHeight * scale)));
            center.setWidth(qMax(1, qRound(kCenterHudWidth * scale)));
            gap = qMax(2, qRound(kHudGap * scale));
        }

        // The center HUD remains centered, while the default team panels use
        // all space on their respective sides. Use the overlay's full width
        // here so the top panels reach the window edges even though the rest
        // of the overlay keeps its authored horizontal margins.
        const QRect horizontalBounds = parentWidget() ? parentWidget()->contentsRect() : rect;
        const int leftEdge = horizontalBounds.x();
        const int rightEdge = horizontalBounds.x() + horizontalBounds.width();
        const int centerLeft = leftEdge + (horizontalBounds.width() - center.width()) / 2;

        if (!hasElementSize(redId))
            redSize.setWidth(qMax(1, centerLeft - gap - leftEdge));
        if (!hasElementSize(blueId))
            blueSize.setWidth(qMax(1, rightEdge - (centerLeft + center.width() + gap)));

        const QPoint redOffset = elementOffset(redId);
        const QPoint centerOffset = elementOffset(centerId);
        const QPoint blueOffset = elementOffset(blueId);
        setItemGeometry(redId,
                        QRect(leftEdge + redOffset.x(),
                              rect.y() + redOffset.y(),
                              redSize.width(),
                              redSize.height()));
        setItemGeometry(centerId,
                        QRect(centerLeft + centerOffset.x(),
                              rect.y() + kCenterHudNativeYOffset + centerOffset.y(),
                              center.width(),
                              center.height()));
        setItemGeometry(blueId,
                        QRect(centerLeft + center.width() + gap + blueOffset.x(),
                              rect.y() + blueOffset.y(),
                              blueSize.width(),
                              blueSize.height()));
    }
};

class HudCenterContentLayout final : public HudElementLayout
{
public:
    explicit HudCenterContentLayout(QWidget *parent = nullptr)
        : HudElementLayout(parent)
    {
    }

    QSize sizeHint() const override
    {
        const QSize title = elementSize(QStringLiteral("match_title"));
        const QSize timer = elementSize(QStringLiteral("match_timer"));
        return QSize(qMax(title.width(), timer.width()), title.height() + timer.height() + 2);
    }

    QSize minimumSize() const override { return sizeHint(); }

protected:
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);

        const QString titleId = QStringLiteral("match_title");
        const QString timerId = QStringLiteral("match_timer");
        const QSize title = elementSize(titleId);
        const QSize timer = elementSize(timerId);
        const int titleWidth = hasElementSize(titleId) ? title.width() : rect.width();
        const int timerWidth = hasElementSize(timerId) ? timer.width() : rect.width();
        // Title and timer use fixed anchors in the center HUD coordinate
        // system. Their positions must not depend on the other label's text
        // metrics.
        const qreal scaleX = rect.width() / qreal(kCenterHudWidth);
        const qreal scaleY = rect.height() / qreal(kCenterHudHeight);
        const auto scaledOffset = [scaleX, scaleY](const QPoint &offset) {
            return QPoint(qRound(offset.x() * scaleX), qRound(offset.y() * scaleY));
        };
        const QPoint titleOffset = scaledOffset(elementOffset(titleId));
        const QPoint timerOffset = scaledOffset(elementOffset(timerId));

        // The title is a child of centerHud, so a negative Y would place it
        // outside the parent's paint region and clip the glyphs. Keep the
        // requested offset when it is visible, but clamp its top edge.
        const int titleY = qMax(rect.y(), rect.y() + titleOffset.y());
        setItemGeometry(titleId,
                        QRect(rect.x() + titleOffset.x(),
                              titleY,
                              titleWidth,
                              title.height()));
        setItemGeometry(timerId,
                        QRect(rect.x() + timerOffset.x(),
                              rect.y() + qRound(kCenterTimerY * scaleY)
                                  + timerOffset.y(),
                              timerWidth,
                              timer.height()));
    }
};

class HudBottomLayout final : public HudElementLayout
{
public:
    explicit HudBottomLayout(QWidget *parent = nullptr)
        : HudElementLayout(parent)
    {
    }

    QSize sizeHint() const override
    {
        const QSize state = elementSize(QStringLiteral("match_state"));
        const QSize source = elementSize(QStringLiteral("source_label"));
        return QSize(state.width() + source.width(), qMax(state.height(), source.height()));
    }

    QSize minimumSize() const override { return sizeHint(); }

    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }

protected:
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);

        const QString stateId = QStringLiteral("match_state");
        const QString sourceId = QStringLiteral("source_label");
        const QSize state = elementSize(stateId);
        const QSize source = elementSize(sourceId);
        const QPoint stateOffset = elementOffset(stateId);
        const QPoint sourceOffset = elementOffset(sourceId);
        const int bottom = rect.y() + rect.height();

        setItemGeometry(stateId,
                        QRect(rect.x() + stateOffset.x(),
                              bottom - state.height() + stateOffset.y(),
                              state.width(),
                              state.height()));
        setItemGeometry(sourceId,
                        QRect(rect.x() + rect.width() - source.width() + sourceOffset.x(),
                              bottom - source.height() + sourceOffset.y(),
                              source.width(),
                              source.height()));
    }
};

// Team panels use authored row coordinates. A flow layout redistributes rows
// whenever a label's text changes, which is exactly the y-drift visible when
// the match starts or a robot changes connection state.
class HudTeamLayout final : public QLayout
{
public:
    explicit HudTeamLayout(QWidget *parent = nullptr)
        : QLayout(parent)
    {
        setContentsMargins(0, 0, 0, 0);
    }

    ~HudTeamLayout() override
    {
        while (QLayoutItem *item = takeAt(0))
            delete item;
    }

    void addElement(const QString &elementId, QWidget *widget)
    {
        if (elementId.isEmpty() || !widget)
            return;
        addChildWidget(widget);
        m_entries.append({elementId, new QWidgetItem(widget)});
        invalidate();
    }

    void addItem(QLayoutItem *item) override
    {
        if (!item)
            return;
        m_entries.append({QString(), item});
        invalidate();
    }

    QLayoutItem *itemAt(int index) const override
    {
        return index >= 0 && index < m_entries.size() ? m_entries.at(index).item : nullptr;
    }

    QLayoutItem *takeAt(int index) override
    {
        if (index < 0 || index >= m_entries.size())
            return nullptr;
        return m_entries.takeAt(index).item;
    }

    int count() const override { return m_entries.size(); }
    QSize sizeHint() const override { return QSize(kTeamHudWidth, kTeamHudHeight); }
    QSize minimumSize() const override { return sizeHint(); }

protected:
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        const qreal scaleX = qMax<qreal>(0.01, rect.width() / qreal(kTeamHudWidth));
        const qreal scaleY = qMax<qreal>(0.01, rect.height() / qreal(kTeamHudHeight));
        const auto scaleRect = [rect, scaleX, scaleY](const QRect &native) {
            return QRect(rect.x() + qRound(native.x() * scaleX),
                         rect.y() + qRound(native.y() * scaleY),
                         qMax(1, qRound(native.width() * scaleX)),
                         qMax(1, qRound(native.height() * scaleY)));
        };

        setItemGeometry(QStringLiteral("team_row"), scaleRect(QRect(22, 10, 376, 40)));
        setItemGeometry(QStringLiteral("card_row"), scaleRect(QRect(22, 39, 376, 26)));
        setItemGeometry(QStringLiteral("health_bar"), scaleRect(QRect(22, 58, 376, 34)));
        setItemGeometry(QStringLiteral("heat_bar"), scaleRect(QRect(22, 95, 376, 20)));
        setItemGeometry(QStringLiteral("status_row"), scaleRect(QRect(22, 122, 376, 28)));
    }

private:
    struct Entry {
        QString elementId;
        QLayoutItem *item = nullptr;
    };

    const Entry *entry(const QString &elementId) const
    {
        for (const Entry &candidate : m_entries) {
            if (candidate.elementId == elementId)
                return &candidate;
        }
        return nullptr;
    }

    void setItemGeometry(const QString &elementId, const QRect &geometry)
    {
        const Entry *candidate = entry(elementId);
        if (!candidate || !candidate->item)
            return;
        if (QWidget *widget = candidate->item->widget())
            widget->setGeometry(geometry);
        else
            candidate->item->setGeometry(geometry);
    }

    QVector<Entry> m_entries;
};
} // namespace

BroadcastWindow::BroadcastWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("赛事转播画面"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose, false);

    m_teamNames.insert(kRedTeam, tr("红方"));
    m_teamNames.insert(kBlueTeam, tr("蓝方"));

    m_matchTimer = new QTimer(this);
    m_matchTimer->setInterval(1000);
    connect(m_matchTimer, &QTimer::timeout, this, [this] {
        if (m_remainingSeconds > 0)
            --m_remainingSeconds;

        if (m_remainingSeconds == 0) {
            finishMatch(settlementTypeForScores());
        } else {
            updateTimerDisplay();
        }
    });

    m_programRenderTimer = new QTimer(this);
    m_programRenderTimer->setSingleShot(true);
    m_programRenderTimer->setInterval(33);
    connect(m_programRenderTimer, &QTimer::timeout,
            this, &BroadcastWindow::renderProgramFrame);

    m_settlementFrameTimer = new QTimer(this);
    m_settlementFrameTimer->setInterval(kSettlementFrameIntervalMs);
    connect(m_settlementFrameTimer, &QTimer::timeout, this, [this] {
        if (!m_settlementFrames.isEmpty()) {
            renderSettlementFrame(m_settlementFrames.dequeue());
        }

        if (m_settlementProcess && m_settlementFrames.size() < kSettlementFrameQueueLimit)
            consumeSettlementOutput();

        if (m_settlementFinished && m_settlementFrames.isEmpty()
            && m_settlementBuffer.size() < kSettlementFrameBytes) {
            m_settlementFrameTimer->stop();
            QProcess *process = m_settlementProcess;
            m_settlementProcess = nullptr;
            if (process)
                process->deleteLater();
        }
    });

    buildUi();
    resetMatch();
}

void BroadcastWindow::buildUi()
{
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("broadcastRoot"));
    central->setStyleSheet(QStringLiteral(
        "QWidget#broadcastRoot { background: #06090d; color: #f4f7fa; }"
        "QLabel#videoBackground { background: #06090d; color: #c5d0d8; font-size: 28px; }"
        "QFrame#teamHud { background: rgba(8, 14, 21, 158); border: 3px solid #41515e; border-radius: 8px; }"
        "QFrame#teamHud[team=\"red\"] { border-color: #e24e5a; }"
        "QFrame#teamHud[team=\"blue\"] { border-color: #4a9cf5; }"
        "QWidget#centerHud { background: transparent; border: none; }"
        "QWidget#interactionLayer { background: transparent; border: none; }"
        "QLabel#redTeam { color: #ff6872; }"
        "QLabel#blueTeam { color: #72b4ff; }"
        "QLabel#timer { color: #f8fbfd; background: transparent; }"
        "QLabel#score { color: #d7e0e6; }"
        "QLabel#matchState { color: #e2e9ee; background: rgba(8, 14, 21, 218); "
        "border: 2px solid #43515e; border-radius: 6px; padding: 8px 14px; }"
        "QLabel#sourceLabel { color: #f4f7fa; background: rgba(8, 14, 21, 218); "
        "border: 2px solid #43515e; border-radius: 6px; padding: 8px 14px; }"
        "QFrame#tickerBar { background: rgba(5, 11, 17, 236); border: 2px solid #435766; "
        "border-left: 5px solid #38c6d9; border-radius: 5px; }"
        "QLabel#tickerTag { color: #38c6d9; background: transparent; font-weight: 700; "
        "letter-spacing: 1px; }"
        "QLabel#connectionStatus { background: transparent; font-weight: 700; }"
        "QLabel#damageNotice { color: #ffffff; background: transparent; "
        "border: none; padding: 0px; font-weight: 700; }"
        "QProgressBar { background: rgba(0, 0, 0, 150); border: 2px solid #9aa7b0; "
        "border-radius: 5px; height: 24px; text-align: center; color: #f5f7f8; }"
        "QProgressBar#redHealth::chunk { background: #e24e5a; border-radius: 3px; }"
        "QProgressBar#blueHealth::chunk { background: #4a9cf5; border-radius: 3px; }"
        "QProgressBar#redHeat::chunk { background: #f0aa36; border-radius: 3px; }"
        "QProgressBar#blueHeat::chunk { background: #55c8d7; border-radius: 3px; }"));

    auto *stack = new QGridLayout(central);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(0);

    m_programView = new QLabel(tr("等待全场实时转播画面\n请由视频接收模块调用 setProgramFrame()"), central);
    m_programView->setObjectName(QStringLiteral("videoBackground"));
    m_programView->setAlignment(Qt::AlignCenter);
    m_programView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    stack->addWidget(m_programView, 0, 0);

    auto *overlay = new QWidget(central);
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlay->setStyleSheet(QStringLiteral("background: transparent;"));

    auto *overlayLayout = new QVBoxLayout(overlay);
    overlayLayout->setContentsMargins(42, 34, 42, 34);
    overlayLayout->setSpacing(0);

    auto *topRow = new HudTopLayout;
    overlayLayout->addLayout(topRow);

    RobotOverlay redOverlay;
    auto *redPanel = createRobotOverlay(m_teamNames.value(kRedTeam),
                                        kRedTeam, &redOverlay);
    topRow->addElement(QStringLiteral("red_team_panel"), redPanel);
    m_overlays.insert(kRedTeam, redOverlay);
    registerLayoutElement(QStringLiteral("red_team_panel"), redPanel);

    auto *centerHud = new CenterHudCanvas(overlay);
    auto *centerContent = centerHud->contentWidget();
    auto *centerLayout = new HudCenterContentLayout;
    centerContent->setLayout(centerLayout);

    // Keep the labels inside fixed-height layers. The custom layout owns their
    // geometry, so changing the displayed text cannot move either layer.
    auto *titleLayer = new QWidget(centerContent);
    titleLayer->setObjectName(QStringLiteral("matchTitleLayer"));
    titleLayer->setAttribute(Qt::WA_TranslucentBackground);
    titleLayer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_matchTitle = makeLabel(titleLayer, Qt::AlignCenter);
    m_matchTitle->setText(tr("1v1 对抗赛 · 1v1 Соревнование"));
    QFont titleFont = m_matchTitle->font();
    titleFont.setBold(true);
    titleFont.setPointSize(18);
    m_matchTitle->setFont(titleFont);
    titleLayer->setFixedHeight(QFontMetrics(titleFont).height());
    auto *titleLayerLayout = new QVBoxLayout(titleLayer);
    titleLayerLayout->setContentsMargins(0, 0, 0, 0);
    titleLayerLayout->addWidget(m_matchTitle);
    centerLayout->addElement(QStringLiteral("match_title"), titleLayer);

    auto *timerLayer = new QWidget(centerContent);
    timerLayer->setObjectName(QStringLiteral("matchTimerLayer"));
    timerLayer->setAttribute(Qt::WA_TranslucentBackground);
    timerLayer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_timerLabel = makeLabel(timerLayer, Qt::AlignCenter);
    m_timerLabel->setObjectName(QStringLiteral("timer"));
    QFont timerFont = m_timerLabel->font();
    timerFont.setBold(true);
    timerFont.setPointSize(47);
    m_timerLabel->setFont(timerFont);
    const int timerHeight = QFontMetrics(timerFont).height();
    m_timerLabel->setFixedHeight(timerHeight);
    timerLayer->setFixedHeight(timerHeight);
    auto *timerLayerLayout = new QVBoxLayout(timerLayer);
    timerLayerLayout->setContentsMargins(0, 0, 0, 0);
    timerLayerLayout->addWidget(m_timerLabel, 0, Qt::AlignCenter);
    centerLayout->addElement(QStringLiteral("match_timer"), timerLayer);
    m_redRoundScoreLabel = centerHud->leftScoreLabel();
    m_blueRoundScoreLabel = centerHud->rightScoreLabel();
    topRow->addElement(QStringLiteral("center_hud"), centerHud);
    registerLayoutElement(QStringLiteral("center_hud"), centerHud);
    registerLayoutElement(QStringLiteral("left_score_panel"), centerHud->leftScorePanel());
    registerLayoutElement(QStringLiteral("right_score_panel"), centerHud->rightScorePanel());
    registerLayoutElement(QStringLiteral("match_title"), titleLayer);
    registerLayoutElement(QStringLiteral("match_timer"), timerLayer);
    registerLayoutElement(QStringLiteral("red_round_score"), m_redRoundScoreLabel);
    registerLayoutElement(QStringLiteral("blue_round_score"), m_blueRoundScoreLabel);

    RobotOverlay blueOverlay;
    auto *bluePanel = createRobotOverlay(m_teamNames.value(kBlueTeam),
                                         kBlueTeam, &blueOverlay);
    topRow->addElement(QStringLiteral("blue_team_panel"), bluePanel);
    m_overlays.insert(kBlueTeam, blueOverlay);
    registerLayoutElement(QStringLiteral("blue_team_panel"), bluePanel);
    overlayLayout->addStretch(1);

    auto *bottomRow = new HudBottomLayout;
    overlayLayout->addLayout(bottomRow);
    m_matchState = makeLabel(overlay, Qt::AlignLeft | Qt::AlignVCenter);
    m_matchState->setObjectName(QStringLiteral("matchState"));
    QFont stateFont = m_matchState->font();
    stateFont.setPointSize(20);
    m_matchState->setFont(stateFont);
    bottomRow->addElement(QStringLiteral("match_state"), m_matchState);
    m_sourceLabel = makeLabel(overlay, Qt::AlignRight | Qt::AlignVCenter);
    m_sourceLabel->setObjectName(QStringLiteral("sourceLabel"));
    QFont sourceFont = m_sourceLabel->font();
    sourceFont.setPointSize(20);
    sourceFont.setBold(true);
    m_sourceLabel->setFont(sourceFont);
    bottomRow->addElement(QStringLiteral("source_label"), m_sourceLabel);
    registerLayoutElement(QStringLiteral("match_state"), m_matchState);
    registerLayoutElement(QStringLiteral("source_label"), m_sourceLabel);

    m_layoutManagers.insert(QStringLiteral("red_team_panel"), topRow);
    m_layoutManagers.insert(QStringLiteral("center_hud"), topRow);
    m_layoutManagers.insert(QStringLiteral("blue_team_panel"), topRow);
    m_layoutManagers.insert(QStringLiteral("match_title"), centerLayout);
    m_layoutManagers.insert(QStringLiteral("match_timer"), centerLayout);
    m_layoutManagers.insert(QStringLiteral("match_state"), bottomRow);
    m_layoutManagers.insert(QStringLiteral("source_label"), bottomRow);

    m_tickerBar = new QFrame(overlay);
    m_tickerBar->setObjectName(QStringLiteral("tickerBar"));
    m_tickerBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_tickerBar->setMinimumHeight(0);
    m_tickerBar->setMaximumHeight(0);
    auto *tickerLayout = new QHBoxLayout(m_tickerBar);
    tickerLayout->setContentsMargins(18, 3, 18, 3);
    tickerLayout->setSpacing(14);

    auto *tickerTag = new QLabel(tr("TICKER"), m_tickerBar);
    tickerTag->setObjectName(QStringLiteral("tickerTag"));
    tickerTag->setAlignment(Qt::AlignCenter);
    tickerTag->setMinimumWidth(68);
    tickerLayout->addWidget(tickerTag, 0, Qt::AlignVCenter);

    m_tickerTextView = new TickerMarqueeWidget(m_tickerBar);
    QFont tickerFont = m_tickerTextView->font();
    tickerFont.setBold(true);
    tickerFont.setPointSize(22);
    m_tickerTextView->setFont(tickerFont);
    tickerLayout->addWidget(m_tickerTextView, 1);
    m_tickerText = tr("欢迎来到赛事转播现场 · 比赛即将开始");
    m_tickerTextView->setText(m_tickerText);

    m_tickerOpacity = new QGraphicsOpacityEffect(m_tickerBar);
    m_tickerOpacity->setOpacity(0.0);
    m_tickerBar->setGraphicsEffect(m_tickerOpacity);
    m_tickerHeightAnimation = new QPropertyAnimation(m_tickerBar, "maximumHeight", this);
    m_tickerHeightAnimation->setDuration(180);
    m_tickerHeightAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_tickerHeightAnimation, &QPropertyAnimation::valueChanged, this,
            [this](const QVariant &value) {
                if (!m_tickerBar)
                    return;
                const int height = qMax(0, value.toInt());
                m_tickerBar->setMinimumHeight(height);
            });
    m_tickerOpacityAnimation = new QPropertyAnimation(m_tickerOpacity, "opacity", this);
    m_tickerOpacityAnimation->setDuration(150);
    m_tickerOpacityAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_tickerHeightAnimation, &QPropertyAnimation::finished, this, [this] {
        if (!m_tickerVisible && m_tickerBar) {
            m_tickerBar->hide();
            m_tickerBar->setMinimumHeight(0);
            m_tickerBar->setMaximumHeight(0);
        }
    });
    overlayLayout->addWidget(m_tickerBar);
    m_tickerBar->hide();

    stack->addWidget(overlay, 0, 0);

    m_interactionLayer = new QWidget(central);
    m_interactionLayer->setObjectName(QStringLiteral("interactionLayer"));
    m_interactionLayer->setAttribute(Qt::WA_TranslucentBackground);
    m_interactionLayer->setAutoFillBackground(false);
    m_interactionLayer->hide();
    stack->addWidget(m_interactionLayer, 0, 0);

    m_settlementView = new QLabel(central);
    m_settlementView->setObjectName(QStringLiteral("settlementView"));
    m_settlementView->setAlignment(Qt::AlignCenter);
    m_settlementView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_settlementView->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_settlementView->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    m_settlementView->setGeometry(central->rect());
    m_settlementView->hide();

    setCentralWidget(central);
}

QWidget *BroadcastWindow::createRobotOverlay(const QString &teamTitle,
                                             quint8 team,
                                             RobotOverlay *overlay)
{
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("teamHud"));
    frame->setProperty("team", team == kRedTeam ? QStringLiteral("red")
                                                 : QStringLiteral("blue"));
    auto *layout = new HudTeamLayout(frame);

    overlay->teamId = team;
    overlay->team = makeLabel(frame, team == kRedTeam ? Qt::AlignRight : Qt::AlignLeft);
    overlay->team->setObjectName(team == kRedTeam ? QStringLiteral("redTeam")
                                                  : QStringLiteral("blueTeam"));
    overlay->team->setText(teamTitle);
    QFont teamFont = overlay->team->font();
    teamFont.setBold(true);
    teamFont.setPointSize(30);
    overlay->team->setFont(teamFont);

    auto *teamRow = new QWidget(frame);
    auto *teamRowLayout = new QHBoxLayout(teamRow);
    teamRowLayout->setContentsMargins(0, 0, 0, 0);
    teamRowLayout->setSpacing(10);
    overlay->connection = makeLabel(
        frame,
        team == kRedTeam ? Qt::AlignRight | Qt::AlignVCenter
                         : Qt::AlignLeft | Qt::AlignVCenter);
    overlay->connection->setObjectName(QStringLiteral("connectionStatus"));
    overlay->connection->setMinimumWidth(68);
    QFont connectionFont = overlay->connection->font();
    connectionFont.setBold(true);
    connectionFont.setPointSize(18);
    overlay->connection->setFont(connectionFont);
    if (team == kRedTeam) {
        teamRowLayout->addStretch(1);
        teamRowLayout->addWidget(overlay->connection, 0, Qt::AlignRight | Qt::AlignVCenter);
        teamRowLayout->addWidget(overlay->team, 1, Qt::AlignRight | Qt::AlignVCenter);
    } else {
        teamRowLayout->addWidget(overlay->team, 1, Qt::AlignLeft | Qt::AlignVCenter);
        teamRowLayout->addWidget(overlay->connection, 0, Qt::AlignLeft | Qt::AlignVCenter);
        teamRowLayout->addStretch(1);
    }
    layout->addElement(QStringLiteral("team_row"), teamRow);

    auto *cardRow = new QWidget(frame);
    auto *cardRowLayout = new QHBoxLayout(cardRow);
    cardRowLayout->setContentsMargins(0, 0, 0, 0);
    cardRowLayout->setSpacing(8);
    const auto addCardIndicator = [frame](const QRect &sourceRect,
                                           const QString &labelText,
                                           QLabel **countLabel) {
        auto *indicator = new QWidget(frame);
        auto *indicatorLayout = new QHBoxLayout(indicator);
        indicatorLayout->setContentsMargins(0, 0, 0, 0);
        indicatorLayout->setSpacing(3);

        auto *icon = new QLabel(indicator);
        icon->setFixedSize(24, 30);
        const QPixmap pixmap = assetCrop(QStringLiteral(":/broadcast/statusbar_atlas.png"),
                                         sourceRect);
        icon->setPixmap(pixmap.scaled(icon->size(), Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation));
        icon->setAlignment(Qt::AlignCenter);
        indicatorLayout->addWidget(icon);

        auto *count = new QLabel(QStringLiteral("0"), indicator);
        count->setAlignment(Qt::AlignCenter);
        QFont countFont = count->font();
        countFont.setBold(true);
        countFont.setPointSize(18);
        count->setFont(countFont);
        count->setToolTip(labelText);
        indicatorLayout->addWidget(count);
        if (countLabel)
            *countLabel = count;
        return indicator;
    };
    cardRowLayout->addStretch(1);
    cardRowLayout->addWidget(addCardIndicator(QRect(4, 269, 53, 66), tr("红牌"),
                                              &overlay->redCard));
    cardRowLayout->addWidget(addCardIndicator(QRect(897, 472, 53, 66), tr("黄牌"),
                                              &overlay->yellowCard));
    cardRowLayout->addStretch(1);
    layout->addElement(QStringLiteral("card_row"), cardRow);

    auto *healthBar = new AtlasProgressBar(
        assetCrop(QStringLiteral(":/broadcast/statusbar_atlas.png"),
                  team == kRedTeam ? QRect(2, 851, 373, 56)
                                   : QRect(386, 851, 373, 56)),
        team == kRedTeam,
        team == kRedTeam,
        frame);
    healthBar->setRange(0, kMaxHp);
    healthBar->setFixedHeight(34);
    overlay->healthBar = healthBar;
    layout->addElement(QStringLiteral("health_bar"), healthBar);

    auto *heatBar = new AtlasProgressBar(
        assetCrop(QStringLiteral(":/broadcast/statusbar_atlas.png"),
                  QRect(211, 410, 276, 24)),
        team == kBlueTeam,
        false,
        frame);
    heatBar->setRange(0, kMaxHeat);
    heatBar->setFixedHeight(20);
    overlay->heatBar = heatBar;
    layout->addElement(QStringLiteral("heat_bar"), heatBar);

    overlay->damageNotice = makeLabel(frame, Qt::AlignCenter);
    overlay->damageNotice->setObjectName(QStringLiteral("damageNotice"));
    QFont damageFont = overlay->damageNotice->font();
    damageFont.setBold(true);
    damageFont.setPointSize(25);
    overlay->damageNotice->setFont(damageFont);
    overlay->damageNotice->setFixedSize(160, 36);
    overlay->damageNotice->move(0, 0);
    overlay->damageNotice->setVisible(false);
    overlay->damageNotice->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlay->damageOpacity = new QGraphicsOpacityEffect(overlay->damageNotice);
    overlay->damageOpacity->setOpacity(1.0);
    overlay->damageNotice->setGraphicsEffect(overlay->damageOpacity);
    overlay->damageAnimation = new QPropertyAnimation(overlay->damageOpacity, "opacity",
                                                      overlay->damageNotice);
    overlay->damageAnimation->setDuration(900);
    QLabel *damageNotice = overlay->damageNotice;
    QGraphicsOpacityEffect *damageOpacity = overlay->damageOpacity;
    connect(overlay->damageAnimation, &QPropertyAnimation::finished, this,
            [damageNotice, damageOpacity] {
        damageNotice->setVisible(false);
        damageOpacity->setOpacity(1.0);
    });
    overlay->damageDropAnimation = new QPropertyAnimation(overlay->damageNotice,
                                                           "pos",
                                                           overlay->damageNotice);
    overlay->damageDropAnimation->setDuration(760);
    overlay->damageDropAnimation->setEasingCurve(QEasingCurve::OutCubic);

    auto *atlasHealthBar = static_cast<AtlasProgressBar *>(healthBar);
    overlay->damageBufferAnimation = new QVariantAnimation(frame);
    overlay->damageBufferAnimation->setDuration(680);
    overlay->damageBufferAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(overlay->damageBufferAnimation, &QVariantAnimation::valueChanged, this,
            [atlasHealthBar](const QVariant &value) {
        atlasHealthBar->setDamageBufferValue(value.toInt());
    });
    connect(overlay->damageBufferAnimation, &QVariantAnimation::finished, this,
            [atlasHealthBar] {
        atlasHealthBar->setDamageBufferValue(atlasHealthBar->value());
    });

    auto *statusRow = new QGridLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setHorizontalSpacing(0);
    statusRow->setVerticalSpacing(0);
    for (int column = 0; column < 4; ++column)
        statusRow->setColumnStretch(column, 1);
    statusRow->setRowMinimumHeight(0, 28);
    overlay->health = makeLabel(frame);
    overlay->state = makeLabel(frame, Qt::AlignCenter);
    overlay->shoot = makeLabel(frame, Qt::AlignRight | Qt::AlignVCenter);
    for (QLabel *label : {overlay->health, overlay->state, overlay->shoot}) {
        QFont statusFont = label->font();
        statusFont.setPointSize(21);
        label->setFont(statusFont);
    }
    overlay->heat = makeLabel(frame);
    QFont heatFont = overlay->heat->font();
    heatFont.setPointSize(18);
    overlay->heat->setFont(heatFont);
    overlay->health->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    overlay->heat->setAlignment(Qt::AlignCenter);
    overlay->state->setAlignment(Qt::AlignCenter);
    overlay->shoot->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusRow->addWidget(overlay->health, 0, 0);
    statusRow->addWidget(overlay->heat, 0, 1);
    statusRow->addWidget(overlay->state, 0, 2);
    statusRow->addWidget(overlay->shoot, 0, 3);
    auto *statusWidget = new QWidget(frame);
    statusWidget->setLayout(statusRow);
    layout->addElement(QStringLiteral("status_row"), statusWidget);

    updateOverlay(nullptr, *overlay, RobotManager::keyOf(team, 0));
    return frame;
}

void BroadcastWindow::updateOverlay(const RobotManager::RobotInfo *robot,
                                    RobotOverlay &overlay,
                                    quint64 robotKey)
{
    overlay.team->setText(m_teamNames.value(overlay.teamId, defaultTeamName(overlay.teamId)));
    const int redCardCount = m_redCards.value(overlay.teamId, 0);
    const int yellowCardCount = m_yellowCards.value(overlay.teamId, 0);
    overlay.redCard->setText(QString::number(redCardCount));
    overlay.yellowCard->setText(QString::number(yellowCardCount));
    overlay.redCard->parentWidget()->setVisible(redCardCount > 0);
    overlay.yellowCard->parentWidget()->setVisible(yellowCardCount > 0);

    if (!robot) {
        if (overlay.damageBufferAnimation)
            overlay.damageBufferAnimation->stop();
        if (overlay.damageDropAnimation)
            overlay.damageDropAnimation->stop();
        if (overlay.damageAnimation)
            overlay.damageAnimation->stop();
        overlay.healthBar->setValue(0);
        static_cast<AtlasProgressBar *>(overlay.healthBar)->setDamageBufferValue(0);
        overlay.heatBar->setValue(0);
        overlay.connection->setText(tr("未连接"));
        overlay.health->setText(tr("HP --"));
        overlay.heat->setText(tr("热量 --"));
        overlay.state->setText(tr("等待状态"));
        overlay.shoot->setText(tr("射击 --"));
        overlay.damageNotice->setVisible(false);
        overlay.damageOpacity->setOpacity(1.0);
        setTextColor(overlay.connection, QColor(QStringLiteral("#c7d0d6")), true);
        setTextColor(overlay.health, QColor(QStringLiteral("#c7d0d6")));
        setTextColor(overlay.state, QColor(QStringLiteral("#c7d0d6")));
        setTextColor(overlay.shoot, QColor(QStringLiteral("#c7d0d6")));
        return;
    }

    if (robot->hp >= 0) {
        const int hp = qBound(0, robot->hp, kMaxHp);
        const int previousHp = m_lastHp.value(robotKey, -1);
        auto *atlasHealthBar = static_cast<AtlasProgressBar *>(overlay.healthBar);
        const bool isDamage = previousHp >= 0 && hp < previousHp;
        if (isDamage) {
            triggerDamageEffect(overlay, previousHp - hp, previousHp, hp);
        } else if (!overlay.damageBufferAnimation
                   || overlay.damageBufferAnimation->state() != QAbstractAnimation::Running) {
            atlasHealthBar->setDamageBufferValue(hp);
        }
        m_lastHp.insert(robotKey, hp);
        overlay.healthBar->setValue(hp);
        overlay.health->setText(tr("HP %1").arg(robot->hp));
    } else {
        if (overlay.damageBufferAnimation)
            overlay.damageBufferAnimation->stop();
        overlay.healthBar->setValue(0);
        static_cast<AtlasProgressBar *>(overlay.healthBar)->setDamageBufferValue(0);
        overlay.health->setText(tr("HP --"));
    }

    if (robot->heat >= 0) {
        overlay.heatBar->setValue(qBound(0, robot->heat, kMaxHeat));
        overlay.heat->setText(tr("热量 %1").arg(robot->heat));
    } else {
        overlay.heatBar->setValue(0);
        overlay.heat->setText(tr("热量 --"));
    }

    if (!robot->online) {
        overlay.connection->setText(tr("离线"));
        setTextColor(overlay.connection, QColor(QStringLiteral("#ffb35c")), true);
        overlay.state->setText(tr("状态未知"));
        overlay.shoot->setText(tr("射击 --"));
        setTextColor(overlay.state, QColor(QStringLiteral("#c7d0d6")), true);
        setTextColor(overlay.shoot, QColor(QStringLiteral("#c7d0d6")));
        return;
    }

    overlay.connection->setText(tr("在线"));
    setTextColor(overlay.connection, QColor(QStringLiteral("#72d39a")), true);
    overlay.state->setText(robot->alive ? tr("存活") : tr("阵亡"));
    overlay.shoot->setText(robot->shootEnabled ? tr("射击 开放") : tr("射击 禁止"));
    setTextColor(overlay.health, QColor(QStringLiteral("#f4f7fa")), true);
    setTextColor(overlay.state,
                 robot->alive ? QColor(QStringLiteral("#72d39a"))
                              : QColor(QStringLiteral("#ff6872")),
                 true);
    setTextColor(overlay.shoot,
                 robot->shootEnabled ? QColor(QStringLiteral("#72d39a"))
                                     : QColor(QStringLiteral("#ff6872")),
                 true);
}

void BroadcastWindow::triggerDamageEffect(RobotOverlay &overlay,
                                           int damage,
                                           int previousHp,
                                           int currentHp)
{
    if (!overlay.damageNotice || !overlay.damageAnimation || !overlay.damageDropAnimation
        || !overlay.damageBufferAnimation || !overlay.healthBar || !overlay.heatBar)
        return;

    auto *atlasHealthBar = static_cast<AtlasProgressBar *>(overlay.healthBar);
    const int bufferStart = qMax(previousHp, atlasHealthBar->damageBufferValue());
    atlasHealthBar->setValue(currentHp);
    atlasHealthBar->setDamageBufferValue(bufferStart);
    overlay.damageBufferAnimation->stop();
    overlay.damageBufferAnimation->setStartValue(bufferStart);
    overlay.damageBufferAnimation->setEndValue(currentHp);
    overlay.damageBufferAnimation->start();

    overlay.damageNotice->setText(tr("-%1").arg(damage));
    overlay.damageNotice->setVisible(true);
    overlay.damageNotice->raise();
    overlay.damageOpacity->setOpacity(1.0);
    QWidget *frame = overlay.damageNotice->parentWidget();
    if (!frame)
        return;

    const QRect healthRect = overlay.healthBar->geometry();
    const QRect heatRect = overlay.heatBar->geometry();
    const int x = qMax(0, healthRect.center().x() - overlay.damageNotice->width() / 2);
    const int startY = qMax(0, healthRect.y() + healthRect.height() + 2);
    const int endY = qBound(startY,
                            heatRect.y() + heatRect.height() + 4,
                            qMax(startY, frame->height() - overlay.damageNotice->height()));
    const QPoint endPos(x, endY);
    const QPoint adjustedStartPos(x, startY);
    overlay.damageNotice->move(adjustedStartPos);
    overlay.damageAnimation->stop();
    overlay.damageAnimation->setStartValue(1.0);
    overlay.damageAnimation->setEndValue(0.0);
    overlay.damageAnimation->start();
    overlay.damageDropAnimation->stop();
    overlay.damageDropAnimation->setStartValue(adjustedStartPos);
    overlay.damageDropAnimation->setEndValue(endPos);
    overlay.damageDropAnimation->start();
}

const RobotManager::RobotInfo *BroadcastWindow::robotForTeam(
    quint8 team,
    const QHash<quint64, RobotManager::RobotInfo> &robots) const
{
    const RobotManager::RobotInfo *selected = nullptr;
    for (auto it = robots.constBegin(); it != robots.constEnd(); ++it) {
        if (it.value().team != team)
            continue;
        if (!selected || (it.value().online && !selected->online)
            || (it.value().online == selected->online && it.value().robotId < selected->robotId)) {
            selected = &it.value();
        }
    }
    return selected;
}

void BroadcastWindow::updateRobots(const QVector<RobotManager::RobotInfo> &robots)
{
    QHash<quint64, RobotManager::RobotInfo> current;
    for (const auto &robot : robots)
        current.insert(RobotManager::keyOf(robot.team, robot.robotId), robot);

    m_onlineCount = 0;
    for (const quint8 team : {kRedTeam, kBlueTeam}) {
        const RobotManager::RobotInfo *selected = robotForTeam(team, current);
        if (selected && selected->online)
            ++m_onlineCount;
    }

    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
        const quint8 team = static_cast<quint8>(it.key());
        const RobotManager::RobotInfo *selected = robotForTeam(team, current);
        const quint64 key = selected ? RobotManager::keyOf(team, selected->robotId)
                                     : RobotManager::keyOf(team, 0);
        updateOverlay(selected, it.value(), key);
    }

    updateMatchState();
}

void BroadcastWindow::updateMatchState()
{
    QString phase;
    if (m_roundEnded || m_remainingSeconds == 0)
        phase = tr("比赛结束");
    else if (m_matchRunning)
        phase = tr("比赛进行中");
    else
        phase = tr("准备开始");
    m_matchState->setText(tr("%1 · 机器人在线 %2 / 2").arg(phase).arg(m_onlineCount));
}

void BroadcastWindow::updateTimerDisplay()
{
    const int minutes = m_remainingSeconds / 60;
    const int seconds = m_remainingSeconds % 60;
    m_timerLabel->setText(QStringLiteral("%1:%2")
                              .arg(minutes, 2, 10, QLatin1Char('0'))
                              .arg(seconds, 2, 10, QLatin1Char('0')));
    updateMatchState();
    emit presentationStateChanged();
}

void BroadcastWindow::startMatch()
{
    if (m_roundEnded || m_remainingSeconds == 0)
        resetMatch();
    if (m_matchRunning)
        return;

    stopSettlement();
    m_roundEnded = false;
    m_settlementType.clear();
    m_matchRunning = true;
    m_matchTimer->start();
    updateTimerDisplay();
    emit matchStateChanged(true);
}

void BroadcastWindow::pauseMatch()
{
    if (!m_matchRunning)
        return;
    m_matchRunning = false;
    m_matchTimer->stop();
    updateTimerDisplay();
    emit matchStateChanged(false);
}

void BroadcastWindow::resetMatch()
{
    stopSettlement();
    m_matchTimer->stop();
    m_matchRunning = false;
    m_roundEnded = false;
    m_settlementType.clear();
    m_remainingSeconds = kMatchDurationSeconds;
    m_redScore = 0;
    m_blueScore = 0;
    clearCards();
    m_lastHp.clear();
    setScores(0, 0);
    updateTimerDisplay();
    emit matchStateChanged(false);
}

void BroadcastWindow::setMatchState(int remainingSeconds, bool running)
{
    stopSettlement();
    m_remainingSeconds = qBound(0, remainingSeconds, kMatchDurationSeconds);
    m_matchRunning = running && m_remainingSeconds > 0;
    m_roundEnded = false;
    m_settlementType.clear();
    if (m_matchRunning)
        m_matchTimer->start();
    else
        m_matchTimer->stop();

    updateTimerDisplay();
    emit matchStateChanged(m_matchRunning);
}

void BroadcastWindow::terminateMatch()
{
    if (m_roundEnded)
        return;
    finishMatch(QStringLiteral("termination"));
}

void BroadcastWindow::finishMatch(const QString &settlementType)
{
    m_matchTimer->stop();
    m_matchRunning = false;
    m_roundEnded = true;
    m_settlementType = settlementType;
    updateTimerDisplay();
    emit matchStateChanged(false);
    playSettlement(settlementType);
    emit roundFinished(settlementType);
    emit presentationStateChanged();
}

QString BroadcastWindow::settlementTypeForScores() const
{
    if (m_redScore > m_blueScore)
        return QStringLiteral("redwin");
    if (m_blueScore > m_redScore)
        return QStringLiteral("bluewin");
    return QStringLiteral("draw");
}

QString BroadcastWindow::settlementAssetDirectory() const
{
    const QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("Assets/gamefinishvideo")),
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("../Assets/gamefinishvideo")),
        QDir::cleanPath(QDir::current().filePath(QStringLiteral("Assets/gamefinishvideo"))),
        QDir(QFileInfo(QStringLiteral(__FILE__)).absolutePath()).filePath(
            QStringLiteral("Assets/gamefinishvideo"))
    };

    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("draw_rgb.mp4"))))
            return QDir(candidate).absolutePath();
    }
    return QString();
}

QString BroadcastWindow::settlementAssetPath(const QString &type, const QString &suffix) const
{
    static const QStringList validTypes = {
        QStringLiteral("redwin"), QStringLiteral("bluewin"),
        QStringLiteral("defeated"), QStringLiteral("draw"),
        QStringLiteral("termination")
    };
    if (!validTypes.contains(type) || (suffix != QStringLiteral("rgb")
                                       && suffix != QStringLiteral("alpha")))
        return QString();

    const QString directory = settlementAssetDirectory();
    if (directory.isEmpty())
        return QString();
    const QString path = QDir(directory).filePath(
        QStringLiteral("%1_%2.mp4").arg(type, suffix));
    return QFileInfo::exists(path) ? path : QString();
}

QString BroadcastWindow::ffmpegExecutable() const
{
    const QDir applicationDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        applicationDir.filePath(QStringLiteral("ffmpeg.exe")),
        applicationDir.filePath(QStringLiteral("tools/ffmpeg/ffmpeg.exe")),
        applicationDir.filePath(QStringLiteral("tools/ffmpeg.exe")),
        QDir::cleanPath(applicationDir.filePath(QStringLiteral("../tools/ffmpeg/ffmpeg.exe")))
    };

    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return QFileInfo(candidate).absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

void BroadcastWindow::playSettlement(const QString &type)
{
    const QString normalizedType = type.trimmed().toLower();
    if (normalizedType.isEmpty())
        return;
    if (m_settlementProcess && m_settlementType == normalizedType)
        return;

    stopSettlement();
    m_roundEnded = true;
    m_settlementType = normalizedType;
    const QString rgbPath = settlementAssetPath(normalizedType, QStringLiteral("rgb"));
    const QString alphaPath = settlementAssetPath(normalizedType, QStringLiteral("alpha"));
    const QString executable = ffmpegExecutable();
    if (rgbPath.isEmpty() || alphaPath.isEmpty() || executable.isEmpty()) {
        if (m_settlementView) {
            m_settlementView->setPixmap(QPixmap());
            m_settlementView->setText(tr("结算动画资源或 FFmpeg 不可用"));
            m_settlementView->show();
            m_settlementView->raise();
        }
        return;
    }

    m_settlementBuffer.clear();
    m_settlementFrames.clear();
    m_lastSettlementFrame = QImage();
    m_settlementFinished = false;
    if (m_settlementView) {
        m_settlementView->setPixmap(QPixmap());
        m_settlementView->setText(QString());
        m_settlementView->show();
        m_settlementView->raise();
    }
    m_settlementFrameTimer->start();

    m_settlementProcess = new QProcess(this);
    QProcess *process = m_settlementProcess;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardOutput,
            this, &BroadcastWindow::consumeSettlementOutput);
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int, QProcess::ExitStatus) {
                if (m_settlementProcess != process)
                    return;
                consumeSettlementOutput();
                m_settlementFinished = true;
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError) {
                if (m_settlementProcess != process)
                    return;
                consumeSettlementOutput();
                m_settlementFinished = true;
                if (m_lastSettlementFrame.isNull() && m_settlementFrames.isEmpty()
                    && m_settlementView) {
                    m_settlementView->setText(tr("结算动画播放失败"));
                    m_settlementView->show();
                    m_settlementView->raise();
                }
            });

    process->start(executable, {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-re"),
        QStringLiteral("-i"), rgbPath,
        QStringLiteral("-re"),
        QStringLiteral("-i"), alphaPath,
        QStringLiteral("-filter_complex"),
        QStringLiteral("[1:v]format=gray[mask];[0:v][mask]alphamerge,"
                       "scale=960:540:flags=lanczos,format=bgra[v]"),
        QStringLiteral("-map"), QStringLiteral("[v]"),
        QStringLiteral("-an"),
        QStringLiteral("-f"), QStringLiteral("rawvideo"),
        QStringLiteral("-pix_fmt"), QStringLiteral("bgra"),
        QStringLiteral("pipe:1")
    });
}

void BroadcastWindow::stopSettlement()
{
    QProcess *process = m_settlementProcess;
    m_settlementProcess = nullptr;
    m_settlementBuffer.clear();
    m_settlementFrames.clear();
    m_lastSettlementFrame = QImage();
    m_settlementFinished = false;
    if (m_settlementFrameTimer)
        m_settlementFrameTimer->stop();
    if (m_settlementView)
        m_settlementView->hide();
    if (!process)
        return;

    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(1000);
    }
    if (process->state() == QProcess::NotRunning)
        delete process;
    else
        process->deleteLater();
}

void BroadcastWindow::playSettlementPreview(const QString &type)
{
    const bool previousRoundEnded = m_roundEnded;
    const QString previousSettlementType = m_settlementType;
    playSettlement(type);

    // A preview must not finish or rename the active match. The animation
    // itself stays owned by the normal settlement pipeline and can be stopped
    // by starting/resetting the match as usual.
    m_roundEnded = previousRoundEnded;
    m_settlementType = previousSettlementType;
}

void BroadcastWindow::clearSettlement()
{
    stopSettlement();
    m_roundEnded = false;
    m_settlementType.clear();
}

void BroadcastWindow::consumeSettlementOutput()
{
    if (!m_settlementProcess)
        return;

    const int freeFrames = kSettlementFrameQueueLimit - m_settlementFrames.size();
    if (freeFrames <= 0)
        return;

    const qint64 bytesNeeded = qint64(freeFrames) * kSettlementFrameBytes
                               - m_settlementBuffer.size();
    if (bytesNeeded > 0)
        m_settlementBuffer.append(m_settlementProcess->read(bytesNeeded));

    while (m_settlementFrames.size() < kSettlementFrameQueueLimit
           && m_settlementBuffer.size() >= kSettlementFrameBytes) {
        const QByteArray frameBytes = m_settlementBuffer.left(kSettlementFrameBytes);
        m_settlementBuffer.remove(0, kSettlementFrameBytes);
        const QImage frame(reinterpret_cast<const uchar *>(frameBytes.constData()),
                           kSettlementFrameWidth,
                           kSettlementFrameHeight,
                           kSettlementFrameWidth * 4,
                           QImage::Format_ARGB32);
        m_settlementFrames.enqueue(frame.copy());
    }
}

void BroadcastWindow::renderSettlementFrame(const QImage &frame)
{
    if (!m_settlementView || frame.isNull())
        return;

    m_lastSettlementFrame = frame;
    const QSize targetSize = m_settlementView->size();
    if (targetSize.isEmpty())
        return;
    const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
        targetSize, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
    m_settlementView->setText(QString());
    m_settlementView->setPixmap(pixmap);
}

void BroadcastWindow::setProgramFrame(const QImage &frame)
{
    setSourceFrame(QStringLiteral("field"), frame);
}

void BroadcastWindow::setSourceFrame(const QString &sourceId, const QImage &frame)
{
    const QString id = sourceId.isEmpty() ? QStringLiteral("field") : sourceId;
    if (frame.isNull())
        m_sourceFrames.remove(id);
    else
        m_sourceFrames.insert(id, frame);
    // Non-program sources remain cached for an instant switch, but must not
    // trigger a full-window scale and repaint on every incoming frame.
    if (id == m_activeSource)
        scheduleProgramRender();

    // Throttled notification for the control-panel preview strip. Emitting on
    // every frame would make the preview QLabel pay a scale + pixmap upload
    // at the incoming frame rate, which is wasteful when the strip is only a
    // few hundred pixels wide.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = m_sourceFrameSignalTimes.value(id, 0);
    if (now - last >= 100) {
        m_sourceFrameSignalTimes.insert(id, now);
        emit sourceFrameUpdated(id);
    }
}

QImage BroadcastWindow::sourceFrame(const QString &sourceId) const
{
    const QString id = sourceId.isEmpty() ? QStringLiteral("field") : sourceId;
    return m_sourceFrames.value(id);
}

QStringList BroadcastWindow::knownSourceIds() const
{
    return m_sourceFrames.keys();
}

void BroadcastWindow::setActiveSource(const QString &sourceId, const QString &sourceTitle)
{
    m_activeSource = sourceId.isEmpty() ? QStringLiteral("field") : sourceId;
    m_activeSourceTitle = sourceTitle.isEmpty()
                              ? (m_activeSource == QStringLiteral("field") ? tr("全场视角")
                                                                             : m_activeSource)
                              : sourceTitle;
    if (m_sourceLabel)
        m_sourceLabel->setText(tr("LIVE · %1").arg(m_activeSourceTitle));
    scheduleProgramRender();
}

void BroadcastWindow::setTeamNames(const QString &redName, const QString &blueName)
{
    if (!redName.trimmed().isEmpty())
        m_teamNames.insert(kRedTeam, redName.trimmed());
    if (!blueName.trimmed().isEmpty())
        m_teamNames.insert(kBlueTeam, blueName.trimmed());

    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it)
        it.value().team->setText(m_teamNames.value(static_cast<quint8>(it.key())));

    emit presentationStateChanged();
}

void BroadcastWindow::setScores(int redScore, int blueScore)
{
    m_redScore = qMax(0, redScore);
    m_blueScore = qMax(0, blueScore);
    if (m_redRoundScoreLabel)
        m_redRoundScoreLabel->setText(QString::number(m_redScore));
    if (m_blueRoundScoreLabel)
        m_blueRoundScoreLabel->setText(QString::number(m_blueScore));
    if (m_scoreLabel)
        m_scoreLabel->setText(tr("总比分 %1  :  %2").arg(m_redScore).arg(m_blueScore));

    emit presentationStateChanged();
}

void BroadcastWindow::setTickerText(const QString &text)
{
    const QString normalized = text.trimmed();
    if (normalized.isEmpty())
        return;

    m_tickerText = normalized;
    if (m_tickerTextView)
        m_tickerTextView->setText(m_tickerText);
    emit presentationStateChanged();
}

void BroadcastWindow::setTickerVisible(bool visible, bool animated)
{
    m_tickerVisible = visible;
    if (!m_tickerBar || !m_tickerHeightAnimation || !m_tickerOpacityAnimation)
        return;

    if (!animated) {
        m_tickerHeightAnimation->stop();
        m_tickerOpacityAnimation->stop();
        const int targetHeight = visible ? 64 : 0;
        m_tickerBar->setMinimumHeight(targetHeight);
        m_tickerBar->setMaximumHeight(targetHeight);
        m_tickerOpacity->setOpacity(visible ? 1.0 : 0.0);
        m_tickerBar->setVisible(visible);
        return;
    }

    animateTicker();
    emit presentationStateChanged();
}

void BroadcastWindow::animateTicker()
{
    if (!m_tickerBar || !m_tickerHeightAnimation || !m_tickerOpacityAnimation
        || !m_tickerOpacity) {
        return;
    }

    const int targetHeight = m_tickerVisible ? 64 : 0;
    if (m_tickerVisible && !m_tickerBar->isVisible()) {
        m_tickerBar->setMinimumHeight(0);
        m_tickerBar->setMaximumHeight(0);
        m_tickerOpacity->setOpacity(0.0);
        m_tickerBar->show();
    }

    m_tickerHeightAnimation->stop();
    m_tickerOpacityAnimation->stop();
    const int currentHeight = qMax(0, m_tickerBar->maximumHeight());
    const qreal currentOpacity = m_tickerOpacity->opacity();
    m_tickerHeightAnimation->setStartValue(currentHeight);
    m_tickerHeightAnimation->setEndValue(targetHeight);
    m_tickerOpacityAnimation->setStartValue(currentOpacity);
    m_tickerOpacityAnimation->setEndValue(m_tickerVisible ? 1.0 : 0.0);
    m_tickerHeightAnimation->start();
    m_tickerOpacityAnimation->start();
}

QStringList BroadcastWindow::layoutElementIds() const
{
    return {
        QStringLiteral("red_team_panel"),
        QStringLiteral("center_hud"),
        QStringLiteral("blue_team_panel"),
        QStringLiteral("left_score_panel"),
        QStringLiteral("right_score_panel"),
        QStringLiteral("match_title"),
        QStringLiteral("match_timer"),
        QStringLiteral("red_round_score"),
        QStringLiteral("blue_round_score"),
        QStringLiteral("match_state"),
        QStringLiteral("source_label")
    };
}

QPoint BroadcastWindow::layoutElementPosition(const QString &elementId) const
{
    return m_layoutOffsets.value(elementId, QPoint());
}

QSize BroadcastWindow::layoutElementSize(const QString &elementId) const
{
    if (m_layoutSizes.contains(elementId))
        return m_layoutSizes.value(elementId);

    QWidget *widget = m_layoutElements.value(elementId);
    if (!widget)
        return QSize();

    const QSize currentSize = widget->size();
    return currentSize.isValid() && currentSize.width() > 0 && currentSize.height() > 0
               ? currentSize
               : widget->sizeHint();
}

void BroadcastWindow::setLayoutElementPosition(const QString &elementId,
                                                const QPoint &position)
{
    if (!m_layoutElements.contains(elementId))
        return;

    m_layoutOffsets.insert(elementId, position);
    if (isVisible())
        applyLayoutPositions();
}

void BroadcastWindow::setLayoutElementSize(const QString &elementId, const QSize &size)
{
    if (!m_layoutElements.contains(elementId) || !size.isValid()
        || size.width() <= 0 || size.height() <= 0) {
        return;
    }

    m_layoutSizes.insert(elementId, size);
    if (isVisible())
        applyLayoutPositions();
}

void BroadcastWindow::resetLayoutPositions()
{
    for (const QString &elementId : layoutElementIds()) {
        const QPoint defaultOffset = defaultLayoutOffset(elementId);
        m_layoutOffsets.insert(elementId, defaultOffset);
        m_layoutSizes.remove(elementId);

        QWidget *widget = m_layoutElements.value(elementId);
        if (!widget)
            continue;
        if (m_layoutMinimumSizes.contains(elementId))
            widget->setMinimumSize(m_layoutMinimumSizes.value(elementId));
        if (m_layoutMaximumSizes.contains(elementId))
            widget->setMaximumSize(m_layoutMaximumSizes.value(elementId));
        widget->updateGeometry();
    }
    if (isVisible())
        applyLayoutPositions();
}

bool BroadcastWindow::saveLayout(const QString &filePath) const
{
    QSettings settings(filePath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("broadcast_layout"));
    settings.setValue(QStringLiteral("version"), 2);
    for (const QString &elementId : layoutElementIds()) {
        const QPoint position = m_layoutOffsets.value(elementId, QPoint());
        settings.setValue(elementId + QStringLiteral("/x"), position.x());
        settings.setValue(elementId + QStringLiteral("/y"), position.y());
        const QString widthKey = elementId + QStringLiteral("/width");
        const QString heightKey = elementId + QStringLiteral("/height");
        if (m_layoutSizes.contains(elementId)) {
            const QSize size = m_layoutSizes.value(elementId);
            settings.setValue(widthKey, size.width());
            settings.setValue(heightKey, size.height());
        } else {
            settings.remove(widthKey);
            settings.remove(heightKey);
        }
    }
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool BroadcastWindow::loadLayout(const QString &filePath)
{
    QSettings settings(filePath, QSettings::IniFormat);
    if (!settings.contains(QStringLiteral("broadcast_layout/center_hud/x")))
        return false;

    const int layoutVersion = settings.value(QStringLiteral("broadcast_layout/version"), 1).toInt();
    m_layoutSizes.clear();
    settings.beginGroup(QStringLiteral("broadcast_layout"));
    for (const QString &elementId : layoutElementIds()) {
        const QString xKey = elementId + QStringLiteral("/x");
        const QString yKey = elementId + QStringLiteral("/y");
        if (settings.contains(xKey) && settings.contains(yKey)) {
            QPoint position(settings.value(xKey).toInt(), settings.value(yKey).toInt());
            if (layoutVersion < 2)
                position -= legacyLayoutOffset(elementId);
            m_layoutOffsets.insert(elementId, position);
        }

        const QString widthKey = elementId + QStringLiteral("/width");
        const QString heightKey = elementId + QStringLiteral("/height");
        if (settings.contains(widthKey) && settings.contains(heightKey)) {
            const QSize size(settings.value(widthKey).toInt(),
                             settings.value(heightKey).toInt());
            if (size.isValid() && size.width() > 0 && size.height() > 0)
                m_layoutSizes.insert(elementId, size);
        }
    }
    settings.endGroup();
    if (isVisible())
        applyLayoutPositions();
    return settings.status() == QSettings::NoError;
}

void BroadcastWindow::awardCard(quint8 team, bool redCard)
{
    if (team != kRedTeam && team != kBlueTeam)
        return;

    auto &cards = redCard ? m_redCards : m_yellowCards;
    cards[team] = qMin(9, cards.value(team, 0) + 1);
    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
        if (static_cast<quint8>(it.key()) != team)
            continue;
        it.value().redCard->setText(QString::number(m_redCards.value(team, 0)));
        it.value().yellowCard->setText(QString::number(m_yellowCards.value(team, 0)));
        it.value().redCard->parentWidget()->setVisible(m_redCards.value(team, 0) > 0);
        it.value().yellowCard->parentWidget()->setVisible(m_yellowCards.value(team, 0) > 0);
    }
}

void BroadcastWindow::clearCards()
{
    m_redCards.clear();
    m_yellowCards.clear();
    for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
        it.value().redCard->setText(QStringLiteral("0"));
        it.value().yellowCard->setText(QStringLiteral("0"));
        it.value().redCard->parentWidget()->setVisible(false);
        it.value().yellowCard->parentWidget()->setVisible(false);
    }
}

void BroadcastWindow::clearProgramFrame()
{
    m_sourceFrames.remove(QStringLiteral("field"));
    scheduleProgramRender();
}

void BroadcastWindow::scheduleProgramRender()
{
    if (!m_programRenderTimer)
        return;
    if (!isVisible())
        return;
    if (!m_programRenderTimer->isActive())
        m_programRenderTimer->start();
}

void BroadcastWindow::renderProgramFrame()
{
    if (!m_programView)
        return;

    const QImage frame = m_sourceFrames.value(m_activeSource);
    if (frame.isNull()) {
        m_programView->setPixmap(QPixmap());
        m_programView->setText(tr("等待 %1\n视频流未接入").arg(m_activeSourceTitle));
        return;
    }

    const QSize targetSize = m_programView->size();
    if (targetSize.isEmpty())
        return;
    const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
        targetSize, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
    m_programView->setText(QString());
    m_programView->setPixmap(pixmap);
}

void BroadcastWindow::registerLayoutElement(const QString &elementId, QWidget *widget)
{
    if (elementId.isEmpty() || !widget)
        return;

    m_layoutElements.insert(elementId, widget);
    const QPoint defaultOffset = defaultLayoutOffset(elementId);
    m_layoutOffsets.insert(elementId, defaultOffset);
    m_layoutMinimumSizes.insert(elementId, widget->minimumSize());
    m_layoutMaximumSizes.insert(elementId, widget->maximumSize());
}

void BroadcastWindow::applyLayoutPositions()
{
    // The main HUD entries are owned by custom layouts.  Feed their saved
    // values into those layouts instead of moving layout-managed children.
    // This keeps a text change from changing the coordinate system underneath
    // a manually edited element.
    auto *centerHud = dynamic_cast<CenterHudCanvas *>(
        m_layoutElements.value(QStringLiteral("center_hud")));
    const auto isCenterElement = [](const QString &elementId) {
        return elementId == QStringLiteral("left_score_panel")
               || elementId == QStringLiteral("right_score_panel")
               || elementId == QStringLiteral("red_round_score")
               || elementId == QStringLiteral("blue_round_score");
    };
    if (centerHud) {
        for (const QString &elementId : {QStringLiteral("left_score_panel"),
                                          QStringLiteral("right_score_panel"),
                                          QStringLiteral("red_round_score"),
                                          QStringLiteral("blue_round_score")}) {
            centerHud->setElementOffset(elementId,
                                         m_layoutOffsets.value(elementId, QPoint()));
            centerHud->setElementSize(elementId,
                                       m_layoutSizes.value(elementId, QSize()));
        }
    }

    for (const QString &elementId : layoutElementIds()) {
        QWidget *widget = m_layoutElements.value(elementId);
        if (!widget)
            continue;

        if (centerHud && isCenterElement(elementId)) {
            if (m_layoutSizes.contains(elementId)) {
                const QSize requestedSize = m_layoutSizes.value(elementId);
                if (widget->minimumSize() != requestedSize)
                    widget->setMinimumSize(requestedSize);
                if (widget->maximumSize() != requestedSize)
                    widget->setMaximumSize(requestedSize);
            } else {
                if (m_layoutMinimumSizes.contains(elementId))
                    widget->setMinimumSize(m_layoutMinimumSizes.value(elementId));
                if (m_layoutMaximumSizes.contains(elementId))
                    widget->setMaximumSize(m_layoutMaximumSizes.value(elementId));
            }
            continue;
        }

        if (QLayout *manager = m_layoutManagers.value(elementId)) {
            auto *hudLayout = static_cast<HudElementLayout *>(manager);
            hudLayout->setElementOffset(elementId,
                                        m_layoutOffsets.value(elementId, QPoint()));
            hudLayout->setElementSize(elementId,
                                       m_layoutSizes.value(elementId, QSize()));

            if (m_layoutSizes.contains(elementId)) {
                const QSize requestedSize = m_layoutSizes.value(elementId);
                if (widget->minimumSize() != requestedSize)
                    widget->setMinimumSize(requestedSize);
                if (widget->maximumSize() != requestedSize)
                    widget->setMaximumSize(requestedSize);
            } else {
                if (m_layoutMinimumSizes.contains(elementId))
                    widget->setMinimumSize(m_layoutMinimumSizes.value(elementId));
                if (m_layoutMaximumSizes.contains(elementId))
                    widget->setMaximumSize(m_layoutMaximumSizes.value(elementId));
            }
            continue;
        }

        // Every registered element is assigned to either a stable layout or
        // CenterHudCanvas above. Keeping this branch explicit prevents a new
        // QWidget::move() fallback from reintroducing the original bug.
    }

    QHash<QLayout *, bool> activatedLayouts;
    for (QLayout *manager : m_layoutManagers) {
        if (manager && !activatedLayouts.contains(manager)) {
            manager->activate();
            activatedLayouts.insert(manager, true);

            // A configured height changes the size hint of the nested HUD
            // layout. Re-activate its owning layout so the surrounding
            // overlay also receives the new row height immediately.
            if (QWidget *parent = manager->parentWidget()) {
                if (QLayout *parentLayout = parent->layout(); parentLayout != manager)
                    parentLayout->activate();
            }
        }
    }
}

void BroadcastWindow::showOnScreen(QScreen *screen)
{
    QScreen *target = screen ? screen : QGuiApplication::primaryScreen();
    if (!target)
        return;

    // Reuse the same native window when toggling or moving the output. Clearing
    // the fullscreen state while hidden avoids stale Windows fullscreen state.
    if (isVisible())
        hide();
    setWindowState(windowState() & ~Qt::WindowFullScreen);
    if (windowHandle())
        windowHandle()->setScreen(target);
    setGeometry(target->geometry());
    showFullScreen();
#ifdef Q_OS_WIN
    coverTaskbarOnScreen(this, target->geometry());
#endif
    raise();
    activateWindow();
}

void BroadcastWindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
    hide();
}

void BroadcastWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (m_settlementView && centralWidget())
        m_settlementView->setGeometry(centralWidget()->rect());
    applyLayoutPositions();
    scheduleProgramRender();
    emit visibilityChanged(true);
}

void BroadcastWindow::hideEvent(QHideEvent *event)
{
#ifdef Q_OS_WIN
    releaseTaskbarCover(this);
#endif
    QMainWindow::hideEvent(event);
    emit visibilityChanged(false);
}

void BroadcastWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (m_settlementView && centralWidget())
        m_settlementView->setGeometry(centralWidget()->rect());
    if (isVisible())
        applyLayoutPositions();
    scheduleProgramRender();
    if (!m_lastSettlementFrame.isNull())
        renderSettlementFrame(m_lastSettlementFrame);
}
