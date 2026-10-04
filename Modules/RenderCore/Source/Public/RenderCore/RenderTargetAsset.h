#ifndef RENDERCORE_RENDERTARGETASSET_H
#define RENDERCORE_RENDERTARGETASSET_H

#include <cstdint>
#include <memory>
#include <string>

#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/TextureAsset.h"

namespace EngineCore
{
    class Archive;
}

/*
 * Render target textures (RENDERPIPELINE.md, "Scene capture"): a texture a
 * SceneCaptureProbe renders the scene into, sampled by materials as any
 * texture is (the Texture and Reflect nodes).
 *
 *   RenderTargetTex2D  .nmrt2d  one image: a planar mirror's capture
 *   RenderTargetTex3D  .nmrt3d  an omnidirectional (cube) capture: six square
 *                               faces side by side, +X -X +Y -Y +Z -Z, in
 *                               one 6S x S texture (GDI has no cube textures)
 *
 * The file holds its settings and, optionally, a baked capture -- what a
 * material samples until the first live capture (or always, if none is ever
 * made). A target made at run time (CreateTransient) has no file: its path is
 * "transient://<Name>", and it is gone with the run.
 *
 *   NRTA v1
 *     RTHD v1   kind, width, height (a cube's face size twice), filter, wrap, clear colour
 *     RTPX v1   (optional) the baked pixels, RGBA8; a cube's as its 6S x S strip
 */
namespace RenderCore
{
    enum class ERenderTargetKind : uint8_t
    {
        Tex2D = 0,   // RenderTargetTex2D
        Cube  = 1,   // RenderTargetTex3D: omnidirectional
    };

    enum : uint32_t
    {
        RENDER_TARGET_MAX_SIZE     = 4096,   // a side; a cube's face
        RENDER_TARGET_DEFAULT_SIZE = 256,
    };

    struct RenderTargetDesc
    {
        ERenderTargetKind Kind   = ERenderTargetKind::Tex2D;
        uint32_t          Width  = RENDER_TARGET_DEFAULT_SIZE;   // a cube: its face size
        uint32_t          Height = RENDER_TARGET_DEFAULT_SIZE;   // a cube: = Width
        ETextureFilter    Filter = ETextureFilter::Linear;
        ETextureWrap      Wrap   = ETextureWrap::Clamp;
        EngineCore::Color Clear{ 0, 0, 0, 255 };                 // what a capture clears to
    };

    struct RenderTargetData
    {
        RenderTargetDesc  Desc;
        EngineCore::Image Baked;   // RGBA8, or null: no bake
    };

    /* "Content/RenderTargets/Pool.nmrt2d" -> Tex2D, ".nmrt3d" -> Cube; false
     * for anything else (a transient path's kind is in its data). */
    RENDERCORE_API bool KindOfRenderTargetPath(const std::string& InPath, ERenderTargetKind& OutKind);

    /* A path that names a render target: an .nmrt2d, an .nmrt3d or a transient one. */
    RENDERCORE_API bool IsRenderTargetPath(const std::string& InPath);

    /* The texture's own size: a cube's strip is 6 faces wide. */
    inline uint32_t RenderTargetTextureWidth(const RenderTargetDesc& InDesc)
    {
        return InDesc.Kind == ERenderTargetKind::Cube ? InDesc.Width * 6 : InDesc.Width;
    }
    inline uint32_t RenderTargetTextureHeight(const RenderTargetDesc& InDesc)
    {
        return InDesc.Kind == ERenderTargetKind::Cube ? InDesc.Width : InDesc.Height;
    }

    RENDERCORE_API bool SerializeRenderTarget(EngineCore::Archive& Ar, RenderTargetData& InOutTarget);
    RENDERCORE_API bool LoadRenderTargetFile(const std::string& InPath, RenderTargetData& OutTarget);

#if WITH_EDITOR
    RENDERCORE_API bool SaveRenderTargetFile(const std::string& InPath, RenderTargetData& InTarget);
#endif

    /*
     * Render targets by path, as TextureAssetCache is for textures: shared by
     * the game and render threads, thread-safe. Invalidate re-reads a file
     * (an edit or a bake); the renderer notices the new data by its pointer.
     */
    class RENDERCORE_API RenderTargetAssetCache
    {
    public:
        /* The target, loaded on first use; null if it cannot be loaded. */
        static std::shared_ptr<const RenderTargetData> Find(const std::string& InPath);

        /* Forget InPath (or every file-backed one, given an empty path). */
        static void Invalidate(const std::string& InPath);

        /* A target with no file, for this run: "transient://<InName>",
         * replacing one of that name. Materials and probes name it by that
         * path. */
        static std::string CreateTransient(const std::string& InName, const RenderTargetDesc& InDesc);

        /* Forgets a transient target (or all of them, given an empty name). */
        static void DestroyTransient(const std::string& InName);
    };
}

#endif //RENDERCORE_RENDERTARGETASSET_H
