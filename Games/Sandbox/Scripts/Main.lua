-- Sandbox's startup script ([Scripting] StartupScript), run on every level
-- load. Demonstrates a script-driven entity with its own tick phase, Transform
-- and Bounds components, and the Events bus reaching a C++ subscriber.

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

Events.Emit("SandboxReady")
