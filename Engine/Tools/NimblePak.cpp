/*
 * NimblePak: makes and lists content packs (Core's CorePack.h).
 *
 *   NimblePak pack <manifest> <out.nmpak> <name> [--names]
 *   NimblePak list <pack.nmpak>
 *
 * A manifest is one entry per line, tab separated -- what the build writes
 * (Engine/cmake/NimblePack.cmake):
 *
 *   F <tab> <runtime path> <tab> <file>     one file, read by the game as <runtime path>
 *   D <tab> <runtime prefix> <tab> <folder> every file under <folder>, as <prefix>/<its path>
 *
 * --names keeps each path in the pack (a debug-info build's); without it the
 * pack holds only their hashes. `list` prints each entry's hash, size, and
 * path where the pack carries it.
 */

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "Public/Core/CorePack.h"
#include "Public/Core/CorePlatform.h"

namespace
{
    namespace fs = std::filesystem;

    std::string ToUtf8(const fs::path& InPath)
    {
        const auto Text = InPath.generic_u8string();
        return std::string(Text.begin(), Text.end());
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

    int Pack(const std::string& InManifest, const std::string& InOut, const std::string& InName, const bool bInNames)
    {
        std::vector<EngineCore::PackInput> Inputs;
        if(!ReadManifest(InManifest, Inputs)) { return 1; }

        std::error_code Error;
        fs::create_directories(fs::u8path(InOut).parent_path(), Error);
        std::string Why;
        if(!EngineCore::WritePackFile(InOut, InName, Inputs, bInNames, &Why))
        {
            std::fprintf(stderr, "NimblePak: %s: %s\n", InOut.c_str(), Why.c_str());
            return 1;
        }
        size_t Bytes = 0;
        for(const EngineCore::PackInput& Input : Inputs) { Bytes += Input.Bytes.size(); }
        std::printf("NimblePak: %s -- %zu file(s), %zu bytes of content%s\n", InOut.c_str(), Inputs.size(), Bytes,
                    bInNames ? ", with names" : "");
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
        std::printf("%s: pack '%s', %zu file(s)%s\n", InPack.c_str(), Pack->GetName().c_str(), Pack->GetEntryCount(),
                    Pack->HasNames() ? "" : " (no names: a Shipping pack)");
        for(const auto& Entry : Pack->ListEntries())
        {
            std::printf("  %016llx  %s\n", static_cast<unsigned long long>(Entry.first), Entry.second.c_str());
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
        return Pack(Args[1], Args[2], Args[3], bNames);
    }
    if(Args.size() == 2 && Args[0] == "list")
    {
        return List(Args[1]);
    }
    std::fprintf(stderr, "usage: NimblePak pack <manifest> <out.nmpak> <name> [--names]\n"
                         "       NimblePak list <pack.nmpak>\n");
    return 2;
}
