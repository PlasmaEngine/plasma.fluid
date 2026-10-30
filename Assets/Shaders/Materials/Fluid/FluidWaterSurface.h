#pragma once

// Surface detail of the FluidWater material that does not come from the simulation: a Gerstner swell that runs across
// the simulated area and the open water around it alike, flow-stretched ripples, and caustics. Shared by the vertex
// shader (displacement) and the pixel shader (normals, foam, caustics) so both see the same waves. Everything works in
// world space on a horizontal water plane, with +Z up and time in seconds.

#define FLUID_WATER_WAVES 6

// Uneven steps in length and direction, so no two waves line up into a regular pattern.
static const float FluidWaterWaveAngle[FLUID_WATER_WAVES] = {0.0, 0.83, -0.61, 1.37, -1.19, 0.29};
static const float FluidWaterWaveLength[FLUID_WATER_WAVES] = {1.0, 0.73, 0.53, 0.39, 0.27, 0.19};
static const float FluidWaterWaveWeight[FLUID_WATER_WAVES] = {1.0, 0.85, 0.7, 0.55, 0.45, 0.35};
static const float FluidWaterWavePhase[FLUID_WATER_WAVES] = {0.0, 2.1, 4.3, 1.3, 5.6, 3.7};

struct plFluidWaterWaves
{
  float3 Displacement; // world space, metres
  float3 Normal;       // world space
  float Crest;         // 0..1, how close the surface is to folding over: whitecaps and the glow of thin crests
  float Height;        // metres above rest
};

// A fan of FLUID_WATER_WAVES Gerstner waves around the wind direction. The longest has wavelength fWaveLength and
// amplitude fAmplitude; the others are shorter with the same steepness, fanned up to fSpread radians off the wind.
// fChoppiness 0 is round sine waves, 1 sharp crests that just stop short of folding.
plFluidWaterWaves FluidWaterGerstner(float2 vPos, float fTime, float fAmplitude, float fWaveLength, float2 vWindDir, float fSpread, float fChoppiness, float fSpeed)
{
  plFluidWaterWaves result;
  result.Displacement = float3(0, 0, 0);
  result.Height = 0.0;

  float3 vNormalSum = float3(0, 0, 0);
  float fFold = 0.0;

  [branch]
  if (fAmplitude <= 0.0 || fWaveLength <= 0.01)
  {
    result.Normal = float3(0, 0, 1);
    result.Crest = 0.0;
    return result;
  }

  float fWindAngle = atan2(vWindDir.y, vWindDir.x);

  [unroll]
  for (int i = 0; i < FLUID_WATER_WAVES; ++i)
  {
    float fLength = fWaveLength * FluidWaterWaveLength[i];
    float fAngle = fWindAngle + FluidWaterWaveAngle[i] * fSpread;
    float2 vDir = float2(cos(fAngle), sin(fAngle));

    float k = 6.2831853 / fLength;
    float a = fAmplitude * (fLength / fWaveLength) * FluidWaterWaveWeight[i];
    float omega = sqrt(9.81 * k) * fSpeed;
    float theta = k * dot(vDir, vPos) - omega * fTime + FluidWaterWavePhase[i];

    float s = sin(theta);
    float c = cos(theta);
    float ka = k * a;
    float q = saturate(fChoppiness) / max(ka * FLUID_WATER_WAVES, 1e-4);

    result.Displacement += float3(q * a * vDir * c, a * s);
    vNormalSum += float3(vDir * ka * c, q * ka * s);
    fFold += q * ka * s;
  }

  result.Height = result.Displacement.z;
  result.Normal = normalize(float3(-vNormalSum.xy, 1.0 - vNormalSum.z));
  result.Crest = saturate((fFold - 0.35) / 0.5);
  return result;
}

// Two phases of a flow-advected texture lookup: surface detail is dragged along the current for one cycle and reset;
// the two phases are half a cycle apart and cross-faded with a triangle weight, so each one resets while it is
// invisible. vRate is the drift in texture units per second; the travel per cycle is capped at fMaxTravel units, so a
// fast current cycles the pattern rather than smearing it. Returns both drifts and the weight of the second phase.
void FluidWaterFlowPhases(float2 vRate, float fTime, float fCycle, float fMaxTravel, out float2 out_vDriftA, out float2 out_vDriftB, out float out_fBlend)
{
  float fCycleClamped = max(fCycle, 0.05);
  float fPhaseA = frac(fTime / fCycleClamped);
  float fPhaseB = frac(fPhaseA + 0.5);

  float2 vTravel = vRate * fCycleClamped;
  float fTravel = length(vTravel);
  if (fTravel > fMaxTravel)
    vTravel *= fMaxTravel / fTravel;

  out_vDriftA = -vTravel * (fPhaseA - 0.5);
  out_vDriftB = -vTravel * (fPhaseB - 0.5);
  out_fBlend = abs(fPhaseA * 2.0 - 1.0);
}

// Where surface detail is mapped. Around a vortex the water plane's own xy would stretch a tiling texture two to three
// times down the funnel wall; there the detail is mapped around the funnel (u) and along its wall (v) instead, in two
// ring bands. Band 0 has a power of two tiles around, each between one and two tiles wide; band 1 twice as many, each
// between a half and one tile wide. Blending them by Band1 keeps every visible tile close to square. On open water
// both bands are the plane's xy.
struct plFluidWaterFrame
{
  float2 Uv[2];       // tiles
  float2 UvDx[2];     // screen-space derivatives of Uv, free of the wrap where the angle jumps
  float2 UvDy[2];
  float TilesAround[2]; // tiles once around the vortex, 0 on open water
  float Band1;        // weight of band 1
  float3 TangentU;    // world direction u grows along
  float3 TangentV;    // world direction v grows along
  float Wall;         // 0..1, how much the wall frame is used
};

// Angular speed of a Rankine vortex (radians per second) at fRadius: the swirl rises linearly inside the core and falls
// off as 1/r outside, the same profile the solver drives the water toward.
float FluidWaterVortexAngularSpeed(float fRadius, float fCoreRadius, float fSwirl)
{
  float fShape = min(fRadius / fCoreRadius, fCoreRadius / max(fRadius, fCoreRadius));
  return fSwirl * fShape / max(fRadius, 0.25 * fCoreRadius);
}

// The turns a ring band of fTiles tiles has made by fTime. Every band turns rigidly, at the vortex's angular speed at
// the radius where its tiles are exactly fTile wide, so the pattern spins with the water without ever being sheared.
float FluidWaterBandTurns(float fTiles, float fTile, float fTime, float fCoreRadius, float fSwirl)
{
  float fRadius = fTiles * fTile / 6.2831853;
  return frac(FluidWaterVortexAngularSpeed(fRadius, fCoreRadius, fSwirl) * fTime / 6.2831853);
}

// The wall coordinates of a position around a vortex. vDelta: plane-local offset from the vortex centre (metres),
// fChord: straight-line distance from the bottom of the funnel (metres), which grows about one for one with the
// distance travelled along the wall. fTile: metres per tile.
void FluidWaterWallUv(float2 vDelta, float fChord, float fTile, float fTime, float fCoreRadius, float fSwirl,
  out float2 out_vUv0, out float2 out_vUv1, out float out_fBand1, out float out_fTiles0)
{
  float fRadius = max(length(vDelta), 1e-3);
  float fTurns = atan2(vDelta.y, vDelta.x) / 6.2831853;
  float fLevel = log2(max(6.2831853 * fRadius / fTile, 1.0));
  float fTiles0 = exp2(floor(fLevel));
  float fTiles1 = fTiles0 * 2.0;

  out_fBand1 = frac(fLevel);
  out_fTiles0 = fTiles0;
  out_vUv0 = float2((fTurns - FluidWaterBandTurns(fTiles0, fTile, fTime, fCoreRadius, fSwirl)) * fTiles0, fChord / fTile);
  out_vUv1 = float2((fTurns - FluidWaterBandTurns(fTiles1, fTile, fTime, fCoreRadius, fSwirl)) * fTiles1, fChord / fTile);
}

// Derivative of a coordinate that wraps every fPeriod units, with the wrap taken out.
float FluidWaterUnwrap(float fDerivative, float fPeriod)
{
  return fPeriod > 0.0 ? fDerivative - round(fDerivative / fPeriod) * fPeriod : fDerivative;
}

// Tangent-space slopes of a normal map sample, turned into a world-space offset of the normal along the frame axes.
float3 FluidWaterDetailOffset(float3 vNormalTS, float3 vTangentU, float3 vTangentV)
{
  float2 vSlope = vNormalTS.xy / max(vNormalTS.z, 0.2);
  return vTangentU * vSlope.x + vTangentV * vSlope.y;
}

float2 FluidWaterHash2(float2 p)
{
  uint2 q = uint2(int2(floor(p)) + 32768);
  uint h1 = (q.x * 1597334677u) ^ (q.y * 3812015801u);
  uint h2 = (q.x * 2654435761u) ^ (q.y * 1013904223u);
  h1 = (h1 ^ (h1 >> 15)) * 2246822519u;
  h2 = (h2 ^ (h2 >> 13)) * 3266489917u;
  return float2(h1 & 0xFFFFu, h2 & 0xFFFFu) / 65535.0;
}

// Distance to the nearest edge between two cells of an animated Voronoi pattern: the thin bright network of focused
// light that the rippling surface throws onto the ground.
float FluidWaterCausticEdge(float2 p, float fTime)
{
  float2 vCell = floor(p);
  float2 vFrac = p - vCell;
  float d1 = 8.0;
  float d2 = 8.0;

  [unroll]
  for (int y = -1; y <= 1; ++y)
  {
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
      float2 o = float2(x, y);
      float2 h = FluidWaterHash2(vCell + o);
      float2 vPoint = o + 0.5 + 0.42 * sin(fTime * (0.6 + 0.5 * h) + 6.2831853 * h) - vFrac;
      float d = dot(vPoint, vPoint);
      if (d < d1)
      {
        d2 = d1;
        d1 = d;
      }
      else if (d < d2)
      {
        d2 = d;
      }
    }
  }

  return sqrt(d2) - sqrt(d1);
}

// Caustic brightness at a world position on the ground, 0..~2, per colour channel with a slight dispersion.
float3 FluidWaterCaustics(float2 vPos, float fTime, float fTilesPerMetre)
{
  float2 p = vPos * fTilesPerMetre;
  // A slight split of the channels; wider turns the pattern into rainbows.
  float2 vShift = float2(0.004, 0.0025);
  float3 c;
  [unroll]
  for (int ch = 0; ch < 3; ++ch)
  {
    float2 q = p + vShift * (ch - 1);
    float e1 = FluidWaterCausticEdge(q, fTime);
    float e2 = FluidWaterCausticEdge(float2(q.y, -q.x) * 1.37 + 3.1, fTime * 1.23);
    float l1 = pow(saturate(1.0 - e1 * 3.5), 4.0);
    float l2 = pow(saturate(1.0 - e2 * 3.5), 4.0);
    c[ch] = l1 + l2 + 2.0 * l1 * l2;
  }
  return c;
}
