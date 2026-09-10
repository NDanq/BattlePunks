#ifndef GAMEWINDOW_GAMEWINDOWLOG_H
#define GAMEWINDOW_GAMEWINDOWLOG_H

#include <string>

#include "Public/Core/CoreLog.h"

/*
 * GameWindow's logging, tagged and routed through EngineCore's seam.
 *
 * Exactly the arrangement GDILog.h uses: this module never chooses where text
 * goes, it only names itself. The host installs one LogHandler and sees Core,
 * GDI and GameWindow messages in the same sink.
 *
 * The tag is worth the concatenation. Window and input problems are reported by
 * users as "it froze" or "the mouse went weird", and the first useful question
 * is always whether the pump is the thing that stopped -- which is answerable
 * from a log only if the pump's messages are identifiable at a glance.
 */

#define GW_LOG(Message)       CORE_LOG     (::std::string("[GameWindow] ") + (Message))
#define GW_LOG_WARN(Message)  CORE_LOG_WARN(::std::string("[GameWindow] ") + (Message))
#define GW_LOG_ERROR(Message) CORE_LOG_ERROR(::std::string("[GameWindow] ") + (Message))

#endif //GAMEWINDOW_GAMEWINDOWLOG_H
