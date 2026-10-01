#include "Public/GameCore/ArchiveTextDump.h"

#include <cstdio>

/*
 * Walks the byte buffer directly rather than Archive's cursor, so it neither
 * disturbs a load in progress nor stops at the sticky error flag -- a bad save
 * is when this is most useful.
 */

namespace GameCore
{
namespace
{
    /* Archive's header layout: tag(4) + version(2) + size(4). Must change with
     * CoreArchive.cpp's. */
    constexpr size_t CHUNK_HEADER_BYTES = 4 + 2 + 4;

    uint32_t ReadU32(const std::vector<uint8_t>& InBytes, const size_t InAt)
    {
        return uint32_t(InBytes[InAt]) | (uint32_t(InBytes[InAt + 1]) << 8)
             | (uint32_t(InBytes[InAt + 2]) << 16) | (uint32_t(InBytes[InAt + 3]) << 24);
    }

    uint16_t ReadU16(const std::vector<uint8_t>& InBytes, const size_t InAt)
    {
        return uint16_t(uint16_t(InBytes[InAt]) | (uint16_t(InBytes[InAt + 1]) << 8));
    }

    /* Non-printable bytes become '?', so a misaligned read looks wrong. */
    std::string TagToText(const uint32_t InTag)
    {
        std::string Text(4, '?');
        for(int Index = 0; Index < 4; ++Index)
        {
            const char Char = char((InTag >> (8 * (3 - Index))) & 0xFFu);
            Text[size_t(Index)] = (Char >= 32 && Char <= 126) ? Char : '?';
        }
        return Text;
    }

    void Indent(std::string& OutText, const int InDepth)
    {
        OutText.append(size_t(InDepth) * 2, ' ');
    }

    /*
     * A whitelist, not a printable-bytes heuristic: this format is full of
     * strings, and scanning a CMPT payload's "Transform" finds "ofsn" with a
     * plausible size behind it. A tag added elsewhere must be added here. The
     * NimbleEngine save tags are listed too, since this reads whole saves.
     */
    bool IsKnownTag(const uint32_t InTag)
    {
        static const uint32_t KnownTags[] =
        {
            EngineCore::MakeArchiveTag('W','R','L','D'),   // GameCore: a whole World
            EngineCore::MakeArchiveTag('C','M','P','T'),   // GameCore: one component type's pool
            EngineCore::MakeArchiveTag('D','A','T','A'),   // GameCore: one component instance
            EngineCore::MakeArchiveTag('N','M','S','G'),   // NimbleEngine: the save-file root
            EngineCore::MakeArchiveTag('G','M','S','T'),   // NimbleEngine: the game's GameState
            EngineCore::MakeArchiveTag('N','M','S','H'),   // RenderCore: a .nmmesh
            EngineCore::MakeArchiveTag('M','H','D','R'),   // RenderCore: mesh counts and bounds
            EngineCore::MakeArchiveTag('M','V','T','X'),   // RenderCore: mesh vertices
            EngineCore::MakeArchiveTag('M','I','D','X'),   // RenderCore: mesh indices
            EngineCore::MakeArchiveTag('M','S','E','C'),   // RenderCore: mesh sections
            EngineCore::MakeArchiveTag('N','T','E','X'),   // RenderCore: a .nmtexture
            EngineCore::MakeArchiveTag('T','H','D','R'),   // RenderCore: texture size and sampling
            EngineCore::MakeArchiveTag('T','P','I','X'),   // RenderCore: texture pixels
            EngineCore::MakeArchiveTag('I','S','R','C'),   // RenderCore: where an imported asset came from
        };

        for(const uint32_t Known : KnownTags)
        {
            if(InTag == Known) { return true; }
        }
        return false;
    }

    void DumpPayload(const std::vector<uint8_t>& InBytes, size_t InAt, size_t InEnd,
                     int InDepth, std::string& OutText);

    /* Consecutive chunks from InAt to InEnd. Depth-bounded, since a corrupt
     * size can describe a chunk that contains itself. */
    void DumpRange(const std::vector<uint8_t>& InBytes, size_t InAt, const size_t InEnd,
                   const int InDepth, std::string& OutText)
    {
        constexpr int MAX_DEPTH = 16;

        if(InDepth >= MAX_DEPTH)
        {
            Indent(OutText, InDepth);
            OutText += "<nesting deeper than " + std::to_string(MAX_DEPTH) + " levels; stopping>\n";
            return;
        }

        while(InAt < InEnd)
        {
            if(InAt + CHUNK_HEADER_BYTES > InEnd)
            {
                Indent(OutText, InDepth);
                OutText += "<" + std::to_string(InEnd - InAt)
                         + " trailing byte(s), too few for a chunk header>\n";
                return;
            }

            const uint32_t Tag     = ReadU32(InBytes, InAt);
            const uint16_t Version = ReadU16(InBytes, InAt + 4);
            const uint32_t Size    = ReadU32(InBytes, InAt + 6);

            if(!IsKnownTag(Tag))
            {
                /* Report and stop rather than invent a chunk. Also where a pool's
                 * second instance lands: owner indices sit between DATA chunks. */
                Indent(OutText, InDepth);
                OutText += "<" + std::to_string(InEnd - InAt) + " byte(s) from @"
                         + std::to_string(InAt) + " that are not a known chunk>\n";
                return;
            }

            const size_t PayloadAt = InAt + CHUNK_HEADER_BYTES;

            char Line[160];
            std::snprintf(Line, sizeof(Line), "%s  v%u  %u byte(s)  @%zu\n",
                          TagToText(Tag).c_str(), unsigned(Version), unsigned(Size), InAt);
            Indent(OutText, InDepth);
            OutText += Line;

            if(PayloadAt + Size > InEnd)
            {
                /* What a truncated or corrupt save looks like. */
                Indent(OutText, InDepth + 1);
                OutText += "<payload size runs past the end of the enclosing range; stopping>\n";
                return;
            }

            DumpPayload(InBytes, PayloadAt, PayloadAt + Size, InDepth + 1, OutText);

            InAt = PayloadAt + Size;
        }
    }

    /*
     * A payload may start with field bytes before its children (WRLD's entity
     * table precedes its CMPT chunks), and nothing in the format says which
     * payloads have children. So scan for the first known tag whose size fits,
     * and report everything before it as field data.
     */
    void DumpPayload(const std::vector<uint8_t>& InBytes, const size_t InAt, const size_t InEnd,
                     const int InDepth, std::string& OutText)
    {
        if(InEnd <= InAt || InEnd - InAt < CHUNK_HEADER_BYTES) { return; }

        for(size_t Scan = InAt; Scan + CHUNK_HEADER_BYTES <= InEnd; ++Scan)
        {
            const uint32_t Tag  = ReadU32(InBytes, Scan);
            const uint32_t Size = ReadU32(InBytes, Scan + 6);

            if(!IsKnownTag(Tag)) { continue; }
            if(Scan + CHUNK_HEADER_BYTES + Size > InEnd) { continue; }

            if(Scan > InAt)
            {
                Indent(OutText, InDepth);
                OutText += "<" + std::to_string(Scan - InAt) + " byte(s) of field data>\n";
            }

            DumpRange(InBytes, Scan, InEnd, InDepth, OutText);
            return;
        }

        /* A leaf payload -- the normal case for DATA. */
        Indent(OutText, InDepth);
        OutText += "<" + std::to_string(InEnd - InAt) + " byte(s) of field data>\n";
    }
}

bool DumpArchiveToText(const EngineCore::Archive& InArchive, std::string& OutText)
{
    const std::vector<uint8_t>& Bytes = InArchive.GetBytes();

    OutText.clear();
    OutText += "Archive: " + std::to_string(Bytes.size()) + " byte(s), "
             + (InArchive.IsLoading() ? "reading" : "writing")
             + (InArchive.HasError() ? ", FAILED" : "")
             + "\n";

    if(Bytes.empty())
    {
        OutText += "<empty>\n";
        return true;
    }

    DumpRange(Bytes, 0, Bytes.size(), 1, OutText);
    return true;
}
}
