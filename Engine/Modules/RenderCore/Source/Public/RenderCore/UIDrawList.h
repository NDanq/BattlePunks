#ifndef RENDERCORE_UIDRAWLIST_H
#define RENDERCORE_UIDRAWLIST_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * A screen-space draw list: textured, coloured triangles in physical pixels
 * from the top-left of the view, each run of them with one texture and one
 * clip rectangle (a scissor). What the UI module paints into and
 * Renderer::SubmitUIDrawList draws over a scene, after its post process
 * (RENDERPIPELINE.md, "UI"). No GDI types: RenderCore draws it.
 *
 * Built by appending. A draw takes the texture and clip it names and joins
 * the last command when both match -- the batching: a document whose rects
 * and text share one atlas (SetSolidFill) and one clip is one command, one
 * draw call. Reset() keeps every buffer's capacity, so a list rebuilt each
 * frame stops allocating once it has seen its largest frame.
 *
 * Textures are named by key: "" is white (0), an asset path is that texture
 * (TextureAssetCache), and "ui:..." is an image given to Renderer::SetUIImage.
 */
namespace RenderCore
{
    /* A rectangle, in pixels from the top-left. */
    struct UIRect
    {
        float X = 0.0f, Y = 0.0f, W = 0.0f, H = 0.0f;
    };

    /* A scissor, whole pixels from the top-left. W < 0 is none: unclipped. */
    struct UIClipRect
    {
        int32_t X = 0, Y = 0, W = -1, H = -1;

        bool IsNone() const { return W < 0; }
        bool operator==(const UIClipRect& InOther) const
        {
            return X == InOther.X && Y == InOther.Y && W == InOther.W && H == InOther.H;
        }
        bool operator!=(const UIClipRect& InOther) const { return !(*this == InOther); }
    };

    /* RGBA, 8 bits each, R in the low byte. */
    struct UIVertex
    {
        float    X, Y;
        float    U, V;
        uint32_t Color;
    };

    struct UIDrawCmd
    {
        uint32_t   FirstIndex = 0;
        uint32_t   IndexCount = 0;
        uint32_t   Texture    = 0;   // into GetTextures()
        UIClipRect Clip;
    };

    /* R, G, B, A in 0-1 to a vertex colour. */
    RENDERCORE_API uint32_t PackUIColor(float InR, float InG, float InB, float InA);

    class RENDERCORE_API UIDrawList
    {
    public:
        static constexpr uint32_t WHITE_TEXTURE = 0;

        UIDrawList();

        /* Empty again, capacity kept; the solid fill goes back to white. */
        void Reset();

        /* InKey's index in GetTextures(), added on first use. "" is white. */
        uint32_t FindOrAddTexture(const std::string& InKey);

        /*
         * What AddRect and AddOutline draw with: InTexture at its texel
         * (InU, InV), which must be opaque white -- a font atlas's reserved
         * texel, so solid fills batch with its text. White by default.
         */
        void SetSolidFill(uint32_t InTexture, float InU, float InV);

        /* Clips what follows to InRect, inside the clip already pushed. */
        void PushClip(const UIRect& InRect);
        void PopClip();
        const UIClipRect& GetClip() const { return Clip; }

        /* InRect textured by InUV (0-1) of InTexture, tinted InColor. */
        void AddImage(const UIRect& InRect, const UIRect& InUV, uint32_t InColor, uint32_t InTexture);

        /* A solid rectangle (SetSolidFill). */
        void AddRect(const UIRect& InRect, uint32_t InColor);

        /* A frame InThickness wide, inside InRect. */
        void AddOutline(const UIRect& InRect, float InThickness, uint32_t InColor);

        /*
         * InTexture stretched over InRect without stretching its corners: the
         * texture's InUV split by InUVMargins (left, top, right, bottom, in UV)
         * into 3 x 3, drawn with corners InMargins (pixels) wide. Margins wider
         * than the rectangle shrink to fit.
         */
        void AddNineSlice(const UIRect& InRect, const float InMargins[4], const UIRect& InUV, const float InUVMargins[4],
                          uint32_t InColor, uint32_t InTexture);

        /* InCount vertices (whole quads: 4 each, corners clockwise from the
         * top-left) in InTexture -- a run of glyphs. */
        void AddQuads(const UIVertex* InVertices, uint32_t InCount, uint32_t InTexture);

        /* InOther's draws after this list's, moved by InOffsetX, InOffsetY
         * pixels (whole ones, for its clips), clipped to this list's clip. */
        void Append(const UIDrawList& InOther, float InOffsetX = 0.0f, float InOffsetY = 0.0f);

        bool                            IsEmpty()     const { return Commands.empty(); }
        const std::vector<UIVertex>&    GetVertices() const { return Vertices; }
        const std::vector<uint32_t>&    GetIndices()  const { return Indices; }
        const std::vector<UIDrawCmd>&   GetCommands() const { return Commands; }
        const std::vector<std::string>& GetTextures() const { return Textures; }

        /* As one block for the render thread: PackedSize bytes, by Pack. */
        size_t PackedSize() const;
        void   Pack(void* OutBlock) const;

    private:
        /* The command the next InIndexCount indices of InTexture join. */
        UIDrawCmd& CommandFor(uint32_t InTexture);
        void       AddQuad(float InX0, float InY0, float InX1, float InY1,
                           float InU0, float InV0, float InU1, float InV1, uint32_t InColor, uint32_t InTexture);

        std::vector<UIVertex>    Vertices;
        std::vector<uint32_t>    Indices;
        std::vector<UIDrawCmd>   Commands;
        std::vector<std::string> Textures;
        std::vector<UIClipRect>  ClipStack;
        UIClipRect               Clip;
        uint32_t                 SolidTexture = WHITE_TEXTURE;
        float                    SolidU = 0.5f, SolidV = 0.5f;
    };

    /*
     * A packed list (UIDrawList::Pack) read in place: what the render thread
     * draws. False from Parse if InBlock is not one.
     */
    struct RENDERCORE_API UIDrawListView
    {
        const UIVertex*  Vertices     = nullptr;
        const uint32_t*  Indices      = nullptr;
        const UIDrawCmd* Commands     = nullptr;
        uint32_t         VertexCount  = 0;
        uint32_t         IndexCount   = 0;
        uint32_t         CommandCount = 0;
        std::vector<std::string> Textures;

        bool Parse(const void* InBlock, size_t InBytes);
    };
}

#endif //RENDERCORE_UIDRAWLIST_H
