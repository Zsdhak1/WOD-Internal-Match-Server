#include "refereeentrypanel.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QEasingCurve>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <cmath>
#include <functional>

namespace {
const QString unknown = QStringLiteral("未提供");
const QColor kRed(0xe8, 0x41, 0x4f);
const QColor kBlue(0x3a, 0x8d, 0xff);
const QColor kGold(0xf2, 0xc9, 0x4c);
const QColor kCanvas(0x0d, 0x10, 0x15);
constexpr int kMaxRevives = 3;
constexpr int kEntranceMs = 320;
constexpr int kToastMs = 1500;
constexpr int kFlashMs = 1200;
constexpr int kNarrowWidth = 860;

// JSON bools, strings, null, fractional values and negative sentinels are not
// known counters. In particular QJsonValue::toInt() must not silently turn
// missing telemetry into 0 or truncate a malformed number.
bool knownNumber(const QJsonValue &value, qint64 *number, qint64 minimum = 0)
{
    if (!value.isDouble())
        return false;
    const double d = value.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d || d < double(minimum)
        || d > 9007199254740991.0)
        return false;
    *number = qint64(d);
    return true;
}

QString counter(const QJsonValue &value, qint64 minimum = 0)
{
    qint64 number = 0;
    return knownNumber(value, &number, minimum) ? QString::number(number) : unknown;
}

QString stringValue(const QJsonValue &value)
{
    if (value.isString() && !value.toString().trimmed().isEmpty())
        return value.toString().trimmed();
    return unknown;
}

QString identifier(const QJsonValue &value)
{
    return value.isString() ? stringValue(value) : counter(value);
}

QString revives(const QJsonObject &side)
{
    // A supplied quotaKnown=false explicitly means the counter is not verified.
    const QJsonValue known = side.value(QStringLiteral("quotaKnown"));
    if (!known.isUndefined() && (!known.isBool() || !known.toBool()))
        return unknown;
    qint64 number = 0;
    const QJsonValue value = side.value(QStringLiteral("remainingRevives"));
    return knownNumber(value, &number) && number <= 3 ? QString::number(number) : unknown;
}

QString reviveState(const QJsonObject &side)
{
    const QJsonValue pending = side.value(QStringLiteral("revivePending"));
    return pending.isBool() && pending.toBool()
        ? QStringLiteral("复活指令待确认；此处保留本局结算快照") : QString();
}

bool validResult(const QJsonObject &summary)
{
    return summary.value(QStringLiteral("valid")).isBool()
        && summary.value(QStringLiteral("valid")).toBool();
}

int winnerSide(const QJsonObject &summary)
{
    qint64 winner = 0;
    if (!validResult(summary)
        || !knownNumber(summary.value(QStringLiteral("winner")), &winner)
        || (winner != 1 && winner != 2))
        return 0;
    return int(winner);
}

QString durationText(const QJsonObject &summary)
{
    qint64 duration = 0;
    if (!knownNumber(summary.value(QStringLiteral("durationMs")), &duration))
        return unknown;
    return QStringLiteral("%1 秒").arg(duration / 1000.0, 0, 'f', 1);
}

QString durationClock(const QJsonObject &summary)
{
    qint64 duration = 0;
    if (!knownNumber(summary.value(QStringLiteral("durationMs")), &duration))
        return QString();
    const qint64 seconds = duration / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QLabel *label(const QString &text, QWidget *parent, const char *role = "muted")
{
    auto *value = new QLabel(text, parent);
    value->setProperty("role", QString::fromLatin1(role));
    value->setWordWrap(true);
    value->setTextFormat(Qt::PlainText);
    return value;
}

QLineEdit *readOnlyField(const QString &name, QWidget *parent, const char *role)
{
    auto *edit = new QLineEdit(parent);
    edit->setObjectName(name);
    edit->setReadOnly(true);
    edit->setFrame(false);
    edit->setFocusPolicy(Qt::NoFocus);
    // The surrounding card owns clicks (click = copy); the field is display only.
    edit->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    edit->setContextMenuPolicy(Qt::NoContextMenu);
    edit->setMinimumWidth(0);
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    edit->setProperty("role", QString::fromLatin1(role));
    return edit;
}

// Read-only value display: big number + small unit on one shared baseline,
// unit immediately after the number. Self-painted (no cursor, no selection);
// QLineEdit is kept as the base so text()/isReadOnly() semantics and the
// stylesheet font/colour (role/tone properties) still apply.
class ValueLine : public QLineEdit
{
public:
    ValueLine(const QString &unit, QWidget *parent) : QLineEdit(parent), m_unit(unit)
    {
        setReadOnly(true);
        setFrame(false);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setContextMenuPolicy(Qt::NoContextMenu);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override
    {
        return QSize(120, fontMetrics().height() + 2);
    }
    QSize minimumSizeHint() const override { return QSize(40, fontMetrics().height() + 2); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        const QFontMetrics fm(font());
        const QRect r = contentsRect();
        const int baseline = r.top() + (r.height() - fm.height()) / 2 + fm.ascent();
        const bool showUnit = !m_unit.isEmpty() && text() != unknown;
        QFont unitFont = font();
        unitFont.setPixelSize(qMax(13, fm.height() * 2 / 5));
        unitFont.setWeight(QFont::DemiBold);
        const QFontMetrics um(unitFont);
        const int unitWidth = showUnit ? um.horizontalAdvance(m_unit) + 8 : 0;
        const QString shown = fm.elidedText(text(), Qt::ElideRight, qMax(0, r.width() - unitWidth));
        const int textWidth = fm.horizontalAdvance(shown);
        int x = r.left();
        if (alignment() & Qt::AlignRight)
            x = r.right() - textWidth - unitWidth;
        p.setFont(font());
        p.setPen(palette().color(QPalette::Text));
        p.drawText(QPoint(x, baseline), shown);
        if (showUnit) {
            p.setFont(unitFont);
            p.setPen(QColor(0x9a, 0xa4, 0xb5));
            p.drawText(QPoint(x + textWidth + 8, baseline), m_unit);
        }
    }

private:
    QString m_unit;
};

QFrame *card(QWidget *parent, const QString &name, const char *role = "card")
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(name);
    frame->setProperty("role", QString::fromLatin1(role));
    return frame;
}

// Re-polish only when a style-relevant dynamic property actually changes, so a
// refresh never triggers a stylesheet re-parse or unnecessary relayout.
void setStyleProperty(QWidget *widget, const char *name, const QVariant &value)
{
    if (widget->property(name) == value)
        return;
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->updateGeometry();
    widget->update();
}

void setTextIfChanged(QLineEdit *edit, const QString &text)
{
    if (edit->text() != text)
        edit->setText(text);
    edit->setCursorPosition(0);
    edit->setToolTip(text);
}

void setTextIfChanged(QLabel *label, const QString &text)
{
    if (label->text() != text)
        label->setText(text);
}
} // namespace

// Copyable data card: click, Enter, Return or Space activates the copy action.
class RefereeEntryPanel::DataCard : public QFrame
{
public:
    explicit DataCard(QWidget *parent) : QFrame(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setProperty("role", QStringLiteral("dataCard"));
    }
    std::function<void()> activate;
    void setFlash(bool flash)
    {
        if (m_flash == flash)
            return;
        m_flash = flash;
        update();
    }
    bool flash() const { return m_flash; }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QFrame::paintEvent(event);
        if (!m_flash)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor fill = kGold;
        fill.setAlpha(30);
        p.setBrush(fill);
        p.setPen(QPen(kGold, 2));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 12, 12);
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && activate)
            activate();
        QFrame::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        const int key = event->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) && activate) {
            if (!event->isAutoRepeat())
                activate();
            event->accept();
            return;
        }
        QFrame::keyPressEvent(event);
    }

private:
    bool m_flash = false;
};

// Three-segment revive quota meter. Unknown counters draw dashed placeholders
// instead of empty segments so "未提供" is never mistaken for 0.
class RefereeEntryPanel::ReviveMeter : public QWidget
{
public:
    ReviveMeter(const QColor &color, QWidget *parent) : QWidget(parent), m_color(color)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFixedHeight(16);
        setMinimumWidth(96);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setValue(int value) // -1 = unknown
    {
        if (m_value == value)
            return;
        m_value = value;
        update();
    }
    void setProgress(double progress)
    {
        if (qFuzzyCompare(m_progress, progress))
            return;
        m_progress = progress;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double gap = 8.0;
        const double w = (width() - gap * (kMaxRevives - 1)) / kMaxRevives;
        const double fill = m_value < 0 ? 0.0 : m_value * m_progress;
        for (int i = 0; i < kMaxRevives; ++i) {
            const QRectF r(i * (w + gap) + 1, 2, w - 2, height() - 4);
            if (m_value < 0) {
                QPen pen(QColor(0x7d, 0x86, 0x96), 1.4, Qt::DashLine);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r, 4, 4);
                continue;
            }
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x2a, 0x30, 0x3b));
            p.drawRoundedRect(r, 4, 4);
            const double amount = qBound(0.0, fill - i, 1.0);
            if (amount > 0.0) {
                p.setBrush(m_color);
                p.drawRoundedRect(QRectF(r.left(), r.top(), r.width() * amount, r.height()), 4, 4);
            }
        }
    }

private:
    QColor m_color;
    int m_value = -1;
    double m_progress = 1.0;
};

// Remaining-HP ratio bar; dashed outline when HP or max HP is unknown.
class RefereeEntryPanel::HpBar : public QWidget
{
public:
    HpBar(const QColor &color, QWidget *parent) : QWidget(parent), m_color(color)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFixedHeight(8);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setRatio(double ratio) // < 0 = unknown
    {
        if (qFuzzyCompare(m_ratio + 2.0, ratio + 2.0))
            return;
        m_ratio = ratio;
        update();
    }
    void setProgress(double progress)
    {
        if (qFuzzyCompare(m_progress, progress))
            return;
        m_progress = progress;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (m_ratio < 0) {
            p.setPen(QPen(QColor(0x7d, 0x86, 0x96), 1.2, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r, 4, 4);
            return;
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x2a, 0x30, 0x3b));
        p.drawRoundedRect(r, 4, 4);
        const double w = r.width() * qBound(0.0, m_ratio, 1.0) * m_progress;
        if (w > 0.5) {
            p.setBrush(m_color);
            p.drawRoundedRect(QRectF(r.left(), r.top(), w, r.height()), 4, 4);
        }
    }

private:
    QColor m_color;
    double m_ratio = -1.0;
    double m_progress = 1.0;
};

// Entrance fade: a mouse-transparent overlay painted in the canvas colour whose
// opacity runs 1 -> 0. Hidden whenever idle, so it costs nothing at rest and no
// QGraphicsEffect is attached to the scroll area or its contents.
class RefereeEntryPanel::Curtain : public QWidget
{
public:
    explicit Curtain(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }
    void setOpacity(double opacity)
    {
        m_opacity = opacity;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QColor c = kCanvas;
        c.setAlphaF(qBound(0.0, m_opacity, 1.0));
        QPainter(this).fillRect(rect(), c);
    }

private:
    double m_opacity = 0.0;
};

// Compact copy-confirmation capsule. Self-painted (check mark / "!" drawn with
// QPainter, no special glyphs) and sized to its text; opacity is animated for
// the fade-out instead of attaching a QGraphicsEffect.
class RefereeEntryPanel::Toast : public QWidget
{
public:
    explicit Toast(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
        QFont f = font();
        f.setPixelSize(15);
        f.setBold(true);
        setFont(f);
        // Hugs its text but may shrink (text elides) so narrow layouts never clip.
        QSizePolicy policy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        policy.setRetainSizeWhenHidden(true);
        setSizePolicy(policy);
        hide();
    }
    void setMessage(const QString &text, bool warn)
    {
        m_text = text;
        m_warn = warn;
        setAccessibleName(text);
        setToolTip(text);
        updateGeometry();
        update();
    }
    QString text() const { return m_text; }
    void setOpacity(double opacity)
    {
        m_opacity = opacity;
        update();
    }
    QSize sizeHint() const override
    {
        const QFontMetrics fm(font());
        return QSize(fm.horizontalAdvance(m_text) + 64, qMax(36, fm.height() + 16));
    }
    QSize minimumSizeHint() const override { return QSize(96, sizeHint().height()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setOpacity(qBound(0.0, m_opacity, 1.0));
        const QString shown = fontMetrics().elidedText(m_text, Qt::ElideRight, qMax(0, width() - 64));
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const QColor fill = m_warn ? QColor(0x3a, 0x2d, 0x10) : kGold;
        const QColor ink = m_warn ? QColor(0xff, 0xd9, 0x8a) : QColor(0x16, 0x13, 0x0a);
        p.setPen(m_warn ? QPen(QColor(0xa0, 0x7a, 0x24), 1) : QPen(Qt::NoPen));
        p.setBrush(fill);
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        const QRectF disc(14, (height() - 20) / 2.0, 20, 20);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawEllipse(disc);
        p.setPen(QPen(fill, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        if (m_warn) {
            p.drawLine(QPointF(disc.center().x(), disc.top() + 5), QPointF(disc.center().x(), disc.top() + 11.5));
            p.drawPoint(QPointF(disc.center().x(), disc.top() + 15));
        } else {
            QPainterPath check;
            check.moveTo(disc.left() + 5.5, disc.top() + 10.5);
            check.lineTo(disc.left() + 8.8, disc.top() + 13.8);
            check.lineTo(disc.left() + 14.8, disc.top() + 6.8);
            p.drawPath(check);
        }
        p.setPen(ink);
        p.drawText(QRectF(disc.right() + 9, 0, width() - disc.right() - 9 - 16, height()),
                   Qt::AlignVCenter | Qt::AlignLeft, shown);
    }

private:
    QString m_text;
    bool m_warn = false;
    double m_opacity = 1.0;
};

namespace {
// Single stylesheet applied once in the constructor. Runtime state changes use
// dynamic properties ("tone", "side") + setStyleProperty(), never setStyleSheet.
QString panelStyleSheet()
{
    return QStringLiteral(R"(
QWidget#refereeEntryPanel { background:#0d1015; color:#eef1f6; }
QWidget#refereeEntryPanel QLabel { color:#eef1f6; background:transparent; font-size:15px; }
QWidget#refereeEntryPanel QLabel[role="muted"] { color:#8e98aa; font-size:14px; }
QWidget#refereeEntryPanel QLabel[role="eyebrow"] { color:#f2c94c; font-size:13px; font-weight:700; }
QWidget#refereeEntryPanel QLabel[role="heading"] { color:#f6f7fa; font-size:20px; font-weight:700; }
QWidget#refereeEntryPanel QLabel[role="cardTitle"] { color:#a5aec0; font-size:14px; font-weight:600; }
QWidget#refereeEntryPanel QLabel[role="unit"] { color:#8e98aa; font-size:16px; font-weight:600; }
QWidget#refereeEntryPanel QLabel[role="note"] { color:#98a2b3; font-size:14px; }
QWidget#refereeEntryPanel QLabel#refereeReasonCode { color:#6c7586; font-size:12px; }
QWidget#refereeEntryPanel QLabel[role="copied"] { color:#16130a; background:#f2c94c; border-radius:10px; padding:2px 10px; font-size:12px; font-weight:700; }
QWidget#refereeEntryPanel QLabel[role="vs"] { color:#5d6678; font-size:22px; font-weight:800; }
QWidget#refereeEntryPanel QLabel[role="round"] { color:#f2c94c; font-size:16px; font-weight:700; }
QWidget#refereeEntryPanel QLabel[role="score"] { color:#ffffff; font-size:44px; font-weight:800; }
QWidget#refereeEntryPanel QLabel[role="sideTag"] { font-size:13px; font-weight:700; }
QWidget#refereeEntryPanel QLabel[role="sideTag"][side="red"] { color:#ff7a85; }
QWidget#refereeEntryPanel QLabel[role="sideTag"][side="blue"] { color:#7fb4ff; }
QWidget#refereeEntryPanel QLabel[role="winBadge"] { color:#16130a; background:#f2c94c; border-radius:12px; padding:4px 14px; font-size:14px; font-weight:800; }
QWidget#refereeEntryPanel QLabel#refereeVerdict { border-radius:16px; padding:6px 18px; font-size:16px; font-weight:700; color:#c9d0dc; background:#1c212b; border:1px solid #333b49; }
QWidget#refereeEntryPanel QLabel#refereeVerdict[tone="pending"] { color:#ffcf6b; background:#33280f; border:1px solid #8a6a1f; }
QWidget#refereeEntryPanel QLabel#refereeVerdict[tone="red"] { color:#ffffff; background:#a92a37; border:1px solid #e8414f; }
QWidget#refereeEntryPanel QLabel#refereeVerdict[tone="blue"] { color:#ffffff; background:#1f5fc0; border:1px solid #3a8dff; }
QWidget#refereeEntryPanel QLabel#refereeWarning { color:#ffd98a; background:#2b2312; border:1px solid #6e5520; border-left:4px solid #f2a93b; border-radius:10px; padding:10px 14px; font-size:15px; font-weight:600; }
QWidget#refereeEntryPanel QLabel#refereeToast { color:#16130a; background:#f2c94c; border-radius:16px; padding:7px 16px; font-size:14px; font-weight:700; }
QWidget#refereeEntryPanel QLabel#refereeToast[tone="warn"] { color:#ffd98a; background:#3a2d10; border:1px solid #a07a24; }
QWidget#refereeEntryPanel QFrame[role="card"] { background:#141922; border:1px solid #262d39; border-radius:16px; }
QWidget#refereeEntryPanel QFrame[role="team"] { background:#151a23; border:1px solid #262d39; border-radius:14px; }
QWidget#refereeEntryPanel QFrame[role="team"][side="red"] { border-left:6px solid #e8414f; }
QWidget#refereeEntryPanel QFrame[role="team"][side="blue"] { border-right:6px solid #3a8dff; }
QWidget#refereeEntryPanel QFrame[role="team"][side="red"][tone="win"] { background:#2a161b; border:1px solid #e8414f; border-left:6px solid #e8414f; }
QWidget#refereeEntryPanel QFrame[role="team"][side="blue"][tone="win"] { background:#14213a; border:1px solid #3a8dff; border-right:6px solid #3a8dff; }
QWidget#refereeEntryPanel QFrame[role="team"][tone="lose"] { background:#12161d; }
QWidget#refereeEntryPanel QFrame[role="stats"] { background:transparent; border:none; }
QWidget#refereeEntryPanel QFrame[role="dataCard"] { background:#171c26; border:1px solid #2a3240; border-radius:12px; }
QWidget#refereeEntryPanel QFrame[role="dataCard"]:hover { border-color:#5a6475; background:#1a202b; }
QWidget#refereeEntryPanel QFrame[role="dataCard"][side="red"] { border-top:3px solid #e8414f; }
QWidget#refereeEntryPanel QFrame[role="dataCard"][side="blue"] { border-top:3px solid #3a8dff; }
QWidget#refereeEntryPanel QFrame[role="dataCard"][tone="gold"] { border-top:3px solid #f2c94c; }
QWidget#refereeEntryPanel QFrame[role="dataCard"][tone="pending"] { border-top:3px solid #f2a93b; background:#1d1a14; }
QWidget#refereeEntryPanel QFrame[role="dataCard"][tone="red"] { border-top:3px solid #e8414f; background:#24161b; }
QWidget#refereeEntryPanel QFrame[role="dataCard"][tone="blue"] { border-top:3px solid #3a8dff; background:#131d30; }
QWidget#refereeEntryPanel QFrame[role="dataCard"]:focus { border:2px solid #f2c94c; }
QWidget#refereeEntryPanel QLineEdit { color:#f5f7fa; background:transparent; border:none; padding:0; selection-background-color:#6b5a22; }
QWidget#refereeEntryPanel QLineEdit[role="teamName"] { font-size:30px; font-weight:800; }
QWidget#refereeEntryPanel QLineEdit[role="value"] { font-size:36px; font-weight:800; }
QWidget#refereeEntryPanel QLineEdit[role="metric"] { font-size:56px; font-weight:800; }
QWidget#refereeEntryPanel QLineEdit[role="hero"] { font-size:42px; font-weight:800; color:#f2c94c; }
QWidget#refereeEntryPanel QLineEdit[tone="unknown"] { color:#b4bccb; font-size:30px; font-weight:700; }
QWidget#refereeEntryPanel QLineEdit[role="hero"][tone="pending"] { color:#ffd27a; font-size:40px; }
QWidget#refereeEntryPanel QPlainTextEdit { color:#eef1f6; background:transparent; border:none; font-size:20px; selection-background-color:#6b5a22; }
QWidget#refereeEntryPanel QPlainTextEdit#refereeNotes { background:#10141b; border:1px solid #2e3644; border-radius:10px; padding:6px; font-size:15px; }
QWidget#refereeEntryPanel QPlainTextEdit#refereeNotes:focus { border:2px solid #f2c94c; }
QWidget#refereeEntryPanel QPlainTextEdit#refereeNotes:disabled { color:#5d6678; background:#12161d; }
QWidget#refereeEntryPanel QPushButton { color:#dfe5ee; background:#232a35; border:1px solid #3a4352; border-radius:10px; padding:12px 22px; font-size:16px; font-weight:700; }
QWidget#refereeEntryPanel QPushButton:hover { background:#2d3542; border-color:#f2c94c; }
QWidget#refereeEntryPanel QPushButton:focus { border:2px solid #f2c94c; }
QWidget#refereeEntryPanel QPushButton:pressed { background:#1b2029; }
QWidget#refereeEntryPanel QPushButton#refereeCopyText { color:#16130a; background:#f2c94c; border:1px solid #f2c94c; padding:14px 34px; font-size:18px; }
QWidget#refereeEntryPanel QPushButton#refereeCopyText:hover { background:#ffdc73; }
QWidget#refereeEntryPanel QPushButton#refereeCopyText:focus { border:2px solid #ffffff; }
QWidget#refereeEntryPanel QPushButton#refereeCopyText:pressed { background:#d9b23d; }
QWidget#refereeEntryPanel QPushButton[role="copyChip"] { color:#c9d0dc; background:#212834; border:1px solid #353e4d; border-radius:8px; padding:4px 12px; font-size:13px; font-weight:600; }
QWidget#refereeEntryPanel QPushButton[role="copyChip"]:hover { color:#16130a; background:#f2c94c; border-color:#f2c94c; }
QWidget#refereeEntryPanel QPushButton[role="copyChip"]:focus { border:2px solid #f2c94c; }
QWidget#refereeEntryPanel QPushButton:disabled { color:#5d6678; background:#191e27; border-color:#262d39; }
QWidget#refereeEntryPanel QScrollArea { background:#0d1015; border:none; }
QWidget#refereeEntryPanel QWidget#refereeCanvas { background:#0d1015; }
QWidget#refereeEntryPanel QScrollBar:vertical { background:#11151c; width:10px; margin:0; }
QWidget#refereeEntryPanel QScrollBar::handle:vertical { background:#3a4352; border-radius:5px; min-height:40px; }
QWidget#refereeEntryPanel QScrollBar::add-line:vertical, QWidget#refereeEntryPanel QScrollBar::sub-line:vertical { height:0; }
)");
}
} // namespace

RefereeEntryPanel::RefereeEntryPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("refereeEntryPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(QStringLiteral("裁判赛后录入信息"));
    setAccessibleName(QStringLiteral("裁判录入辅助"));
    setStyleSheet(panelStyleSheet());

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("refereeScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(scroll);
    auto *canvas = new QWidget(scroll);
    canvas->setObjectName(QStringLiteral("refereeCanvas"));
    scroll->setWidget(canvas);
    auto *canvasLayout = new QHBoxLayout(canvas);
    canvasLayout->setContentsMargins(28, 22, 28, 22);
    auto *content = new QWidget(canvas);
    content->setMaximumWidth(1760);
    content->setMinimumWidth(0);
    canvasLayout->addStretch();
    canvasLayout->addWidget(content, 1);
    canvasLayout->addStretch();
    m_mainGrid = new QGridLayout(content);
    m_mainGrid->setContentsMargins(0, 0, 0, 0);
    m_mainGrid->setHorizontalSpacing(0);
    m_mainGrid->setVerticalSpacing(16);

    buildHeader(content, m_mainGrid);
    buildFields(content);
    buildActions(content);
    // Cards absorb the spare height on tall screens instead of leaving a gap.
    m_mainGrid->setRowStretch(3, 1);

    m_entrance = new QVariantAnimation(this);
    m_entrance->setStartValue(0.0);
    m_entrance->setEndValue(1.0);
    m_entrance->setDuration(kEntranceMs);
    m_entrance->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_entrance, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &v) { applyEntranceProgress(v.toDouble()); });
    connect(m_entrance, &QVariantAnimation::finished, this, [this] { applyEntranceProgress(1.0); });

    // Single-shot timers only run for 1.2-1.5 s after a copy; idle = no timers.
    m_toastTimer = new QTimer(this);
    m_toastTimer->setSingleShot(true);
    m_toastTimer->setInterval(kToastMs);
    // Toast: fully visible for ~1.2 s, then a 300 ms fade (total ~1.5 s).
    m_toastFade = new QVariantAnimation(this);
    m_toastFade->setStartValue(1.0);
    m_toastFade->setEndValue(0.0);
    m_toastFade->setDuration(300);
    m_toastFade->setEasingCurve(QEasingCurve::InQuad);
    connect(m_toastFade, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &v) { m_toast->setOpacity(v.toDouble()); });
    connect(m_toastFade, &QVariantAnimation::finished, this, [this] { m_toast->hide(); });
    connect(m_toastTimer, &QTimer::timeout, this, [this] {
        if (m_animationsEnabled) m_toastFade->start();
        else m_toast->hide();
    });
    m_flashTimer = new QTimer(this);
    m_flashTimer->setSingleShot(true);
    m_flashTimer->setInterval(kFlashMs);
    connect(m_flashTimer, &QTimer::timeout, this, [this] { clearFlash(); });

    updateLayoutForWidth(1000);
    refreshResult();
}

void RefereeEntryPanel::buildHeader(QWidget *content, QGridLayout *layout)
{
    // Title strip: eyebrow + match ids on the left, verdict pill on the right.
    auto *strip = new QWidget(content);
    auto *stripLayout = new QHBoxLayout(strip);
    stripLayout->setContentsMargins(4, 0, 4, 0);
    stripLayout->setSpacing(16);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(4);
    titles->addWidget(label(QStringLiteral("SCOMPETITION · 裁判录入辅助"), strip, "eyebrow"));
    m_matchLabel = label(QString(), strip, "heading");
    m_matchLabel->setObjectName(QStringLiteral("refereeMatchLabel"));
    m_matchLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titles->addWidget(m_matchLabel);
    m_stateLabel = label(QString(), strip);
    m_stateLabel->setObjectName(QStringLiteral("refereeState"));
    titles->addWidget(m_stateLabel);
    stripLayout->addLayout(titles, 1);
    m_verdictPill = label(QString(), strip);
    m_verdictPill->setObjectName(QStringLiteral("refereeVerdict"));
    m_verdictPill->setWordWrap(false);
    m_verdictPill->setAccessibleName(QStringLiteral("本局裁决状态"));
    stripLayout->addWidget(m_verdictPill, 0, Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(strip, 0, 0);

    // Red vs blue match-up with team colour bars and win badges.
    auto *matchCard = card(content, QStringLiteral("refereeMatchCard"));
    auto *matchLayout = new QVBoxLayout(matchCard);
    matchLayout->setContentsMargins(18, 16, 18, 16);
    m_matchupLayout = new QGridLayout;
    m_matchupLayout->setHorizontalSpacing(18);
    m_matchupLayout->setVerticalSpacing(12);
    matchLayout->addLayout(m_matchupLayout);
    auto buildTeam = [matchCard](bool red, QLineEdit **name, QLabel **badge) {
        auto *team = card(matchCard, red ? QStringLiteral("refereeRedTeamCard") : QStringLiteral("refereeBlueTeamCard"), "team");
        team->setProperty("side", red ? QStringLiteral("red") : QStringLiteral("blue"));
        auto *v = new QVBoxLayout(team);
        v->setContentsMargins(22, 14, 22, 14);
        v->setSpacing(4);
        auto *top = new QHBoxLayout;
        auto *tag = label(red ? QStringLiteral("RED · 红方") : QStringLiteral("BLUE · 蓝方"), team, "sideTag");
        tag->setProperty("side", red ? QStringLiteral("red") : QStringLiteral("blue"));
        tag->setWordWrap(false);
        *badge = label(QStringLiteral("WIN · 本局胜方"), team, "winBadge");
        (*badge)->setWordWrap(false);
        (*badge)->setObjectName(red ? QStringLiteral("refereeRedWinBadge") : QStringLiteral("refereeBlueWinBadge"));
        QSizePolicy keep = (*badge)->sizePolicy();
        keep.setRetainSizeWhenHidden(true);
        (*badge)->setSizePolicy(keep);
        (*badge)->hide();
        if (red) { top->addWidget(tag); top->addStretch(); top->addWidget(*badge); }
        else { top->addWidget(*badge); top->addStretch(); top->addWidget(tag); }
        v->addLayout(top);
        *name = readOnlyField(red ? QStringLiteral("refereeRedName") : QStringLiteral("refereeBlueName"), team, "teamName");
        (*name)->setAccessibleName(red ? QStringLiteral("红方队名") : QStringLiteral("蓝方队名"));
        (*name)->setAlignment(red ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignRight | Qt::AlignVCenter);
        v->addWidget(*name);
        return team;
    };
    m_redTeamCard = buildTeam(true, &m_redName, &m_redWinBadge);
    m_blueTeamCard = buildTeam(false, &m_blueName, &m_blueWinBadge);
    m_matchCenter = new QWidget(matchCard);
    m_matchCenter->setMinimumWidth(200);
    auto *center = new QVBoxLayout(m_matchCenter);
    center->setContentsMargins(4, 0, 4, 0);
    center->setSpacing(2);
    m_roundLabel = label(QString(), m_matchCenter, "round");
    m_roundLabel->setObjectName(QStringLiteral("refereeRoundLabel"));
    m_roundLabel->setAlignment(Qt::AlignCenter);
    center->addWidget(m_roundLabel);
    m_seriesScore = label(QString(), m_matchCenter, "score");
    m_seriesScore->setObjectName(QStringLiteral("refereeSeriesScore"));
    m_seriesScore->setAlignment(Qt::AlignCenter);
    m_seriesScore->setWordWrap(false);
    center->addWidget(m_seriesScore);
    m_seriesLabel = label(QString(), m_matchCenter);
    m_seriesLabel->setObjectName(QStringLiteral("refereeSeriesLabel"));
    m_seriesLabel->setAlignment(Qt::AlignCenter);
    center->addWidget(m_seriesLabel);
    layout->addWidget(matchCard, 1, 0);

    m_warningLabel = label(QString(), content);
    m_warningLabel->setObjectName(QStringLiteral("refereeWarning"));
    m_warningLabel->setAccessibleName(QStringLiteral("抄录警告"));
    layout->addWidget(m_warningLabel, 2, 0);
}

RefereeEntryPanel::Field RefereeEntryPanel::makeField(FieldId id, const QString &objectSuffix, const QString &title,
                                                      const QString &accessible, QWidget *parent)
{
    Field f;
    f.card = new DataCard(parent);
    f.card->setObjectName(QStringLiteral("refereeCard%1").arg(objectSuffix));
    f.card->activate = [this, id] { copyField(id); };
    auto *v = new QVBoxLayout(f.card);
    v->setContentsMargins(18, 12, 14, 14);
    v->setSpacing(6);
    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    f.title = label(title, f.card, "cardTitle");
    f.title->setWordWrap(false);
    top->addWidget(f.title, 1);
    f.copiedBadge = label(QStringLiteral("已复制"), f.card, "copied");
    f.copiedBadge->setWordWrap(false);
    f.copiedBadge->setObjectName(QStringLiteral("refereeCopied%1").arg(objectSuffix));
    f.copiedBadge->hide();
    top->addWidget(f.copiedBadge);
    f.copy = new QPushButton(QStringLiteral("复制"), f.card);
    f.copy->setObjectName(QStringLiteral("refereeCopy%1").arg(objectSuffix));
    f.copy->setProperty("role", QStringLiteral("copyChip"));
    f.copy->setCursor(Qt::PointingHandCursor);
    // The card itself is the keyboard tab stop (Enter/Space); the chip is the
    // mouse affordance, so tab order does not contain duplicate stops.
    f.copy->setFocusPolicy(Qt::NoFocus);
    f.copy->setAccessibleName(QStringLiteral("复制%1").arg(accessible));
    connect(f.copy, &QPushButton::clicked, this, [this, id] { copyField(id); });
    top->addWidget(f.copy);
    v->addLayout(top);
    f.card->setAccessibleName(accessible);
    return f;
}

void RefereeEntryPanel::buildFields(QWidget *content)
{
    m_fieldHost = new QWidget(content);
    m_fieldHost->setObjectName(QStringLiteral("refereeFieldHost"));
    m_fieldGrid = new QGridLayout(m_fieldHost);
    m_fieldGrid->setContentsMargins(0, 0, 0, 0);
    m_fieldGrid->setHorizontalSpacing(16);
    m_fieldGrid->setVerticalSpacing(16);
    m_mainGrid->addWidget(m_fieldHost, 3, 0);

    auto addValue = [](Field &f, const QString &name, const char *role, const QString &unitText) {
        // ValueLine paints the unit right after the number on the same
        // baseline; text() stays the bare transcription value.
        auto *edit = new ValueLine(unitText, f.card);
        edit->setObjectName(name);
        edit->setProperty("role", QString::fromLatin1(role));
        edit->setAccessibleName(f.card->accessibleName());
        f.value = edit;
        f.card->layout()->addWidget(edit);
    };
    auto addNote = [](Field &f, const QString &name) {
        // Stretch above the note lets cards grow vertically on tall screens
        // while value + note stay grouped at the bottom of the card.
        static_cast<QVBoxLayout *>(f.card->layout())->addStretch(1);
        f.note = label(QString(), f.card, "note");
        f.note->setObjectName(name);
        f.card->layout()->addWidget(f.note);
    };

    Field &winner = m_fields[WinnerField];
    winner = makeField(WinnerField, QStringLiteral("Winner"), QStringLiteral("本局胜方"), QStringLiteral("本局胜方"), m_fieldHost);
    addValue(winner, QStringLiteral("refereeWinner"), "hero", QString());
    addNote(winner, QStringLiteral("refereeWinnerNote"));

    Field &ending = m_fields[EndingField];
    ending = makeField(EndingField, QStringLiteral("Ending"), QStringLiteral("结束方式"), QStringLiteral("结束方式"), m_fieldHost);
    ending.card->setProperty("tone", QStringLiteral("gold"));
    addValue(ending, QStringLiteral("refereeEndingMethod"), "value", QString());
    addNote(ending, QStringLiteral("refereeEndingNote"));

    Field &duration = m_fields[DurationField];
    duration = makeField(DurationField, QStringLiteral("Duration"), QStringLiteral("本局用时"), QStringLiteral("本局用时"), m_fieldHost);
    duration.card->setProperty("tone", QStringLiteral("gold"));
    addValue(duration, QStringLiteral("refereeDurationValue"), "value", QStringLiteral("秒"));
    addNote(duration, QStringLiteral("refereeDuration"));

    auto buildSide = [&](bool red) {
        auto *group = card(m_fieldHost, red ? QStringLiteral("refereeRedStatsCard") : QStringLiteral("refereeBlueStatsCard"), "stats");
        group->setProperty("side", red ? QStringLiteral("red") : QStringLiteral("blue"));
        auto *h = new QHBoxLayout(group);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(16);
        const QString sideName = red ? QStringLiteral("红方") : QStringLiteral("蓝方");
        const QString prefix = red ? QStringLiteral("Red") : QStringLiteral("Blue");
        const QColor color = red ? kRed : kBlue;
        Field &hp = m_fields[red ? RedHpField : BlueHpField];
        hp = makeField(red ? RedHpField : BlueHpField, prefix + QStringLiteral("Hp"),
                       sideName + QStringLiteral("剩余 HP"), sideName + QStringLiteral("剩余血量"), group);
        hp.card->setProperty("side", red ? QStringLiteral("red") : QStringLiteral("blue"));
        addValue(hp, QStringLiteral("referee%1Hp").arg(prefix), "metric", QStringLiteral("HP"));
        auto *bar = new HpBar(color, hp.card);
        hp.card->layout()->addWidget(bar);
        addNote(hp, QStringLiteral("referee%1ResourceNote").arg(prefix));
        Field &rv = m_fields[red ? RedRevivesField : BlueRevivesField];
        rv = makeField(red ? RedRevivesField : BlueRevivesField, prefix + QStringLiteral("Revives"),
                       sideName + QStringLiteral("剩余复活"), sideName + QStringLiteral("剩余复活次数"), group);
        rv.card->setProperty("side", red ? QStringLiteral("red") : QStringLiteral("blue"));
        addValue(rv, QStringLiteral("referee%1Revives").arg(prefix), "metric", QStringLiteral("/ 3 次"));
        auto *meter = new ReviveMeter(color, rv.card);
        rv.card->layout()->addWidget(meter);
        addNote(rv, QStringLiteral("referee%1ReviveNote").arg(prefix));
        h->addWidget(hp.card, 1);
        h->addWidget(rv.card, 1);
        if (red) { m_redStatsCard = group; m_redHpBar = bar; m_redMeter = meter; m_redMaxHp = hp.note; }
        else { m_blueStatsCard = group; m_blueHpBar = bar; m_blueMeter = meter; m_blueMaxHp = hp.note; }
    };
    buildSide(true);
    buildSide(false);

    Field &reason = m_fields[ReasonField];
    reason = makeField(ReasonField, QStringLiteral("Reason"), QStringLiteral("胜因 / 终止原因（系统原判定）"),
                       QStringLiteral("胜因"), m_fieldHost);
    reason.card->setProperty("tone", QStringLiteral("gold"));
    m_reason = new QPlainTextEdit(reason.card);
    m_reason->setObjectName(QStringLiteral("refereeReason"));
    m_reason->setAccessibleName(QStringLiteral("胜利 / 终止原因（系统原判定）"));
    m_reason->setReadOnly(true);
    m_reason->setFocusPolicy(Qt::NoFocus);
    m_reason->setFrameShape(QFrame::NoFrame);
    m_reason->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_reason->viewport()->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_reason->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_reason->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_reason->setMinimumHeight(60);
    // Ignored: preferred height (QPlainTextEdit defaults to ~192 px) must not
    // inflate the scroll canvas; the box only takes the leftover height.
    m_reason->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_reason->document()->setDocumentMargin(0);
    reason.card->layout()->addWidget(m_reason);
    addNote(reason, QStringLiteral("refereeReasonCode"));

    m_notesCard = card(m_fieldHost, QStringLiteral("refereeNotesCard"));
    auto *nv = new QVBoxLayout(m_notesCard);
    nv->setContentsMargins(18, 12, 18, 14);
    nv->setSpacing(8);
    nv->addWidget(label(QStringLiteral("裁判备注（可编辑，仅随复制副本，不改判）"), m_notesCard, "cardTitle"));
    m_notes = new QPlainTextEdit(m_notesCard);
    m_notes->setObjectName(QStringLiteral("refereeNotes"));
    m_notes->setAccessibleName(QStringLiteral("裁判备注"));
    m_notes->setPlaceholderText(QStringLiteral("可补充现场情况；备注仅随本局复制，不上传或修改赛果"));
    m_notes->setTabChangesFocus(true);
    m_notes->setMinimumHeight(64);
    // Ignored: preferred height (QPlainTextEdit defaults to ~192 px) must not
    // inflate the scroll canvas; the box only takes the leftover height.
    m_notes->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    nv->addWidget(m_notes, 1);
    connect(m_notes, &QPlainTextEdit::textChanged, this, [this] {
        m_copyStatus->setText(QStringLiteral("备注已修改，尚未复制；此面板不会提交外部成绩系统。"));
    });

    m_curtain = new Curtain(m_fieldHost);
}

void RefereeEntryPanel::buildActions(QWidget *content)
{
    m_actionBar = card(content, QStringLiteral("refereeActionBar"));
    auto *outer = new QVBoxLayout(m_actionBar);
    outer->setContentsMargins(18, 14, 18, 14);
    outer->setSpacing(8);
    auto *row = new QHBoxLayout;
    row->setSpacing(12);
    m_copyTextButton = new QPushButton(QStringLiteral("复制全部（文本）"), m_actionBar);
    m_copyTextButton->setObjectName(QStringLiteral("refereeCopyText"));
    m_copyTextButton->setAccessibleName(QStringLiteral("复制本局全部录入文本"));
    m_copyTextButton->setCursor(Qt::PointingHandCursor);
    m_copyTextButton->setAutoDefault(true);
    m_copyJsonButton = new QPushButton(QStringLiteral("复制 JSON"), m_actionBar);
    m_copyJsonButton->setObjectName(QStringLiteral("refereeCopyJson"));
    m_copyJsonButton->setAccessibleName(QStringLiteral("复制本局 JSON"));
    m_copyJsonButton->setCursor(Qt::PointingHandCursor);
    m_copyJsonButton->setAutoDefault(true);
    m_toast = new Toast(m_actionBar);
    m_toast->setObjectName(QStringLiteral("refereeToast"));
    row->addWidget(m_copyTextButton);
    row->addWidget(m_copyJsonButton);
    row->addSpacing(6);
    row->addWidget(m_toast, 0, Qt::AlignVCenter);
    row->addStretch(1);
    outer->addLayout(row);
    m_copyStatus = label(QString(), m_actionBar, "note");
    m_copyStatus->setObjectName(QStringLiteral("refereeCopyStatus"));
    outer->addWidget(m_copyStatus);
    outer->addWidget(label(QStringLiteral("资料来源：本局结算快照。未提供 ≠ 0；复制不是保存或提交成绩，请在外部系统核对后提交。"), m_actionBar, "note"));
    m_mainGrid->addWidget(m_actionBar, 4, 0);
    connect(m_copyTextButton, &QPushButton::clicked, this, [this] { copyEntryText(); });
    connect(m_copyJsonButton, &QPushButton::clicked, this, [this] { copyJson(); });
}

void RefereeEntryPanel::setResult(const QJsonObject &summary)
{
    // Identical snapshot (e.g. pending preview followed by the same final
    // summary): keep the referee's note and do not replay the entrance.
    if (m_resultApplied && summary == m_result)
        return;
    m_resultApplied = true;
    m_result = summary;
    {
        const QSignalBlocker block(m_notes);
        m_notes->setPlainText(summary.value(QStringLiteral("notes")).toString());
    }
    clearFlash();
    m_toastTimer->stop();
    m_toastFade->stop();
    m_toast->hide();
    refreshResult();
    playEntrance();
}

void RefereeEntryPanel::clearResult()
{
    setResult(QJsonObject());
}

QJsonObject RefereeEntryPanel::result() const
{
    if (m_result.isEmpty())
        return {};
    QJsonObject copy = m_result;
    if (copy.contains(QStringLiteral("notes")) || !m_notes->toPlainText().isEmpty())
        copy.insert(QStringLiteral("notes"), m_notes->toPlainText());
    return copy;
}

QString RefereeEntryPanel::resolvedEndingMethod() const
{
    const QString explicitMethod = m_result.value(QStringLiteral("endingMethod")).toString().trimmed();
    if (m_result.contains(QStringLiteral("endingMethod"))) {
        if (explicitMethod == QStringLiteral("normal") || explicitMethod == QStringLiteral("revive_exhausted")
            || explicitMethod == QStringLiteral("violation") || explicitMethod == QStringLiteral("safety_stop")
            || explicitMethod == QStringLiteral("time_decision"))
            return explicitMethod;
        return QString(); // Unknown explicit protocol values must not be guessed.
    }
    const QString reason = m_result.value(QStringLiteral("reason")).toString();
    if (reason == QStringLiteral("remaining_revives") || reason == QStringLiteral("remaining_hp")
        || reason == QStringLiteral("kill_score") || reason == QStringLiteral("time_draw")
        || reason == QStringLiteral("fewer_deaths") || reason == QStringLiteral("hp_ratio")
        || reason == QStringLiteral("damage_dealt") || reason == QStringLiteral("fewer_penalties"))
        return QStringLiteral("time_decision");
    if (reason == QStringLiteral("revive_exhausted"))
        return QStringLiteral("revive_exhausted");
    if (reason == QStringLiteral("red_card") || reason == QStringLiteral("yellow_card_limit")
        || reason == QStringLiteral("double_disqualification") || reason == QStringLiteral("forfeit"))
        return QStringLiteral("violation");
    if (reason == QStringLiteral("terminated"))
        return QStringLiteral("safety_stop");
    if (reason == QStringLiteral("manual") || reason == QStringLiteral("knockout")
        || reason == QStringLiteral("double_knockout") || reason == QStringLiteral("sudden_death_kill"))
        return QStringLiteral("normal");
    return QString();
}

QString RefereeEntryPanel::endingMethodText() const
{
    const QString method = resolvedEndingMethod();
    if (method == QStringLiteral("normal")) return QStringLiteral("常规结束");
    if (method == QStringLiteral("revive_exhausted")) return QStringLiteral("耗尽复活");
    if (method == QStringLiteral("violation")) return QStringLiteral("违规判负");
    if (method == QStringLiteral("safety_stop")) return QStringLiteral("安全终止（裁判终止）");
    if (method == QStringLiteral("time_decision")) return QStringLiteral("时间判定");
    return unknown;
}

QString RefereeEntryPanel::winnerText() const
{
    const int side = winnerSide(m_result);
    if (side == 1) return QStringLiteral("红方 · %1").arg(stringValue(m_result.value(QStringLiteral("redName"))));
    if (side == 2) return QStringLiteral("蓝方 · %1").arg(stringValue(m_result.value(QStringLiteral("blueName"))));
    if (validResult(m_result) && m_result.value(QStringLiteral("settlement")).toString() == QStringLiteral("termination"))
        return QStringLiteral("无胜方 · 裁判终止");
    if (validResult(m_result) && m_result.value(QStringLiteral("settlement")).toString() == QStringLiteral("draw"))
        return QStringLiteral("未判定胜方 · 平局记录");
    return QStringLiteral("未判定");
}

QString RefereeEntryPanel::reasonText() const
{
    const QString supplied = m_result.value(QStringLiteral("reasonText")).toString().trimmed();
    if (!supplied.isEmpty())
        return supplied;
    const QString reason = m_result.value(QStringLiteral("reason")).toString();
    if (reason == QStringLiteral("remaining_revives")) return QStringLiteral("时间到，按双方剩余复活次数判定");
    if (reason == QStringLiteral("remaining_hp")) return QStringLiteral("时间到，按双方剩余血量判定");
    if (reason == QStringLiteral("revive_exhausted")) return QStringLiteral("一方复活次数耗尽并阵亡，系统结束本局");
    if (reason == QStringLiteral("terminated")) return QStringLiteral("裁判终止本局；不代表物理断电已执行");
    if (reason == QStringLiteral("manual")) return QStringLiteral("裁判手动指定本局胜方");
    if (reason == QStringLiteral("red_card")) return QStringLiteral("红牌违规判负");
    if (reason == QStringLiteral("yellow_card_limit")) return QStringLiteral("黄牌达到规则上限，违规判负");
    if (reason == QStringLiteral("double_disqualification")) return QStringLiteral("双方均被取消资格，未产生胜方");
    if (reason == QStringLiteral("forfeit")) return QStringLiteral("离线超出规则宽限，判负");
    if (reason == QStringLiteral("kill_score")) return QStringLiteral("常规时间到，按原有击杀积分判定");
    if (reason == QStringLiteral("sudden_death_kill")) return QStringLiteral("原有 SD 加时中单方击杀，系统判定胜方");
    return unknown;
}

QString RefereeEntryPanel::warningText() const
{
    if (m_result.isEmpty())
        return QStringLiteral("待本局结束：尚无可抄录的结算快照。");
    if (!validResult(m_result))
        return QStringLiteral("本局尚未有效结算，胜方待判定。不得把此副本作为已完成胜负成绩提交。");
    if (winnerSide(m_result) == 0)
        return QStringLiteral("本局未产生胜方 / 已终止。请勿抄录为任一方获胜，须核对外部系统的终止或待判定选项。");
    const QJsonObject red = m_result.value(QStringLiteral("red")).toObject();
    const QJsonObject blue = m_result.value(QStringLiteral("blue")).toObject();
    if (counter(red.value(QStringLiteral("hp"))) == unknown || counter(blue.value(QStringLiteral("hp"))) == unknown
        || revives(red) == unknown || revives(blue) == unknown || endingMethodText() == unknown)
        return QStringLiteral("部分字段未提供，不能当作 0。请核对缺失数据后再向外部系统提交。");
    return QString();
}

QString RefereeEntryPanel::seriesText() const
{
    QJsonValue redWins = m_result.value(QStringLiteral("redSeriesWins"));
    QJsonValue blueWins = m_result.value(QStringLiteral("blueSeriesWins"));
    const QJsonValue series = m_result.value(QStringLiteral("seriesWins"));
    if (series.isObject()) {
        if (redWins.isUndefined()) redWins = series.toObject().value(QStringLiteral("red"));
        if (blueWins.isUndefined()) blueWins = series.toObject().value(QStringLiteral("blue"));
    } else if (series.isArray() && series.toArray().size() == 2) {
        if (redWins.isUndefined()) redWins = series.toArray().at(0);
        if (blueWins.isUndefined()) blueWins = series.toArray().at(1);
    }
    return QStringLiteral("%1 : %2").arg(counter(redWins), counter(blueWins));
}

QString RefereeEntryPanel::plainText() const
{
    const QJsonObject red = m_result.value(QStringLiteral("red")).toObject();
    const QJsonObject blue = m_result.value(QStringLiteral("blue")).toObject();
    QStringList lines;
    lines << QStringLiteral("【本局裁判录入信息 · 结算快照】");
    const QString warning = warningText();
    if (!warning.isEmpty()) lines << QStringLiteral("注意：%1").arg(warning);
    lines << QStringLiteral("场次：%1").arg(identifier(m_result.value(QStringLiteral("matchId"))))
          << QStringLiteral("本局编号：%1").arg(identifier(m_result.value(QStringLiteral("roundId"))))
          << QStringLiteral("BO3 局号：%1").arg(counter(m_result.value(QStringLiteral("roundIndex")), 1))
          << QStringLiteral("系列比分 %1").arg(seriesText())
          << QStringLiteral("红方队伍：%1").arg(stringValue(m_result.value(QStringLiteral("redName"))))
          << QStringLiteral("蓝方队伍：%1").arg(stringValue(m_result.value(QStringLiteral("blueName"))))
          << QStringLiteral("本局胜方：%1").arg(winnerText())
          << QStringLiteral("结束方式：%1").arg(endingMethodText())
          << QStringLiteral("红方剩余血量：%1 HP（最大血量：%2）").arg(counter(red.value(QStringLiteral("hp"))), counter(red.value(QStringLiteral("maxHp")), 1))
          << QStringLiteral("红方剩余复活次数：%1").arg(revives(red))
          << QStringLiteral("蓝方剩余血量：%1 HP（最大血量：%2）").arg(counter(blue.value(QStringLiteral("hp"))), counter(blue.value(QStringLiteral("maxHp")), 1))
          << QStringLiteral("蓝方剩余复活次数：%1").arg(revives(blue));
    if (!reviveState(red).isEmpty()) lines << QStringLiteral("红方状态：%1").arg(reviveState(red));
    if (!reviveState(blue).isEmpty()) lines << QStringLiteral("蓝方状态：%1").arg(reviveState(blue));
    lines << QStringLiteral("胜利 / 终止原因：%1").arg(reasonText())
          << QStringLiteral("判定代码：%1").arg(stringValue(m_result.value(QStringLiteral("reason"))))
          << QStringLiteral("本局用时：%1").arg(durationText(m_result));
    if (!m_notes->toPlainText().trimmed().isEmpty())
        lines << QStringLiteral("裁判备注：%1").arg(m_notes->toPlainText());
    lines << QStringLiteral("仅供抄录；本面板没有向外部系统保存或提交成绩。");
    return lines.join(QLatin1Char('\n'));
}

QString RefereeEntryPanel::jsonText() const
{
    QJsonObject copy = result();
    const QString resolved = resolvedEndingMethod();
    if (!copy.contains(QStringLiteral("endingMethod")) && !resolved.isEmpty())
        copy.insert(QStringLiteral("endingMethod"), resolved);
    const QString warning = warningText();
    if (!warning.isEmpty())
        copy.insert(QStringLiteral("transcriptionWarning"), warning);
    return QString::fromUtf8(QJsonDocument(copy).toJson(QJsonDocument::Indented));
}

// Single-field clipboard values are bare values (no label, no unit) so they
// can be pasted straight into an external form field. Unknown stays "未提供".
QString RefereeEntryPanel::fieldCopyText(FieldId id) const
{
    const QJsonObject red = m_result.value(QStringLiteral("red")).toObject();
    const QJsonObject blue = m_result.value(QStringLiteral("blue")).toObject();
    switch (id) {
    case WinnerField: return winnerText();
    case EndingField: return endingMethodText();
    case DurationField: {
        qint64 duration = 0;
        return knownNumber(m_result.value(QStringLiteral("durationMs")), &duration)
            ? QString::number(duration / 1000.0, 'f', 1) : unknown;
    }
    case RedHpField: return counter(red.value(QStringLiteral("hp")));
    case RedRevivesField: return revives(red);
    case BlueHpField: return counter(blue.value(QStringLiteral("hp")));
    case BlueRevivesField: return revives(blue);
    case ReasonField: return reasonText();
    case FieldCount: break;
    }
    return QString();
}

void RefereeEntryPanel::refreshResult()
{
    const bool present = !m_result.isEmpty();
    const int side = winnerSide(m_result);
    const QJsonObject red = m_result.value(QStringLiteral("red")).toObject();
    const QJsonObject blue = m_result.value(QStringLiteral("blue")).toObject();

    // Header: ids, round, series score, verdict pill.
    setTextIfChanged(m_matchLabel, present
        ? QStringLiteral("场次 %1  ·  本局 %2").arg(identifier(m_result.value(QStringLiteral("matchId"))),
                                                identifier(m_result.value(QStringLiteral("roundId"))))
        : QStringLiteral("等待本局结算…"));
    const QString round = counter(m_result.value(QStringLiteral("roundIndex")), 1);
    setTextIfChanged(m_roundLabel, round == unknown ? QStringLiteral("BO3 · 局次未提供") : QStringLiteral("BO3 · 第 %1 局").arg(round));
    const QString series = seriesText();
    setTextIfChanged(m_seriesScore, series == QStringLiteral("未提供 : 未提供") ? QStringLiteral("– : –") : series);
    setTextIfChanged(m_seriesLabel, QStringLiteral("系列比分 %1").arg(series));
    QString verdict;
    QString verdictTone;
    if (!present) { verdict = QStringLiteral("等待本局结算…"); verdictTone = QStringLiteral("idle"); }
    else if (side == 1) { verdict = QStringLiteral("红方胜 · RED WIN"); verdictTone = QStringLiteral("red"); }
    else if (side == 2) { verdict = QStringLiteral("蓝方胜 · BLUE WIN"); verdictTone = QStringLiteral("blue"); }
    else { verdict = QStringLiteral("待裁判判定"); verdictTone = QStringLiteral("pending"); }
    setTextIfChanged(m_verdictPill, verdict);
    setStyleProperty(m_verdictPill, "tone", verdictTone);
    setTextIfChanged(m_stateLabel, !present ? QStringLiteral("本局结束后自动显示只读结算快照；不能改判。")
        : validResult(m_result) ? QStringLiteral("本局已结算 · 以下为自动提供的只读数据，点击卡片可单项复制")
                                : QStringLiteral("等待有效赛后结算 · 不能改判"));

    // Match-up cards.
    setTextIfChanged(m_redName, present ? stringValue(m_result.value(QStringLiteral("redName"))) : QStringLiteral("红方"));
    setTextIfChanged(m_blueName, present ? stringValue(m_result.value(QStringLiteral("blueName"))) : QStringLiteral("蓝方"));
    setStyleProperty(m_redTeamCard, "tone", side == 1 ? QStringLiteral("win") : side == 2 ? QStringLiteral("lose") : QString());
    setStyleProperty(m_blueTeamCard, "tone", side == 2 ? QStringLiteral("win") : side == 1 ? QStringLiteral("lose") : QString());
    m_redWinBadge->setVisible(side == 1);
    m_blueWinBadge->setVisible(side == 2);

    const QString warning = warningText();
    setTextIfChanged(m_warningLabel, warning);
    m_warningLabel->setVisible(present && !warning.isEmpty());

    // Data cards. The QLineEdit text is always the exact transcription value.
    auto setValue = [](Field &f, const QString &text) {
        setTextIfChanged(f.value, text);
        setStyleProperty(f.value, "tone", text == unknown ? QStringLiteral("unknown") : QString());
        f.card->setToolTip(QStringLiteral("点击或按 Enter / 空格复制：%1").arg(text));
    };
    Field &winner = m_fields[WinnerField];
    setValue(winner, winnerText());
    setStyleProperty(winner.value, "tone", side == 0 ? QStringLiteral("pending") : QString());
    setStyleProperty(winner.card, "tone", side == 1 ? QStringLiteral("red") : side == 2 ? QStringLiteral("blue") : QStringLiteral("pending"));
    setTextIfChanged(winner.note, side ? QStringLiteral("以系统原判定为准，面板不改判")
                                       : QStringLiteral("未判定 / 无胜方：请勿抄录为任一方获胜"));
    setValue(m_fields[EndingField], endingMethodText());
    const QString method = resolvedEndingMethod();
    setTextIfChanged(m_fields[EndingField].note, method.isEmpty() ? QStringLiteral("协议代码未提供") : QStringLiteral("协议代码 %1").arg(method));
    setValue(m_fields[DurationField], fieldCopyText(DurationField));
    const QString clock = durationClock(m_result);
    setTextIfChanged(m_fields[DurationField].note, clock.isEmpty()
        ? QStringLiteral("本局用时：未提供 · 不会自动补零")
        : QStringLiteral("本局用时：%1（%2）").arg(durationText(m_result), clock));

    auto sideCards = [&](const QJsonObject &obj, FieldId hpId, FieldId rvId, HpBar *bar, ReviveMeter *meter) {
        const QString hp = counter(obj.value(QStringLiteral("hp")));
        const QString maxHp = counter(obj.value(QStringLiteral("maxHp")), 1);
        setValue(m_fields[hpId], hp);
        qint64 hpNumber = 0, maxNumber = 0;
        const bool ratioKnown = knownNumber(obj.value(QStringLiteral("hp")), &hpNumber)
            && knownNumber(obj.value(QStringLiteral("maxHp")), &maxNumber, 1);
        bar->setRatio(ratioKnown ? double(hpNumber) / double(maxNumber) : -1.0);
        setTextIfChanged(m_fields[hpId].note, QStringLiteral("最大血量 %1 HP").arg(maxHp));
        const QString rv = revives(obj);
        setValue(m_fields[rvId], rv);
        meter->setValue(rv == unknown ? -1 : rv.toInt());
        QString note = rv == unknown ? QStringLiteral("配额未提供 ≠ 0") : QStringLiteral("剩余次数以系统配额为准");
        if (!reviveState(obj).isEmpty())
            note = reviveState(obj);
        setTextIfChanged(m_fields[rvId].note, note);
    };
    sideCards(red, RedHpField, RedRevivesField, m_redHpBar, m_redMeter);
    sideCards(blue, BlueHpField, BlueRevivesField, m_blueHpBar, m_blueMeter);

    const QString reason = reasonText();
    if (m_reason->toPlainText() != reason)
        m_reason->setPlainText(reason);
    m_fields[ReasonField].card->setToolTip(QStringLiteral("点击或按 Enter / 空格复制：%1").arg(reason));
    setTextIfChanged(m_fields[ReasonField].note, QStringLiteral("判定代码：%1").arg(stringValue(m_result.value(QStringLiteral("reason")))));

    for (int i = 0; i < FieldCount; ++i) {
        Field &f = m_fields[i];
        // Missing values: the chip is disabled; activating the card still
        // copies the literal 未提供 with an explicit warning toast.
        const bool hasValue = present && fieldCopyText(FieldId(i)) != unknown;
        f.card->setEnabled(present);
        f.copy->setEnabled(hasValue);
        f.copy->setToolTip(hasValue ? QStringLiteral("复制该项") : QStringLiteral("未提供，无可复制的数值"));
    }
    m_copyTextButton->setEnabled(present);
    m_copyJsonButton->setEnabled(present);
    m_notes->setEnabled(present);
    m_copyStatus->setText(present ? QStringLiteral("核对双方与局号后复制。本局数据不会自动提交外部系统。")
                                  : QStringLiteral("本局结束后显示录入信息。"));
}

void RefereeEntryPanel::showToast(const QString &text, bool warn)
{
    m_toastFade->stop();
    m_toast->setMessage(text, warn);
    m_toast->setOpacity(1.0);
    m_toast->show();
    m_toastTimer->start(kToastMs - 300); // restarts on repeated copies; one shot, then idle
}

QString RefereeEntryPanel::toastText() const
{
    return m_toast->isHidden() ? QString() : m_toast->text();
}

void RefereeEntryPanel::flashField(FieldId id)
{
    clearFlash();
    Field &f = m_fields[id];
    f.card->setFlash(true);
    f.copiedBadge->show();
    m_flashedField = id;
    m_flashTimer->start();
}

void RefereeEntryPanel::clearFlash()
{
    if (m_flashTimer)
        m_flashTimer->stop();
    if (m_flashedField < 0)
        return;
    Field &f = m_fields[m_flashedField];
    f.card->setFlash(false);
    f.copiedBadge->hide();
    m_flashedField = -1;
}

void RefereeEntryPanel::copyField(FieldId id)
{
    if (m_result.isEmpty() || id < 0 || id >= FieldCount)
        return;
    const QString value = fieldCopyText(id);
    QGuiApplication::clipboard()->setText(value);
    flashField(id);
    const QString name = m_fields[id].card->accessibleName();
    const QString warning = warningText();
    if (value == unknown) {
        showToast(QStringLiteral("已复制「%1」：未提供（不是 0）").arg(name), true);
    } else if (!warning.isEmpty() && (id == WinnerField || id == EndingField)) {
        showToast(QStringLiteral("已复制「%1」· 注意：本局胜方未判定").arg(name), true);
    } else {
        showToast(QStringLiteral("已复制「%1」：%2").arg(name, value.left(24)), false);
    }
    m_copyStatus->setText(warning.isEmpty() ? QStringLiteral("已复制单项「%1」。").arg(name)
                                            : QStringLiteral("已复制单项「%1」，但请注意：%2").arg(name, warning));
}

void RefereeEntryPanel::copyEntryText()
{
    if (m_result.isEmpty()) return;
    QGuiApplication::clipboard()->setText(plainText());
    const QString warning = warningText();
    showToast(warning.isEmpty() ? QStringLiteral("已复制全部文本") : QStringLiteral("已复制全部文本 · 含警告"), !warning.isEmpty());
    m_copyStatus->setText(warning.isEmpty() ? QStringLiteral("已复制本局录入文本；请在外部系统核对后保存。")
                                           : QStringLiteral("已复制，但请注意：%1").arg(warning));
}

void RefereeEntryPanel::copyJson()
{
    if (m_result.isEmpty()) return;
    QGuiApplication::clipboard()->setText(jsonText());
    const QString warning = warningText();
    showToast(warning.isEmpty() ? QStringLiteral("已复制 JSON") : QStringLiteral("已复制 JSON · 含警告"), !warning.isEmpty());
    m_copyStatus->setText(warning.isEmpty() ? QStringLiteral("已复制本局 JSON；未发送网络请求或提交成绩。")
                                           : QStringLiteral("已复制 JSON，但请注意：%1").arg(warning));
}

void RefereeEntryPanel::setAnimationsEnabled(bool enabled)
{
    m_animationsEnabled = enabled;
    if (!enabled)
        finishAnimationsForTest();
}

void RefereeEntryPanel::finishAnimationsForTest()
{
    if (m_entrance->state() != QAbstractAnimation::Stopped)
        m_entrance->stop();
    applyEntranceProgress(1.0);
}

bool RefereeEntryPanel::isAnimating() const
{
    return m_entrance->state() == QAbstractAnimation::Running;
}

void RefereeEntryPanel::playEntrance()
{
    ++m_entranceCount;
    m_entrance->stop();
    // Empty state and hidden/disabled-animation panels snap to the final frame.
    if (!m_animationsEnabled || m_result.isEmpty() || !isVisible()) {
        applyEntranceProgress(1.0);
        return;
    }
    applyEntranceProgress(0.0);
    m_entrance->start();
}

void RefereeEntryPanel::applyEntranceProgress(double progress)
{
    // Bars/segments roll up from 0 to the value; the curtain sweeps away from
    // top to bottom. Text values are always final (copy never sees a tween).
    for (HpBar *bar : {m_redHpBar, m_blueHpBar})
        bar->setProgress(progress);
    for (ReviveMeter *meter : {m_redMeter, m_blueMeter})
        meter->setProgress(progress);
    if (progress >= 1.0) {
        m_curtain->hide();
        return;
    }
    m_curtain->setGeometry(m_fieldHost->rect());
    m_curtain->setOpacity(1.0 - progress);
    m_curtain->raise();
    m_curtain->show();
}

void RefereeEntryPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateLayoutForWidth(event->size().width());
}

void RefereeEntryPanel::updateLayoutForWidth(int width)
{
    const bool narrow = width < kNarrowWidth;
    if (m_curtain && m_curtain->isVisible())
        m_curtain->setGeometry(m_fieldHost->rect());
    if (narrow == m_narrow && m_fieldGrid->count() > 0)
        return; // no re-parenting churn on every resize step
    m_narrow = narrow;

    for (auto *w : {static_cast<QWidget *>(m_redTeamCard), m_matchCenter, static_cast<QWidget *>(m_blueTeamCard)})
        m_matchupLayout->removeWidget(w);
    m_matchupLayout->addWidget(m_redTeamCard, 0, 0);
    m_matchupLayout->addWidget(m_matchCenter, narrow ? 1 : 0, narrow ? 0 : 1);
    m_matchupLayout->addWidget(m_blueTeamCard, narrow ? 2 : 0, narrow ? 0 : 2);
    m_matchupLayout->setColumnStretch(0, 1);
    m_matchupLayout->setColumnStretch(1, 0);
    m_matchupLayout->setColumnStretch(2, narrow ? 0 : 1);

    QWidget *order[] = {m_fields[WinnerField].card, m_fields[EndingField].card, m_fields[DurationField].card,
                        m_redStatsCard, m_blueStatsCard, m_fields[ReasonField].card, m_notesCard};
    for (QWidget *w : order)
        m_fieldGrid->removeWidget(w);
    if (narrow) {
        int row = 0;
        for (QWidget *w : order)
            m_fieldGrid->addWidget(w, row++, 0, 1, 4);
    } else {
        m_fieldGrid->addWidget(order[0], 0, 0, 1, 2);
        m_fieldGrid->addWidget(order[1], 0, 2);
        m_fieldGrid->addWidget(order[2], 0, 3);
        m_fieldGrid->addWidget(order[3], 1, 0, 1, 2);
        m_fieldGrid->addWidget(order[4], 1, 2, 1, 2);
        m_fieldGrid->addWidget(order[5], 2, 0, 1, 2);
        m_fieldGrid->addWidget(order[6], 2, 2, 1, 2);
    }
    for (int c = 0; c < 4; ++c)
        m_fieldGrid->setColumnStretch(c, 1);
    for (int r = 0; r < 8; ++r)
        m_fieldGrid->setRowStretch(r, !narrow && r < 3 ? (r == 1 ? 5 : 4) : 0);
    // The root layout resizes the scroll area *before* this handler switches
    // modes, so the canvas would keep the previous mode's height (e.g. a
    // stacked 2000 px canvas at 1920 wide). Re-fit once, only on a mode switch.
    // Queued (not a timer): runs once after the resize has settled the viewport.
    // setWidgetResizable(true) makes QScrollArea recompute the widget size.
    if (auto *scroll = findChild<QScrollArea *>(QStringLiteral("refereeScroll"))) {
        QMetaObject::invokeMethod(scroll, [scroll] {
            QWidget *canvas = scroll->widget();
            if (!canvas)
                return;
            if (QLayout *l = canvas->layout()) {
                l->invalidate(); // drop the cached heightForWidth of the old mode
                l->activate();
            }
            // setWidgetResizable(true) is a no-op when already set, so size
            // the canvas explicitly from the settled viewport.
            const QSize vp = scroll->viewport()->size();
            const int h = canvas->hasHeightForWidth() ? canvas->heightForWidth(vp.width())
                                                      : canvas->minimumSizeHint().height();
            canvas->resize(vp.width(), qMax(vp.height(), h));
        }, Qt::QueuedConnection);
    }
}
