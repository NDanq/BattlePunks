#ifndef NIMBLEENGINE_PLUGINCATALOG_H
#define NIMBLEENGINE_PLUGINCATALOG_H

#include <map>
#include <string>
#include <vector>

#include "Public/NimbleEngine/NimbleEngineAPI.h"

/*
 * Every plugin in the search folders, read from its descriptor, and what a
 * configure would make of them -- Engine/cmake/NimblePlugins.cmake's rules,
 * in C++, for the tools that show plugins without configuring: the editor's
 * Project Settings > Plugins page and New Plugin dialog.
 *
 * The CMake rules are the authority; these follow them, message for message
 * (Engine/Tests/PluginCatalogTests.cpp runs Plugins.Resolve's cases). Where
 * they disagree, the build is right and this is the bug.
 */
namespace NimbleEngine::PluginCatalog
{
    struct ModuleInfo
    {
        std::string Name;
        std::string Type;   // Runtime, Editor, ClientOnly, ServerOnly
    };

    struct Dependency
    {
        std::string Name;
        std::string MinVersion;   // empty: any
        bool        bOptional = false;
    };

    struct PluginInfo
    {
        std::string Name;
        std::string Version;
        std::string EngineVersion;
        std::string Category;
        std::string Description;
        bool        bEnabledByDefault = false;
        std::vector<ModuleInfo> Modules;
        std::vector<Dependency> Dependencies;

        std::string Dir;            // the plugin's folder
        std::string Root;           // the search folder it was found in
        std::string Overrides;      // a same-named plugin's folder this one hides, if any
        bool        bHasContent = false;
        bool        bHasShaders = false;   // Content/Shaders or Content/Materials
        bool        bShadersCooked = false;   // NimbleCook's Runtime stamp is there

        /* Why the descriptor will not configure; empty if it will. Such a
         * plugin stops the configure even disabled: every descriptor found is read. */
        std::string Error;
    };

    /* What enabling a set of plugins would come to. */
    struct Resolution
    {
        std::vector<std::string> Order;     // enabled, each after what it needs: load order
        std::string              Error;     // the configure's FATAL_ERROR, if any; Order is then partial
        std::vector<std::string> Warnings;  // [Plugins] lines naming nothing found, bad values
    };

    /* Every <root>/<Name>/<Name>.nmplugin, the first root that has a name
     * keeping it, roots in order (project, NIMBLE_PLUGIN_DIRS, Engine/Plugins). */
    NE_API std::vector<PluginInfo> Find(const std::vector<std::string>& InRoots);

    /* InInfo read from InFile; Error set if it is malformed. */
    NE_API PluginInfo ReadDescriptor(const std::string& InFile);

    /* [Plugins] Key=Value lines of a config file's text, keys as written. */
    NE_API std::vector<std::pair<std::string, std::string>> ReadPluginsSection(const std::string& InConfigText);

    /* Whether InInfo is enabled under InSection (ReadPluginsSection): its
     * line if it has one, else its default. Names match without case. */
    NE_API bool IsEnabled(const PluginInfo& InInfo, const std::vector<std::pair<std::string, std::string>>& InSection);

    /* The configure's decision for InFound under InSection, against
     * InEngineVersion (GetEngineVersion() for this build's). */
    NE_API Resolution Resolve(const std::vector<PluginInfo>& InFound,
                              const std::vector<std::pair<std::string, std::string>>& InSection,
                              const std::string& InEngineVersion);

    /* Empty if InName can name a new plugin, else why not: the descriptor's
     * rule (a letter, then letters or digits, two at least) and no clash with
     * InTaken (other plugins, engine modules, targets), without case. */
    NE_API std::string ValidateName(const std::string& InName, const std::vector<std::string>& InTaken);

    /* Dotted versions compared numerically: <0, 0, >0. "1.2" == "1.2.0". */
    NE_API int CompareVersions(const std::string& InA, const std::string& InB);

    /* This engine's version (Engine/CMakeLists.txt's project()). */
    NE_API const char* GetEngineVersion();
}

#endif //NIMBLEENGINE_PLUGINCATALOG_H
