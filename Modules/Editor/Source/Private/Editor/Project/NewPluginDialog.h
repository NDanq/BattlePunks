#ifndef NIMBLEEDITOR_NEWPLUGINDIALOG_H
#define NIMBLEEDITOR_NEWPLUGINDIALOG_H

#if WITH_EDITOR

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

/*
 * New Plugin (Project Settings > Plugins, File > New Plugin): a plugin made
 * from a starter in Engine/Templates/Plugin by EditorPlugins::CreatePlugin.
 *
 * The parts people get wrong by hand are not offered: the folder, file and
 * descriptor name match, the module's section matches its CMake target, the
 * export header, NIMBLE_IMPLEMENT_PLUGIN and the prefixed shader name all
 * come from the one Name. It is checked as it is typed against the
 * descriptor's rule and every name in use; the files it will write are
 * listed before anything is written.
 */
class NewPluginDialog : public QDialog
{
public:
    /* InLocations: the search folders it may go in, InProjectDir (the
     * project's Plugins) first and preselected. InTaken: names in use. */
    NewPluginDialog(const QStringList& InLocations, const QString& InProjectDir, const QStringList& InTaken, QWidget* InParent = nullptr);

    /* A name and starter filled in (the console's plugin.newdialog). */
    void Prefill(const QString& InName, const QString& InStarter);

    /* After Accepted: the plugin made. */
    const QString& GetCreatedName() const { return CreatedName; }
    const QString& GetCreatedDir() const  { return CreatedDir; }

private:
    void Update();
    void Create();

    QString     TemplateDir;
    QStringList Taken;

    QLineEdit*      Name        = nullptr;
    QLabel*         NameCheck   = nullptr;
    QComboBox*      Location    = nullptr;
    QComboBox*      Starter     = nullptr;
    QLabel*         StarterNote = nullptr;
    QComboBox*      Category    = nullptr;
    QLineEdit*      Description = nullptr;
    QCheckBox*      Test        = nullptr;
    QCheckBox*      Content     = nullptr;
    QCheckBox*      Editor      = nullptr;
    QPlainTextEdit* Preview     = nullptr;
    QPushButton*    CreateButton = nullptr;

    QString CreatedName;
    QString CreatedDir;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_NEWPLUGINDIALOG_H
