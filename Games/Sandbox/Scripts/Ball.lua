-- BrickBreaker's ball, in plain Lua (Prefabs/Ball.nmprefab names it). It asks
-- the game framework what to do rather than deciding for itself:
--
--   Game.IsPhase / Game.PhaseChanged   moving only while the phase is Playing
--   Game.GetPlayer(0)                  the paddle (the entity with a PlayerState)
--   Game.AddLives                      a lost ball; the mode's rules pick the next phase
--   Game.GetRound                      a little faster each round
--
-- Walls are the edges of the 1280 x 720 view (+Y down); the paddle and bricks
-- are colliders, met in OnOverlapBegin at the start of TP_PostPhysics.

local FieldWidth     = 1280
local FieldHeight    = 720
local BaseSpeed      = 420              -- pixels per second in the first round
local SpeedPerRound  = 40
local MaxBounceAngle = math.rad(60)     -- off the paddle's very edge

local function Speed()
    return BaseSpeed + SpeedPerRound * Game.GetRound()
end

return {
    OnStart = function(self)
        self.VX, self.VY = 0, 0
        self.Half = self.Entity:GetComponent("Transform"):GetScale() * 0.5
        self.PhaseHandle = Events.On(Game.PhaseChanged, function(phase, previous) self:OnPhase(phase, previous) end)
    end,

    OnDestroy = function(self)
        Events.Off(self.PhaseHandle)
    end,

    -- Launched when the phase becomes Playing, stopped by anything else.
    OnPhase = function(self, phase)
        if phase == "Playing" then
            local angle = math.rad(math.random(-25, 25))
            self.VX, self.VY = Speed() * math.sin(angle), -Speed() * math.cos(angle)
        else
            self.VX, self.VY = 0, 0
        end
    end,

    OnPhysics = function(self, dt)
        local transform = self.Entity:GetComponent("Transform")
        self.Bounced = false

        if Game.IsPhase("Serving") then
            -- Riding the paddle until it launches us.
            local paddle = Game.GetPlayer(0)
            if paddle:IsValid() then
                local paddleTransform = paddle:GetComponent("Transform")
                local px, py = paddleTransform:GetPosition()
                local _, paddleHeight = paddleTransform:GetScale()
                transform:SetPosition(px, py - paddleHeight * 0.5 - self.Half - 2, 0)
            end
            return
        end

        if not Game.IsPhase("Playing") then
            return
        end

        local x, y = transform:GetPosition()
        x = x + self.VX * dt
        y = y + self.VY * dt

        if x < self.Half then
            x, self.VX = self.Half, math.abs(self.VX)
        elseif x > FieldWidth - self.Half then
            x, self.VX = FieldWidth - self.Half, -math.abs(self.VX)
        end
        if y < self.Half then
            y, self.VY = self.Half, math.abs(self.VY)
        end
        transform:SetPosition(x, y, 0)

        -- Out of the bottom: a life, and the rules decide what happens next.
        if y > FieldHeight + self.Half * 2 then
            self.VX, self.VY = 0, 0
            Game.AddLives(Game.GetPlayer(0), -1)
        end
    end,

    OnOverlapBegin = function(self, other)
        -- One bounce a step, even between two bricks.
        if self.Bounced or not Game.IsPhase("Playing") then
            return
        end

        local transform = self.Entity:GetComponent("Transform")
        local x, y = transform:GetPosition()
        local otherTransform = other:GetComponent("Transform")
        local ox, oy = otherTransform:GetPosition()
        local width, height = otherTransform:GetScale()
        local name = other:GetName()

        if name == "Paddle" then
            if self.VY <= 0 then
                return   -- already on the way up
            end
            -- Where it hit sets the angle: the centre straight up, the edges steep.
            local offset = math.max(-1, math.min(1, (x - ox) / (width * 0.5)))
            local angle = offset * MaxBounceAngle
            self.VX, self.VY = Speed() * math.sin(angle), -Speed() * math.cos(angle)
            transform:SetPosition(x, oy - height * 0.5 - self.Half, 0)
            self.Bounced = true
        elseif name == "Brick" then
            -- Out along the shallower overlap: a side hit turns X, a face hit Y.
            local overlapX = width * 0.5 + self.Half - math.abs(x - ox)
            local overlapY = height * 0.5 + self.Half - math.abs(y - oy)
            if overlapX < overlapY then
                self.VX = (x < ox) and -math.abs(self.VX) or math.abs(self.VX)
            else
                self.VY = (y < oy) and -math.abs(self.VY) or math.abs(self.VY)
            end
            self.Bounced = true
        end
    end,
}
