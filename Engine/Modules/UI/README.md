# UI

In-game interfaces, such as HUDs, menus and pause screens, built from XML
documents. A document lays out widgets, draws them with the engine's own
renderer, and runs native C++ functions, Lua functions or `EventBus` events when
a button is clicked. Its values can follow a data store the game fills.
`NimbleEditor` has a WYSIWYG designer for these documents: **UI Designer**, which
opens from the Content Browser.

A walkthrough, in three pages built on WorldsLab's Shooting Range (menu, HUD, settings;
C++, Lua and script graphs): `Engine/Docs/Guides/UI/`.

The MVP is a static UI. Animations and visual-state transitions are specified,
parsed and kept in every save and cook, but they do not play yet. See
[Reserved: animations and visual states](#reserved-animations-and-visual-states).

```
 Content/UI/Pause.nmui ──(editor: XML)──┐          ┌── GameSession::GetUI()  (one per game session)
                                        ├─► UIDocumentAsset ─► UIDocumentInstance ─► UIDrawList ─► Renderer::SubmitUIDrawList
 Saved/Cooked/Content/UI/... ─(game: binary)──┘     (shared)          (widgets, layout,     (pixels)        (RenderCore: drawn over the
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
  `Games/WorldsLab/Content/UI/ShootingRange/` is the example the guides use: a HUD, a pause
  menu and a settings page (`Docs/Guides/UI`).

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
- **In a Grid:** `Row`, `Column`, `RowSpan`, `ColumnSpan` (0 first), and
  `Margin`, `HAlign`, `VAlign` within its cells.

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
| `ScrollBox` | one | `ScrollOffset` (bindable, two-way), `ScrollStep`, `Orientation`, `ScrollBar` (`Auto`/`Always`/`Never`), `BarWidth`, `Track`, `Thumb`, `ThumbHovered`, `OnScrolled`; its `Size` is its desired size; see below |
| `Grid` | any | `RowSpacing`, `ColumnSpacing`, `Uniform`; children placed by the Grid slot |
| `WrapBox` | any | `ItemSpacing`, `LineSpacing`, `LineAlign`; children in lines that wrap at its width |
| `TextInput` | none | `Text` (bindable, two-way), `Placeholder`, `MaxLength`, `Mode` (`Text`/`Integer`/`Number`), `Password`, font and colours, `Background`, `FocusedBrush`, `Padding`, `OnChanged`, `OnCommit`; see below |
| `Include` | none | `Document` (a `.nmui`), `Prefix`; see "Documents inside documents" |
| `ListView` | none | `ItemDocument`, `Items`, `ItemHeight`, `Spacing`, `SelectedIndex` (bindable, two-way), the scroll properties, `SelectedBrush`, `OnSelect`, `OnActivate`; see below |

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

**ScrollBox: more than fits.** Its one child is laid out at full length and
seen through the box, clipped. The wheel over it scrolls it (an inner box
first, then, at its end, an outer one); the bar's thumb drags, and a click
beside the thumb moves a page; focus moving to something out of view (Tab, the
arrows, a pad) scrolls it in. `ScrollOffset` binds two-way, so a menu can
remember where a list was. In the designer, selecting a widget inside a scroll
box scrolls it into view.

**Grid and WrapBox.** A Grid's columns are as wide as their widest child and
its rows as tall as their tallest; a child spanning several shares its
surplus among them. `Uniform` makes every cell the largest's size and
stretches them over the grid (an inventory, a level select). A WrapBox puts
its children in lines that wrap at its width; its height comes from that
width, and is right on the first frame (layout runs again, at most twice,
when a pass asks).

**TextInput: typing.** One line. A click, Tab or the arrows focus it; while it
has focus the window's text input is on (`UIInputResult::bWantsTextInput`) and
it takes typed text and the editing keys -- Left and Right move the caret
rather than focus, Space types, Return commits. Shift extends a selection,
Ctrl+A/C/X/V select all, copy, cut and paste (the system clipboard, through
`UISystem::SetClipboard`; a password field never copies). The text scrolls
sideways to keep the caret in view. `Mode` Integer or Number accepts only a
number (`$value` is it); `MaxLength` counts characters. `OnChanged` runs on
each edit, `OnCommit` on Return or when focus leaves it, if it changed. Back
(Escape, the pad's East) leaves the field instead of running the document's
`OnBack`.

**ListView: a list from the data store.** `Items="Saves"` shows `Saves.Count`
items, each as an instance of `ItemDocument` whose bindings read
`Saves.<i>.<Field>` -- the row document writes `{Bind:Name}` and shows each
item's own. Only the rows in view are made, and they are reused as it scrolls:
a thousand items cost a dozen rows. A click selects (`SelectedIndex`, two-way;
`OnSelect`), Up and Down move the selection while it has focus, and Accept or
a second click activates (`OnActivate`, `$value` the index). Buttons in a row
still click, and run the row's own UI graph, whose `self.Prefix` is its
item's (`Saves.3.`). Fill a list from Lua with `UI.SetCount('Saves', n)` and
`UI.SetString('Saves.' .. i .. '.Name', ...)`, or from a graph with the List
Item Key node. A row's graph opens once per row, not per item: read the item
through its bindings or `self.Prefix`, not in On Document Open.

Sandbox's Settings menu has a Profile page of a TextInput and a ListView whose
rows are `Content/UI/SaveSlotRow.nmui` (see "In a game").

**Later widgets:** rich text, and rounded or SDF boxes.

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
| `Graph:Handler` | A UI Event in the document's UI graph, on that document's own instance of it: `self:Handler(value, sender)`. No arguments; see "UI graphs". |

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
| `Game.OpenLevel('Name')` | Loads a registered level by name (`GameSession::RequestLevel`); the swap closes every document and unpauses. |
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
  (`PE_Move`, a slider's drag) wherever it goes. A press focuses what takes
  focus.
- **The wheel** goes to the widget under the pointer, then up its parents, to
  the first that uses it (`UIWidget::OnWheel`: a scroll box).
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

  - A focused text field (`UIWidget::WantsTextInput`) takes the frame's typed
    text and editing keys first (`UIInputFrame::Text`, `Backspace`, `Left`,
    `bPaste`...): Left, Right and Accept edit it instead of navigating.

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
| `UI.GetNumber`, `UI.GetString`, `UI.GetBool` | Read data store values. |
| `UI.PlayAnimation(path, name)` | Reserved. Returns false. |

The `Game` library also gains `Game.SetPaused(bool)` and `Game.IsPaused()`.

The `Config` library (GameCore) reads and writes the engine's config vars by
`"Section.Key"`, which is what a settings menu changes: `Config.GetBool`,
`GetNumber`, `GetString`, the matching `Set*` (the var's callback fires, as an
`.ini` would), and `Config.Has`. They are typed: `GetBool` of a float var is a
script error.

## UI graphs

[UIGRAPHS.md](UIGRAPHS.md) is the how-to: making a graph, and exposing
C++ functions, variables, events and structs (and Lua) to it.

A document's logic can be a node graph instead of C++ or Lua: a
`*.ui.nmgraph` (the NodeGraph module's UI schema), named by the document's
`Graph` setting.

```xml
<UI ... Graph="Content/UI/PauseMenu.ui.nmgraph" OnBack="Graph:Document_OnBack">
  <Button Name="Resume" OnClick="Graph:Resume_OnClick"/>
  <CheckBox Name="Shadows" Checked="{Bind:LightLab.Shadows}" OnChanged="Graph:Shadows_OnChanged"/>
```

**The graph.** It has the script graph's nodes: flow, maths, variables, graph
functions, On Event and Emit, the World, and every script library (`UI`,
`Config`, `Game`, `Input`, a game's own). It leaves out the nodes that need an
entity. In their place it has:

| Node | Generated as |
|---|---|
| On Document Open | `OnOpen`, run once after the document opens (after that frame's input). |
| On Document Close | `OnClose`, run as it closes. |
| On Tick | `OnTick(dt)`, every frame while it is open, paused or not. Only a graph that has one costs anything per frame. |
| UI Event (`Handler`) | `<Handler>(Value, Sender)`, called by `Graph:<Handler>`. `Value` is `$value`, a bool for a check box (`ValueType`). `Sender` is the widget's name. |
| This Document | `self.Document`, its path: the first argument of the UI library's functions. |
| Close This Document; Set Widget Text / Visible / Enabled / Focus; Show Page | The UI library's calls on this document, with the widget chosen by name. |

On Event handlers listen while the document is open. A graph's variables are
per document, because each open document has its own instance (`self`). So
two open copies of a document keep separate state.

**At runtime** (`UISystem`), opening a document instances its graph's stored
Lua as a script object (`ScriptEngine::CreateObject`). It queues `OnOpen` and
runs `Graph:` actions on that object. On close it runs `OnClose` and releases
the object. A hot reload of the document, including a saved graph, rebuilds
the object. A system without a script engine logs `Graph:` actions instead of
running them. Games stage `Content/UI/*.ui.nmgraph`
for every target: the graph carries its Lua, so a Client build needs no cook.

**In the designer**, an action field's **+ > Graph handler** does the rest:

- It makes the UI graph if there is none (`<Document>.ui.nmgraph` beside
  it, set as `Graph`).
- It finds or adds the UI Event `<Widget>_<Event>` (`Document_OnBack` for the
  document's own events), opens the Graph Editor on it, and adds
  `Graph:<Handler>` to the field.
- A field's arrow button opens the handler it names. A field naming a handler
  the graph lacks is outlined in orange.
- The document's settings have `Graph`, with **Create** or **Open**.
- Saving the graph reloads its document in Play-in-Editor, and in the Preview.
- Handlers are named, so renaming a widget breaks nothing. When a widget with
  handlers named for it is renamed, the designer asks to rename them and the
  actions that call them too (`Play_OnClick` to `Start_OnClick`); deleting
  one says which handlers are left with nothing calling them.

**Documents inside documents.** An `Include` shows another document in its
rectangle, laid out at its host's scale and drawn, clicked and navigated as
part of it: Tab and the arrows go in and out, and there is one focus in the
whole tree. Its bindings read `Prefix` + key -- two Includes of one row with
`Prefix="Slot1."` and `"Slot2."` show two items -- and `{Bind:/Key}` is the key
as written. Its UI graph is its own instance: `self.Prefix` is its prefix, and
the graph's **Key** node prefixes a key for it. An included document reloads
when saved. One that would include itself, or go eight deep, is refused with
a warning. A ListView's rows are included documents too.

## In a game

`GameSession::GetUI()` returns the session's `UISystem`. Each frame it:

1. Routes the window's input through the UI, and gives the game what is left.
2. Runs queued actions.
3. Ticks the mode and the World, unless the game is paused.
4. Publishes the session's values to the data store.
5. Applies bindings, lays out and paints whatever changed, and submits the
   merged draw list to the default scene. It submits only when something changed.

**A player's settings** are config vars. The launcher reads
`<AppData>/NimbleEngine/<[Application] UserDataName, else the game module>/User.ini`
over `Config/DefaultEngine.ini`; `Config.Save()` (Lua and graphs) writes what
differs from the game's own values there. LightLab's pause menu and Sandbox's
Settings save on close, from their UI graphs' On Document Close.

`GameSession::SetPaused(true)` stops the World and the mode's `OnTick`, but the UI
keeps running, so a pause menu can resume the game. Documents close when their
level unloads, unless they were opened as persistent.

LightLab's pause menu is the complete example. Its logic is a graph,
`Content/UI/PauseMenu.ui.nmgraph`; the game's C++ only opens it.

- `LightLabGame.cpp` opens it on `Pause` (Escape, or the gamepad's Start) and
  pauses.
- Its pages are a Switcher bound to `LightLab.Page`. The Settings and Back
  buttons set the value (`Native:UI.SetValue('LightLab.Page', 1)`).
- Its graph's On Document Open fills `LightLab.Shadows`, `LightLab.BakedLighting`,
  `LightLab.LookSensitivity` and `LightLab.FlySpeed` from their config vars
  (`Config.Get*`). The Settings page's check boxes and sliders bind to these
  two-way.
- Each `OnChanged` is `Graph:<Setting>_OnChanged`, which sets the var
  (`Config.Set*`): the same var the T/F/L keys, an `.ini` and the console set.
- Resume (`Graph:Resume_OnClick`) and `OnBack` (`Graph:Document_OnBack`: from
  Settings to the first page, from there resume) call the graph function
  Resume. It closes the menu, unpauses, and takes the mouse back.
- Quit runs `Native:Game.Quit`. Load Baked Lighting World runs
  `Native:Game.OpenLevel('BakedLighting')`.

## Files and cooking

`.nmui` is either XML (authored) or binary (cooked). The first bytes tell them
apart. A game build has no XML parser, so it reads only the cooked form.

| Command (editor Console) | What it does |
|---|---|
| `cook ui` | Cooks every document to `<project>/Saved/Cooked/Content/UI/...`. |
| `cook <doc.nmui> [out]` | Cooks one document. |

- An Editor build stages `Content/UI/`. A Client or Server build stages
  `Saved/Cooked/Content/UI/`, as prefabs are staged. The game's CMakeLists warns when
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
    native actions, and a handler in the document's UI graph (see "UI
    graphs").
  - A bad value turns the field red and is not written.
  - With nothing selected, the panel shows the document's settings and its
    `<Styles>` as XML.

**Preview**
- The document takes the mouse, keys and typing as a game would, and runs: its
  actions and its UI graph run in a scratch World and script engine of the
  Preview's own (`UIPreviewRuntime`), from On Document Open. Config vars the
  graph changes are put back when the Preview stops; `Config.Save` does
  nothing in the editor, and the `Game` library answers defaults. A game
  module's own config vars are not declared while the editor has not loaded
  it: `Config.Get*` of one warns and reads a default. Script errors show in
  the status bar.

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
| `UITests` (`UI.Core`, editor builds) | Registry, values, actions, layout across viewports and every scale mode, paint and batching, text, styles, round-trips of XML and cooked files (byte-identical, deterministic), structure editing, input routing, check boxes and sliders (clicks, drags, keys, two-way binding), scroll boxes, grids and wrap boxes, text input, Include and ListView (prefixes, focus across, row reuse), the Sandbox Profile page end to end, native/Lua/Event/Graph actions, UI graphs (per-document instances, lifecycle, hot reload, LightLab's menu end to end), the Lua library, hot reload, and cost (zero allocations per unchanged frame). |
| `UI_TESTS_GPU=Vulkan UI_TESTS_DUMP=<dir> ./UITests`, from a runtime directory | Draws the shipped documents on a GPU and checks pixels. |
| RenderCore smoke test | `UIDrawList` batching, clipping, nine-slice and packing. `NIMBLE_SMOKE_GPU=Vulkan` checks orientation, scissor, blending, the retained list, and clearing. |
| GDI smoke test | Scissor rows and clipping, and the dynamic mesh's append and rewind. |
| `NimbleEngine.GameFrameworkTests` | The session's UI: clicks do not reach the game, pause and resume, OnBack, HUD pass-through, a self-closing button, published values. |
| `NimbleEngine.InputTests` | `InputState::Consume*` and `WasInputActionPressed`. |
| `GameCore.Smoke` | `ScriptEngine::CallFunction`, script objects (`CreateObject`), the `Config` library and `Config.Save`. |
| Core smoke test (`CORE_BUILD_SMOKETEST`) | The user settings layer: the baseline, a save and reload, lines kept for vars not declared. |
| `NodeGraph.Tests` | The UI schema: its nodes, golden `UIMenu.lua`, the Lua run as a script object, refused handler names. |
| `Editor.UIDesigner` | The designer's document (undo, merging, structure, save and reload), its details panel (a Grid slot too), Graph handler (making the graph, finding a handler again, flagging a missing one), renames and deletes followed in the graph, and the Preview's runtime (graphs run, config put back). |
