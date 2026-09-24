#ifndef NIMBLEEDITOR_ABOUTDIALOG_H
#define NIMBLEEDITOR_ABOUTDIALOG_H

#if WITH_EDITOR

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

/*
 * Help > About: which build this is -- the commit it was built from (git
 * describe, full SHA, branch, commit date, whether the tree had uncommitted
 * changes), the version and CI build number, and the configuration, target,
 * compiler, Qt and renderer it runs with. Copy to Clipboard puts the same
 * lines on the clipboard for a bug report.
 *
 * The git values are stamped at build time (EditorBuildInfo.h.in), so they
 * describe the source the binary was built from, not the checkout it is run
 * beside.
 */
class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    /* InRendererName: the backend the viewport runs on, e.g. "Vulkan". */
    explicit AboutDialog(const QString& InRendererName, QWidget* InParent = nullptr);

    /* Label/value rows, in display order -- also what Copy produces. */
    static QList<QPair<QString, QString>> CollectBuildInfo(const QString& InRendererName);
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_ABOUTDIALOG_H
