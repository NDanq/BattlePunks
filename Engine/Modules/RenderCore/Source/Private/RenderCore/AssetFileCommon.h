#ifndef RENDERCORE_ASSETFILECOMMON_H
#define RENDERCORE_ASSETFILECOMMON_H

#include <cstddef>
#include <cstdint>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/AnimMath.h"
#include "Public/RenderCore/AssetImportInfo.h"

/* What the asset files' serializers share. Private to RenderCore. */
namespace RenderCore
{
    constexpr uint32_t TAG_IMPORT_SOURCE = EngineCore::MakeArchiveTag('I','S','R','C');

    /* The optional 'ISRC' chunk. Call at the end of a file's outer chunk. */
    inline void SerializeImportInfo(EngineCore::Archive& Ar, AssetImportInfo& InOutInfo)
    {
        if(Ar.IsSaving() && InOutInfo.IsEmpty()) { return; }
        if(Ar.IsLoading() && Ar.PeekChunkTag() != TAG_IMPORT_SOURCE) { return; }

        uint16_t Version = 1;
        if(!Ar.OpenChunk(TAG_IMPORT_SOURCE, Version)) { return; }
        Ar << InOutInfo.SourcePath;
        Ar << InOutInfo.SourceTime;
        Ar << InOutInfo.Settings;
        Ar.CloseChunk();
    }

    /* Guards a resize from an on-disk count: false (archive failed) if the
     * remaining bytes cannot hold that many. Always true when saving. */
    inline bool CanHoldElements(EngineCore::Archive& Ar, const uint64_t InCount, const size_t InElementBytes,
                                const char* InWhat)
    {
        if(Ar.IsSaving()) { return true; }
        const uint64_t Remaining = uint64_t(Ar.GetSize() - Ar.GetCursor());
        if(InCount > Remaining / InElementBytes)
        {
            Ar.SetError(InWhat);
            return false;
        }
        return true;
    }

    /* Translation, rotation (x y z w), scale: ten floats. */
    inline void SerializeAnimTransform(EngineCore::Archive& Ar, AnimTransform& InOutTransform)
    {
        Ar << InOutTransform.Translation.X << InOutTransform.Translation.Y << InOutTransform.Translation.Z;
        Ar << InOutTransform.Rotation.X << InOutTransform.Rotation.Y << InOutTransform.Rotation.Z << InOutTransform.Rotation.W;
        Ar << InOutTransform.Scale.X << InOutTransform.Scale.Y << InOutTransform.Scale.Z;
    }

    /* A required chunk: opened, or the archive failed with InWhat. */
    inline bool OpenRequiredChunk(EngineCore::Archive& Ar, const uint32_t InTag, const uint16_t InVersion,
                                  const char* InWhat)
    {
        uint16_t Version = InVersion;
        if(!Ar.OpenChunk(InTag, Version))
        {
            Ar.SetError(InWhat);
            return false;
        }
        return true;
    }
}

#endif //RENDERCORE_ASSETFILECOMMON_H
