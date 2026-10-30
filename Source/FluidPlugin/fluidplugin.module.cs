using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class FluidPluginModule : ModuleRules
{
    public FluidPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/FluidPlugin", "FluidPluginPCH.h",
            "BUILDSYSTEM_BUILDING_FLUIDPLUGIN_LIB");

        // Shader headers shared with the engine's data directories (constants structs).
        PrivateIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Data", "Base"));
        PrivateIncludePaths.Add(Path.Combine(context.ProjectRoot, "Assets"));
    }
}
