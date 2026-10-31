# Spinner (Spinner.nmplugin): added by nimble_add_plugins() when the project
# enables it -- [Plugins] Spinner=true in Config/DefaultEngine.ini.

nimble_add_plugin_module(Spinner
        SOURCES      Source/Spinner/Private/Spinner/Spinner.cpp
                     Source/Spinner/Public/Spinner/SpinnerComponent.h
        PUBLIC_DEPS  GameCore::GameCore)

nimble_add_plugin_test(SpinnerTests
        SOURCES Tests/SpinnerTests.cpp
        LINK    Spinner)
