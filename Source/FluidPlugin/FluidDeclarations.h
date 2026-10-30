#pragma once

#include <Core/ResourceManager/ResourceHandle.h>
#include <Foundation/Math/Vec2.h>
#include <Foundation/Math/Vec3.h>
#include <Foundation/Reflection/Reflection.h>
#include <FluidPlugin/FluidPluginDLL.h>

class plStreamWriter;
class plStreamReader;

using plFluidPresetResourceHandle = plTypedResourceHandle<class plFluidPresetResource>;

/// \brief What a volume simulates. One grid, three state layouts; see the package's Shaders/Fluid folder.
struct plFluidMode
{
  using StorageType = plUInt8;

  enum Enum
  {
    Gas,   ///< smoke, dust, ground fog: velocity, density and temperature with a pressure projection
    Water, ///< shallow water over the ground: waves, currents, whirlpools, run-up on a beach, foam and a wet map
    Snow,  ///< accumulating, trampled ground cover: depth and compression

    Default = Gas
  };

  /// \brief Maps a stored mode value of any layout version onto the current enum. The retired Flow mode (3) and
  /// Liquid (1) both became Water.
  static Enum FromStoredValue(plUInt8 uiValue);
};
PL_DECLARE_REFLECTABLE_TYPE(PL_FLUIDPLUGIN_DLL, plFluidMode);

/// \brief What happens at the border of the grid.
struct plFluidBoundary
{
  using StorageType = plUInt8;

  enum Enum
  {
    Open,   ///< fluid leaves and does not come back
    Closed, ///< the border is a wall
    Wrap,   ///< tiles with itself, which is what a baked tiling flowmap needs

    Default = Open
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_FLUIDPLUGIN_DLL, plFluidBoundary);

/// \brief How the advection step is integrated.
struct plFluidAdvection
{
  using StorageType = plUInt8;

  enum Enum
  {
    SemiLagrangian, ///< one backward trace, cheapest and smoothest
    MacCormack,     ///< adds an error correcting second trace: sharper, roughly twice the advection cost

    Default = SemiLagrangian
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_FLUIDPLUGIN_DLL, plFluidAdvection);

/// \brief Shape of a Fluid Emitter.
struct plFluidEmitterShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    Sphere,
    Box,
    Cone,   ///< injects along its own axis; the emitter's velocity sets the direction
    Vortex,  ///< swirls Gas and Water around the emitter's centre, optionally draining it inward: a whirlpool
    Capsule, ///< a segment along the emitter's local X, Extents.x long to each side, with Radius around it

    Default = Sphere
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_FLUIDPLUGIN_DLL, plFluidEmitterShape);

/// \brief Where a Fluid Body takes its shapes from.
struct plFluidBodySource
{
  using StorageType = plUInt8;

  enum Enum
  {
    Auto,     ///< skeleton bones while animation poses arrive, otherwise the bounds
    Bounds,   ///< the owner's local bounds as one oriented box
    Skeleton, ///< one capsule per bone of the last animation pose

    Default = Auto
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_FLUIDPLUGIN_DLL, plFluidBodySource);

/// \brief Shape of a Fluid Collider.
struct plFluidColliderShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    Sphere,
    Box,
    Capsule,

    Default = Box
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_FLUIDPLUGIN_DLL, plFluidColliderShape);

/// \brief Which channel a volume's Debug View draws over the scene.
struct plFluidDebugView
{
  using StorageType = plUInt8;

  enum Enum
  {
    Off,
    Velocity,
    Density,
    Pressure,
    Height,
    Obstacles,

    Default = Off
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_FLUIDPLUGIN_DLL, plFluidDebugView);

/// \brief Every solver parameter of one volume. Comes from a Fluid Preset asset, or from the volume's own Settings
/// while Override Settings is on; a volume may further override the mode and resolution.
struct PL_FLUIDPLUGIN_DLL plFluidSettings
{
  plEnum<plFluidMode> m_Mode;
  plEnum<plFluidBoundary> m_Boundary;
  plEnum<plFluidAdvection> m_Advection;

  plUInt32 m_uiResolution = 256;
  plUInt32 m_uiPressureIterations = 20;
  float m_fViscosity = 0.0f;

  // Gas
  float m_fDensityDissipation = 0.98f;
  float m_fVelocityDissipation = 0.995f;
  float m_fBuoyancy = 1.0f;
  float m_fAmbientTemperature = 0.0f;
  float m_fVorticity = 0.3f;

  // Water. Shallow water over the ground the volume bakes, or over open water Depth deep where there is none.
  float m_fDepth = 2.0f;       ///< metres of open water; deeper ground counts as this deep. Waves travel at sqrt(9.81 * Depth) m/s
  float m_fDamping = 0.004f;   ///< share of the flow lost per step at 60 steps per second: 0.004 lets a swell cross 30 m, 0.02 kills it within a few
  float m_fSpongeWidth = 1.0f; ///< metres along each Open border that absorb waves instead of reflecting them
  float m_fFriction = 0.025f;  ///< Manning roughness of the ground: 0.012 smooth concrete, 0.025 sand, 0.05 weedy river bed
  float m_fDryDepth = 0.01f;   ///< metres of water below which the ground counts as dry
  float m_fWetMemory = 10.0f;  ///< seconds the wet map takes to fade once the water has left
  float m_fFoamDivergence = 1.0f; ///< foam where waves break: converging flow and steep crests
  float m_fFoamSurf = 1.0f;       ///< foam in the swash zone and where water runs against colliders
  float m_fFoamShear = 1.0f;      ///< foam where the flow shears: the wakes behind rocks and hulls, the edges of currents
  float m_fFoamDecay = 0.5f;      ///< share of the foam that survives one second
  float m_fRainDropRate = 200.0f;
  float m_fRainDropSize = 0.15f;
  plUInt32 m_uiSubSteps = 4; ///< most sub-steps per frame. Fewer than a frame needs slow the waves down instead of going unstable
  float m_fChop = 0.0f;       ///< metres of surface pressure a churning noise field pushes around: the solver raises waves from it and the current carries them
  float m_fChopScale = 4.0f;  ///< metres across a chop cell, about the wavelength of the waves it raises
  float m_fTurbulence = 0.0f; ///< 0..1, how unsteadily vortices turn: spiral arms of stronger flow that heave the surface

  // Snow. Rates are metres per minute at full precipitation.
  //
  // Snow is held in two layers. Packed snow sits where it fell and does not move. Loose snow is what a footstep,
  // a wheel or fresh snowfall has disturbed: it slides down slopes and the wind carries it away, until it settles
  // back into the packed layer. That split is what makes a print throw up a ridge that then blows downwind.
  float m_fAccumulationRate = 0.1f;
  float m_fMaxDepth = 0.4f;
  float m_fCompression = 0.8f;
  float m_fRidge = 0.6f;    ///< share of the snow a press displaces that becomes loose instead of compacting away
  float m_fSlump = 0.5f;    ///< how fast loose snow slides down a slope, 0 leaves prints with vertical walls
  float m_fRepose = 0.02f;  ///< height difference between neighbours, in metres, that a slope holds before sliding
  float m_fSettle = 0.5f;   ///< share of the loose layer that packs down per second and stops being mobile
  float m_fRefillRate = 0.02f;

  // Coupling
  float m_fWindStrength = 1.0f;
  float m_fTouchStrength = 1.0f;
  float m_fTouchRadiusScale = 1.0f;

  // Output
  float m_fHeightScale = 1.0f;
  float m_fEdgeFade = 1.0f;

  /// \brief Resolution clamped to what the solver supports, so a bad asset cannot allocate an absurd grid.
  plUInt32 GetClampedResolution() const;

  /// \brief Layout version Save writes. Fluid presets and volume components both store their settings through
  /// Save and Load, so the two layouts cannot drift apart.
  static constexpr plUInt8 SerializationVersion = 6;

  /// \brief Writes every field in the SerializationVersion layout. Writes no version byte; the caller stores it.
  void Save(plStreamWriter& inout_stream) const;

  /// \brief Reads the fields of the given layout version (1 to SerializationVersion). Fields the layout does not
  /// carry keep their defaults. Fails on an unknown version.
  plResult Load(plStreamReader& inout_stream, plUInt8 uiVersion);
};
PL_DECLARE_REFLECTABLE_TYPE(PL_FLUIDPLUGIN_DLL, plFluidSettings);

/// \brief One source of fluid this frame: an emitter, a touch source or a scripted impulse, in world space.
struct plFluidWorldSource
{
  PL_DECLARE_POD_TYPE();

  plVec3 m_vPosition = plVec3::MakeZero();
  plVec3 m_vPrevPosition = plVec3::MakeZero();
  plVec3 m_vVelocity = plVec3::MakeZero();
  plVec3 m_vExtents = plVec3::MakeZero(); ///< box half extents, or cone length and half width
  plVec3 m_vAxis = plVec3::MakeZero();    ///< Box: unit world direction of its local X. Capsule: half the segment, world metres
  float m_fRadius = 0.5f;
  float m_fDensity = 0.0f;
  float m_fTemperature = 0.0f;
  float m_fHeightImpulse = 0.0f;
  float m_fPress = 0.0f;
  float m_fFalloff = 1.0f;
  float m_fSwirl = 0.0f; ///< Vortex: tangential speed at the edge of the core, m/s; positive is counter-clockwise seen from the fluid volume's +Z
  float m_fDrain = 0.0f; ///< Vortex: inflow speed toward the centre, m/s
  plUInt8 m_uiShape = 0; ///< plFluidEmitterShape
};

/// \brief One collider this frame, in world space.
struct plFluidWorldCollider
{
  PL_DECLARE_POD_TYPE();

  plVec3 m_vPosition = plVec3::MakeZero();
  plVec3 m_vExtents = plVec3::MakeZero();
  float m_fRotation = 0.0f; ///< radians around the volume's plane normal, filled in by the volume
  float m_fSlip = 0.0f;
  plQuat m_qRotation = plQuat::MakeIdentity();
  plUInt8 m_uiShape = 1; ///< plFluidColliderShape
};
