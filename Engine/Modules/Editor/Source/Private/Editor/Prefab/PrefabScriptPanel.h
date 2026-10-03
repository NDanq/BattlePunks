#ifndef NIMBLEEDITOR_PREFABSCRIPTPANEL_H
#define NIMBLEEDITOR_PREFABSCRIPTPANEL_H

#if WITH_EDITOR

#include <QString>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;

/*
 * The Prefab Editor's Script dock, for the prefab's script (a runtime path):
 *
 *   .lua             a text editor on the file, saved with the prefab
 *   .script.nmgraph  the path, and a button that opens it in the Graph Editor
 *                    (a graph saves and stages there, like any graph)
 *   none             a note on how to add one
 */
class PrefabScriptPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PrefabScriptPanel(QWidget* InParent = nullptr);

    /* Shows InRuntimePath's script, reading a Lua file from the project.
     * Unsaved Lua text is dropped: save first. */
    void SetScript(const QString& InRuntimePath);

    /* Lua text edited since it was read or saved. */
    bool IsModified() const;

    /* Writes edited Lua text to its file and stages it. True if there was
     * nothing to write. */
    bool Save(QString& OutError);

    /* Stages the Lua script for a run: edited text to the STAGED copy only, so
     * a simulation runs what is on screen without touching the project file;
     * otherwise the file as it is. */
    void StageForRun();

    /* What a new prefab's Lua script starts as. */
    static QString StarterLua(const QString& InPrefabName);

signals:
    /* IsModified() may have changed. */
    void ModifiedChanged();

private:
    QString        RuntimePath;
    QString        FilePath;      // the project file, for Lua
    QLabel*        PathLabel  = nullptr;
    QStackedWidget* Pages     = nullptr;
    QLabel*        NoneNote   = nullptr;
    QPlainTextEdit* LuaEditor = nullptr;
    QPushButton*   OpenGraphButton = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PREFABSCRIPTPANEL_H
