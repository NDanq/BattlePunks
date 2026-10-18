#include "Public/RenderCore/RetargetAsset.h"

#include <unordered_set>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Private/RenderCore/AssetCacheCommon.h"
#include "Private/RenderCore/AssetFileCommon.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t TAG_RETARGET = EngineCore::MakeArchiveTag('N','R','T','G');
        constexpr uint32_t TAG_HEADER   = EngineCore::MakeArchiveTag('R','H','D','R');
        constexpr uint32_t TAG_SOURCE   = EngineCore::MakeArchiveTag('R','S','R','C');

        /* Counts past these are a corrupt file, not a big retarget. */
        constexpr uint32_t MAX_SOURCES = 1024;
        constexpr uint32_t MAX_BONES   = 4096;

        void SerializeSource(EngineCore::Archive& Ar, RetargetSource& InOutSource)
        {
            Ar << InOutSource.Skeleton << InOutSource.RootScale;
            uint32_t Count = uint32_t(InOutSource.Bones.size());
            Ar << Count;
            /* A bone is at least two empty names, a mode and a quaternion. */
            if(Ar.IsLoading())
            {
                if(Count > MAX_BONES || !CanHoldElements(Ar, Count, 2 * sizeof(uint32_t) + 1 + 4 * sizeof(float),
                                                         "retarget: a bone count is past the end of the file"))
                {
                    Ar.SetError("retarget: an implausible bone count");
                    return;
                }
                InOutSource.Bones.assign(Count, RetargetBoneMap());
            }
            for(RetargetBoneMap& Bone : InOutSource.Bones)
            {
                if(Ar.HasError()) { return; }
                uint8_t Mode = uint8_t(Bone.Translation);
                Ar << Bone.Target << Bone.Source << Mode;
                Bone.Translation = ERetargetTranslation(Mode);
                Ar << Bone.PoseOffset.X << Bone.PoseOffset.Y << Bone.PoseOffset.Z << Bone.PoseOffset.W;
            }
        }

        TAssetCache<RetargetData>& GetCache()
        {
            static TAssetCache<RetargetData> Cache;
            return Cache;
        }
    }

    const RetargetBoneMap* RetargetSource::FindBone(const std::string& InTarget) const
    {
        for(const RetargetBoneMap& Bone : Bones)
        {
            if(Bone.Target == InTarget) { return &Bone; }
        }
        return nullptr;
    }

    const RetargetSource* RetargetData::FindSource(const std::string& InSkeleton) const
    {
        if(InSkeleton.empty()) { return nullptr; }
        for(const RetargetSource& Source : Sources)
        {
            if(Source.Skeleton == InSkeleton) { return &Source; }
        }
        return nullptr;
    }

    const char* RetargetData::FindDefect() const
    {
        if(Target.empty()) { return "retarget: no target skeleton"; }
        std::unordered_set<std::string> Skeletons;
        for(const RetargetSource& Source : Sources)
        {
            if(Source.Skeleton.empty()) { return "retarget: a source names no skeleton"; }
            if(!Skeletons.insert(Source.Skeleton).second) { return "retarget: two sources name one skeleton"; }
            if(!(Source.RootScale >= 0.0f)) { return "retarget: a source's root scale is negative"; }
            std::unordered_set<std::string> Targets;
            for(const RetargetBoneMap& Bone : Source.Bones)
            {
                if(Bone.Target.empty()) { return "retarget: a bone map names no target bone"; }
                if(!Targets.insert(Bone.Target).second) { return "retarget: a source maps one target bone twice"; }
                if(uint8_t(Bone.Translation) > uint8_t(ERetargetTranslation::AnimationRelative)) { return "retarget: a bone's translation is of no known kind"; }
                if(AnimMath::Dot(Bone.PoseOffset, Bone.PoseOffset) < 1e-6f) { return "retarget: a bone's pose offset is not a rotation"; }
            }
        }
        return nullptr;
    }

    bool SerializeRetarget(EngineCore::Archive& Ar, RetargetData& InOutRetarget)
    {
        if(!OpenRequiredChunk(Ar, TAG_RETARGET, 1, "not a retarget (no NRTG chunk)")) { return false; }

        if(Ar.IsSaving())
        {
            if(const char* Defect = InOutRetarget.FindDefect())
            {
                Ar.SetError(Defect);
                Ar.CloseChunk();
                return false;
            }
        }

        uint32_t Sources = uint32_t(InOutRetarget.Sources.size());
        if(OpenRequiredChunk(Ar, TAG_HEADER, 1, "retarget: no RHDR chunk"))
        {
            Ar << InOutRetarget.Target << Sources;
            Ar.CloseChunk();
        }
        if(Ar.IsLoading() && !Ar.HasError())
        {
            if(Sources > MAX_SOURCES) { Ar.SetError("retarget: an implausible source count"); }
            else                      { InOutRetarget.Sources.assign(Sources, RetargetSource()); }
        }
        for(RetargetSource& Source : InOutRetarget.Sources)
        {
            if(Ar.HasError() || !OpenRequiredChunk(Ar, TAG_SOURCE, 1, "retarget: fewer RSRC chunks than sources")) { break; }
            SerializeSource(Ar, Source);
            Ar.CloseChunk();
        }

        if(!Ar.HasError())
        {
            SerializeImportInfo(Ar, InOutRetarget.ImportInfo);
        }
        Ar.CloseChunk();

        if(Ar.IsLoading() && !Ar.HasError())
        {
            for(RetargetSource& Source : InOutRetarget.Sources)
            {
                for(RetargetBoneMap& Bone : Source.Bones) { Bone.PoseOffset = AnimMath::Normalize(Bone.PoseOffset); }
            }
            if(const char* Defect = InOutRetarget.FindDefect())
            {
                Ar.SetError(Defect);
            }
        }
        return !Ar.HasError();
    }

    bool LoadRetargetFile(const std::string& InPath, RetargetData& OutRetarget)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        OutRetarget = RetargetData();
        if(!SerializeRetarget(Ar, OutRetarget))
        {
            RENDERCORE_LOG_WARN("cannot load retarget " + InPath)
            OutRetarget = RetargetData();
            return false;
        }
        return true;
    }

#if WITH_EDITOR
    bool SaveRetargetFile(const std::string& InPath, RetargetData& InRetarget)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        if(!SerializeRetarget(Ar, InRetarget))
        {
            return false;
        }
        return EngineCore::SaveArchiveToFile(InPath, Ar);
    }
#endif

    std::shared_ptr<const RetargetData> RetargetAssetCache::Find(const std::string& InPath)
    {
        return GetCache().Find(InPath, &LoadRetargetFile, "retarget");
    }

    void RetargetAssetCache::Invalidate(const std::string& InPath) { GetCache().Invalidate(InPath); }
    uint64_t RetargetAssetCache::GetGeneration()                    { return GetCache().GetGeneration(); }
}
