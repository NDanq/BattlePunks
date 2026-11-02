#include "Public/NimbleEngine/PluginCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

/* NimblePlugins.cmake's rules in C++ (PluginCatalog.h). Each function says
 * which CMake function it follows; keep the two in step. */

namespace NimbleEngine::PluginCatalog
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string Trim(const std::string& InText)
        {
            const size_t First = InText.find_first_not_of(" \t\r\n");
            if(First == std::string::npos) { return std::string(); }
            const size_t Last = InText.find_last_not_of(" \t\r\n");
            return InText.substr(First, Last - First + 1);
        }

        std::string Lower(std::string InText)
        {
            std::transform(InText.begin(), InText.end(), InText.begin(), [](unsigned char C) { return char(std::tolower(C)); });
            return InText;
        }

        std::string ToUtf8(const fs::path& InPath)
        {
            const auto Text = InPath.generic_u8string();
            return std::string(Text.begin(), Text.end());
        }

        bool IsLetter(const char InChar) { return (InChar >= 'A' && InChar <= 'Z') || (InChar >= 'a' && InChar <= 'z'); }
        bool IsDigit(const char InChar)  { return InChar >= '0' && InChar <= '9'; }

        /* ^[A-Za-z][A-Za-z0-9]+$ */
        bool IsPluginName(const std::string& InName)
        {
            if(InName.size() < 2 || !IsLetter(InName[0])) { return false; }
            return std::all_of(InName.begin() + 1, InName.end(), [](const char C) { return IsLetter(C) || IsDigit(C); });
        }

        /* InParts numbers separated by dots, at least InMinParts (InExactParts: exactly). */
        bool IsDotted(const std::string& InText, const size_t InMinParts, const bool bInExact)
        {
            size_t Parts = 0;
            size_t Index = 0;
            while(true)
            {
                const size_t Start = Index;
                while(Index < InText.size() && IsDigit(InText[Index])) { ++Index; }
                if(Index == Start) { return false; }
                ++Parts;
                if(Index == InText.size()) { break; }
                if(InText[Index] != '.') { return false; }
                ++Index;
            }
            return bInExact ? Parts == InMinParts : Parts >= InMinParts;
        }

        bool IsTrue(const std::string& InValue)
        {
            const std::string Value = Lower(InValue);
            return Value == "true" || Value == "1" || Value == "yes" || Value == "on";
        }

        bool IsFalse(const std::string& InValue)
        {
            const std::string Value = Lower(InValue);
            return Value == "false" || Value == "0" || Value == "no" || Value == "off";
        }

        /* _nimble_ini_read: sections as written, and "section.key" (both lowercased) -> value, last wins. */
        struct Ini
        {
            std::vector<std::string>           Sections;
            std::map<std::string, std::string> Values;
        };

        Ini ParseIni(const std::string& InText)
        {
            Ini Result;
            std::string Section;
            std::istringstream Stream(InText);
            std::string Line;
            while(std::getline(Stream, Line))
            {
                Line = Trim(Line);
                if(Line.empty() || Line[0] == ';' || Line[0] == '#') { continue; }
                if(Line.front() == '[' && Line.back() == ']')
                {
                    const std::string Name = Trim(Line.substr(1, Line.size() - 2));
                    Result.Sections.push_back(Name);
                    Section = Lower(Name);
                    continue;
                }
                const size_t Eq = Line.find('=');
                if(Eq == std::string::npos) { continue; }
                Result.Values[Section + "." + Lower(Trim(Line.substr(0, Eq)))] = Trim(Line.substr(Eq + 1));
            }
            return Result;
        }

        std::string Get(const Ini& InIni, const std::string& InKey)
        {
            const auto Found = InIni.Values.find(InKey);
            return Found == InIni.Values.end() ? std::string() : Found->second;
        }

        std::string ReadText(const fs::path& InPath)
        {
            std::ifstream File(InPath, std::ios::binary);
            std::ostringstream Text;
            Text << File.rdbuf();
            return Text.str();
        }

        /* _nimble_plugin_split_list */
        std::vector<std::string> SplitList(const std::string& InText)
        {
            std::vector<std::string> Result;
            std::stringstream Stream(InText);
            std::string Item;
            while(std::getline(Stream, Item, ','))
            {
                Item = Trim(Item);
                if(!Item.empty()) { Result.push_back(Item); }
            }
            return Result;
        }

        std::string JoinRoots(const std::vector<std::string>& InRoots)
        {
            std::string Result;
            for(const std::string& Root : InRoots) { Result += (Result.empty() ? "" : ";") + Root; }
            return Result;
        }
    }

    // _nimble_plugin_read_descriptor
    PluginInfo ReadDescriptor(const std::string& InFile)
    {
        PluginInfo Info;
        const fs::path File = fs::u8path(InFile);
        Info.Dir = ToUtf8(File.parent_path());
        const std::string Folder = ToUtf8(File.parent_path().filename());
        const std::string Stem   = ToUtf8(File.stem());
        const Ini D = ParseIni(ReadText(File));

        const auto Fail = [&](const std::string& InWhy) -> PluginInfo&
        {
            Info.Error = InFile + ": " + InWhy;
            return Info;
        };

        Info.Name = Get(D, "plugin.name");
        if(Info.Name.empty()) { Info.Name = Stem; return Fail("[Plugin] Name is required"); }
        if(!IsPluginName(Info.Name))
        {
            return Fail("plugin name '" + Info.Name + "' must be a letter then letters or digits, two characters at least -- it is the '"
                        + Info.Name + ":/' prefix of the plugin's content");
        }
        if(Info.Name != Stem || Info.Name != Folder)
        {
            return Fail("plugin '" + Info.Name + "' must live in " + Info.Name + "/" + Info.Name + ".nmplugin");
        }
        Info.Version = Get(D, "plugin.version");
        if(!IsDotted(Info.Version, 3, true))
        {
            return Fail("[Plugin] Version must be major.minor.patch (got '" + Info.Version + "')");
        }
        Info.EngineVersion = Get(D, "plugin.engineversion");
        if(!IsDotted(Info.EngineVersion, 1, false))
        {
            return Fail("[Plugin] EngineVersion is required, e.g. 0.1 (got '" + Info.EngineVersion + "')");
        }
        Info.Category    = Get(D, "plugin.category");
        Info.Description = Get(D, "plugin.description");

        const std::string Default = Get(D, "plugin.enabledbydefault");
        if(IsTrue(Default))                         { Info.bEnabledByDefault = true; }
        else if(!Default.empty() && !IsFalse(Default)) { return Fail("[Plugin] EnabledByDefault must be true or false"); }

        for(const std::string& Section : D.Sections)
        {
            if(Section.compare(0, 7, "Module_") != 0 || Section.size() <= 7) { continue; }
            ModuleInfo Module;
            Module.Name = Section.substr(7);
            Module.Type = Get(D, Lower(Section) + ".type");
            if(Module.Type != "Runtime" && Module.Type != "Editor" && Module.Type != "ClientOnly" && Module.Type != "ServerOnly")
            {
                return Fail("[" + Section + "] Type must be Runtime, Editor, ClientOnly or ServerOnly (got '" + Module.Type + "')");
            }
            Info.Modules.push_back(Module);
        }

        for(const bool bOptional : { false, true })
        {
            for(const std::string& Entry : SplitList(Get(D, bOptional ? "dependencies.optionalplugins" : "dependencies.plugins")))
            {
                Dependency Dep;
                Dep.bOptional = bOptional;
                const size_t At = Entry.find(">=");
                Dep.Name       = Entry.substr(0, At);
                Dep.MinVersion = At == std::string::npos ? std::string() : Entry.substr(At + 2);
                if(!IsPluginName(Dep.Name) || (At != std::string::npos && !IsDotted(Dep.MinVersion, 3, true)))
                {
                    return Fail("dependency '" + Entry + "' must be Name or Name>=major.minor.patch");
                }
                Info.Dependencies.push_back(Dep);
            }
        }

        std::error_code Error;
        const fs::path Dir = File.parent_path();
        Info.bHasContent = fs::is_directory(Dir / "Content", Error);
        Info.bHasShaders = fs::is_directory(Dir / "Content" / "Shaders", Error) || fs::is_directory(Dir / "Content" / "Materials", Error);
        Info.bShadersCooked = fs::is_regular_file(Dir / "Saved" / "Cooked" / "Shaders" / "Runtime.stamp", Error);
        return Info;
    }

    // nimble_plugins_resolve, discovery
    std::vector<PluginInfo> Find(const std::vector<std::string>& InRoots)
    {
        std::vector<PluginInfo>  Found;
        std::vector<std::string> Stems;   // what the CMake keeps names by: the file's
        for(const std::string& RootText : InRoots)
        {
            std::error_code Error;
            const fs::path Root = fs::u8path(RootText);
            if(!fs::is_directory(Root, Error)) { continue; }

            std::vector<fs::path> Descriptors;
            for(const fs::directory_entry& Folder : fs::directory_iterator(Root, Error))
            {
                if(!Folder.is_directory(Error)) { continue; }
                for(const fs::directory_entry& File : fs::directory_iterator(Folder.path(), Error))
                {
                    if(File.is_regular_file(Error) && File.path().extension() == ".nmplugin") { Descriptors.push_back(File.path()); }
                }
            }
            std::sort(Descriptors.begin(), Descriptors.end());

            for(const fs::path& File : Descriptors)
            {
                const std::string Stem = ToUtf8(File.stem());
                const auto Kept = std::find(Stems.begin(), Stems.end(), Stem);
                if(Kept != Stems.end())
                {
                    PluginInfo& Keeper = Found[size_t(Kept - Stems.begin())];
                    if(Keeper.Overrides.empty()) { Keeper.Overrides = ToUtf8(File.parent_path()); }
                    continue;
                }
                PluginInfo Info = ReadDescriptor(ToUtf8(File));
                Info.Root = RootText;
                Found.push_back(std::move(Info));
                Stems.push_back(Stem);
            }
        }
        return Found;
    }

    std::vector<std::pair<std::string, std::string>> ReadPluginsSection(const std::string& InConfigText)
    {
        std::vector<std::pair<std::string, std::string>> Result;
        bool bIn = false;
        std::istringstream Stream(InConfigText);
        std::string Line;
        while(std::getline(Stream, Line))
        {
            Line = Trim(Line);
            if(Line.empty() || Line[0] == ';' || Line[0] == '#') { continue; }
            if(Line.front() == '[' && Line.back() == ']')
            {
                bIn = Lower(Trim(Line.substr(1, Line.size() - 2))) == "plugins";
                continue;
            }
            const size_t Eq = Line.find('=');
            if(bIn && Eq != std::string::npos)
            {
                Result.emplace_back(Trim(Line.substr(0, Eq)), Trim(Line.substr(Eq + 1)));
            }
        }
        return Result;
    }

    bool IsEnabled(const PluginInfo& InInfo, const std::vector<std::pair<std::string, std::string>>& InSection)
    {
        bool bOn = InInfo.bEnabledByDefault;
        for(const auto& Line : InSection)   // the last line wins, as the CMake read's
        {
            if(Lower(Line.first) != Lower(InInfo.Name)) { continue; }
            if(IsTrue(Line.second))       { bOn = true; }
            else if(IsFalse(Line.second)) { bOn = false; }
        }
        return bOn;
    }

    // nimble_plugins_resolve, enabling, checks and order
    Resolution Resolve(const std::vector<PluginInfo>& InFound, const std::vector<std::pair<std::string, std::string>>& InSection,
                       const std::string& InEngineVersion)
    {
        Resolution Result;
        std::vector<std::string> Roots;
        for(const PluginInfo& Info : InFound)
        {
            if(std::find(Roots.begin(), Roots.end(), Info.Root) == Roots.end()) { Roots.push_back(Info.Root); }
        }

        /* A malformed descriptor stops the configure, enabled or not. */
        for(const PluginInfo& Info : InFound)
        {
            if(!Info.Error.empty())
            {
                Result.Error = Info.Error;
                return Result;
            }
        }

        for(const auto& Line : InSection)
        {
            if(!IsTrue(Line.second) && !IsFalse(Line.second))
            {
                Result.Warnings.push_back("[Plugins] " + Line.first + "=" + Lower(Line.second) + " is not true or false; ignored");
            }
            const bool bKnown = std::any_of(InFound.begin(), InFound.end(), [&Line](const PluginInfo& InInfo)
            {
                return Lower(InInfo.Name) == Lower(Line.first);
            });
            if(!bKnown)
            {
                Result.Warnings.push_back("[Plugins] names '" + Line.first + "', which is in none of: " + JoinRoots(Roots));
            }
        }

        std::vector<const PluginInfo*> Enabled;
        for(const PluginInfo& Info : InFound)
        {
            if(IsEnabled(Info, InSection)) { Enabled.push_back(&Info); }
        }
        const auto FindFound = [&InFound](const std::string& InName) -> const PluginInfo*
        {
            for(const PluginInfo& Info : InFound) { if(Info.Name == InName) { return &Info; } }
            return nullptr;
        };
        const auto IsOn = [&Enabled](const std::string& InName)
        {
            return std::any_of(Enabled.begin(), Enabled.end(), [&InName](const PluginInfo* InInfo) { return InInfo->Name == InName; });
        };

        for(const PluginInfo* Info : Enabled)
        {
            if(!InEngineVersion.empty() && CompareVersions(InEngineVersion, Info->EngineVersion) < 0)
            {
                Result.Error = "Plugin " + Info->Name + " needs engine " + Info->EngineVersion + " or newer; this engine is " + InEngineVersion;
                return Result;
            }
            for(const Dependency& Dep : Info->Dependencies)
            {
                if(Dep.bOptional) { continue; }
                const PluginInfo* Needed = FindFound(Dep.Name);
                if(!Needed)
                {
                    Result.Error = "Plugin " + Info->Name + " depends on " + Dep.Name + ", which is in none of: " + JoinRoots(Roots);
                    return Result;
                }
                if(!IsOn(Dep.Name))
                {
                    Result.Error = "Plugin " + Info->Name + " depends on " + Dep.Name + ", which is not enabled: add " + Dep.Name
                                   + "=true to [Plugins]";
                    return Result;
                }
                if(!Dep.MinVersion.empty() && CompareVersions(Needed->Version, Dep.MinVersion) < 0)
                {
                    Result.Error = "Plugin " + Info->Name + " needs " + Dep.Name + " " + Dep.MinVersion + " or newer; found " + Needed->Version;
                    return Result;
                }
            }
        }

        /* _nimble_plugin_visit: depth first, an explicit stack that is the current path. */
        for(const PluginInfo* Start : Enabled)
        {
            std::vector<std::string> Stack{ Start->Name };
            while(!Stack.empty())
            {
                const std::string Top = Stack.back();
                if(std::find(Result.Order.begin(), Result.Order.end(), Top) != Result.Order.end())
                {
                    Stack.pop_back();
                    continue;
                }
                const PluginInfo* Info = FindFound(Top);
                std::string Next;
                for(const bool bOptional : { false, true })
                {
                    for(const Dependency& Dep : Info->Dependencies)
                    {
                        if(Dep.bOptional != bOptional) { continue; }
                        if(!IsOn(Dep.Name) || std::find(Result.Order.begin(), Result.Order.end(), Dep.Name) != Result.Order.end()) { continue; }
                        if(std::find(Stack.begin(), Stack.end(), Dep.Name) != Stack.end())
                        {
                            std::string Cycle;
                            for(const std::string& Each : Stack) { Cycle += Each + " -> "; }
                            Result.Error = "Plugin dependency cycle: " + Cycle + Dep.Name;
                            return Result;
                        }
                        if(Next.empty()) { Next = Dep.Name; }   // every candidate checked for a cycle, the first followed
                    }
                }
                if(Next.empty())
                {
                    Result.Order.push_back(Top);
                    Stack.pop_back();
                }
                else
                {
                    Stack.push_back(Next);
                }
            }
        }
        return Result;
    }

    std::string ValidateName(const std::string& InName, const std::vector<std::string>& InTaken)
    {
        if(InName.empty())
        {
            return "Give it a name.";
        }
        if(!IsPluginName(InName))
        {
            return "A letter, then letters or digits only (two characters at least): it names the folder, the module, "
                   "its classes and the '" + InName + ":/' content prefix.";
        }
        for(const std::string& Taken : InTaken)
        {
            if(Lower(Taken) == Lower(InName))
            {
                return "'" + Taken + "' is taken: a plugin, an engine module or a build target already has that name.";
            }
        }
        return std::string();
    }

    int CompareVersions(const std::string& InA, const std::string& InB)
    {
        std::stringstream A(InA), B(InB);
        std::string PartA, PartB;
        while(true)
        {
            const bool bA = bool(std::getline(A, PartA, '.'));
            const bool bB = bool(std::getline(B, PartB, '.'));
            if(!bA && !bB) { return 0; }
            const int64_t ValueA = bA ? int64_t(std::strtoll(PartA.c_str(), nullptr, 10)) : 0;
            const int64_t ValueB = bB ? int64_t(std::strtoll(PartB.c_str(), nullptr, 10)) : 0;
            if(ValueA != ValueB) { return ValueA < ValueB ? -1 : 1; }
        }
    }

    const char* GetEngineVersion()
    {
#ifdef NIMBLE_ENGINE_VERSION
        return NIMBLE_ENGINE_VERSION;
#else
        return "";
#endif
    }
}
