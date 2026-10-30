#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <Foundation/Math/Transform.h>
#include <FluidPlugin/FluidDeclarations.h>

using plFluidColliderComponentManager = plComponentManager<class plFluidColliderComponent, plBlockStorageType::Compact>;

/// \brief Blocks the fluid inside every volume whose box contains it.
///
/// The component has no update of its own. plFluidWorldModule reads it once per frame through FillCollider and keeps
/// the obstacle mask the GPU already rasterised while no non-static collider moved.
class PL_FLUIDPLUGIN_DLL plFluidColliderComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plFluidColliderComponent, plComponent, plFluidColliderComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  //////////////////////////////////////////////////////////////////////////
  // plFluidColliderComponent

public:
  plFluidColliderComponent();
  ~plFluidColliderComponent();

  /// \brief Shape rasterised into the obstacle mask.
  plEnum<plFluidColliderShape> m_Shape; // [ property ]

  /// \brief Radius of a Sphere or Capsule collider, metres.
  float m_fRadius = 0.5f; // [ property ]

  /// \brief Half extents of a Box collider. A Capsule uses x as its half length along the object's local X, the same
  /// convention as a Capsule emitter and a Fluid Body bone. Metres.
  plVec3 m_vExtents = plVec3(0.5f, 0.5f, 0.5f); // [ property ]

  /// \brief The collider never moves, so it does not force the obstacle mask to be rebuilt.
  bool m_bStatic = true; // [ property ]

  /// \brief 0 makes the surface stick and kill the tangential flow, 1 lets the fluid slide along it freely.
  float m_fSlip = 0.0f; // [ property ]

  /// \brief Fills one world-space collider. Sphere and Capsule pack their radius into the extents.
  bool FillCollider(plFluidWorldCollider& out_collider) const;

  /// \brief The owner's global transform, which the world module compares against last frame's.
  plTransform GetGlobalTransform() const;

  /// \brief True when the owner's global transform differs from the given one beyond a sub-millimetre epsilon.
  bool HasMovedSince(const plTransform& last) const;
};
