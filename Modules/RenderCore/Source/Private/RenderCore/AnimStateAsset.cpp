#include "Public/RenderCore/AnimStateAsset.h"

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Private/RenderCore/AssetCacheCommon.h"
#include "Private/RenderCore/AssetFileCommon.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t TAG_STATE      = EngineCore::MakeArchiveTag('N','A','S','T');
        constexpr uint32_t TAG_PARAMETERS = EngineCore::MakeArchiveTag('A','P','R','M');
        constexpr uint32_t TAG_LAYERS     = EngineCore::MakeArchiveTag('A','L','Y','R');
        constexpr uint32_t TAG_LOOK_AT    = EngineCore::MakeArchiveTag('A','L','O','K');
        constexpr uint32_t TAG_RETARGET   = EngineCore::MakeArchiveTag('A','R','T','G');

        /* Counts past these are a corrupt file, not a big anim state. */
        constexpr uint32_t MAX_ITEMS = 4096;

        /* A count, then each element by InEach; refuses an implausible count. */
        template<typename T, typename F>
        void SerializeList(EngineCore::Archive& Ar, std::vector<T>& InOutList, F&& InEach)
        {
            uint32_t Count = uint32_t(InOutList.size());
            Ar << Count;
            if(Ar.IsLoading())
            {
                if(Count > MAX_ITEMS || !CanHoldElements(Ar, Count, sizeof(uint32_t), "anim state: a count is past the end of the file"))
                {
                    Ar.SetError("anim state: an implausible count");
                    return;
                }
                InOutList.assign(Count, T());
            }
            for(T& Each : InOutList)
            {
                if(Ar.HasError()) { return; }
                InEach(Ar, Each);
            }
        }

        template<typename TEnum>
        void SerializeEnum(EngineCore::Archive& Ar, TEnum& InOutValue)
        {
            uint8_t Raw = uint8_t(InOutValue);
            Ar << Raw;
            InOutValue = TEnum(Raw);
        }

        /* InVersion: the ALYR chunk's (AnimStateAsset.h). */
        void SerializeLayer(EngineCore::Archive& Ar, AnimLayer& InOutLayer, const uint16_t InVersion)
        {
            const bool b2D     = InVersion >= 2;
            const bool bPlaced = InVersion >= 4;
            const bool bSync   = InVersion >= 5;
            Ar << InOutLayer.Name << InOutLayer.Weight << InOutLayer.WeightParameter << InOutLayer.EntryState;
            if(InVersion >= 3)
            {
                SerializeEnum(Ar, InOutLayer.Blend);
                SerializeEnum(Ar, InOutLayer.AdditiveBase);
            }
            SerializeList(Ar, InOutLayer.MaskBones, [](EngineCore::Archive& InAr, std::string& InOutBone) { InAr << InOutBone; });
            if(bPlaced) { Ar << InOutLayer.bEditorPlaced << InOutLayer.AnyEditorX << InOutLayer.AnyEditorY; }
            SerializeList(Ar, InOutLayer.States, [b2D, bPlaced, bSync](EngineCore::Archive& InAr, AnimStateNode& InOutState)
            {
                InAr << InOutState.Name << InOutState.BlendParameter << InOutState.Speed;
                if(b2D) { InAr << InOutState.BlendParameterY; }
                if(bPlaced) { InAr << InOutState.EditorX << InOutState.EditorY; }
                SerializeEnum(InAr, InOutState.Loop);
                if(bSync) { InAr << InOutState.SyncGroup; }
                SerializeList(InAr, InOutState.Samples, [b2D](EngineCore::Archive& InSampleAr, AnimBlendSample& InOutSample)
                {
                    InSampleAr << InOutSample.Clip << InOutSample.Position;
                    if(b2D) { InSampleAr << InOutSample.PositionY; }
                });
            });
            SerializeList(Ar, InOutLayer.Transitions, [](EngineCore::Archive& InAr, AnimTransition& InOutTransition)
            {
                InAr << InOutTransition.From << InOutTransition.To << InOutTransition.Duration << InOutTransition.bAtEnd;
                SerializeList(InAr, InOutTransition.Conditions, [](EngineCore::Archive& InConditionAr, AnimCondition& InOutCondition)
                {
                    InConditionAr << InOutCondition.Parameter;
                    SerializeEnum(InConditionAr, InOutCondition.Compare);
                    InConditionAr << InOutCondition.Value;
                });
            });
        }

        /* The optional ALOK chunk: written only when it says anything. */
        void SerializeLookAt(EngineCore::Archive& Ar, AnimLookAt& InOutLookAt)
        {
            if(Ar.IsSaving() && InOutLookAt.Parameter.empty() && InOutLookAt.Bones.empty()) { return; }
            if(Ar.IsLoading() && Ar.PeekChunkTag() != TAG_LOOK_AT) { return; }

            uint16_t Version = 1;
            if(!Ar.OpenChunk(TAG_LOOK_AT, Version)) { return; }
            Ar << InOutLookAt.Parameter << InOutLookAt.WeightParameter << InOutLookAt.MinAngle << InOutLookAt.MaxAngle;
            Ar << InOutLookAt.Axis[0] << InOutLookAt.Axis[1] << InOutLookAt.Axis[2] << InOutLookAt.Response;
            SerializeList(Ar, InOutLookAt.Bones, [](EngineCore::Archive& InAr, AnimLookBone& InOutBone) { InAr << InOutBone.Bone << InOutBone.Share; });
            Ar.CloseChunk();
        }

        /* The optional ARTG chunk: written only when it names a retarget. */
        void SerializeRetargetPath(EngineCore::Archive& Ar, std::string& InOutRetarget)
        {
            if(Ar.IsSaving() && InOutRetarget.empty()) { return; }
            if(Ar.IsLoading() && Ar.PeekChunkTag() != TAG_RETARGET) { return; }

            uint16_t Version = 1;
            if(!Ar.OpenChunk(TAG_RETARGET, Version)) { return; }
            Ar << InOutRetarget;
            Ar.CloseChunk();
        }

        bool IsLookAtValid(const AnimLookAt& InLookAt)
        {
            const float Length2 = InLookAt.Axis[0] * InLookAt.Axis[0] + InLookAt.Axis[1] * InLookAt.Axis[1] + InLookAt.Axis[2] * InLookAt.Axis[2];
            return InLookAt.MinAngle <= InLookAt.MaxAngle && Length2 > 1e-8f && InLookAt.Response >= 0.0f;
        }

        TAssetCache<AnimStateData>& GetCache()
        {
            static TAssetCache<AnimStateData> Cache;
            return Cache;
        }
    }

    int32_t AnimStateData::FindParameter(const std::string& InName) const
    {
        for(size_t Index = 0; Index < Parameters.size(); ++Index)
        {
            if(Parameters[Index].Name == InName) { return int32_t(Index); }
        }
        return -1;
    }

    const char* AnimStateData::FindDefect() const
    {
        for(const AnimStateParameter& Parameter : Parameters)
        {
            if(Parameter.Name.empty()) { return "anim state: a parameter has no name"; }
            if(uint8_t(Parameter.Type) > uint8_t(EAnimParameterType::Trigger)) { return "anim state: a parameter of no known type"; }
        }
        for(const AnimLayer& Layer : Layers)
        {
            if(Layer.States.empty()) { return "anim state: a layer has no states"; }
            if(Layer.EntryState < 0 || size_t(Layer.EntryState) >= Layer.States.size()) { return "anim state: a layer's entry is not one of its states"; }
            if(uint8_t(Layer.Blend) > uint8_t(EAnimLayerBlend::Additive)) { return "anim state: a layer blends in no known way"; }
            if(uint8_t(Layer.AdditiveBase) > uint8_t(EAnimAdditiveBase::ReferencePose)) { return "anim state: an additive layer has no known base"; }
            for(const AnimStateNode& State : Layer.States)
            {
                if(uint8_t(State.Loop) > uint8_t(EAnimLoop::Loop)) { return "anim state: a state loops in no known way"; }
            }
            for(const AnimTransition& Transition : Layer.Transitions)
            {
                if(Transition.From < -1 || Transition.From >= int32_t(Layer.States.size())
                   || Transition.To < 0 || size_t(Transition.To) >= Layer.States.size())
                {
                    return "anim state: a transition names a state its layer lacks";
                }
                for(const AnimCondition& Condition : Transition.Conditions)
                {
                    if(uint8_t(Condition.Compare) > uint8_t(EAnimCompare::Triggered)) { return "anim state: a condition compares in no known way"; }
                }
            }
        }
        if(!IsLookAtValid(LookAt)) { return "anim state: the look-at's range is reversed, its axis zero or its response negative"; }
        for(const AnimLookBone& Bone : LookAt.Bones)
        {
            if(Bone.Bone.empty()) { return "anim state: a look-at bone has no name"; }
        }
        return nullptr;
    }

    bool SerializeAnimState(EngineCore::Archive& Ar, AnimStateData& InOutState)
    {
        if(!OpenRequiredChunk(Ar, TAG_STATE, 1, "not an anim state (no NAST chunk)")) { return false; }

        if(Ar.IsSaving())
        {
            if(const char* Defect = InOutState.FindDefect())
            {
                Ar.SetError(Defect);
                Ar.CloseChunk();
                return false;
            }
        }

        if(OpenRequiredChunk(Ar, TAG_PARAMETERS, 1, "anim state: no APRM chunk"))
        {
            SerializeList(Ar, InOutState.Parameters, [](EngineCore::Archive& InAr, AnimStateParameter& InOutParameter)
            {
                InAr << InOutParameter.Name;
                SerializeEnum(InAr, InOutParameter.Type);
                InAr << InOutParameter.Default;
            });
            Ar.CloseChunk();
        }
        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_LAYERS, 5, "anim state: no ALYR chunk"))
        {
            const uint16_t Version = Ar.GetChunkVersion();
            SerializeList(Ar, InOutState.Layers, [Version](EngineCore::Archive& InAr, AnimLayer& InOutLayer) { SerializeLayer(InAr, InOutLayer, Version); });
            Ar.CloseChunk();
        }
        if(!Ar.HasError()) { SerializeLookAt(Ar, InOutState.LookAt); }
        if(!Ar.HasError()) { SerializeRetargetPath(Ar, InOutState.Retarget); }
        Ar.CloseChunk();

        if(Ar.IsLoading() && !Ar.HasError())
        {
            if(const char* Defect = InOutState.FindDefect())
            {
                Ar.SetError(Defect);
            }
        }
        return !Ar.HasError();
    }

    bool LoadAnimStateFile(const std::string& InPath, AnimStateData& OutState)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        OutState = AnimStateData();
        if(!SerializeAnimState(Ar, OutState))
        {
            RENDERCORE_LOG_WARN("cannot load anim state " + InPath)
            OutState = AnimStateData();
            return false;
        }
        return true;
    }

#if WITH_EDITOR
    bool SaveAnimStateFile(const std::string& InPath, AnimStateData& InState)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        if(!SerializeAnimState(Ar, InState))
        {
            return false;
        }
        return EngineCore::SaveArchiveToFile(InPath, Ar);
    }
#endif

    std::shared_ptr<const AnimStateData> AnimStateAssetCache::Find(const std::string& InPath)
    {
        return GetCache().Find(InPath, &LoadAnimStateFile, "anim state");
    }

    void AnimStateAssetCache::Invalidate(const std::string& InPath) { GetCache().Invalidate(InPath); }
    uint64_t AnimStateAssetCache::GetGeneration()                    { return GetCache().GetGeneration(); }
}
