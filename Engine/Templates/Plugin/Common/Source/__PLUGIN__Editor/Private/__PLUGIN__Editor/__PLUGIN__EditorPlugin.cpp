#include "Public/Core/CoreLog.h"
#include "Public/NimbleEngine/Plugin.h"

/* @PLUGIN_NAME@Editor: built only in Editor targets ([Module_@PLUGIN_NAME@Editor]
 * Type=Editor), loaded after @PLUGIN_NAME@, never in a Client or Server build.
 * The place for editor-only code; the editor has no extension points for
 * plugins yet (Engine/Docs/Guides/PluginGuide.md, "What plugins can't do yet"). */

namespace
{
    class @PLUGIN_NAME@EditorPlugin : public NimbleEngine::IPlugin
    {
    public:
        bool OnStartup(NimbleEngine::PluginContext& InContext) override
        {
            CORE_LOG("[" + InContext.GetPluginName() + "] editor module started")
            return true;
        }
    };
}

NIMBLE_IMPLEMENT_PLUGIN(@PLUGIN_NAME@EditorPlugin)
