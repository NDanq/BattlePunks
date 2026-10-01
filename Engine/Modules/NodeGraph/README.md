# NodeGraph

Node graphs that **generate source code**. A graph is edited in NimbleEditor
and compiled to an ordinary source file that is committed next to it. The
runtime never loads a graph.

| Graph | File | Generates | Consumed by |
|---|---|---|---|
| Material | `Glow.material.nmgraph` | `Glow.fsh` (the engine's macro GLSL) and `Glow.material` (parameter defaults) | `PrimitiveComponent::Material = "Glow"`, once cooked (below) |
| Script | `Mover.script.nmgraph` | `Mover.lua`, a class table | `Entity:SetScript("Scripts/Mover.lua")`, like any script |

Written for this repository. **Editor targets only**, and Qt-free: the editor
draws graphs (`Modules/Editor/Source/Private/Editor/Graph/`), but loading,
saving and compiling happen here. That lets Build > Cook Content and the
tests regenerate every graph without opening a window.

## Position in the tree

```
NimbleEditor --> NodeGraph --> GameCore --> EngineCore
                            \-> PugiXml (PRIVATE, GameCore's vendored copy)
```

**STATIC.** Unlike GameCore it owns no process-wide registry that two images
could each get a copy of. The schemas are built on first use, from GameCore's
registries, by whichever executable links this: the editor or the test.

## Pieces

| File | Role |
|---|---|
| `Graph.h` | The data: nodes (GUID, type name, position, literals, properties), links (`<guid>.<pin>` to `<guid>.<pin>`), variables, comments |
| `Schema.h` | What the names mean: pin types and conversions, node definitions, wildcard resolution, one compiler per schema. `GetSchemas()` |
| `GraphFile.h` | `.nmgraph` read and write, the path rules, the clipboard fragment, `GenerateGraphOutput` |
| `GraphCompiler.h` | `ValidateGraph` (what every schema shares), then `CompileGraph` into the schema's compiler; diagnostics name their node |
| `MaterialSchema.cpp` | Material to GLSL: data flow from Pixel Output (and each Discard) back, one `tN` temporary per computed value, Branch and For Loop as real `if` and `for` blocks, parameters in one `mat4` at binding 2 |
| `Shaders/MaterialPlatform.glsl` | Per-platform macros a generated shader may include: the control-flow hints |
| `ScriptSchema.cpp` | Script to Lua: exec flow from event nodes, pure values re-evaluated per statement; component nodes generated from the script bindings |
| `Scripts/cook_material_graphs.py` | `-check` a generated shader with glslc, or cook a folder of them, through GDI's own `cook_shaders.py` |

## The `.nmgraph` format

XML written with pugixml, the same choice as the text world package. There is
one file per graph, and the schema is named twice: once by the compound
extension, which is what tools route on, and once by the root's `Schema`
attribute, which is a check. A mismatch is a load error. A bare `*.nmgraph`
falls back to the attribute, with a warning.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<NodeGraph Version="1" Schema="Script">
  <Properties>
    <Property Name="TickPhase" Value="PostPhysics" />
  </Properties>
  <Variables>
    <Variable Name="Speed" Type="number" Default="60" />
  </Variables>
  <Nodes>
    <Node Id="6e6f...000a" Type="Event.OnTick" Version="1" X="-400" Y="80" />
    <Node Id="6e6f...000b" Type="Variable.Get" Version="1" X="-400" Y="200">
      <Prop Name="Variable" Value="Speed" />
    </Node>
    <Node Id="6e6f...000c" Type="Math.Multiply" Version="1" X="-200" Y="160" Wildcard="number">
      <Const Pin="B" Value="2" />
    </Node>
  </Nodes>
  <Links>
    <Link From="6e6f...000b.Value" To="6e6f...000c.A" />
  </Links>
  <Comments>
    <Comment X="-420" Y="40" Width="820" Height="260" Text="Slide along +x" />
  </Comments>
</NodeGraph>
```

`Tests/Data/Mover.script.nmgraph` is a complete example; `Games/Sandbox/Materials/Ripples.material.nmgraph` is a material with a For Loop.

- **Deterministic.** Nodes are sorted by GUID and links by their ends, and
  attributes come in a fixed order. Numbers use the shortest round-trip form
  (`std::to_chars`). An unchanged graph saves to the same bytes, and the save
  leaves such a file untouched.
- **GUIDs, never indices.** Node ids are `GameCore::EntityGuid`s: 128 random
  bits as 32 hex digits. Pins are named, so adding a pin to a node type breaks
  no link.
- **Only what differs.** A literal equal to its pin's default is not stored,
  so a changed default reaches old graphs.
- **Unknown node types survive.** A node whose type the schema no longer has,
  such as a component binding that was removed, loads as a *placeholder*. Its
  element is kept verbatim and written back unchanged, with its links. It
  fails compilation until it is deleted or the type returns.
- **Safe saves.** The file is written to `<file>.tmp` and then renamed over,
  as `SaveWorldPackage` does.
- **Clipboard.** Copy writes the selection as a `<NodeGraphFragment>`. Paste
  gives each node a fresh GUID and remaps the copied links onto the copies.

## Pin types and settings

A pin's type is one of three things. `Schema::ResolvePinType` is the one place
that turns any of them into a concrete type:

- **fixed**: `vec3`;
- **wildcard** (`*`): resolved from links, below;
- **derived** (`?`): computed by the schema from the node's settings, as the
  Component Mask's output is.

Node settings are properties of kind String, Identifier, Choice, Variable or
**Bool**. A Bool is a checkbox in the editor, stored as `true`/`false`, and
anything else is a validation error.

## Wildcards

Math nodes are `*`-typed: a material Add is float, vec2, vec3 or vec4, and a
script Add is number or Vector3. A node's type is, in order:

1. its stored `Wildcard`;
2. otherwise inferred from its input links. If they disagree, the widest
   wins: `WildcardTypes` is ordered narrowest first. The rule looks at output
   links only when no input decides;
3. otherwise the first allowed type.

The inference never depends on the order links happen to be stored in, so a
graph generates the same code in memory and after a save and reload (a test
checks this). The editor stores a type when a link first fixes it, and
Details > Type overrides it.

## Script graphs

- Event nodes (On Start, On Tick, On Destroy, On Event) each become one
  function of the class table.
- A statement's **Then** leads to the next statement.
- Pure nodes are evaluated where a statement needs them, into `local tN`,
  once per statement. So a Get after a Set sees the new value.
- A statement's own outputs (a Raycast's Hit) are locals declared at the top
  of its function. They can be read only in that function.
- Values travel as **lanes**, as the bindings pass them. A Vector3 is three
  numbers, a Color four, a Box six. Vector math is written per component and
  allocates nothing.
- Variables are fields of the class table (`Speed = 60,`), written per
  instance through `self`.
- On Event subscribes in `OnStart` and unsubscribes in `OnDestroy`, so a
  destroyed entity's handler never runs.

**Components come from their bindings.** Every type registered with
`GAMECORE_DEFINE_COMPONENT_SCRIPT` gets these nodes:

- Get, Has, Add and Remove on an entity;
- a getter per field, plus a setter unless the field is read-only;
- a call per method.

A method's pins come from the optional signature on its binding:

```cpp
Bind.Method("Translate", [](TransformComponent& C, ScriptCall& Call) { ... }, "Delta:Vector3");
Bind.Method("Scaled",    ...,                                                  "Factor:number -> Result:number");
```

The types are `number`, `integer`, `bool`, `string`, `Vector3`, `Color` and
`Entity`. A method without a signature still gets a node, called with no
arguments, and a compile warning.

## Material graphs

A material graph `Materials/Glow.material.nmgraph` generates two files beside
it, both committed:

- **`Glow.fsh`**, a pixel shader with the shape of `GDI/Shaders/Basic.fsh`:
  `#include <Common.glsl>`, `OUTPUT(0, vec4, Output)`, and **one uniform**,
  `mat4 MaterialParams`, alone in a UBO at binding 2. Bindings 0 and 1 are
  Common.glsl's view and transform blocks.
- **`Glow.material`**, the parameters' defaults, which the renderer reads with
  the shader:

  ```
  NimbleMaterial 1
  Param Color 0 vec3 0.2 0.8 1 0
  ```

**Why one `mat4`.** GDI's backends pack a shader's custom uniforms back to
back, in hash-map order, with no std140 padding (`PackCustomUBO` in the Vulkan
backend, `PackCustom` in D3D12). That is exactly right for one value and wrong
for two. So the four columns of `MaterialParams` are the slots:

| Column | Holds |
|---|---|
| 0, 1, 2 | Parameters, in name order: a `vec3` parameter reads `MaterialParams[i].xyz` |
| 3 | The drawing primitive's Tint (RGBA, 0 to 1): the **Instance Tint** input |

A material therefore has **at most three parameters**, and more is a compile
error. Slots follow names, not links, so a slot never moves because a link was
drawn elsewhere.

**Selecting components.** There are two nodes for picking out parts of a vector:

- **Component Mask** has R, G, B and A checkboxes and passes the ticked
  components through in that order. R and B of a vec3 is `.xz`, a vec2.
- **Swizzle** takes 1 to 4 letters, `xyzw` or `rgba`, in any case, but not
  mixed. It reorders and repeats: `zyx` reverses a vec3; `xxx` of a float
  becomes `vec3(f)`.

Either node's output type follows its settings, not its links. The pin is
declared `DERIVED_TYPE`, and `MaterialSchema::ResolvePinType` computes it.
Every type question in the compilers, the validation and the editor goes
through that one hook, so a downstream wildcard node follows along: an Add fed
a vec2 mask becomes vec2.

A component the input does not have (B of a vec2, `z` of a vec2) is an error on
the node, never a silent clamp. `DescribeNode` gives the editor the node's
header text, `.xz → vec2`.

**Control flow.** The Logic nodes write real statements:

- **Branch** compares A with B (`>`, `>=`, `<`, `<=`, `==`, `!=`) and gives
  True or False. It is an `if` block. With **Ternary** ticked in Details it is
  one expression instead, `A > B ? True : False`: both sides are computed
  first, and a Hint does not apply (it is a warning).
- **For Loop** and **For Loop End** are a pair, joined by their Loop pins, and
  everything between them is the body of a `for`. Value starts as Initial and
  becomes Next each time round. Index counts 0, 1, 2 and so on. Result is the
  value after the last time round. Count is a constant unless something is
  linked to it.

Each value is computed in the innermost block that all of its uses share. A
value only the True side needs is computed inside that side, so it is skipped
when False runs. A value both sides need is computed once, before the `if`.
Index and Value exist only in the body. Reading them anywhere else is an error
on the reading node, as is a For Loop without exactly one For Loop End. The
For Loop End takes its For Loop's type. Unless Details sets that type, it is
the widest suggested by the links of either node.

**Discard.** A Discard node throws the pixel away when A compares with B as
its Compare says. The default is `<` and B is 0.5, so it discards when an alpha
is below 0.5. Like Pixel Output it is a root, so it needs no link out, and a
graph may have any number of them. Each becomes `if(A < B) { discard; }` at the
top of `main`, in GUID order, before the colour. A value that a Discard and the
colour share is computed once, above both.

**Hints.** Branch has a Hint of None, Branch or Flatten; For Loop has None, Loop
or Unroll. Unroll needs a constant Count. A hint becomes a `HINT_*` macro from
`Shaders/MaterialPlatform.glsl`. A shader includes that header first, and only
when it uses a hint. The header maps each hint per platform:

| Platform | `HINT_BRANCH` becomes |
|---|---|
| Vulkan (glslc) | `[[branch]]`, under `GL_EXT_control_flow_attributes`, into the SPIR-V |
| D3D12 | `[branch]`, written by spirv-cross from that SPIR-V |
| OpenGL | `/* branch */`, a no-op: drivers do not take the attributes |

The macros are comments rather than empty because `cook_shaders.py`'s
preprocessor substitutes only defines that have a value.
`cook_material_graphs.py` resolves an `#include` against GDI's `Shaders/`
first, then this module's.

The inputs are:

- the interpolated TexCoord and Position;
- `gl_FragCoord`;
- the viewport size and the mouse;
- Instance Tint.

**Drawing one.** A primitive names the material by its base name:
`PrimitiveComponent::Material = "Glow"`. RenderCore then loads
`Materials/Glow.material` and the cooked shader under
`Shaders/Cooked/<Platform>/Materials/`, as GDI names its own shaders (see
RenderCore's DESIGN.md, "Materials"). A game's build cooks and stages them with
`nimble_stage_materials` (`Engine/cmake/NimbleMaterials.cmake`), for every
backend GDI is built with. The editor cooks one for its viewport on save and,
with Live Preview, on every clean recompile. The graph window's **Preview** dock
then shows it, drawn by that same renderer.

GDI is a vendored subtree and is never edited from here. So
`Scripts/cook_material_graphs.py` **imports** its `cook_shaders.py` read-only
and reuses its preprocessor and tool lookup. `#include <Common.glsl>` resolves
against GDI's `Shaders/`.

```
python Scripts/cook_material_graphs.py -check Games/Sandbox/Materials/Glow.fsh
python Scripts/cook_material_graphs.py -target Vulkan -src Games/Sandbox/Materials -out <runtime>/Shaders/Cooked -subdir Materials
```

`-src` takes a shader file or a folder.

## Tests

`NodeGraphTests` (ctest `NodeGraph.Tests`, on by default with
`NODEGRAPH_BUILD_TESTS`) covers:

- the path rules;
- byte-identical save, load and save;
- placeholders;
- paste remapping;
- validation errors landing on the right node;
- golden generated code in `Tests/Data/`;
- a **run** of the generated Lua: an entity with the Mover script attached
  moves 60 units in a second.

`NodeGraph.MaterialGLSL` compiles the golden shader with glslc when Python and
the Vulkan SDK are present.

A missing golden file is written and fails once, so a new one is always looked
at. After an intended change to the output, run with
`NODEGRAPH_UPDATE_GOLDEN=1` and review the diff.
