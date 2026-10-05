-- NetArena's startup script ([Scripting] StartupScript in
-- Config/DefaultEngine.ini), run on every level load after the level has built
-- its scene -- on every process: the server and each client alike. The Net
-- library says which one this is (networking starts before the level loads).
--
-- The capsules and their RPC are C++ (NetArenaGame.cpp); from Lua the same
-- RPC is a method of the component, routed by the network:
--
--   local pawn = World.FindEntity("Host"):GetComponent("NetArenaPawn")
--   pawn:ServerMove(1, 1, 0, 0, 0.016)                          -- move 1: right, a frame
--   Net.Call(pawn:GetEntity(), "NetArenaPawn", "ServerMove", 2, 1, 0, 0, 0.016)

local Role = Net.IsDedicatedServer() and "a dedicated server"
          or Net.IsServer() and "a listen server"
          or Net.IsClient() and "a client"
          or "standalone"
print("NetArena (Lua): Main.lua on " .. Role)

-- The server hears clients come and go; Player is the entity made for each.
-- (Event numbers arrive as Lua floats, as every EventBus number does: %d
-- prints one as an integer.)
if Net.IsServer() then
    Events.On("ClientConnected", function(Client, Player)
        print(string.format("NetArena (Lua): client %d joined as %s", Client, Player:GetName()))
    end)
    Events.On("ClientDisconnected", function(Client, Player)
        print(string.format("NetArena (Lua): client %d left", Client))
    end)
end
