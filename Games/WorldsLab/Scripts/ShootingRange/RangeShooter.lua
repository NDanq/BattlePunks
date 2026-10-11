-- RangeShooter.nmprefab's script: the shooter's pistol, its shots and the HUD.
-- Its movement and camera are RangeMove's (WorldsLabRange.cpp); this reads
-- the same harness (input map "Range").
--
--   H (or Fire while holstered)  draw / holster: the anim state's Armed. The
--        pistol moves between holster and hand on the clips' WeaponToHand and
--        WeaponToHolster notifies (the Blender file's Child Of offsets).
--   Fire  hit-scan from the camera through the crosshair (Physics.Raycast,
--        Jolt), skipping layer 2 -- the catwalks' grating. A steel plate is
--        knocked swinging on its hinge (an impulse), and rings; paper scores;
--        anything else kicks up dust. Muzzle flash and smoke at the barrel.
--   Aim   scope in: RangeMove draws the camera in over the shoulder and narrows
--        the view; tighter spread, a slower walk
--   R     reload: 17 a magazine, from the reserve
--
-- The HUD is Content/UI/ShootingRange/RangeHUD.nmui, fed through UI strings.

local HUD = "Content/UI/ShootingRange/RangeHUD.nmui"
local SOUNDS = "Content/Sounds/ShootingRange/"
local PARTICLES = "Content/Particles/ShootingRange/"

local HOLSTER = { Bone = "DEF-spine",  Position = { -19.719, 0.128, -4.773 }, Rotation = { -75.407,  7.744,   2.009 } }
local HAND    = { Bone = "DEF-hand.R", Position = {  -2.110, 7.087, -0.769 }, Rotation = { -85.592, 13.264, 175.197 } }

local MAGAZINE     = 17
local RESERVE      = 68
local RELOAD_TIME  = 1.7
local FIRE_DELAY   = 0.11     -- seconds between shots (it is semi-automatic: one a press)
local RANGE        = 6000
local SHOT_MASK    = 32767 - 2 -- every layer but 2: rounds pass the catwalks' grating
local STEEL_KICK   = 2400     -- impulse along the shot into a steel plate
local SPREAD_HIP   = 0.9      -- degrees
local SPREAD_AIM   = 0.15

local function Seat(socket, where)
    socket:SetBone(where.Bone)
    socket:SetPositionOffset(where.Position[1], where.Position[2], where.Position[3])
    socket:SetRotationOffset(where.Rotation[1], where.Rotation[2], where.Rotation[3])
end

-- The view direction of a rotation (pitch, yaw) in degrees: +Z turned (Rz * Ry * Rx).
local function Forward(pitch, yaw)
    local p, y = math.rad(pitch), math.rad(yaw)
    return math.sin(y) * math.cos(p), -math.sin(p), math.cos(y) * math.cos(p)
end

return {
    OnStart = function(self)
        self.Animation = self.Root:GetComponent("Animation")
        self.Harness   = self.Root:GetComponent("AvatarHarness")
        self.Socket    = self.Instance:Find("Pistol"):GetComponent("Anim Socket")
        self.Muzzle    = self.Instance:Find("Muzzle"):GetComponent("Transform")
        self.Camera    = self.Instance:Find("Camera"):GetComponent("Transform")
        self.Boom      = self.Instance:Find("CameraBoom"):GetComponent("Transform")
        self.Mag, self.Reserve = MAGAZINE, RESERVE
        self.Shots, self.Hits = 0, 0
        self.Drawn, self.Ready = false, false
        self.Reloading, self.Cooldown = 0.0, 0.0
        self.StatusTime = 0.0
        self.Effects = {}
        Seat(self.Socket, HOLSTER)
        self.Animation:SetBool("Armed", false)

        UI.Open(HUD)
        self:UpdateHud()

        local id = self.Root:GetID()
        Events.On("AnimNotify", function(entity, name)
            if entity:GetID() ~= id then return end
            if name == "WeaponToHand" then
                Seat(self.Socket, HAND)
                self.Ready = self.Drawn
                self:UpdateHud()
            elseif name == "WeaponToHolster" then
                Seat(self.Socket, HOLSTER)
            end
        end)
    end,

    OnDestroy = function(self)
        UI.Close(HUD)
    end,

    OnPostPhysics = function(self, dt)
        self.Cooldown = math.max(0.0, self.Cooldown - dt)
        self:TickEffects(dt)
        if self.StatusTime > 0.0 then
            self.StatusTime = self.StatusTime - dt
            if self.StatusTime <= 0.0 then self:SetStatus("") end
        end

        if self.Reloading > 0.0 then
            self.Reloading = self.Reloading - dt
            if self.Reloading <= 0.0 then
                local take = math.min(MAGAZINE - self.Mag, self.Reserve)
                self.Mag, self.Reserve = self.Mag + take, self.Reserve - take
                self:SetStatus("")
                self:UpdateHud()
            end
        end

        local h = self.Harness
        -- With the mouse free a click takes it back (the mode), and shoots nothing.
        if not Input.IsMouseCaptured() and not Config.GetBool("WorldsLab.RangeAutoPlay") then return end
        if h:WasPressed("Holster") then
            self:SetDrawn(not self.Drawn)
        elseif h:WasPressed("Fire") then
            if not self.Drawn then self:SetDrawn(true) else self:Fire(h:IsDown("Aim")) end
        elseif h:WasPressed("Reload") then
            self:Reload()
        end
    end,

    SetDrawn = function(self, drawn)
        self.Drawn = drawn
        self.Ready = false            -- until the hand has the grip (WeaponToHand)
        self.Reloading = 0.0
        self.Animation:SetBool("Armed", drawn)
        self:SetStatus("")
        self:UpdateHud()
    end,

    Reload = function(self)
        if not self.Ready or self.Reloading > 0.0 or self.Mag >= MAGAZINE or self.Reserve <= 0 then return end
        self.Reloading = RELOAD_TIME
        self:SetStatus("RELOADING")
        Audio.PlaySound2D(SOUNDS .. "Reload.nmsound", 0.8)
    end,

    Fire = function(self, aiming)
        if not self.Ready or self.Reloading > 0.0 or self.Cooldown > 0.0 then return end
        if self.Mag <= 0 then
            Audio.PlaySound2D(SOUNDS .. "DryFire.nmsound", 0.9)
            self:SetStatus(self.Reserve > 0 and "EMPTY  -  R TO RELOAD" or "OUT OF AMMO", 1.5)
            return
        end
        self.Mag = self.Mag - 1
        self.Shots = self.Shots + 1
        self.Cooldown = FIRE_DELAY

        -- Through the crosshair: from the camera, along its view, a little spread.
        local cx, cy, cz = self.Camera:GetWorldPosition()
        local pitch, yaw = self.Camera:GetWorldRotation()
        local spread = aiming and SPREAD_AIM or SPREAD_HIP
        pitch = pitch + (math.random() * 2 - 1) * spread
        yaw = yaw + (math.random() * 2 - 1) * spread
        local dx, dy, dz = Forward(pitch, yaw)
        -- Start level with the shooter's shoulder, not behind it: the camera sits back of the
        -- boom by 2.3 m, or less scoped in (RangeMove) or pulled in by a wall.
        local bx0, by0, bz0 = self.Boom:GetWorldPosition()
        local ahead = math.max(0, (bx0 - cx) * dx + (by0 - cy) * dy + (bz0 - cz) * dz)
        local ox, oy, oz = cx + dx * ahead, cy + dy * ahead, cz + dz * ahead

        local mx, my, mz = self.Muzzle:GetWorldPosition()
        self:Effect("MuzzleFlash", mx, my, mz, 1.2)
        Audio.PlaySound2D(SOUNDS .. "Gunshot.nmsound", 1.0)

        local hit, entity, hx, hy, hz = Physics.Raycast(ox, oy, oz, dx, dy, dz, RANGE, SHOT_MASK)
        if hit and entity then
            local name = entity:GetName()
            local bx, by, bz = hx - dx * 3, hy - dy * 3, hz - dz * 3
            if name == "SteelPlate" then
                self.Hits = self.Hits + 1
                local body = entity:GetComponent("Rigid Body")
                if body then body:AddImpulse(dx * STEEL_KICK, dy * STEEL_KICK, dz * STEEL_KICK) end
                self:Effect("Sparks", bx, by, bz, 1.0)
                Audio.PlaySoundAt(SOUNDS .. "SteelHit.nmsound", hx, hy, hz, 1.0)
            elseif name == "PaperStand" then
                self.Hits = self.Hits + 1
                self:Effect("Dust", bx, by, bz, 1.4)
                Audio.PlaySoundAt(SOUNDS .. "PaperHit.nmsound", hx, hy, hz, 0.7)
            else
                self:Effect("Dust", bx, by, bz, 1.4)
                Audio.PlaySoundAt(SOUNDS .. "Impact.nmsound", hx, hy, hz, 0.6)
            end
        end
        if self.Mag == 0 then self:SetStatus(self.Reserve > 0 and "EMPTY  -  R TO RELOAD" or "OUT OF AMMO") end
        self:UpdateHud()
    end,

    -- A particle burst at a point, destroyed after its life.
    Effect = function(self, system, x, y, z, life)
        local fx = World.CreateEntity("RangeEffect")
        fx:AddComponent("Transform"):SetPosition(x, y, z)
        local particles = fx:AddComponent("Particle System")
        particles:SetSystem(PARTICLES .. system .. ".nmparticle")
        particles:SetEmitting(true)
        self.Effects[#self.Effects + 1] = { Entity = fx, Left = life }
    end,

    TickEffects = function(self, dt)
        for i = #self.Effects, 1, -1 do
            local e = self.Effects[i]
            e.Left = e.Left - dt
            if e.Left <= 0.0 then
                if e.Entity:IsValid() then e.Entity:Destroy() end
                table.remove(self.Effects, i)
            end
        end
    end,

    SetStatus = function(self, text, seconds)
        UI.SetString("Range.Status", text)
        self.StatusTime = seconds or 0.0
    end,

    UpdateHud = function(self)
        UI.SetString("Range.Mag", tostring(self.Mag))
        UI.SetString("Range.Reserve", tostring(self.Reserve))
        UI.SetNumber("Range.MagFill", self.Mag / MAGAZINE)
        UI.SetString("Range.Hits", tostring(self.Hits))
        UI.SetString("Range.Shots", tostring(self.Shots))
        UI.SetString("Range.Accuracy", self.Shots > 0 and string.format("%d%%", math.floor(100 * self.Hits / self.Shots + 0.5)) or "--")
        UI.SetVisible(HUD, "Crosshair", self.Drawn and self.Ready)
    end,
}
