-- The Mover class: slides its entity along +x at 60 units/s. Attached by path
-- -- scripted:SetScript("Scripts/Mover.lua") in Main.lua -- so this file
-- returns its class table rather than registering it anywhere. Each entity
-- that uses this path gets its own instance, with self.Entity set.
return {
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
}
