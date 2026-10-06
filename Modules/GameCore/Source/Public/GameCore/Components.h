#ifndef GAMECORE_COMPONENTS_H
#define GAMECORE_COMPONENTS_H

/*
 * The engine's own built-in components, one header and one .cpp each under
 * Components/. They are defined the same way a game defines its own:
 * GAMECORE_COMPONENT_BODY() first, then GAMECORE_DEFINE_COMPONENT(...) once in
 * the component's .cpp, next to its serializer, its Lua binding and its editor
 * properties. World and ComponentPool special-case none of them. Every one is
 * GAMECORE_API despite looking like plain POD -- see DESIGN.md for why
 * StaticType() makes that exception never apply.
 *
 * This header includes them all. Include a single component's header when
 * that is all a file needs.
 */
#include "Public/GameCore/Components/BoundsComponent.h"
#include "Public/GameCore/Components/CollisionComponent.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Components/PhysicsConstraintComponent.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"

#endif //GAMECORE_COMPONENTS_H
