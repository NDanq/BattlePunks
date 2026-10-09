-- Content/Prefabs/Kart.nmprefab's script: a kart lights up in its driver's
-- colour, and goes grey again when they get out.
--
-- OnPossessed and OnUnpossessed come from the avatar system (Engine/Source/
-- AVATARS.md) on EVERY process -- the server as it happens, a client as the
-- change replicates -- so each one tints its own copy and nothing extra is
-- sent. A client that joins while a kart is driven hears it too.

local Idle = { 0.35, 0.35, 0.38 }

-- NetArena's capsule colours, by player: red, blue, green, yellow.
local Colors = {
    [0] = { 0.90, 0.08, 0.08 },
    [1] = { 0.08, 0.25, 0.95 },
    [2] = { 0.10, 0.80, 0.20 },
    [3] = { 0.95, 0.85, 0.10 },
}

return {
    OnStart = function(self)
        self.Body = self.Instance:Find("Body"):GetComponent("Render")
    end,

    Tint = function(self, rgb)
        self.Body:SetMaterialParameter(0, "BaseColor", rgb[1], rgb[2], rgb[3], 0)
    end,

    -- player: the entity with the PlayerState, not the capsule it left.
    OnPossessed = function(self, player)
        local state = player:GetComponent("PlayerState")
        self:Tint(state and Colors[state:GetPlayerIndex() % 4] or Idle)
        print("NetArena (Lua): " .. player:GetName() .. " is driving " .. self.Entity:GetName())
    end,

    OnUnpossessed = function(self, player)
        self:Tint(Idle)
    end,
}
