-- DemoLab's startup script ([World_DemoLab] StartupScript in
-- Config/DefaultEngine.ini), run on every load of the world after its mode has
-- loaded. Place prefabs, subscribe to events:
--
--   World.SpawnPrefab("Content/Prefabs/Door.nmprefab", 100, 200, 0)
--   Events.On("EntityCreated", function(entity) print(entity:GetName()) end)
--
-- Games/Sandbox/Scripts shows more.

print("DemoLab: Main.lua loaded")
