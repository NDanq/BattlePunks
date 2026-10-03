#include "Public/RenderCore/Components/RenderComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

#include "Public/Core/CoreTypes.h"
#include "Private/RenderCore/RenderComponentProxy.h"

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
    }

    /*
     * The owning module owns the serializer, so a build without RenderCore
     * skips these chunks by size and warns.
     *
     * C.Proxy is not saved: it names a live render-thread object that
     * WorldBridge recreates on the first frame after a load.
     */
    /* Each slot's material is a graph path. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(RenderComponent, 1)
    {
        Ar.Flag("Visible",   C.bVisible);
        for(size_t Slot = 0; Slot < RENDER_MATERIAL_SLOTS; ++Slot)
        {
            Ar.Field(MATERIAL_SLOT_FIELDS[Slot], C.Materials[Slot]);
        }
        Ar.Field("Mesh",     C.Mesh);
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
#endif // WITH_EDITOR
}
