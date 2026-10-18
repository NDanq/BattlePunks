#include "Public/RenderCore/Components/AnimationComponent.h"

#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/ScriptLibrary.h"

#include "Public/RenderCore/AnimationSystem.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

/* AnimationComponent: registration, its parameters, and what it exposes to
 * saves, Lua, the editor and replication. ANIMATION.md has the design. */

namespace RenderCore
{
    GAMECORE_DEFINE_COMPONENT(AnimationComponent, "Animation")

    namespace
    {
        bool NameIs(const AnimationParameter& InParam, const char* InName)
        {
            return InParam.Name[0] != '\0' && std::strncmp(InParam.Name, InName, ANIMATION_PARAMETER_NAME_MAX) == 0;
        }

        /* A parameter's fields in saved worlds (and replication): P<i>Name, P<i>Value. */
        struct ParameterFieldNames
        {
            char Name[16], Value[16];
        };

        const ParameterFieldNames& ParameterFields(const size_t InIndex)
        {
            static ParameterFieldNames Names[ANIMATION_PARAMETERS_MAX];
            static bool bMade = false;
            if(!bMade)
            {
                for(size_t Index = 0; Index < ANIMATION_PARAMETERS_MAX; ++Index)
                {
                    std::snprintf(Names[Index].Name,  sizeof(Names[Index].Name),  "P%zuName",  Index);
                    std::snprintf(Names[Index].Value, sizeof(Names[Index].Value), "P%zuValue", Index);
                }
                bMade = true;
            }
            return Names[InIndex];
        }
    }

    int32_t AnimationComponent::FindParameter(const char* InName) const
    {
        if(!InName || InName[0] == '\0') { return -1; }
        for(size_t Index = 0; Index < ANIMATION_PARAMETERS_MAX; ++Index)
        {
            if(NameIs(Parameters[Index], InName)) { return int32_t(Index); }
        }
        return -1;
    }

    float AnimationComponent::GetFloat(const char* InName, const float InDefault) const
    {
        const int32_t Index = FindParameter(InName);
        return Index >= 0 ? Parameters[Index].Value : InDefault;
    }

    bool AnimationComponent::SetFloat(const char* InName, const float InValue)
    {
        if(!InName || InName[0] == '\0' || std::strlen(InName) >= ANIMATION_PARAMETER_NAME_MAX)
        {
            return false;
        }
        int32_t Index = FindParameter(InName);
        for(size_t Free = 0; Index < 0 && Free < ANIMATION_PARAMETERS_MAX; ++Free)
        {
            if(Parameters[Free].Name[0] == '\0')
            {
                std::snprintf(Parameters[Free].Name, sizeof(Parameters[Free].Name), "%s", InName);
                Index = int32_t(Free);
            }
        }
        if(Index < 0)
        {
            return false;
        }
        Parameters[Index].Value = InValue;
        return true;
    }

    /* C.Parameters by slot, up to the last in use, so an empty one between
     * keeps its place, as the Render component's overrides do. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(AnimationComponent, 2)
    {
        Ar.Field("Animation", C.Animation);
        Ar.Field("PlayRate",  C.PlayRate);
        Ar.Flag("Paused",     C.bPaused);
        if(Ar.GetVersion() >= 2) { Ar.Field("Retarget", C.Retarget); }

        int32_t Count = 0;
        for(size_t Index = 0; Index < ANIMATION_PARAMETERS_MAX; ++Index)
        {
            if(C.Parameters[Index].Name[0] != '\0') { Count = int32_t(Index + 1); }
        }
        Ar.Field("ParameterCount", Count);
        if(Count < 0) { Count = 0; }
        if(size_t(Count) > ANIMATION_PARAMETERS_MAX) { Count = int32_t(ANIMATION_PARAMETERS_MAX); }
        for(size_t Index = 0; Index < size_t(Count); ++Index)
        {
            Ar.Field(ParameterFields(Index).Name,  C.Parameters[Index].Name);
            Ar.Field(ParameterFields(Index).Value, C.Parameters[Index].Value);
        }
        if(Ar.IsLoading())
        {
            for(size_t Index = size_t(Count); Index < ANIMATION_PARAMETERS_MAX; ++Index)
            {
                C.Parameters[Index] = AnimationParameter();
            }
        }
    }

    /*
     * WHAT LUA MAY TOUCH: the fields, the parameters by name, and where a
     * bone is. animation:SetFloat("Speed", 300); animation:SetTrigger("Jump");
     * local x, y, z = animation:GetBoneLocation("Hand_R").
     */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(AnimationComponent)
    {
        Bind.Field("Animation", &AnimationComponent::Animation);
        Bind.Field("Retarget",  &AnimationComponent::Retarget);
        Bind.Field("PlayRate",  &AnimationComponent::PlayRate);
        Bind.Flag("Paused",     &AnimationComponent::bPaused);

        Bind.Method("SetFloat", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            const char* Name  = Call.GetString(1);
            const float Value = Call.GetFloat(2);
            if(Call.HasError()) { return; }
            Call.PushBool(C.SetFloat(Name, Value));
        }, "Name:string, Value:number -> Set:bool");
        Bind.Method("SetBool", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            const char* Name   = Call.GetString(1);
            const bool  bValue = Call.GetBool(2);
            if(Call.HasError()) { return; }
            Call.PushBool(C.SetBool(Name, bValue));
        }, "Name:string, Value:bool -> Set:bool");
        Bind.Method("SetTrigger", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            const char* Name = Call.GetString(1);
            if(Call.HasError()) { return; }
            Call.PushBool(C.SetTrigger(Name));
        }, "Name:string -> Set:bool");
        Bind.Pure("GetFloat", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            const char* Name = Call.GetString(1);
            if(Call.HasError()) { return; }
            Call.PushNumber(C.GetFloat(Name));
        }, "Name:string -> Value:number");
        Bind.Pure("GetBool", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            const char* Name = Call.GetString(1);
            if(Call.HasError()) { return; }
            Call.PushBool(C.GetBool(Name));
        }, "Name:string -> Value:bool");

        /* Where InBone is in the world now: its position (three values in
         * Lua, a Vector3 pin in a graph), or Found false. */
        Bind.Pure("GetBoneLocation", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            const char* Bone = Call.GetString(1);
            if(Call.HasError()) { return; }
            GameCore::TransformPose Pose;
            const AnimationSystem* System = AnimationSystem::Find(C.Owner.OwningWorld);
            const bool bFound = System && System->GetBoneWorldPose(C.Owner.Entity, Bone, Pose);
            Call.PushVector3(bFound ? Pose.Position : EngineCore::Vector3());
            Call.PushBool(bFound);
        }, "Bone:string -> Location:Vector3, Found:bool");

        /* An IK chain's goal, in the world (its anim state's chains: the hand on
         * a door, the foot on a pedal), by Weight (0..1, default 1); false if
         * it has no chain so named. It holds until cleared. */
        Bind.Method("SetIKGoal", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            const char* Chain = Call.GetString(1);
            const EngineCore::Vector3 Goal = Call.GetVector3(2);
            const float Weight = Call.HasArg(5) ? Call.GetFloat(5) : 1.0f;
            if(Call.HasError()) { return; }
            AnimationSystem* System = AnimationSystem::Find(C.Owner.OwningWorld);
            AnimInstance* Instance = System ? System->FindInstance(C.Owner.Entity) : nullptr;
            Call.PushBool(Instance && Instance->SetIKGoal(Chain, AnimVec3{ float(Goal.GetX()), float(Goal.GetY()), float(Goal.GetZ()) },
                                                          std::max(Weight, 0.0f)));
        }, "Chain:string, Goal:Vector3, Weight:number -> Set:bool");
        Bind.Method("ClearIKGoal", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            const char* Chain = Call.GetString(1);
            if(Call.HasError()) { return; }
            AnimationSystem* System = AnimationSystem::Find(C.Owner.OwningWorld);
            AnimInstance* Instance = System ? System->FindInstance(C.Owner.Entity) : nullptr;
            Call.PushBool(Instance && Instance->SetIKGoal(Chain, AnimVec3(), 0.0f));
        }, "Chain:string -> Cleared:bool");

        /* The state a layer is in (0: the base layer); "" for a lone clip. */
        Bind.Pure("GetState", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            const int32_t Layer = Call.HasArg(1) ? Call.GetInt(1) : 0;
            if(Call.HasError()) { return; }
            AnimationSystem* System = AnimationSystem::Find(C.Owner.OwningWorld);
            const AnimInstance* Instance = System ? System->FindInstance(C.Owner.Entity) : nullptr;
            Call.PushString(Instance && Layer >= 0 ? Instance->GetCurrentStateName(size_t(Layer)) : "");
        }, "Layer:integer -> State:string");

        /* Whether a layer is still crossfading into its state. */
        Bind.Pure("IsInTransition", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            const int32_t Layer = Call.HasArg(1) ? Call.GetInt(1) : 0;
            if(Call.HasError()) { return; }
            AnimationSystem* System = AnimationSystem::Find(C.Owner.OwningWorld);
            const AnimInstance* Instance = System ? System->FindInstance(C.Owner.Entity) : nullptr;
            Call.PushBool(Instance && Layer >= 0 && Instance->IsInTransition(size_t(Layer)));
        }, "Layer:integer -> Blending:bool");

        /* The clip's playhead in seconds (an anim state's: its first layer's state). */
        Bind.Pure("GetTime", [](AnimationComponent& C, GameCore::ScriptCall& Call)
        {
            AnimationSystem* System = AnimationSystem::Find(C.Owner.OwningWorld);
            const AnimInstance* Instance = System ? System->FindInstance(C.Owner.Entity) : nullptr;
            Call.PushNumber(Instance ? Instance->GetTime() : 0.0);
        }, "-> Seconds:number");
    }

}

/*
 * The animation events, typed: script graphs get On AnimNotify and On
 * AnimEnded nodes with these outputs (Lua keeps Events.On). The animation
 * system emits them (AnimationSystem.h); a game may emit them too.
 */
GAMECORE_DEFINE_SCRIPT_LIBRARY(Animation)
{
    Lib.Event("AnimNotify", "Entity:Entity, Name:string")
       .Tooltip("A clip of the entity's animation passed a notify (mesh viewer, Clip > Notifies)");
    Lib.Event("AnimEnded", "Entity:Entity, State:string, Layer:integer")
       .Tooltip("A state that plays once ended (State \"\" for a lone clip); Layer 0 is the base");
}

namespace RenderCore
{
#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(AnimationComponent)
    {
        Props.TypeCategory("Animation");
        Props.Category("Animation");
        Props.Field("Animation", &AnimationComponent::Animation, GameCore::PF_AssetAnimation);
        Props.Field("Retarget",  &AnimationComponent::Retarget,  GameCore::PF_AssetRetarget);
        Props.Field("Play Rate", &AnimationComponent::PlayRate);
        Props.Flag("Paused",     &AnimationComponent::bPaused);

        /* What it starts with; gameplay sets them as it runs. */
        Props.Category("Parameters");
        Props.StructArray("Parameters", &AnimationComponent::Parameters, [](auto& Member)
        {
            Member.Field("Name",  &AnimationParameter::Name);
            Member.Field("Value", &AnimationParameter::Value);
        });
    }
#endif // WITH_EDITOR

#if WITH_NETCORE
    /*
     * REPLICATION, by hand: the binder takes members, and the parameters are
     * an array. What it plays and its parameters go to clients; each client
     * then animates on its own -- poses are never sent (ANIMATION.md).
     */
    namespace
    {
        struct AnimationReplicationRegistrar
        {
            AnimationReplicationRegistrar()
            {
                GameCore::ReplicatedFieldEntry Entries[4 + 2 * ANIMATION_PARAMETERS_MAX];
                size_t Count = 0;
                const auto Add = [&](const char* InName, const GameCore::EScriptFieldType InType, const size_t InOffset, const size_t InSize)
                {
                    GameCore::ReplicatedFieldEntry& Entry = Entries[Count++];
                    std::snprintf(Entry.Name, sizeof(Entry.Name), "%s", InName);
                    Entry.Kind      = GameCore::EReplicatedKind::Value;
                    Entry.FieldType = InType;
                    Entry.Offset    = uint32_t(InOffset);
                    Entry.Size      = uint32_t(InSize);
                };
                Add("Animation", GameCore::EScriptFieldType::FixedString, offsetof(AnimationComponent, Animation), ANIMATION_PATH_MAX);
                Add("PlayRate",  GameCore::EScriptFieldType::Float,       offsetof(AnimationComponent, PlayRate), sizeof(float));
                Add("Paused",    GameCore::EScriptFieldType::Flag,        offsetof(AnimationComponent, bPaused), sizeof(uint8_t));
                Add("Retarget",  GameCore::EScriptFieldType::FixedString, offsetof(AnimationComponent, Retarget), ANIMATION_PATH_MAX);
                for(size_t Index = 0; Index < ANIMATION_PARAMETERS_MAX; ++Index)
                {
                    const size_t Base = offsetof(AnimationComponent, Parameters) + Index * sizeof(AnimationParameter);
                    Add(ParameterFields(Index).Name,  GameCore::EScriptFieldType::FixedString, Base + offsetof(AnimationParameter, Name),
                        ANIMATION_PARAMETER_NAME_MAX);
                    Add(ParameterFields(Index).Value, GameCore::EScriptFieldType::Float, Base + offsetof(AnimationParameter, Value), sizeof(float));
                }
                static_assert(4 + 2 * ANIMATION_PARAMETERS_MAX <= GameCore::REPLICATED_FIELDS_PER_TYPE_MAX, "one change bit each");
                bRegistered = GameCore::RegisterComponentReplication(AnimationComponent::StaticType().ID, Entries, Count, 0);
            }
            ~AnimationReplicationRegistrar()
            {
                if(bRegistered) { GameCore::UnregisterComponentReplication(AnimationComponent::StaticType().ID); }
            }
            bool bRegistered = false;
        };
        AnimationReplicationRegistrar GAnimationReplicationRegistrar;
    }
#endif // WITH_NETCORE
}
