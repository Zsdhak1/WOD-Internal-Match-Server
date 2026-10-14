#include "refereeentrypanel.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>

#include <cstdio>

#define VERIFY(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

namespace {
void settle(int ms = 80)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(4);
    }
}

QJsonObject fixture()
{
    return {
        {QStringLiteral("valid"), true},
        {QStringLiteral("winner"), 1},
        {QStringLiteral("reason"), QStringLiteral("remaining_revives")},
        {QStringLiteral("reasonText"), QStringLiteral("时间到：红方剩余复活 2 次，蓝方 1 次，红方获胜")},
        {QStringLiteral("settlement"), QStringLiteral("redwin")},
        {QStringLiteral("durationMs"), 180000},
        {QStringLiteral("matchId"), QStringLiteral("A-012")},
        {QStringLiteral("roundId"), QStringLiteral("A-012-R2")},
        {QStringLiteral("roundIndex"), 2},
        {QStringLiteral("redSeriesWins"), 1},
        {QStringLiteral("blueSeriesWins"), 0},
        {QStringLiteral("redName"), QStringLiteral("星火机器人战队")},
        {QStringLiteral("blueName"), QStringLiteral("蔚蓝机器人战队")},
        {QStringLiteral("red"), QJsonObject{
             {QStringLiteral("hp"), 125}, {QStringLiteral("maxHp"), 300},
             {QStringLiteral("remainingRevives"), 2}, {QStringLiteral("quotaKnown"), true},
             {QStringLiteral("revivePending"), false}}},
        {QStringLiteral("blue"), QJsonObject{
             {QStringLiteral("hp"), 250}, {QStringLiteral("maxHp"), 300},
             {QStringLiteral("remainingRevives"), 1}, {QStringLiteral("quotaKnown"), true},
             {QStringLiteral("revivePending"), false}}},
    };
}
} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Windows offscreen does not always enumerate installed Chinese fonts.
    const QString fonts = qEnvironmentVariable("SystemRoot") + QStringLiteral("/Fonts/");
    if (!qEnvironmentVariable("SystemRoot").isEmpty()) {
        QFontDatabase::addApplicationFont(fonts + QStringLiteral("msyh.ttc"));
        QFontDatabase::addApplicationFont(fonts + QStringLiteral("msyhbd.ttc"));
        QFontDatabase::addApplicationFont(fonts + QStringLiteral("segoeui.ttf"));
        app.setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
    }
    RefereeEntryPanel panel;
    // Screenshot determinism: entrance / toast fades snap to the final frame.
    panel.setAnimationsEnabled(false);
    auto *winner = panel.findChild<QLineEdit *>("refereeWinner");
    auto *ending = panel.findChild<QLineEdit *>("refereeEndingMethod");
    auto *redHp = panel.findChild<QLineEdit *>("refereeRedHp");
    auto *blueHp = panel.findChild<QLineEdit *>("refereeBlueHp");
    auto *redRevives = panel.findChild<QLineEdit *>("refereeRedRevives");
    auto *blueRevives = panel.findChild<QLineEdit *>("refereeBlueRevives");
    auto *notes = panel.findChild<QPlainTextEdit *>("refereeNotes");
    auto *reason = panel.findChild<QPlainTextEdit *>("refereeReason");
    auto *copyText = panel.findChild<QPushButton *>("refereeCopyText");
    auto *copyJson = panel.findChild<QPushButton *>("refereeCopyJson");
    auto *warning = panel.findChild<QLabel *>("refereeWarning");
    auto *scroll = panel.findChild<QScrollArea *>("refereeScroll");
    VERIFY(winner && ending && redHp && blueHp && redRevives && blueRevives && notes && reason && copyText && copyJson && warning && scroll);
    VERIFY(panel.result().isEmpty() && !copyText->isEnabled() && !copyJson->isEnabled());
    VERIFY(redHp->text() == QStringLiteral("未提供"));
    for (auto *edit : panel.findChildren<QLineEdit *>()) VERIFY(edit->isReadOnly());
    VERIFY(reason->isReadOnly() && !notes->isReadOnly());

    const QJsonObject original = fixture();
    panel.setResult(original);
    VERIFY(panel.result() == original);
    VERIFY(winner->text() == QStringLiteral("红方 · 星火机器人战队"));
    VERIFY(ending->text() == QStringLiteral("时间判定"));
    VERIFY(redHp->text() == QStringLiteral("125") && blueHp->text() == QStringLiteral("250"));
    VERIFY(redRevives->text() == QStringLiteral("2") && blueRevives->text() == QStringLiteral("1"));
    VERIFY(panel.plainText().contains(QStringLiteral("本局用时：180.0 秒")));
    VERIFY(panel.plainText().contains(QStringLiteral("A-012-R2")));
    VERIFY(panel.plainText().contains(QStringLiteral("系列比分 1 : 0")));
    // A larger blue HP is not a UI-level reason to reverse the official winner.
    VERIFY(panel.result().value(QStringLiteral("winner")).toInt() == 1);
    notes->setPlainText(QStringLiteral("裁判已核对本局数据\n现场无额外违规"));
    VERIFY(panel.result().value(QStringLiteral("notes")).toString() == notes->toPlainText());
    VERIFY(!original.contains(QStringLiteral("notes")));
    copyText->click();
    VERIFY(QGuiApplication::clipboard()->text() == panel.plainText());
    copyJson->click();
    VERIFY(QGuiApplication::clipboard()->text() == panel.jsonText());
    VERIFY(panel.toastText().contains(QStringLiteral("JSON"))); // non-blocking toast, no QMessageBox

    // Single-field copy: bare value per card; chip and card (keyboard) both copy.
    struct Expect { RefereeEntryPanel::FieldId id; const char *chip; QString value; };
    const Expect expects[] = {
        {RefereeEntryPanel::WinnerField, "refereeCopyWinner", QStringLiteral("红方 · 星火机器人战队")},
        {RefereeEntryPanel::EndingField, "refereeCopyEnding", QStringLiteral("时间判定")},
        {RefereeEntryPanel::DurationField, "refereeCopyDuration", QStringLiteral("180.0")},
        {RefereeEntryPanel::RedHpField, "refereeCopyRedHp", QStringLiteral("125")},
        {RefereeEntryPanel::RedRevivesField, "refereeCopyRedRevives", QStringLiteral("2")},
        {RefereeEntryPanel::BlueHpField, "refereeCopyBlueHp", QStringLiteral("250")},
        {RefereeEntryPanel::BlueRevivesField, "refereeCopyBlueRevives", QStringLiteral("1")},
        {RefereeEntryPanel::ReasonField, "refereeCopyReason", QStringLiteral("时间到：红方剩余复活 2 次，蓝方 1 次，红方获胜")},
    };
    for (const Expect &e : expects) {
        auto *chip = panel.findChild<QPushButton *>(QString::fromLatin1(e.chip));
        VERIFY(chip && chip->isEnabled() && !chip->accessibleName().isEmpty());
        QGuiApplication::clipboard()->clear();
        chip->click();
        VERIFY(QGuiApplication::clipboard()->text() == e.value);
        VERIFY(panel.fieldCopyText(e.id) == e.value);
    }
    {
        auto *card = panel.findChild<QWidget *>(QStringLiteral("refereeCardRedHp"));
        VERIFY(card && card->focusPolicy() == Qt::StrongFocus && !card->accessibleName().isEmpty());
        QGuiApplication::clipboard()->clear();
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(card, &enter);
        VERIFY(QGuiApplication::clipboard()->text() == QStringLiteral("125"));
        QGuiApplication::clipboard()->clear();
        QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(card, &space);
        VERIFY(QGuiApplication::clipboard()->text() == QStringLiteral("125"));
        VERIFY(panel.findChild<QLabel *>(QStringLiteral("refereeCopiedRedHp"))->isVisibleTo(&panel));
    }

    // Re-applying the identical snapshot must not replay the entrance nor drop the note.
    {
        const int entrances = panel.entranceAnimationCount();
        const QString note = notes->toPlainText();
        panel.setResult(original);
        VERIFY(panel.entranceAnimationCount() == entrances);
        VERIFY(notes->toPlainText() == note);
        QJsonObject other = original;
        other.insert(QStringLiteral("durationMs"), 181000);
        panel.setResult(other);
        VERIFY(panel.entranceAnimationCount() == entrances + 1);
        panel.setResult(other);
        VERIFY(panel.entranceAnimationCount() == entrances + 1);
        // With animations on, a visible panel actually animates; finish hook stops it.
        panel.resize(1920, 1080);
        panel.show();
        panel.setAnimationsEnabled(true);
        panel.setResult(original);
        VERIFY(panel.isAnimating());
        panel.finishAnimationsForTest();
        VERIFY(!panel.isAnimating());
        panel.setAnimationsEnabled(false);
        notes->setPlainText(note);
    }
    const QJsonObject exported = QJsonDocument::fromJson(panel.jsonText().toUtf8()).object();
    VERIFY(exported.value(QStringLiteral("endingMethod")).toString() == QStringLiteral("time_decision"));
    VERIFY(exported.value(QStringLiteral("red")).toObject() == original.value(QStringLiteral("red")).toObject());
    VERIFY(exported.value(QStringLiteral("notes")).toString() == notes->toPlainText());

    const QString output = qEnvironmentVariable("SCOMP_REFEREE_QA_DIR");
    if (!output.isEmpty()) VERIFY(QDir().mkpath(output));
    for (const QSize size : {QSize(1920, 1080), QSize(720, 1280)}) {
        panel.resize(size);
        panel.show();
        scroll->verticalScrollBar()->setValue(0);
        settle(140);
        if (scroll->horizontalScrollBar()->maximum() != 0) {
            for (QWidget *w : scroll->widget()->findChildren<QWidget *>())
                if (w->isVisible() && w->minimumSizeHint().width() > size.width() - 120)
                    std::fprintf(stderr, "WIDE %s %s min=%d\n", w->metaObject()->className(), qPrintable(w->objectName()), w->minimumSizeHint().width());
        }
        VERIFY(scroll->horizontalScrollBar()->maximum() == 0);
        VERIFY(redHp->width() > 80 && blueHp->width() > 80);
        const auto *redCard = panel.findChild<QWidget *>("refereeRedStatsCard");
        const auto *blueCard = panel.findChild<QWidget *>("refereeBlueStatsCard");
        VERIFY(redCard && blueCard);
        if (size.width() < 860) VERIFY(blueCard->geometry().top() > redCard->geometry().bottom());
        else VERIFY(blueCard->geometry().left() > redCard->geometry().right());
        const QImage image = panel.grab().toImage();
        VERIFY(image.size() == size && !image.isNull());
        if (!output.isEmpty()) {
            VERIFY(image.save(output + QStringLiteral("/referee-entry-%1x%2.png").arg(size.width()).arg(size.height())));
            if (size.width() >= 1920)
                VERIFY(image.save(output + QStringLiteral("/referee-entry-timedecision-1920x1080.png")));
            if (size.width() < 860) {
                scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                settle();
                VERIFY(panel.grab().save(output + QStringLiteral("/referee-entry-%1x%2-lower.png").arg(size.width()).arg(size.height())));
            }
        }
        // 1920x1080: every field card and both copy buttons visible without scrolling.
        if (size.width() >= 1920) {
            VERIFY(scroll->verticalScrollBar()->maximum() == 0);
            for (const char *name : {"refereeCardWinner", "refereeCardEnding", "refereeCardDuration", "refereeCardRedHp",
                                     "refereeCardRedRevives", "refereeCardBlueHp", "refereeCardBlueRevives", "refereeCardReason"}) {
                auto *w = panel.findChild<QWidget *>(QString::fromLatin1(name));
                VERIFY(w && w->isVisible());
                VERIFY(QRect(QPoint(0, 0), size).contains(QRect(w->mapTo(&panel, QPoint(0, 0)), w->size())));
            }
            VERIFY(copyText->mapTo(&panel, QPoint(0, copyText->height())).y() <= size.height());
        }
        std::printf("SIZE %dx%d hp-width=%d/%d scroll-height=%d\n", size.width(), size.height(), redHp->width(), blueHp->width(), scroll->verticalScrollBar()->maximum());
    }

    // Single-item copy highlight screenshot (card flash + toast capsule).
    panel.resize(1920, 1080);
    scroll->verticalScrollBar()->setValue(0);
    settle();
    VERIFY(scroll->verticalScrollBar()->maximum() == 0); // narrow -> wide must fully restore
    panel.findChild<QPushButton *>(QStringLiteral("refereeCopyRedRevives"))->click();
    VERIFY(QGuiApplication::clipboard()->text() == QStringLiteral("2"));
    VERIFY(panel.findChild<QLabel *>(QStringLiteral("refereeCopiedRedRevives"))->isVisibleTo(&panel));
    VERIFY(panel.toastText().contains(QStringLiteral("红方剩余复活次数")));
    if (!output.isEmpty())
        VERIFY(panel.grab().save(output + QStringLiteral("/referee-entry-copied-1920x1080.png")));

    QJsonObject exhausted = original;
    exhausted.insert(QStringLiteral("reason"), QStringLiteral("revive_exhausted"));
    exhausted.insert(QStringLiteral("reasonText"), QStringLiteral("蓝方复活次数耗尽并阵亡，红方获胜"));
    exhausted.insert(QStringLiteral("blue"), QJsonObject{{QStringLiteral("hp"), 0}, {QStringLiteral("remainingRevives"), 0}, {QStringLiteral("quotaKnown"), true}});
    panel.setResult(exhausted);
    VERIFY(notes->toPlainText().isEmpty()); // New snapshot cannot retain an old note.
    VERIFY(ending->text() == QStringLiteral("耗尽复活"));
    VERIFY(blueHp->text() == QStringLiteral("0") && blueRevives->text() == QStringLiteral("0"));
    VERIFY(winner->text().startsWith(QStringLiteral("红方")));
    // A legitimate 0 is a real value: still copyable, not "未提供".
    VERIFY(panel.findChild<QPushButton *>(QStringLiteral("refereeCopyBlueRevives"))->isEnabled());
    VERIFY(panel.fieldCopyText(RefereeEntryPanel::BlueRevivesField) == QStringLiteral("0"));
    if (!output.isEmpty()) {
        panel.resize(1920, 1080);
        scroll->verticalScrollBar()->setValue(0);
        settle();
        VERIFY(panel.grab().save(output + QStringLiteral("/referee-entry-exhausted-1920x1080.png")));
    }

    for (const QString &code : {QStringLiteral("remaining_hp"), QStringLiteral("kill_score")}) {
        QJsonObject timeout = original;
        timeout.insert(QStringLiteral("reason"), code);
        panel.setResult(timeout);
        VERIFY(ending->text() == QStringLiteral("时间判定"));
    }
    QJsonObject explicitMethod = original;
    explicitMethod.insert(QStringLiteral("endingMethod"), QStringLiteral("violation"));
    panel.setResult(explicitMethod);
    VERIFY(ending->text() == QStringLiteral("违规判负")); // Explicit protocol field has priority.
    if (!output.isEmpty()) {
        QJsonObject violation = original;
        violation.insert(QStringLiteral("winner"), 2);
        violation.insert(QStringLiteral("reason"), QStringLiteral("red_card"));
        violation.insert(QStringLiteral("reasonText"), QStringLiteral("红方红牌违规判负，蓝方获胜"));
        violation.insert(QStringLiteral("durationMs"), 96500);
        violation.insert(QStringLiteral("redSeriesWins"), 0);
        violation.insert(QStringLiteral("blueSeriesWins"), 1);
        panel.setResult(violation);
        VERIFY(ending->text() == QStringLiteral("违规判负") && winner->text().startsWith(QStringLiteral("蓝方")));
        settle();
        VERIFY(panel.grab().save(output + QStringLiteral("/referee-entry-violation-1920x1080.png")));
    }
    explicitMethod.insert(QStringLiteral("endingMethod"), QStringLiteral("future_unknown_code"));
    panel.setResult(explicitMethod);
    VERIFY(ending->text() == QStringLiteral("未提供"));
    explicitMethod.insert(QStringLiteral("endingMethod"), QJsonValue(QJsonValue::Null));
    panel.setResult(explicitMethod);
    VERIFY(ending->text() == QStringLiteral("未提供"));

    QJsonObject stopped = original;
    stopped.insert(QStringLiteral("winner"), 0);
    stopped.insert(QStringLiteral("reason"), QStringLiteral("terminated"));
    stopped.insert(QStringLiteral("settlement"), QStringLiteral("termination"));
    stopped.remove(QStringLiteral("reasonText"));
    panel.setResult(stopped);
    VERIFY(winner->text().contains(QStringLiteral("无胜方")));
    VERIFY(ending->text().contains(QStringLiteral("裁判终止")));
    VERIFY(reason->toPlainText().contains(QStringLiteral("不代表物理断电")));
    copyText->click();
    VERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("请勿抄录为任一方获胜")));
    copyJson->click();
    VERIFY(QJsonDocument::fromJson(QGuiApplication::clipboard()->text().toUtf8()).object().value(QStringLiteral("transcriptionWarning")).toString().contains(QStringLiteral("未产生胜方")));
    if (!output.isEmpty()) {
        panel.resize(1920, 1080);
        scroll->verticalScrollBar()->setValue(0);
        settle();
        VERIFY(panel.grab().save(output + QStringLiteral("/referee-entry-terminated-1920x1080.png")));
    }

    QJsonObject missing = original;
    missing.insert(QStringLiteral("valid"), false);
    missing.insert(QStringLiteral("reasonText"), QStringLiteral("资料未完整，等待裁判核实；本局胜方尚未判定"));
    missing.insert(QStringLiteral("winner"), 2); // A winner without valid=true must remain pending.
    missing.insert(QStringLiteral("red"), QJsonObject{{QStringLiteral("hp"), -1}, {QStringLiteral("remainingRevives"), 3}, {QStringLiteral("quotaKnown"), false}});
    missing.insert(QStringLiteral("blue"), QJsonObject{{QStringLiteral("hp"), QJsonValue(QJsonValue::Null)}, {QStringLiteral("remainingRevives"), true}});
    missing.remove(QStringLiteral("durationMs"));
    panel.setResult(missing);
    VERIFY(winner->text() == QStringLiteral("未判定"));
    VERIFY(reason->toPlainText() == QStringLiteral("资料未完整，等待裁判核实；本局胜方尚未判定"));
    VERIFY(redHp->text() == QStringLiteral("未提供") && blueHp->text() == QStringLiteral("未提供"));
    VERIFY(redRevives->text() == QStringLiteral("未提供") && blueRevives->text() == QStringLiteral("未提供"));
    VERIFY(panel.plainText().contains(QStringLiteral("本局用时：未提供")));
    VERIFY(panel.jsonText().contains(QStringLiteral("\"hp\": -1")));
    VERIFY(panel.result().value(QStringLiteral("blue")).toObject().value(QStringLiteral("hp")).isNull());
    VERIFY(panel.plainText().contains(QStringLiteral("不得把此副本")));
    // Missing values: chip disabled; keyboard/card copy yields the literal 未提供 + warning toast.
    for (const char *name : {"refereeCopyRedHp", "refereeCopyBlueHp", "refereeCopyRedRevives", "refereeCopyBlueRevives", "refereeCopyDuration"})
        VERIFY(!panel.findChild<QPushButton *>(QString::fromLatin1(name))->isEnabled());
    VERIFY(panel.findChild<QPushButton *>(QStringLiteral("refereeCopyReason"))->isEnabled());
    {
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Enter, Qt::NoModifier);
        QApplication::sendEvent(panel.findChild<QWidget *>(QStringLiteral("refereeCardBlueRevives")), &enter);
        VERIFY(QGuiApplication::clipboard()->text() == QStringLiteral("未提供"));
        VERIFY(panel.toastText().contains(QStringLiteral("不是 0")));
    }
    settle();
    // Even with the warning banner, 1920x1080 must fit without scrolling.
    VERIFY(scroll->verticalScrollBar()->maximum() == 0);
    if (!output.isEmpty())
        VERIFY(panel.grab().save(output + QStringLiteral("/referee-entry-unknown-1920x1080.png")));

    QJsonObject pending = original;
    QJsonObject redPending = pending.value(QStringLiteral("red")).toObject();
    redPending.insert(QStringLiteral("revivePending"), true);
    pending.insert(QStringLiteral("red"), redPending);
    panel.setResult(pending);
    VERIFY(panel.plainText().contains(QStringLiteral("复活指令待确认")));
    VERIFY(redRevives->text() == QStringLiteral("2")); // UI must not consume or replenish quotas.
    for (const QJsonValue &bad : {QJsonValue(-1), QJsonValue(1.25), QJsonValue(true), QJsonValue(QStringLiteral("0"))}) {
        QJsonObject malformed = original;
        malformed.insert(QStringLiteral("red"), QJsonObject{{QStringLiteral("hp"), bad}, {QStringLiteral("remainingRevives"), bad}});
        panel.setResult(malformed);
        VERIFY(redHp->text() == QStringLiteral("未提供") && redRevives->text() == QStringLiteral("未提供"));
    }
    panel.clearResult();
    VERIFY(panel.result().isEmpty() && notes->toPlainText().isEmpty());
    VERIFY(winner->text() == QStringLiteral("未判定") && redHp->text() == QStringLiteral("未提供"));
    VERIFY(!copyText->isEnabled() && !copyJson->isEnabled() && !notes->isEnabled());
    VERIFY(!panel.plainText().contains(QStringLiteral("星火")));
    VERIFY(panel.plainText().contains(QStringLiteral("待本局结束")));
    panel.setResult(QJsonObject{{QStringLiteral("valid"), true}, {QStringLiteral("winner"), 0}});
    VERIFY(winner->text() == QStringLiteral("未判定") && ending->text() == QStringLiteral("未提供"));
    VERIFY(panel.result().value(QStringLiteral("red")).isUndefined());
    panel.close();
    std::puts("PASS: read-only referee snapshot, timeout/exhaustion/termination, unknown telemetry, no re-adjudication, reset isolation, clipboard text/JSON and responsive real widget screenshots.");
    return 0;
}
