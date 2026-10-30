using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class FluidPluginTarget : TargetRules
{
    public FluidPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plFluidPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        ExtraModules.Add("FluidPluginModule");
    }
}
