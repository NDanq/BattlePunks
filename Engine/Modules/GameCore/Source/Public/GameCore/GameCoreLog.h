#ifndef GAMECORE_GAMECORELOG_H
#define GAMECORE_GAMECORELOG_H

#include <string>

#include "Public/Core/CoreLog.h"

/*
 * GameCore's logging, tagged and routed through EngineCore's seam.
 *
 * Exactly the arrangement GDILog.h and GameWindowLog.h use: this module never
 * chooses where text goes, it only names itself. The host installs one
 * LogHandler and sees Core, GDI, GameWindow and GameCore messages in the same
 * sink.
 *
 * A Lua script's own print() and its runtime errors are routed through these
 * macros too (see ScriptEngine.cpp), with a "[Lua] " sub-tag ahead of the
 * message text so a script's own noise is visually distinct from the module's.
 */

#define GAMECORE_LOG(Message)       CORE_LOG     (::std::string("[GameCore] ") + (Message))
#define GAMECORE_LOG_WARN(Message)  CORE_LOG_WARN(::std::string("[GameCore] ") + (Message))
#define GAMECORE_LOG_ERROR(Message) CORE_LOG_ERROR(::std::string("[GameCore] ") + (Message))

#endif //GAMECORE_GAMECORELOG_H
