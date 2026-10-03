-- @PROJECT_NAME@'s startup script ([Scripting] StartupScript in
-- Config/DefaultEngine.ini), run on every level load after the level has built
-- its scene. Place prefabs, subscribe to events:
--
--   World.SpawnPrefab("Content/Prefabs/Door.nmprefab", 100, 200, 0)
--   Events.On("EntityCreated", function(entity) print(entity:GetName()) end)
--
-- Games/Sandbox/Scripts shows more.

print("@PROJECT_NAME@: Main.lua loaded")
