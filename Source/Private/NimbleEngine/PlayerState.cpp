#include "Public/NimbleEngine/PlayerState.h"

#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"

/*
 * PlayerState.h's component: registration, save, script and editor views.
 * The rules that change it are GameSession's (GameSession.cpp).
 */

using NimbleEngine::PlayerStateComponent;

GAMECORE_DEFINE_COMPONENT(PlayerStateComponent, "PlayerState")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(PlayerStateComponent, 1)
{
    Ar.Field("PlayerName",  C.PlayerName);
    Ar.Field("PlayerIndex", C.PlayerIndex);
    Ar.Field("Score",       C.Score);
    Ar.Field("Lives",       C.Lives);
    Ar.Flag("Active",       C.bActive);
}

/* player:GetScore(), player:GetLives(): read-only, so a change goes through
 * Game.AddScore / Game.AddLives and the mode hears of it. */
GAMECORE_DEFINE_COMPONENT_SCRIPT(PlayerStateComponent)
{
    Bind.Field("PlayerName",     &PlayerStateComponent::PlayerName);
    Bind.ReadOnly("PlayerIndex", &PlayerStateComponent::PlayerIndex);
    Bind.ReadOnly("Score",       &PlayerStateComponent::Score);
    Bind.ReadOnly("Lives",       &PlayerStateComponent::Lives);
    Bind.Flag("Active",          &PlayerStateComponent::bActive);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(PlayerStateComponent)
{
    Props.Field("PlayerName",  &PlayerStateComponent::PlayerName);
    Props.Field("PlayerIndex", &PlayerStateComponent::PlayerIndex);
    Props.Field("Score",       &PlayerStateComponent::Score);
    Props.Field("Lives",       &PlayerStateComponent::Lives);
    Props.Flag("Active",       &PlayerStateComponent::bActive);
}
#endif
