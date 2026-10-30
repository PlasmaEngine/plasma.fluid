#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <FluidPlugin/FluidDeclarations.h>

using plFluidEmitterComponentManager = plComponentManager<class plFluidEmitterComponent, plBlockStorageType::Compact>;

/// \brief Injects density, temperature and velocity into every fluid volume whose box contains it.
///
/// The component has no update of its own. plFluidWorldModule reads it once per frame through FillSource and then
/// calls AdvanceTrail, so only the module advances time.
class PL_FLUIDPLUGIN_DLL plFluidEmitterComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plFluidEmitterComponent, plComponent, plFluidEmitterComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plFluidEmitterComponent

public:
  plFluidEmitterComponent();
  ~plFluidEmitterComponent();

  /// \brief Shape of the injected footprint. A Cone injects along the emitter's Velocity direction.
  plEnum<plFluidEmitterShape> m_Shape; // [ property ]

  /// \brief Radius of a Sphere or Capsule emitter, metres.
  float m_fRadius = 0.5f; // [ property ]

  /// \brief Half extents of a Box emitter, or length and half width of a Cone, metres.
  plVec3 m_vExtents = plVec3(0.5f, 0.5f, 0.5f); // [ property ]

  /// \brief How much density (smoke, snow, foam) is added per second.
  float m_fDensity = 1.0f; // [ property ]

  /// \brief Temperature written into a Gas volume. Above the preset's Ambient Temperature the plume rises.
  float m_fTemperature = 0.5f; // [ property ]

  /// \brief Velocity injected into the field, in the owner's local space, metres per second.
  plVec3 m_vVelocity = plVec3::MakeZero(); // [ property ]

  /// \brief Pushes on the surface of a Water volume, in metres per second: negative dents it, positive lifts it. Water
  /// feels it as a pressure, so a steady push holds a dent and a moving or oscillating one makes waves.
  float m_fHeightImpulse = 0.0f; // [ property ]

  /// \brief How hard the emitter presses on the pressure field.
  float m_fPress = 1.0f; // [ property ]

  /// \brief 0 injects evenly across the footprint, 1 fades out fully towards its border.
  float m_fFalloff = 1.0f; // [ property ]

  /// \brief The emitter contributes exactly once, then stays silent until ResetBurst.
  bool m_bBurst = false; // [ property ]

  /// \brief Injects along the segment travelled since the last frame instead of at a single point.
  bool m_bTrail = true; // [ property ]

  /// \brief Vortex: tangential speed at the edge of the core, metres per second. Positive turns counter-clockwise
  /// seen from the fluid volume's +Z.
  float m_fSwirl = 0.0f; // [ property ]

  /// \brief Vortex: inflow speed pulled toward the centre, metres per second. 0 is pure rotation.
  float m_fDrain = 0.0f; // [ property ]

  /// \brief Seconds for Height Impulse to swing through one full sine cycle, positive then negative. 0 keeps it
  /// constant. A Box emitter along a volume edge with a period becomes a swell generator.
  float m_fOscillationPeriod = 0.0f; // [ property ]

  /// \brief Fills one world-space source. False when a Burst emitter already fired. fluidTime is the time the world's
  /// fluid has run, which keeps running in the editor while the world clock is paused; the oscillation follows it.
  bool FillSource(plFluidWorldSource& out_source, plTime fluidTime) const;

  /// \brief Stores this frame's world position for the next frame's trail and consumes a Burst. Only the world module
  /// calls this, exactly once per frame.
  void AdvanceTrail();

  /// \brief Lets a Burst emitter fire again.
  void ResetBurst();

private:
  plVec3 m_vLastPosition = plVec3::MakeZero();
  bool m_bHasLastPosition = false;
  bool m_bBurstConsumed = false;
};
