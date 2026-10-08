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
 * Content is stored once however many paths name it. Little-endian:
 *
 *   u32 'NPAK'  u16 version  u16 flags (1: names)
 *   string name                       (u32 length + UTF-8)
 *   u32 blobs   { u64 offset, u64 size }        from the start of the file
 *   u32 entries { u64 path hash, u32 blob }     sorted by hash
 *   u32 names   { u64 path hash, string path }  0 without PF_Names
 *   blob bytes
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
     * (a debug-info build's pack). False, with OutError, if two paths hash
     * alike or the file cannot be written. */
    CORE_API bool WritePackFile(const std::string& InFile, const std::string& InName, const std::vector<PackInput>& InEntries,
                                bool bInNames, std::string* OutError = nullptr);

    class CORE_API PackFileProvider : public IFileProvider
    {
    public:
        /* InBytes, a whole pack, parsed; null with OutError if it is not one. */
        static std::shared_ptr<PackFileProvider> Load(std::vector<uint8_t> InBytes, std::string& OutError);

        bool ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes) override;
        bool Exists (const std::string& InPath) override;

        const std::string& GetName() const { return Name; }
        size_t             GetEntryCount() const { return Entries.size(); }
        bool               HasNames() const { return !Names.empty(); }

        /* The path behind InHash as it was packed, if this pack carries names;
         * else null. */
        const std::string* FindName(uint64_t InHash) const;

        /* Every entry's hash and, with names, its path; by hash. */
        std::vector<std::pair<uint64_t, std::string>> ListEntries() const;

    private:
        PackFileProvider() = default;

        struct Blob
        {
            uint64_t Offset = 0;
            uint64_t Size   = 0;
        };

        std::string                            Name;
        std::vector<uint8_t>                   Data;      // the whole file
        std::vector<Blob>                      Blobs;
        std::unordered_map<uint64_t, uint32_t> Entries;   // path hash -> blob
        std::unordered_map<uint64_t, std::string> Names;
    };

    /*
     * Every <InDirectory>/*.nmpak, read from disk and mounted on InProvider
     * as "Pack:<file name>": Engine.nmpak first, then the rest by name, so a
     * game's pack overrides the engine's. Returns how many mounted; a pack
     * that will not read is logged and skipped.
     */
    CORE_API int MountPackFiles(IFileProvider& InProvider, const std::string& InDirectory);
}

#endif //ENGINECORE_COREPACK_H
