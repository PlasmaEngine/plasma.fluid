#pragma once

#include <Core/ResourceManager/Resource.h>
#include <FluidPlugin/FluidDeclarations.h>

/// \brief Everything the editor writes for one fluid preset; the runtime resource holds it verbatim.
struct PL_FLUIDPLUGIN_DLL plFluidPresetResourceDescriptor
{
  plFluidSettings m_Settings;

  /// \brief Writes a version byte (plFluidSettings::SerializationVersion, currently 4) followed by the settings.
  plResult Save(plStreamWriter& inout_stream) const;

  /// \brief Reads versions 1 to 3. Fields a version does not carry keep their defaults.
  plResult Load(plStreamReader& inout_stream);
};

/// \brief One fluid preset: the complete solver parameter set a fluid volume simulates with.
class PL_FLUIDPLUGIN_DLL plFluidPresetResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plFluidPresetResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plFluidPresetResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plFluidPresetResource, plFluidPresetResourceDescriptor);

public:
  plFluidPresetResource();

  const plFluidPresetResourceDescriptor& GetDescriptor() const { return m_Descriptor; }
  const plFluidSettings& GetSettings() const { return m_Descriptor.m_Settings; }

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* Stream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  plFluidPresetResourceDescriptor m_Descriptor;
};
