#include "Public/AICore/NavDebugDraw.h"

#include "DetourNavMesh.h"
#include "DetourTileCache.h"

#include "Private/AICore/NavInternal.h"
#include "Public/AICore/NavMesh.h"

/*
 * Navmesh lines. Drawn from Detour's tiles directly -- every polygon edge
 * once, coloured by area, the outer edges darker -- rather than through
 * DebugUtils' duDebugDrawNavMesh, which fills triangles a line-only overlay
 * cannot show.
 */
namespace AICore
{
    namespace
    {
        uint32_t Darker(const uint32_t InColor)
        {
            const uint32_t R = ((InColor >> 16) & 0xFF) / 2, G = ((InColor >> 8) & 0xFF) / 2, B = (InColor & 0xFF) / 2;
            return 0xFF000000u | (R << 16) | (G << 8) | B;
        }

        void Box(std::vector<NavDebugLine>& OutLines, const float* InMin, const float* InMax, const uint32_t InColor)
        {
            const float X[2] = { InMin[0], InMax[0] }, Y[2] = { InMin[1], InMax[1] }, Z[2] = { InMin[2], InMax[2] };
            const auto Line = [&](const int InA, const int InB)
            {
                NavDebugLine L;
                L.From = EngineCore::Vector3(X[InA & 1], Y[(InA >> 1) & 1], Z[(InA >> 2) & 1]);
                L.To   = EngineCore::Vector3(X[InB & 1], Y[(InB >> 1) & 1], Z[(InB >> 2) & 1]);
                L.Color = InColor;
                OutLines.push_back(L);
            };
            const int Edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                                       { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            for(const auto& Edge : Edges) { Line(Edge[0], Edge[1]); }
        }
    }

    uint32_t GetNavAreaColor(const uint8_t InArea)
    {
        if(InArea == NAV_AREA_GROUND)    { return 0xFF2EC4B6; }   // teal
        if(InArea == NAV_AREA_HIGH_COST) { return 0xFFF4A261; }   // amber
        if(InArea == NAV_AREA_BLOCKED)   { return 0xFF555555; }
        /* A custom area: a hue of its own. */
        const uint32_t Hash = uint32_t(InArea) * 2654435761u;
        return 0xFF000000u | (0x40 + (Hash >> 24) % 0xA0) << 16 | (0x40 + (Hash >> 16) % 0xA0) << 8 | (0x40 + (Hash >> 8) % 0xA0);
    }

    void DrawNavMesh(const NavMesh& InMesh, std::vector<NavDebugLine>& OutLines, const uint32_t InFlags, const float InLift)
    {
        const dtNavMesh* Mesh = InMesh.GetDetourMesh();
        if(!Mesh) { return; }
        for(int TileIndex = 0; TileIndex < Mesh->getMaxTiles(); ++TileIndex)
        {
            const dtMeshTile* Tile = Mesh->getTile(TileIndex);
            if(!Tile || !Tile->header) { continue; }
            if(InFlags & NDF_TileBounds) { Box(OutLines, Tile->header->bmin, Tile->header->bmax, 0x80FFFFFF); }

            for(int PolyIndex = 0; PolyIndex < Tile->header->polyCount; ++PolyIndex)
            {
                const dtPoly& Poly = Tile->polys[PolyIndex];
                if(Poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) { continue; }
                const uint32_t Color = GetNavAreaColor(Poly.getArea());
                for(int Edge = 0; Edge < Poly.vertCount; ++Edge)
                {
                    const unsigned short Neighbour = Poly.neis[Edge];
                    const bool bBoundary = Neighbour == 0;
                    if(bBoundary && !(InFlags & NDF_Boundaries)) { continue; }
                    if(!bBoundary)
                    {
                        if(!(InFlags & NDF_PolygonEdges)) { continue; }
                        /* An inner edge is drawn by its lower-numbered polygon only. */
                        if(!(Neighbour & DT_EXT_LINK) && int(Neighbour - 1) < PolyIndex) { continue; }
                    }
                    const float* A = &Tile->verts[Poly.verts[Edge] * 3];
                    const float* B = &Tile->verts[Poly.verts[(Edge + 1) % Poly.vertCount] * 3];
                    NavDebugLine Line;
                    Line.From = EngineCore::Vector3(A[0], A[1] + InLift, A[2]);
                    Line.To   = EngineCore::Vector3(B[0], B[1] + InLift, B[2]);
                    Line.Color = bBoundary ? Darker(Color) : Color;
                    OutLines.push_back(Line);
                }
            }
        }

        if(InFlags & NDF_Obstacles)
        {
            const dtTileCache* Cache = InMesh.GetTileCache();
            for(int Index = 0; Cache && Index < Cache->getObstacleCount(); ++Index)
            {
                const dtTileCacheObstacle* Obstacle = Cache->getObstacle(Index);
                if(!Obstacle || Obstacle->state == DT_OBSTACLE_EMPTY) { continue; }
                float Min[3], Max[3];
                Cache->getObstacleBounds(Obstacle, Min, Max);
                Box(OutLines, Min, Max, Obstacle->state == DT_OBSTACLE_PROCESSED ? 0xFFE63946 : 0xFFFFB703);
            }
        }
    }

    void DrawNavPath(const std::vector<EngineCore::Vector3>& InPoints, std::vector<NavDebugLine>& OutLines, const uint32_t InColor, const float InLift)
    {
        const EngineCore::Vector3 Lift(0.0, InLift, 0.0);
        for(size_t Index = 0; Index < InPoints.size(); ++Index)
        {
            const EngineCore::Vector3 P = InPoints[Index] + Lift;
            if(Index > 0) { OutLines.push_back({ InPoints[Index - 1] + Lift, P, InColor }); }
            const float S = 15.0f;
            OutLines.push_back({ P - EngineCore::Vector3(S, 0.0, 0.0), P + EngineCore::Vector3(S, 0.0, 0.0), InColor });
            OutLines.push_back({ P - EngineCore::Vector3(0.0, 0.0, S), P + EngineCore::Vector3(0.0, 0.0, S), InColor });
        }
    }
}
