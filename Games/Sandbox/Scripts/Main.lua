-- Sandbox's startup script ([Scripting] StartupScript), run on every level
-- load. Places prefab instances -- the level is made of them (Prefabs/, and the
-- native prefabs SandboxGame.cpp declares) -- and demonstrates the Events bus
-- reaching a C++ subscriber, and the game's own script library and component
-- (SandboxScripting.cpp).

Events.On("EntityCreated", function(entity)
    print("EntityCreated: " .. entity:GetName())
end)

-- "Scripted": Transform and Bounds, driven by Scripts/Mover.lua -- the
-- prefab's script, started on the World's next tick ahead of every C++ system.
World.SpawnPrefab("Prefabs/Mover.nmprefab", 0, 0, 0)

-- The game's own script library (SandboxScripting.cpp): pickups along
-- Marker's path, each an instance of the native prefab Sandbox.Pickup -- a
-- game-defined "Pickup" component, driven by Scripts/Pickup.script.nmgraph.
for i = 1, 5 do
    local pickup = Sandbox.SpawnPickup(i * 220, 360, 0, i)
    pickup:GetComponent("Pickup"):SetBobHeight(4 + 2 * i)
end

-- Prints the typed events the library declares (ScoreBoard.script.nmgraph).
World.SpawnPrefab("Prefabs/ScoreBoard.nmprefab")

-- A trigger zone in Marker's path: a Collision on its Bounds, and
-- OnOverlapBegin / OnOverlapEnd in Scripts/Zone.lua.
World.SpawnPrefab("Prefabs/Zone.nmprefab", 600, 360, 0)

-- Input: an "Input" component reading SandboxInput.cpp's "Default" map
-- (Scripts/InputDemo.lua).
World.SpawnPrefab("Prefabs/InputDemo.nmprefab")

-- The same events, from hand-written Lua: the library publishes their names.
-- PickupStats, a script struct, is its fields' values in order: Value, ID.
Events.On(Sandbox.PickupCollected, function(pickup, value)
    local worth, id = pickup:GetComponent("Pickup"):GetStats()
    if id == 5 then print("Sandbox: the last pickup collected, score " .. Sandbox.GetScore()) end
end)

Events.Emit("SandboxReady")
