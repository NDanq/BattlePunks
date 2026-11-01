-- RangeShooter.nmprefab's script: the shooter's pistol, its shots and what the HUD shows.
-- Its movement and camera are RangeMove's (WorldsLabRange.cpp); this reads
-- the same harness (input map "Range").
--
--   H (or Fire while holstered)  draw / holster: the anim state's Armed. The
--        pistol moves between holster and hand on the clips' WeaponToHand and
--        WeaponToHolster notifies (the Blender file's Child Of offsets).
--   Fire  hit-scan from the camera through the crosshair (Physics.Raycast,
--        Jolt), skipping layer 2 -- the catwalks' grating. A steel plate is
--        knocked swinging on its hinge (an impulse), and rings; paper scores;
--        a target dummy (its Ragdoll's hit boxes) takes three body shots or
--        one to the head, then falls along the shot and drops its pistol --
--        a shot body is kicked again; anything else kicks up dust. Muzzle
--        flash and smoke at the barrel.
--   Aim   scope in: RangeMove draws the camera in over the shoulder and narrows
--        the view; tighter spread, a slower walk
--   R     reload: 17 a magazine, from the reserve
--
-- The HUD (Content/UI/ShootingRange/RangeHUD.nmui) is the mode's (WorldsLabRange.cpp): it reads
-- the shooter's RangeGun component, which this script keeps. A spawned prefab's script runs on
-- the authority only, so on a Play Together client the gun arrives replicated. The clock is
-- the mode's too. The pause menu's Reset Score reaches the server as an RPC; its tally is the
-- gun's, and the script hears Range.ScoreReset for its own kills. The flash, the
-- sounds and the impacts are every process's to show (gun:Shot, gun:Sound: multicast RPCs), and
-- the mode poses every shooter's pistol from its gun (holstered, or in hand once Ready).

local DRAW_SECONDS = 0.5   -- a dedicated server runs no animation, so no WeaponToHand: in hand after this
-- What a shot hit, for the effect and sound every process shows (gun:Shot; ERangeHit in WorldsLabRange.cpp).
local HIT_NOTHING, HIT_STEEL, HIT_DUMMY, HIT_PAPER, HIT_OTHER, HIT_DUMMY_DOWN = 0, 1, 2, 3, 4, 5
-- gun:Sound's (ERangeSound).
local SOUND_RELOAD, SOUND_DRY_FIRE = 0, 1

local MAGAZINE     = 17
local RESERVE      = 68
local RELOAD_TIME  = 1.7
local FIRE_DELAY   = 0.11     -- seconds between shots (it is semi-automatic: one a press)
local RANGE        = 6000
local SHOT_MASK    = 32767 - 2 -- every layer but 2: rounds pass the catwalks' grating
local STEEL_KICK   = 2400     -- impulse along the shot into a steel plate
local DUMMY_HEALTH = 3        -- body shots a dummy takes; a head shot is all of them
local KILL_KICK    = 6000     -- impulse along the killing shot (kg cm/s), at the part hit
local BODY_KICK    = 2500     -- along a shot into one already down
local SPREAD_HIP   = 0.9      -- degrees
local SPREAD_AIM   = 0.15

-- The view direction of a rotation (pitch, yaw) in degrees: +Z turned (Rz * Ry * Rx).
local function Forward(pitch, yaw)
    local p, y = math.rad(pitch), math.rad(yaw)
    return math.sin(y) * math.cos(p), -math.sin(p), math.cos(y) * math.cos(p)
end

return {
    OnStart = function(self)
        self.Animation = self.Root:GetComponent("Animation")
        self.Harness   = self.Root:GetComponent("AvatarHarness")
        self.Camera    = self.Instance:Find("Camera"):GetComponent("Transform")
        self.Boom      = self.Instance:Find("CameraBoom"):GetComponent("Transform")
        -- What the HUD shows, wherever it is shown (RangeGun, WorldsLabRange.cpp).
        self.Gun = self.Root:GetComponent("RangeGun") or self.Root:AddComponent("RangeGun")
        self.Mag, self.Reserve = MAGAZINE, RESERVE
        self.Gun:SetCapacity(MAGAZINE)
        self.Gun:SetShots(0)
        self.Gun:SetHits(0)
        self.Drawn, self.Ready = false, false
        self.DrawTime = 0.0
        self.Reloading, self.Cooldown = 0.0, 0.0
        self.StatusTime = 0.0
        self.Kills = 0
        self.Animation:SetBool("Armed", false)
        self:SetStatus("")
        self:UpdateHud()

        -- Reset Score: the gun's tally was zeroed by its RPC (WorldsLabRange.cpp); our kills go too.
        -- A dummy's wounds are the dummy's, shared by every shooter, so they stay.
        self.ResetHandle = Events.On("Range.ScoreReset", function()
            self.Kills = 0
        end)

        local id = self.Root:GetID()
        Events.On("AnimNotify", function(entity, name)
            if entity:GetID() ~= id then return end
            -- The mode seats the pistol in the hand once the gun is Ready (PoseShooters).
            if name == "WeaponToHand" then
                self.Ready = self.Drawn
                self:UpdateHud()
            end
        end)
    end,

    OnDestroy = function(self)
        Events.Off(self.ResetHandle)
    end,

    OnPostPhysics = function(self, dt)
        self.Cooldown = math.max(0.0, self.Cooldown - dt)
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

        -- No animation on a dedicated server, so no WeaponToHand: the pistol is in hand a moment after the draw.
        if self.DrawTime > 0.0 then
            self.DrawTime = self.DrawTime - dt
            if self.DrawTime <= 0.0 and self.Drawn and not self.Ready then
                self.Ready = true
                self:UpdateHud()
            end
        end

        local h = self.Harness
        -- With the mouse free a click takes it back (the mode), and shoots nothing. Only where the
        -- shooter is driven: on a server a client's shooter fires from its input, whatever the mouse.
        if h:IsLocallyControlled() and not Input.IsMouseCaptured() and not Config.GetBool("WorldsLab.RangeAutoPlay") then return end
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
        self.DrawTime = (drawn and Net.IsDedicatedServer()) and DRAW_SECONDS or 0.0
        self.Reloading = 0.0
        self.Animation:SetBool("Armed", drawn)
        self:SetStatus("")
        self:UpdateHud()
    end,

    Reload = function(self)
        if not self.Ready or self.Reloading > 0.0 or self.Mag >= MAGAZINE or self.Reserve <= 0 then return end
        self.Reloading = RELOAD_TIME
        self:SetStatus("RELOADING")
        self.Gun:Sound(SOUND_RELOAD)
    end,

    Fire = function(self, aiming)
        if not self.Ready or self.Reloading > 0.0 or self.Cooldown > 0.0 then return end
        if self.Mag <= 0 then
            self.Gun:Sound(SOUND_DRY_FIRE)
            self:SetStatus(self.Reserve > 0 and "EMPTY  -  R TO RELOAD" or "OUT OF AMMO", 1.5)
            return HIT_DUMMY, BODY_KICK
        end
        self.Mag = self.Mag - 1
        self.Gun:SetShots(self.Gun:GetShots() + 1)
        self.Cooldown = FIRE_DELAY
        self.Animation:SetTrigger("Fire")   -- the anim state's Recoil layer kicks the gun hand

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

        -- What it hit decides here; every process shows it (gun:Shot, MulticastShot in WorldsLabRange.cpp).
        local hit, entity, hx, hy, hz = Physics.Raycast(ox, oy, oz, dx, dy, dz, RANGE, SHOT_MASK)
        local kind, kick = HIT_NOTHING, 0   -- kick: what physics took here; the clients' copies take it too
        if hit and entity then
            local name = entity:GetName()
            if name == "SteelPlate" then
                kind, kick = HIT_STEEL, STEEL_KICK
                self.Gun:SetHits(self.Gun:GetHits() + 1)
                local body = entity:GetComponent("Rigid Body")
                if body then body:AddImpulse(dx * STEEL_KICK, dy * STEEL_KICK, dz * STEEL_KICK) end
            elseif entity:GetComponent("Ragdoll") then
                kind, kick = self:HitDummy(entity, entity:GetComponent("Ragdoll"), dx, dy, dz, hx, hy, hz)
            elseif name == "PaperStand" then
                kind = HIT_PAPER
                self.Gun:SetHits(self.Gun:GetHits() + 1)
            else
                kind = HIT_OTHER
            end
        else
            hx, hy, hz = ox + dx * RANGE, oy + dy * RANGE, oz + dz * RANGE
        end
        self.Gun:Shot(hx, hy, hz, dx, dy, dz, kind, kick)
        if self.Mag == 0 then self:SetStatus(self.Reserve > 0 and "EMPTY  -  R TO RELOAD" or "OUT OF AMMO") end
        self:UpdateHud()
    end,

    -- A shot into a target dummy: wounds while it stands, its death on the last,
    -- a kick once it is down. The Ragdoll component applies the impulse at the
    -- part nearest the hit. Returns the hit's kind and the kick physics took, for gun:Shot.
    HitDummy = function(self, entity, ragdoll, dx, dy, dz, hx, hy, hz)
        self.Gun:SetHits(self.Gun:GetHits() + 1)
        if ragdoll:IsActive() then
            ragdoll:AddImpulse(dx * BODY_KICK, dy * BODY_KICK, dz * BODY_KICK, hx, hy, hz)
            return HIT_DUMMY, BODY_KICK
        end
        -- Its wounds are on the dummy (RangeTarget): one count, whoever shoots it.
        local target = entity:GetComponent("RangeTarget") or entity:AddComponent("RangeTarget")
        local head = ragdoll:GetPartAt(hx, hy, hz) == "Head"   -- the limb the round hit
        local wounds = target:GetWounds() + (head and DUMMY_HEALTH or 1)
        target:SetWounds(wounds)
        if wounds >= DUMMY_HEALTH then
            target:SetWounds(0)
            self.Kills = self.Kills + 1
            ragdoll:ActivateWithImpulse(dx * KILL_KICK, dy * KILL_KICK, dz * KILL_KICK, hx, hy, hz)
            self:SetStatus(head and "HEADSHOT" or "DOWN", 1.0)
            return HIT_DUMMY_DOWN, KILL_KICK
        end
        return HIT_DUMMY, 0
    end,

    SetStatus = function(self, text, seconds)
        self.Gun:SetStatus(text)
        self.StatusTime = seconds or 0.0
    end,

    -- The gun as the HUD shows it; the mode's HUD reads it wherever this shooter is driven.
    UpdateHud = function(self)
        self.Gun:SetMag(self.Mag)
        self.Gun:SetReserve(self.Reserve)
        self.Gun:SetDrawn(self.Drawn)
        self.Gun:SetReady(self.Ready)
    end,
}
