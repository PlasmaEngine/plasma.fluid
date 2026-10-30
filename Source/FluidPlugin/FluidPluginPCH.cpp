#include <FluidPlugin/FluidPluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <FluidPlugin/Rendering/FluidGpuManager.h>
#include <FluidPlugin/Resources/FluidPresetResource.h>
#include <Foundation/Configuration/Startup.h>

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(Fluid, FluidPlugin)

  BEGIN_SUBSYSTEM_DEPENDENCIES
    "Foundation",
    "Core",
    "RenderWorld",
    "WindFieldManager"
  END_SUBSYSTEM_DEPENDENCIES

  ON_CORESYSTEMS_STARTUP
  {
    plResourceManager::RegisterResourceForAssetType("Fluid Preset", plGetStaticRTTI<plFluidPresetResource>());

    {
      plFluidPresetResourceDescriptor desc;
      plFluidPresetResourceHandle hResource = plResourceManager::CreateResource<plFluidPresetResource>("Missing Fluid Preset", std::move(desc), "Empty Fluid Preset");
      plResourceManager::SetResourceTypeMissingFallback<plFluidPresetResource>(hResource);
    }
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
    plResourceManager::SetResourceTypeMissingFallback<plFluidPresetResource>(plFluidPresetResourceHandle());
    plFluidPresetResource::CleanupDynamicPluginReferences();
  }

  ON_HIGHLEVELSYSTEMS_STARTUP
  {
    // After the wind field manager, so a volume's first step already sees this frame's wind.
    plFluidGpuManager::OnStartup();
  }

  ON_HIGHLEVELSYSTEMS_SHUTDOWN
  {
    plFluidGpuManager::OnShutdown();
  }

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on

PL_STATICLINK_LIBRARY(FluidPlugin)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(FluidPlugin_FluidDeclarations);
  PL_STATICLINK_REFERENCE(FluidPlugin_Components_FluidVolumeComponent);
  PL_STATICLINK_REFERENCE(FluidPlugin_Components_FluidEmitterComponent);
  PL_STATICLINK_REFERENCE(FluidPlugin_Components_FluidColliderComponent);
  PL_STATICLINK_REFERENCE(FluidPlugin_Components_FluidBodyComponent);
  PL_STATICLINK_REFERENCE(FluidPlugin_Components_FluidSmokeRenderComponent);
  PL_STATICLINK_REFERENCE(FluidPlugin_Components_FluidDriftSprayComponent);
  PL_STATICLINK_REFERENCE(FluidPlugin_Components_FluidSplashComponent);
  PL_STATICLINK_REFERENCE(FluidPlugin_FluidMessages);
  PL_STATICLINK_REFERENCE(FluidPlugin_Rendering_FluidGpuManager);
  PL_STATICLINK_REFERENCE(FluidPlugin_Resources_FluidPresetResource);
  PL_STATICLINK_REFERENCE(FluidPlugin_World_FluidWorldModule);
}
