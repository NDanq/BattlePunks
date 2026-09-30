# NodeGraph

Node graphs that **generate source code**. A graph is edited in NimbleEditor
and compiled to an ordinary source file that is committed next to it. The
runtime never loads a graph.

| Graph | File | Generates | Consumed by |
|---|---|---|---|
| Material | `Glow.material.nmgraph` | `Glow.fsh`, the engine's macro GLSL | GDI's shader cook (`cook_material_graphs.py`, below) |
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
| `MaterialSchema.cpp` | Material to GLSL: pure data flow from Pixel Output back, one `tN` temporary per computed value, parameters as `SHADER_UNIFORM`s at binding 2 |
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

`Tests/Data/Mover.script.nmgraph` is a complete example.

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

The output has the shape of `GDI/Shaders/Basic.fsh`: `#include <Common.glsl>`,
`OUTPUT(0, vec4, Output)`, then one `UBO_BEGIN(2, <Name>_Block)` of the
parameters. Bindings 0 and 1 are Common.glsl's view and transform blocks. The
inputs are the interpolated TexCoord and Position, `gl_FragCoord`, the
viewport size and the mouse.

GDI is a vendored subtree and is never edited from here. So
`Scripts/cook_material_graphs.py` **imports** its `cook_shaders.py` read-only
and reuses its preprocessor and tool lookup. `#include <Common.glsl>` resolves
against GDI's `Shaders/`.

```
python Scripts/cook_material_graphs.py -check Games/Sandbox/Shaders/Glow.fsh
python Scripts/cook_material_graphs.py -target Vulkan -src Games/Sandbox/Shaders -out <dir>
```

There is no live preview yet: RenderCore has no material path to draw a
generated shader with. That is the next piece of work.

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
