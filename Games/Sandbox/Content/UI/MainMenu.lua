-- MainMenu.lua -- the Sandbox main menu's handlers. Content/UI/MainMenu.nmui
-- runs this once when it opens (its Script attribute) and names these from
-- its buttons: OnClick="Lua:MainMenu.Play".

MainMenu = MainMenu or {}

-- Play: the menu goes, and the game it held paused starts.
function MainMenu.Play()
    UI.Close("Content/UI/MainMenu.nmui")
    Game.SetPaused(false)
end
