/*
 * PluginCatalogTests -- NimbleEngine/PluginCatalog.h, the C++ twin of
 * Engine/cmake/NimblePlugins.cmake's discovery and resolution, on the same
 * cases as Plugins.Resolve (Tests/Plugins/PluginResolveTest.cmake) and
 * expecting the same outcome and message from each. A case added there
 * belongs here too. Then the host's shadow copies (Plugin.h,
 * SetShadowCopyDir), on whatever plugins the build beside this test has.
 */
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Public/Core/CoreFileProvider.h"
#include "Public/NimbleEngine/Plugin.h"
#include "Public/NimbleEngine/PluginCatalog.h"

namespace fs = std::filesystem;
using namespace NimbleEngine;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

namespace
{
    fs::path GRoot;

    /* plugin(<case> <root> <Name> <Version> <lines...>), as the CMake test's. */
    void Plugin(const std::string& InCase, const std::string& InRoot, const std::string& InName, const std::string& InVersion,
                const std::vector<std::string>& InLines = {}, const std::string& InEngine = "0.1")
    {
        const fs::path Dir = GRoot / InCase / InRoot / InName;
        fs::create_directories(Dir);
        std::ofstream File(Dir / (InName + ".nmplugin"));
        File << "[Plugin]\nName=" << InName << "\nVersion=" << InVersion << "\nEngineVersion=" << InEngine << "\n";
        for(const std::string& Line : InLines) { File << Line << "\n"; }
    }

    void Config(const std::string& InCase, const std::vector<std::string>& InLines)
    {
        fs::create_directories(GRoot / InCase);
        std::ofstream File(GRoot / InCase / "Config.ini");
        File << "[Plugins]\n";
        for(const std::string& Line : InLines) { File << Line << "\n"; }
    }

    PluginCatalog::Resolution Resolve(const std::string& InCase, std::vector<PluginCatalog::PluginInfo>* OutFound = nullptr)
    {
        const std::vector<std::string> Roots = { (GRoot / InCase / "Project").generic_string(), (GRoot / InCase / "Engine").generic_string() };
        const std::vector<PluginCatalog::PluginInfo> Found = PluginCatalog::Find(Roots);
        std::ifstream File(GRoot / InCase / "Config.ini");
        const std::string Text((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
        if(OutFound) { *OutFound = Found; }
        return PluginCatalog::Resolve(Found, PluginCatalog::ReadPluginsSection(Text), "0.1");
    }

    std::string Join(const std::vector<std::string>& InList)
    {
        std::string Result;
        for(const std::string& Each : InList) { Result += (Result.empty() ? "" : ";") + Each; }
        return Result;
    }

    void ExpectOrder(const std::string& InCase, const std::string& InWant)
    {
        const PluginCatalog::Resolution R = Resolve(InCase);
        Expect(R.Error.empty() && Join(R.Order) == InWant, InCase + ": order " + InWant + " (got " + Join(R.Order) + R.Error + ")");
    }

    void ExpectError(const std::string& InCase, const std::string& InWant)
    {
        const PluginCatalog::Resolution R = Resolve(InCase);
        Expect(R.Error.find(InWant) != std::string::npos, InCase + ": error '" + InWant + "' (got '" + R.Error + "')");
    }

    void TestResolve()
    {
        std::printf("resolve (Plugins.Resolve's cases)\n");
        Plugin("order", "Project", "Alpha", "1.0.0", { "[Dependencies]", "Plugins=Beta>=1.2.0" });
        Plugin("order", "Engine",  "Beta",  "1.3.0", { "[Dependencies]", "OptionalPlugins=Gamma, Delta" });
        Plugin("order", "Engine",  "Gamma", "0.1.0", { "EnabledByDefault=true" });
        Plugin("order", "Engine",  "Delta", "0.1.0");
        Config("order", { "Alpha=true", "Beta=true" });
        ExpectOrder("order", "Gamma;Beta;Alpha");

        Plugin("override", "Project", "Beta", "2.0.0");
        Plugin("override", "Engine",  "Beta", "1.0.0");
        Config("override", { "Beta=true" });
        std::vector<PluginCatalog::PluginInfo> Found;
        Resolve("override", &Found);
        Expect(Found.size() == 1 && Found[0].Version == "2.0.0" && Found[0].Dir.find("/Project/Beta") != std::string::npos
               && Found[0].Overrides.find("/Engine/Beta") != std::string::npos, "override: the project's wins, and says what it hides");

        Plugin("enable", "Engine", "OnByDefault", "1.0.0", { "EnabledByDefault=true" });
        Plugin("enable", "Engine", "OffByDefault", "1.0.0");
        Plugin("enable", "Engine", "TurnedOff", "1.0.0", { "EnabledByDefault=true" });
        Config("enable", { "TurnedOff=false", "offbydefault=yes" });
        ExpectOrder("enable", "OffByDefault;OnByDefault");

        Plugin("moduletype", "Engine", "Moduled", "1.0.0", { "[Module_Moduled]", "Type=Sometimes" });
        Config("moduletype", { "Moduled=true" });
        ExpectError("moduletype", "Type must be Runtime, Editor, ClientOnly or ServerOnly");

        Plugin("missing", "Engine", "Alpha", "1.0.0", { "[Dependencies]", "Plugins=Nowhere" });
        Config("missing", { "Alpha=true" });
        ExpectError("missing", "depends on Nowhere, which is in none of");

        Plugin("disabled", "Engine", "Alpha", "1.0.0", { "[Dependencies]", "Plugins=Beta" });
        Plugin("disabled", "Engine", "Beta", "1.0.0");
        Config("disabled", { "Alpha=true" });
        ExpectError("disabled", "depends on Beta, which is not enabled");

        Plugin("tooold", "Engine", "Alpha", "1.0.0", { "[Dependencies]", "Plugins=Beta>=2.0.0" });
        Plugin("tooold", "Engine", "Beta", "1.9.9");
        Config("tooold", { "Alpha=true", "Beta=true" });
        ExpectError("tooold", "needs Beta 2.0.0 or newer; found 1.9.9");

        Plugin("cycle", "Engine", "Alpha", "1.0.0", { "[Dependencies]", "Plugins=Beta" });
        Plugin("cycle", "Engine", "Beta", "1.0.0", { "[Dependencies]", "Plugins=Alpha" });
        Config("cycle", { "Alpha=true", "Beta=true" });
        ExpectError("cycle", "Plugin dependency cycle");

        Plugin("shortname", "Engine", "A", "1.0.0");
        Config("shortname", {});
        ExpectError("shortname", "must be a letter then letters or digits");

        Plugin("badversion", "Engine", "Alpha", "1.0");
        Config("badversion", {});
        ExpectError("badversion", "Version must be major.minor.patch");

        Plugin("newengine", "Engine", "Alpha", "1.0.0", { "EnabledByDefault=true" }, "0.9");
        Config("newengine", {});
        ExpectError("newengine", "needs engine 0.9 or newer; this engine is 0.1");

        fs::create_directories(GRoot / "misnamed" / "Engine" / "Alpha");
        std::ofstream(GRoot / "misnamed" / "Engine" / "Alpha" / "Alpha.nmplugin") << "[Plugin]\nName=Beta\nVersion=1.0.0\nEngineVersion=0.1\n";
        Config("misnamed", {});
        ExpectError("misnamed", "must live in Beta/Beta.nmplugin");
    }

    void TestDetails()
    {
        std::printf("details\n");
        Plugin("details", "Engine", "Weather", "1.2.3",
               { "Category=Rendering", "Description=Rain and snow", "[Module_Weather]", "Type=Runtime",
                 "[Module_WeatherEditor]", "Type=Editor", "[Dependencies]", "Plugins=TimeOfDay>=0.1.0", "OptionalPlugins=Sequencer" });
        fs::create_directories(GRoot / "details" / "Engine" / "Weather" / "Content" / "Shaders");
        std::vector<PluginCatalog::PluginInfo> Found;
        Config("details", {});
        Resolve("details", &Found);
        Expect(Found.size() == 1 && Found[0].Error.empty(), "a full descriptor reads");
        if(Found.size() == 1)
        {
            const PluginCatalog::PluginInfo& W = Found[0];
            Expect(W.Category == "Rendering" && W.Description == "Rain and snow", "category and description");
            Expect(W.Modules.size() == 2 && W.Modules[1].Name == "WeatherEditor" && W.Modules[1].Type == "Editor", "its modules and their types");
            Expect(W.Dependencies.size() == 2 && W.Dependencies[0].MinVersion == "0.1.0" && W.Dependencies[1].bOptional,
                   "its dependencies, required and optional");
            Expect(W.bHasContent && W.bHasShaders && !W.bShadersCooked, "content and shaders, not cooked");
        }

        Expect(PluginCatalog::CompareVersions("1.2", "1.2.0") == 0 && PluginCatalog::CompareVersions("1.10.0", "1.9.9") > 0,
               "versions compare by number");
        Expect(PluginCatalog::ValidateName("Weather", { "Audio" }).empty(), "a good name");
        Expect(!PluginCatalog::ValidateName("audio", { "Audio" }).empty(), "a taken name, in any case");
        Expect(!PluginCatalog::ValidateName("Q", {}).empty() && !PluginCatalog::ValidateName("No Space", {}).empty(), "bad names");
        Expect(std::string(PluginCatalog::GetEngineVersion()).size() > 0, "this engine's version is known");
    }

    /* The host's shadow copies, on the plugins built beside this test (if any). */
    void TestShadowCopies()
    {
        std::printf("shadow copies\n");
        EngineCore::FilesystemProvider  Disk;
        EngineCore::LayeredFileProvider Assets(&Disk, false);
        EngineCore::SetFileProvider(&Assets);
        if(Plugins::Discover(std::string(), false) == 0)
        {
            std::printf("  (no plugins beside this test: skipped)\n");
            EngineCore::SetFileProvider(nullptr);
            return;
        }
        Expect(!Plugins::GetSearchRoots().empty(), "the manifest names its search folders");

        const fs::path Shadow = fs::absolute(GRoot / "Shadow");
        fs::create_directories(Shadow / "Run-left-by-a-dead-process");
        Plugins::SetShadowCopyDir(Shadow.generic_string());
        Expect(!fs::exists(Shadow / "Run-left-by-a-dead-process"), "an earlier run's copies are swept");
        Plugins::LoadModules();
        const std::string First = Plugins::GetNameAt(0);
        Expect(Plugins::IsLoaded(First), "the first plugin loads from its copy");

        size_t Copies = 0;
        for(const fs::directory_entry& Each : fs::recursive_directory_iterator(Shadow))
        {
            if(Each.is_regular_file()) { ++Copies; }
        }
        Expect(Copies > 0, "the copies are there while it runs");
        Plugins::Close();
        Expect(fs::is_empty(Shadow), "and gone after Close");
        Plugins::SetShadowCopyDir(std::string());
        EngineCore::SetFileProvider(nullptr);
    }
}

int main()
{
    GRoot = fs::temp_directory_path() / ("PluginCatalogTests-" + std::to_string(std::rand()));
    fs::remove_all(GRoot);
    TestResolve();
    TestDetails();
    TestShadowCopies();
    fs::remove_all(GRoot);
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
