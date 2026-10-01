#ifndef NIMBLEEDITOR_ABOUTDIALOG_H
#define NIMBLEEDITOR_ABOUTDIALOG_H

#if WITH_EDITOR

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

/*
 * Help > About: the build's commit, version, configuration, compiler, Qt and
 * renderer, with Copy to Clipboard for bug reports. Git values are stamped at
 * build time (EditorBuildInfo.h.in).
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
