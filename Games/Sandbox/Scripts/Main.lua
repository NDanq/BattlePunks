-- Sandbox's startup script ([Scripting] StartupScript), run on every level
-- load. Demonstrates a script-driven entity with its own tick phase, Transform
-- and Bounds components, the Events bus reaching a C++ subscriber, and the
-- game's own script library and component (SandboxScripting.cpp).

Events.On("EntityCreated", function(entity)
    print("EntityCreated: " .. entity:GetName())
end)

local scripted = World.CreateEntity("Scripted")
local transform = scripted:AddComponent("Transform")
transform:SetPosition(0, 0, 0)

local bounds = scripted:AddComponent("Bounds")
bounds:SetLocal(-0.5, -0.5, -0.5, 0.5, 0.5, 0.5)

-- Loaded and started on the World's next tick, ahead of every C++ system.
scripted:SetScript("Scripts/Mover.lua")

-- The game's own script library (SandboxScripting.cpp): pickups along
-- Marker's path, each a game-defined "Pickup" component driven by a script
-- stored in Scripts/Pickup.script.nmgraph.
Sandbox.SetPickupRadius(24)
for i = 1, 5 do
    local pickup = Sandbox.SpawnPickup(i * 220, 360, 0, i)
    pickup:GetComponent("Pickup"):SetBobHeight(4 + 2 * i)
    pickup:SetScript("Scripts/Pickup.script.nmgraph")
end

-- Prints the typed events the library declares (ScoreBoard.script.nmgraph).
World.CreateEntity("ScoreBoard"):SetScript("Scripts/ScoreBoard.script.nmgraph")

-- The same events, from hand-written Lua: the library publishes their names.
-- PickupStats, a script struct, is its fields' values in order: Value, ID.
Events.On(Sandbox.PickupCollected, function(pickup, value)
    local worth, id = pickup:GetComponent("Pickup"):GetStats()
    if id == 5 then print("Sandbox: the last pickup collected, score " .. Sandbox.GetScore()) end
end)

Events.Emit("SandboxReady")
