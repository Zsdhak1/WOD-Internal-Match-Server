#ifndef MATCHTIMINGDIALOG_H
#define MATCHTIMINGDIALOG_H

#include "matchtimingpresets.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimeEdit;

// Secondary editor for the match timing presets: add, rename, change the
// round / intermission length, delete and restore the built-in presets.
// Edits are local until 保存; validation failures keep the dialog open.
class MatchTimingDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MatchTimingDialog(const QVector<MatchTimingPreset> &presets,
                               const QString &current, QWidget *parent = nullptr);
    QVector<MatchTimingPreset> presets() const { return m_presets; }
    QString currentName() const;

private slots:
    void onRowChanged(int row);
    void onFieldsEdited();
    void onAdd();
    void onRemove();
    void onRestoreDefaults();
    void onAccept();

private:
    void rebuildList(int selectRow);
    void loadFields(int row);
    MatchTimingPreset fieldsPreset() const;

    QVector<MatchTimingPreset> m_presets;
    QListWidget *m_list = nullptr;
    QLineEdit *m_name = nullptr;
    QTimeEdit *m_round = nullptr;
    QTimeEdit *m_intermission = nullptr;
    QPushButton *m_remove = nullptr;
    QLabel *m_error = nullptr;
    bool m_loading = false;
};

#endif
