#ifndef RENDERCORE_RENDERCOMPONENTS_H
#define RENDERCORE_RENDERCOMPONENTS_H

/*
 * RenderCore's own GameCore components, one header and one .cpp each under
 * Components/. They are declared the same way a game declares its own:
 * GAMECORE_COMPONENT_BODY() first, RENDERCORE_PROXY_BODY() second if it has
 * a proxy, then GAMECORE_DEFINE_COMPONENT()/RENDERCORE_DEFINE_PROXY_COMPONENT()
 * once in the component's .cpp. World and ComponentPool special-case none of
 * them; a game's own render component follows the same two-macro shape.
 *
 * This header includes them all. Include a single component's header when
 * that is all a file needs.
 */
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/Components/DirectionalLightComponent.h"
#include "Public/RenderCore/Components/LightShadow.h"
#include "Public/RenderCore/Components/PointLightComponent.h"
#include "Public/RenderCore/Components/PostProcessComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/Components/SkyLightComponent.h"
#include "Public/RenderCore/Components/SpotLightComponent.h"

#endif //RENDERCORE_RENDERCOMPONENTS_H
