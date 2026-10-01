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
| `ScriptSchema.cpp` | Script to Lua: exec flow from event nodes, pure values re-evaluated per statement; component nodes generated from the script bindings |
| `Scripts/cook_material_graphs.py` | `-check` a material function's permutations with glslc, cook them loose or into a shader cache, through GDI's own `cook_shaders.py`. Also cooks the engine's `Content/Shaders` |
| `Tools/NodeGraphGenerate.cpp` | Regenerates graphs' outputs from the command line, as the editor does on save |

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

Node settings are properties of kind String, Identifier, Choice, Variable,
**Bool** or **Texture**. A Bool is a checkbox in the editor, stored as
`true`/`false`, and anything else is a validation error. A Texture is a texture
asset's runtime-relative path (`Content/Textures/Brick.nmtexture`), typed,
browsed for or dropped from the Content Browser; anything but a `.nmtexture`
is a validation error.

An input pin with `Implicit` set has no constant: unlinked, the schema
supplies what it names -- a Texture Sample's UV is "Texture Coordinate". The
node and the Details panel show those words instead of a value.

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
it, both committed (RenderCore's MATERIALS.md has the whole design):

- **`Glow.mtl.glsl`**, a *material function*, not a whole shader: the
  material's uniform block and `CalcMaterial(Inputs, Surface)` -- plus
  `CalcMaterialOffset(Inputs)` when World Position Offset is linked. The
  engine's templates (`Engine/Content/Shaders/MaterialTemplate_VS/PS.glsl`)
  own `main()` and include it as `<Material.glsl>`, once per permutation.
- **`Glow.material`**, what the renderer needs to draw it:

  ```
  NimbleMaterial 2
  Domain Opaque
  Shading Lit
  TwoSided 0
  ClipValue 0.5
  MaxOffset 0
  HasOffset 0
  BlockSize 48
  Param Color vec3 32 0.2 0.8 1 0
  Texture 1 Nearest Default Content/Textures/Brick.nmtexture
  ```

**Material Output.** Its pins are the surface -- BaseColor, Metallic,
Roughness, Specular, Emissive, Opacity, OpacityMask, Normal,
AmbientOcclusion -- and World Position Offset; its settings are Domain
(Opaque, Masked, Translucent, PostProcess), Shading (Lit, Unlit), Two Sided,
Clip Value and Max Offset. Only a linked pin, or one set to something other
than its default, is written; the rest keep `DefaultMaterialSurface`'s value.
A pin the domain or shading never reads (BaseColor on an Unlit material) is a
warning. The first output, **Pixel Output** (one colour), still compiles: as
Unlit and Translucent, how every material drew before.

**Parameters** are members of one std140 block at binding 2, in name order,
after the engine's 32 bytes (the tint, Clip Value, Two Sided). The offsets are
computed here and written to the `.material`, and the renderer fills the block
byte for byte (GDI's `GDISetUniformBlock`). Any number fit, up to the block's
1024 bytes.

**Texture Sample** reads a texture asset at UV (unlinked: the texture
coordinate) and gives RGBA, RGB, R, G, B and A. Its settings are the Texture
and the sampler: **Filter** (Default, Nearest, Linear) and **Wrap** (Default,
Repeat, Clamp, Mirror); Default keeps what the texture was imported with. Each
distinct texture-and-sampler combination is one material texture slot, from 1
(slot 0 is a PostProcess material's scene), declared `Material_Texture<slot>`
at binding 4 + slot; seven fit. The `.material`'s `Texture` lines say which file
and settings fill each slot. One node's outputs share one sample. It works in
both stages; in World Position Offset it reads the full-size level
(`textureLod`), as the vertex stage has no derivatives. A Texture Sample with
no texture is an error.

**Two stages.** World Position Offset is the vertex stage's root; everything
else is the pixel stage's. Each stage is compiled from its own roots, so a
node both reach is written in both functions. A node's `StageMask` says where
it can run: Pixel Coordinate, Pixel Depth, Clip Position and Scene Texture are
pixel-only. A pixel-only node feeding World Position Offset, however
indirectly, is a compile error on that node; the editor refuses the link
(`Schema::CanLinkStages`), greys such nodes in the palette when dragging from
a vertex path, and badges them "Pixel".

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
`Engine/Content/Shaders/MaterialPlatform.glsl`. A shader includes that header first, and only
when it uses a hint. The header maps each hint per platform:

| Platform | `HINT_BRANCH` becomes |
|---|---|
| Vulkan (glslc) | `[[branch]]`, under `GL_EXT_control_flow_attributes`, into the SPIR-V |
| D3D12 | `[branch]`, written by spirv-cross from that SPIR-V |
| OpenGL | `/* branch */`, a no-op: drivers do not take the attributes |

The macros are comments rather than empty because `cook_shaders.py`'s
preprocessor substitutes only defines that have a value.
`cook_material_graphs.py` resolves every `#include` against
`Engine/Content/Shaders`.

The inputs read `MaterialInputs`, which each stage's template fills:

- Texture Coordinate, Vertex Color, Vertex Normal and World Position (both
  stages; World Position is before the offset in the vertex stage);
- Clip Position, Pixel Coordinate and Pixel Depth (linear and device; pixel only);
- Viewport Size, Mouse, Instance Tint and Time (both);
- Scene Texture (PostProcess materials only);
- Texture Sample (both; Textures in the palette).

**Drawing one.** A primitive names the material by its base name:
a slot of `RenderComponent::Materials`, "Glow" -- slot *i* for mesh section
*i*, slot 0 for a quad. RenderCore loads `Materials/Glow.material`
and the permutations its domain and shading need, named `Glow@<key>` under
`Shaders/Cooked/<Platform>/Materials/`. A game's build cooks them into its
module's shader cache with `nimble_stage_materials`
(`Engine/cmake/NimbleMaterials.cmake`), mounted when the module loads; Editor
targets also stage them loose. The editor cooks one for its viewport on save
and, with Live Preview, on every clean recompile. The graph window's **Preview**
dock then shows it, drawn by that same renderer.

GDI is a vendored subtree and is never edited from here. So
`Scripts/cook_material_graphs.py` **imports** its `cook_shaders.py` read-only
and reuses its preprocessor and tool lookup. `#include <Common.glsl>` resolves
against `Engine/Content/Shaders`, which holds the engine's own shaders (Basic,
Mesh, Screen and their includes). The engine's build cooks that folder into
the runtime directory the same way; GDI's own cook (and its `.qrc` bundles,
which only a Qt host needs) is switched off with `GDI_COOK_SHADERS`.

```
python Scripts/cook_material_graphs.py -check Games/Sandbox/Materials/Glow.mtl.glsl
python Scripts/cook_material_graphs.py -target Vulkan -src Games/Sandbox/Materials -out <runtime>/Shaders/Cooked -subdir Materials
python Scripts/cook_material_graphs.py -target Vulkan -src Games/Sandbox/Materials -subdir Materials -cache <runtime>/Shaders/Cache/SandboxGame.Vulkan.nmsc -module SandboxGame
```

`-src` takes a file or a folder; `-defaults` adds the shared MaterialDefault
vertex shaders. `NodeGraphGenerate <graph.nmgraph>...` regenerates graphs'
outputs from the command line, as the editor does on save.

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

`NodeGraph.MaterialGLSL` compiles every permutation of the golden material
functions with glslc when Python and the Vulkan SDK are present.

A missing golden file is written and fails once, so a new one is always looked
at. After an intended change to the output, run with
`NODEGRAPH_UPDATE_GOLDEN=1` and review the diff.
