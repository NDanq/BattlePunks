#ifndef UI_UILOG_H
#define UI_UILOG_H

#include <string>

#include "Public/Core/CoreLog.h"

/* The UI module's logging, tagged and routed through EngineCore's seam, as GameCoreLog.h. */

#define UI_LOG(Message)       CORE_LOG     (::std::string("[UI] ") + (Message))
#define UI_LOG_WARN(Message)  CORE_LOG_WARN(::std::string("[UI] ") + (Message))
#define UI_LOG_ERROR(Message) CORE_LOG_ERROR(::std::string("[UI] ") + (Message))

#endif //UI_UILOG_H
