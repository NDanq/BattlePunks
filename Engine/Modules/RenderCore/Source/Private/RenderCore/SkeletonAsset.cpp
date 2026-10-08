#include "Public/RenderCore/SkeletonAsset.h"

#include <unordered_set>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Private/RenderCore/AssetCacheCommon.h"
#include "Private/RenderCore/AssetFileCommon.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t TAG_SKELETON = EngineCore::MakeArchiveTag('N','S','K','L');
        constexpr uint32_t TAG_BONES    = EngineCore::MakeArchiveTag('S','B','O','N');

        /* Why a pose pass over InSkeleton could go wrong; null if it cannot. */
        const char* FindDefect(const SkeletonData& InSkeleton)
        {
            if(InSkeleton.Bones.size() > SKELETON_MAX_BONES) { return "skeleton: more bones than SKELETON_MAX_BONES"; }
            std::unordered_set<std::string> Names;
            for(size_t Index = 0; Index < InSkeleton.Bones.size(); ++Index)
            {
                const SkeletonBone& Bone = InSkeleton.Bones[Index];
                if(Bone.Parent >= int32_t(Index) || Bone.Parent < -1) { return "skeleton: a bone comes before its parent"; }
                if(!Names.insert(Bone.Name).second) { return "skeleton: two bones share a name"; }
            }
            return nullptr;
        }

        TAssetCache<SkeletonData>& GetCache()
        {
            static TAssetCache<SkeletonData> Cache;
            return Cache;
        }
    }

    int32_t SkeletonData::FindBone(const std::string& InName) const
    {
        for(size_t Index = 0; Index < Bones.size(); ++Index)
        {
            if(Bones[Index].Name == InName) { return int32_t(Index); }
        }
        return -1;
    }

    void SkeletonData::ComputeRefPoseModel(std::vector<AnimMatrix>& OutModel) const
    {
        OutModel.resize(Bones.size());
        for(size_t Index = 0; Index < Bones.size(); ++Index)
        {
            const AnimMatrix Local = AnimMath::ToMatrix(Bones[Index].RefPose);
            const int32_t    Parent = Bones[Index].Parent;
            OutModel[Index] = Parent >= 0 ? AnimMath::Mul(OutModel[size_t(Parent)], Local) : Local;
        }
    }

    bool SerializeSkeleton(EngineCore::Archive& Ar, SkeletonData& InOutSkeleton)
    {
        if(!OpenRequiredChunk(Ar, TAG_SKELETON, 1, "not a skeleton (no NSKL chunk)")) { return false; }

        if(Ar.IsSaving())
        {
            if(const char* Defect = FindDefect(InOutSkeleton))
            {
                Ar.SetError(Defect);
                Ar.CloseChunk();
                return false;
            }
        }

        if(OpenRequiredChunk(Ar, TAG_BONES, 1, "skeleton: no SBON chunk"))
        {
            uint32_t Count = uint32_t(InOutSkeleton.Bones.size());
            Ar << Count;
            /* A bone is at least an empty name, a parent and ten floats. */
            if(Ar.IsLoading() && CanHoldElements(Ar, Count, sizeof(uint32_t) + sizeof(int32_t) + 10 * sizeof(float),
                                                 "skeleton: bone count is past the end of the file"))
            {
                InOutSkeleton.Bones.assign(Count, SkeletonBone());
            }
            for(SkeletonBone& Bone : InOutSkeleton.Bones)
            {
                if(Ar.HasError()) { break; }
                Ar << Bone.Name << Bone.Parent;
                SerializeAnimTransform(Ar, Bone.RefPose);
            }
            Ar.CloseChunk();
        }

        if(!Ar.HasError())
        {
            SerializeImportInfo(Ar, InOutSkeleton.ImportInfo);
        }
        Ar.CloseChunk();

        if(Ar.IsLoading() && !Ar.HasError())
        {
            if(const char* Defect = FindDefect(InOutSkeleton))
            {
                Ar.SetError(Defect);
            }
        }
        return !Ar.HasError();
    }

    bool LoadSkeletonFile(const std::string& InPath, SkeletonData& OutSkeleton)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        OutSkeleton = SkeletonData();
        if(!SerializeSkeleton(Ar, OutSkeleton))
        {
            RENDERCORE_LOG_WARN("cannot load skeleton " + InPath)
            OutSkeleton = SkeletonData();
            return false;
        }
        return true;
    }

#if WITH_EDITOR
    bool SaveSkeletonFile(const std::string& InPath, SkeletonData& InSkeleton)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        if(!SerializeSkeleton(Ar, InSkeleton))
        {
            return false;
        }
        return EngineCore::SaveArchiveToFile(InPath, Ar);
    }
#endif

    std::shared_ptr<const SkeletonData> SkeletonAssetCache::Find(const std::string& InPath)
    {
        return GetCache().Find(InPath, &LoadSkeletonFile, "skeleton");
    }

    void SkeletonAssetCache::Invalidate(const std::string& InPath) { GetCache().Invalidate(InPath); }
    uint64_t SkeletonAssetCache::GetGeneration()                    { return GetCache().GetGeneration(); }
}
