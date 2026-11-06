#include "Public/Core/CorePack.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>

#include <zstd.h>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"

namespace EngineCore
{
namespace
{
    constexpr uint32_t PACK_MAGIC   = 0x4B41504Eu;   // "NPAK" read as little-endian bytes
    constexpr uint16_t PACK_VERSION = 2;
    constexpr uint16_t PF_Names     = 1;
    constexpr uint16_t PF_Scrambled = 2;

    constexpr uint8_t CODEC_STORED = 0;
    constexpr uint8_t CODEC_ZSTD   = 1;

    /* A zstd blob's bytes per chunk, decoded: what a range read decodes at
     * least. Big enough that the frames compress nearly as well as one would. */
    constexpr uint32_t PACK_CHUNK_SIZE = 256 * 1024;

    /* Below this a blob is stored as it is: a frame's own overhead eats it. */
    constexpr size_t PACK_MIN_COMPRESS = 64;

    /* One decoder per thread: loaders read packs from job threads. */
    ZSTD_DCtx* ThreadDecoder()
    {
        struct Holder
        {
            ZSTD_DCtx* Context = ZSTD_createDCtx();
            ~Holder() { ZSTD_freeDCtx(Context); }
        };
        thread_local Holder Decoder;
        return Decoder.Context;
    }

    /* The scramble's seed. Not a secret worth the name: it only keeps a
     * pack's text out of a hex viewer. */
    constexpr uint64_t SCRAMBLE_KEY = 0x4E696D626C655041ull;   // "NimblePA"

    /* XORs InSize bytes with a splitmix64 stream seeded by the blob's file
     * offset: the same call scrambles and unscrambles. */
    /* The same, for InSize bytes that start InFrom bytes into the blob (a
     * range read): the stream is skipped ahead to InFrom's word. */
    void ScrambleRange(uint8_t* InOutData, const size_t InSize, const uint64_t InOffset, const uint64_t InFrom)
    {
        uint64_t State = SCRAMBLE_KEY ^ (InOffset * 0x9E3779B97F4A7C15ull);
        State += (InFrom / 8) * 0x9E3779B97F4A7C15ull;
        size_t Index = 0;
        size_t Byte  = size_t(InFrom % 8);
        while(Index < InSize)
        {
            State += 0x9E3779B97F4A7C15ull;
            uint64_t Word = State;
            Word = (Word ^ (Word >> 30)) * 0xBF58476D1CE4E5B9ull;
            Word = (Word ^ (Word >> 27)) * 0x94D049BB133111EBull;
            Word ^= Word >> 31;
            for(; Byte < 8 && Index < InSize; ++Byte, ++Index)
            {
                InOutData[Index] ^= uint8_t(Word >> (8 * Byte));
            }
            Byte = 0;
        }
    }

    void Scramble(uint8_t* InOutData, const size_t InSize, const uint64_t InOffset)
    {
        ScrambleRange(InOutData, InSize, InOffset, 0);
    }

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
                   const bool bInNames, const bool bInScramble, const int InZstdLevel, std::string* OutError)
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

    /* Each blob as stored: compressed where that pays, else the input itself. */
    std::vector<const std::vector<uint8_t>*> Stored(Blobs.begin(), Blobs.end());
    std::vector<uint8_t>                     Codecs(Blobs.size(), CODEC_STORED);
    std::vector<std::vector<uint8_t>>        Compressed(Blobs.size());
    if(InZstdLevel > 0)
    {
        /* Every chunk of every blob is a job, so one big texture does not
         * leave the other cores idle. */
        struct Job
        {
            uint32_t Blob;
            uint64_t From;
            size_t   Size;
        };
        std::vector<Job>    Jobs;
        std::vector<size_t> FirstJob(Blobs.size(), 0);
        for(uint32_t Index = 0; Index < Blobs.size(); ++Index)
        {
            FirstJob[Index] = Jobs.size();
            const size_t Size = Blobs[Index]->size();
            if(Size < PACK_MIN_COMPRESS) { continue; }
            for(uint64_t From = 0; From < Size; From += PACK_CHUNK_SIZE)
            {
                Jobs.push_back({ Index, From, size_t(std::min<uint64_t>(PACK_CHUNK_SIZE, Size - From)) });
            }
        }

        const int Level = std::min(InZstdLevel, ZSTD_maxCLevel());
        std::vector<std::vector<uint8_t>> Frames(Jobs.size());
        std::atomic<size_t> Next{ 0 };
        std::atomic<bool>   bFailed{ false };
        const auto Work = [&]()
        {
            ZSTD_CCtx* Encoder = ZSTD_createCCtx();
            for(size_t Index = Next++; Index < Jobs.size() && !bFailed; Index = Next++)
            {
                const Job& Each = Jobs[Index];
                std::vector<uint8_t>& Frame = Frames[Index];
                Frame.resize(ZSTD_compressBound(Each.Size));
                const size_t Written = Encoder ? ZSTD_compressCCtx(Encoder, Frame.data(), Frame.size(),
                                                                   Blobs[Each.Blob]->data() + Each.From, Each.Size, Level)
                                               : size_t(-1);
                if(!Encoder || ZSTD_isError(Written)) { bFailed = true; break; }
                Frame.resize(Written);
            }
            ZSTD_freeCCtx(Encoder);
        };
        const size_t ThreadCount = std::min<size_t>(Jobs.size(), std::max(1u, std::thread::hardware_concurrency()));
        std::vector<std::thread> Threads;
        for(size_t Index = 1; Index < ThreadCount; ++Index) { Threads.emplace_back(Work); }
        Work();
        for(std::thread& Each : Threads) { Each.join(); }
        if(bFailed)
        {
            return Fail("could not compress the content");
        }

        for(uint32_t Index = 0; Index < Blobs.size(); ++Index)
        {
            const size_t End = Index + 1 < Blobs.size() ? FirstJob[Index + 1] : Jobs.size();
            const size_t ChunkCount = End - FirstJob[Index];
            if(ChunkCount == 0) { continue; }
            size_t Size = 4 + 4 * ChunkCount;
            for(size_t JobIndex = FirstJob[Index]; JobIndex < End; ++JobIndex) { Size += Frames[JobIndex].size(); }
            if(Size * 10 > Blobs[Index]->size() * 9) { continue; }   // saves under a tenth: store it

            std::vector<uint8_t>& Out = Compressed[Index];
            Out.reserve(Size);
            Put(Out, ChunkCount, 4);
            for(size_t JobIndex = FirstJob[Index]; JobIndex < End; ++JobIndex) { Put(Out, Frames[JobIndex].size(), 4); }
            for(size_t JobIndex = FirstJob[Index]; JobIndex < End; ++JobIndex)
            {
                Out.insert(Out.end(), Frames[JobIndex].begin(), Frames[JobIndex].end());
                std::vector<uint8_t>().swap(Frames[JobIndex]);
            }
            Stored[Index] = &Out;
            Codecs[Index] = CODEC_ZSTD;
        }
    }

    std::vector<uint8_t> Head;
    Put(Head, PACK_MAGIC, 4);
    Put(Head, PACK_VERSION, 2);
    Put(Head, (bInNames ? PF_Names : 0) | (bInScramble ? PF_Scrambled : 0), 2);
    PutString(Head, InName);
    Put(Head, PACK_CHUNK_SIZE, 4);

    /* Offsets are known once the tables' size is: measure them first. */
    const size_t BlobTable = 4 + Blobs.size() * 25;
    size_t Tail = 4 + Entries.size() * 12 + 4;
    if(bInNames)
    {
        for(const auto& Each : Shown) { Tail += 8 + 4 + Each.second.size(); }
    }
    uint64_t Offset = uint64_t(Head.size() + BlobTable + Tail);

    std::vector<uint64_t> BlobOffsets;
    Put(Head, Blobs.size(), 4);
    for(size_t Index = 0; Index < Blobs.size(); ++Index)
    {
        BlobOffsets.push_back(Offset);
        Put(Head, Offset, 8);
        Put(Head, Stored[Index]->size(), 8);
        Put(Head, Blobs[Index]->size(), 8);
        Put(Head, Codecs[Index], 1);
        Offset += Stored[Index]->size();
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
    std::vector<uint8_t> Scrambled;
    for(size_t Index = 0; Index < Blobs.size(); ++Index)
    {
        const std::vector<uint8_t>* Bytes = Stored[Index];
        if(bInScramble)
        {
            Scrambled = *Bytes;
            Scramble(Scrambled.data(), Scrambled.size(), BlobOffsets[Index]);
            Bytes = &Scrambled;
        }
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
    if(Version != 1 && Version != PACK_VERSION)
    {
        OutError = "pack version " + std::to_string(Version) + "; this build reads 1 to " + std::to_string(PACK_VERSION);
        return nullptr;
    }
    Pack->bScrambled = (Flags & PF_Scrambled) != 0;
    Pack->Name = In.GetString();
    if(Version >= 2)
    {
        Pack->ChunkSize = In.Get(4);
    }

    const uint64_t BlobCount = In.Get(4);
    for(uint64_t Index = 0; Index < BlobCount && !In.bError; ++Index)
    {
        Blob Each;
        Each.Offset     = In.Get(8);
        Each.StoredSize = In.Get(8);
        Each.Size       = Version >= 2 ? In.Get(8) : Each.StoredSize;
        const uint64_t Codec = Version >= 2 ? In.Get(1) : CODEC_STORED;
        if(In.bError || Each.Offset > Pack->Data.size() || Each.StoredSize > Pack->Data.size() - Each.Offset)
        {
            In.bError = true;
            break;
        }
        if(Codec == CODEC_STORED)
        {
            if(Each.Size != Each.StoredSize) { In.bError = true; break; }
        }
        else if(Codec == CODEC_ZSTD)
        {
            /* The chunk table: as many chunks as the decoded size needs, and
             * their frames exactly fill the stored bytes. */
            const uint64_t ChunkCount = Pack->ChunkSize == 0 ? 0 : (Each.Size + Pack->ChunkSize - 1) / Pack->ChunkSize;
            const uint64_t TableSize  = 4 + 4 * ChunkCount;
            if(ChunkCount == 0 || ChunkCount > UINT32_MAX || TableSize > Each.StoredSize) { In.bError = true; break; }
            std::vector<uint8_t> Table(static_cast<size_t>(TableSize));
            Pack->CopyStored(Each, 0, TableSize, Table.data());
            Reader TableIn{ Table };
            if(TableIn.Get(4) != ChunkCount) { In.bError = true; break; }
            Each.FirstChunk = uint32_t(Pack->Chunks.size());
            Each.ChunkCount = uint32_t(ChunkCount);
            uint64_t From = TableSize;
            for(uint64_t Chunk = 0; Chunk < ChunkCount; ++Chunk)
            {
                const uint64_t Size = TableIn.Get(4);
                Pack->Chunks.push_back({ From, uint32_t(Size) });
                From += Size;
            }
            if(From != Each.StoredSize) { In.bError = true; break; }
        }
        else
        {
            OutError = "an entry is stored with codec " + std::to_string(Codec) + ", which this build does not read";
            return nullptr;
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
    return Pack;
}

void PackFileProvider::CopyStored(const Blob& InBlob, const uint64_t InFrom, const uint64_t InSize, uint8_t* OutBytes) const
{
    if(InSize == 0)
    {
        return;
    }
    std::memcpy(OutBytes, Data.data() + InBlob.Offset + InFrom, size_t(InSize));
    if(bScrambled)
    {
        ScrambleRange(OutBytes, size_t(InSize), InBlob.Offset, InFrom);
    }
}

bool PackFileProvider::DecodeChunk(const Blob& InBlob, const uint32_t InIndex, uint8_t* OutBytes, const size_t InSize) const
{
    const Chunk& Each = Chunks[InBlob.FirstChunk + InIndex];
    const uint8_t* Frame = Data.data() + InBlob.Offset + Each.From;
    if(bScrambled)
    {
        thread_local std::vector<uint8_t> Unscrambled;
        Unscrambled.resize(Each.Size);
        CopyStored(InBlob, Each.From, Each.Size, Unscrambled.data());
        Frame = Unscrambled.data();
    }
    ZSTD_DCtx* Decoder = ThreadDecoder();
    const size_t Written = Decoder ? ZSTD_decompressDCtx(Decoder, OutBytes, InSize, Frame, Each.Size) : size_t(-1);
    return !ZSTD_isError(Written) && Written == InSize;
}

bool PackFileProvider::ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes)
{
    const auto Found = Entries.find(HashPackPath(InPath));
    if(Found == Entries.end())
    {
        return false;
    }
    const Blob& Each = Blobs[Found->second];
    OutBytes.resize(size_t(Each.Size));
    if(Each.ChunkCount == 0)
    {
        CopyStored(Each, 0, Each.Size, OutBytes.data());
        return true;
    }
    for(uint32_t Index = 0; Index < Each.ChunkCount; ++Index)
    {
        const uint64_t From = uint64_t(Index) * ChunkSize;
        if(!DecodeChunk(Each, Index, OutBytes.data() + From, size_t(std::min<uint64_t>(ChunkSize, Each.Size - From))))
        {
            CORE_LOG_ERROR("Content pack " + Name + ": '" + InPath + "' does not decompress")
            OutBytes.clear();
            return false;
        }
    }
    return true;
}

bool PackFileProvider::ReadRange(const std::string& InPath, const uint64_t InOffset, const uint64_t InSize, std::vector<uint8_t>& OutBytes)
{
    OutBytes.clear();
    const auto Found = Entries.find(HashPackPath(InPath));
    if(Found == Entries.end())
    {
        return false;
    }
    const Blob& Each = Blobs[Found->second];
    if(InOffset > Each.Size || InSize > Each.Size - InOffset)
    {
        return false;
    }
    OutBytes.resize(size_t(InSize));
    if(Each.ChunkCount == 0)
    {
        CopyStored(Each, InOffset, InSize, OutBytes.data());
        return true;
    }

    /* Only the chunks the range touches; a whole one decodes in place. */
    std::vector<uint8_t> Decoded;
    const uint64_t End = InOffset + InSize;
    for(uint64_t Index = InOffset / ChunkSize; InSize != 0 && Index <= (End - 1) / ChunkSize; ++Index)
    {
        const uint64_t ChunkFrom  = Index * ChunkSize;
        const size_t   ChunkBytes = size_t(std::min<uint64_t>(ChunkSize, Each.Size - ChunkFrom));
        const uint64_t From       = std::max(InOffset, ChunkFrom);
        const uint64_t To         = std::min(End, ChunkFrom + ChunkBytes);
        uint8_t* Target = OutBytes.data() + (From - InOffset);
        bool bDecoded;
        if(From == ChunkFrom && To == ChunkFrom + ChunkBytes)
        {
            bDecoded = DecodeChunk(Each, uint32_t(Index), Target, ChunkBytes);
        }
        else
        {
            Decoded.resize(ChunkBytes);
            bDecoded = DecodeChunk(Each, uint32_t(Index), Decoded.data(), ChunkBytes);
            if(bDecoded) { std::memcpy(Target, Decoded.data() + (From - ChunkFrom), size_t(To - From)); }
        }
        if(!bDecoded)
        {
            CORE_LOG_ERROR("Content pack " + Name + ": '" + InPath + "' does not decompress")
            OutBytes.clear();
            return false;
        }
    }
    return true;
}

bool PackFileProvider::GetEntrySizes(const uint64_t InHash, uint64_t& OutSize, uint64_t& OutStoredSize) const
{
    const auto Found = Entries.find(InHash);
    if(Found == Entries.end())
    {
        return false;
    }
    OutSize       = Blobs[Found->second].Size;
    OutStoredSize = Blobs[Found->second].StoredSize;
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

bool HashedMemoryProvider::ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes)
{
    const auto Found = Files.find(HashPackPath(InPath));
    if(Found == Files.end() || !Found->second)
    {
        return false;
    }
    OutBytes = *Found->second;
    return true;
}

bool HashedMemoryProvider::Exists(const std::string& InPath)
{
    return Files.count(HashPackPath(InPath)) != 0;
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
                 + (Pack->HasNames() ? ", with names" : "") + (Pack->IsScrambled() ? ", scrambled" : ""))
        ++Mounted;
    }
    return Mounted;
}
}
