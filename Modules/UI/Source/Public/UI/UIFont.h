#ifndef UI_UIFONT_H
#define UI_UIFONT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/Core/CoreImage.h"
#include "Public/RenderCore/UIDrawList.h"
#include "Public/UI/UIAPI.h"
#include "Public/UI/UITypes.h"

namespace RenderCore { class Renderer; }

/*
 * Fonts: a TrueType file baked (stb_truetype) into a glyph atlas per pixel
 * size, ASCII and Latin-1, on first use. Baked at the size text is drawn in
 * physical pixels, so it is sharp at every scale. Each atlas reaches the
 * renderer as a UI image ("ui:font:<path>@<px>", Renderer::SetUIImage) and
 * keeps an opaque white texel, so a document's solid fills batch with its
 * text (UIDrawList::SetSolidFill).
 *
 * The atlas is white with the glyphs in alpha, so UI.fsh's texture times
 * colour is the text colour.
 */
namespace UI
{
    struct UIGlyph
    {
        float X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;   // the quad, pixels from the pen on the baseline
        float U0 = 0, V0 = 0, U1 = 0, V1 = 0;
        float Advance = 0;                       // pixels
        bool  bValid  = false;
    };

    class UI_API UIFont
    {
    public:
        UIFont() = default;
        ~UIFont();
        UIFont(const UIFont&)            = delete;
        UIFont& operator=(const UIFont&) = delete;

        const std::string& GetTextureKey() const { return TextureKey; }
        int   GetPixelSize()  const { return PixelSize; }
        float GetAscent()     const { return Ascent; }       // pixels above the baseline
        float GetLineHeight() const { return LineHeight; }   // pixels from one baseline to the next

        /* InCodepoint's glyph; '?' for one the atlas lacks. */
        const UIGlyph& GetGlyph(uint32_t InCodepoint) const;

        /* The pen's extra move between InLeft and InRight, pixels. */
        float GetKerning(uint32_t InLeft, uint32_t InRight) const;

        /* The white texel's UV. */
        float GetWhiteU() const { return WhiteU; }
        float GetWhiteV() const { return WhiteV; }

        const EngineCore::Image& GetAtlas() const { return Atlas; }

    private:
        friend class UIFontCache;
        struct FontFile;

        std::shared_ptr<FontFile> File;     // the TrueType bytes, shared by its sizes
        std::string       TextureKey;
        int               PixelSize  = 0;
        float             Ascent     = 0.0f;
        float             LineHeight = 0.0f;
        float             KernScale  = 0.0f;
        float             WhiteU = 0.0f, WhiteV = 0.0f;
        UIGlyph           Glyphs[256];      // by codepoint, 0-255
        EngineCore::Image Atlas;            // RGBA8: white, glyphs in alpha
        bool              bUploaded = false;
    };

    /* A run of text laid out in pixels from its top-left. */
    struct UITextLayout
    {
        std::vector<RenderCore::UIVertex> Quads;   // white; 4 per glyph, clockwise from the top-left
        UIVec2                            Size;    // pixels: the widest line, and the lines' height
    };

    class UI_API UIFontCache
    {
    public:
        static constexpr const char* DEFAULT_FONT = "Engine/Content/Fonts/NotoSans-Regular.ttf";

        UIFontCache();
        ~UIFontCache();
        UIFontCache(const UIFontCache&)            = delete;
        UIFontCache& operator=(const UIFontCache&) = delete;

        /* InPath ("" the default) with an em of InPixelSize (clamped 4-256),
         * baked on first use. Null, warned once, if the file is missing or not a font. */
        UIFont* Find(const std::string& InPath, int InPixelSize);

        /*
         * InUtf8 in InFont: lines split at '\n' and, if InWrapPixels > 0, at
         * the last space before that width (or mid-word, for a word too long).
         * Each line placed by InJustify within the widest; InLineHeight scales
         * the font's line height.
         */
        static void Layout(const UIFont& InFont, const std::string& InUtf8, float InWrapPixels, EUIJustify InJustify,
                           float InLineHeight, UITextLayout& OutLayout);

        /* Atlases baked since the last call, to InRenderer (SetUIImage). */
        void Upload(RenderCore::Renderer& InRenderer);

        /* Every atlas sent again on the next Upload (a renderer restart). */
        void MarkAllForUpload();

        /* Drops InPath's atlases (or all, ""): the next Find re-reads the file. */
        void Invalidate(const std::string& InPath);

        size_t GetFontCount() const;

        /* Bumped by Invalidate: text laid out before it lays out again. */
        uint64_t GetGeneration() const;

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //UI_UIFONT_H
