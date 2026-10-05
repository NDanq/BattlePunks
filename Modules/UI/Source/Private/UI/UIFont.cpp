#include "Public/UI/UIFont.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "Public/Core/CoreFileProvider.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/UI/UILog.h"

#include "Private/UI/UIStb.h"

namespace UI
{
    namespace
    {
        /* Rows kept clear at the top of every atlas for the white texel. */
        constexpr int WHITE_ROWS = 4;

        /* The codepoints baked: printable ASCII and Latin-1. */
        constexpr int RANGE_ASCII_FIRST  = 32,  RANGE_ASCII_COUNT  = 95;
        constexpr int RANGE_LATIN_FIRST  = 160, RANGE_LATIN_COUNT  = 96;

        /* InUtf8's next codepoint at InAt (advanced); U+FFFD for a bad sequence. */
        uint32_t NextCodepoint(const std::string& InUtf8, size_t& InAt)
        {
            const auto Byte = [&InUtf8](const size_t InIndex) { return uint8_t(InUtf8[InIndex]); };
            const uint8_t Lead = Byte(InAt++);
            if(Lead < 0x80) return Lead;
            int Extra = 0;
            uint32_t Code = 0;
            if((Lead & 0xE0) == 0xC0)      { Extra = 1; Code = Lead & 0x1F; }
            else if((Lead & 0xF0) == 0xE0) { Extra = 2; Code = Lead & 0x0F; }
            else if((Lead & 0xF8) == 0xF0) { Extra = 3; Code = Lead & 0x07; }
            else return 0xFFFD;
            for(int Each = 0; Each < Extra; ++Each)
            {
                if(InAt >= InUtf8.size() || (Byte(InAt) & 0xC0) != 0x80) return 0xFFFD;
                Code = (Code << 6) | (Byte(InAt++) & 0x3F);
            }
            return Code;
        }
    }

    struct UIFont::FontFile
    {
        std::vector<uint8_t> Bytes;
        stbtt_fontinfo       Info{};
    };

    UIFont::~UIFont() = default;

    const UIGlyph& UIFont::GetGlyph(const uint32_t InCodepoint) const
    {
        if(InCodepoint < 256 && Glyphs[InCodepoint].bValid) return Glyphs[InCodepoint];
        return Glyphs[uint32_t('?')];
    }

    float UIFont::GetKerning(const uint32_t InLeft, const uint32_t InRight) const
    {
        if(!File) return 0.0f;
        return float(stbtt_GetCodepointKernAdvance(&File->Info, int(InLeft), int(InRight))) * KernScale;
    }

    // === UIFontCache ================================================================

    struct UIFontCache::Impl
    {
        std::map<std::string, std::shared_ptr<UIFont::FontFile>> Files;   // by path
        std::map<std::string, std::unique_ptr<UIFont>>           Fonts;   // by "path@px"
        std::set<std::string>                                    Failed;  // paths warned about
        uint64_t                                                 Generation = 1;

        std::shared_ptr<UIFont::FontFile> LoadFile(const std::string& InPath)
        {
            auto Found = Files.find(InPath);
            if(Found != Files.end()) return Found->second;

            auto File = std::make_shared<UIFont::FontFile>();
            EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
            if(!Provider || !Provider->ReadAll(InPath, File->Bytes) || File->Bytes.empty()
               || !stbtt_InitFont(&File->Info, File->Bytes.data(), stbtt_GetFontOffsetForIndex(File->Bytes.data(), 0)))
            {
                if(Failed.insert(InPath).second)
                {
                    UI_LOG_WARN("font '" + InPath + "' is missing or not a TrueType font; text in it is not drawn")
                }
                return nullptr;
            }
            Files[InPath] = File;
            return File;
        }

        std::unique_ptr<UIFont> Bake(const std::string& InPath, const int InPixelSize)
        {
            std::shared_ptr<UIFont::FontFile> File = LoadFile(InPath);
            if(!File) return nullptr;

            stbtt_packedchar Ascii[RANGE_ASCII_COUNT];
            stbtt_packedchar Latin[RANGE_LATIN_COUNT];
            /* Sized by the em, as CSS and every UI toolkit size text: FontSize
             * 24 is a 24 px em, its line a little taller. */
            stbtt_pack_range Ranges[2] = {};
            Ranges[0].font_size = STBTT_POINT_SIZE(float(InPixelSize));
            Ranges[0].first_unicode_codepoint_in_range = RANGE_ASCII_FIRST;
            Ranges[0].num_chars = RANGE_ASCII_COUNT;
            Ranges[0].chardata_for_range = Ascii;
            Ranges[1] = Ranges[0];
            Ranges[1].first_unicode_codepoint_in_range = RANGE_LATIN_FIRST;
            Ranges[1].num_chars = RANGE_LATIN_COUNT;
            Ranges[1].chardata_for_range = Latin;

            /* The smallest power-of-two square (or 2:1) the glyphs pack into. */
            int Width = 128, Height = 128;
            std::vector<uint8_t> Alpha;
            bool bPacked = false;
            for(int Attempt = 0; Attempt < 8 && !bPacked; ++Attempt)
            {
                Alpha.assign(size_t(Width) * size_t(Height), 0);
                stbtt_pack_context Pack;
                if(stbtt_PackBegin(&Pack, Alpha.data() + size_t(WHITE_ROWS) * size_t(Width), Width, Height - WHITE_ROWS, Width, 1, nullptr))
                {
                    stbtt_PackSetOversampling(&Pack, InPixelSize <= 48 ? 2 : 1, 1);
                    bPacked = stbtt_PackFontRanges(&Pack, File->Bytes.data(), 0, Ranges, 2) != 0;
                    stbtt_PackEnd(&Pack);
                }
                if(!bPacked)
                {
                    if(Width == Height) { Width *= 2; } else { Height *= 2; }
                }
            }
            if(!bPacked)
            {
                UI_LOG_WARN("font '" + InPath + "' at " + std::to_string(InPixelSize) + " px does not fit an atlas")
                return nullptr;
            }

            auto Font = std::make_unique<UIFont>();
            Font->File      = File;
            Font->PixelSize = InPixelSize;
            char Key[64];
            std::snprintf(Key, sizeof(Key), "ui:font:%08x@%d", HashName(InPath), InPixelSize);
            Font->TextureKey = Key;

            const float Scale = stbtt_ScaleForMappingEmToPixels(&File->Info, float(InPixelSize));
            int Ascent = 0, Descent = 0, LineGap = 0;
            stbtt_GetFontVMetrics(&File->Info, &Ascent, &Descent, &LineGap);
            Font->Ascent     = std::round(float(Ascent) * Scale);
            Font->LineHeight = std::round(float(Ascent - Descent + LineGap) * Scale);
            Font->KernScale  = Scale;

            const auto Store = [&Font, Width, Height](const int InCodepoint, const stbtt_packedchar& InChar)
            {
                UIGlyph& G = Font->Glyphs[InCodepoint];
                G.X0 = InChar.xoff;  G.Y0 = InChar.yoff;
                G.X1 = InChar.xoff2; G.Y1 = InChar.yoff2;
                G.U0 = float(InChar.x0) / float(Width);
                G.V0 = float(InChar.y0 + WHITE_ROWS) / float(Height);
                G.U1 = float(InChar.x1) / float(Width);
                G.V1 = float(InChar.y1 + WHITE_ROWS) / float(Height);
                G.Advance = InChar.xadvance;
                G.bValid  = true;
            };
            for(int At = 0; At < RANGE_ASCII_COUNT; ++At) { Store(RANGE_ASCII_FIRST + At, Ascii[At]); }
            for(int At = 0; At < RANGE_LATIN_COUNT; ++At) { Store(RANGE_LATIN_FIRST + At, Latin[At]); }

            /* The white texel: a 2 x 2 block, sampled at its middle, clear of filtering. */
            for(int Y = 1; Y <= 2; ++Y)
            {
                for(int X = 1; X <= 2; ++X) { Alpha[size_t(Y) * size_t(Width) + size_t(X)] = 255; }
            }
            Font->WhiteU = 2.0f / float(Width);
            Font->WhiteV = 2.0f / float(Height);

            Font->Atlas = EngineCore::Image(Width, Height, EngineCore::EImageFormat::RGBA8);
            uint8_t* Pixels = Font->Atlas.GetData();
            for(size_t At = 0; At < Alpha.size(); ++At)
            {
                Pixels[At * 4 + 0] = 255;
                Pixels[At * 4 + 1] = 255;
                Pixels[At * 4 + 2] = 255;
                Pixels[At * 4 + 3] = Alpha[At];
            }
            return Font;
        }
    };

    UIFontCache::UIFontCache() : P(new Impl) {}
    UIFontCache::~UIFontCache() { delete P; }

    UIFont* UIFontCache::Find(const std::string& InPath, int InPixelSize)
    {
        const std::string& Path = InPath.empty() ? std::string(DEFAULT_FONT) : InPath;
        InPixelSize = std::clamp(InPixelSize, 4, 256);
        const std::string Key = Path + "@" + std::to_string(InPixelSize);
        auto Found = P->Fonts.find(Key);
        if(Found != P->Fonts.end()) return Found->second.get();
        if(P->Failed.count(Path)) return nullptr;

        std::unique_ptr<UIFont> Baked = P->Bake(Path, InPixelSize);
        if(!Baked) return nullptr;
        UIFont* Made = Baked.get();
        P->Fonts[Key] = std::move(Baked);
        return Made;
    }

    void UIFontCache::Layout(const UIFont& InFont, const std::string& InUtf8, const float InWrapPixels, const EUIJustify InJustify,
                             const float InLineHeight, UITextLayout& OutLayout)
    {
        OutLayout.Quads.clear();
        OutLayout.Size = UIVec2();

        /* Glyphs placed on lines first (pen x per glyph), then justified. */
        struct Placed { uint32_t Code; float X; };
        struct Line   { size_t First = 0, Count = 0; float Width = 0.0f; };
        std::vector<Placed> Glyphs;
        std::vector<Line>   Lines(1);
        float Pen = 0.0f;
        uint32_t Previous = 0;

        const auto EndLine = [&]()
        {
            Line& Current = Lines.back();
            /* Trailing spaces do not count toward a line's width. */
            float Width = 0.0f;
            for(size_t At = Current.First; At < Current.First + Current.Count; ++At)
            {
                if(Glyphs[At].Code != ' ') { Width = Glyphs[At].X + InFont.GetGlyph(Glyphs[At].Code).Advance; }
            }
            Current.Width = Width;
            Lines.push_back(Line{ Glyphs.size(), 0, 0.0f });
            Pen = 0.0f;
            Previous = 0;
        };

        size_t At = 0;
        while(At < InUtf8.size())
        {
            uint32_t Code = NextCodepoint(InUtf8, At);
            if(Code == '\r') continue;
            if(Code == '\n') { EndLine(); continue; }
            if(Code == '\t') Code = ' ';

            /* A word: this glyph up to the next space, measured to see if it fits. */
            if(InWrapPixels > 0.0f && Code != ' ' && (Previous == ' ' || Previous == 0) && Lines.back().Count > 0)
            {
                float WordWidth = 0.0f;
                size_t Peek = At;
                uint32_t PeekCode = Code, PeekPrevious = 0;
                while(true)
                {
                    WordWidth += InFont.GetGlyph(PeekCode).Advance + (PeekPrevious ? InFont.GetKerning(PeekPrevious, PeekCode) : 0.0f);
                    if(Peek >= InUtf8.size()) break;
                    size_t Next = Peek;
                    const uint32_t NextCode = NextCodepoint(InUtf8, Next);
                    if(NextCode == ' ' || NextCode == '\n' || NextCode == '\t') break;
                    PeekPrevious = PeekCode;
                    PeekCode = NextCode;
                    Peek = Next;
                }
                if(Pen + WordWidth > InWrapPixels) { EndLine(); }
            }

            const UIGlyph& Glyph = InFont.GetGlyph(Code);
            if(Previous) { Pen += InFont.GetKerning(Previous, Code); }
            /* A single word wider than the wrap breaks where it must. */
            if(InWrapPixels > 0.0f && Code != ' ' && Lines.back().Count > 0 && Pen + Glyph.Advance > InWrapPixels && Previous != ' ')
            {
                EndLine();
            }
            if(Code == ' ' && Lines.back().Count == 0 && Lines.size() > 1 && InWrapPixels > 0.0f)
            {
                continue;   // a wrapped line does not start with a space
            }
            Glyphs.push_back({ Code, Pen });
            ++Lines.back().Count;
            Pen += Glyph.Advance;
            Previous = Code;
        }
        EndLine();
        Lines.pop_back();   // EndLine's empty successor

        const float LineStep = std::round(InFont.GetLineHeight() * (InLineHeight > 0.0f ? InLineHeight : 1.0f));
        float Widest = 0.0f;
        for(const Line& Each : Lines) { Widest = std::max(Widest, Each.Width); }

        OutLayout.Quads.reserve(Glyphs.size() * 4);
        for(size_t LineIndex = 0; LineIndex < Lines.size(); ++LineIndex)
        {
            const Line& Each = Lines[LineIndex];
            const float Shift = InJustify == EUIJustify::Center ? std::round((Widest - Each.Width) * 0.5f)
                              : InJustify == EUIJustify::Right  ? (Widest - Each.Width) : 0.0f;
            const float Baseline = float(LineIndex) * LineStep + InFont.GetAscent();
            for(size_t G = Each.First; G < Each.First + Each.Count; ++G)
            {
                const UIGlyph& Glyph = InFont.GetGlyph(Glyphs[G].Code);
                if(Glyph.X1 <= Glyph.X0 || Glyph.Y1 <= Glyph.Y0) continue;   // a space
                const float X = Glyphs[G].X + Shift;
                const uint32_t White = 0xFFFFFFFFu;
                OutLayout.Quads.push_back({ X + Glyph.X0, Baseline + Glyph.Y0, Glyph.U0, Glyph.V0, White });
                OutLayout.Quads.push_back({ X + Glyph.X1, Baseline + Glyph.Y0, Glyph.U1, Glyph.V0, White });
                OutLayout.Quads.push_back({ X + Glyph.X1, Baseline + Glyph.Y1, Glyph.U1, Glyph.V1, White });
                OutLayout.Quads.push_back({ X + Glyph.X0, Baseline + Glyph.Y1, Glyph.U0, Glyph.V1, White });
            }
        }
        OutLayout.Size = UIVec2{ std::ceil(Widest), Lines.empty() ? 0.0f : LineStep * float(Lines.size()) };
    }

    void UIFontCache::Upload(RenderCore::Renderer& InRenderer)
    {
        for(auto& Each : P->Fonts)
        {
            UIFont& Font = *Each.second;
            if(!Font.bUploaded)
            {
                InRenderer.SetUIImage(Font.TextureKey.c_str(), Font.Atlas);
                Font.bUploaded = true;
            }
        }
    }

    void UIFontCache::MarkAllForUpload()
    {
        for(auto& Each : P->Fonts) { Each.second->bUploaded = false; }
    }

    void UIFontCache::Invalidate(const std::string& InPath)
    {
        if(InPath.empty())
        {
            P->Fonts.clear();
            P->Files.clear();
            P->Failed.clear();
        }
        else
        {
            const std::string Prefix = InPath + "@";
            for(auto It = P->Fonts.begin(); It != P->Fonts.end();)
            {
                It = It->first.compare(0, Prefix.size(), Prefix) == 0 ? P->Fonts.erase(It) : std::next(It);
            }
            P->Files.erase(InPath);
            P->Failed.erase(InPath);
        }
        ++P->Generation;
    }

    size_t UIFontCache::GetFontCount() const
    {
        return P->Fonts.size();
    }

    uint64_t UIFontCache::GetGeneration() const
    {
        return P->Generation;
    }
}
