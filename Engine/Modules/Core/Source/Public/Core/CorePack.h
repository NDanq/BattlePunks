#ifndef ENGINECORE_COREPACK_H
#define ENGINECORE_COREPACK_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Public/Core/CoreAPI.h"
#include "Public/Core/CoreFileProvider.h"

/*
 * CONTENT PACKS (.nmpak): a packaged game's files, coalesced -- what a shader
 * cache is for shaders, for everything a game reads (config, scripts, cooked
 * worlds, prefabs and UI, meshes, textures, shader caches...).
 *
 * A pack is mounted as a read-only layer of the process's file provider
 * (MountPackFiles), so every loader that reads through GetFileProvider() finds
 * its files there by the same bundle-relative path it always used.
 *
 * NAMES ARE NOT STORED, only HashPackPath of each: a pack's index says nothing
 * about what is in it. A pack made for a debug-info build (Debug,
 * RelWithDebInfo) also carries the path behind each hash, for tools and logs
 * (HasNames, FindName); a Shipping pack does not.
 *
 * A Shipping pack is also SCRAMBLED: each blob XORed with a keystream seeded
 * from its offset, so the paths content names inside itself (a world's
 * prefabs, a material's textures, a script's literals) do not show in a hex
 * viewer either. Undone on read; it deters casual inspection, not a
 * determined one -- the key is in the engine.
 *
 * Content is stored once however many paths name it, and COMPRESSED with
 * zstd when the pack is written with a level (WritePackFile): each blob in
 * chunks of the pack's chunk size, every chunk its own zstd frame, so a range
 * read decodes only the chunks it touches. A blob that would not shrink by a
 * tenth is stored as it is. The scramble is applied to what is stored -- the
 * compressed bytes -- and undone before decoding. Little-endian:
 *
 *   u32 'NPAK'  u16 version (2)  u16 flags (1: names, 2: scrambled)
 *   string name                       (u32 length + UTF-8)
 *   u32 chunk size                    a zstd blob's bytes per chunk, decoded
 *   u32 blobs   { u64 offset, u64 stored size, u64 size, u8 codec }
 *                                     offset from the start of the file;
 *                                     codec 0: stored, 1: zstd chunks
 *   u32 entries { u64 path hash, u32 blob }     sorted by hash
 *   u32 names   { u64 path hash, string path }  0 without PF_Names
 *   blob bytes; a zstd blob's are
 *     u32 chunks, { u32 stored size } per chunk, then each chunk's frame
 *
 * Version 1 (no chunk size, blobs { u64 offset, u64 size }, all stored) still
 * reads.
 */
namespace EngineCore
{
    /* Lower-cased, '/' separated, no leading "./" or "/": how a pack keys a path. */
    CORE_API std::string NormalizePackPath(const std::string& InPath);

    /* FNV-1a 64 of NormalizePackPath(InPath). */
    CORE_API uint64_t HashPackPath(const std::string& InPath);

    struct PackInput
    {
        std::string          Path;    // bundle-relative, as a loader asks for it
        std::vector<uint8_t> Bytes;
    };

    /* Writes InEntries to InFile as pack InName. bInNames: keep each path
     * (a debug-info build's pack); bInScramble: scramble the content (a
     * Shipping build's); InZstdLevel: compress the content at that zstd level
     * (1-22, higher smaller and slower to write, no slower to read), or 0 to
     * store it as it is. Compression runs on every core. False, with
     * OutError, if two paths hash alike, compression fails or the file cannot
     * be written. */
    CORE_API bool WritePackFile(const std::string& InFile, const std::string& InName, const std::vector<PackInput>& InEntries,
                                bool bInNames, bool bInScramble, int InZstdLevel, std::string* OutError = nullptr);

    class CORE_API PackFileProvider : public IFileProvider
    {
    public:
        /* InBytes, a whole pack, parsed; null with OutError if it is not one. */
        static std::shared_ptr<PackFileProvider> Load(std::vector<uint8_t> InBytes, std::string& OutError);

        bool ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes) override;
        bool Exists (const std::string& InPath) override;
        bool ReadRange(const std::string& InPath, uint64_t InOffset, uint64_t InSize, std::vector<uint8_t>& OutBytes) override;

        const std::string& GetName() const { return Name; }
        size_t             GetEntryCount() const { return Entries.size(); }
        bool               HasNames() const { return !Names.empty(); }
        bool               IsScrambled() const { return bScrambled; }

        /* The path behind InHash as it was packed, if this pack carries names;
         * else null. */
        const std::string* FindName(uint64_t InHash) const;

        /* Every entry's hash and, with names, its path; by hash. */
        std::vector<std::pair<uint64_t, std::string>> ListEntries() const;

        /* InHash's entry's size as read (OutSize) and as stored in the pack
         * (OutStoredSize, smaller when compressed); false if no such entry. */
        bool GetEntrySizes(uint64_t InHash, uint64_t& OutSize, uint64_t& OutStoredSize) const;

        /* Whether any entry is compressed. */
        bool IsCompressed() const { return !Chunks.empty(); }

    private:
        PackFileProvider() = default;

        struct Blob
        {
            uint64_t Offset     = 0;   // in Data
            uint64_t StoredSize = 0;   // bytes at Offset
            uint64_t Size       = 0;   // decoded
            uint32_t FirstChunk = 0;   // in Chunks, when ChunkCount
            uint32_t ChunkCount = 0;   // 0: stored as it is
        };

        struct Chunk
        {
            uint64_t From = 0;   // into its blob's stored bytes
            uint32_t Size = 0;   // stored (compressed) bytes
        };

        /* InSize stored bytes of InBlob from InFrom into OutBytes, unscrambled. */
        void CopyStored(const Blob& InBlob, uint64_t InFrom, uint64_t InSize, uint8_t* OutBytes) const;

        /* InBlob's chunk InIndex decoded into OutBytes, which holds exactly
         * that chunk's decoded size; false if it does not decode to it. */
        bool DecodeChunk(const Blob& InBlob, uint32_t InIndex, uint8_t* OutBytes, size_t InSize) const;

        std::string                            Name;
        std::vector<uint8_t>                   Data;      // the whole file
        std::vector<Blob>                      Blobs;
        std::vector<Chunk>                     Chunks;    // every zstd blob's, blob by blob
        uint64_t                               ChunkSize = 0;
        std::unordered_map<uint64_t, uint32_t> Entries;   // path hash -> blob
        std::unordered_map<uint64_t, std::string> Names;
        bool                                   bScrambled = false;
    };

    /* Read-only files in memory keyed by HashPackPath, not by name: a shader
     * cache cooked for Shipping (RenderCore's ShaderCache.h). */
    class CORE_API HashedMemoryProvider : public IFileProvider
    {
    public:
        using FileBytes = std::shared_ptr<const std::vector<uint8_t>>;
        explicit HashedMemoryProvider(std::unordered_map<uint64_t, FileBytes> InFiles) : Files(std::move(InFiles)) {}

        bool ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes) override;
        bool Exists (const std::string& InPath) override;
        size_t GetFileCount() const { return Files.size(); }

    private:
        std::unordered_map<uint64_t, FileBytes> Files;
    };

    /*
     * Every "<InDirectory>/<name>.nmpak", read from disk and mounted on InProvider
     * as "Pack:<file name>": Engine.nmpak first, then the rest by name, so a
     * game's pack overrides the engine's. Returns how many mounted; a pack
     * that will not read is logged and skipped.
     */
    CORE_API int MountPackFiles(IFileProvider& InProvider, const std::string& InDirectory);
}

#endif //ENGINECORE_COREPACK_H
