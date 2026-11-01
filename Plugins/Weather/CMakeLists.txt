# Weather (Weather.nmplugin): added by nimble_add_plugins() when a project
# enables it -- [Plugins] Weather=true in its Config/DefaultEngine.ini.

nimble_add_plugin_module(Weather
        SOURCES      Source/Weather/Private/Weather/Weather.cpp
                     Source/Weather/Private/Weather/WeatherComponent.cpp
                     Source/Weather/Private/Weather/WeatherPlugin.cpp
                     Source/Weather/Private/Weather/WeatherRain.cpp
                     Source/Weather/Public/Weather/Weather.h
                     Source/Weather/Public/Weather/WeatherComponent.h
        PUBLIC_DEPS  GameCore::GameCore RenderCore::RenderCore)

nimble_add_plugin_test(WeatherTests
        SOURCES Tests/WeatherTests.cpp
        LINK    Weather)
