#include "Public/RenderCore/Components/RenderComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

#include "Public/Core/CoreTypes.h"
#include "Private/RenderCore/RenderComponentProxy.h"

#include <cstdio>
#include <cstring>

/* RenderComponent: registration, its proxy factory, and what it exposes to
 * saves, Lua and the editor. */

namespace RenderCore
{
    RENDERCORE_DEFINE_PROXY_COMPONENT(RenderComponent, "Render", RenderComponentProxy)

    CreateProxyFn GetRenderComponentFactory()
    {
        return &RenderComponent_CreateProxy;
    }

    namespace
    {
        /* RenderComponent's slots: in saved worlds and Lua, and in the editor. */
        const char* const MATERIAL_SLOT_FIELDS[RENDER_MATERIAL_SLOTS] = {
            "Material0", "Material1", "Material2", "Material3", "Material4", "Material5", "Material6", "Material7" };

        /* A parameter override's fields in saved worlds: Parameter<i><Field>. */
        struct ParameterFieldNames
        {
            char Name[24], Slot[24], X[24], Y[24], Z[24], W[24];
        };

        const ParameterFieldNames& ParameterFields(const size_t InIndex)
        {
            static ParameterFieldNames Names[RENDER_MATERIAL_PARAMETERS_MAX];
            static bool bMade = false;
            if(!bMade)
            {
                for(size_t Index = 0; Index < RENDER_MATERIAL_PARAMETERS_MAX; ++Index)
                {
                    ParameterFieldNames& N = Names[Index];
                    std::snprintf(N.Name, sizeof(N.Name), "Parameter%zuName", Index);
                    std::snprintf(N.Slot, sizeof(N.Slot), "Parameter%zuSlot", Index);
                    std::snprintf(N.X,    sizeof(N.X),    "Parameter%zuX",    Index);
                    std::snprintf(N.Y,    sizeof(N.Y),    "Parameter%zuY",    Index);
                    std::snprintf(N.Z,    sizeof(N.Z),    "Parameter%zuZ",    Index);
                    std::snprintf(N.W,    sizeof(N.W),    "Parameter%zuW",    Index);
                }
                bMade = true;
            }
            return Names[InIndex];
        }

        bool ValidSlot(const int32_t InSlot) { return InSlot >= 0 && size_t(InSlot) < RENDER_MATERIAL_SLOTS; }

        bool NameIs(const RenderMaterialParameter& InParam, const char* InName)
        {
            return InParam.Name[0] != '\0' && std::strncmp(InParam.Name, InName, RENDER_MATERIAL_PARAMETER_NAME_MAX) == 0;
        }
    }

    // === Materials and their parameters ========================================

    const char* RenderComponent::GetMaterial(const int32_t InSlot) const
    {
        return ValidSlot(InSlot) ? Materials[InSlot] : "";
    }

    bool RenderComponent::SetMaterial(const int32_t InSlot, const char* InMaterial)
    {
        if(!ValidSlot(InSlot))
        {
            return false;
        }
        std::snprintf(Materials[InSlot], RENDER_MATERIAL_PATH_MAX, "%s", InMaterial ? InMaterial : "");
        return true;
    }

    int32_t RenderComponent::GetNumRegisteredMaterials() const
    {
        int32_t Count = 0;
        for(const char (&Slot)[RENDER_MATERIAL_PATH_MAX] : Materials)
        {
            if(Slot[0] != '\0') { ++Count; }
        }
        return Count;
    }

    bool RenderComponent::SetMaterialParameter(const int32_t InSlot, const char* InName,
                                               const float InX, const float InY, const float InZ, const float InW)
    {
        if(!ValidSlot(InSlot) || !InName || InName[0] == '\0' || std::strlen(InName) >= RENDER_MATERIAL_PARAMETER_NAME_MAX)
        {
            return false;
        }
        /* The existing override, else the first free entry. An editor may
         * leave a gap, so neither is assumed to be at the end. */
        RenderMaterialParameter* Target = nullptr;
        for(RenderMaterialParameter& Each : Parameters)
        {
            if(Each.Slot == uint8_t(InSlot) && NameIs(Each, InName)) { Target = &Each; break; }
        }
        for(size_t Index = 0; !Target && Index < RENDER_MATERIAL_PARAMETERS_MAX; ++Index)
        {
            if(Parameters[Index].Name[0] == '\0')
            {
                Target = &Parameters[Index];
                std::snprintf(Target->Name, sizeof(Target->Name), "%s", InName);
                Target->Slot = uint8_t(InSlot);
            }
        }
        if(!Target)
        {
            return false;
        }
        Target->Value[0] = InX;
        Target->Value[1] = InY;
        Target->Value[2] = InZ;
        Target->Value[3] = InW;
        return true;
    }

    bool RenderComponent::GetMaterialParameter(const int32_t InSlot, const char* InName, float OutValue[4]) const
    {
        if(!InName) { return false; }
        for(const RenderMaterialParameter& Each : Parameters)
        {
            if(Each.Slot == uint8_t(InSlot) && NameIs(Each, InName))
            {
                std::memcpy(OutValue, Each.Value, sizeof(Each.Value));
                return true;
            }
        }
        return false;
    }

    bool RenderComponent::ClearMaterialParameter(const int32_t InSlot, const char* InName)
    {
        if(!InName) { return false; }
        for(size_t Index = 0; Index < RENDER_MATERIAL_PARAMETERS_MAX; ++Index)
        {
            if(Parameters[Index].Slot == uint8_t(InSlot) && NameIs(Parameters[Index], InName))
            {
                /* Keep them packed. */
                for(size_t Next = Index + 1; Next < RENDER_MATERIAL_PARAMETERS_MAX; ++Next)
                {
                    Parameters[Next - 1] = Parameters[Next];
                }
                Parameters[RENDER_MATERIAL_PARAMETERS_MAX - 1] = RenderMaterialParameter();
                return true;
            }
        }
        return false;
    }

    int32_t RenderComponent::GetMaterialParameterCount() const
    {
        int32_t Count = 0;
        for(const RenderMaterialParameter& Each : Parameters)
        {
            if(Each.Name[0] != '\0') { ++Count; }
        }
        return Count;
    }

    int32_t RenderComponent::GetMaterialParameterSpan() const
    {
        int32_t Span = 0;
        for(size_t Index = 0; Index < RENDER_MATERIAL_PARAMETERS_MAX; ++Index)
        {
            if(Parameters[Index].Name[0] != '\0') { Span = int32_t(Index + 1); }
        }
        return Span;
    }

    /*
     * The owning module owns the serializer, so a build without RenderCore
     * skips these chunks by size and warns.
     *
     * C.Proxy is not saved: it names a live render-thread object that
     * WorldBridge recreates on the first frame after a load.
     */
    /* Each slot's material is a graph path. 2: the parameter overrides, a
     * count then that many (up to the last in use, so an empty one between
     * keeps its place), so a component with none writes one field. 6:
     * CustomPassGroups (an older save keeps the default, none). 5: Occluder
     * (an older save keeps the default, off). 4: CastInsetShadow
     * (an older save keeps the default, off). 3: CastShadows (an
     * older save keeps the default, on). */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(RenderComponent, 6)
    {
        Ar.Flag("Visible",   C.bVisible);
        for(size_t Slot = 0; Slot < RENDER_MATERIAL_SLOTS; ++Slot)
        {
            Ar.Field(MATERIAL_SLOT_FIELDS[Slot], C.Materials[Slot]);
        }
        Ar.Field("Mesh",     C.Mesh);

        if(Ar.GetVersion() >= 2)
        {
            int32_t Count = C.GetMaterialParameterSpan();
            Ar.Field("ParameterCount", Count);
            if(Count < 0) { Count = 0; }
            if(size_t(Count) > RENDER_MATERIAL_PARAMETERS_MAX) { Count = int32_t(RENDER_MATERIAL_PARAMETERS_MAX); }
            for(size_t Index = 0; Index < size_t(Count); ++Index)
            {
                const ParameterFieldNames& Names = ParameterFields(Index);
                RenderMaterialParameter& Param = C.Parameters[Index];
                Ar.Field(Names.Name, Param.Name);
                Ar.Field(Names.Slot, Param.Slot);
                Ar.Field(Names.X,    Param.Value[0]);
                Ar.Field(Names.Y,    Param.Value[1]);
                Ar.Field(Names.Z,    Param.Value[2]);
                Ar.Field(Names.W,    Param.Value[3]);
            }
            /* Loading fewer than were there: the rest are gone, not stale. */
            if(Ar.IsLoading())
            {
                for(size_t Index = size_t(Count); Index < RENDER_MATERIAL_PARAMETERS_MAX; ++Index)
                {
                    C.Parameters[Index] = RenderMaterialParameter();
                }
            }
        }
        if(Ar.GetVersion() >= 3)
        {
            Ar.Flag("CastShadows", C.bCastShadows);
        }
        if(Ar.GetVersion() >= 4)
        {
            Ar.Flag("CastInsetShadow", C.bCastInsetShadow);
        }
        if(Ar.GetVersion() >= 5)
        {
            Ar.Flag("Occluder", C.bOccluder);
        }
        if(Ar.GetVersion() >= 6)
        {
            Ar.Field("CustomPassGroups", C.CustomPassGroups);
        }
    }

    /*
     * WHAT LUA MAY TOUCH. Written through GameCore's typed binding API -- this
     * module never includes lua.h (ComponentScript.h says why). A change made
     * from Lua reaches the screen the same way a C++ one does: WorldBridge
     * re-extracts the component into its proxy every frame. C.Proxy is not
     * listed, so scripts cannot see the render handle.
     */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(RenderComponent)
    {
        Bind.Flag("Visible", &RenderComponent::bVisible);
        Bind.Field("Mesh",     &RenderComponent::Mesh);

        /* Slots by index: render:GetMaterial(0), render:SetMaterial(1, path),
         * render:GetNumRegisteredMaterials(). */
        Bind.NativePure<&RenderComponent::GetMaterial>("GetMaterial", "Index -> Material");
        Bind.Native<&RenderComponent::SetMaterial>("SetMaterial", "Index, Material -> Set");
        Bind.NativePure<&RenderComponent::GetNumRegisteredMaterials>("GetNumRegisteredMaterials", "-> Count");

        /* render:SetMaterialParameter(slot, "Color", r, g, b, a): this
         * entity's draws only. Missing components are 0; a float parameter
         * reads X. */
        Bind.Method("SetMaterialParameter", [](RenderComponent& C, GameCore::ScriptCall& Call)
        {
            const int32_t Slot = Call.GetInt(1);
            const char*   Name = Call.GetString(2);
            float Value[4] = {};
            for(int Each = 0; Each < 4; ++Each)
            {
                if(Call.HasArg(3 + Each)) { Value[Each] = Call.GetFloat(3 + Each); }
            }
            if(Call.HasError()) { return; }
            Call.PushBool(C.SetMaterialParameter(Slot, Name, Value[0], Value[1], Value[2], Value[3]));
        }, "Slot:integer, Name:string, X:number, Y:number, Z:number, W:number -> Set:bool");

        /* A Color pin (0..255) as a vec4 parameter (0..1), for script graphs. */
        Bind.Method("SetMaterialColor", [](RenderComponent& C, GameCore::ScriptCall& Call)
        {
            const int32_t           Slot  = Call.GetInt(1);
            const char*             Name  = Call.GetString(2);
            const EngineCore::Color Color = Call.GetColor(3);
            if(Call.HasError()) { return; }
            Call.PushBool(C.SetMaterialParameter(Slot, Name, Color.Rf(), Color.Gf(), Color.Bf(), Color.Af()));
        }, "Slot:integer, Name:string, Color:Color -> Set:bool");

        /* The override if set, else Set false and zeros: the material's own
         * default lives on the render thread. */
        Bind.Pure("GetMaterialParameter", [](RenderComponent& C, GameCore::ScriptCall& Call)
        {
            const int32_t Slot = Call.GetInt(1);
            const char*   Name = Call.GetString(2);
            if(Call.HasError()) { return; }
            float Value[4] = {};
            const bool bSet = C.GetMaterialParameter(Slot, Name, Value);
            for(const float Each : Value) { Call.PushNumber(Each); }
            Call.PushBool(bSet);
        }, "Slot:integer, Name:string -> X:number, Y:number, Z:number, W:number, Set:bool");

        Bind.Native<&RenderComponent::ClearMaterialParameter>("ClearMaterialParameter", "Slot, Name -> Cleared");

        Bind.Flag("CastShadows", &RenderComponent::bCastShadows);
        Bind.Flag("CastInsetShadow", &RenderComponent::bCastInsetShadow);
        Bind.Flag("Occluder", &RenderComponent::bOccluder);
        Bind.Field("CustomPassGroups", &RenderComponent::CustomPassGroups);
    }

#if WITH_EDITOR
    /* Editor-visible fields; deliberately separate from the Lua list
     * (ComponentProperties.h). */
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(RenderComponent)
    {
        Props.Flag("Visible", &RenderComponent::bVisible);
        Props.Category("Appearance");
        Props.Field("Mesh",     &RenderComponent::Mesh, GameCore::PF_AssetMesh);
        /* A list: entry i is section i's material; empty ones use the mesh's own. */
        Props.Array("Materials", &RenderComponent::Materials, GameCore::PF_AssetMaterial);
        Props.Category("Shadows");
        Props.Flag("Cast Shadows", &RenderComponent::bCastShadows);
        Props.Flag("Cast Inset Shadow", &RenderComponent::bCastInsetShadow);
        Props.Category("Culling");
        Props.Flag("Occluder", &RenderComponent::bOccluder);
        Props.Category("Custom Render Passes");
        Props.Field("Custom Pass Groups", &RenderComponent::CustomPassGroups);

        /* This entity's overrides: a material slot, a parameter of its
         * material (the Parameter node's Name), and up to four values. */
        Props.Category("Material Parameters");
        Props.StructArray("Parameters", &RenderComponent::Parameters, [](auto& Member)
        {
            Member.Field("Slot", &RenderMaterialParameter::Slot);
            Member.Field("Name", &RenderMaterialParameter::Name);
            Member.Element("X", &RenderMaterialParameter::Value, 0);
            Member.Element("Y", &RenderMaterialParameter::Value, 1);
            Member.Element("Z", &RenderMaterialParameter::Value, 2);
            Member.Element("W", &RenderMaterialParameter::Value, 3);
        });
    }
#endif // WITH_EDITOR
}
