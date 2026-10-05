#ifndef RENDERCORE_UIRENDERER_H
#define RENDERCORE_UIRENDERER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Public/RenderCore/UIDrawList.h"

class GDIMesh;
class GDITextureResource;

namespace RenderCore
{
    /*
     * One scene's UI (Renderer::SubmitUIDrawList), render thread only: the
     * packed list it was last given, kept and drawn every frame until the next
     * (RenderContext::DrawUI), and the GPU copy of it.
     *
     * The copy is a dynamic mesh of the scene's own, rewritten only when the
     * list changes: nothing else writes it, so between submits the last
     * update's vertices are still in place and a frame draws them as they are.
     * A static HUD costs no upload at all.
     */
    struct UISceneState
    {
        void*          Block      = nullptr;   // EngineCore::Memory; owned
        size_t         Bytes      = 0;
        uint64_t       Generation = 0;         // bumped by each Set
        UIDrawListView List;                   // Block, parsed; empty if it did not parse

        GDIMesh*       Mesh               = nullptr;
        uint64_t       UploadedGeneration = 0;   // the list Mesh holds; 0 none

        /* Each command's texture for Generation, resolved once (TextureEpoch:
         * RenderContext's count of UI image changes when they were). */
        std::vector<GDITextureResource*> CommandTextures;
        uint64_t                         TexturesGeneration = 0;
        uint64_t                         TexturesEpoch      = 0;

        /* The last list unpacked into the mesh's streams; kept for its capacity. */
        std::vector<float> Positions, UVs, Colors;

        UISceneState() = default;
        UISceneState(const UISceneState&)            = delete;
        UISceneState& operator=(const UISceneState&) = delete;
        ~UISceneState();

        /* Takes InBlock (UIDrawList::Pack, in EngineCore::Memory), or clears
         * with null. A block that does not parse draws nothing. */
        void Set(void* InBlock, size_t InBytes);

        /* The list and the mesh, while the driver is alive (RenderScene::ReleaseAll). */
        void Release();
    };
}

#endif //RENDERCORE_UIRENDERER_H
