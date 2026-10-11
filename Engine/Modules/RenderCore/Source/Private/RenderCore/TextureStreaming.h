#ifndef RENDERCORE_TEXTURESTREAMING_H
#define RENDERCORE_TEXTURESTREAMING_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/TextureAsset.h"

/*
 * TEXTURE STREAMING (TEXTURE_STREAMING.md): a material texture's mips on the
 * GPU by how near what samples it is.
 *
 * Each streamed texture keeps levels [ResidentTop, last] -- always its small
 * tail (MinResident), more as draws ask. A draw reports how many screen
 * pixels one UV unit of its mesh spans where it is (NoteDemand); Update turns
 * the largest demand of the frame into the level whose texels match the
 * pixels, reads the missing levels on the streaming thread (one range read:
 * LoadTextureLevels), and commits them on the render thread under an upload
 * budget, as a new TextureData -- RenderContext rebuilds the GPU texture
 * when the pointer changes. The pool budget raises every wanted level alike
 * until the wanted set fits, and a texture with more than it wants drops
 * levels after DropDelay, or at once when the pool is over.
 *
 * Render thread only, but for the streaming thread, which works on copies.
 */
namespace RenderCore
{
    /* Exported for RenderCoreSmokeTest only. */
    class RENDERCORE_API TextureStreamer
    {
    public:
        struct Entry;

        TextureStreamer();
        ~TextureStreamer();

        TextureStreamer(const TextureStreamer&)            = delete;
        TextureStreamer& operator=(const TextureStreamer&) = delete;

        /* [Rendering] TextureStreaming. */
        static bool IsEnabled();

        /*
         * InPath's entry, made on first use: its layout read and its
         * smallest levels loaded at once. Null if it is not streamed --
         * streaming off, a render target, no mips, already small, or a file
         * that cannot be laid out -- and the caller loads it whole
         * (TextureAssetCache). The address is stable until ReleaseAll.
         */
        Entry* FindEntry(const std::string& InPath);

        /* The levels the GPU should hold now; its level 0 is ResidentTop. */
        static const std::shared_ptr<const TextureData>& GetResident(const Entry& InEntry);
        /* The file's level that resident level 0 is; the level it wants now. */
        static int GetResidentTop(const Entry& InEntry);
        static int GetWanted(const Entry& InEntry);

        /* A draw sampling it this frame: InPixelsPerUV, screen pixels one UV
         * unit spans where it is drawn; zero or less: not known, so all of it. */
        static void NoteDemand(Entry& InEntry, float InPixelsPerUV);

        /* Once a frame, before the draws queue: commits what loaded, turns
         * the last frame's demand into wanted levels, drops and requests. */
        void Update();

        /* Loads and commits every level the last Update wanted, ignoring
         * the upload budget, before returning: for tests and one-off renders. */
        void Flush();

        /* Stops the streaming thread; forgets every entry. */
        void ReleaseAll();

        /* The render thread's time making GPU textures from resident sets
         * this frame (RenderContext's): shown with the stats. */
        void NoteCreateMs(float InMs);

        TextureStreamingStats GetStats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> P;
    };
}

#endif //RENDERCORE_TEXTURESTREAMING_H
