#include "Public/NimbleEngine/ShaderCook.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreLog.h"

#if defined(_WIN32)
    #define NM_POPEN  _popen
    #define NM_PCLOSE _pclose
#else
    #define NM_POPEN  popen
    #define NM_PCLOSE pclose
#endif

namespace NimbleEngine::ShaderCook
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string ToUtf8(const fs::path& InPath)
        {
            const auto Text = InPath.generic_u8string();
            return std::string(Text.begin(), Text.end());
        }

        std::string Quote(const std::string& InArg)
        {
            return "\"" + InArg + "\"";
        }

        /* InFrom's files copied below InTo where newer or missing; how many. */
        int CopyNewer(const fs::path& InFrom, const fs::path& InTo)
        {
            int Copied = 0;
            std::error_code Error;
            if(!fs::is_directory(InFrom, Error)) { return 0; }
            for(fs::recursive_directory_iterator It(InFrom, Error), End; It != End; It.increment(Error))
            {
                if(Error) { break; }
                if(!It->is_regular_file(Error)) { continue; }
                const fs::path To = InTo / fs::relative(It->path(), InFrom, Error);
                std::error_code Missing;
                if(fs::exists(To, Missing) && fs::last_write_time(To, Missing) >= It->last_write_time(Error)) { continue; }
                fs::create_directories(To.parent_path(), Error);
                if(fs::copy_file(It->path(), To, fs::copy_options::overwrite_existing, Error))
                {
                    ++Copied;
                }
                else
                {
                    CORE_LOG_WARN("[Shaders] could not copy " + ToUtf8(It->path()) + " to " + ToUtf8(To) + ": " + Error.message())
                }
            }
            return Copied;
        }
    }

    std::string GetEngineSourceDir()
    {
#if defined(NIMBLE_ENGINE_SOURCE_DIR)
        return NIMBLE_ENGINE_SOURCE_DIR;
#else
        return std::string();
#endif
    }

    std::string GetCookedShaderDir(const std::string& InRoot)
    {
        return ToUtf8(fs::u8path(InRoot) / "Saved" / "Cooked" / "Shaders");
    }

    bool IsCooked(const std::string& InRoot, const char* InVariant)
    {
        std::error_code Error;
        return !InRoot.empty() && fs::is_regular_file(fs::u8path(GetCookedShaderDir(InRoot)) / (std::string(InVariant) + ".stamp"), Error);
    }

    std::string FindCooker(const std::string& InDir)
    {
#if defined(_WIN32)
        const fs::path Cooker = fs::u8path(InDir) / "NimbleCook.exe";
#else
        const fs::path Cooker = fs::u8path(InDir) / "NimbleCook";
#endif
        std::error_code Error;
        return fs::is_regular_file(Cooker, Error) ? ToUtf8(Cooker) : std::string();
    }

    bool RunCooker(const std::string& InCooker, const std::string& InEngineDir, const std::string& InProjectDir)
    {
        std::string Command = Quote(InCooker) + " shaders -engine " + Quote(InEngineDir);
        if(!InProjectDir.empty())
        {
            Command += " -project " + Quote(InProjectDir);
        }
        Command += " 2>&1";
#if defined(_WIN32)
        Command = "\"" + Command + "\"";   // cmd /c strips the outer pair, keeping the quotes inside
#endif
        CORE_LOG("[Shaders] " + Command)
        FILE* Pipe = NM_POPEN(Command.c_str(), "r");
        if(!Pipe)
        {
            CORE_LOG_ERROR("[Shaders] could not start " + InCooker)
            return false;
        }
        char Line[1024];
        while(std::fgets(Line, sizeof(Line), Pipe))
        {
            std::string Text = Line;
            while(!Text.empty() && (Text.back() == '\n' || Text.back() == '\r')) { Text.pop_back(); }
            if(!Text.empty()) { CORE_LOG("[Shaders] " + Text) }
        }
        return NM_PCLOSE(Pipe) == 0;
    }

    int MountCookedShaders(EngineCore::LayeredFileProvider& InAssets, const std::string& InEngineDir, const std::string& InProjectDir)
    {
        int Mounted = 0;
        const std::pair<const char*, std::string> Roots[] = { { "CookedShaders.Engine", InEngineDir }, { "CookedShaders.Project", InProjectDir } };
        for(const auto& [Name, Root] : Roots)
        {
            if(!IsCooked(Root, "Runtime")) { continue; }
            const std::string Dir = ToUtf8(fs::u8path(GetCookedShaderDir(Root)) / "Runtime");
            if(InAssets.MountLayer(Name, std::make_shared<EngineCore::FilesystemProvider>(Dir)))
            {
                ++Mounted;
            }
        }
        return Mounted;
    }

    int RestageCookedShaders(const std::string& InRuntimeDir, const std::string& InEngineDir, const std::string& InProjectDir,
                             const bool bInEditor)
    {
        int Copied = 0;
        for(const std::string& Root : { InEngineDir, InProjectDir })
        {
            if(Root.empty()) { continue; }
            const fs::path Cooked = fs::u8path(GetCookedShaderDir(Root));
            Copied += CopyNewer(Cooked / "Runtime", fs::u8path(InRuntimeDir));
            if(bInEditor)
            {
                Copied += CopyNewer(Cooked / "Editor", fs::u8path(InRuntimeDir));
            }
        }
        return Copied;
    }
}
