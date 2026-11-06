#include "Public/RenderCore/RenderProxy.h"

#include <cstring>
#include <string>

#include "Public/RenderCore/RenderCoreLog.h"

/*
 * A fixed COMPONENT_TYPE_MAX-entry table mapping a GameCore component type to
 * the factory that turns its bytes into a proxy -- the same shape and the
 * same reasoning as GameCore::ComponentType.cpp's own registry: constant-
 * initialised into .bss so the first RegisterProxyType() call anywhere in
 * the process finds a valid table with no dynamic-initialisation-order
 * question, indexed directly by ComponentTypeID rather than scanned by name
 * (a ComponentTypeID is already the small dense integer GameCore's own
 * registry hands out; there is no name to intern here).
 */

namespace RenderCore
{
    namespace
    {
        struct ProxyTypeEntry
        {
            CreateProxyFn       Fn          = nullptr;
            EngineCore::ImageID OwningImage = EngineCore::IMAGE_Host;
        };

        ProxyTypeEntry GProxyTypes[GameCore::COMPONENT_TYPE_MAX];
    }

    bool RegisterProxyType(const GameCore::ComponentTypeID InType, const CreateProxyFn InFn, const EngineCore::ImageID InOwningImage)
    {
        if(InType == GameCore::COMPONENT_TYPE_INVALID || size_t(InType) >= GameCore::COMPONENT_TYPE_MAX || !InFn)
        {
            RENDERCORE_LOG_ERROR("RegisterProxyType given an invalid type or a null factory; ignoring")
            return false;
        }

        if(GProxyTypes[InType].Fn != nullptr)
        {
            RENDERCORE_LOG_ERROR(std::string("RegisterProxyType: component type ")
                                  + std::to_string(InType) + " is already registered; ignoring")
            return false;
        }

        GProxyTypes[InType].Fn          = InFn;
        GProxyTypes[InType].OwningImage = InOwningImage;
        return true;
    }

    void UnregisterProxyType(const GameCore::ComponentTypeID InType)
    {
        if(InType == GameCore::COMPONENT_TYPE_INVALID || size_t(InType) >= GameCore::COMPONENT_TYPE_MAX)
        {
            return;
        }

        /*
         * SWEEPING EVERY LIVE PROXY OF InType is WorldBridge's job, not this
         * function's: this table only maps a type to its factory, and has no
         * reach into whatever RenderScene a Renderer happens to own -- there
         * may be more than one bound World, or none. A caller that has
         * already removed every component of InType from its World (which
         * drives RemoveProxy through WorldBridge's own OnComponentRemoved
         * handler) reaches here with nothing left to sweep; this only clears
         * the slot so a later RegisterProxyType for the same type does not
         * see a stale factory from an unloaded image.
         */
        GProxyTypes[InType].Fn          = nullptr;
        GProxyTypes[InType].OwningImage = EngineCore::IMAGE_Host;
    }

    CreateProxyFn FindProxyFactory(const GameCore::ComponentTypeID InType)
    {
        if(InType == GameCore::COMPONENT_TYPE_INVALID || size_t(InType) >= GameCore::COMPONENT_TYPE_MAX)
        {
            return nullptr;
        }
        return GProxyTypes[InType].Fn;
    }
}
