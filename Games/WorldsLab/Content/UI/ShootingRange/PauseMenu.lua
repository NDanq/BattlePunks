-- The pause menu's Script (PauseMenu.nmui names it): run once, the first time the
-- document opens, in the game's Lua state. A button's "Lua:RangeMenu.ResetScore" finds
-- this function by its global path.

local MENU = "Content/UI/ShootingRange/PauseMenu.nmui"

RangeMenu = RangeMenu or {}

-- Reset Score: the shooter (RangeShooter.lua) listens for the event and zeroes its
-- tally; the menu says it happened.
function RangeMenu.ResetScore()
    Events.Emit("Range.ResetScore")
    UI.SetText(MENU, "Notice", "Score reset")
end
