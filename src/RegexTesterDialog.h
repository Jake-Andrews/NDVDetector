#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>

class RegexTesterDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RegexTesterDialog(QWidget* parent = nullptr);
    ~RegexTesterDialog() override = default;

    static QString globToRegex(const QString& glob);

private slots:
    void onGlobToggled(bool);
    void onCaseToggled(bool);
    void onPatternChanged();
    void onTestTextChanged();

private:
    void updateMatchState();

    QCheckBox  *m_globCheck{}, *m_caseCheck{};
    QLineEdit  *m_patternEdit{}, *m_testEdit{};
    QLabel     *m_statusLabel{};
};

