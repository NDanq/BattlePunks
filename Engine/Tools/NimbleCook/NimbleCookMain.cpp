/*
 * NimbleCook: cooks what the build only stages. The engine's shaders, and
 * every material graph's permutations, compiled for each GDI backend -- the
 * build never runs a shader compiler (Engine/CMakeLists.txt, "Cooked
 * shaders"; Engine/cmake/NimbleMaterials.cmake).
 *
 *   NimbleCook shaders [-engine <Engine dir>] [-project <dir> [-module <name>]]
 *                      [-target <OpenGL|Vulkan|D3D12>]... [-editor] [-shipping]
 *                      [-force] [-python <exe>]
 *
 * The engine always; with -project, that project's Content/Materials too.
 * -module names the project's shader cache (default: its
 * Config/DefaultEngine.ini [Game] Module). Each is cooked below its own
 * <root>/Saved/Cooked/Shaders/, laid out as the runtime reads it:
 *
 *   Runtime/   every build stages it beside the executable (or in its pack)
 *       Shaders/Cooked/<Platform>/...            the engine's plain shaders
 *       Shaders/Cache/<Module>.<Platform>.nmsc   the material permutations
 *   Editor/    -editor: an Editor build stages it too -- loose materials,
 *              which the editor's own on-save cook overwrites
 *       Shaders/Cooked/<Platform>/<...>/<Name>@<key>.<stage>
 *       <...>/<Name>.material
 *   Shipping/  -shipping: a packaged Shipping build stages it over Runtime/
 *       Shaders/Cache/<Module>.<Platform>.nmsc   the same, naming nothing (-hash-paths)
 *
 * and a <Variant>.stamp beside each when it cooked: what the build stages
 * after. A variant whose stamp is newer than every input, for the same
 * targets, is skipped (-force cooks it anyway).
 *
 * The targets default to this build's GDI backends. The compiling --
 * preprocessing, permutation keys, glslc, spirv-cross, fxc -- is
 * NodeGraph's cook_material_graphs.py through GDI's cook_shaders.py, run
 * once per variant and platform.
 *
 * Exits 0 when everything cooked or was up to date, 1 if anything failed,
 * 2 on bad arguments.
 */

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    struct Options
    {
        fs::path                 EngineDir;
        fs::path                 ProjectDir;
        std::string              Module;
        std::vector<std::string> Targets;
        std::string              Python;
        bool                     bEditor   = false;
        bool                     bShipping = false;
        bool                     bForce    = false;
    };

    /* One cook_material_graphs.py run's arguments, less -target. */
    using Args = std::vector<std::string>;

    std::string ToUtf8(const fs::path& InPath)
    {
        const auto Text = InPath.generic_u8string();
        return std::string(Text.begin(), Text.end());
    }

    std::string Trim(const std::string& InText)
    {
        const size_t First = InText.find_first_not_of(" \t\r\n");
        if(First == std::string::npos) { return std::string(); }
        const size_t Last = InText.find_last_not_of(" \t\r\n");
        return InText.substr(First, Last - First + 1);
    }

    /* [InSection] InKey from an .ini, or empty. */
    std::string ReadIni(const fs::path& InFile, const std::string& InSection, const std::string& InKey)
    {
        std::ifstream File(InFile);
        std::string Line;
        bool bInSection = false;
        while(std::getline(File, Line))
        {
            Line = Trim(Line);
            if(Line.empty() || Line[0] == ';' || Line[0] == '#') { continue; }
            if(Line[0] == '[')
            {
                bInSection = Line == "[" + InSection + "]";
                continue;
            }
            const size_t Equals = Line.find('=');
            if(bInSection && Equals != std::string::npos && Trim(Line.substr(0, Equals)) == InKey)
            {
                return Trim(Line.substr(Equals + 1));
            }
        }
        return std::string();
    }

    /* The newest modification time of InPath, or of any file below it. */
    fs::file_time_type Newest(const fs::path& InPath)
    {
        std::error_code Error;
        fs::file_time_type Time = fs::file_time_type::min();
        if(fs::is_regular_file(InPath, Error))
        {
            return fs::last_write_time(InPath, Error);
        }
        if(!fs::is_directory(InPath, Error)) { return Time; }
        for(fs::recursive_directory_iterator It(InPath, Error), End; It != End; It.increment(Error))
        {
            if(Error) { break; }
            if(It->is_regular_file(Error))
            {
                const fs::file_time_type Each = It->last_write_time(Error);
                if(!Error && Each > Time) { Time = Each; }
            }
        }
        return Time;
    }

    std::string JoinTargets(const std::vector<std::string>& InTargets)
    {
        std::string Text;
        for(const std::string& Target : InTargets) { Text += (Text.empty() ? "" : " ") + Target; }
        return Text;
    }

    /* Whether InStamp records InTargets and is newer than every input. */
    bool IsUpToDate(const fs::path& InStamp, const std::vector<std::string>& InTargets, const std::vector<fs::path>& InInputs)
    {
        std::error_code Error;
        if(!fs::is_regular_file(InStamp, Error)) { return false; }
        std::ifstream File(InStamp);
        std::string Line;
        std::getline(File, Line);
        if(Trim(Line) != JoinTargets(InTargets)) { return false; }
        const fs::file_time_type Stamped = fs::last_write_time(InStamp, Error);
        for(const fs::path& Input : InInputs)
        {
            if(Newest(Input) > Stamped) { return false; }
        }
        return true;
    }

    /* InArg quoted for the shell, if it needs it. */
    std::string Quote(const std::string& InArg)
    {
        if(!InArg.empty() && InArg.find_first_of(" \t\"&|<>^()") == std::string::npos) { return InArg; }
        std::string Quoted = "\"";
        for(const char Char : InArg)
        {
            if(Char == '"') { Quoted += '\\'; }
            Quoted += Char;
        }
        return Quoted + "\"";
    }

    /* InArg with "{out}" (a variant's folder) and "{platform}" (the target) filled in. */
    std::string Expand(std::string InArg, const std::string& InOut, const std::string& InPlatform)
    {
        for(const auto& [Key, Value] : { std::pair<std::string, std::string>{ "{out}", InOut }, { "{platform}", InPlatform } })
        {
            for(size_t At = InArg.find(Key); At != std::string::npos; At = InArg.find(Key, At + Value.size()))
            {
                InArg.replace(At, Key.size(), Value);
            }
        }
        return InArg;
    }

    int Run(const std::vector<std::string>& InCommand)
    {
        std::string Line;
        for(const std::string& Arg : InCommand) { Line += (Line.empty() ? "" : " ") + Quote(Arg); }
#ifdef _WIN32
        Line = "\"" + Line + "\"";   // cmd /c strips the outer pair, keeping the quotes inside
#endif
        std::fflush(stdout);
        return std::system(Line.c_str());
    }

    struct Cooker
    {
        const Options&           Opts;
        fs::path                 Script;
        std::vector<fs::path>    CommonInputs;   // every cook's: the engine's shaders and includes, the scripts
        std::vector<std::string> Failures;
        int                      Cooked  = 0;
        int                      Skipped = 0;

        /* Variant InName below InRoot: InRuns once per target, after clearing
         * what an earlier cook left. */
        void CookVariant(const std::string& InWhat, const fs::path& InRoot, const std::string& InName,
                         const std::vector<fs::path>& InInputs, const std::vector<Args>& InRuns)
        {
            const fs::path Out   = InRoot / InName;
            const fs::path Stamp = InRoot / (InName + ".stamp");
            std::vector<fs::path> Inputs = CommonInputs;
            Inputs.insert(Inputs.end(), InInputs.begin(), InInputs.end());
            if(!Opts.bForce && IsUpToDate(Stamp, Opts.Targets, Inputs))
            {
                std::printf("NimbleCook: %s %s is up to date\n", InWhat.c_str(), InName.c_str());
                ++Skipped;
                return;
            }

            std::printf("NimbleCook: cooking %s %s (%s) into %s\n", InWhat.c_str(), InName.c_str(),
                        JoinTargets(Opts.Targets).c_str(), ToUtf8(Out).c_str());
            std::error_code Error;
            fs::remove(Stamp, Error);
            fs::remove_all(Out, Error);   // a deleted material's permutations go too
            fs::create_directories(Out, Error);

            bool bOk = true;
            for(const std::string& Target : Opts.Targets)
            {
                for(const Args& Each : InRuns)
                {
                    std::vector<std::string> Command{ Opts.Python, ToUtf8(Script), "-target", Target };
                    for(const std::string& Arg : Each)
                    {
                        Command.push_back(Expand(Arg, ToUtf8(Out), Target));
                    }
                    if(Run(Command) != 0)
                    {
                        Failures.push_back(InWhat + " " + InName + " for " + Target);
                        bOk = false;
                    }
                }
            }
            if(!bOk) { return; }

            std::ofstream File(Stamp, std::ios::trunc);
            File << JoinTargets(Opts.Targets) << "\n";
            ++Cooked;
        }

        void CookEngine()
        {
            const fs::path Shaders   = Opts.EngineDir / "Content" / "Shaders";
            const fs::path Materials = Opts.EngineDir / "Content" / "Materials";
            const std::string Parent = ToUtf8(Opts.EngineDir.parent_path());   // "Engine/Content/..." below it
            const fs::path Root      = Opts.EngineDir / "Saved" / "Cooked" / "Shaders";
            const Args Defaults{ "-src", ToUtf8(Materials), "-root", Parent, "-subdir", "Materials", "-defaults" };

            const std::vector<Args> Plain{ { "-src", ToUtf8(Shaders), "-out", "{out}/Shaders/Cooked" } };
            CookVariant("Engine", Root, "Runtime", { Materials }, WithCache(Plain, Defaults, "Engine", false));
            if(Opts.bEditor)
            {
                Args Loose = Defaults;
                Loose.insert(Loose.end(), { "-out", "{out}/Shaders/Cooked", "-data-out", "{out}" });
                CookVariant("Engine", Root, "Editor", { Materials }, { Loose });
            }
            if(Opts.bShipping)
            {
                CookVariant("Engine", Root, "Shipping", { Materials }, WithCache({}, Defaults, "Engine", true));
            }
        }

        void CookProject()
        {
            const fs::path Materials = Opts.ProjectDir / "Content" / "Materials";
            std::error_code Error;
            if(!fs::is_directory(Materials, Error))
            {
                std::printf("NimbleCook: %s has no Content/Materials; no project shaders to cook\n", ToUtf8(Opts.ProjectDir).c_str());
                return;
            }
            const fs::path Root = Opts.ProjectDir / "Saved" / "Cooked" / "Shaders";
            const Args Source{ "-src", ToUtf8(Materials), "-root", ToUtf8(Opts.ProjectDir) };
            const std::string What = Opts.Module.empty() ? std::string("Project") : Opts.Module;

            // Without a module there is no cache to cook: only an editor's loose files.
            if(!Opts.Module.empty())
            {
                CookVariant(What, Root, "Runtime", { Materials }, WithCache({}, Source, Opts.Module, false));
            }
            else
            {
                std::printf("NimbleCook: %s names no [Game] Module; its materials are cooked loose only\n",
                            ToUtf8(Opts.ProjectDir).c_str());
            }
            if(Opts.bEditor)
            {
                Args Loose = Source;
                Loose.insert(Loose.end(), { "-out", "{out}/Shaders/Cooked", "-data-out", "{out}" });
                CookVariant(What, Root, "Editor", { Materials }, { Loose });
            }
            if(Opts.bShipping && !Opts.Module.empty())
            {
                CookVariant(What, Root, "Shipping", { Materials }, WithCache({}, Source, Opts.Module, true));
            }
        }

        /* InRuns plus InSource cooked into {out}/Shaders/Cache/<InModule>.<Platform>.nmsc. */
        std::vector<Args> WithCache(std::vector<Args> InRuns, const Args& InSource, const std::string& InModule, const bool bHashPaths)
        {
            Args Cache = InSource;
            Cache.insert(Cache.end(), { "-cache", "{out}/Shaders/Cache/" + InModule + ".{platform}.nmsc", "-module", InModule });
            if(bHashPaths) { Cache.push_back("-hash-paths"); }
            InRuns.push_back(Cache);
            return InRuns;
        }
    };
}

int main(int argc, char** argv)
{
    if(argc < 2 || std::string(argv[1]) != "shaders")
    {
        std::fprintf(stderr,
                     "Usage: NimbleCook shaders [-engine <dir>] [-project <dir> [-module <name>]]\n"
                     "                          [-target <OpenGL|Vulkan|D3D12>]... [-editor] [-shipping] [-force] [-python <exe>]\n");
        return 2;
    }

    Options Opts;
    Opts.EngineDir = fs::u8path(NIMBLECOOK_ENGINE_DIR);
    Opts.Python    = NIMBLECOOK_PYTHON;
    for(int Index = 2; Index < argc; ++Index)
    {
        const std::string Name = argv[Index];
        const bool bHasValue   = Index + 1 < argc;
        if(Name == "-editor")        { Opts.bEditor = true; }
        else if(Name == "-shipping") { Opts.bShipping = true; }
        else if(Name == "-force")    { Opts.bForce = true; }
        else if(bHasValue && Name == "-engine")  { Opts.EngineDir = fs::u8path(argv[++Index]); }
        else if(bHasValue && Name == "-project") { Opts.ProjectDir = fs::u8path(argv[++Index]); }
        else if(bHasValue && Name == "-module")  { Opts.Module = argv[++Index]; }
        else if(bHasValue && Name == "-python")  { Opts.Python = argv[++Index]; }
        else if(bHasValue && Name == "-target")
        {
            const std::string Target = argv[++Index];
            if(Target != "OpenGL" && Target != "Vulkan" && Target != "D3D12")
            {
                std::fprintf(stderr, "NimbleCook: unknown -target %s (OpenGL, Vulkan or D3D12)\n", Target.c_str());
                return 2;
            }
            Opts.Targets.push_back(Target);
        }
        else
        {
            std::fprintf(stderr, "NimbleCook: unknown argument %s\n", Name.c_str());
            return 2;
        }
    }

    if(Opts.Targets.empty())
    {
        const std::string Built = NIMBLECOOK_TARGETS;   // this build's GDI backends, ","-separated
        size_t Start = 0;
        while(Start < Built.size())
        {
            size_t End = Built.find(',', Start);
            if(End == std::string::npos) { End = Built.size(); }
            if(End > Start) { Opts.Targets.push_back(Built.substr(Start, End - Start)); }
            Start = End + 1;
        }
    }
    if(Opts.Targets.empty())
    {
        std::printf("NimbleCook: no shader targets (this build has no OpenGL, Vulkan or D3D12 backend); nothing to cook\n");
        return 0;
    }
    if(Opts.Python.empty()) { Opts.Python = "python"; }

    std::error_code Error;
    Opts.EngineDir = fs::absolute(Opts.EngineDir, Error);
    const fs::path Script = Opts.EngineDir / "Modules" / "NodeGraph" / "Scripts" / "cook_material_graphs.py";
    if(!fs::is_regular_file(Script, Error))
    {
        std::fprintf(stderr, "NimbleCook: %s is not the Engine folder (no %s)\n", ToUtf8(Opts.EngineDir).c_str(), ToUtf8(Script).c_str());
        return 2;
    }
    if(!Opts.ProjectDir.empty())
    {
        Opts.ProjectDir = fs::absolute(Opts.ProjectDir, Error);
        if(Opts.Module.empty())
        {
            Opts.Module = ReadIni(Opts.ProjectDir / "Config" / "DefaultEngine.ini", "Game", "Module");
        }
    }

    Cooker Cook{ Opts };
    Cook.Script       = Script;
    Cook.CommonInputs = { Opts.EngineDir / "Content" / "Shaders", Script,
                          Opts.EngineDir / "Modules" / "GDI" / "Scripts" / "cook_shaders.py" };
    Cook.CookEngine();
    if(!Opts.ProjectDir.empty()) { Cook.CookProject(); }

    std::printf("NimbleCook: %d cooked, %d up to date, %d failed\n", Cook.Cooked, Cook.Skipped, int(Cook.Failures.size()));
    for(const std::string& Failure : Cook.Failures)
    {
        std::printf("NimbleCook: FAILED %s\n", Failure.c_str());
    }
    return Cook.Failures.empty() ? 0 : 1;
}
