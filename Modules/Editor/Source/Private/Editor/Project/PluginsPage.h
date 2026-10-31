#ifndef NIMBLEEDITOR_PLUGINSPAGE_H
#define NIMBLEEDITOR_PLUGINSPAGE_H

#if WITH_EDITOR

#include <map>
#include <vector>

#include <QString>
#include <QStringList>
#include <QWidget>

#include "Public/NimbleEngine/PluginCatalog.h"

class ConfigIniFile;
class QLabel;
class QTableWidget;

/*
 * Project Settings > Plugins: every plugin in the build's search folders
 * (Plugins::GetSearchRoots, the manifest's), enabled or not, with what this
 * editor has of it, its details and a checkbox that edits [Plugins].
 *
 * Plugins are built, not loaded, so a change applies at the next build and
 * the restart after it: Apply writes [Plugins], and Project Settings offers
 * the build (EditorMainWindow::RebuildForPlugins). Enabling a plugin offers
 * to enable what it needs; disabling one, what needs it.
 *
 * The descriptors are read and judged by PluginCatalog, the C++ copy of
 * NimblePlugins.cmake's rules: a configure that would stop says so here
 * first, with the same message.
 */
class PluginsPage : public QWidget
{
public:
    /* InFile: the project's DefaultEngine.ini as Project Settings has it,
     * for [Plugins]; InConfigPath where it lives (the project's folder). */
    PluginsPage(const ConfigIniFile& InFile, const QString& InConfigPath, QWidget* InParent = nullptr);

    /* [Plugins] lines for every plugin switched since the last Apply, set in
     * InOutFile, and in this process's config (the status bar's notice);
     * "Plugins.<Name> = true" for each. */
    QStringList Apply(ConfigIniFile& InOutFile);

    /* File > New Plugin: the dialog, prefilled where given. The plugin it
     * makes is listed, checked and selected; Apply enables it. */
    void OpenNewPluginDialog(const QString& InName = QString(), const QString& InStarter = QString());

    /* The search folders read again. Switches not applied are kept. */
    void Refresh();

    /* The folder new project plugins go in: <project>/Plugins. */
    QString GetProjectPluginsDir() const;

    /* Every name a new plugin cannot take: those found, the engine's modules, the game module. */
    QStringList GetTakenNames() const;

    /* The search folders, the project's first. */
    const QStringList& GetRoots() const { return Roots; }

private:
    void Fill();
    void ShowDetails();
    void OnToggled(int InRow, bool bInOn);
    /* [Plugins] as it would be with the switches: for PluginCatalog. */
    std::vector<std::pair<std::string, std::string>> PendingSection() const;
    const NimbleEngine::PluginCatalog::PluginInfo* FindPlugin(const QString& InName) const;
    bool IsOn(const QString& InName) const;
    QString LocationOf(const NimbleEngine::PluginCatalog::PluginInfo& InInfo) const;
    QString StatusOf(const NimbleEngine::PluginCatalog::PluginInfo& InInfo, QString* OutTip = nullptr) const;

    QString                                              ConfigPath;
    QStringList                                          Roots;
    std::vector<std::pair<std::string, std::string>>     FileSection;   // [Plugins] as the file has it
    std::vector<NimbleEngine::PluginCatalog::PluginInfo> Found;
    std::map<QString, bool>                              Switches;      // lowercased name -> switched to (only those touched)
    QString                                              Selected;

    QLabel*       Summary = nullptr;
    QTableWidget* Table   = nullptr;
    QLabel*       Details = nullptr;
    bool          bFilling = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PLUGINSPAGE_H
