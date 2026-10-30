#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <Foundation/Math/Color.h>
#include <FluidPlugin/FluidPluginDLL.h>

struct plMsgUpdateLocalBounds;
struct plMsgExtractRenderData;

using plFluidSmokeRenderComponentManager = plComponentManager<class plFluidSmokeRenderComponent, plBlockStorageType::Compact>;

/// \brief Draws the Gas plFluidVolumeComponent on the same object as volumetric smoke.
///
/// The volume's 2D grid is extruded across its Height Range into a 3D texture that the engine's VDB volume renderer
/// lights, shadows and composites like any baked VDB volume. The component holds no GPU state: plFluidVolumeComponent
/// reads Depth and Falloff when it builds its frame input, and plFluidGpuManager owns the texture. Nothing draws
/// unless Fluid.SmokeVolumes is on.
class PL_FLUIDPLUGIN_DLL plFluidSmokeRenderComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plFluidSmokeRenderComponent, plComponent, plFluidSmokeRenderComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plFluidSmokeRenderComponent

public:
  plFluidSmokeRenderComponent();
  ~plFluidSmokeRenderComponent();

  /// \brief Multiplies the simulated density before it is marched.
  float m_fDensityScale = 1.0f; // [ property ]

  /// \brief Colour of the light the smoke scatters.
  plColor m_Albedo = plColor::White; // [ property ]

  /// \brief Tint of the glow; hot smoke glows along a blackbody ramp driven by the simulated temperature.
  plColor m_EmissionColor = plColor::Black; // [ property ]

  float m_fEmissionScale = 0.0f; // [ property ]

  /// \brief Phase function asymmetry: positive scatters forward, towards the viewer when looking into the light.
  float m_fAnisotropy = 0.3f; // [ property ]

  /// \brief Voxels across the Height Range of the volume.
  plUInt16 m_uiDepth = 16; // [ property ]

  /// \brief Exponent of the thickness profile. 1 fades linearly from the plane to the slab faces; higher keeps the
  /// smoke closer to the plane.
  float m_fFalloff = 1.0f; // [ property ]

protected:
  void OnUpdateLocalBounds(plMsgUpdateLocalBounds& msg);
  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;
};
