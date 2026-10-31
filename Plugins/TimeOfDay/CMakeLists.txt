# TimeOfDay (TimeOfDay.nmplugin): added by nimble_add_plugins() when a
# project enables it -- [Plugins] TimeOfDay=true in its Config/DefaultEngine.ini.

nimble_add_plugin_module(TimeOfDay
        SOURCES      Source/TimeOfDay/Private/TimeOfDay/TimeOfDay.cpp
                     Source/TimeOfDay/Private/TimeOfDay/TimeOfDayComponent.cpp
                     Source/TimeOfDay/Private/TimeOfDay/TimeOfDayPlugin.cpp
                     Source/TimeOfDay/Public/TimeOfDay/TimeOfDay.h
                     Source/TimeOfDay/Public/TimeOfDay/TimeOfDayComponent.h
        PUBLIC_DEPS  GameCore::GameCore RenderCore::RenderCore)

nimble_add_plugin_test(TimeOfDayTests
        SOURCES Tests/TimeOfDayTests.cpp
        LINK    TimeOfDay)
