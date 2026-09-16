-- Sandbox's startup script: run once by NimbleEngine.cpp after IGame::OnStartup
-- succeeds, per [Scripting] StartupScript in Config/DefaultEngine.ini.
--
-- Demonstrates the pieces GameCore's Lua surface offers a game that never
-- touches C++ for its own gameplay: a registered class with a chosen tick
-- phase, a Transform and a Bounds component, and the Events bus reaching a
-- C++ subscriber (SandboxMain.cpp binds "EntityCreated" the same way this
-- script does, from the other side of the language boundary).

Events.On("EntityCreated", function(entity)
    print("EntityCreated: " .. entity:GetName())
end)

Script.Register("Mover", {
    -- PostPhysics, not the PrePhysics default: this class's own movement
    -- should be reflected before anything downstream (a camera, a trigger)
    -- reads this frame's resolved position.
    TickPhase = TickPhase.PostPhysics,

    OnStart = function(self)
        print("Mover started on " .. self.Entity:GetName())
    end,

    OnTick = function(self, dt)
        local t = self.Entity:GetComponent("Transform")
        t:Translate(60 * dt, 0, 0)
    end,
})

local scripted = World.CreateEntity("Scripted")
local transform = scripted:AddComponent("Transform")
transform:SetPosition(0, 0, 0)

local bounds = scripted:AddComponent("Bounds")
bounds:SetLocal(-0.5, -0.5, -0.5, 0.5, 0.5, 0.5)

scripted:AddScript("Mover")

Events.Emit("SandboxReady")
