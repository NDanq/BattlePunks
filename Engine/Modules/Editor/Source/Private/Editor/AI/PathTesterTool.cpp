#include "Private/Editor/AI/PathTesterTool.h"

#if WITH_EDITOR && WITH_AICORE

#include "Private/Editor/AI/EditorAI.h"
#include "Public/AICore/AIWorld.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/NimbleEngine/PhysicsFacade.h"

bool PathTesterTool::OnPress(ViewportWidget& InViewport, const Ray& InRay, const Qt::KeyboardModifiers InModifiers)
{
    (void)InViewport;
    AICore::AIWorld* AI = EditorAI::GetAI();
    if(!AI || !AI->GetNav().HasNavMesh()) { return false; }

    /* What the cursor is over (physics if simulated, else the Bounds and
     * terrain), onto the navmesh. */
    NimbleEngine::PhysicsRaycastHit Hit;
    if(!NimbleEngine::PhysicsRaycast(AI->GetWorld(), InRay.Origin, InRay.Direction, 200000.0f, Hit)) { return true; }
    EngineCore::Vector3 OnMesh;
    if(!AI->GetNav().GetQuery().ProjectPoint(Hit.Point, OnMesh, &AI->GetNav().GetDefaultFilter())) { OnMesh = Hit.Point; }

    EditorAI::TestPath& Path = EditorAI::GetTestPath();
    if((InModifiers & Qt::ShiftModifier) && Path.bHasStart)
    {
        Path.End = OnMesh;
        Path.bHasEnd = true;
    }
    else if(!Path.bHasStart || Path.bHasEnd)
    {
        Path.Start = OnMesh;
        Path.bHasStart = true;
        Path.bHasEnd = false;
    }
    else
    {
        Path.End = OnMesh;
        Path.bHasEnd = true;
    }
    EditorAI::UpdateTestPath();
    if(OnChanged) { OnChanged(); }
    return true;
}

#endif // WITH_EDITOR && WITH_AICORE
