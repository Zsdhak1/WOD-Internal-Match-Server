#include "broadcastwindow.h"

#include <QCloseEvent>
#include <QColor>
#include <QEasingCurve>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSizePolicy>
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

QPoint defaultLayoutOffset(const QString &elementId)
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

class AtlasPixmapLayer final : public QWidget
{
public:
    AtlasPixmapLayer(const QPixmap &pixmap, QWidget *parent = nullptr)
        : QWidget(parent), m_pixmap(pixmap)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAutoFillBackground(false);
    }

    QSize sizeHint() const override { return m_pixmap.size(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(rect(), m_pixmap);
    }

private:
    QPixmap m_pixmap;
};

class AtlasCenterHud final : public QWidget
{
public:
    explicit AtlasCenterHud(QWidget *parent = nullptr)
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

        m_panelLayer = new AtlasPixmapLayer(
            m_n15.scaled(QSize(600, 141), Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
            this);
        m_leftBadgeLayer = new AtlasPixmapLayer(
            m_n16Left.scaled(QSize(123, 93), Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
            this);
        m_rightBadgeLayer = new AtlasPixmapLayer(
            m_n16Right.scaled(QSize(123, 92), Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
            this);

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

        auto *leftScoreLayout = new QVBoxLayout(m_leftBadgeLayer);
        leftScoreLayout->setContentsMargins(0, 0, 0, 0);
        leftScoreLayout->addWidget(m_leftScore, 1);

        auto *rightScoreLayout = new QVBoxLayout(m_rightBadgeLayer);
        rightScoreLayout->setContentsMargins(0, 0, 0, 0);
        rightScoreLayout->addWidget(m_rightScore, 1);
    }

    QSize sizeHint() const override { return QSize(kCenterHudWidth, kCenterHudHeight); }

    QWidget *contentWidget() const { return m_content; }
    QLabel *leftScoreLabel() const { return m_leftScore; }
    QLabel *rightScoreLabel() const { return m_rightScore; }
    QWidget *leftScorePanel() const { return m_leftBadgeLayer; }
    QWidget *rightScorePanel() const { return m_rightBadgeLayer; }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        const qreal scaleX = qMax<qreal>(0.01, width() / qreal(kCenterHudWidth));
        const qreal scaleY = qMax<qreal>(0.01, height() / qreal(kCenterHudHeight));
        const int panelWidth = qMax(1, qRound(600 * scaleX));
        const int panelHeight = qMax(1, qRound(141 * scaleY));
        const int badgeWidth = qMax(1, qRound(123 * scaleX));
        const int leftBadgeHeight = qMax(1, qRound(93 * scaleY));
        const int rightBadgeHeight = qMax(1, qRound(92 * scaleY));
        const int overlap = qMax(0, qRound(30 * scaleX));
        const int compositionWidth = badgeWidth + panelWidth + badgeWidth - overlap * 2;

        const int compositionLeft = qMax(0, (width() - compositionWidth) / 2);
        const int panelLeft = compositionLeft + badgeWidth - overlap;
        const int panelTop = qMax(0, (height() - panelHeight) / 2);
        const int leftBadgeTop = qMax(0, (height() - leftBadgeHeight) / 2);
        const int rightBadgeTop = qMax(0, (height() - rightBadgeHeight) / 2);

        m_panelRect = QRect(panelLeft, panelTop, panelWidth, panelHeight);
        m_leftBadgeRect = QRect(compositionLeft, leftBadgeTop, badgeWidth, leftBadgeHeight);
        m_rightBadgeRect = QRect(panelLeft + panelWidth - overlap,
                                 rightBadgeTop,
                                 badgeWidth,
                                 rightBadgeHeight);

        m_panelLayer->setGeometry(m_panelRect);
        m_leftBadgeLayer->setGeometry(m_leftBadgeRect);
        m_rightBadgeLayer->setGeometry(m_rightBadgeRect);
        const int contentSide = qRound(66 * scaleX);
        const int contentTop = qRound(42 * scaleY);
        m_content->setGeometry(m_panelRect.adjusted(contentSide,
                                                     -contentTop,
                                                     -contentSide,
                                                     contentTop));
        QWidget::resizeEvent(event);
    }

private:
    QPixmap m_n15;
    QPixmap m_n16Left;
    QPixmap m_n16Right;
    QWidget *m_panelLayer = nullptr;
    QWidget *m_leftBadgeLayer = nullptr;
    QWidget *m_rightBadgeLayer = nullptr;
    QWidget *m_content = nullptr;
    QLabel *m_leftScore = nullptr;
    QLabel *m_rightScore = nullptr;
    QRect m_panelRect;
    QRect m_leftBadgeRect;
    QRect m_rightBadgeRect;
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

        updateTimerDisplay();
        if (m_remainingSeconds == 0) {
            m_matchTimer->stop();
            m_matchRunning = false;
            emit matchStateChanged(false);
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

    auto *topRow = new QHBoxLayout;
    topRow->setSpacing(18);

    RobotOverlay redOverlay;
    auto *redPanel = createRobotOverlay(m_teamNames.value(kRedTeam),
                                        kRedTeam, &redOverlay);
    topRow->addWidget(redPanel, 1, Qt::AlignTop);
    m_overlays.insert(kRedTeam, redOverlay);
    registerLayoutElement(QStringLiteral("red_team_panel"), redPanel);

    auto *centerHud = new AtlasCenterHud(overlay);
    auto *centerContent = centerHud->contentWidget();
    auto *centerLayout = new QVBoxLayout(centerContent);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(2);

    // Keep the labels inside fixed-height layers.  The layers remain owned by
    // the center layout, while the layout editor moves the layers themselves.
    // This prevents QLabel::setText() from reflowing a manually positioned
    // timer when the displayed digits change.
    auto *titleLayer = new QWidget(centerContent);
    titleLayer->setObjectName(QStringLiteral("matchTitleLayer"));
    titleLayer->setAttribute(Qt::WA_TranslucentBackground);
    titleLayer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_matchTitle = makeLabel(titleLayer, Qt::AlignCenter);
    m_matchTitle->setText(tr("1v1 步兵对抗赛"));
    QFont titleFont = m_matchTitle->font();
    titleFont.setBold(true);
    titleFont.setPointSize(18);
    m_matchTitle->setFont(titleFont);
    titleLayer->setFixedHeight(QFontMetrics(titleFont).height());
    auto *titleLayerLayout = new QVBoxLayout(titleLayer);
    titleLayerLayout->setContentsMargins(0, 0, 0, 0);
    titleLayerLayout->addWidget(m_matchTitle);
    centerLayout->addWidget(titleLayer);

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
    timerLayer->setFixedHeight(QFontMetrics(timerFont).height());
    auto *timerLayerLayout = new QVBoxLayout(timerLayer);
    timerLayerLayout->setContentsMargins(0, 0, 0, 0);
    timerLayerLayout->addWidget(m_timerLabel);
    centerLayout->addWidget(timerLayer);
    m_redRoundScoreLabel = centerHud->leftScoreLabel();
    m_blueRoundScoreLabel = centerHud->rightScoreLabel();
    topRow->addWidget(centerHud, 0, Qt::AlignTop);
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
    topRow->addWidget(bluePanel, 1, Qt::AlignTop);
    m_overlays.insert(kBlueTeam, blueOverlay);
    registerLayoutElement(QStringLiteral("blue_team_panel"), bluePanel);
    overlayLayout->addLayout(topRow);

    overlayLayout->addStretch(1);

    auto *bottomRow = new QHBoxLayout;
    m_matchState = makeLabel(overlay, Qt::AlignLeft | Qt::AlignVCenter);
    m_matchState->setObjectName(QStringLiteral("matchState"));
    QFont stateFont = m_matchState->font();
    stateFont.setPointSize(20);
    m_matchState->setFont(stateFont);
    bottomRow->addWidget(m_matchState, 0, Qt::AlignLeft | Qt::AlignBottom);
    bottomRow->addStretch(1);
    m_sourceLabel = makeLabel(overlay, Qt::AlignRight | Qt::AlignVCenter);
    m_sourceLabel->setObjectName(QStringLiteral("sourceLabel"));
    QFont sourceFont = m_sourceLabel->font();
    sourceFont.setPointSize(20);
    sourceFont.setBold(true);
    m_sourceLabel->setFont(sourceFont);
    bottomRow->addWidget(m_sourceLabel, 0, Qt::AlignRight | Qt::AlignBottom);
    overlayLayout->addLayout(bottomRow);
    registerLayoutElement(QStringLiteral("match_state"), m_matchState);
    registerLayoutElement(QStringLiteral("source_label"), m_sourceLabel);

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
    frame->setMinimumWidth(420);

    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(22, 16, 22, 16);
    layout->setSpacing(7);

    overlay->teamId = team;
    overlay->team = makeLabel(frame, team == kRedTeam ? Qt::AlignRight : Qt::AlignLeft);
    overlay->team->setObjectName(team == kRedTeam ? QStringLiteral("redTeam")
                                                  : QStringLiteral("blueTeam"));
    overlay->team->setText(teamTitle);
    QFont teamFont = overlay->team->font();
    teamFont.setBold(true);
    teamFont.setPointSize(30);
    overlay->team->setFont(teamFont);

    auto *teamRow = new QHBoxLayout;
    teamRow->setContentsMargins(0, 0, 0, 0);
    teamRow->setSpacing(10);
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
        teamRow->addStretch(1);
        teamRow->addWidget(overlay->connection, 0, Qt::AlignRight | Qt::AlignVCenter);
        teamRow->addWidget(overlay->team, 0, Qt::AlignRight | Qt::AlignVCenter);
    } else {
        teamRow->addWidget(overlay->team, 0, Qt::AlignLeft | Qt::AlignVCenter);
        teamRow->addWidget(overlay->connection, 0, Qt::AlignLeft | Qt::AlignVCenter);
        teamRow->addStretch(1);
    }
    layout->addLayout(teamRow);

    auto *cardRow = new QHBoxLayout;
    cardRow->setContentsMargins(0, 0, 0, 0);
    cardRow->setSpacing(8);
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
    cardRow->addStretch(1);
    cardRow->addWidget(addCardIndicator(QRect(4, 269, 53, 66), tr("红牌"),
                                        &overlay->redCard));
    cardRow->addWidget(addCardIndicator(QRect(897, 472, 53, 66), tr("黄牌"),
                                        &overlay->yellowCard));
    cardRow->addStretch(1);
    layout->addLayout(cardRow);

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
    layout->addWidget(healthBar);

    auto *heatBar = new AtlasProgressBar(
        assetCrop(QStringLiteral(":/broadcast/statusbar_atlas.png"),
                  QRect(211, 410, 276, 24)),
        team == kBlueTeam,
        false,
        frame);
    heatBar->setRange(0, kMaxHeat);
    heatBar->setFixedHeight(20);
    overlay->heatBar = heatBar;
    layout->addWidget(heatBar);

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

    auto *statusRow = new QHBoxLayout;
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
    statusRow->addWidget(overlay->health);
    statusRow->addStretch(1);
    statusRow->addWidget(overlay->heat);
    statusRow->addStretch(1);
    statusRow->addWidget(overlay->state);
    statusRow->addStretch(1);
    statusRow->addWidget(overlay->shoot);
    layout->addLayout(statusRow);

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
    if (m_remainingSeconds == 0)
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
    if (m_remainingSeconds == 0)
        resetMatch();
    if (m_matchRunning)
        return;

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
    m_matchTimer->stop();
    m_matchRunning = false;
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
    m_remainingSeconds = qBound(0, remainingSeconds, kMatchDurationSeconds);
    m_matchRunning = running && m_remainingSeconds > 0;
    if (m_matchRunning)
        m_matchTimer->start();
    else
        m_matchTimer->stop();

    updateTimerDisplay();
    emit matchStateChanged(m_matchRunning);
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
    renderProgramFrame();
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
    renderProgramFrame();
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
    m_layoutAppliedSizes.clear();
    if (isVisible())
        applyLayoutPositions();
}

bool BroadcastWindow::saveLayout(const QString &filePath) const
{
    QSettings settings(filePath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("broadcast_layout"));
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

    m_layoutSizes.clear();
    settings.beginGroup(QStringLiteral("broadcast_layout"));
    for (const QString &elementId : layoutElementIds()) {
        const QString xKey = elementId + QStringLiteral("/x");
        const QString yKey = elementId + QStringLiteral("/y");
        if (settings.contains(xKey) && settings.contains(yKey)) {
            m_layoutOffsets.insert(elementId,
                                   QPoint(settings.value(xKey).toInt(),
                                          settings.value(yKey).toInt()));
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
    renderProgramFrame();
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

    const QPixmap pixmap = QPixmap::fromImage(frame).scaled(
        m_programView->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
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
    for (const QString &elementId : layoutElementIds()) {
        QWidget *widget = m_layoutElements.value(elementId);
        if (!widget)
            continue;

        const QPoint offset = m_layoutOffsets.value(elementId, QPoint());
        const QPoint currentPosition = widget->pos();
        QPoint basePosition;

        if (!m_layoutBasePositions.contains(elementId)) {
            basePosition = currentPosition;
        } else if (!m_layoutAppliedPositions.contains(elementId)
                   || currentPosition != m_layoutAppliedPositions.value(elementId)) {
            // A parent layout or a resize changed the automatic position.
            basePosition = currentPosition;
        } else {
            basePosition = m_layoutBasePositions.value(elementId);
        }

        m_layoutBasePositions.insert(elementId, basePosition);
        const QPoint targetPosition = basePosition + offset;
        if (currentPosition != targetPosition)
            widget->move(targetPosition);
        m_layoutAppliedPositions.insert(elementId, targetPosition);

        const QSize currentSize = widget->size();
        QSize baseSize;
        if (!m_layoutBaseSizes.contains(elementId)) {
            baseSize = currentSize;
        } else if (m_layoutAppliedSizes.contains(elementId)
                   && currentSize != m_layoutAppliedSizes.value(elementId)) {
            // A parent layout or a resize changed the automatic size.
            baseSize = currentSize;
        } else {
            baseSize = m_layoutBaseSizes.value(elementId);
        }
        m_layoutBaseSizes.insert(elementId, baseSize);

        if (m_layoutSizes.contains(elementId)) {
            const QSize requestedSize = m_layoutSizes.value(elementId);
            if (widget->minimumSize() != requestedSize)
                widget->setMinimumSize(requestedSize);
            if (widget->maximumSize() != requestedSize)
                widget->setMaximumSize(requestedSize);
            if (currentSize != requestedSize)
                widget->resize(requestedSize);
            m_layoutAppliedSizes.insert(elementId, requestedSize);
        } else {
            if (m_layoutMinimumSizes.contains(elementId))
                widget->setMinimumSize(m_layoutMinimumSizes.value(elementId));
            if (m_layoutMaximumSizes.contains(elementId))
                widget->setMaximumSize(m_layoutMaximumSizes.value(elementId));
            if (baseSize.isValid() && baseSize.width() > 0 && baseSize.height() > 0
                && widget->size() != baseSize) {
                widget->resize(baseSize);
            }
            m_layoutAppliedSizes.insert(elementId, widget->size());
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
    applyLayoutPositions();
    QTimer::singleShot(0, this, &BroadcastWindow::applyLayoutPositions);
    renderProgramFrame();
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
    if (isVisible())
        applyLayoutPositions();
    renderProgramFrame();
}
