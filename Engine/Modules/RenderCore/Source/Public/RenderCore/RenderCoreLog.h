#ifndef RENDERCORE_RENDERCORELOG_H
#define RENDERCORE_RENDERCORELOG_H

#include <string>

#include "Public/Core/CoreLog.h"

/*
 * RenderCore's logging, tagged and routed through EngineCore's seam.
 *
 * Exactly the arrangement GameCoreLog.h, GDILog.h and GameWindowLog.h use:
 * this module never chooses where text goes, it only names itself. The host
 * installs one LogHandler and sees Core, GDI (through RenderCore, which is
 * the only image that still links it), GameWindow, GameCore and RenderCore
 * messages in the same sink.
 */

#define RENDERCORE_LOG(Message)       CORE_LOG     (::std::string("[RenderCore] ") + (Message))
#define RENDERCORE_LOG_WARN(Message)  CORE_LOG_WARN(::std::string("[RenderCore] ") + (Message))
#define RENDERCORE_LOG_ERROR(Message) CORE_LOG_ERROR(::std::string("[RenderCore] ") + (Message))

#endif //RENDERCORE_RENDERCORELOG_H
