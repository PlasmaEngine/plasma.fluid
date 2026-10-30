#include <FluidPlugin/FluidPluginPCH.h>

#include <Foundation/IO/Stream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <FluidPlugin/Resources/FluidPresetResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plFluidPresetResource, 1, plRTTIDefaultAllocator<plFluidPresetResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plFluidPresetResource);

//////////////////////////////////////////////////////////////////////////

plResult plFluidPresetResourceDescriptor::Save(plStreamWriter& inout_stream) const
{
  // The descriptor version is the settings layout version.
  const plUInt8 uiVersion = plFluidSettings::SerializationVersion;
  inout_stream << uiVersion;

  m_Settings.Save(inout_stream);

  return PL_SUCCESS;
}

plResult plFluidPresetResourceDescriptor::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  if (uiVersion < 1 || uiVersion > plFluidSettings::SerializationVersion)
  {
    plLog::Error("Unknown fluid preset version {}", uiVersion);
    return PL_FAILURE;
  }

  // Load starts from the defaults, so a reload never keeps values of the previous content.
  return m_Settings.Load(inout_stream, uiVersion);
}

//////////////////////////////////////////////////////////////////////////

plFluidPresetResource::plFluidPresetResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plFluidPresetResource, plFluidPresetResourceDescriptor)
{
  m_Descriptor = descriptor;

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Loaded;

  return res;
}

plResourceLoadDesc plFluidPresetResource::UnloadData(Unload WhatToUnload)
{
  PL_IGNORE_UNUSED(WhatToUnload);

  m_Descriptor = plFluidPresetResourceDescriptor();

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;

  return res;
}

plResourceLoadDesc plFluidPresetResource::UpdateContent(plStreamReader* Stream)
{
  PL_LOG_BLOCK("plFluidPresetResource::UpdateContent", GetResourceIdOrDescription());

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;

  if (Stream == nullptr)
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  // skip the absolute file path data that the standard file reader writes into the stream
  {
    plStringBuilder sAbsFilePath;
    (*Stream) >> sAbsFilePath;
  }

  // skip the asset file header at the start of the file
  plAssetFileHeader AssetHash;
  AssetHash.Read(*Stream).IgnoreResult();

  if (m_Descriptor.Load(*Stream).Failed())
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  res.m_State = plResourceState::Loaded;
  return res;
}

void plFluidPresetResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plFluidPresetResource);
}

PL_STATICLINK_FILE(FluidPlugin, FluidPlugin_Resources_FluidPresetResource);
