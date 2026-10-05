#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ScriptLibrary.h"

#include <set>
#include <string>

#include "Public/GameCore/GameCoreLog.h"

/*
 * The "Config" script library: the engine's config vars ([Section] Key in an
 * .ini, the console's set) by "Section.Key", for Lua and script graphs -- a
 * settings menu reads and writes them:
 *
 *     if Config.GetBool("Shadows.Enabled") then ... end
 *     Config.SetNumber("LightLab.LookSensitivity", 1.5)
 *     Config.Save()   -- the player's settings file, so they survive a restart
 *
 * Typed, so a graph's pins are: GetBool on a float var is a script error,
 * not a quiet conversion. A set fires the var's callback as an .ini would.
 *
 * A var that is not declared -- a game module's, in an editor that has not
 * loaded it (the UI Designer's Preview) -- is warned about once and reads as
 * false / 0 / "": a menu still opens. A set of one does nothing.
 */

namespace
{
    /* InCall's var by its first argument, of InType; null, with a script error, if not. */
    EngineCore::ConfigVar* FindVar(GameCore::ScriptCall& InCall, const EngineCore::EConfigType InType, const char* InTypeName)
    {
        const char* Path = InCall.GetString(1);
        EngineCore::ConfigVar* Var = Path && *Path ? EngineCore::ConfigFindVarByPath(Path) : nullptr;
        if(!Var)
        {
            static std::set<std::string> Warned;
            if(Warned.insert(Path ? Path : "").second)
            {
                GAMECORE_LOG_WARN(std::string("Config: no config var '") + (Path ? Path : "") + "' (its module not loaded?) -- reads as a default")
            }
            return nullptr;
        }
        const bool bNumber = InType == EngineCore::EConfigType::CT_Float;
        const bool bFits = Var->GetType() == InType
                        || (bNumber && Var->GetType() == EngineCore::EConfigType::CT_Int);
        if(!bFits)
        {
            InCall.Error((std::string("config var '") + Path + "' is not a " + InTypeName).c_str());
            return nullptr;
        }
        return Var;
    }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Config)
{
    Lib.Pure("Has", [](GameCore::ScriptCall& Call)
    {
        const char* Path = Call.GetString(1);
        Call.PushBool(Path && *Path && EngineCore::ConfigFindVarByPath(Path) != nullptr);
    }, "Path:string -> Exists:bool").Tooltip("Whether a config var \"Section.Key\" exists");

    Lib.Pure("GetBool", [](GameCore::ScriptCall& Call)
    {
        const EngineCore::ConfigVar* Var = FindVar(Call, EngineCore::EConfigType::CT_Bool, "bool");
        Call.PushBool(Var && Var->GetBool());
    }, "Path:string -> Value:bool").Tooltip("A bool config var, \"Shadows.Enabled\"");

    Lib.Pure("GetNumber", [](GameCore::ScriptCall& Call)
    {
        const EngineCore::ConfigVar* Var = FindVar(Call, EngineCore::EConfigType::CT_Float, "number");
        double Value = 0.0;
        if(Var) { Value = Var->GetType() == EngineCore::EConfigType::CT_Int ? double(Var->GetInt()) : double(Var->GetFloat()); }
        Call.PushNumber(Value);
    }, "Path:string -> Value:number").Tooltip("A float or int config var");

    Lib.Pure("GetString", [](GameCore::ScriptCall& Call)
    {
        const EngineCore::ConfigVar* Var = FindVar(Call, EngineCore::EConfigType::CT_String, "string");
        Call.PushString(Var ? Var->GetString() : "");
    }, "Path:string -> Value:string");

    Lib.Function("SetBool", [](GameCore::ScriptCall& Call)
    {
        if(EngineCore::ConfigVar* Var = FindVar(Call, EngineCore::EConfigType::CT_Bool, "bool")) { Var->SetBool(Call.GetBool(2)); }
    }, "Path:string, Value:bool").Tooltip("Sets a bool config var; its callback fires if it changed");

    Lib.Function("SetNumber", [](GameCore::ScriptCall& Call)
    {
        EngineCore::ConfigVar* Var = FindVar(Call, EngineCore::EConfigType::CT_Float, "number");
        if(!Var) return;
        if(Var->GetType() == EngineCore::EConfigType::CT_Int) { Var->SetInt(int32_t(Call.GetNumber(2))); }
        else                                                  { Var->SetFloat(float(Call.GetNumber(2))); }
    }, "Path:string, Value:number").Tooltip("Sets a float or int config var (an int is truncated)");

    Lib.Function("SetString", [](GameCore::ScriptCall& Call)
    {
        if(EngineCore::ConfigVar* Var = FindVar(Call, EngineCore::EConfigType::CT_String, "string")) { Var->SetString(Call.GetString(2)); }
    }, "Path:string, Value:string");

    Lib.Function("Save", [](GameCore::ScriptCall& Call)
    {
        Call.PushBool(EngineCore::ConfigSaveUserFile());   // logs why not
    }, "-> Saved:bool").Tooltip("Writes the player's settings (what differs from the game's own) to their user file. "
                               "An editor session has none: not saved, and says so");
}
