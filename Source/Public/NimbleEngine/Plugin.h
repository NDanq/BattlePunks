#ifndef NIMBLEENGINE_PLUGIN_H
#define NIMBLEENGINE_PLUGIN_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreModule.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"

namespace GameCore
{
    class World;
}

namespace RenderCore
{
    class Renderer;
}

/*
 * PLUGINS: modules a project drops in (Engine/cmake/NimblePlugins.cmake says
 * how they are found, enabled and built). At run time a plugin is
 *
 *   * its libraries, opened once, before the game module, and closed when
 *     the process is done with them -- never reloaded: component types and
 *     script libraries register for the life of the process;
 *   * its files: Config/Default<Name>.ini under the project's config, and
 *     Content/ readable ONLY as "<Name>:/<path>" (strict: an unprefixed path
 *     never reaches a plugin's content, and a prefix no enabled plugin owns
 *     is an error, logged once);
 *   * its shaders (Content/Shaders, Content/Materials), cooked by NimbleCook
 *     into a cache of its own, Shaders/Cache/<Name>.<Platform>.nmsc: the
 *     only unprefixed path a plugin answers, mounted on the renderer;
 *   * an IPlugin per library, whose OnStartup registers what cannot register
 *     itself -- systems on each level's World (PluginContext::AddWorldHook).
 *     Components, script libraries and config vars still register themselves
 *     as the library loads (GAMECORE_DEFINE_COMPONENT and friends).
 *
 * The build writes Plugins/Plugins.manifest beside the executable: the
 * enabled plugins in load order, each with its libraries and, outside
 * Shipping, its source folder. The host reads it in three steps:
 *
 *     Plugins::Discover(RuntimeDir, bUseSource)   before the project's config
 *     Plugins::MountContent(Assets)               once the asset provider exists
 *     Plugins::LoadModules()                      after config, before the game
 *     Plugins::MountShaderCaches(Renderer)        once the renderer is up
 *     ...
 *     Plugins::UnmountShaderCaches(Renderer)      before it shuts down
 *     Plugins::ShutdownModules()                  after the game module unloads
 *     Plugins::Close()                            last
 *
 * Main thread only.
 */
namespace NimbleEngine
{
    /* Called for every level World a GameSession makes (after the engine's
     * own systems attach, before the level's OnLoad) and, in reverse order,
     * as it tears one down (before the engine's detach). */
    struct PluginWorldHook
    {
        const char* Name = "";
        void (*Attach)(GameCore::World& InWorld, void* InUserData) = nullptr;
        void (*Detach)(GameCore::World& InWorld, void* InUserData) = nullptr;
        void* UserData = nullptr;
    };

    /* What a plugin's OnStartup is handed: the host's registries, scoped to
     * that plugin so the host can undo them after OnShutdown. */
    class NE_API PluginContext
    {
    public:
        const std::string& GetPluginName() const { return PluginName; }
        const std::string& GetVersion()    const { return Version; }

        /* Every level World made from now on. Undone by the host after
         * OnShutdown; a hook never outlives its plugin. */
        void AddWorldHook(const PluginWorldHook& InHook);

        /* One of the plugin's own files by its path in the plugin folder --
         * "Config/DefaultTimeOfDay.ini", "TimeOfDay.nmplugin". Content is
         * read as "<Name>:/..." through the asset provider like any asset. */
        bool ReadPluginFile(const std::string& InPath, std::vector<uint8_t>& OutBytes) const;

    private:
        friend struct PluginHostAccess;
        std::string                    PluginName;
        std::string                    Version;
        std::shared_ptr<EngineCore::IFileProvider> Files;
    };

    /*
     * DELIBERATELY NOT NE_API, for IModule's reason (CoreModule.h): the
     * derived class lives in the plugin's library and so does its vtable.
     * Its layout is frozen by NIMBLE_PLUGIN_ABI_VERSION: changing a virtual
     * here bumps PLUGIN_ABI_REVISION.
     */
    class IPlugin
    {
    public:
        virtual ~IPlugin() = default;

        /* False refuses the plugin: logged, its other libraries still load,
         * and nothing it registered stays. */
        virtual bool OnStartup(PluginContext& InContext) = 0;

        /* Unregister whatever OnStartup registered by hand. World hooks are
         * removed by the host. */
        virtual void OnShutdown(PluginContext& InContext) {}
    };

    /* The load gate: the module ABI's build flags (SIMD, debug CRT, ...),
     * WITH_EDITOR, which changes class layouts, and this interface's own
     * revision. A library reporting anything else is refused. */
    enum : uint32_t
    {
        PLUGIN_ABI_REVISION    = 1,
        PLUGIN_ABI_FLAG_EDITOR = 1u << 20
    };
    constexpr uint32_t NIMBLE_PLUGIN_ABI_VERSION =
          (EngineCore::NIMBLE_MODULE_ABI_VERSION & 0xFFFF0000u)
        | uint32_t(PLUGIN_ABI_REVISION)
#if WITH_EDITOR
        | uint32_t(PLUGIN_ABI_FLAG_EDITOR)
#endif
        ;

    /* Permanent names, as CoreModule.h's: version through the integer. */
    constexpr const char* PLUGIN_SYMBOL_ABI_VERSION = "NimblePluginGetABIVersion";
    constexpr const char* PLUGIN_SYMBOL_CREATE      = "NimblePluginCreate";
    constexpr const char* PLUGIN_SYMBOL_DESTROY     = "NimblePluginDestroy";
    using PluginGetABIVersionFn = uint32_t (*)();
    using PluginCreateFn        = IPlugin* (*)();
    using PluginDestroyFn       = void     (*)(IPlugin*);

    /*
     * "<Name>:/<path>" -> InInner's "Content/<path>"; every other path is not
     * this provider's (false). What makes plugin content strict: mounted as a
     * layer, it answers nothing unprefixed, and nothing of another plugin's.
     */
    class NE_API PrefixFileProvider : public EngineCore::IFileProvider
    {
    public:
        PrefixFileProvider(std::string InName, std::shared_ptr<EngineCore::IFileProvider> InInner);

        bool ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes) override;
        bool Exists (const std::string& InPath) override;
        bool ReadRange(const std::string& InPath, uint64_t InOffset, uint64_t InSize, std::vector<uint8_t>& OutBytes) override;

        /* InPath's path inside the plugin folder, if it carries this prefix. */
        bool Resolve(const std::string& InPath, std::string& OutInner) const;

    private:
        std::string                                Prefix;   // "<Name>:/"
        std::shared_ptr<EngineCore::IFileProvider> Inner;
    };

    /* "<Name>:/..." -> "<Name>", for a path that has a plugin prefix (a letter
     * then letters or digits, two at least, then ":/"); empty for any other --
     * "Content/x", "C:/x". */
    NE_API std::string GetPluginPrefix(const std::string& InPath);

    namespace Plugins
    {
        /* Reads <InRuntimeDir>/Plugins/Plugins.manifest ("" = the working
         * directory) and each plugin's Config/Default<Name>.ini, so the
         * project's config, loaded after, wins. bInUseSource reads each
         * plugin's files from its source folder (the editor, -uncooked)
         * rather than what the build staged or packed. Returns how many
         * plugins the manifest lists; a missing manifest is none. Again
         * replaces what an earlier call found. */
        NE_API int Discover(const std::string& InRuntimeDir, bool bInUseSource);

        /* Each discovered plugin's content on InAssets as layer
         * "Plugin:<Name>" (PrefixFileProvider), its shader cache as
         * "PluginShaders:<Name>" (that one file, from a newer cook in its
         * source folder where there is one), and the strict-prefix check
         * under them. */
        NE_API void MountContent(EngineCore::IFileProvider& InAssets);
        NE_API void UnmountContent(EngineCore::IFileProvider& InAssets);

        /* Each plugin with shaders: its cache on InRenderer
         * (Renderer::MountShaderCache), so its custom passes' shaders and
         * its materials load. After MountContent. */
        NE_API void MountShaderCaches(RenderCore::Renderer& InRenderer);
        NE_API void UnmountShaderCaches(RenderCore::Renderer& InRenderer);

        /* Where LoadModules opens each library from: a copy of it in a folder
         * of this run's below InDir, so a build can replace the original while
         * this process runs -- the editor's, whose Compile Source rebuilds
         * plugins (Windows locks a loaded DLL; its PDB too, which goes
         * along). Empty (the default): the library itself, as a game does.
         * Before LoadModules. Other runs' leftover folders are removed where
         * they can be. A library a game module links finds the copy already
         * loaded, by its name. */
        NE_API void SetShadowCopyDir(const std::string& InDir);

        /* Opens every discovered plugin's libraries in load order and starts
         * each IPlugin. False if any library failed (logged); the rest still
         * load. */
        NE_API bool LoadModules();

        /* OnShutdown, in reverse order, and removes what they registered
         * through their contexts. The libraries stay open. */
        NE_API void ShutdownModules();

        /* Closes the libraries and forgets everything Discover found. */
        NE_API void Close();

        /* [Plugins] in the loaded config against what this build has --
         * the project enabling a plugin since the build was configured, or
         * the reverse -- one warning each: rebuild to apply. Claims the
         * section's lines, so they are not reported as unknown config.
         * After the project's config loads. True if it warned. */
        NE_API bool WarnConfigMismatch();

        /* [Plugins] lines the build does not match, without warning: Wanted
         * what the line says, and the build has the plugin or not. */
        struct ConfigMismatch
        {
            std::string Name;
            bool        bWanted = false;
        };
        NE_API std::vector<ConfigMismatch> GetConfigMismatches();

        /* The folders the build looked for plugins in, in order (the
         * manifest's [Search]; empty in Shipping), and the engine version it
         * checked them against. */
        NE_API const std::vector<std::string>& GetSearchRoots();
        NE_API const std::string&              GetBuiltEngineVersion();

        /* Whether this build has InName (the manifest lists it), and why it
         * did not start if it did not (empty if it did, or was not loaded). */
        NE_API bool        IsBuilt(const std::string& InName);
        NE_API std::string GetLoadProblem(const std::string& InName);

        NE_API size_t             GetCount();
        NE_API const std::string& GetNameAt(size_t InIndex);
        NE_API bool               IsLoaded(const std::string& InName);

        /* GameSession's: every plugin's world hooks on InWorld. */
        NE_API void AttachWorld(GameCore::World& InWorld);
        NE_API void DetachWorld(GameCore::World& InWorld);
    }
}

/*
 * In ONE .cpp of each plugin library, at file scope, outside any namespace:
 *
 *     NIMBLE_IMPLEMENT_PLUGIN(TimeOfDayPlugin)
 */
#define NIMBLE_IMPLEMENT_PLUGIN(ClassName)                                                 \
    extern "C" NIMBLE_MODULE_API uint32_t NimblePluginGetABIVersion()                      \
    {                                                                                      \
        return ::NimbleEngine::NIMBLE_PLUGIN_ABI_VERSION;                                  \
    }                                                                                      \
    extern "C" NIMBLE_MODULE_API ::NimbleEngine::IPlugin* NimblePluginCreate()             \
    {                                                                                      \
        return new ClassName();                                                            \
    }                                                                                      \
    extern "C" NIMBLE_MODULE_API void NimblePluginDestroy(::NimbleEngine::IPlugin* InPlugin) \
    {                                                                                      \
        delete InPlugin;                                                                   \
    }

#endif //NIMBLEENGINE_PLUGIN_H
