-- Demo Lab's credits (Credits.nmui's Script). Written by Tools/UI/make_credits.py
-- from Docs/Credits.md: edit that, then rerun the script.

LabCredits = LabCredits or {}

LabCredits.Text = table.concat({
    "A NimbleEngine showcase",
    "",
    "DESIGN, CODE AND LEVELS",
    "The NimbleEngine team, with Claude",
    "",
    "GAMEPLAY",
    "Ported from BattlePunks: player, combat, AI, encounters, story mode",
    "",
    "CHARACTERS",
    "The SteamDummy, skinned to the CrashDummy rig",
    "",
    "ENGINE",
    "NimbleEngine: Vulkan renderer, DDGI, Jolt Physics, Recast navigation, Sequencer, UI",
    "Plugins: Time Of Day, Weather",
    "",
    "MUSIC",
    "Unexplored Expansion, by Bo Jingles (a longer loop of TAD's CC0 \"Unexplored (short ver.)\") (CC0)",
    "Factory ambiance, by yd (CC0)",
    "Determined Pursuit (epic orchestra loop), by Emma_MA (CC0)",
    "Epic Endgame Cinematic, by cynicmusic (pixelsphere.org, cynicmusic.com) (CC0)",
    "",
    "NATURE RECORDINGS (AMBIENCE)",
    "Beach Ocean Waves (wave_01..04), by jasinski (Freesound #18363), extracted by qubodup (CC0)",
    "Park ambiences (park_ambience_wind.wav), by Thimras (CC0)",
    "Park ambiences (park_ambience_wind.wav, park_ambience_birds.wav), by Thimras (CC0)",
    "Tree Creaking, by Department64, cleaned up by AntumDeluge (CC0)",
    "",
    "FOLEY (KENNEY AUDIO PACKS)",
    "Impact Sounds, by Kenney (CC0)",
    "RPG Audio, by Kenney (CC0)",
    "Interface Sounds, by Kenney (CC0)",
    "",
    "SYNTHESISED IN THIS REPOSITORY",
    "Robots, weapons, machinery, doors, alarms, the beacon, the lab and reactor ambiences (Tools/Audio)",
    "",
    "Thank you for playing.",
}, "\n")

function LabCredits.Show()
    UI.SetString("Credits.Text", LabCredits.Text)
end
