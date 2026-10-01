#ifndef RENDERCORE_SHADERCACHE_H
#define RENDERCORE_SHADERCACHE_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderTypes.h"

/*
 * A module's cooked shaders and material descriptions in one file,
 * Shaders/Cache/<Module>.<Platform>.nmsc, written by
 * cook_material_graphs.py -cache. Mounted, it is a read-only file layer
 * (EngineCore::MemoryFileProvider on the installed LayeredFileProvider) that
 * serves the same paths a loose cook writes -- Shaders/Cooked/<Platform>/...
 * and Materials/<Name>.material -- so nothing that loads a shader or a
 * material knows which it read. See MATERIALS.md, "The shader cache".
 *
 * Format, EngineCore::Archive chunks (little endian):
 *
 *   'NMSC' v1  string Module, string Platform, u32 MaterialTemplateVersion
 *   'BLOB' v1  u64 Hash, u32 Size, Size bytes          one per distinct content
 *   'FILE' v1  string Path, u64 Hash                   one per path
 *
 * Hash is FNV-1a 64 of the bytes; a mismatch fails the read.
 */
namespace RenderCore
{
    struct ShaderCacheContents
    {
        std::string Module;
        std::string Platform;
        uint32_t    TemplateVersion = 0;
        std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> Files;
    };

    /* False, with OutError, for a malformed or corrupt cache. */
    RENDERCORE_API bool ReadShaderCache(std::vector<uint8_t> InBytes, ShaderCacheContents& OutContents, std::string& OutError);

    /* "Shaders/Cache/SandboxGame.Vulkan.nmsc". */
    RENDERCORE_API std::string GetShaderCachePath(const std::string& InModule, ERenderBackend InBackend);

    /* "OpenGL", "Vulkan", "D3D12", "Metal"; "" for Null. */
    RENDERCORE_API const char* GetShaderPlatformName(ERenderBackend InBackend);

    /* The layer name a module's cache is mounted under. */
    RENDERCORE_API std::string GetShaderCacheLayerName(const std::string& InModule);
}

#endif //RENDERCORE_SHADERCACHE_H
