-- DayCycle's startup script ([World_DayCycle] StartupScript): the two plugins
-- this project enables, used from Lua (Engine/Docs/Guides/PluginGuide.md).
-- Each plugin's script library is a Lua global named for it.

-- Spinner (Plugins/Spinner, project-local): add its component at run time
-- and drive it through its library.
local pillar = World.FindEntity("Pillar")
if pillar then
    pillar:AddComponent("Spinner")
    Spinner.SetSpeed(pillar, 20)
end

-- TimeOfDay (Engine/Plugins/TimeOfDay): react to the clock. The pillar
-- spins faster at night, and each hour is logged as it begins.
Events.On("TimeOfDayHour", function(hour, clock)
    print(string.format("[DayCycle] %02d:00, %s", hour, TimeOfDay.IsDaytime() and "day" or "night"))
    if pillar then
        Spinner.SetSpeed(pillar, TimeOfDay.IsDaytime() and 20 or 90)
    end
end)
