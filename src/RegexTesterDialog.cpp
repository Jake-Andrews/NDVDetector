#include "RegexTesterDialog.h"
#include <QRegularExpression>
#include <QVBoxLayout>

RegexTesterDialog::RegexTesterDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Regex / Glob Tester"));
    resize(420, 190);

    m_globCheck = new QCheckBox(tr("Pattern is a glob ( *  ? )"), this);
    m_caseCheck = new QCheckBox(tr("Case‑insensitive (i)"), this);
    m_patternEdit = new QLineEdit(this);
    m_testEdit = new QLineEdit(this);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("font-weight:bold");

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(tr("Pattern:"), this));
    lay->addWidget(m_patternEdit);
    lay->addWidget(m_globCheck);
    lay->addWidget(m_caseCheck);
    lay->addSpacing(6);
    lay->addWidget(new QLabel(tr("Test text:"), this));
    lay->addWidget(m_testEdit);
    lay->addSpacing(6);
    lay->addWidget(m_statusLabel);

    connect(m_globCheck, &QCheckBox::toggled, this, &RegexTesterDialog::onGlobToggled);
    connect(m_caseCheck, &QCheckBox::toggled, this, &RegexTesterDialog::onCaseToggled);
    connect(m_patternEdit, &QLineEdit::textChanged, this, &RegexTesterDialog::onPatternChanged);
    connect(m_testEdit, &QLineEdit::textChanged, this, &RegexTesterDialog::onTestTextChanged);

    updateMatchState();
}

QString RegexTesterDialog::globToRegex(QString const& glob)
{
    QString rx;
    rx.reserve(glob.size() * 2);
    rx += '^';
    for (QChar c : glob) {
        switch (c.unicode()) {
        case '*':
            rx += ".*";
            break;
        case '?':
            rx += '.';
            break;
        case '.':
            rx += "\\.";
            break;
        case '\\':
            rx += "\\\\";
            break;
        case '+':
        case '(':
        case ')':
        case '{':
        case '}':
        case '^':
        case '$':
        case '|':
        case '[':
        case ']':
            rx += '\\';
            rx += c;
            break;
        default:
            rx += c;
        }
    }
    rx += '$';
    return rx;
}

void RegexTesterDialog::onGlobToggled(bool) { updateMatchState(); }
void RegexTesterDialog::onCaseToggled(bool) { updateMatchState(); }
void RegexTesterDialog::onPatternChanged() { updateMatchState(); }
void RegexTesterDialog::onTestTextChanged() { updateMatchState(); }

void RegexTesterDialog::updateMatchState()
{
    QString pattern = m_patternEdit->text();
    if (m_globCheck->isChecked())
        pattern = globToRegex(pattern);

    QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
    if (m_caseCheck->isChecked())
        opts |= QRegularExpression::CaseInsensitiveOption;

    QRegularExpression re(pattern, opts);
    if (!re.isValid()) {
        m_statusLabel->setText(tr("❌  Invalid pattern: %1").arg(re.errorString()));
        m_statusLabel->setStyleSheet("color:red;font-weight:bold");
        return;
    }

    QString test = m_testEdit->text();
    bool matched = re.match(test).hasMatch();
    m_statusLabel->setText(matched ? tr("✅  Match") : tr("✗  No match"));
    m_statusLabel->setStyleSheet(matched ? "color:green;font-weight:bold" : "color:grey;font-weight:bold");
}

