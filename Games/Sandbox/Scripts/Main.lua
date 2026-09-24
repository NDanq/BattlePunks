-- Sandbox's startup script: run once by NimbleEngine.cpp after IGame::OnStartup
-- succeeds, per [Scripting] StartupScript in Config/DefaultEngine.ini.
--
-- Demonstrates the pieces GameCore's Lua surface offers a game that never
-- touches C++ for its own gameplay: an entity driven by a script file (with
-- its own tick phase), a Transform and a Bounds component, and the Events bus
-- reaching a C++ subscriber (SandboxGame.cpp binds "EntityCreated" the same
-- way this script does, from the other side of the language boundary).

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
