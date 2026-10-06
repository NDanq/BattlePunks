#include "Public/NimbleEngine/NetFacade.h"

#include <string>

#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"

/*
 * The "Net" script library: NetFacade.h for Lua and script graphs, and RPCs by
 * name. Always registered -- with NetCore compiled out, or no session, every
 * answer is Standalone's and every call runs here, as from C++.
 *
 *     if Net.HasAuthority() then SpawnWave() end
 *     if Net.IsLocallyOwned(pawn) then
 *         pawn:GetComponent("Ship"):ServerFire(x, y, z)       -- a component's RPC, routed
 *         Net.Call(pawn, "Ship", "ServerFire", x, y, z)        -- the same, by name (graphs)
 *     end
 *     Events.On("ClientConnected", function(client, player) ... end)   -- server only
 */

namespace
{
    const GameCore::NetComponent* NetOf(GameCore::ScriptCall& InCall, const int InArg)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(InArg);
        return Entity.IsSet() ? GameCore::Entity(&InCall.GetWorld(), Entity).GetComponent<GameCore::NetComponent>() : nullptr;
    }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Net)
{
    // === Role ===================================================================

    Lib.Pure("HasAuthority", [](GameCore::ScriptCall& Call) { Call.PushBool(NimbleEngine::HasAuthority()); },
             "-> Authority:bool")
       .Tooltip("A server, or not networked: this process decides shared state. Guard spawning, scoring and simulation with it");

    Lib.Pure("IsServer", [](GameCore::ScriptCall& Call) { Call.PushBool(NimbleEngine::IsServer()); }, "-> Server:bool")
       .Tooltip("A dedicated or listen server");

    Lib.Pure("IsClient", [](GameCore::ScriptCall& Call) { Call.PushBool(NimbleEngine::IsClient()); }, "-> Client:bool")
       .Tooltip("Connected to a server, which owns the shared state");

    Lib.Pure("IsDedicatedServer", [](GameCore::ScriptCall& Call) { Call.PushBool(NimbleEngine::IsDedicatedServer()); },
             "-> Dedicated:bool")
       .Tooltip("A headless server: no local player");

    Lib.Pure("IsStandalone", [](GameCore::ScriptCall& Call)
    {
        Call.PushBool(NimbleEngine::GetNetMode() == NimbleEngine::ENetMode::Standalone);
    }, "-> Standalone:bool").Tooltip("Not networked (or NetCore is compiled out)");

    Lib.Pure("IsConnected", [](GameCore::ScriptCall& Call) { Call.PushBool(NimbleEngine::IsNetConnected()); },
             "-> Connected:bool")
       .Tooltip("A client welcomed by its server, or a server listening");

    Lib.Pure("GetLocalClientId", [](GameCore::ScriptCall& Call) { Call.PushInteger(NimbleEngine::GetLocalClientId()); },
             "-> Client:integer")
       .Tooltip("This client's id on its server; -1 on a server or when not networked");

    // === Entities ===============================================================

    Lib.Pure("IsNetworked", [](GameCore::ScriptCall& Call)
    {
        const GameCore::NetComponent* Net = NetOf(Call, 1);
        Call.PushBool(Net && Net->NetId != 0);
    }, "Entity:Entity -> Networked:bool").Tooltip("The server has given it a network id: it exists on every process");

    Lib.Pure("GetOwner", [](GameCore::ScriptCall& Call)
    {
        const GameCore::NetComponent* Net = NetOf(Call, 1);
        Call.PushInteger(Net ? Net->OwnerClient : GameCore::NET_CLIENT_NONE);
    }, "Entity:Entity -> Client:integer").Tooltip("The client that controls it; -1 for the server's own (or a local entity)");

    Lib.Pure("IsLocallyOwned", [](GameCore::ScriptCall& Call)
    {
        /* An entity without a NetComponent is this process's alone. */
        const GameCore::NetComponent* Net = NetOf(Call, 1);
        Call.PushBool(!Net || NimbleEngine::IsLocallyOwned(*Net));
    }, "Entity:Entity -> Owned:bool").Tooltip("This process controls it: its input should steer it");

    // === RPCs ===================================================================

    /* By name, for script graphs and for a type known only as a string. Lua
     * can also call component:RpcName(...) on a component proxy. */
    Lib.Function("Call", [](GameCore::ScriptCall& Call)
    {
        const GameCore::EntityID Entity    = Call.GetEntity(1);
        const char*              Component = Call.GetString(2);
        const char*              Name      = Call.GetString(3);
        if(Call.HasError()) { return; }

        const GameCore::ComponentTypeInfo* Type = GameCore::FindComponentType(Component);
        const GameCore::RpcEntry*          Rpc  = Type ? GameCore::FindComponentRpc(Type->ID, Name) : nullptr;
        if(!Rpc)
        {
            Call.Error((std::string("no RPC ") + Component + "." + Name).c_str());
            return;
        }
        Call.PushBool(GameCore::CallRpcFromScript(Call, 4, Call.GetWorld(), Entity, Type->ID, *Rpc));
    }, "Entity:Entity, Component:string, Rpc:string -> Sent:bool")
       .Tooltip("Calls the component's RPC on Entity, routed by its target (Server, Client, Multicast); "
                "the RPC's own arguments follow");

    // === Events =================================================================
    // Emitted by the server's NetHost (NetHost.cpp) on the level's World.

    Lib.Event("ClientConnected", "Client:integer, Player:Entity")
       .Tooltip("Server: a client joined; Player is the entity made for it (unset with [Net] SpawnPlayerPerClient off)");
    Lib.Event("ClientDisconnected", "Client:integer, Player:Entity")
       .Tooltip("Server: a client left; its entities are destroyed right after");
}
