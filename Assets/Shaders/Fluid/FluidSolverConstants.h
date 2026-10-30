#pragma once

#include <Shaders/Common/ConstantBufferMacros.h>
#include <Shaders/Common/Platforms.h>
#include <Shaders/Fluid/FluidConstants.h>

// Internal contract between plFluidGpuManager and the solver kernels. Consumers never include this - they use
// Shaders/Fluid/FluidSample.h, which ships with the engine.
//
// Every mode packs its whole grid state into ONE texture, ping-ponged between passes:
//   Gas:   RGBA16F (velocity.x, velocity.y, density, temperature)   velocity is plane-local, m/s
//   Water: RGBA32F (surface, face velocity x, face velocity y, foam)   see "Water" below
//   Snow:  RGBA16F (packed depth, compression, loose depth, disturbance) depths in metres
//          disturbance = metres of snow recently kicked up by something pressing into fresh snow; it decays
//          within a fraction of a second and drives the drift spray
//
// Snow keeps two layers because a footprint is not just a dent: the snow it displaces has to go somewhere. Packed
// snow stays put. Loose snow, thrown up by a press or newly fallen, slides down slopes and blows downwind until it
// settles back into the packed layer. The surface a consumer sees is packed + loose.
// Gas additionally owns a divergence texture and a ping-ponged pressure pair, both R32F.
// Water additionally owns a ping-ponged RGBA16F aux pair: (wet map, surface pressure head, unused, unused).
// Every mode owns an obstacle mask (R32F, 1 = solid).
//
// Water is depth-averaged shallow water on a staggered grid. Texel (i, j) holds:
//   .x  eta, the height of the water surface above the volume plane, metres. The ground under the texel is the bed b
//       (FluidBedInput, clamped to at most SolverOpenDepth below the plane); the water depth is max(eta - b, 0). A
//       texel whose depth is below SolverDryDepth is dry, and an eta below the bed reads as eta = b.
//   .y  the x velocity on the face between texel (i, j) and (i + 1, j), m/s
//   .z  the y velocity on the face between texel (i, j) and (i, j + 1), m/s
//   .w  foam, 0..2, carried by the flow
// The faces on the grid border are walls for Open and Closed volumes. An Open volume's sponge relaxes the surface
// toward rest, which also lets water flow in and out through the border region.
// Per frame FluidWaterInject applies the sources once (surface pressure, source velocity, vortices, foam, rain, the
// wet map); then SolverSubSteps pairs of FluidWaterVelocity (momentum on the faces) and FluidWaterHeight (mass and
// foam in the texels) advance it by SolverSubStepTime each.

#define FLUID_SOLVER_THREADS 8

#define FLUID_MAX_SOURCES 512
#define FLUID_MAX_COLLIDERS 128

// Sources are binned into FLUID_SOURCE_TILES^2 tiles per volume (Shaders/Fluid/FluidSources.h). The index list holds
// at most FLUID_MAX_SOURCE_INDICES entries; a source that no longer fits is dropped from the remaining tiles.
#define FLUID_SOURCE_TILES 16
#define FLUID_MAX_SOURCE_INDICES 16384

#define FLUID_SOURCE_SPHERE 0
#define FLUID_SOURCE_BOX 1
#define FLUID_SOURCE_CONE 2
#define FLUID_SOURCE_VORTEX 3 // Gas, Water: swirls the flow around the source's centre, optionally draining it inward
#define FLUID_SOURCE_CAPSULE 4 // a segment Position +- Axis with Radius around it: bones of a character or ragdoll

#define FLUID_COLLIDER_SPHERE 0
#define FLUID_COLLIDER_BOX 1
#define FLUID_COLLIDER_CAPSULE 2

#define FLUID_BOUNDARY_OPEN 0
#define FLUID_BOUNDARY_CLOSED 1
#define FLUID_BOUNDARY_WRAP 2

// SolverFlags
#define FLUID_FLAG_MACCORMACK 0x1

// Water: a source's HeightImpulse (m/s) acts on the surface as a pressure head of HeightImpulse * this many seconds,
// metres of water. Pressure moves water without adding or removing any, so a floating body keeps its dent and a
// moving one pushes a bow wave and a wake instead of draining the volume.
#define FLUID_WATER_PRESSURE_TIME 0.1

// Water: wind stress coefficient, the drag of air on water times the air to water density ratio. The acceleration of
// the water column is this times wind speed times wind velocity, over the depth.
#define FLUID_WATER_WIND_STRESS 0.0015

// Water: metres per second of water that ground above the rest level soaks up, so wave run-up drains off a beach.
#define FLUID_WATER_INFILTRATION 0.05

CONSTANT_BUFFER2(plFluidSolverConstants, 2, BG_DRAW_CALL)
{
  UINT2(SolverRes);          // grid resolution in texels
  FLOAT1(SolverTexelSize);   // metres per texel
  FLOAT1(SolverStepTime);    // seconds simulated by this step

  FLOAT2(SolverInvRes);      // 1 / SolverRes
  UINT1(SolverMode);         // FLUID_MODE_*
  UINT1(SolverBoundary);     // FLUID_BOUNDARY_*

  FLOAT1(SolverDensityDissipation);  // fraction of density surviving one step
  FLOAT1(SolverVelocityDissipation);
  FLOAT1(SolverBuoyancy);            // m/s^2 per unit of temperature above ambient
  FLOAT1(SolverAmbientTemperature);

  FLOAT1(SolverVorticity);   // vorticity confinement strength
  FLOAT1(SolverViscosity);
  FLOAT1(SolverSubStepTime); // Water: seconds one sub-step simulates
  FLOAT1(SolverDamping);     // Water: share of the face velocity surviving one sub-step

  FLOAT1(SolverOpenDepth);   // Water: metres of open water; deeper beds count as this deep, which sets the wave speed
  FLOAT1(SolverFoamDecay);   // Water: share of foam surviving one sub-step
  FLOAT1(SolverRainRate);    // expected rain drops per step over the whole grid
  FLOAT1(SolverRainRadius);  // texels

  FLOAT1(SolverAccumulation); // Snow, metres per step
  FLOAT1(SolverMaxDepth);     // Snow, metres
  FLOAT1(SolverCompression);  // Snow, 0..1 how far a press sinks
  FLOAT1(SolverRefill);       // Snow, metres per step of print refill

  FLOAT2(SolverWind);         // plane-local wind, m/s
  FLOAT1(SolverRidge);        // Snow, share of pressed mass that becomes loose instead of compacting away
  FLOAT1(SolverSlump);        // Snow, how fast loose snow slides down a slope

  UINT1(SolverNumSources);
  UINT1(SolverNumColliders);
  UINT1(SolverSeed);          // changes every step, seeds rain and noise
  UINT1(SolverFlags);

  UINT2(SolverAtlasOffset);   // texel offset of this volume's rectangle inside the atlas
  FLOAT1(SolverSettle);       // Snow, share of the loose layer that packs down this step
  FLOAT1(SolverRepose);       // Snow, height difference between neighbours a slope holds, metres

  FLOAT4(SolverInitialState); // what FluidClear writes into a newly created state texture, in that mode's packing

  UINT4(SolverSmokeDims);     // Gas smoke texture: width, height, depth in voxels, unused

  FLOAT1(SolverSmokeFalloff); // exponent of the smoke's thickness profile across the slab
  FLOAT1(SolverDisturbanceDecay); // Snow, share of the disturbance signal that survives one step
  FLOAT1(SolverMaxSpeed);     // Water: m/s cap on a face velocity, a quarter texel per sub-step, which keeps depths positive
  FLOAT1(SolverFriction);     // Water: Manning roughness of the bed, s/m^(1/3)

  FLOAT1(SolverShoreFoam);    // Water: foam per second per m/s of flow in the swash zone and against colliders
  FLOAT1(SolverShoreDamping); // Water: extra damping per second in the swash zone, so a beach swallows waves
  FLOAT1(SolverGravity);      // m/s^2
  UINT1(SolverHasBed);        // 1 when FluidBedInput holds this volume's baked bed; 0 means open water everywhere

  FLOAT1(SolverDryDepth);     // Water: metres of depth below which a texel is dry
  FLOAT1(SolverSpongeWidth);  // Water: texels along each Open border that absorb waves instead of reflecting them
  FLOAT1(SolverWetFade);      // Water: wet map lost per frame step once a texel is dry, 1 / Wet Memory * step time
  UINT1(SolverSubSteps);      // Water: sub-steps this frame, informational

  FLOAT1(SolverFoamDivergence); // Water: foam gain of converging flow and steep crests, where waves break
  FLOAT1(SolverFoamShear);      // Water: foam gain of flow strain, the wakes behind rocks and hulls and the edges of currents
  FLOAT1(SolverTime);           // world time in seconds, wrapped every 1000 s
  FLOAT1(SolverChop);           // Water: metres of pressure head in the chop noise field, 0 for none

  FLOAT1(SolverChopScale);      // Water: metres across one chop cell, about the wavelength of the waves it raises
  FLOAT1(SolverChopRate);       // Water: how fast the chop field changes, per second
  FLOAT1(SolverTurbulence);     // Gas, Water: 0..1, how unsteadily vortices turn
  FLOAT1(SolverTurbulencePadding0);
};

// The bed: how the ground under a volume sits relative to its plane. Baked on the CPU by raycasting the scene's static
// physics along the volume's -Z, uploaded as FluidBedInput (Texture2D<float4>, R channel), covering the volume's
// rectangle with uv 0..1 at its own resolution; sample it with LinearClampSampler at uv = texel position * SolverInvRes.
//   R < 0: the ground is -R metres below the plane
//   R >= 0: the ground stands R metres above the plane
// Gas and Snow treat R >= 0 as solid. Water treats the bed as ground it can flood and drain: the plane is the rest
// level of the water, so a beach above it starts dry and waves run up it.
// With SolverHasBed == 0 a 1x1 placeholder is bound and every kernel must treat the volume as open deep water.

// One emitter, touch source or scripted impulse, already converted into volume-local texel space.
struct PL_SHADER_STRUCT plFluidSourceGpu
{
  FLOAT2(Position);     // volume-local texel coordinates
  FLOAT2(PrevPosition); // last step's texel coordinates; the source is smeared along this segment

  FLOAT2(Velocity);     // plane-local velocity injected, m/s
  FLOAT1(Radius);       // texels
  FLOAT1(Density);      // added per second: density (Gas) or foam (Water)

  FLOAT1(Temperature);   // temperature added per second (Gas)
  FLOAT1(HeightImpulse); // Water: metres per second; negative pushes the surface down (see FLUID_WATER_PRESSURE_TIME)
  FLOAT1(Press);         // 0..1 how hard the source presses into snow (Snow)
  FLOAT1(Shape);         // FLUID_SOURCE_*

  FLOAT2(Extents);   // box half extents in texels, or cone length and half width
  FLOAT1(Falloff);   // 0 hard edge, 1 smooth
  FLOAT1(HeightFade); // 0..1, how much the source's distance from the plane weakens it

  FLOAT1(Swirl);          // Vortex: tangential speed at the edge of the core, m/s; positive turns counter-clockwise seen from +Z
  FLOAT1(Drain);          // Vortex: inflow speed pulled toward the centre, m/s; 0 is pure rotation
  FLOAT2(Axis);           // Box: unit direction of the box's local X in the plane (zero = axis aligned).
                          // Capsule: half the segment in texels; the capsule spans Position - Axis .. Position + Axis
};

// One collider rasterised into the obstacle mask, in volume-local texel space.
struct PL_SHADER_STRUCT plFluidColliderGpu
{
  FLOAT2(Position); // volume-local texel coordinates
  FLOAT2(Extents);  // sphere: x = radius. box: half extents. capsule: x = half length, y = radius

  FLOAT1(Shape);    // FLUID_COLLIDER_*
  FLOAT1(Slip);     // 0 no-slip wall, 1 free-slip
  FLOAT1(Rotation); // radians inside the plane
  FLOAT1(Padding0);
};
