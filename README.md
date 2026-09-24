# NimbleEngine

## Conventions

### Namespaces

Each module owns one namespace: `EngineCore` (Core), `GameCore`, `RenderCore`,
`GDIPlatform` and friends (GDI), `GameWindowSystem` (GameWindow). A handful of
nested ones exist where a module has a distinct sub-area — `EngineCore::Memory`,
`EngineCore::Threads` — and GDI additionally carries module-private helper
namespaces (`VKOps`, `VKUtil`, `D3D12Ops`, `D3D12Util`, `MTLUtil`) that are not
part of any public surface.

**`using namespace` is permitted at file scope in `.cpp` files only**, placed
after all `#include`s. Never before an include: that changes how the included
header resolves names.

**Never in a header** — public or `*Internal.h`. Both are included transitively:
a directive in `Public/Core/CoreTypes.h` lands in every game under `Games/`, and
one in `Private/RenderCore/RenderCoreInternal.h` lands in every `.cpp` in the
module. The resulting ambiguity errors surface at the use site, in code whose
author did not write the directive.

**Headers qualify fully.** The `GameCore::` and `EngineCore::` prefixes
throughout `Public/RenderCore/RenderProxy.h`, `Public/GDI/GDI.h` and the rest
are not noise to be cleaned up — they tell a reader which module a type comes
from, and they are what keeps the forward-declaration discipline in
`Engine/Source/Public/NimbleEngine/NimbleEngine.h` legible. Leave them.

**Namespace aliases (`namespace GC = GameCore;`) are not used.** Every module
namespace here is a single short word, so an alias saves a few characters at the
cost of grep-ability: `GC::World` is not findable by searching for
`GameCore::World`. Aliases would earn their keep against deep `A::B::C::` chains;
this tree has none. They also cannot be forward-declared through, which the
`NimbleEngine.h` pattern above depends on.

Self-qualification inside a module — `Memory::Allocate` written from within
`namespace EngineCore`, as in `Public/Core/CorePointer.h` — is already the short
form and stays as is.

#### Which files carry a directive

As of this writing: the four module `Tests/SmokeMain.cpp`, `CoreMemoryExample.cpp`,
`Engine/Source/Private/NimbleEngine/NimbleEngine.cpp`, the GDI Vulkan
`VKTexture.cpp` / `VKRenderTarget.cpp`, and the four files the convention was
written alongside — `RenderCore/.../WorldBridge.cpp`, `RenderCore/.../RenderThread.cpp`,
`GDI/.../VulkanDynamicGDI.cpp`, `GDI/.../D3D12DynamicGDI.cpp`.

The two GDI backend files sit at global scope rather than inside a module
namespace, so their directives are wider than the RenderCore ones. That is still
contained — nothing includes a `.cpp` — but it is the reason those two carry a
note saying so.

Game code under `Games/` decides for itself; `SandboxMain.cpp` qualifies
everything deliberately, which doubles as a check that the public surface is
usable without any directive at all.
