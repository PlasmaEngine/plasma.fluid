#pragma once

#include <Core/ResourceManager/ResourceManager.h>
#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <FluidPlugin/FluidDeclarations.h>
#include <FluidPlugin/Rendering/FluidGpuManager.h>
#include <FluidPlugin/Resources/FluidPresetResource.h>

struct plMsgUpdateLocalBounds;
struct plMsgFluidImpulse;

using plFluidVolumeComponentManager = plComponentManager<class plFluidVolumeComponent, plBlockStorageType::Compact>;

/// \brief One rectangle of simulated fluid: the grid, the preset that drives the solver and the box that decides which
/// emitters, colliders and touch sources feed it.
///
/// The owner's local XY plane spans the grid and local +Z is the plane normal. The component itself only publishes
/// state; plFluidWorldModule gathers the sources and colliders once per frame and submits the result to
/// plFluidGpuManager, which owns every texture.
class PL_FLUIDPLUGIN_DLL plFluidVolumeComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plFluidVolumeComponent, plComponent, plFluidVolumeComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plFluidVolumeComponent

public:
  plFluidVolumeComponent();
  ~plFluidVolumeComponent();

  /// \brief The Fluid Preset asset holding every solver parameter. Without one the solver defaults apply.
  void SetPreset(const plFluidPresetResourceHandle& hPreset);  // [ property ]
  const plFluidPresetResourceHandle& GetPreset() const;        // [ property ]
  PL_ADD_RESOURCEHANDLE_ACCESSORS_WITH_SETTER(Preset, m_hPreset, SetPreset);

  /// \brief While set, Mode replaces the preset's mode.
  void SetOverrideMode(bool bOverride);                        // [ property ]
  bool GetOverrideMode() const { return m_bOverrideMode; }     // [ property ]

  // Named FluidMode, not Mode: plComponent::GetMode already returns the component's plComponentMode.
  void SetFluidMode(plEnum<plFluidMode> mode);                 // [ property ]
  plEnum<plFluidMode> GetFluidMode() const { return m_Mode; }  // [ property ]

  /// \brief World size of the simulated rectangle in the owner's local XY plane, metres.
  void SetExtents(plVec2 vExtents);                            // [ property ]
  plVec2 GetExtents() const { return m_vExtents; }             // [ property ]

  /// \brief While set, Resolution replaces the preset's grid resolution.
  void SetOverrideResolution(bool bOverride);                                // [ property ]
  bool GetOverrideResolution() const { return m_bOverrideResolution; }       // [ property ]

  void SetResolution(plUInt32 uiResolution);                                 // [ property ]
  plUInt32 GetResolution() const { return m_uiResolution; }                  // [ property ]

  /// \brief Object the volume follows. Empty leaves the volume where its owner is.
  void SetFollowTargetReference(const char* szReference);                    // [ property ]

  /// \brief Object the volume follows, resolved. Invalid means a static volume.
  plGameObjectHandle GetFollowTarget() const { return m_hFollowTarget; }

  /// \brief While set, the volume follows the main or editor camera instead of Follow Target.
  bool m_bFollowMainCamera = false; // [ property ]

  /// \brief Full height of the slab around the plane that sources and colliders must be inside, metres.
  void SetHeightRange(float fHeightRange);                     // [ property ]
  float GetHeightRange() const { return m_fHeightRange; }      // [ property ]

  /// \brief Dynamic and character physics bodies inside the box disturb the fluid without a Fluid Emitter.
  bool m_bUseTouchSources = true; // [ property ]

  /// \brief The world's wind pushes the fluid, scaled by the preset's Wind Strength.
  bool m_bUseWind = true; // [ property ]

  /// \brief The world's weather feeds the volume's precipitation input.
  bool m_bUseWeather = true; // [ property ]

  /// \brief Seconds without any source after which the volume stops taking new input.
  /// \brief The volume's own snowfall or rainfall rate, 0 to 1. Added to the world's weather precipitation when Use
  /// Weather is on, so a volume can drive itself with no weather component in the scene.
  float m_fPrecipitation = 0.0f; // [ property ]

  /// \brief Snow only: metres of depth the grid starts with, so there is something to trample before snow has
  /// fallen. Applied when the volume is created and whenever its resolution or mode changes.
  float m_fInitialDepth = 0.0f; // [ property ]

  float m_fSleepAfter = 3.0f; // [ property ]

  /// \brief Beyond this camera distance the module stops gathering sources and colliders for the volume, metres.
  float m_fLodDistance = 60.0f; // [ property ]

  /// \brief Decides which volume wins where two overlap. Higher wins.
  plInt32 m_iPriority = 0; // [ property ]

  /// \brief Draws one solver channel of this volume over the scene.
  plEnum<plFluidDebugView> m_DebugView; // [ property ]

  /// \brief Bakes the ground under the volume from the scene's static physics, so water knows where it is shallow
  /// and where it runs up onto land. The bake runs over a few frames and restarts when the volume moves or resizes.
  bool m_bLandFromScene = false; // [ property ]

  /// \brief Collision layer the land bake raycasts with: static shapes whose layer collides with it count as ground.
  plUInt8 m_uiLandCollisionLayer = 0; // [ property ]

  /// \brief Water: scales the preset's Foam Surf, the foam of the swash zone and of water running against colliders.
  float m_fShoreFoam = 1.0f; // [ property ]

  /// \brief Water: extra damping per second in the last half metre of depth, so a beach swallows waves.
  float m_fShoreDamping = 1.0f; // [ property ]

  /// \brief While set, Settings replaces the preset's settings entirely. The Mode and Resolution overrides still apply
  /// on top.
  void SetOverrideSettings(bool bOverride);                            // [ property ]
  bool GetOverrideSettings() const { return m_bOverrideSettings; }     // [ property ]

  /// \brief The volume's own solver settings, used while Override Settings is on. Read on every GetEffectiveSettings
  /// call while the override is on, so an edit takes effect on the next frame.
  plFluidSettings m_OverrideSettings; // [ property ]

  /// \brief Physics bodies the automatic touch gather finds crossing the water plane also become obstacles, so waves
  /// and flow break against them. Bodies under a Fluid Body use that component's Obstacle option instead.
  bool m_bTouchObstacles = true; // [ property ]

  /// \brief The volume's slot in plFluidGpuManager. Invalid while the component is not activated.
  plFluidVolumeHandle GetVolumeHandle() const { return m_hVolume; }

  /// \brief The preset's settings, or the volume's own while Override Settings is on, with the Mode and Resolution
  /// overrides applied. Resolves the preset on first use after a change.
  const plFluidSettings& GetEffectiveSettings() const;

  /// \brief Fills everything the volume knows about itself: transform, extents, half thickness, settings, priority and
  /// shore parameters. Sources, colliders, wind, weather, camera distance and the baked bed are the world module's job.
  void BuildInput(plFluidVolumeInput& out_input) const;

protected:
  void OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg);
  void OnMsgFluidImpulse(plMsgFluidImpulse& msg);

  const char* DummyGetter() const { return nullptr; }

  plFluidPresetResourceHandle m_hPreset;
  plGameObjectHandle m_hFollowTarget;
  plVec2 m_vExtents = plVec2(16.0f, 16.0f);
  float m_fHeightRange = 2.0f;
  plUInt32 m_uiResolution = 256;
  plEnum<plFluidMode> m_Mode;
  bool m_bOverrideMode = false;
  bool m_bOverrideResolution = false;
  bool m_bOverrideSettings = false;

  plFluidVolumeHandle m_hVolume;

  /// Resolved lazily out of the preset or m_OverrideSettings; the setters only raise the dirty flag so no resource
  /// loads from a property edit or a transform change.
  mutable plFluidSettings m_Settings;
  mutable bool m_bSettingsDirty = true;
};
