#ifndef REFEREEENTRYPANEL_H
#define REFEREEENTRYPANEL_H

#include <QJsonObject>
#include <QString>
#include <QWidget>

class QFrame;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QTimer;
class QVariantAnimation;

// Local, read-only settlement inspector for transcription into an external
// results system. Does not adjudicate, persist, send network data, or own any
// MatchReferee/RobotManager. The only editable field is an optional local note.
// No Q_OBJECT: helper widgets are plain overrides and animations are driven by
// QVariantAnimation lambdas, so no generated moc file is required.
class RefereeEntryPanel : public QWidget
{
public:
    // Individually copyable transcription fields (one data card each).
    enum FieldId {
        WinnerField,
        EndingField,
        DurationField,
        RedHpField,
        RedRevivesField,
        BlueHpField,
        BlueRevivesField,
        ReasonField,
        FieldCount
    };

    explicit RefereeEntryPanel(QWidget *parent = nullptr);
    // Re-applying an identical snapshot is a no-op (no animation replay, the
    // referee's unsaved note is kept). A different snapshot resets the note.
    void setResult(const QJsonObject &summary);
    void clearResult();
    // Original snapshot plus current optional notes; missing telemetry remains
    // missing (never converted to zero). Calling clearResult() returns {}.
    QJsonObject result() const;
    QString plainText() const;
    QString jsonText() const;
    QString entryText() const { return plainText(); }

    // Value placed on the clipboard when a single field card is copied.
    QString fieldCopyText(FieldId id) const;
    void copyField(FieldId id);

    // Test / QA hooks for deterministic screenshots.
    void setAnimationsEnabled(bool enabled);
    bool animationsEnabled() const { return m_animationsEnabled; }
    void finishAnimationsForTest();
    int entranceAnimationCount() const { return m_entranceCount; }
    bool isAnimating() const;
    // Current non-blocking copy toast text; empty when the toast is hidden.
    QString toastText() const;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    class DataCard;
    class ReviveMeter;
    class HpBar;
    class Curtain;
    class Toast;

    struct Field {
        DataCard *card = nullptr;
        QLabel *title = nullptr;
        QLabel *unit = nullptr;
        QLabel *note = nullptr;
        QLabel *copiedBadge = nullptr;
        QPushButton *copy = nullptr;
        QLineEdit *value = nullptr; // null for the reason card (QPlainTextEdit)
    };

    void buildHeader(QWidget *content, QGridLayout *layout);
    void buildFields(QWidget *content);
    void buildActions(QWidget *content);
    Field makeField(FieldId id, const QString &objectSuffix, const QString &title,
                    const QString &accessible, QWidget *parent);
    void updateLayoutForWidth(int width);
    void refreshResult();
    void playEntrance();
    void applyEntranceProgress(double progress);
    void showToast(const QString &text, bool warn);
    void flashField(FieldId id);
    void clearFlash();
    QString resolvedEndingMethod() const;
    QString endingMethodText() const;
    QString winnerText() const;
    QString reasonText() const;
    QString warningText() const;
    QString seriesText() const;
    void copyEntryText();
    void copyJson();

    QJsonObject m_result;
    bool m_narrow = false;
    bool m_resultApplied = false;
    bool m_animationsEnabled = true;
    int m_entranceCount = 0;
    int m_flashedField = -1;

    Field m_fields[FieldCount];
    QGridLayout *m_mainGrid = nullptr;
    QGridLayout *m_matchupLayout = nullptr;
    QGridLayout *m_fieldGrid = nullptr;
    QWidget *m_fieldHost = nullptr;
    QFrame *m_redTeamCard = nullptr;
    QFrame *m_blueTeamCard = nullptr;
    QWidget *m_matchCenter = nullptr;
    QFrame *m_redStatsCard = nullptr;
    QFrame *m_blueStatsCard = nullptr;
    QFrame *m_notesCard = nullptr;
    QWidget *m_actionBar = nullptr;
    QLabel *m_matchLabel = nullptr;
    QLabel *m_roundLabel = nullptr;
    QLabel *m_seriesLabel = nullptr;
    QLabel *m_seriesScore = nullptr;
    QLabel *m_verdictPill = nullptr;
    QLabel *m_redWinBadge = nullptr;
    QLabel *m_blueWinBadge = nullptr;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_warningLabel = nullptr;
    QLabel *m_copyStatus = nullptr;
    Toast *m_toast = nullptr;
    QVariantAnimation *m_toastFade = nullptr;
    QLineEdit *m_redName = nullptr;
    QLineEdit *m_blueName = nullptr;
    QLabel *m_redMaxHp = nullptr;
    QLabel *m_blueMaxHp = nullptr;
    HpBar *m_redHpBar = nullptr;
    HpBar *m_blueHpBar = nullptr;
    ReviveMeter *m_redMeter = nullptr;
    ReviveMeter *m_blueMeter = nullptr;
    QPlainTextEdit *m_reason = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QPushButton *m_copyTextButton = nullptr;
    QPushButton *m_copyJsonButton = nullptr;
    Curtain *m_curtain = nullptr;
    QVariantAnimation *m_entrance = nullptr;
    QTimer *m_toastTimer = nullptr;
    QTimer *m_flashTimer = nullptr;
};

#endif // REFEREEENTRYPANEL_H
