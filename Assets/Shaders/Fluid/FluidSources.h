#pragma once

#include <Shaders/Fluid/FluidCommon.h>

// The source table and its tile lists. The manager bins every source into the FLUID_SOURCE_TILES x FLUID_SOURCE_TILES
// tiles its reach touches, so a texel only visits the sources of its own tile, however many the volume has:
//
//   uint2 range = FluidSourceRange(id.xy);
//   for (uint i = 0; i < range.y; ++i)
//   {
//     plFluidSourceGpu src = FluidSources[FluidSourceIndices[range.x + i]];
//     ...
//   }
//
// A source's reach includes a one texel margin, so a kernel may also evaluate it on the faces of its texel.

StructuredBuffer<plFluidSourceGpu> FluidSources BIND_GROUP(BG_DRAW_CALL);
StructuredBuffer<uint2> FluidSourceTiles BIND_GROUP(BG_DRAW_CALL); // per tile, row by row: first index, count
StructuredBuffer<uint> FluidSourceIndices BIND_GROUP(BG_DRAW_CALL);

uint2 FluidSourceRange(uint2 vTexel)
{
  uint2 vTileSize = max(SolverRes / FLUID_SOURCE_TILES, uint2(1, 1));
  uint2 vTile = min(vTexel / vTileSize, uint2(FLUID_SOURCE_TILES - 1, FLUID_SOURCE_TILES - 1));
  return FluidSourceTiles[vTile.y * FLUID_SOURCE_TILES + vTile.x];
}
