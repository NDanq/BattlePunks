/*
 * NimblePak: makes and lists content packs (Core's CorePack.h).
 *
 *   NimblePak pack <manifest> <out.nmpak> <name> [--names] [--strip] [--scramble] [--zstd=<level>]
 *   NimblePak list <pack.nmpak>
 *
 * A manifest is one entry per line, tab separated -- what the build writes
 * (Engine/cmake/NimblePack.cmake):
 *
 *   F <tab> <runtime path> <tab> <file>     one file, read by the game as <runtime path>
 *   D <tab> <runtime prefix> <tab> <folder> every file under <folder>, as <prefix>/<its path>
 *
 * --names keeps each path in the pack (a debug-info build's); without it the
 * pack holds only their hashes. `list` prints each entry's hash, size (and
 * stored size, when compressed), and path where the pack carries it.
 *
 * --zstd=<level> compresses the content at that zstd level (1-22); without
 * it, or at 0, it is stored as it is. The build asks for a fast level for a
 * debug-info build and a small one for Shipping (NimblePack.cmake).
 *
 * --strip (a Shipping build's) packs what the game runs, not what a person
 * reads: a .lua file, and a script or UI graph's generated Lua, as stripped
 * Lua bytecode -- no comments, local names or line numbers -- and a .ini
 * without its comment and blank lines.
 *
 * --scramble (a Shipping build's) XORs each entry's bytes (CorePack.h), so
 * what content names inside itself does not show in a hex viewer.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "Public/Core/CorePack.h"
#include "Public/Core/CorePlatform.h"
#include "Public/GameCore/GraphText.h"

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
}

namespace
{
    namespace fs = std::filesystem;

    std::string ToUtf8(const fs::path& InPath)
    {
        const auto Text = InPath.generic_u8string();
        return std::string(Text.begin(), Text.end());
    }

    bool EndsWith(const std::string& InText, const char* InSuffix)
    {
        const std::string Suffix = InSuffix;
        if(InText.size() < Suffix.size()) { return false; }
        for(size_t Index = 0; Index < Suffix.size(); ++Index)
        {
            const char Char = InText[InText.size() - Suffix.size() + Index];
            if((Char >= 'A' && Char <= 'Z' ? char(Char - 'A' + 'a') : Char) != Suffix[Index]) { return false; }
        }
        return true;
    }

    int WriteChunk(lua_State*, const void* InData, const size_t InSize, void* InOut)
    {
        auto* Out = static_cast<std::vector<uint8_t>*>(InOut);
        Out->insert(Out->end(), static_cast<const uint8_t*>(InData), static_cast<const uint8_t*>(InData) + InSize);
        return 0;
    }

    /* InSource compiled and dumped stripped. False, with the reason, if it does not compile. */
    bool CompileLua(const std::string& InSource, const std::string& InName, std::vector<uint8_t>& OutBytes, std::string& OutError)
    {
        lua_State* State = luaL_newstate();
        if(!State) { OutError = "out of memory"; return false; }
        bool bOk = luaL_loadbuffer(State, InSource.data(), InSource.size(), ("=" + InName).c_str()) == LUA_OK;
        if(bOk)
        {
            OutBytes.clear();
            bOk = lua_dump(State, &WriteChunk, &OutBytes, 1) == 0;
            if(!bOk) { OutError = "lua_dump failed"; }
        }
        else
        {
            OutError = lua_tostring(State, -1);
        }
        lua_close(State);
        return bOk;
    }

    /* A Shipping pack's form of one file; false only for a script that does not compile. */
    bool Strip(EngineCore::PackInput& InOut)
    {
        const std::string Text(InOut.Bytes.begin(), InOut.Bytes.end());
        std::string Lua, Error;
        if(EndsWith(InOut.Path, ".lua"))
        {
            Lua = Text;
        }
        else if(EndsWith(InOut.Path, ".script.nmgraph") || EndsWith(InOut.Path, ".ui.nmgraph"))
        {
            if(!GameCore::ExtractGraphGeneratedCode(Text, ".lua", Lua))
            {
                std::fprintf(stderr, "NimblePak: %s has no generated Lua (save it in the editor); packed as it is\n", InOut.Path.c_str());
                return true;
            }
        }
        else if(EndsWith(InOut.Path, ".ini"))
        {
            std::string Kept;
            size_t Start = 0;
            while(Start < Text.size())
            {
                size_t End = Text.find('\n', Start);
                if(End == std::string::npos) { End = Text.size(); }
                const std::string Line = Text.substr(Start, End - Start);
                Start = End + 1;
                const size_t First = Line.find_first_not_of(" \t\r");
                if(First == std::string::npos || Line[First] == ';' || Line[First] == '#') { continue; }
                Kept += Line + "\n";
            }
            InOut.Bytes.assign(Kept.begin(), Kept.end());
            return true;
        }
        else
        {
            return true;
        }

        if(!CompileLua(Lua, "?", InOut.Bytes, Error))
        {
            std::fprintf(stderr, "NimblePak: %s does not compile: %s\n", InOut.Path.c_str(), Error.c_str());
            return false;
        }
        return true;
    }

    bool ReadManifest(const std::string& InManifest, std::vector<EngineCore::PackInput>& OutInputs)
    {
        std::vector<uint8_t> Bytes;
        if(!EngineCore::appFileReadAll(InManifest, Bytes))
        {
            std::fprintf(stderr, "NimblePak: cannot read %s\n", InManifest.c_str());
            return false;
        }
        const std::string Text(Bytes.begin(), Bytes.end());

        const auto AddFile = [&OutInputs](const std::string& InRuntime, const std::string& InFile)
        {
            EngineCore::PackInput Input;
            Input.Path = InRuntime;
            if(!EngineCore::appFileReadAll(InFile, Input.Bytes))
            {
                std::fprintf(stderr, "NimblePak: cannot read %s (for %s)\n", InFile.c_str(), InRuntime.c_str());
                return false;
            }
            OutInputs.push_back(std::move(Input));
            return true;
        };

        size_t Start = 0;
        while(Start < Text.size())
        {
            size_t End = Text.find('\n', Start);
            if(End == std::string::npos) { End = Text.size(); }
            std::string Line = Text.substr(Start, End - Start);
            Start = End + 1;
            if(!Line.empty() && Line.back() == '\r') { Line.pop_back(); }
            if(Line.empty()) { continue; }

            const size_t Tab1 = Line.find('\t');
            const size_t Tab2 = Tab1 == std::string::npos ? std::string::npos : Line.find('\t', Tab1 + 1);
            if(Tab2 == std::string::npos)
            {
                std::fprintf(stderr, "NimblePak: bad manifest line: %s\n", Line.c_str());
                return false;
            }
            const std::string Kind    = Line.substr(0, Tab1);
            const std::string Runtime = Line.substr(Tab1 + 1, Tab2 - Tab1 - 1);
            const std::string Source  = Line.substr(Tab2 + 1);

            if(Kind == "F")
            {
                if(!AddFile(Runtime, Source)) { return false; }
            }
            else if(Kind == "D")
            {
                std::error_code Error;
                std::vector<fs::path> Files;
                for(const fs::directory_entry& Entry : fs::recursive_directory_iterator(fs::u8path(Source), Error))
                {
                    if(Entry.is_regular_file(Error)) { Files.push_back(Entry.path()); }
                }
                std::sort(Files.begin(), Files.end());
                for(const fs::path& File : Files)
                {
                    const std::string Relative = ToUtf8(fs::relative(File, fs::u8path(Source), Error));
                    if(!AddFile(Runtime.empty() ? Relative : Runtime + "/" + Relative, ToUtf8(File))) { return false; }
                }
            }
            else
            {
                std::fprintf(stderr, "NimblePak: unknown manifest entry '%s'\n", Kind.c_str());
                return false;
            }
        }
        return true;
    }

    int Pack(const std::string& InManifest, const std::string& InOut, const std::string& InName, const bool bInNames,
             const bool bInStrip, const bool bInScramble, const int InZstdLevel)
    {
        std::vector<EngineCore::PackInput> Inputs;
        if(!ReadManifest(InManifest, Inputs)) { return 1; }
        if(bInStrip)
        {
            for(EngineCore::PackInput& Input : Inputs)
            {
                if(!Strip(Input)) { return 1; }
            }
        }

        std::error_code Error;
        fs::create_directories(fs::u8path(InOut).parent_path(), Error);
        std::string Why;
        if(!EngineCore::WritePackFile(InOut, InName, Inputs, bInNames, bInScramble, InZstdLevel, &Why))
        {
            std::fprintf(stderr, "NimblePak: %s: %s\n", InOut.c_str(), Why.c_str());
            return 1;
        }
        size_t Bytes = 0;
        for(const EngineCore::PackInput& Input : Inputs) { Bytes += Input.Bytes.size(); }
        std::error_code SizeError;
        const auto PackBytes = static_cast<unsigned long long>(fs::file_size(fs::u8path(InOut), SizeError));
        std::printf("NimblePak: %s -- %zu file(s), %zu bytes of content in %llu%s%s%s%s\n", InOut.c_str(), Inputs.size(), Bytes,
                    PackBytes, bInNames ? ", with names" : "", bInStrip ? ", stripped" : "", bInScramble ? ", scrambled" : "",
                    InZstdLevel > 0 ? (", zstd " + std::to_string(InZstdLevel)).c_str() : "");
        return 0;
    }

    int List(const std::string& InPack)
    {
        std::vector<uint8_t> Bytes;
        std::string Why;
        std::shared_ptr<EngineCore::PackFileProvider> Pack =
            EngineCore::appFileReadAll(InPack, Bytes) ? EngineCore::PackFileProvider::Load(std::move(Bytes), Why) : nullptr;
        if(!Pack)
        {
            std::fprintf(stderr, "NimblePak: %s: %s\n", InPack.c_str(), Why.empty() ? "cannot read" : Why.c_str());
            return 1;
        }
        std::printf("%s: pack '%s', %zu file(s)%s%s\n", InPack.c_str(), Pack->GetName().c_str(), Pack->GetEntryCount(),
                    Pack->HasNames() ? "" : " (no names: a Shipping pack)", Pack->IsScrambled() ? ", scrambled" : "");
        for(const auto& Entry : Pack->ListEntries())
        {
            uint64_t Size = 0, Stored = 0;
            Pack->GetEntrySizes(Entry.first, Size, Stored);
            const std::string Sizes = Stored == Size ? std::to_string(Size) : std::to_string(Size) + " (" + std::to_string(Stored) + ")";
            std::printf("  %016llx  %22s  %s\n", static_cast<unsigned long long>(Entry.first), Sizes.c_str(), Entry.second.c_str());
        }
        return 0;
    }

    /* --zstd=<level>'s level; 0 without it. */
    int ZstdLevel(const std::vector<std::string>& InArgs)
    {
        for(const std::string& Arg : InArgs)
        {
            if(Arg.compare(0, 7, "--zstd=") == 0) { return std::max(0, std::atoi(Arg.c_str() + 7)); }
        }
        return 0;
    }
}

int main(int argc, char** argv)
{
    const std::vector<std::string> Args(argv + 1, argv + argc);
    if(Args.size() >= 4 && Args[0] == "pack")
    {
        const bool bNames = std::find(Args.begin(), Args.end(), "--names") != Args.end();
        const bool bStrip    = std::find(Args.begin(), Args.end(), "--strip") != Args.end();
        const bool bScramble = std::find(Args.begin(), Args.end(), "--scramble") != Args.end();
        return Pack(Args[1], Args[2], Args[3], bNames, bStrip, bScramble, ZstdLevel(Args));
    }
    if(Args.size() == 2 && Args[0] == "list")
    {
        return List(Args[1]);
    }
    std::fprintf(stderr, "usage: NimblePak pack <manifest> <out.nmpak> <name> [--names] [--strip] [--scramble] [--zstd=<level>]\n"
                         "       NimblePak list <pack.nmpak>\n");
    return 2;
}
