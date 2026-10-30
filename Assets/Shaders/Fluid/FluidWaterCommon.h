#pragma once

#include <Shaders/Fluid/FluidCommon.h>

// Shared by the Water kernels: the staggered state layout, the bed and the obstacle mask. See "Water" in
// FluidSolverConstants.h for what each channel holds.

Texture2D<float4> FluidStateInput BIND_GROUP(BG_DRAW_CALL);
Texture2D<float4> FluidBedInput BIND_GROUP(BG_DRAW_CALL); // R: ground height relative to the plane, metres
Texture2D<float4> FluidObstacleInput BIND_GROUP(BG_DRAW_CALL);

// Ground under a texel, metres above the plane. Deeper ground counts as SolverOpenDepth deep, which is what keeps the
// wave speed, and with it the sub-step count, bounded however deep the scene's water is.
float WaterBedAt(int2 vTexel)
{
  if (SolverHasBed == 0)
    return -SolverOpenDepth;

  float2 uv = FluidTexelToUv(float2(FluidWrapTexel(vTexel)) + 0.5);
  return max(FluidBedInput.SampleLevel(LinearClampSampler, uv, 0).x, -SolverOpenDepth);
}

// The water surface over a texel. A surface below the ground is a dry texel, whose surface is the ground itself.
float WaterSurface(float fEta, float fBed)
{
  return max(fEta, fBed);
}

bool WaterIsOutside(int2 vTexel)
{
  return any(vTexel < int2(0, 0)) || any(vTexel >= int2(SolverRes));
}

// Velocity of the x face between texel vTexel and its +x neighbour. The faces on the grid border are walls unless the
// volume wraps.
float WaterFaceU(int2 vTexel)
{
  if (SolverBoundary != FLUID_BOUNDARY_WRAP && (vTexel.x < 0 || vTexel.x >= int(SolverRes.x) - 1 || WaterIsOutside(vTexel)))
    return 0.0;

  return FluidLoad(FluidStateInput, vTexel).y;
}

// Velocity of the y face between texel vTexel and its +y neighbour.
float WaterFaceV(int2 vTexel)
{
  if (SolverBoundary != FLUID_BOUNDARY_WRAP && (vTexel.y < 0 || vTexel.y >= int(SolverRes.y) - 1 || WaterIsOutside(vTexel)))
    return 0.0;

  return FluidLoad(FluidStateInput, vTexel).z;
}

// Bilinear fetch of the x face velocities at a texel-space position. The x face of texel (i, j) sits at (i + 1, j + 0.5).
float WaterSampleU(float2 vPos)
{
  float2 f = vPos - float2(1.0, 0.5);
  int2 i0 = int2(floor(f));
  float2 w = saturate(f - float2(i0));

  float a = lerp(WaterFaceU(i0), WaterFaceU(i0 + int2(1, 0)), w.x);
  float b = lerp(WaterFaceU(i0 + int2(0, 1)), WaterFaceU(i0 + int2(1, 1)), w.x);
  return lerp(a, b, w.y);
}

// Bilinear fetch of the y face velocities. The y face of texel (i, j) sits at (i + 0.5, j + 1).
float WaterSampleV(float2 vPos)
{
  float2 f = vPos - float2(0.5, 1.0);
  int2 i0 = int2(floor(f));
  float2 w = saturate(f - float2(i0));

  float a = lerp(WaterFaceV(i0), WaterFaceV(i0 + int2(1, 0)), w.x);
  float b = lerp(WaterFaceV(i0 + int2(0, 1)), WaterFaceV(i0 + int2(1, 1)), w.x);
  return lerp(a, b, w.y);
}

// 0 at the inner edge of an Open volume's sponge, rising quadratically to 1 on the outermost texel. 0 everywhere for
// Closed and Wrap volumes.
float WaterSponge(int2 vTexel)
{
  if (SolverBoundary != FLUID_BOUNDARY_OPEN || SolverSpongeWidth <= 0.0)
    return 0.0;

  float fToEdge = float(min(min(vTexel.x, vTexel.y), min(int(SolverRes.x) - 1 - vTexel.x, int(SolverRes.y) - 1 - vTexel.y)));
  float fRamp = saturate(1.0 - fToEdge / SolverSpongeWidth);
  return fRamp * fRamp;
}
