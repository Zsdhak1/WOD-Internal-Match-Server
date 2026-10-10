#ifndef HUDDIGITSLABEL_H
#define HUDDIGITSLABEL_H

#include <QLabel>
#include <QFontInfo>
#include <QPainter>
#include <QPolygonF>

// Original, scalable angular numeric face. Keep QLabel's text/property API
// for snapshots and accessibility, but draw deterministic beveled glyphs so
// every deployment looks identical without installing a system font.
class HudDigitsLabel final : public QLabel
{
public:
    explicit HudDigitsLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(text, parent)
    {
        setProperty("angularDigits", true);
        setProperty("boldDigits", true);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    explicit HudDigitsLabel(QWidget *parent = nullptr)
        : HudDigitsLabel(QString(), parent) {}

protected:
    void paintEvent(QPaintEvent *) override
    {
        const QString value = text();
        if (value.isEmpty()) return;
        // Four digits and the narrow square colon use fixed advances to avoid
        // countdown jitter. Scale down for unusually large/custom scores.
        qreal units = 0;
        for (QChar c : value) units += c == QLatin1Char(':') ? 0.26 : 0.72;
        // The last digit's ink ends at 0.64, not its 0.72 advance. Exclude
        // that trailing spacing when centring the visible numeric artwork.
        if (value.contains(QLatin1Char(':')) && value.back().isDigit()) units -= 0.08;
        qreal h = qMin(qreal(QFontInfo(font()).pixelSize()), height() * 0.88);
        h = qMin(h, width() * 0.94 / qMax(qreal(0.01), units));
        const qreal total = units * h;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::WindowText));
        p.translate((width() - total) / 2, (height() - h) / 2);
        p.scale(h, h);
        // Bold 0.19-em strokes (formerly 0.13), retaining the same outer
        // bounds, fixed advances, small segment gaps and beveled corners.
        const auto horizontal = [&p](qreal y) {
            QPolygonF shape;
            shape << QPointF(.08,y) << QPointF(.125,y-.095)
                  << QPointF(.515,y-.095) << QPointF(.56,y)
                  << QPointF(.515,y+.095) << QPointF(.125,y+.095);
            p.drawPolygon(shape);
        };
        const auto vertical = [&p](qreal x, qreal top, qreal bottom) {
            QPolygonF shape;
            shape << QPointF(x,top) << QPointF(x+.095,top+.04)
                  << QPointF(x+.095,bottom-.04) << QPointF(x,bottom)
                  << QPointF(x-.095,bottom-.04) << QPointF(x-.095,top+.04);
            p.drawPolygon(shape);
        };
        // Bits: top, upper-right, lower-right, bottom, lower-left,
        // upper-left, middle. This source artwork is not derived from a font.
        static constexpr unsigned masks[] = {
            0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
        };
        for (QChar c : value) {
            if (c == QLatin1Char(':')) {
                p.drawRect(QRectF(.05,.27,.15,.15));
                p.drawRect(QRectF(.05,.61,.15,.15));
                p.translate(.26,0);
                continue;
            }
            const int digit = c.digitValue();
            if (digit >= 0 && digit <= 9) {
                const unsigned mask = masks[digit];
                p.save();
                if (digit == 1) p.translate(-.225,0); // Centre the narrow glyph in its fixed-width cell.
                if (mask & 0x01) horizontal(.095);
                if (mask & 0x02) vertical(.545,.13,.465);
                if (mask & 0x04) vertical(.545,.535,.87);
                if (mask & 0x08) horizontal(.905);
                if (mask & 0x10) vertical(.095,.535,.87);
                if (mask & 0x20) vertical(.095,.13,.465);
                if (mask & 0x40) horizontal(.50);
                p.restore();
            }
            p.translate(.72,0);
        }
    }
};

#endif // HUDDIGITSLABEL_H
