#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Quat.h>
#include <FluidPlugin/FluidDeclarations.h>

struct plMsgAnimationPoseUpdated;

using plFluidBodyComponentManager = plComponentManager<class plFluidBodyComponent, plBlockStorageType::Compact>;

/// \brief Draws the owner into every fluid volume it overlaps: it drags smoke and flow, pushes a wake into water,
/// presses prints into snow and, with Obstacle, blocks flow like a collider.
///
/// The shapes come from the owner's local bounds (one oriented box) or, while animation poses arrive, from its
/// skeleton (one capsule per bone). The component has no update of its own. plFluidWorldModule calls ComputeShapes
/// once per frame, feeds the shapes to every overlapping volume and then calls AdvanceHistory. A physics actor whose
/// object or any ancestor carries an active Fluid Body is left out of the automatic physics touch sources.
class PL_FLUIDPLUGIN_DLL plFluidBodyComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plFluidBodyComponent, plComponent, plFluidBodyComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plFluidBodyComponent

public:
  plFluidBodyComponent();
  ~plFluidBodyComponent();

  /// \brief Where the shapes come from. Auto uses the skeleton while a pose arrived in the last couple of frames.
  plEnum<plFluidBodySource> m_Source; // [ property ]

  /// \brief Capsule radius of each bone, and the soft edge of the bounds box, metres.
  float m_fRadius = 0.1f; // [ property ]

  /// \brief Bones shorter than this (fingers, toes) produce no capsule, metres.
  float m_fMinBoneLength = 0.05f; // [ property ]

  /// \brief Most bone capsules per frame; the longest bones win. 1 to 64.
  plUInt16 m_uiMaxShapes = 24; // [ property ]

  /// \brief Scales the velocity the body drags smoke and flow with.
  float m_fStrength = 1.0f; // [ property ]

  /// \brief Scales how far moving or sinking pushes a water surface down.
  float m_fWake = 1.0f; // [ property ]

  /// \brief How hard the body presses into snow.
  float m_fPress = 1.0f; // [ property ]

  /// \brief Also blocks the flow like a Fluid Collider.
  bool m_bObstacle = false; // [ property ]

  /// \brief One world-space shape of this frame.
  struct Shape
  {
    plVec3 m_vCenter = plVec3::MakeZero();
    plVec3 m_vLastCenter = plVec3::MakeZero(); ///< centre of the same shape index last frame; the centre when there is none
    plVec3 m_vVelocity = plVec3::MakeZero();   ///< metres per second, zero when there is no history
    plVec3 m_vAxis = plVec3::MakeZero();       ///< Box: unit direction of its local X. Capsule: half the segment, metres
    plVec3 m_vHalfExtents = plVec3::MakeZero(); ///< Box: half extents, metres
    plQuat m_qRotation = plQuat::MakeIdentity(); ///< Box: world rotation
    float m_fRadius = 0.1f;
    plUInt8 m_uiShape = 0; ///< plFluidEmitterShape: Sphere, Box or Capsule
  };

  /// \brief Appends this frame's world shapes and returns which source produced them (Bounds or Skeleton).
  /// Velocities come from the history AdvanceHistory stored last frame.
  plFluidBodySource::Enum ComputeShapes(plDynamicArray<Shape>& inout_shapes, float fDeltaTime) const;

  /// \brief Remembers this frame's shape centres for the next frame's velocities. Only the world module calls this,
  /// once per frame, with exactly what ComputeShapes produced.
  void AdvanceHistory(plArrayPtr<const Shape> shapes, plFluidBodySource::Enum source);

private:
  /// Copies the joint positions and parents of the new pose. Writes only this component's members, so it is safe
  /// from an asynchronous component update.
  void OnMsgAnimationPoseUpdated(plMsgAnimationPoseUpdated& ref_msg); // [ msg handler ]

  bool UseSkeleton() const;
  void ComputeSkeletonShapes(plDynamicArray<Shape>& inout_shapes) const;
  void ComputeBoundsShape(plDynamicArray<Shape>& inout_shapes) const;

  // Last pose: joint positions in the owner's space (skeleton root transform applied), and parent indices.
  plDynamicArray<plVec3> m_JointPositions;
  plDynamicArray<plUInt16> m_JointParents;
  plUInt32 m_uiPoseFrame = 0;
  bool m_bHasPose = false;

  // Shape centres of last frame, by shape index.
  plDynamicArray<plVec3> m_LastCenters;
  plEnum<plFluidBodySource> m_LastSource;
  bool m_bHasHistory = false;
};
