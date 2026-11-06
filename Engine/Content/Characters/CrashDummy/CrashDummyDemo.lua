-- CrashDummyDemo.nmprefab's script: walks the dummy through everything its
-- anim state does (ANIMATION.md, "Engine characters"), on a loop --
--
--   idle -> walk -> draw the pistol while walking, fire three shots -> armed run,
--   hit from the front, then hard from the left -> stop ->
--   jump, fall, land -> holster -> again
--
-- and moves the pistol between holster and hand as the clips' notifies say:
-- WeaponToHand in UB_EquipWeapon (frame 11, the hand on the grip) and
-- WeaponToHolster in UB_HolsterWeapon (frame 20, the gun back in the holster).
-- The Pistol's Anim Socket rides one bone or the other; the offsets are the
-- Blender file's Child Of constraints, converted to the engine's axes.
--
-- Through all of it the upper body nods up and down: AimPitch, the anim
-- state's look-at, which bends the spine by that many degrees (up positive,
-- clamped to -40..55).
--
-- A game drives the same parameters from its own movement instead:
-- Speed (units/s), Grounded, Jump (a trigger) and Armed -- AimPitch from
-- its camera's pitch, Fire (a trigger, the Recoil layer's kick) on each shot,
-- and on each hit taken HitDir (0 front, 1 back, 2 left, 3 right), HitWeight
-- (0..1) and Hit (a trigger, the HitReact layer's jolt).

local HOLSTER = { Bone = "DEF-spine",  Position = { -19.719, 0.128, -4.773 }, Rotation = { -75.407,  7.744,   2.009 } }
local HAND    = { Bone = "DEF-hand.R", Position = {  -2.110, 7.087, -0.769 }, Rotation = { -85.592, 13.264, 175.197 } }

-- What happens when, in seconds from the start of a loop.
local TIMELINE = {
    { At =  0.0, Speed =   0 },
    { At =  2.0, Speed = 150 },
    { At =  4.0, Armed = true },
    { At =  5.6, Fire  = true },
    { At =  5.9, Fire  = true },
    { At =  6.3, Fire  = true },
    { At =  7.0, Speed = 400 },
    { At =  8.0, Hit   = 0, Weight = 0.6 },
    { At =  9.0, Hit   = 2, Weight = 1.0 },
    { At = 10.0, Speed =   0 },
    { At = 11.5, Jump  = true },
    { At = 14.5, Armed = false },
}
local LOOP_SECONDS = 17.0
local AIR_SECONDS  = 0.6     -- from the jump's take-off to touching down
local ACCELERATION = 400.0   -- units/s each second, toward the timeline's Speed
local NOD_DEGREES  = 35.0    -- how far AimPitch swings each way
local NOD_SECONDS  = 4.0     -- one swing up and back down

local function Seat(socket, where)
    socket:SetBone(where.Bone)
    socket:SetPositionOffset(where.Position[1], where.Position[2], where.Position[3])
    socket:SetRotationOffset(where.Rotation[1], where.Rotation[2], where.Rotation[3])
end

return {
    OnStart = function(self)
        self.Animation = self.Root:GetComponent("Animation")
        self.Socket    = self.Instance:Find("Pistol"):GetComponent("Anim Socket")
        self.Time      = 0.0
        self.Next      = 1
        self.Speed     = 0.0
        self.Target    = 0.0
        self.AirLeft   = -1.0
        Seat(self.Socket, HOLSTER)

        local id = self.Root:GetID()
        Events.On("AnimNotify", function(entity, name)
            if entity:GetID() ~= id then return end
            if name == "WeaponToHand" then Seat(self.Socket, HAND)
            elseif name == "WeaponToHolster" then Seat(self.Socket, HOLSTER) end
        end)
        -- Off the ground once the take-off has played.
        Events.On("AnimEnded", function(entity, state, layer)
            if entity:GetID() == id and state == "JumpStart" then
                self.Animation:SetBool("Grounded", false)
                self.AirLeft = AIR_SECONDS
            end
        end)
    end,

    OnPostPhysics = function(self, dt)
        self.Time = self.Time + dt
        if self.Time >= LOOP_SECONDS then
            self.Time = self.Time - LOOP_SECONDS
            self.Next = 1
        end
        while TIMELINE[self.Next] and TIMELINE[self.Next].At <= self.Time do
            local step = TIMELINE[self.Next]
            if step.Speed ~= nil then self.Target = step.Speed end
            if step.Armed ~= nil then self.Animation:SetBool("Armed", step.Armed) end
            if step.Jump then self.Animation:SetTrigger("Jump") end
            if step.Fire then self.Animation:SetTrigger("Fire") end
            if step.Hit ~= nil then
                self.Animation:SetFloat("HitDir", step.Hit)
                self.Animation:SetFloat("HitWeight", step.Weight)
                self.Animation:SetTrigger("Hit")
            end
            self.Next = self.Next + 1
        end

        local change = ACCELERATION * dt
        self.Speed = self.Speed + math.max(-change, math.min(change, self.Target - self.Speed))
        self.Animation:SetFloat("Speed", self.Speed)
        self.Animation:SetFloat("AimPitch", NOD_DEGREES * math.sin(self.Time * 2.0 * math.pi / NOD_SECONDS))

        if self.AirLeft >= 0.0 then
            self.AirLeft = self.AirLeft - dt
            if self.AirLeft < 0.0 then self.Animation:SetBool("Grounded", true) end
        end
    end,
}
