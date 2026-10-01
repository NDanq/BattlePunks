-- Slides its entity along +x at 60 units/s. Attached by path (Main.lua), so
-- it returns its class table; each entity gets an instance with self.Entity.
return {
    -- Not the PrePhysics default: settle before downstream readers this frame.
    TickPhase = TickPhase.PostPhysics,

    OnStart = function(self)
        print("Mover started on " .. self.Entity:GetName())
    end,

    OnTick = function(self, dt)
        local t = self.Entity:GetComponent("Transform")
        t:Translate(60 * dt, 0, 0)
    end,
}
