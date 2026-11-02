-- The archive's keycard reader (L2), named by KeycardReader.nmprefab: an
-- interactable whose use is decided here, in Lua, rather than in C++.
--
-- Its LabInteractable does the part every interactable shares: the prompt, the
-- reach, the press, the glow. A press emits "L2.ReaderSwiped"; this script
-- decides what that means. With the keycard (L2.Keycard) the archive opens
-- (L2.ArchiveOpen) and the reader is spent; without it, access is denied and
-- the reader stays usable. Its state comes from the latched signals, so a
-- checkpoint reload finds it right.
local ACCEPT = "Content/Sounds/World/Keycard_Accept.nmsound"
local DENY   = "Content/Sounds/World/Keycard_Deny.nmsound"

return {
    OnStart = function(self)
        self.Use = self.Entity:GetComponent("LabInteractable")
        self.Handle = Events.On("Lab.Signal", function(channel, value) self:OnSignal(channel, value) end)
        self:Refresh()
    end,

    OnDestroy = function(self)
        if self.Handle then Events.Off(self.Handle) end
    end,

    -- The prompt and the glow follow the story: it glows (an objective) only
    -- once the keycard is held, and is done once the archive is open.
    Refresh = function(self)
        if Lab.GetSignal("L2.ArchiveOpen") then
            self.Use:SetPrompt("Archive open")
            self.Use:SetHighlight("none")
            self.Use:SetOnce(true)
        elseif Lab.GetSignal("L2.Keycard") then
            self.Use:SetPrompt("Swipe the keycard")
            self.Use:SetHighlight("")
        else
            self.Use:SetPrompt("Keycard reader (locked)")
            self.Use:SetHighlight("none")
        end
    end,

    OnSignal = function(self, channel, value)
        if channel == "L2.ReaderSwiped" then
            local x, y, z = self.Entity:GetComponent("Transform"):GetPosition()
            if Lab.GetSignal("L2.Keycard") then
                Audio.PlaySoundAt(ACCEPT, x, y, z, 1.0)
                Lab.Emit("L2.ArchiveOpen", true)
                self.Use:SetOnce(true)              -- used and Once: the C++ side stops offering it
                self.Use:SetPrompt("Archive open")
                self.Use:SetHighlight("none")
            else
                Audio.PlaySoundAt(DENY, x, y, z, 1.0)
                Lab.ShowBanner("Access denied. The keycard is in the workshop.", 3)
            end
        elseif channel == "L2.Keycard" then
            self:Refresh()
        end
    end,
}
