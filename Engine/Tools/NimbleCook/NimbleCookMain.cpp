/*
 * NimbleCook: cooks what the build only stages. The engine's shaders, and
 * every material graph's permutations, compiled for each GDI backend -- the
 * build never runs a shader compiler (Engine/CMakeLists.txt, "Cooked
 * shaders"; Engine/cmake/NimbleMaterials.cmake).
 *
 *   NimbleCook shaders [-engine <Engine dir>] [-project <dir> [-module <name>]]
 *                      [-target <OpenGL|Vulkan|D3D12>]... [-editor] [-shipping]
 *                      [-force] [-jobs <N>] [-python <exe>]
 *
 * The engine always; with -project, that project's Content/Materials too, and
 * its Content/Shaders (plain shaders: custom render passes), by file name.
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
 * once per variant and platform. -jobs (default 32) is how many shaders
 * compile at once in all: ceil(jobs / 4) runs go at a time -- the slowest,
 * D3D12, first -- each compiling jobs / that many on its own threads (the
 * script's -jobs). Overlapping them keeps the cores busy while one run waits
 * on its last slow shader. -jobs 1 cooks one shader at a time. Each run's
 * lines are printed together when it is done, after a
 * "NimbleCook: [<done>/<runs>] done: <what> <variant> for <platform>" line.
 *
 * Exits 0 when everything cooked or was up to date, 1 if anything failed,
 * 2 on bad arguments.
 */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #define NM_POPEN  _popen
    #define NM_PCLOSE _pclose
#else
    #define NM_POPEN  popen
    #define NM_PCLOSE pclose
#endif

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
        int                      Jobs      = 32;   // shaders compiling at once, across every run
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

    /* InCommand run to its end, everything it printed (stdout and stderr)
     * into OutOutput. Its exit code; -1 if it did not start. */
    int RunCaptured(const std::vector<std::string>& InCommand, std::string& OutOutput)
    {
        std::string Line;
        for(const std::string& Arg : InCommand) { Line += (Line.empty() ? "" : " ") + Quote(Arg); }
        Line += " 2>&1";
#ifdef _WIN32
        Line = "\"" + Line + "\"";   // cmd /c strips the outer pair, keeping the quotes inside
#endif
        FILE* Pipe = NM_POPEN(Line.c_str(), "r");
        if(!Pipe)
        {
            OutOutput = "could not start: " + Line + "\n";
            return -1;
        }
        char Buffer[4096];
        size_t Read;
        while((Read = std::fread(Buffer, 1, sizeof(Buffer), Pipe)) > 0)
        {
            OutOutput.append(Buffer, Read);
        }
        return NM_PCLOSE(Pipe);
    }

    /* The slowest platform first, so its long runs start while the cores are free. */
    int PlatformOrder(const std::string& InTarget)
    {
        return InTarget == "D3D12" ? 0 : InTarget == "Vulkan" ? 1 : 2;
    }

    struct Cooker
    {
        /* One variant being cooked: its stamp is written once all its runs are. */
        struct Variant
        {
            fs::path         Stamp;
            std::atomic<int> Failed{ 0 };
        };

        /* One cook_material_graphs.py run. */
        struct Job
        {
            std::string              Label;   // "Engine Runtime for Vulkan"
            std::vector<std::string> Command;
            size_t                   Variant;
            int                      Order;
        };

        const Options&                        Opts;
        fs::path                              Script;
        std::vector<fs::path>                 CommonInputs;   // every cook's: the engine's shaders and includes, the scripts
        std::vector<std::unique_ptr<Variant>> Variants;
        std::vector<Job>                      Jobs;
        std::vector<std::string>              Failures;
        int                                   Cooked  = 0;
        int                                   Skipped = 0;

        /* Variant InName below InRoot: InRuns once per target, queued for Run
         * after clearing what an earlier cook left. */
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

            Variants.push_back(std::make_unique<Variant>());
            Variants.back()->Stamp = Stamp;
            for(const std::string& Target : Opts.Targets)
            {
                for(const Args& Each : InRuns)
                {
                    const bool bCache = std::find(Each.begin(), Each.end(), "-cache") != Each.end();
                    Job Queued{ InWhat + " " + InName + (bCache && InRuns.size() > 1 ? " cache" : "") + " for " + Target,
                                { Opts.Python, ToUtf8(Script), "-target", Target }, Variants.size() - 1, PlatformOrder(Target) };
                    for(size_t Index = 0; Index < Each.size(); ++Index)
                    {
                        /* The material descriptions are the same for every
                         * platform: written by the first one's run alone, as
                         * the runs go at once. */
                        if(Each[Index] == "-data-out" && Target != Opts.Targets.front())
                        {
                            ++Index;
                            continue;
                        }
                        Queued.Command.push_back(Expand(Each[Index], ToUtf8(Out), Target));
                    }
                    Jobs.push_back(std::move(Queued));
                }
            }
        }

        /* Every queued run, ceil(Jobs / JobsPerRun) at a time, each compiling Jobs /
         * that many shaders at once; then each variant's stamp, if all its
         * runs cooked. */
        void Run()
        {
            if(Jobs.empty()) { return; }
            std::stable_sort(Jobs.begin(), Jobs.end(), [](const Job& A, const Job& B) { return A.Order < B.Order; });

            /* Shaders each run compiles at once: few, so many runs overlap -- a run
             * spends its end waiting on its slowest shader, which the others fill.
             * Measured on 12 threads: 4 beat 8 (for the same total). */
            constexpr int JobsPerRun = 4;
            const int    Budget = std::max(1, Opts.Jobs);
            const size_t Width  = std::min(Jobs.size(), size_t((Budget + JobsPerRun - 1) / JobsPerRun));
            const int    Each   = std::max(1, Budget / int(Width));
            std::printf("NimbleCook: %d run(s), %d at a time, %d shader(s) at once in each\n", int(Jobs.size()), int(Width), Each);
            std::fflush(stdout);

            std::atomic<size_t> Next{ 0 };
            std::mutex          PrintLock;
            int                 Done = 0;
            const auto Worker = [&]()
            {
                for(size_t Index = Next++; Index < Jobs.size(); Index = Next++)
                {
                    Job& Current = Jobs[Index];
                    std::vector<std::string> Command = Current.Command;
                    Command.insert(Command.begin() + 4, { "-jobs", std::to_string(Each) });   // after "<python> <script> -target <p>"
                    std::string Output;
                    const int   Exit = RunCaptured(Command, Output);

                    std::lock_guard<std::mutex> Lock(PrintLock);
                    ++Done;
                    std::printf("NimbleCook: [%d/%d] %s: %s\n", Done, int(Jobs.size()), Exit == 0 ? "done" : "FAILED",
                                Current.Label.c_str());
                    std::fwrite(Output.data(), 1, Output.size(), stdout);
                    std::fflush(stdout);
                    if(Exit != 0)
                    {
                        Failures.push_back(Current.Label);
                        ++Variants[Current.Variant]->Failed;
                    }
                }
            };
            std::vector<std::thread> Threads;
            for(size_t Index = 0; Index < Width; ++Index)
            {
                Threads.emplace_back(Worker);
            }
            for(std::thread& Thread : Threads)
            {
                Thread.join();
            }

            for(const std::unique_ptr<Variant>& Each : Variants)
            {
                if(Each->Failed > 0) { continue; }
                std::ofstream File(Each->Stamp, std::ios::trunc);
                File << JoinTargets(Opts.Targets) << "\n";
                ++Cooked;
            }
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
            Args Source{ "-src", ToUtf8(Materials), "-root", ToUtf8(Opts.ProjectDir) };
            std::vector<fs::path> Inputs{ Materials };
            /* Its own plain shaders too (custom render passes): Content/Shaders, by file name. */
            const fs::path Shaders = Opts.ProjectDir / "Content" / "Shaders";
            if(fs::is_directory(Shaders, Error))
            {
                Source.insert(Source.end(), { "-src", ToUtf8(Shaders) });
                Inputs.push_back(Shaders);
            }
            const std::string What = Opts.Module.empty() ? std::string("Project") : Opts.Module;

            // Without a module there is no cache to cook: only an editor's loose files.
            if(!Opts.Module.empty())
            {
                CookVariant(What, Root, "Runtime", Inputs, WithCache({}, Source, Opts.Module, false));
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
                CookVariant(What, Root, "Editor", Inputs, { Loose });
            }
            if(Opts.bShipping && !Opts.Module.empty())
            {
                CookVariant(What, Root, "Shipping", Inputs, WithCache({}, Source, Opts.Module, true));
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
                     "                          [-target <OpenGL|Vulkan|D3D12>]... [-editor] [-shipping] [-force]\n"
                     "                          [-jobs <N>] [-python <exe>]\n");
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
        else if(bHasValue && Name == "-jobs")
        {
            const std::string Jobs = argv[++Index];
            Opts.Jobs = std::atoi(Jobs.c_str());
            if(Jobs.empty() || Jobs.find_first_not_of("0123456789") != std::string::npos || Opts.Jobs < 1)
            {
                std::fprintf(stderr, "NimbleCook: -jobs needs a number above 0, not %s\n", Jobs.c_str());
                return 2;
            }
        }
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
    Cook.Run();

    std::printf("NimbleCook: %d cooked, %d up to date, %d failed\n", Cook.Cooked, Cook.Skipped, int(Cook.Failures.size()));
    for(const std::string& Failure : Cook.Failures)
    {
        std::printf("NimbleCook: FAILED %s\n", Failure.c_str());
    }
    return Cook.Failures.empty() ? 0 : 1;
}
