-- NetArena's startup script ([World_NetArena] StartupScript in
-- Config/DefaultEngine.ini), run on every load of the world after its mode has
-- loaded -- on every process: the server and each client alike. The Net
-- library says which one this is (networking starts before the level loads).
--
-- The capsules are avatars (Engine/Source/AVATARS.md): each one's Avatar
-- Harness reads its player's keys and the engine sends them to the server. From
-- Lua, a capsule's input reads the same way on every process:
--
--   local harness = Game.GetLocalAvatar(0):GetComponent("AvatarHarness")
--   print(harness:GetAxis("MoveRight"), harness:IsLocallyControlled())

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

-- Every process hears who takes which capsule (a client, as it replicates).
Events.On(Game.Possessed, function(Player, Avatar)
    print("NetArena (Lua): " .. Player:GetName() .. " drives " .. Avatar:GetName())
end)
