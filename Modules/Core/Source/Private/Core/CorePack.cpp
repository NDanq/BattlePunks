#include "Public/Core/CorePack.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"

namespace EngineCore
{
namespace
{
    constexpr uint32_t PACK_MAGIC   = 0x4B41504Eu;   // "NPAK" read as little-endian bytes
    constexpr uint16_t PACK_VERSION = 1;
    constexpr uint16_t PF_Names     = 1;

    uint64_t Fnv1a64(const uint8_t* InData, const size_t InSize)
    {
        uint64_t Hash = 0xcbf29ce484222325ull;
        for(size_t Index = 0; Index < InSize; ++Index)
        {
            Hash ^= InData[Index];
            Hash *= 0x100000001b3ull;
        }
        return Hash;
    }

    // --- little-endian writing and reading ------------------------------------

    void Put(std::vector<uint8_t>& Out, const uint64_t InValue, const int InBytes)
    {
        for(int Byte = 0; Byte < InBytes; ++Byte)
        {
            Out.push_back(uint8_t(InValue >> (8 * Byte)));
        }
    }

    void PutString(std::vector<uint8_t>& Out, const std::string& InText)
    {
        Put(Out, InText.size(), 4);
        Out.insert(Out.end(), InText.begin(), InText.end());
    }

    struct Reader
    {
        const std::vector<uint8_t>& Data;
        size_t                      Cursor = 0;
        bool                        bError = false;

        uint64_t Get(const int InBytes)
        {
            if(bError || Cursor + size_t(InBytes) > Data.size()) { bError = true; return 0; }
            uint64_t Value = 0;
            for(int Byte = 0; Byte < InBytes; ++Byte)
            {
                Value |= uint64_t(Data[Cursor++]) << (8 * Byte);
            }
            return Value;
        }

        std::string GetString()
        {
            const uint64_t Length = Get(4);
            if(bError || Cursor + Length > Data.size()) { bError = true; return std::string(); }
            std::string Text(reinterpret_cast<const char*>(Data.data() + Cursor), size_t(Length));
            Cursor += size_t(Length);
            return Text;
        }
    };
}

std::string NormalizePackPath(const std::string& InPath)
{
    std::string Path;
    Path.reserve(InPath.size());
    for(const char Char : InPath)
    {
        Path.push_back(Char == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(Char))));
    }
    while(Path.compare(0, 2, "./") == 0) { Path.erase(0, 2); }
    while(!Path.empty() && Path[0] == '/') { Path.erase(0, 1); }
    return Path;
}

uint64_t HashPackPath(const std::string& InPath)
{
    const std::string Path = NormalizePackPath(InPath);
    return Fnv1a64(reinterpret_cast<const uint8_t*>(Path.data()), Path.size());
}

bool WritePackFile(const std::string& InFile, const std::string& InName, const std::vector<PackInput>& InEntries,
                   const bool bInNames, std::string* OutError)
{
    const auto Fail = [OutError](const std::string& InWhy)
    {
        if(OutError) { *OutError = InWhy; }
        return false;
    };

    /* Content once: blob by content hash (and bytes, against a collision). */
    std::vector<const std::vector<uint8_t>*> Blobs;
    std::multimap<uint64_t, uint32_t>        BlobByContent;
    std::map<uint64_t, uint32_t>             Entries;   // path hash -> blob, sorted
    std::map<uint64_t, std::string>          Paths;     // normalized: what was hashed
    std::map<uint64_t, std::string>          Shown;     // as given, '/' separated: the names table
    for(const PackInput& Input : InEntries)
    {
        const uint64_t PathHash = HashPackPath(Input.Path);
        const auto Taken = Paths.find(PathHash);
        if(Taken != Paths.end())
        {
            if(Taken->second == NormalizePackPath(Input.Path))
            {
                return Fail("'" + Input.Path + "' is in the pack twice");
            }
            return Fail("'" + Input.Path + "' and '" + Taken->second + "' hash alike; rename one");
        }
        Paths[PathHash] = NormalizePackPath(Input.Path);
        std::string AsGiven = Input.Path;
        std::replace(AsGiven.begin(), AsGiven.end(), '\\', '/');
        Shown[PathHash] = AsGiven;

        const uint64_t ContentHash = Fnv1a64(Input.Bytes.data(), Input.Bytes.size());
        uint32_t BlobIndex = uint32_t(Blobs.size());
        const auto Range = BlobByContent.equal_range(ContentHash);
        for(auto It = Range.first; It != Range.second; ++It)
        {
            if(*Blobs[It->second] == Input.Bytes) { BlobIndex = It->second; break; }
        }
        if(BlobIndex == Blobs.size())
        {
            Blobs.push_back(&Input.Bytes);
            BlobByContent.emplace(ContentHash, BlobIndex);
        }
        Entries[PathHash] = BlobIndex;
    }

    std::vector<uint8_t> Head;
    Put(Head, PACK_MAGIC, 4);
    Put(Head, PACK_VERSION, 2);
    Put(Head, bInNames ? PF_Names : 0, 2);
    PutString(Head, InName);

    /* Offsets are known once the tables' size is: measure them first. */
    const size_t BlobTable = 4 + Blobs.size() * 16;
    size_t Tail = 4 + Entries.size() * 12 + 4;
    if(bInNames)
    {
        for(const auto& Each : Shown) { Tail += 8 + 4 + Each.second.size(); }
    }
    uint64_t Offset = uint64_t(Head.size() + BlobTable + Tail);

    Put(Head, Blobs.size(), 4);
    for(const std::vector<uint8_t>* Bytes : Blobs)
    {
        Put(Head, Offset, 8);
        Put(Head, Bytes->size(), 8);
        Offset += Bytes->size();
    }
    Put(Head, Entries.size(), 4);
    for(const auto& Entry : Entries)
    {
        Put(Head, Entry.first, 8);
        Put(Head, Entry.second, 4);
    }
    Put(Head, bInNames ? Shown.size() : 0, 4);
    if(bInNames)
    {
        for(const auto& Each : Shown)
        {
            Put(Head, Each.first, 8);
            PutString(Head, Each.second);
        }
    }

    const std::string Temporary = InFile + ".tmp";
    FileHandle File = appFileOpenWrite(Temporary);
    if(!File)
    {
        return Fail("cannot write " + Temporary);
    }
    bool bWritten = appFileWrite(File, Head.data(), Head.size()) == Head.size();
    for(const std::vector<uint8_t>* Bytes : Blobs)
    {
        bWritten = bWritten && (Bytes->empty() || appFileWrite(File, Bytes->data(), Bytes->size()) == Bytes->size());
    }
    appFileClose(File);

    std::error_code Error;
    const std::filesystem::path Target = std::filesystem::u8path(InFile);
    if(!bWritten)
    {
        std::filesystem::remove(std::filesystem::u8path(Temporary), Error);
        return Fail("could not write all of " + InFile);
    }
    std::filesystem::rename(std::filesystem::u8path(Temporary), Target, Error);
    if(Error)
    {
        return Fail("could not replace " + InFile + ": " + Error.message());
    }
    return true;
}

std::shared_ptr<PackFileProvider> PackFileProvider::Load(std::vector<uint8_t> InBytes, std::string& OutError)
{
    std::shared_ptr<PackFileProvider> Pack(new PackFileProvider());
    Pack->Data = std::move(InBytes);
    Reader In{ Pack->Data };

    if(In.Get(4) != PACK_MAGIC)
    {
        OutError = "not a content pack (no NPAK header)";
        return nullptr;
    }
    const uint64_t Version = In.Get(2);
    const uint64_t Flags   = In.Get(2);
    if(Version != PACK_VERSION)
    {
        OutError = "pack version " + std::to_string(Version) + "; this build reads " + std::to_string(PACK_VERSION);
        return nullptr;
    }
    Pack->Name = In.GetString();

    const uint64_t BlobCount = In.Get(4);
    for(uint64_t Index = 0; Index < BlobCount && !In.bError; ++Index)
    {
        Blob Each;
        Each.Offset = In.Get(8);
        Each.Size   = In.Get(8);
        if(Each.Offset > Pack->Data.size() || Each.Size > Pack->Data.size() - Each.Offset)
        {
            In.bError = true;
            break;
        }
        Pack->Blobs.push_back(Each);
    }
    const uint64_t EntryCount = In.Get(4);
    for(uint64_t Index = 0; Index < EntryCount && !In.bError; ++Index)
    {
        const uint64_t Hash      = In.Get(8);
        const uint64_t BlobIndex = In.Get(4);
        if(BlobIndex >= Pack->Blobs.size()) { In.bError = true; break; }
        Pack->Entries[Hash] = uint32_t(BlobIndex);
    }
    const uint64_t NameCount = In.Get(4);
    for(uint64_t Index = 0; Index < NameCount && !In.bError; ++Index)
    {
        const uint64_t Hash = In.Get(8);
        Pack->Names[Hash] = In.GetString();
    }
    if(In.bError)
    {
        OutError = "the pack is truncated or malformed";
        return nullptr;
    }
    (void)Flags;
    return Pack;
}

bool PackFileProvider::ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes)
{
    const auto Found = Entries.find(HashPackPath(InPath));
    if(Found == Entries.end())
    {
        return false;
    }
    const Blob& Each = Blobs[Found->second];
    OutBytes.assign(Data.begin() + std::ptrdiff_t(Each.Offset), Data.begin() + std::ptrdiff_t(Each.Offset + Each.Size));
    return true;
}

bool PackFileProvider::Exists(const std::string& InPath)
{
    return Entries.count(HashPackPath(InPath)) != 0;
}

const std::string* PackFileProvider::FindName(const uint64_t InHash) const
{
    const auto Found = Names.find(InHash);
    return Found == Names.end() ? nullptr : &Found->second;
}

std::vector<std::pair<uint64_t, std::string>> PackFileProvider::ListEntries() const
{
    std::vector<std::pair<uint64_t, std::string>> Out;
    Out.reserve(Entries.size());
    for(const auto& Entry : Entries)
    {
        const std::string* Path = FindName(Entry.first);
        Out.emplace_back(Entry.first, Path ? *Path : std::string());
    }
    std::sort(Out.begin(), Out.end());
    return Out;
}

int MountPackFiles(IFileProvider& InProvider, const std::string& InDirectory)
{
    namespace fs = std::filesystem;
    std::error_code Error;
    std::vector<std::string> Files;
    for(const fs::directory_entry& Entry : fs::directory_iterator(fs::u8path(InDirectory), Error))
    {
        const auto NameU8 = Entry.path().filename().u8string();
        const std::string Name(NameU8.begin(), NameU8.end());
        if(Entry.is_regular_file(Error) && Name.size() > 6 && Name.compare(Name.size() - 6, 6, ".nmpak") == 0)
        {
            Files.push_back(Name);
        }
    }
    /* The engine's first: a later mount is asked first, so the game's wins. */
    std::sort(Files.begin(), Files.end(), [](const std::string& InA, const std::string& InB)
    {
        const bool bEngineA = InA == "Engine.nmpak", bEngineB = InB == "Engine.nmpak";
        return bEngineA != bEngineB ? bEngineA : InA < InB;
    });

    int Mounted = 0;
    for(const std::string& Name : Files)
    {
        const std::string Path = InDirectory + "/" + Name;
        std::vector<uint8_t> Bytes;
        std::string Why;
        std::shared_ptr<PackFileProvider> Pack = appFileReadAll(Path, Bytes) ? PackFileProvider::Load(std::move(Bytes), Why) : nullptr;
        if(!Pack)
        {
            CORE_LOG_ERROR("Content pack " + Path + " not mounted: " + (Why.empty() ? std::string("unreadable") : Why))
            continue;
        }
        if(!InProvider.MountLayer("Pack:" + Name, Pack))
        {
            CORE_LOG_ERROR("Content pack " + Path + " not mounted: the file provider takes no layers")
            continue;
        }
        CORE_LOG("Content pack " + Path + ": " + std::to_string(Pack->GetEntryCount()) + " file(s)"
                 + (Pack->HasNames() ? ", with names" : ""))
        ++Mounted;
    }
    return Mounted;
}
}
