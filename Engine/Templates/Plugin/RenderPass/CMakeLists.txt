# @PLUGIN_NAME@ (@PLUGIN_NAME@.nmplugin): added by nimble_add_plugins() when the
# project enables it -- [Plugins] @PLUGIN_NAME@=true in Config/DefaultEngine.ini.
# Its shader, Content/Shaders/@PLUGIN_NAME@Tint.fsh, is cooked by NimbleCook
# (NimbleCook shaders -project <game>), never by this build.

nimble_add_plugin_module(@PLUGIN_NAME@
        SOURCES      Source/@PLUGIN_NAME@/Private/@PLUGIN_NAME@/@PLUGIN_NAME@Pass.cpp
                     Source/@PLUGIN_NAME@/Public/@PLUGIN_NAME@/@PLUGIN_NAME@.h
        PUBLIC_DEPS  GameCore::GameCore RenderCore::RenderCore)
@IF_EDITOR@
@IF_EDITOR@# Editor-only: built in Editor targets, left out of Client and Server builds.
@IF_EDITOR@nimble_add_plugin_module(@PLUGIN_NAME@Editor
@IF_EDITOR@        SOURCES      Source/@PLUGIN_NAME@Editor/Private/@PLUGIN_NAME@Editor/@PLUGIN_NAME@EditorPlugin.cpp
@IF_EDITOR@        PUBLIC_DEPS  @PLUGIN_NAME@)
@IF_TEST@
@IF_TEST@nimble_add_plugin_test(@PLUGIN_NAME@Tests
@IF_TEST@        SOURCES Tests/@PLUGIN_NAME@Tests.cpp
@IF_TEST@        LINK    @PLUGIN_NAME@)
