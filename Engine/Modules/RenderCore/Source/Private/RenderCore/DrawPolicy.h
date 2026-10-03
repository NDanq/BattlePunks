#ifndef RENDERCORE_DRAWPOLICY_H
#define RENDERCORE_DRAWPOLICY_H

#include <algorithm>
#include <cstdint>
#include <vector>

#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderPipeline.h"
#include "Public/RenderCore/RenderTypes.h"

class GDIMesh;

namespace RenderCore
{
    struct RenderMaterial;   // RenderContext.cpp: a loaded material's description and shaders
    struct RenderMaterialParameter;

    /* One draw, queued in its stage's policy. */
    struct DrawCommand
    {
        enum class EKind : uint8_t { Quad, MaterialQuad, MeshSection, Fullscreen };

        EKind             Kind  = EKind::Quad;
        float             Depth = 0.0f;          // view depth, for back-to-front stages
        RenderTransform   Transform;
        float             Width  = 1.0f;
        float             Height = 1.0f;
        EngineCore::Color Tint{255, 255, 255};
        RenderMaterial*   Mat   = nullptr;       // MaterialQuad, Fullscreen; MeshSection (null: Mesh_PS)
        GDIMesh*          Mesh  = nullptr;
        uint32_t          First = 0;
        uint32_t          Count = 0;             // 0: the whole mesh

        /* The entity's parameter overrides; those for MaterialSlot apply. */
        const RenderMaterialParameter* Overrides     = nullptr;
        uint32_t                       OverrideCount = 0;
        uint8_t                        MaterialSlot  = 0;
    };

    enum class EDrawSort : uint8_t
    {
        Submission,     // as queued
        BackToFront,    // farthest first; ties in submission order (coplanar 2D quads keep their layering)
    };

    /* A stage's draws for this frame, and how they are ordered. */
    class DrawPolicy
    {
    public:
        void Init(const ERenderStage InStage, const EDrawSort InSort)
        {
            Stage = InStage;
            Sort  = InSort;
        }

        ERenderStage GetStage() const { return Stage; }
        EDrawSort    GetSort()  const { return Sort; }

        void   Add(const DrawCommand& InDraw) { Draws.push_back(InDraw); }
        void   Clear()                        { Draws.clear(); }
        size_t Num() const                    { return Draws.size(); }
        bool   IsEmpty() const                { return Draws.empty(); }

        /* The draws in issue order. Indices are sorted, not the draws: a
         * DrawCommand's SIMD math is over-aligned, which std::stable_sort's
         * buffer refuses on MSVC. */
        std::vector<uint32_t> GetIssueOrder() const
        {
            std::vector<uint32_t> Order(Draws.size());
            for(uint32_t Index = 0; Index < uint32_t(Order.size()); ++Index) { Order[Index] = Index; }
            if(Sort == EDrawSort::BackToFront)
            {
                const std::vector<DrawCommand>& All = Draws;
                std::stable_sort(Order.begin(), Order.end(), [&All](const uint32_t InA, const uint32_t InB)
                {
                    return All[InA].Depth > All[InB].Depth;
                });
            }
            return Order;
        }

        const DrawCommand& operator[](const size_t InIndex) const { return Draws[InIndex]; }

    private:
        ERenderStage             Stage = ERenderStage::Unlit;
        EDrawSort                Sort  = EDrawSort::Submission;
        std::vector<DrawCommand> Draws;
    };
}

#endif //RENDERCORE_DRAWPOLICY_H
