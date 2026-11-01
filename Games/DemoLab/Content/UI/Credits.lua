-- Demo Lab's credits (Credits.nmui's Script): the text of Docs/Credits.md.
-- Keep the two in step when a sound or an asset is added.

LabCredits = LabCredits or {}

LabCredits.Text = table.concat({
    "A NimbleEngine showcase",
    "",
    "DESIGN, CODE AND LEVELS",
    "The NimbleEngine team, with Claude",
    "",
    "GAMEPLAY",
    "Ported from BattlePunks: player, combat, AI bots, encounters, story mode",
    "",
    "CHARACTERS",
    "The SteamDummy, skinned to the CrashDummy rig (BattlePunks)",
    "",
    "SOUNDS",
    "Gunshot, Dry Fire, Reload, Impact, Steel Hit: synthesised in this repository",
    "(Games/WorldsLab/Tools/ShootingRange/make_range_sounds.py); no third-party recordings",
    "",
    "ENGINE",
    "NimbleEngine: Vulkan renderer, DDGI, Jolt Physics, Recast navigation, Sequencer, UI",
    "Plugins: Time Of Day",
    "",
    "Thank you for playing.",
}, "\n")

function LabCredits.Show()
    UI.SetString("Credits.Text", LabCredits.Text)
end
