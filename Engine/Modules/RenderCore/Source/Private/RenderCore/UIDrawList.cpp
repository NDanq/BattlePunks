#include "Public/RenderCore/UIDrawList.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RenderCore
{
    namespace
    {
        /* The packed list: this header, then the vertices, indices and
         * commands, then the texture keys, each NUL-terminated. */
        struct PackedHeader
        {
            uint32_t Magic;
            uint32_t VertexCount;
            uint32_t IndexCount;
            uint32_t CommandCount;
            uint32_t TextureCount;
            uint32_t TextureBytes;
        };
        constexpr uint32_t PACKED_MAGIC = 0x4C44494Eu;   // "NIDL"

        static_assert(sizeof(UIVertex) % 4 == 0 && sizeof(UIDrawCmd) % 4 == 0, "packed back to back, 4-byte aligned");

        uint8_t ToByte(const float InValue)
        {
            return uint8_t(std::lround(std::clamp(InValue, 0.0f, 1.0f) * 255.0f));
        }

        /* InA and InB's overlap; empty (not none) if they miss. */
        UIClipRect Intersect(const UIClipRect& InA, const UIClipRect& InB)
        {
            if(InA.IsNone()) return InB;
            if(InB.IsNone()) return InA;
            const int32_t Left   = std::max(InA.X, InB.X);
            const int32_t Top    = std::max(InA.Y, InB.Y);
            const int32_t Right  = std::min(InA.X + InA.W, InB.X + InB.W);
            const int32_t Bottom = std::min(InA.Y + InA.H, InB.Y + InB.H);
            UIClipRect Out;
            Out.X = Left;
            Out.Y = Top;
            Out.W = std::max(Right - Left, 0);
            Out.H = std::max(Bottom - Top, 0);
            return Out;
        }

        /* Pixels covered by InRect, rounded out to whole pixels. */
        UIClipRect ToClip(const UIRect& InRect)
        {
            UIClipRect Out;
            Out.X = int32_t(std::floor(InRect.X));
            Out.Y = int32_t(std::floor(InRect.Y));
            Out.W = std::max(int32_t(std::ceil(InRect.X + InRect.W)) - Out.X, 0);
            Out.H = std::max(int32_t(std::ceil(InRect.Y + InRect.H)) - Out.Y, 0);
            return Out;
        }
    }

    uint32_t PackUIColor(const float InR, const float InG, const float InB, const float InA)
    {
        return uint32_t(ToByte(InR)) | (uint32_t(ToByte(InG)) << 8) | (uint32_t(ToByte(InB)) << 16) | (uint32_t(ToByte(InA)) << 24);
    }

    UIDrawList::UIDrawList()
    {
        Textures.emplace_back();   // WHITE_TEXTURE
    }

    void UIDrawList::Reset()
    {
        Vertices.clear();
        Indices.clear();
        Commands.clear();
        Textures.resize(1);
        ClipStack.clear();
        Clip         = UIClipRect();
        SolidTexture = WHITE_TEXTURE;
        SolidU       = 0.5f;
        SolidV       = 0.5f;
    }

    uint32_t UIDrawList::FindOrAddTexture(const std::string& InKey)
    {
        /* A handful per list: a scan beats a map, and keeps Reset() allocation free. */
        for(uint32_t At = 0; At < Textures.size(); ++At)
        {
            if(Textures[At] == InKey) return At;
        }
        Textures.push_back(InKey);
        return uint32_t(Textures.size() - 1);
    }

    void UIDrawList::SetSolidFill(const uint32_t InTexture, const float InU, const float InV)
    {
        SolidTexture = InTexture < Textures.size() ? InTexture : WHITE_TEXTURE;
        SolidU       = InU;
        SolidV       = InV;
    }

    void UIDrawList::PushClip(const UIRect& InRect)
    {
        ClipStack.push_back(Clip);
        Clip = Intersect(Clip, ToClip(InRect));
    }

    void UIDrawList::PopClip()
    {
        if(ClipStack.empty()) return;
        Clip = ClipStack.back();
        ClipStack.pop_back();
    }

    UIDrawCmd& UIDrawList::CommandFor(const uint32_t InTexture)
    {
        if(Commands.empty() || Commands.back().Texture != InTexture || Commands.back().Clip != Clip)
        {
            UIDrawCmd Command;
            Command.FirstIndex = uint32_t(Indices.size());
            Command.Texture    = InTexture;
            Command.Clip       = Clip;
            Commands.push_back(Command);
        }
        return Commands.back();
    }

    void UIDrawList::AddQuad(const float InX0, const float InY0, const float InX1, const float InY1,
                             const float InU0, const float InV0, const float InU1, const float InV1,
                             const uint32_t InColor, const uint32_t InTexture)
    {
        if(InX1 <= InX0 || InY1 <= InY0 || (InColor >> 24) == 0) return;
        if(Clip.W == 0 || Clip.H == 0) return;   // clipped away entirely

        UIDrawCmd& Command = CommandFor(InTexture);
        const uint32_t Base = uint32_t(Vertices.size());
        Vertices.push_back({ InX0, InY0, InU0, InV0, InColor });
        Vertices.push_back({ InX1, InY0, InU1, InV0, InColor });
        Vertices.push_back({ InX1, InY1, InU1, InV1, InColor });
        Vertices.push_back({ InX0, InY1, InU0, InV1, InColor });
        const uint32_t Quad[6] = { Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 };
        Indices.insert(Indices.end(), Quad, Quad + 6);
        Command.IndexCount += 6;
    }

    void UIDrawList::AddImage(const UIRect& InRect, const UIRect& InUV, const uint32_t InColor, const uint32_t InTexture)
    {
        AddQuad(InRect.X, InRect.Y, InRect.X + InRect.W, InRect.Y + InRect.H,
                InUV.X, InUV.Y, InUV.X + InUV.W, InUV.Y + InUV.H, InColor, InTexture < Textures.size() ? InTexture : WHITE_TEXTURE);
    }

    void UIDrawList::AddRect(const UIRect& InRect, const uint32_t InColor)
    {
        AddQuad(InRect.X, InRect.Y, InRect.X + InRect.W, InRect.Y + InRect.H,
                SolidU, SolidV, SolidU, SolidV, InColor, SolidTexture);
    }

    void UIDrawList::AddOutline(const UIRect& InRect, const float InThickness, const uint32_t InColor)
    {
        const float T = std::min({ InThickness, InRect.W * 0.5f, InRect.H * 0.5f });
        if(T <= 0.0f) return;
        AddRect({ InRect.X, InRect.Y, InRect.W, T }, InColor);
        AddRect({ InRect.X, InRect.Y + InRect.H - T, InRect.W, T }, InColor);
        AddRect({ InRect.X, InRect.Y + T, T, InRect.H - 2.0f * T }, InColor);
        AddRect({ InRect.X + InRect.W - T, InRect.Y + T, T, InRect.H - 2.0f * T }, InColor);
    }

    void UIDrawList::AddNineSlice(const UIRect& InRect, const float InMargins[4], const UIRect& InUV, const float InUVMargins[4],
                                  const uint32_t InColor, const uint32_t InTexture)
    {
        /* Corners keep their size until they meet; then both sides shrink alike. */
        float Left = std::max(InMargins[0], 0.0f), Top = std::max(InMargins[1], 0.0f);
        float Right = std::max(InMargins[2], 0.0f), Bottom = std::max(InMargins[3], 0.0f);
        if(Left + Right > InRect.W && Left + Right > 0.0f)
        {
            const float Scale = InRect.W / (Left + Right);
            Left *= Scale;
            Right *= Scale;
        }
        if(Top + Bottom > InRect.H && Top + Bottom > 0.0f)
        {
            const float Scale = InRect.H / (Top + Bottom);
            Top *= Scale;
            Bottom *= Scale;
        }

        const float X[4] = { InRect.X, InRect.X + Left, InRect.X + InRect.W - Right, InRect.X + InRect.W };
        const float Y[4] = { InRect.Y, InRect.Y + Top, InRect.Y + InRect.H - Bottom, InRect.Y + InRect.H };
        const float U[4] = { InUV.X, InUV.X + InUVMargins[0], InUV.X + InUV.W - InUVMargins[2], InUV.X + InUV.W };
        const float V[4] = { InUV.Y, InUV.Y + InUVMargins[1], InUV.Y + InUV.H - InUVMargins[3], InUV.Y + InUV.H };
        const uint32_t Texture = InTexture < Textures.size() ? InTexture : WHITE_TEXTURE;
        for(int Row = 0; Row < 3; ++Row)
        {
            for(int Column = 0; Column < 3; ++Column)
            {
                AddQuad(X[Column], Y[Row], X[Column + 1], Y[Row + 1], U[Column], V[Row], U[Column + 1], V[Row + 1], InColor, Texture);
            }
        }
    }

    void UIDrawList::AddQuads(const UIVertex* InVertices, const uint32_t InCount, const uint32_t InTexture)
    {
        if(!InVertices || InCount < 4 || Clip.W == 0 || Clip.H == 0) return;
        const uint32_t Quads = InCount / 4;
        UIDrawCmd& Command = CommandFor(InTexture < Textures.size() ? InTexture : WHITE_TEXTURE);
        uint32_t Base = uint32_t(Vertices.size());
        Vertices.insert(Vertices.end(), InVertices, InVertices + Quads * 4);
        for(uint32_t Quad = 0; Quad < Quads; ++Quad, Base += 4)
        {
            const uint32_t Each[6] = { Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 };
            Indices.insert(Indices.end(), Each, Each + 6);
        }
        Command.IndexCount += Quads * 6;
    }

    void UIDrawList::Append(const UIDrawList& InOther, const float InOffsetX, const float InOffsetY)
    {
        const uint32_t VertexBase = uint32_t(Vertices.size());
        Vertices.insert(Vertices.end(), InOther.Vertices.begin(), InOther.Vertices.end());
        const int32_t ClipX = int32_t(std::lround(InOffsetX)), ClipY = int32_t(std::lround(InOffsetY));
        if(InOffsetX != 0.0f || InOffsetY != 0.0f)
        {
            for(size_t At = VertexBase; At < Vertices.size(); ++At)
            {
                Vertices[At].X += float(ClipX);
                Vertices[At].Y += float(ClipY);
            }
        }
        for(const UIDrawCmd& Each : InOther.Commands)
        {
            const UIClipRect Saved = Clip;
            UIClipRect Moved = Each.Clip;
            if(!Moved.IsNone()) { Moved.X += ClipX; Moved.Y += ClipY; }
            Clip = Intersect(Saved, Moved);
            if(Clip.W != 0 && Clip.H != 0 && Each.IndexCount > 0)
            {
                UIDrawCmd& Command = CommandFor(FindOrAddTexture(InOther.Textures[Each.Texture]));
                for(uint32_t At = 0; At < Each.IndexCount; ++At)
                {
                    Indices.push_back(InOther.Indices[Each.FirstIndex + At] + VertexBase);
                }
                Command.IndexCount += Each.IndexCount;
            }
            Clip = Saved;
        }
    }

    size_t UIDrawList::PackedSize() const
    {
        size_t TextureBytes = 0;
        for(const std::string& Each : Textures) { TextureBytes += Each.size() + 1; }
        return sizeof(PackedHeader) + Vertices.size() * sizeof(UIVertex) + Indices.size() * sizeof(uint32_t)
             + Commands.size() * sizeof(UIDrawCmd) + TextureBytes;
    }

    void UIDrawList::Pack(void* OutBlock) const
    {
        auto* At = static_cast<uint8_t*>(OutBlock);
        PackedHeader Header;
        Header.Magic        = PACKED_MAGIC;
        Header.VertexCount  = uint32_t(Vertices.size());
        Header.IndexCount   = uint32_t(Indices.size());
        Header.CommandCount = uint32_t(Commands.size());
        Header.TextureCount = uint32_t(Textures.size());
        Header.TextureBytes = 0;
        for(const std::string& Each : Textures) { Header.TextureBytes += uint32_t(Each.size() + 1); }

        const auto Put = [&At](const void* InData, const size_t InBytes)
        {
            if(InBytes) std::memcpy(At, InData, InBytes);
            At += InBytes;
        };
        Put(&Header, sizeof(Header));
        Put(Vertices.data(), Vertices.size() * sizeof(UIVertex));
        Put(Indices.data(), Indices.size() * sizeof(uint32_t));
        Put(Commands.data(), Commands.size() * sizeof(UIDrawCmd));
        for(const std::string& Each : Textures) { Put(Each.c_str(), Each.size() + 1); }
    }

    bool UIDrawListView::Parse(const void* InBlock, const size_t InBytes)
    {
        *this = UIDrawListView();
        if(!InBlock || InBytes < sizeof(PackedHeader)) return false;

        PackedHeader Header;
        std::memcpy(&Header, InBlock, sizeof(Header));
        const size_t Fixed = sizeof(PackedHeader) + size_t(Header.VertexCount) * sizeof(UIVertex)
                           + size_t(Header.IndexCount) * sizeof(uint32_t) + size_t(Header.CommandCount) * sizeof(UIDrawCmd);
        if(Header.Magic != PACKED_MAGIC || Fixed + Header.TextureBytes != InBytes) return false;

        const auto* At = static_cast<const uint8_t*>(InBlock) + sizeof(PackedHeader);
        Vertices = reinterpret_cast<const UIVertex*>(At);
        At += size_t(Header.VertexCount) * sizeof(UIVertex);
        Indices = reinterpret_cast<const uint32_t*>(At);
        At += size_t(Header.IndexCount) * sizeof(uint32_t);
        Commands = reinterpret_cast<const UIDrawCmd*>(At);
        At += size_t(Header.CommandCount) * sizeof(UIDrawCmd);

        const char* Text = reinterpret_cast<const char*>(At);
        const char* End  = Text + Header.TextureBytes;
        while(Text < End && Textures.size() < Header.TextureCount)
        {
            const size_t Length = strnlen(Text, size_t(End - Text));
            if(Text + Length >= End) return false;   // unterminated
            Textures.emplace_back(Text, Length);
            Text += Length + 1;
        }
        if(Textures.size() != Header.TextureCount) return false;

        /* Every command's range and texture in bounds, every index a vertex. */
        for(uint32_t Each = 0; Each < Header.CommandCount; ++Each)
        {
            const UIDrawCmd& Command = Commands[Each];
            if(Command.Texture >= Header.TextureCount || Command.FirstIndex > Header.IndexCount
               || Command.IndexCount > Header.IndexCount - Command.FirstIndex)
            {
                return false;
            }
        }
        for(uint32_t Each = 0; Each < Header.IndexCount; ++Each)
        {
            if(Indices[Each] >= Header.VertexCount) return false;
        }

        VertexCount  = Header.VertexCount;
        IndexCount   = Header.IndexCount;
        CommandCount = Header.CommandCount;
        return true;
    }
}
