#pragma once

#include <Core/World/World.h>
#include <Core/World/WorldModule.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Transform.h>
#include <FluidPlugin/Components/FluidBodyComponent.h>
#include <FluidPlugin/FluidDeclarations.h>
#include <FluidPlugin/Rendering/FluidGpuManager.h>

class plFluidVolumeComponent;
class plPhysicsWorldModuleInterface;

/// \brief Drives every fluid volume of one world.
///
/// Runs once per frame in PostTransform, after the component updates placed this frame's emitters and colliders. For
/// each registered volume it gathers the emitters, physics touch sources, queued impulses and colliders inside the
/// volume's box, samples wind and weather, and submits the result to plFluidGpuManager. Volumes with Land From Scene
/// also get their bed baked here, a few raycast rows per frame. Nothing here touches the GPU.
///
/// The update runs with m_bOnlyUpdateWhenSimulating false, so the editor simulates fluid without entering Play mode.
class PL_FLUIDPLUGIN_DLL plFluidWorldModule : public plWorldModule
{
  PL_DECLARE_WORLD_MODULE();
  PL_ADD_DYNAMIC_REFLECTION(plFluidWorldModule, plWorldModule);

public:
  plFluidWorldModule(plWorld* pWorld);
  ~plFluidWorldModule();

  virtual void Initialize() override;
  virtual void Deinitialize() override;

  /// \brief Adds a volume to this frame's set. Called from plFluidVolumeComponent::OnActivated.
  void RegisterVolume(plFluidVolumeComponent* pVolume);

  /// \brief Removes a volume and drops the state the module kept for it.
  void DeregisterVolume(plFluidVolumeComponent* pVolume);

  /// \brief Queues a one-off disturbance. Every volume whose box contains the position picks it up on the next update.
  void AddImpulse(const plVec3& vPosition, float fRadius, const plVec3& vVelocity, float fAmount, float fHeightImpulse);

  plUInt32 GetVolumeCount() const { return m_Volumes.GetCount(); }

private:
  /// Last position of a physics actor that fed a volume, for its velocity.
  struct PhysicsTouchState
  {
    plVec3 m_vLastPosition = plVec3::MakeZero();
    plUInt32 m_uiLastSeenFrame = 0;
  };

  /// The land bake of one volume: the bed every submission passes, and the bake in progress that will replace it.
  struct LandBake
  {
    plSharedPtr<const plFluidBed> m_pPublished; ///< last completed bake; never modified once published
    plSharedPtr<plFluidBed> m_pPending;         ///< bake in progress, null when none runs
    plUInt32 m_uiNextRow = 0;                   ///< next row of m_pPending to raycast

    /// What the pending or published bake was made for. A change beyond a small tolerance restarts the bake.
    plTransform m_Transform = plTransform::MakeIdentity();
    plVec2 m_vExtents = plVec2::MakeZero();
    float m_fHalfThickness = 0.0f;
    plUInt8 m_uiCollisionLayer = 0;
    bool m_bHasKey = false;
  };

  /// One registered volume plus everything the module remembers between frames for it.
  struct VolumeEntry
  {
    plFluidVolumeComponent* m_pVolume = nullptr;
    plHybridArray<plTransform, 8> m_LastColliders; ///< global transforms of last frame's collider set, in gather order
    bool m_bCollidersValid = false;                ///< false forces the next gather to rebuild the obstacle mask
    plHashTable<plGameObjectHandle, PhysicsTouchState> m_PhysicsTouchStates;
    plUInt32 m_uiPhysicsTouchFrame = 0;
    bool m_bHadBodyObstacles = false; ///< last gather added moving obstacle colliders: Fluid Bodies or touching physics bodies
    LandBake m_LandBake;
  };

  /// This frame's shapes of one Fluid Body, a range of m_BodyShapes.
  struct BodyEntry
  {
    plFluidBodyComponent* m_pBody = nullptr;
    plUInt32 m_uiFirstShape = 0;
    plUInt32 m_uiShapeCount = 0;
    plFluidBodySource::Enum m_Source = plFluidBodySource::Bounds;
  };

  void UpdateVolumes(const plWorldModule::UpdateContext& context);

  /// Takes the screenshots Fluid.Debug.Screenshots asks for, quits after Fluid.Debug.QuitAfter and logs the volume stats
  /// for Fluid.Debug.LogStats, all by fluid time. At each screenshot time it runs one capture sequence: every camera of
  /// Fluid.Debug.Camera times every material view of Fluid.Debug.CaptureViews, one screenshot each.
  void UpdateDebugCaptures();

  /// Pins the main view's camera to camera uiIndex of Fluid.Debug.Camera. False when there is no such camera.
  bool PinDebugCamera(plUInt32 uiIndex);

  plUInt32 m_uiDebugScreenshotsTaken = 0;
  bool m_bDebugQuitRequested = false;
  double m_fNextStatsLog = 0.0;
  plInt32 m_iCaptureShot = -1;        ///< shot of the running capture sequence, -1 while none runs
  plUInt32 m_uiCaptureShots = 0;      ///< shots in the running sequence
  plUInt32 m_uiCaptureFrame = 0;      ///< frames since the current shot's camera and view were applied
  plUInt32 m_uiCaptureCamera = 0;     ///< camera of Fluid.Debug.Camera the main view is pinned to
  plInt32 m_iCaptureRestoreView = 0;  ///< Fluid.Debug.Material before the sequence started

  /// Time the fluid of this world has run: the world clock while it simulates, the global clock while it does not.
  plTime m_FluidTime = plTime::MakeZero();

  /// Advances the volume's land bake by a few rows and sets ref_input.m_pBed to the last completed bed, or null when
  /// the volume does not take its land from the scene. Returns true while a bake is in progress.
  bool UpdateLandBake(VolumeEntry& ref_entry, plFluidVolumeInput& ref_input, const plPhysicsWorldModuleInterface* pPhysics);

  /// Appends one source per active emitter inside the box and advances that emitter's trail.
  void GatherEmitters(plFluidVolumeInput& ref_input, const plTransform& boxTransform, const plVec3& vBoxHalfExtents);

  /// \brief Draws the world's output atlas as a 2D overlay while any volume has a Debug View set.
  void DrawDebugView();

  /// Appends one source per dynamic or character physics actor inside the box, nearest to the volume centre first and
  /// capped by Fluid.Touch.MaxPhysicsSources. With bObstacles set, every such actor whose bounds straddle the volume
  /// plane also appends a sphere collider at the waterline to out_colliders. Returns silently when the world has no
  /// physics module.
  void GatherPhysicsTouchSources(VolumeEntry& ref_entry, plFluidVolumeInput& ref_input, const plTransform& boxTransform, const plVec3& vBoxHalfExtents, bool bObstacles, plDynamicArray<plFluidWorldCollider>& out_colliders);

  /// Appends every active collider inside the box and sets m_bCollidersDirty when the set or a non-static transform
  /// changed since the last frame.
  void GatherColliders(VolumeEntry& ref_entry, plFluidVolumeInput& ref_input, const plTransform& boxTransform, const plVec3& vBoxHalfExtents);

  /// Appends the queued impulses that fall inside the box.
  void GatherImpulses(plFluidVolumeInput& ref_input, const plTransform& boxTransform, const plVec3& vBoxHalfExtents) const;

  /// Collects every active Fluid Body's shapes of this frame into m_Bodies and m_BodyShapes.
  void CollectBodies(float fDeltaTime);

  /// Appends one source per body shape whose centre lies inside the box grown by the shape's reach, and one collider
  /// per such shape of an Obstacle body to out_colliders.
  void GatherBodies(plFluidVolumeInput& ref_input, const plTransform& boxTransform, const plVec3& vBoxHalfExtents, plDynamicArray<plFluidWorldCollider>& out_colliders) const;

  /// True when the object or any of its ancestors carries an active Fluid Body.
  static bool HasFluidBodyInHierarchy(const plGameObject* pObject);

  plDynamicArray<VolumeEntry> m_Volumes;
  plDynamicArray<BodyEntry> m_Bodies;                           ///< rebuilt every update
  plDynamicArray<plFluidBodyComponent::Shape> m_BodyShapes;     ///< rebuilt every update
  plDynamicArray<plFluidWorldSource> m_Impulses; ///< consumed and cleared by the next update
};
