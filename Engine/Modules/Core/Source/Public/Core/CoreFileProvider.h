#ifndef ENGINECORE_COREFILEPROVIDER_H
#define ENGINECORE_COREFILEPROVIDER_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Public/Core/CoreAPI.h"

/*
 * The resource-policy seam: WHERE an asset comes from.
 *
 * Distinct from CorePlatform.h's app* functions, which are the OS-mechanism
 * seam (HOW bytes are read). Odessa resolves shader paths out of a Qt .qrc
 * bundle compiled into the executable; a standalone or SDL host reads them
 * from disk next to the binary. Both satisfy this interface, and nothing in
 * GDI needs to know which one it got.
 *
 * Paths handed to a provider are bundle-relative and carry no scheme -- e.g.
 * "Shaders/Cooked/Vulkan/Basic.vsh.spv". It is the provider's job to prepend
 * the ":" qrc prefix, a base directory, or whatever else it needs.
 */

namespace EngineCore
{
    class CORE_API IFileProvider
    {
    public:
        virtual ~IFileProvider() = default;

        /* Binary read of the whole resource. False if it could not be read. */
        virtual bool ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes) = 0;

        /* Convenience text read. Bytes are returned verbatim -- no newline or
         * encoding translation, so this is safe to call on anything. */
        virtual bool ReadAllText(const std::string& InPath, std::string& OutText);

        virtual bool Exists(const std::string& InPath) = 0;

        /* InSize bytes of the resource from InOffset: what a streamed asset
         * reads (RenderCore's texture streaming) instead of the whole file.
         * False if the resource is shorter or cannot be read. This default
         * reads it all and keeps the range; a provider that can seek does
         * better. */
        virtual bool ReadRange(const std::string& InPath, uint64_t InOffset, uint64_t InSize, std::vector<uint8_t>& OutBytes);

        /*
         * Optional: a provider that can take extra read-only layers
         * (LayeredFileProvider) mounts InLayer under InName, consulted before
         * the layers mounted earlier; a second mount under the same name
         * replaces the first. False from a provider that cannot. Thread safe
         * where implemented: readers on any thread see a layer whole or not
         * at all.
         */
        virtual bool MountLayer(const std::string& InName, std::shared_ptr<IFileProvider> InLayer) { return false; }
        virtual bool UnmountLayer(const std::string& InName) { return false; }
    };

    /*
     * Plain-filesystem provider, built on appFile*. Resolves against an
     * optional base directory. This is what the smoke tests and a non-Qt host
     * use.
     */
    class CORE_API FilesystemProvider : public IFileProvider
    {
    public:
        FilesystemProvider() = default;
        explicit FilesystemProvider(std::string InBaseDir) : BaseDir(std::move(InBaseDir)) {}

        bool ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes) override;
        bool Exists (const std::string& InPath) override;
        bool ReadRange(const std::string& InPath, uint64_t InOffset, uint64_t InSize, std::vector<uint8_t>& OutBytes) override;

        const std::string& GetBaseDir() const { return BaseDir; }

    private:
        std::string Resolve(const std::string& InPath) const;

        std::string BaseDir;
    };

    /*
     * Read-only files held in memory, by bundle-relative path: what a mounted
     * shader cache is (RenderCore's ShaderCache.h). Immutable once built, so
     * any thread may read it.
     */
    class CORE_API MemoryFileProvider : public IFileProvider
    {
    public:
        using FileBytes = std::shared_ptr<const std::vector<uint8_t>>;

        explicit MemoryFileProvider(std::map<std::string, FileBytes> InFiles) : Files(std::move(InFiles)) {}

        bool ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes) override;
        bool Exists (const std::string& InPath) override;

        size_t GetFileCount() const { return Files.size(); }

    private:
        std::map<std::string, FileBytes> Files;
    };

    /*
     * A base provider plus named layers mounted on top (MountLayer), newest
     * first. With bInBaseFirst the base is asked before every layer instead:
     * the editor's choice, so a material it just cooked to disk beats the
     * cache cooked by the last build. Not owning the base.
     */
    class CORE_API LayeredFileProvider : public IFileProvider
    {
    public:
        LayeredFileProvider(IFileProvider* InBase, const bool bInBaseFirst) : Base(InBase), bBaseFirst(bInBaseFirst) {}

        bool ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes) override;
        bool Exists (const std::string& InPath) override;
        bool ReadRange(const std::string& InPath, uint64_t InOffset, uint64_t InSize, std::vector<uint8_t>& OutBytes) override;

        bool MountLayer(const std::string& InName, std::shared_ptr<IFileProvider> InLayer) override;
        bool UnmountLayer(const std::string& InName) override;

        std::vector<std::string> GetLayerNames() const;

    private:
        struct Layer
        {
            std::string                    Name;
            std::shared_ptr<IFileProvider> Provider;
        };

        /* The order to ask in, taken under the lock; the providers are then
         * read outside it, kept alive by the copies. */
        std::vector<std::shared_ptr<IFileProvider>> Snapshot() const;

        IFileProvider*     Base = nullptr;
        bool               bBaseFirst = false;
        mutable std::mutex Mutex;
        std::vector<Layer> Layers;   // oldest first
    };

    /* Process-wide provider used by GDI's shader loading. Not owned. */
    CORE_API void           SetFileProvider(IFileProvider* InProvider);
    CORE_API IFileProvider* GetFileProvider();

    /* InSize bytes of InPath from InOffset: through GetFileProvider() when it
     * has the file, else the platform layer -- where LoadArchiveFromFile
     * would read it from. False if it is shorter or cannot be read. */
    CORE_API bool ReadFileRange(const std::string& InPath, uint64_t InOffset, uint64_t InSize, std::vector<uint8_t>& OutBytes);
}

#endif //ENGINECORE_COREFILEPROVIDER_H
