#ifndef NODEGRAPH_NODEGRAPHLOG_H
#define NODEGRAPH_NODEGRAPHLOG_H

#include <string>

#include "Public/Core/CoreLog.h"

/* NodeGraph's logging, tagged and routed through EngineCore's seam, as
 * GameCoreLog.h does for GameCore. */

#define NODEGRAPH_LOG(Message)       CORE_LOG     (::std::string("[NodeGraph] ") + (Message))
#define NODEGRAPH_LOG_WARN(Message)  CORE_LOG_WARN(::std::string("[NodeGraph] ") + (Message))
#define NODEGRAPH_LOG_ERROR(Message) CORE_LOG_ERROR(::std::string("[NodeGraph] ") + (Message))

#endif //NODEGRAPH_NODEGRAPHLOG_H
