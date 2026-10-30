#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/Id.h>
#include <Foundation/Types/RefCounted.h>
#include <Foundation/Types/SharedPtr.h>
#include <FluidPlugin/FluidDeclarations.h>
#include <RendererCore/Volumes/VDBVolumeResource.h>
#include <RendererFoundation/RendererFoundationDLL.h>

class plRenderContext;
struct plRenderWorldRenderEvent;
struct plRenderWorldFrameBindingEvent;

using plFluidVolumeId = plGenericId<20, 12>;

/// \brief One registered plFluidVolumeComponent inside a world's fluid simulation.
class plFluidVolumeHandle
{
  PL_DECLARE_HANDLE_TYPE(plFluidVolumeHandle, plFluidVolumeId);
};

/// \brief The ground under a volume, baked from the scene's static physics. Immutable once published: a new bake is a
/// new object, which is how the manager notices it has to upload again.
struct PL_FLUIDPLUGIN_DLL plFluidBed : public plRefCounted
{
  plUInt32 m_uiResolution = 0; ///< samples along each edge of the volume rectangle
  /// Bed height relative to the volume plane, metres, row-major with u along the volume's X: negative is ground that far
  /// below the plane, zero or positive is ground standing that high. Missing hits are stored as deep ground.
  plDynamicArray<float> m_Heights;
};

/// \brief What a volume publishes once per frame. Everything is world space; the manager converts into grid space.
struct PL_FLUIDPLUGIN_DLL plFluidVolumeInput
{
  plTransform m_Transform = plTransform::MakeIdentity(); ///< local XY spans the grid, local +Z is the plane normal
  plVec2 m_vExtents = plVec2(16.0f, 16.0f);              ///< world size of the simulated rectangle, metres
  float m_fHalfThickness = 1.0f;                         ///< how far above and below the plane sources reach
  plFluidSettings m_Settings;

  plDynamicArray<plFluidWorldSource> m_Sources;
  plDynamicArray<plFluidWorldCollider> m_Colliders;

  plVec3 m_vWind = plVec3::MakeZero(); ///< world-space wind at the volume centre
  float m_fPrecipitation = 0.0f;       ///< 0..1, the volume's own rate plus the world's weather when it uses it
  float m_fInitialDepth = 0.0f;        ///< Snow: metres of depth a freshly created grid starts with
  float m_fCameraDistance = 0.0f;      ///< metres from the main camera to the volume centre; picks the active volumes
  float m_fLodFactor = 0.0f;           ///< camera distance over the volume's LOD distance: > 1 halves the grid, > 2 freezes it
  float m_fSleepAfter = 3.0f;          ///< seconds without a source before the volume stops stepping
  plInt32 m_iPriority = 0;
  plTime m_DeltaTime;
  plTime m_WorldTime; ///< the world clock's accumulated time; drives time-varying patterns such as wind gusts
  bool m_bCollidersDirty = true; ///< false lets the manager keep the obstacle mask it already rasterised

  plSharedPtr<const plFluidBed> m_pBed; ///< the baked ground, or null for open deep water everywhere
  float m_fShoreFoam = 1.0f;            ///< Water: scales the swash zone and contact foam
  float m_fShoreDamping = 1.0f;         ///< Water: extra damping per second in the swash zone

  bool m_bSmokeVolume = false;  ///< Gas: extrude the grid into a 3D texture the VDB volume renderer draws as smoke
  plUInt32 m_uiSmokeDepth = 16; ///< voxels across the slab thickness
  float m_fSmokeFalloff = 1.0f; ///< exponent of the thickness profile; higher thins the smoke towards the slab faces
};

/// \brief What a Water volume's surface looked like a few frames ago, from the activity readback.
struct PL_FLUIDPLUGIN_DLL plFluidWaterStats
{
  float m_fLowestSurface = 0.0f;  ///< metres above the plane; deeper ground counts as the open water depth
  float m_fHighestSurface = 0.0f; ///< metres above the plane
  float m_fCentreSurface = 0.0f;  ///< lowest surface in the middle of the grid, metres
  float m_fFastestFlow = 0.0f;    ///< fastest face velocity, m/s
  float m_fInvalidTexels = 0.0f;  ///< texels holding a NaN or an infinity; anything but 0 is a solver bug
  plUInt32 m_uiReadback = 0;      ///< counts the readbacks, so a caller can tell a fresh one
};

/// \brief Per-volume counters for the editor's Debug View and the profiler.
struct PL_FLUIDPLUGIN_DLL plFluidVolumeStats
{
  plUInt32 m_uiResolution = 0;
  plUInt32 m_uiDispatches = 0;
  plUInt32 m_uiSteps = 0;
  plUInt32 m_uiVramBytes = 0;
  bool m_bActive = false;
  bool m_bAsleep = false;
};

/// \brief Owns every world's fluid simulation: per-volume solver textures, the shared output atlas and the volume
/// table consumers read.
///
/// Volumes submit on the update thread; nothing touches the GPU there. At BeginRender, after the wind field and touch
/// map have advanced, the manager picks the active volumes, steps each of them and copies the results into the
/// world's atlas. Views bind the atlas and the table at frame scope through BindForWorld, so any material, particle
/// or pipeline shader can call SampleFluid* from Shaders/Fluid/FluidSample.h.
///
/// The work is recorded on the device's main command list. On the serial render path views read this frame's
/// results; on the threaded path that list is submitted after the view graphs, so views read the previous frame's.
/// A frame that (re)creates the atlas or table therefore publishes no volumes, so nothing reads them undefined.
///
/// Same lifecycle as plWindFieldManager and plVegetationTouchMapManager, including the rule that a removed world
/// keeps zeroed constants alive: a render context holds frame-scope bindings past the removal, so the storage must
/// stay valid and read as "no volumes" rather than return to the pool for another owner to overwrite.
class PL_FLUIDPLUGIN_DLL plFluidGpuManager
{
public:
  /// \brief True while Fluid.Enable is set. Consumers see FluidNumVolumes == 0 when it is off.
  static bool IsEnabled();

  /// \brief Registers a volume. It owns no GPU memory until it is submitted and picked as active.
  static plFluidVolumeHandle AllocateVolume(plUInt32 uiWorldIndex);

  /// \brief Frees the volume's solver textures and its slot.
  static void Free(plUInt32 uiWorldIndex, plFluidVolumeHandle& inout_hVolume);

  /// \brief Publishes this frame's state of one volume. Call once per frame from the world module; a volume that
  /// stops submitting is dropped after a grace period.
  static void SubmitVolume(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume, plFluidVolumeInput&& input);

  /// \brief Drops a world's simulation. Its constants stay alive and zeroed for contexts that still bind them.
  static void RemoveWorld(plUInt32 uiWorldIndex);

  /// \brief Binds the world's fluid constants, atlas and volume table on the context's frame bind group.
  static void BindForWorld(plRenderContext* pRenderContext, plUInt32 uiWorldIndex);

  /// \brief Atlas textures of a world, for the Debug View. Channel 0 velocity, 1 density, 2 surface.
  static plGALTextureHandle GetAtlasTexture(plUInt32 uiWorldIndex, plUInt32 uiChannel);

  /// \brief Where a volume ended up in the atlas this frame, in uv. Invalid volumes return a zero rectangle.
  static plVec4 GetAtlasRect(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume);

  static plFluidVolumeStats GetVolumeStats(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume);

  /// \brief The 3D smoke texture of a Gas volume, wrapped as a VDB volume resource. Invalid unless the volume is Gas,
  /// asked for smoke and Fluid.SmokeVolumes is on. Its grid maps onto the volume's box: x and y span the extents, z
  /// the height range.
  static plVDBVolumeResourceHandle GetSmokeVolume(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume);

  /// \brief Edge length of the Snow activity grid GetSnowActivity returns.
  static constexpr plUInt32 SnowActivityGridSize = 16;

  /// \brief How much a Snow volume's surface was just disturbed, read back from the GPU a few frames late.
  ///
  /// out_activity receives SnowActivityGridSize^2 floats, row by row with u along x: the disturbance signal summed
  /// over each cell, i.e. the metres of snow presses recently kicked up there. It rises as a burst when something
  /// presses into fresh snow and decays to zero within a second; snowfall and wind never raise it. out_uiFrame counts the readbacks that have arrived,
  /// so a caller can tell a fresh grid from one it has already seen. Returns false and leaves the outputs untouched
  /// when the volume is not a stepping Snow volume, or no readback has arrived yet.
  static bool GetSnowActivity(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume, plArrayPtr<float> out_activity, plUInt32& out_uiFrame);

  /// \brief The surface statistics of a Water volume, read back a few frames late. False before the first readback or
  /// for any other mode.
  static bool GetWaterStats(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume, plFluidWaterStats& out_stats);

  /// \brief The Water activity around a volume-local uv (0..1 across the extents), read back a few frames late, over
  /// every cell within vRadiusUv of it: x the lowest and y the highest surface (metres above the plane), z the fastest
  /// face velocity (m/s). out_uiFrame counts the readbacks that have arrived. False before the first readback, for any
  /// other mode, or while the volume sleeps.
  static bool GetWaterActivityAround(plUInt32 uiWorldIndex, plFluidVolumeHandle hVolume, const plVec2& vUv, const plVec2& vRadiusUv, plVec4& out_cell, plUInt32& out_uiFrame);

  static void OnStartup();
  static void OnShutdown();

private:
  static void OnRenderEvent(const plRenderWorldRenderEvent& e);
  static void OnFrameBindingEvent(const plRenderWorldFrameBindingEvent& e);

  struct Data;
  static Data* s_pData;
};
