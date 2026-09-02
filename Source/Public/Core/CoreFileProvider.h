#ifndef ENGINECORE_COREFILEPROVIDER_H
#define ENGINECORE_COREFILEPROVIDER_H

#include <cstdint>
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

        const std::string& GetBaseDir() const { return BaseDir; }

    private:
        std::string Resolve(const std::string& InPath) const;

        std::string BaseDir;
    };

    /* Process-wide provider used by GDI's shader loading. Not owned. */
    CORE_API void           SetFileProvider(IFileProvider* InProvider);
    CORE_API IFileProvider* GetFileProvider();
}

#endif //ENGINECORE_COREFILEPROVIDER_H
