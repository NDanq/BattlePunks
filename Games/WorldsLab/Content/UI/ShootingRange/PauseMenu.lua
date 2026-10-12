-- The pause menu's Script (PauseMenu.nmui names it): run once, the first time the
-- document opens, in the game's Lua state. A button's "Lua:RangeMenu.ResetScore" finds
-- this function by its global path.

local MENU = "Content/UI/ShootingRange/PauseMenu.nmui"

RangeMenu = RangeMenu or {}

-- Reset Score: the mode (WorldsLabRange.cpp) hears the event on this process and asks the
-- server to zero the shooter's tally (RangeGun's ServerResetScore); the menu says it happened.
function RangeMenu.ResetScore()
    Events.Emit("Range.ResetScore")
    UI.SetText(MENU, "Notice", "Score reset")
end
