-- WorldsLab's startup script ([World_WorldsLab] StartupScript in
-- Config/DefaultEngine.ini), run on every load of the world after its mode has
-- loaded. Place prefabs, subscribe to events:
--
--   World.SpawnPrefab("Content/Prefabs/Door.nmprefab", 100, 200, 0)
--   Events.On("EntityCreated", function(entity) print(entity:GetName()) end)
--
-- Games/Sandbox/Scripts shows more.

print("WorldsLab: Main.lua loaded")

-- Sequencer phase-0 smoke test (local): a hand-written .nmseq moves SeqMover.
local mover = World.CreateEntity("SeqMover")
mover:AddComponent("Transform")

local function where(e)
    local x, y, z = e:GetComponent("Transform"):GetPosition()
    local rx, ry, rz = e:GetComponent("Transform"):GetRotation()
    return string.format("pos.x=%.2f rot.y=%.2f", x, ry)
end

Events.On("Smoke.Begin",   function() print("[SeqSmoke] Begin " .. where(mover)) end)
Events.On("Smoke.Halfway", function(e, label) print("[SeqSmoke] Halfway (" .. tostring(label) .. ") " .. where(e)) end)
Events.On("Smoke.Done",    function(e) print("[SeqSmoke] Done " .. where(e)) end)
Events.On("SequenceStarted",  function(p, path) print("[SeqSmoke] SequenceStarted " .. path) end)
Events.On("SequenceFinished", function(p, path) print("[SeqSmoke] SequenceFinished " .. path .. " " .. where(mover)) end)

local player = Sequence.Play("Content/Cinematics/SequencerSmoke.nmseq")
print("[SeqSmoke] Play -> " .. tostring(player) .. ", duration " .. tostring(Sequence.GetDuration(player)))
