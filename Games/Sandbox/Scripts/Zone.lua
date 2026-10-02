-- A trigger zone in hand-written Lua (Main.lua makes it): the engine calls
-- OnOverlapBegin / OnOverlapEnd at the start of TP_PostPhysics when another
-- collider starts or stops overlapping this entity's Collision box. O asks
-- the Collision library what is in the zone right now.
return {
    TickPhase = TickPhase.PostPhysics,

    OnOverlapBegin = function(self, other)
        print("Zone: " .. other:GetName() .. " entered")
    end,

    OnOverlapEnd = function(self, other)
        -- An overlap also ends when the other entity is destroyed.
        print("Zone: " .. (other:IsValid() and other:GetName() or "something") .. " left")
    end,

    OnTick = function(self, dt)
        if Input.WasKeyPressed("O") then
            local collision = self.Entity:GetComponent("Collision")
            local inside = Collision.CountInBox(540, 320, -1, 660, 400, 1, -1)
            print("Zone: overlapping " .. collision:GetOverlapCount() .. ", colliders in the zone's box: " .. inside)
        end
    end,
}
