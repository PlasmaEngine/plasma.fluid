#pragma once

#include <Shaders/Fluid/FluidSolverConstants.h>

// Shared grid helpers for the solver kernels. Everything works in texel coordinates; the centre of texel (i, j) is
// at (i + 0.5, j + 0.5).

bool FluidOutOfGrid(uint2 id)
{
  return id.x >= SolverRes.x || id.y >= SolverRes.y;
}

float2 FluidTexelToUv(float2 vTexel)
{
  return vTexel * SolverInvRes;
}

// Applies the volume's boundary rule to a texel coordinate. Open and closed both clamp on read; they differ in how
// the velocity is reflected, which the kernels handle themselves.
int2 FluidWrapTexel(int2 vTexel)
{
  if (SolverBoundary == FLUID_BOUNDARY_WRAP)
  {
    int2 res = int2(SolverRes);
    return int2(((vTexel % res) + res) % res);
  }

  return clamp(vTexel, int2(0, 0), int2(SolverRes) - 1);
}

float4 FluidLoad(Texture2D<float4> tex, int2 vTexel)
{
  return tex.Load(int3(FluidWrapTexel(vTexel), 0));
}

float FluidLoadR(Texture2D<float4> tex, int2 vTexel)
{
  return tex.Load(int3(FluidWrapTexel(vTexel), 0)).x;
}

// Bilinear fetch at a texel-space position, honouring the boundary rule. Done with explicit loads rather than a
// sampler so wrap and clamp behave identically on every backend.
float4 FluidSampleBilinear(Texture2D<float4> tex, float2 vTexelPos)
{
  float2 vBase = vTexelPos - 0.5;
  int2 i0 = int2(floor(vBase));
  float2 f = saturate(vBase - float2(i0));

  float4 s00 = FluidLoad(tex, i0);
  float4 s10 = FluidLoad(tex, i0 + int2(1, 0));
  float4 s01 = FluidLoad(tex, i0 + int2(0, 1));
  float4 s11 = FluidLoad(tex, i0 + int2(1, 1));

  return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

bool FluidIsSolid(Texture2D<float4> obstacle, int2 vTexel)
{
  return FluidLoadR(obstacle, vTexel) > 0.5;
}

// Cheap hash for rain placement and jitter. Deterministic per texel and step.
uint FluidHash(uint x)
{
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

float FluidRandom(uint2 id, uint uiSalt)
{
  uint h = FluidHash(id.x * 73856093u ^ id.y * 19349663u ^ (SolverSeed + uiSalt) * 83492791u);
  return float(h & 0x00FFFFFFu) / float(0x01000000u);
}

// Smooth gradient noise in about -1..1 over a 3D point, for chop and turbulence. Gradient noise rather than value noise:
// value noise leaves the square lattice visible in a surface built from it. Independent of SolverSeed, so it drifts
// continuously with time instead of flickering per frame.
float FluidGradientNoise(float3 p)
{
  float3 i = floor(p);
  float3 f = p - i;
  float3 fade = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

  uint3 u = uint3(int3(i) + 4096);
  float n[8];
  [unroll]
  for (int k = 0; k < 8; ++k)
  {
    float3 corner = float3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
    uint3 c = u + uint3(corner);
    uint h = FluidHash(c.x * 73856093u ^ c.y * 19349663u ^ c.z * 83492791u);
    // One of 16 directions spread over the sphere.
    float z = float(h & 0xFu) / 7.5 - 1.0;
    float a = float((h >> 4) & 0xFFu) * (6.2831853 / 256.0);
    float s = sqrt(saturate(1.0 - z * z));
    float3 g = float3(s * cos(a), s * sin(a), z);
    n[k] = dot(g, f - corner);
  }

  float x0 = lerp(lerp(n[0], n[1], fade.x), lerp(n[2], n[3], fade.x), fade.y);
  float x1 = lerp(lerp(n[4], n[5], fade.x), lerp(n[6], n[7], fade.x), fade.y);
  return lerp(x0, x1, fade.z) * 1.6;
}

// Three octaves of FluidGradientNoise, -1..1, mostly. Each octave is turned against the last, so no lattice lines up.
float FluidNoiseFbm(float3 p)
{
  float3 p1 = float3(p.x * 0.8 - p.y * 0.6, p.x * 0.6 + p.y * 0.8, p.z) * 2.03 + 17.1;
  float3 p2 = float3(p1.x * 0.8 + p1.y * 0.6, -p1.x * 0.6 + p1.y * 0.8, p1.z) * 2.03 + 41.7;
  return FluidGradientNoise(p) * 0.6 + FluidGradientNoise(p1) * 0.28 + FluidGradientNoise(p2) * 0.12;
}

// Weight of a source at a texel, including its trail from PrevPosition to Position.
float FluidSourceWeight(plFluidSourceGpu src, float2 vTexelPos)
{
  uint uiShape = (uint)src.Shape;
  float fRadius = max(src.Radius, 0.5);

  // Distance to the segment the source travelled this step, so a fast mover leaves a continuous trail.
  float2 vSeg = src.Position - src.PrevPosition;
  float fSegLenSqr = dot(vSeg, vSeg);
  float fT = fSegLenSqr > 1e-6 ? saturate(dot(vTexelPos - src.PrevPosition, vSeg) / fSegLenSqr) : 1.0;
  float2 vClosest = src.PrevPosition + vSeg * fT;
  float2 vDelta = vTexelPos - vClosest;

  if (uiShape == FLUID_SOURCE_CAPSULE)
  {
    // The capsule's own segment, not the trail: a bone moves little per step, and its length is the shape.
    float2 vA = src.Position - src.Axis;
    float2 vAB = 2.0 * src.Axis;
    float fLenSqr = dot(vAB, vAB);
    float fS = fLenSqr > 1e-6 ? saturate(dot(vTexelPos - vA, vAB) / fLenSqr) : 0.5;
    float fCapsuleDist = length(vTexelPos - (vA + vAB * fS));
    float fCapsuleFalloff = lerp(0.5, fRadius, saturate(src.Falloff));
    return saturate((fRadius - fCapsuleDist) / max(fCapsuleFalloff, 0.5));
  }

  if (uiShape == FLUID_SOURCE_BOX)
  {
    // Rotate into the box's frame; a zero Axis keeps the box aligned with the grid.
    float2 vBoxX = dot(src.Axis, src.Axis) > 0.25 ? normalize(src.Axis) : float2(1, 0);
    vDelta = float2(dot(vDelta, vBoxX), dot(vDelta, float2(-vBoxX.y, vBoxX.x)));

    float2 vLocal = abs(vDelta);
    float2 vExtents = max(src.Extents, 0.5);
    float2 vOutside = max(vLocal - vExtents, 0.0);
    float fDist = length(vOutside);
    return saturate(1.0 - fDist / max(fRadius * src.Falloff + 0.5, 0.5));
  }

  if (uiShape == FLUID_SOURCE_CONE)
  {
    // Extents.x is the length along the injected velocity, Extents.y the half width at the far end.
    float2 vDir = normalize(src.Velocity + float2(1e-5, 0));
    float fAlong = dot(vDelta, vDir);
    float fAcross = abs(dot(vDelta, float2(-vDir.y, vDir.x)));
    float fLength = max(src.Extents.x, 1.0);
    float fHalfWidth = max(src.Extents.y, 0.5) * saturate(fAlong / fLength);
    if (fAlong < 0.0 || fAlong > fLength || fAcross > fHalfWidth)
      return 0.0;

    return saturate(1.0 - fAcross / max(fHalfWidth, 0.5)) * saturate(1.0 - fAlong / fLength);
  }

  float fDist = length(vDelta);
  float fFalloff = lerp(0.5, fRadius, saturate(src.Falloff));
  return saturate((fRadius - fDist) / max(fFalloff, 0.5));
}

// Rankine shape of a Vortex source: rises linearly to 1 at the core radius and falls off as radius / distance outside.
// Multiply by Swirl for the tangential speed and by Drain for the inflow speed. Distances in texels.
float FluidVortexShape(float fDist, float fRadius)
{
  return min(fDist / fRadius, fRadius / max(fDist, fRadius));
}

// How far a Vortex source reaches: 1 inside the core, fading to 0 at four core radii so a vortex never acts across the
// whole grid.
float FluidVortexReach(float fDist, float fRadius)
{
  return saturate((4.0 * fRadius - fDist) / (3.0 * fRadius));
}

// Coverage of a Vortex source's core: 1 inside the core radius with a one texel soft edge.
float FluidVortexCore(float fDist, float fRadius)
{
  return saturate(fRadius + 0.5 - fDist);
}
