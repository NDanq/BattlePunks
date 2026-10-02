#include "Public/RenderCore/RenderComponents.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

#include <cstring>
#include <string>

#include "Public/RenderCore/MaterialDescription.h"
#include "Private/RenderCore/RenderComponentProxy.h"

/* Registration for the two built-in components. */

namespace RenderCore
{
    RENDERCORE_DEFINE_PROXY_COMPONENT(RenderComponent, "Render", RenderComponentProxy)

    /* No RENDERCORE_PROXY_BODY() -- a CameraComponent drives WorldBridge's
     * view build directly (a later build-order step reads it, it does not
     * mirror it onto a proxy), so GAMECORE_DEFINE_COMPONENT alone is all
     * this one needs. */
    GAMECORE_DEFINE_COMPONENT(CameraComponent, "Camera")

    /* Read by WorldBridge's per-frame lighting build, like the camera. */
    GAMECORE_DEFINE_COMPONENT(DirectionalLightComponent, "Directional Light")
    GAMECORE_DEFINE_COMPONENT(SkyLightComponent, "Sky Light")
    GAMECORE_DEFINE_COMPONENT(PostProcessComponent, "Post Process")

    CreateProxyFn GetRenderComponentFactory()
    {
        return &RenderComponent_CreateProxy;
    }

    namespace
    {
        /* Saved before materials were paths: a bare name ("Glow") is the
         * graph Materials/Glow.material.nmgraph, which is what it meant. */
        void UpgradeMaterialName(char* InOutMaterial, const size_t InCapacity)
        {
            const std::string Name(InOutMaterial, strnlen(InOutMaterial, InCapacity));
            std::string Stem;
            if(Name.empty() || Name.find('/') != std::string::npos || !GetMaterialStem(Name, Stem))
            {
                return;
            }
            const std::string Path = Stem + MATERIAL_GRAPH_EXTENSION;
            if(Path.size() < InCapacity)
            {
                std::memcpy(InOutMaterial, Path.c_str(), Path.size() + 1);
            }
        }

        /* RenderComponent's slots: in saved worlds and Lua, and in the editor. */
        const char* const MATERIAL_SLOT_FIELDS[RENDER_MATERIAL_SLOTS] = {
            "Material0", "Material1", "Material2", "Material3", "Material4", "Material5", "Material6", "Material7" };
    }

    /*
     * The owning module owns the serializer, so a build without RenderCore
     * skips these chunks by size and warns.
     *
     * C.Proxy is not saved: it names a live render-thread object that
     * WorldBridge recreates on the first frame after a load.
     */
    /* Version 2: a material per slot. Version 1's one Material is slot 0.
     * Version 3: no Width, Height or Tint -- read and dropped (the Transform's
     * Scale sizes the quad).
     * Version 4: materials are graph paths; an older bare name is upgraded. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(RenderComponent, 4)
    {
        if(Ar.GetVersion() < 3)
        {
            float             Width  = 1.0f;
            float             Height = 1.0f;
            EngineCore::Color Tint{255, 255, 255};
            Ar.Field("Width",  Width);
            Ar.Field("Height", Height);
            Ar.Field("Tint",   Tint);
        }
        Ar.Flag("Visible",   C.bVisible);
        if(Ar.GetVersion() < 2)
        {
            Ar.Field("Material", C.Materials[0]);
        }
        else
        {
            for(size_t Slot = 0; Slot < RENDER_MATERIAL_SLOTS; ++Slot)
            {
                Ar.Field(MATERIAL_SLOT_FIELDS[Slot], C.Materials[Slot]);
            }
        }
        Ar.Field("Mesh",     C.Mesh);
        if(Ar.IsLoading() && Ar.GetVersion() < 4)
        {
            for(char (&Slot)[RENDER_MATERIAL_PATH_MAX] : C.Materials) { UpgradeMaterialName(Slot, sizeof(Slot)); }
        }
    }

    /* Every field is real data -- a CameraComponent holds no per-run state at
     * all (its position comes from the sibling Transform, which has its own
     * serializer). */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(CameraComponent, 1)
    {
        Ar.Field("ZNear", C.ZNear);
        Ar.Field("ZFar",  C.ZFar);
        Ar.Flag("Active", C.bActive);
    }

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(DirectionalLightComponent, 1)
    {
        Ar.Field("Direction", C.Direction);
        Ar.Field("Color",     C.Color);
        Ar.Field("Intensity", C.Intensity);
        Ar.Flag("Active",     C.bActive);
    }

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(SkyLightComponent, 1)
    {
        Ar.Field("SkyColor",    C.SkyColor);
        Ar.Field("GroundColor", C.GroundColor);
        Ar.Field("Intensity",   C.Intensity);
        Ar.Flag("Active",       C.bActive);
    }

    /* Version 2: the material is a graph path; an older bare name is upgraded. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(PostProcessComponent, 2)
    {
        Ar.Field("Material", C.Material);
        if(Ar.IsLoading() && Ar.GetVersion() < 2) { UpgradeMaterialName(C.Material, sizeof(C.Material)); }
        Ar.Field("Priority", C.Priority);
        Ar.Flag("Enabled",   C.bEnabled);
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
        Bind.Element("Material", &RenderComponent::Materials, 0);   // slot 0, as before slots
        for(size_t Slot = 0; Slot < RENDER_MATERIAL_SLOTS; ++Slot)
        {
            Bind.Element(MATERIAL_SLOT_FIELDS[Slot], &RenderComponent::Materials, Slot);
        }
        Bind.Field("Mesh",     &RenderComponent::Mesh);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(CameraComponent)
    {
        Bind.Field("ZNear", &CameraComponent::ZNear);
        Bind.Field("ZFar",  &CameraComponent::ZFar);
        Bind.Flag("Active", &CameraComponent::bActive);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(DirectionalLightComponent)
    {
        Bind.Field("Direction", &DirectionalLightComponent::Direction);
        Bind.Field("Color",     &DirectionalLightComponent::Color);
        Bind.Field("Intensity", &DirectionalLightComponent::Intensity);
        Bind.Flag("Active",     &DirectionalLightComponent::bActive);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(SkyLightComponent)
    {
        Bind.Field("SkyColor",    &SkyLightComponent::SkyColor);
        Bind.Field("GroundColor", &SkyLightComponent::GroundColor);
        Bind.Field("Intensity",   &SkyLightComponent::Intensity);
        Bind.Flag("Active",       &SkyLightComponent::bActive);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(PostProcessComponent)
    {
        Bind.Field("Material", &PostProcessComponent::Material);
        Bind.Flag("Enabled",   &PostProcessComponent::bEnabled);
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
    }

    GAMECORE_DEFINE_COMPONENT_PROPERTIES(CameraComponent)
    {
        Props.Field("ZNear", &CameraComponent::ZNear);
        Props.Field("ZFar",  &CameraComponent::ZFar);
        Props.Flag("Active", &CameraComponent::bActive);
    }

    GAMECORE_DEFINE_COMPONENT_PROPERTIES(DirectionalLightComponent)
    {
        Props.Field("Direction", &DirectionalLightComponent::Direction);
        Props.Field("Color",     &DirectionalLightComponent::Color);
        Props.Field("Intensity", &DirectionalLightComponent::Intensity);
        Props.Flag("Active",     &DirectionalLightComponent::bActive);
    }

    GAMECORE_DEFINE_COMPONENT_PROPERTIES(SkyLightComponent)
    {
        Props.Field("SkyColor",    &SkyLightComponent::SkyColor);
        Props.Field("GroundColor", &SkyLightComponent::GroundColor);
        Props.Field("Intensity",   &SkyLightComponent::Intensity);
        Props.Flag("Active",       &SkyLightComponent::bActive);
    }

    GAMECORE_DEFINE_COMPONENT_PROPERTIES(PostProcessComponent)
    {
        Props.Field("Material", &PostProcessComponent::Material, GameCore::PF_AssetMaterial);
        Props.Field("Priority", &PostProcessComponent::Priority);
        Props.Flag("Enabled",   &PostProcessComponent::bEnabled);
    }
#endif // WITH_EDITOR
}
