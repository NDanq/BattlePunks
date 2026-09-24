#if WITH_EDITOR

#include "Private/Editor/AboutDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtGlobal>

#include "EditorBuildInfo.h"

/* NIMBLE_BUILD_CONFIG and NIMBLE_BUILD_TARGET come from the Editor's
 * CMakeLists.txt as bare tokens (Debug, Editor), set on this file only. */
#define NIMBLE_ABOUT_STRINGIFY2(X) #X
#define NIMBLE_ABOUT_STRINGIFY(X)  NIMBLE_ABOUT_STRINGIFY2(X)

namespace
{
    QString CompilerDescription()
    {
#if defined(__clang__)
        return QStringLiteral("Clang %1.%2.%3").arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
#elif defined(_MSC_VER)
        /* _MSC_FULL_VER is MMNNBBBBB: 194033807 -> 19.40.33807. */
        return QStringLiteral("MSVC %1.%2.%3")
            .arg(_MSC_FULL_VER / 10000000)
            .arg((_MSC_FULL_VER / 100000) % 100)
            .arg(_MSC_FULL_VER % 100000);
#elif defined(__GNUC__)
        return QStringLiteral("GCC %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#else
        return QStringLiteral("unknown");
#endif
    }
}

QList<QPair<QString, QString>> AboutDialog::CollectBuildInfo(const QString& InRendererName)
{
    const int BuildNumber = NIMBLEEDITOR_BUILD_NUMBER;

    QList<QPair<QString, QString>> Rows;
    Rows << qMakePair(QStringLiteral("Version"),       QStringLiteral(NIMBLEEDITOR_VERSION));
    Rows << qMakePair(QStringLiteral("Commit"),        QStringLiteral(NIMBLEEDITOR_GIT_DESCRIBE));
    Rows << qMakePair(QStringLiteral("SHA"),           QStringLiteral(NIMBLEEDITOR_GIT_SHA));
    Rows << qMakePair(QStringLiteral("Branch"),        QStringLiteral(NIMBLEEDITOR_GIT_BRANCH));
    Rows << qMakePair(QStringLiteral("Commit date"),   QStringLiteral(NIMBLEEDITOR_GIT_COMMIT_DATE));
    Rows << qMakePair(QStringLiteral("Working tree"),  NIMBLEEDITOR_GIT_DIRTY ? QStringLiteral("uncommitted changes")
                                                                              : QStringLiteral("clean"));
    Rows << qMakePair(QStringLiteral("Build"),         BuildNumber > 0 ? QStringLiteral("#%1").arg(BuildNumber)
                                                                       : QStringLiteral("local"));
    Rows << qMakePair(QStringLiteral("Configuration"), QStringLiteral(NIMBLE_ABOUT_STRINGIFY(NIMBLE_BUILD_CONFIG)));
    Rows << qMakePair(QStringLiteral("Target"),        QStringLiteral(NIMBLE_ABOUT_STRINGIFY(NIMBLE_BUILD_TARGET)));
    Rows << qMakePair(QStringLiteral("Compiler"),      CompilerDescription());
    Rows << qMakePair(QStringLiteral("Qt"),            QStringLiteral("%1 (built against %2)")
                                                           .arg(QString::fromLatin1(qVersion()),
                                                                QStringLiteral(QT_VERSION_STR)));
    Rows << qMakePair(QStringLiteral("Renderer"),      InRendererName);
    return Rows;
}

AboutDialog::AboutDialog(const QString& InRendererName, QWidget* InParent)
    : QDialog(InParent)
{
    setWindowTitle(QStringLiteral("About NimbleEditor"));
    setAttribute(Qt::WA_DeleteOnClose);

    auto* Layout = new QVBoxLayout(this);

    auto* Title = new QLabel(QStringLiteral("<span style='font-size:14pt; font-weight:bold;'>NimbleEditor</span>"
                                            "<br><span style='color:gray'>%1</span>")
                                 .arg(QStringLiteral(NIMBLEEDITOR_VERSION_STRING).toHtmlEscaped()),
                             this);
    Title->setTextFormat(Qt::RichText);
    Layout->addWidget(Title);

    const QList<QPair<QString, QString>> Rows = CollectBuildInfo(InRendererName);

    auto* Form = new QFormLayout();
    Form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    for(const QPair<QString, QString>& Row : Rows)
    {
        auto* Value = new QLabel(Row.second, this);
        Value->setTextInteractionFlags(Qt::TextSelectableByMouse);   // a SHA is copied more than read
        Form->addRow(Row.first + QLatin1Char(':'), Value);
    }
    Layout->addLayout(Form);

    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton* Copy = Buttons->addButton(QStringLiteral("&Copy to Clipboard"), QDialogButtonBox::ActionRole);
    connect(Copy, &QPushButton::clicked, this, [Rows]()
    {
        QStringList Lines;
        Lines << QStringLiteral("NimbleEditor %1").arg(QStringLiteral(NIMBLEEDITOR_VERSION_STRING));
        for(const QPair<QString, QString>& Row : Rows)
        {
            Lines << QStringLiteral("%1: %2").arg(Row.first, Row.second);
        }
        QApplication::clipboard()->setText(Lines.join(QLatin1Char('\n')));
    });
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    Layout->addWidget(Buttons);

    Layout->setSizeConstraint(QLayout::SetFixedSize);
}

#endif // WITH_EDITOR
