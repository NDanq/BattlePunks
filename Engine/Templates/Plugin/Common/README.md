# @PLUGIN_NAME@

@PLUGIN_DESCRIPTION@

Made from the **@PLUGIN_STARTER@** plugin starter. How plugins work, and what
each file here is for: Engine/Docs/Guides/PluginGuide.md.

## Use it

1. Enable it in the project's `Config/DefaultEngine.ini` (or Project
   Settings > Plugins):

   ```ini
   [Plugins]
   @PLUGIN_NAME@=true
   ```

2. Build the game. The configure lists `plugin @PLUGIN_NAME@ 0.1.0`.
3. If it has shaders (`Content/Shaders`), cook them:
   `NimbleCook shaders -project <game>`.
4. Run its test: `ctest -L plugin`.

Content in `Content/` is read as `@PLUGIN_NAME@:/<path>`, and only so.
