#ifndef RENDERCORE_MATERIALDESCRIPTION_H
#define RENDERCORE_MATERIALDESCRIPTION_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/TextureAsset.h"

/*
 * What a material is to the renderer: <Name>.material (version 2), which
 * NodeGraph's MaterialSchema generates with <Name>.mtl.glsl into the graph's
 * <gen_code> and the cook writes out, and the permutation keys its cooked
 * shaders are named by. See MATERIALS.md.
 *
 * The keys and names here must agree with cook_material_graphs.py, which
 * names the files, and with the templates' MATERIAL_* defines.
 */
namespace RenderCore
{
    /* MaterialCommon.glsl's MATERIAL_TEMPLATE_VERSION: a shader cache cooked
     * against another is refused (ShaderCache.h). */
    constexpr uint32_t MATERIAL_TEMPLATE_VERSION = 3;

    enum class EMaterialDomain : uint8_t { Opaque = 0, Masked = 1, Translucent = 2, PostProcess = 3 };
    enum class EMaterialShading : uint8_t { Lit = 0, Unlit = 1 };
    enum class EMaterialVertexInput : uint8_t { Quad = 0, Mesh = 1, Fullscreen = 2 };

    /* Bits 0-2 domain, 3-4 shading: a pixel stage's key. */
    constexpr uint32_t GetPixelPermutationKey(const EMaterialDomain InDomain, const EMaterialShading InShading)
    {
        return uint32_t(InDomain) | (uint32_t(InShading) << 3);
    }

    /* Bits 8-10 vertex input: a vertex stage's key. */
    constexpr uint32_t GetVertexPermutationKey(const EMaterialVertexInput InInput)
    {
        return uint32_t(InInput) << 8;
    }

    /* "Glow@0000000a.fsh" -- the provider-relative name below
     * Shaders/Cooked/<Platform>/, before the backend's extension. InMaterial
     * may be a stem with folders ("Materials/Glow"). */
    RENDERCORE_API std::string GetPermutationFileName(const std::string& InMaterial, uint32_t InKey, bool bInVertex);

    /* A material is named by its graph's runtime path. */
    constexpr const char* MATERIAL_GRAPH_EXTENSION = ".material.nmgraph";

    /* Where a material's cooked files are, as a stem: the graph path without
     * its extension -- "Materials/Glow.material.nmgraph" is "Materials/Glow":
     * the description "Materials/Glow.material", the permutations
     * "Shaders/Cooked/<Platform>/Materials/Glow@<key>...". A bare name, as
     * worlds and meshes named materials before ("Glow"), is "Materials/Glow".
     * False for anything else (a path to another kind of file). */
    RENDERCORE_API bool GetMaterialStem(const std::string& InMaterial, std::string& OutStem);

    /* The shared vertex shaders a material without World Position Offset draws with. */
    constexpr const char* MATERIAL_DEFAULT_VERTEX = "MaterialDefault";

    /* Bytes 0-31 of every material block: Material_Tint (vec4), then
     * Material_ClipValue, Material_TwoSided and two reserved floats. */
    constexpr uint32_t MATERIAL_ENGINE_BLOCK_BYTES = 32;
    constexpr uint32_t MATERIAL_BLOCK_MAX_BYTES    = 1024;   // GDI_MATERIAL_BLOCK_MAX_BYTES

    struct MaterialParameter
    {
        std::string Name;
        std::string Type;          // float, vec2, vec3 or vec4
        uint32_t    Offset = 0;    // bytes into the material block
        float       Default[4] = {};
    };

    /* A Texture Sample's texture: which material texture slot it fills, from
     * which file, and the sampler settings its node overrides. */
    constexpr uint32_t MATERIAL_TEXTURE_SLOT_FIRST = 1;   // slot 0: a PostProcess material's Scene Color
    constexpr uint32_t MATERIAL_TEXTURE_SLOTS      = 8;   // GDI_MATERIAL_TEXTURE_SLOTS

    struct MaterialTexture
    {
        uint32_t       Slot = MATERIAL_TEXTURE_SLOT_FIRST;
        std::string    Path;                              // runtime-relative .nmtexture
        bool           bFilter = false;                   // false: the texture asset's own
        ETextureFilter Filter  = ETextureFilter::Linear;
        bool           bWrap   = false;                   // false: the texture asset's own
        ETextureWrap   Wrap    = ETextureWrap::Repeat;
    };

    struct RENDERCORE_API MaterialDescription
    {
        EMaterialDomain  Domain     = EMaterialDomain::Opaque;
        EMaterialShading Shading    = EMaterialShading::Lit;
        bool             bTwoSided  = false;
        float            ClipValue  = 0.5f;
        float            MaxOffset  = 0.0f;   // World Position Offset's reach: pads culling bounds
        bool             bHasOffset = false;
        uint32_t         BlockSize  = MATERIAL_ENGINE_BLOCK_BYTES;
        std::vector<MaterialParameter> Parameters;
        std::vector<MaterialTexture>   Textures;

        const MaterialParameter* FindParameter(const std::string& InName) const;
    };

    /* "NimbleMaterial 2" text. False with OutError on an older version, a bad
     * line, or a parameter outside the block. */
    RENDERCORE_API bool ParseMaterialDescription(const std::string& InText, MaterialDescription& OutDescription,
                                                 std::string& OutError);

    /* The block's bytes: InTint and the description's header values, then
     * every parameter's default at its offset. */
    RENDERCORE_API void BuildMaterialBlock(const MaterialDescription& InDescription, EngineCore::Color InTint,
                                           std::vector<uint8_t>& OutBytes);

    RENDERCORE_API const char* GetMaterialDomainName(EMaterialDomain InDomain);
}

#endif //RENDERCORE_MATERIALDESCRIPTION_H
