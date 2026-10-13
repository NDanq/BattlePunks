#ifndef NIMBLEENGINE_AIINTERNAL_H
#define NIMBLEENGINE_AIINTERNAL_H

namespace GameCore { class World; }
namespace RenderCore { class Renderer; }

/* What GameSession calls of AIFacade.cpp: no AICore header past this one. */
namespace NimbleEngine
{
    namespace AIDetail
    {
        /* InWorld's AIWorld with the engine's host, on an authority. */
        void AttachLevelAI(GameCore::World& InWorld);
        void DetachLevelAI(GameCore::World& InWorld);

        /* AI.ShowNavMesh / AI.ShowPaths, over the frame (editor builds). */
        void DrawAIDebug(GameCore::World& InWorld, RenderCore::Renderer& InRenderer);
    }
}

#endif //NIMBLEENGINE_AIINTERNAL_H
