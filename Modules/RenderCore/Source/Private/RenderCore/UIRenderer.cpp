#include "Private/RenderCore/UIRenderer.h"

#include "Public/Core/CoreMemory.h"
#include "Public/GDI/GDIMesh.h"

namespace RenderCore
{
    UISceneState::~UISceneState()
    {
        /* The mesh needs the driver: ReleaseAll has freed it by now. Only the
         * CPU block can still be here (a scene that never rendered). */
        EngineCore::Memory::Free(Block);
    }

    void UISceneState::Set(void* InBlock, const size_t InBytes)
    {
        EngineCore::Memory::Free(Block);
        Block = InBlock;
        Bytes = InBlock ? InBytes : 0;
        ++Generation;
        if(!Block || !List.Parse(Block, Bytes))
        {
            List = UIDrawListView();
        }
    }

    void UISceneState::Release()
    {
        Set(nullptr, 0);
        delete Mesh;   // every backend defers the buffers past a frame still using them
        Mesh               = nullptr;
        UploadedGeneration = 0;
        CommandTextures.clear();
        TexturesGeneration = 0;
    }
}
