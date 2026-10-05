# UI

In-game interfaces, such as HUDs, menus and pause screens, built from XML
documents. A document lays out widgets, draws them with the engine's own
renderer, and runs native C++ functions, Lua functions or `EventBus` events when
a button is clicked. Its values can follow a data store the game fills.
`NimbleEditor` has a WYSIWYG designer for these documents: **UI Designer**, which
opens from the Content Browser.

The MVP is a static UI. Animations and visual-state transitions are specified,
parsed and kept in every save and cook, but they do not play yet. See
[Reserved: animations and visual states](#reserved-animations-and-visual-states).

```
 Content/UI/Pause.nmui ──(editor: XML)──┐          ┌── GameSession::GetUI()  (one per game session)
                                        ├─► UIDocumentAsset ─► UIDocumentInstance ─► UIDrawList ─► Renderer::SubmitUIDrawList
 Cooked/Content/UI/... ─(game: binary)──┘     (shared)          (widgets, layout,     (pixels)        (RenderCore: drawn over the
                                                                 input, bindings)                    scene, after post process)
```

## Pieces

| Piece | Where | Role |
|---|---|---|
| Widgets | `UIWidget.h`, `UIWidgets.h` | The element classes. Behaviour is virtual: `MeasureContent`, `ArrangeChildren`, `Paint`, `OnPointer`. |
| Registry | `UIProperty.h` | Every widget type, its XML name and its properties (`UI_DEFINE_WIDGET`). XML, cooking, bindings, Lua and the editor all read it. |
| Document | `UIDocument.h` | `UIDocumentAsset` is what a file holds, as plain data. `UIDocumentInstance` is its widgets, laid out and painted. `UIAssetCache` holds loaded documents by path. |
| Files | `UIFile.h` | XML in editor builds, and the cooked binary in every build. |
| System | `UISystem.h` | The open documents, the data store, the fonts and the action queue for one viewport. |
| Actions | `UIAction.h` | `Native:`, `Lua:` and `Event:` actions, and the native registry (`UI_DEFINE_ACTIONS`). |
| Bindings | `UIBinding.h` | The data store and `{Bind:Key}`. |
| Fonts | `UIFont.h` | TrueType fonts baked to atlases with stb_truetype (`External/stb`). |
| Lua | `UIScriptLibrary.cpp` | The `UI` library. |
| Draw list | RenderCore's `UIDrawList.h` | What a document paints. RenderCore draws it (RENDERPIPELINE.md, "UI"). |

The module depends on GameCore and RenderCore. It knows nothing about any input
system or window: the host builds a `UIInputFrame` and a viewport size and passes
them in. NimbleEngine's `GameSession` is the game's host, and the designer is the
editor's.

## Documents

A `.nmui` holds one `<UI>` root element. Its attributes are the document's settings:

| Setting | Default | Meaning |
|---|---|---|
| `ReferenceSize` | `1920,1080` | The resolution the document is authored at. Layout units are pixels at this size. |
| `ScaleMode` | `FitHeight` | How the reference size meets the screen: `FitHeight`, `FitWidth`, `Fit` (all visible), `Fill` (covers), or `None` (layout units are pixels). |
| `InputMode` | `GameAndUI` | `UIOnly` (a menu) takes every input and shows the cursor. `GameAndUI` (a HUD) takes the mouse only over a widget. `GameOnly` takes none. |
| `Layer` | `0` | Higher layers draw over lower ones and take input first. Documents on the same layer stack in the order they were opened. |
| `Script` | none | A `.lua` file run once, the first time the document opens. Its handlers go here. |
| `DefaultFont` | the engine's | A `.ttf` used by Text widgets that name no font. |
| `OnOpen`, `OnClose`, `OnBack` | none | Actions run on open, on close, and on Back (Escape or the gamepad's East button) while the document has input. |

Inside the root element come `<Styles>`, exactly one root widget (usually a
`Canvas`), and the reserved `<Animations>` and `<VisualStates>`:

```xml
<UI Format="1" ReferenceSize="1920,1080" ScaleMode="FitHeight" InputMode="UIOnly" Script="Content/UI/MainMenu.lua">
  <Styles>
    <Style Name="MenuButton" Type="Button" Padding="32,14" Margin="0,8" Hovered.Tint="#2F6FD9F2"/>
  </Styles>
  <Canvas Name="Root">
    <VerticalBox Anchors="0.5,0.5,0.5,0.5" Pivot="0.5,0.5" AutoSize="true">
      <Button Name="Play" Class="MenuButton" InitialFocus="true" OnClick="Lua:MainMenu.Play">
        <Text Text="Play" FontSize="34" HAlign="Center"/>
      </Button>
      <Button Name="Quit" Class="MenuButton" OnClick="Native:Game.Quit">
        <Text Text="Quit" FontSize="34" HAlign="Center"/>
      </Button>
    </VerticalBox>
    <Text Position="32,32" Text="Score: {Bind:Player0.Score}"/>
  </Canvas>
</UI>
```

- An element's name is its widget type, its attributes are its properties, and
  its child elements are its children.
- Struct properties use dotted names: `Normal.Tint`, `Background.Texture`.
- The formats are:
  - colours: `#RRGGBBAA`, `#RRGGBB` or `#RGB`
  - `Vec2`: `x,y`
  - `Margin`: `l,t,r,b`, or `h,v`, or a single value
  - enums: by name
- `Class="A B"` applies styles A then B under the widget's own values. A style
  with a `Type` applies only to that type and its subclasses.
- Unknown properties are kept and warned about when instanced, so a typo shows
  in the log and a newer file still opens.
- Shipped examples: `Games/Sandbox/Content/UI/` (`HUD.nmui`, `MainMenu.nmui` with
  `MainMenu.lua`, and `Settings.nmui`, tabbed pages) and `Games/LightLab/Content/UI/PauseMenu.nmui`.

## Widgets

**Every widget:**

| Property | Meaning |
|---|---|
| `Name` | Used by actions (`$self`), Lua and navigation to find it. |
| `Class` | The styles it uses. |
| `Visibility` | `Visible`, `Hidden` (keeps its space), `Collapsed` (takes no space), `HitTestInvisible` (drawn, but neither it nor its children take hits), or `SelfHitTestInvisible` (its children still take hits). |
| `IsEnabled` | Disabled widgets take no input. |
| `Opacity` | Multiplied down the tree. |
| `RenderTranslate`, `RenderScale` | Change where it is drawn, not where it is laid out. Scale is about its centre. Hit tests follow the drawn position. |
| `ClipChildren` | Children are cut off at its edges (a GPU scissor). |
| `Focusable`, `InitialFocus`, `NavUp`/`NavDown`/`NavLeft`/`NavRight` | Gamepad and keyboard focus. |

**Slot properties: where a widget sits in its parent.** The parent's type decides which apply.

- **In a Canvas:**
  - `Anchors` (min X, min Y, max X, max Y, each 0-1 of the parent), `Position`,
    `Size`, `Pivot`, `AutoSize`, `Margin` and `ZOrder`.
  - On an axis where the two anchors are equal, the widget is `Size` wide, or
    its desired size with `AutoSize`. It sits at the anchor plus `Position`,
    less `Pivot` times its size.
  - On an axis where the anchors differ, it stretches between them, inset by
    `Margin`.
  - `ZOrder` sorts siblings for drawing and hit testing.
- **In a box, an Overlay, a Border or a Button:**
  - `Margin` is space around it.
  - `HAlign` and `VAlign` are `Fill`, `Left`/`Top`, `Center` or `Right`/`Bottom`.
  - Along a box's axis, `Fill` is a weight. With `Fill` 0 the child takes its
    desired size; otherwise it takes a share of the space left over.

**The MVP widgets:**

| Widget | Children | Its own properties |
|---|---|---|
| `Canvas` | any | none; children are placed by anchors |
| `Border` | one | `Background` (brush), `Padding` |
| `Image` | none | `Brush`, `DesiredSize` (0,0 uses the texture's size), `Stretch` (`Fill`, `None`, `Uniform`, `UniformToFill`) |
| `Text` | none | `Text` (bindable, can interpolate), `Font`, `FontSize` (an em, in layout units), `Color`, `Justify`, `Wrap`, `LineHeight`, `ShadowOffset`, `ShadowColor` |
| `Button` | one | `Normal`, `Hovered`, `Pressed`, `Disabled` (brushes; focus shows Hovered), `Padding`, `OnClick`, `OnPressed`, `OnReleased`, `OnHovered`, `OnUnhovered` |
| `HorizontalBox`, `VerticalBox` | any | `Spacing` |
| `Overlay` | any | none; children stack over each other |
| `Switcher` | any | `ActiveIndex` (bindable): its children are pages, one shown at a time; see below |
| `ProgressBar` | none | `Value` (0-1, bindable), `Background`, `FillBrush`, `FillDirection` |
| `CheckBox` | one (its label) | `Checked` (bindable, two-way), `BoxSize`, `Spacing`, `Box`, `BoxHovered`, `Check` (brushes), `Padding`, `OnChanged`; see below |
| `Slider` | none | `Value` (bindable, two-way), `Min`, `Max`, `Step`, `TrackHeight`, `ThumbSize`, `Track`, `FillBrush`, `Thumb`, `ThumbHovered` (brushes), `OnChanged`; its `Size` is its desired size; see below |
| `Spacer` | none | its `Size` is its desired size |

**Brush** (`Background`, `Brush`, a button's four):

| Field | Meaning |
|---|---|
| `Texture` | A `.nmtexture`. Empty means a flat colour. |
| `Tint` | Colour multiplied with the texture. |
| `DrawAs` | `None`, `Box`, `Image` or `NineSlice`. |
| `Margin` | The nine-slice corners, in texture pixels. |
| `UVRect` | x, y, w, h of the texture to use, for a sprite-sheet cell. |
| `OutlineColor`, `OutlineThickness` | An outline drawn inside the rectangle. |

**Switcher: pages.** A settings menu's Input / Graphics / Gameplay. Every page is
laid out over the others in the Switcher's rectangle, and it is the size of
its largest page, so switching moves nothing around it. Only the page at
`ActiveIndex` (its children in order, 0 first; clamped) is drawn, hit or
focused; focus on a page that goes away moves to the first widget still
shown. Switch it with:

- a binding: `ActiveIndex="{Bind:Settings.Page}"` and tabs that set the value,
  `OnClick="Native:UI.SetValue('Settings.Page', 1)"`. Several Switchers can
  follow one value -- `Games/Sandbox/Content/UI/Settings.nmui` switches its
  pages and the underline under its tabs together this way;
- `Native:UI.ShowPage('Pages', 'Graphics')` (by a page's `Name`, or its number);
- Lua: `UI.ShowPage(path, 'Pages', 'Graphics')`; C++: `UISwitcher::ShowPage`.

In the designer, selecting a widget on another page shows that page (in the
canvas only; the document keeps its own `ActiveIndex`).

**CheckBox and Slider: settings.** Both take focus, and bind two-way:
`Checked="{Bind:Settings.Shadows}"` shows the store's value, and when the
player changes it the new value is written back to the store, so the game,
and anything else bound to the key (a label showing a slider's value, say),
sees it. `OnChanged` runs after each change, and `$value` is the new value:
1 or 0 for a check box, the value for a slider.

- A **CheckBox** draws its box at the left with its one child (a label) beside
  it, `Spacing` apart. A click on the box or the label, or `Accept` while it
  has focus, toggles it.
- A **Slider** is horizontal. A press jumps the thumb to the pointer, and a
  drag carries it, even outside the slider, until release. While it has
  focus, Left and Right step it by `Step`, or by a twentieth of the range
  when `Step` is 0, and stay on it at either end. Up and Down still move
  focus. `Value` is clamped to `Min`..`Max` and snapped to `Min + n * Step`.

LightLab's pause menu has a Settings page of both (see "In a game").

**Later widgets:** ScrollBox, TextInput (it needs the
window's text input, which is already plumbed as `GameWindow::SetTextInputActive`
and `OnText`), Grid and WrapBox, ListView, an `Include` element
for documents inside documents, rich text, and rounded or SDF boxes.

**A game's own widget** is a class plus a registration in its module. It shows
in the designer's palette while that module is loaded:

```cpp
class RadarWidget : public UI::UIWidget
{
public:
    float Range = 100.0f;
    void Paint(UI::UIPaintContext& InContext, const UI::UIRect& InRect) const override;
    bool TakesHits() const override { return true; }
};
UI_DEFINE_WIDGET(RadarWidget, UIWidget, "Radar")
{
    Type.Palette("Game");
    Type.Field<&RadarWidget::Range>("Range", UI::UPF_Bindable);
}
```

## Layout and scale

- Documents are authored in layout units: pixels at `ReferenceSize`. The
  document scales them to the viewport by `ScaleMode`. For example, a 1080p
  layout at 1440p with `FitHeight` is drawn at 4/3 scale. On an ultrawide
  screen the same layout gets a wider layout space, and anything anchored or
  stretched follows it.
- Text is baked at its on-screen pixel size (`FontSize` times scale), so it
  stays sharp at every resolution.
- Rectangles snap to whole pixels.
- Layout has two passes, measure and then arrange, and runs only when something
  moved: a property flagged `UPF_Layout`, or the viewport size. Anything else
  only repaints.
- Wrapped text measures at the width it was last arranged to, so a size change
  takes one extra layout pass.

## Actions

An Action property is text:

```
OnClick="Native:Game.Quit"
OnClick="Lua:MainMenu.Play"
OnClick="Lua:Menu.Select('Arena', 2); Event:Menu.Changed($self)"
```

| Kind | Runs |
|---|---|
| `Native:Group.Name` | A C++ function registered with `UI_DEFINE_ACTIONS(Group) { Actions.Add("Name", &Fn); }`. A game module's actions unregister when it unloads. `UIActionContext` provides the system, the document, the sender, the arguments and `HostData` (the `GameSession*`). |
| `Lua:Path.To.Fn` | A Lua function found by its global path (`ScriptEngine::CallFunction`). Script libraries are globals, so `Lua:Game.SetPaused(false)` works too. |
| `Event:Name` | An `EventBus` event on the session's World. Lua listens with `Events.On`. |

- Arguments can be numbers, `'strings'`, `true` and `false`, plus two tokens:
  `$self` is the widget's name, and `$value` is its value (a slider's
  value, a check box's 1 or 0, otherwise 0). There can be at most four.
- Actions are parsed when the document loads. Bad text is refused in the editor
  and warned about at runtime.
- Actions are **queued** and run after the input that fired them, before the game
  ticks. An action that closes its own document is therefore safe.

**Built-in native actions** (`Game.*` in NimbleEngine's `GameUI.cpp`, which needs the game session; `UI.*` in the UI module's `UIBuiltinActions.cpp`):

| Action | What it does |
|---|---|
| `Game.Quit` | Ends the game. |
| `Game.Pause`, `Game.Resume`, `Game.TogglePause` | `GameSession::SetPaused`. |
| `UI.Open('path')`, `UI.Close('path')`, `UI.Toggle('path')` | Open, close or toggle a document. With no path, `UI.Close` closes its own document. |
| `UI.ShowPage('Switcher', page)` | Shows a Switcher's page, by name or number, in the same document. |
| `UI.SetValue('Key', value)` | Sets a value in the data store. |

## Data binding

The game pushes values into the data store: `UISystem::SetValue("Player0.Score",
UIValue::Number(10))`, or in Lua `UI.SetNumber("Player0.Score", 10)`. A
bindable property (`UPF_Bindable`) can take its value from the store:

- `Text="{Bind:Player0.Score}"` binds the whole value. Any bindable property can do this.
- `Text="Score: {Bind:Player0.Score}"` interpolates into text. Only String properties can do this.

How bindings update:

- A document re-applies its bindings when the store's version changes.
- It sets a property only if the value differs, so an unchanged HUD costs one
  comparison per binding.
- A key that has not been set leaves a whole-value property alone and
  interpolates as empty text.

`GameSession` publishes these values every frame:

- `Player<N>.Score` and `Player<N>.Lives`, from `PlayerStateComponent`
- `Game.Phase`

**Two-way.** A CheckBox's `Checked` and a Slider's `Value` write back: when the
player changes one, a whole-value binding on it sets its key in the store
(`UIDocumentInstance::SetInputValue`, for a widget of your own too).

Reserved for later: pull bindings (Lua expressions, component fields) and
format strings.

## Input

The host builds a `UIInputFrame` and calls `UISystem::ProcessInput`.
`UIInputResult` reports what the UI took.

- **Hit testing:**
  - Documents are tested top layer first. Within a document, widgets are
    tested in reverse draw order (topmost first).
  - Each test respects `Visibility`, clipping and render transforms.
  - Only widgets that draw something or take input are hit (`TakesHits`).
    Layout containers are not, so a full-screen HUD root lets clicks through.
- **Clicks:** a click is a press and a release on the same widget. While the
  button is held, the pressed widget keeps the pointer, and hears it move
  (`PE_Move`, a slider's drag) wherever it goes.
- **Navigation:**
  - Applies in UIOnly documents, and in any document that has focus.
  - The focused widget sees a direction first (`UIWidget::OnNavigate`): a
    slider uses Left and Right itself.
  - Otherwise directions move focus to the widget named by its `Nav*` property,
    or to the nearest focusable widget in that direction.
  - `Accept` clicks the focused widget. `Back` runs `OnBack`. `Tab` and
    `Shift+Tab` cycle through focusable widgets.
  - Keys come from the engine's `UI` input map (NimbleEngine's `GameUI.cpp`),
    which can be rebound like any other map:

    | Action | Keyboard | Gamepad |
    |---|---|---|
    | Up, Down, Left, Right | arrow keys | D-pad |
    | Accept | Return, Space | South |
    | Back | Escape | East |
    | Next | Tab | none |

- **Consumption:** `GameSession` removes what the UI took from the input the
  game reads. See INPUT.md, "The UI". A click on a button never reaches the
  game behind it.

## Lua

The `UI` library acts on the session's system:

| Function | What it does |
|---|---|
| `UI.Open(path[, persistent])` | Opens a document. Returns its handle, or 0 if it failed. |
| `UI.Close(path)` | Closes a document. |
| `UI.IsOpen(path)` | Whether a document is open. |
| `UI.SetText(path, widget, text)` | Sets a Text widget's text. |
| `UI.SetVisible(path, widget, bool)` | Shows the widget, or collapses it. |
| `UI.SetEnabled(path, widget, bool)` | Enables or disables the widget. |
| `UI.SetProperty(path, widget, property, text)` | Sets any property, written as in XML. |
| `UI.SetFocus(path, widget)` | Moves focus to the widget. |
| `UI.ShowPage(path, switcher, page)` | Shows a Switcher's page, by name or number. |
| `UI.SetNumber`, `UI.SetString`, `UI.SetBool` | Set data store values. |
| `UI.GetNumber`, `UI.GetString` | Read data store values. |
| `UI.PlayAnimation(path, name)` | Reserved. Returns false. |

The `Game` library also gains `Game.SetPaused(bool)` and `Game.IsPaused()`.

## In a game

`GameSession::GetUI()` returns the session's `UISystem`. Each frame it:

1. Routes the window's input through the UI, and gives the game what is left.
2. Runs queued actions.
3. Ticks the mode and the World, unless the game is paused.
4. Publishes the session's values to the data store.
5. Applies bindings, lays out and paints whatever changed, and submits the
   merged draw list to the default scene. It submits only when something changed.

`GameSession::SetPaused(true)` stops the World and the mode's `OnTick`, but the UI
keeps running, so a pause menu can resume the game. Documents close when their
level unloads, unless they were opened as persistent.

LightLab's pause menu is the complete example:

- `LightLabGame.cpp` opens it on `Pause` (Escape, or the gamepad's Start).
- Its pages are a Switcher. The Settings button runs
  `Native:UI.ShowPage('Pages', 'Settings')`, and `OnBack` runs
  `Native:LightLab.Back`, which returns to the first page, or resumes from it.
- Resume runs `Native:LightLab.Resume`, defined with `UI_DEFINE_ACTIONS(LightLab)`.
  Quit runs `Native:Game.Quit`.
- The Settings page has check boxes for shadows and baked lighting, and sliders
  for look sensitivity and fly speed. Each is bound to a key
  (`{Bind:LightLab.Shadows}`) that the game fills from its config var as the
  menu opens. Each `OnChanged` runs `Native:LightLab.SetShadows($value)` (and
  so on), which sets the var: the one the T/F/L keys, an `.ini` and the console
  also set.

## Files and cooking

`.nmui` is either XML (authored) or binary (cooked). The first bytes tell them
apart. A game build has no XML parser, so it reads only the cooked form.

| Command (editor Console) | What it does |
|---|---|
| `cook ui` | Cooks every document to `<project>/Cooked/Content/UI/...`. |
| `cook <doc.nmui> [out]` | Cooks one document. |

- An Editor build stages `Content/UI/`. A Client or Server build stages
  `Cooked/Content/UI/`, as prefabs are staged. The game's CMakeLists warns when
  nothing is cooked.
- The cooked form has styles applied, keeps every property as text so an
  unknown property survives, and keeps the reserved sections as written. It is
  deterministic: the same document always cooks to the same bytes.
- XML is written deterministically too: two-space indent, LF line endings, and
  attributes in the order they were read.

**Hot reload:** saving in the designer stages the file and calls
`UIAssetCache::Invalidate`. Open instances rebuild on the next tick, keeping
focus by widget name. This works in Play-in-Editor.

## Editor: UI Designer

Open a `.nmui` from the Content Browser, or right-click and choose
**New UI...**. Each document gets a tab:

**Canvas**
- Drawn by the game's own renderer: the same fonts, layout and batching. The
  selection handles are drawn by the same renderer, over the document.
- The toolbar's resolution presets lay out for 720p, 1080p, 1440p, 4K,
  ultrawide, portrait, 16:10, or a custom size.
- The wheel zooms about the cursor, the middle button pans, F fits the frame,
  and 100% shows one design pixel per screen pixel.

**Editing**
- Click selects.
- Dragging a Canvas child moves it (`Position`, or `Margin` on a stretched
  axis). Its eight handles resize it. Moves snap to a 4-unit grid while Snap is on.
- Dragging a box's child reorders it.
- Arrow keys nudge (Shift for 10).
- Delete, Ctrl+D (duplicate), Ctrl+C and Ctrl+V (copy and paste as XML).
- Each gesture is one undo step.

**Panels**
- **Palette:** every registered type, a game's included. Drag one onto the
  canvas or the hierarchy, or double-click it to add it to the selection.
- **Hierarchy:** drag a row to reparent or reorder it.
- **Details:** built from the registry.
  - Slot properties appear as the parent reads them.
  - A label in bold means the value is set on this widget, and has a reset
    button. Italic means the value comes from a style. Plain means it is the
    type's default.
  - Bindable properties take `{Bind:...}`. Action fields offer the registered
    native actions.
  - A bad value turns the field red and is not written.
  - With nothing selected, the panel shows the document's settings and its
    `<Styles>` as XML.

**Preview**
- The document takes the mouse, arrow keys, Return, Escape and Tab as a game
  would. Actions are logged to the status bar, not run.

**Save**
- Ctrl+S writes the XML, stages it, and hot-reloads Play-in-Editor.

**Console**
- `ui <path | new | close | select <name> | resolution <w>x<h> | preview on|off | screenshot <file.png>>`
  drives the designer from scripts. For example,
  `NimbleEditor --command "after 2000 ui Content/UI/HUD.nmui" --command "after 3000 ui screenshot hud.png"`.

## Performance

UI trees are small, so the costs that matter are allocations, strings and draw
calls. Measured in `UITests` (Debug, 500 widgets):

| Case | Time |
|---|---|
| Unchanged frame | about 0.2 µs, with **zero allocations** (a gating test) |
| Full relayout | under 1 ms |
| Repaint | about 0.3 ms |

How it gets there:

- **Memory:** a document makes all its widgets in one block, in document order.
  Each widget's rectangles, desired size and links sit in parallel arrays, so
  layout, paint and hit tests walk memory in order.
- **Dirty flags:** separate layout and paint flags. A clean document skips both.
- **Retained draw lists:**
  - Each document keeps its last list. The system merges them and submits only
    when one changed.
  - RenderCore keeps a scene's list and its dynamic mesh between frames, so a
    static HUD costs no GPU upload.
  - The designer composes and submits only on a change, in the same way.
- **Text:** shaped once per change, then copied.
- **Hit tests:** the last result is reused while the pointer and the painted
  rectangles stay where they were.
- **Strings:** property, action and binding keys are hashed or parsed when the
  document loads, not per frame.
- **Draw calls:**
  - Batches merge on matching texture and clip.
  - A font atlas keeps a white texel, so flat fills after text join the text's draw.
  - A document is typically 1-3 draw calls.

## Reserved: animations and visual states

```xml
<Animations>
  <Animation Name="Intro" Duration="0.6">
    <Track Target="Title" Property="Opacity" Easing="CubicOut">
      <Key Time="0" Value="0"/><Key Time="0.6" Value="1"/>
    </Track>
  </Animation>
</Animations>
<VisualStates Target="Play">
  <State Name="Hovered"><Set Property="RenderScale" Value="1.05,1.05"/></State>
  <Transition From="*" To="Hovered" Duration="0.1" Easing="QuadOut"/>
</VisualStates>
```

Both sections are parsed and kept, as written, through load, save and cook.
`UIAnimation.h` (`UIAnimationAsset`, `EUIEasing` with working `Ease` curves, and
`UIAnimator`) and `UIVisualState.h` define the shapes.

What is not implemented yet:

- `UIAnimator::Play` refuses.
- `UIDocumentInstance::PlayAnimation` returns false and warns once.
- `UIWidget::GoToState` records the state's name only.

Properties flagged `UPF_Animatable` (Float, Color, Vec2: `Opacity`, tints,
`RenderTranslate`, `RenderScale`, `Position`, `Value`) are what tracks will
drive. Render transforms already draw and hit test, so animating them needs no
layout. Rotation will need stencil clipping, because scissors are axis-aligned.

## Fonts

- The engine's font is Noto Sans (`Engine/Content/Fonts`, SIL OFL 1.1; `OFL.txt`
  ships beside it).
- A font is baked per pixel size on first use: ASCII and Latin-1, with 2x
  horizontal oversampling up to 48 px, and kerning.
- Each atlas is sent to the renderer as an image (`Renderer::SetUIImage`,
  `ui:font:<hash>@<px>`).
- Codepoints outside Latin-1 draw as `?`.
- **Later:** cooked atlases, SDF text for outlines and scaling, and glyphs added
  on demand.

## Tests

| Test | What it checks |
|---|---|
| `UITests` (`UI.Core`, editor builds) | Registry, values, actions, layout across viewports and every scale mode, paint and batching, text, styles, round-trips of XML and cooked files (byte-identical, deterministic), structure editing, input routing, check boxes and sliders (clicks, drags, keys, two-way binding), native/Lua/Event actions, the Lua library, hot reload, and cost (zero allocations per unchanged frame). |
| `UI_TESTS_GPU=Vulkan UI_TESTS_DUMP=<dir> ./UITests`, from a runtime directory | Draws the shipped documents on a GPU and checks pixels. |
| RenderCore smoke test | `UIDrawList` batching, clipping, nine-slice and packing. `NIMBLE_SMOKE_GPU=Vulkan` checks orientation, scissor, blending, the retained list, and clearing. |
| GDI smoke test | Scissor flipping and clipping, and the dynamic mesh's append and rewind. |
| `NimbleEngine.GameFrameworkTests` | The session's UI: clicks do not reach the game, pause and resume, OnBack, HUD pass-through, a self-closing button, published values. |
| `NimbleEngine.InputTests` | `InputState::Consume*` and `WasInputActionPressed`. |
| `GameCore.Smoke` | `ScriptEngine::CallFunction`. |
| `Editor.UIDesigner` | The designer's document (undo, merging, structure, save and reload) and its details panel. |
