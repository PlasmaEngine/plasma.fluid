using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class FluidTestTarget : TargetRules
{
    public FluidTestTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputName = "FluidTest";
        // Kept out of the package binary directory, which is what installs and publishes draw from.
        OutputDirectory = Path.Combine(context.ProjectRoot, "Bin", "Tests", $"{context.Platform}_x64_{context.Configuration}");
        UsePCHFiles = true;
        UseUnityBuild = false; // test fixtures reuse translation-unit-local helper names
        UseIncrementalLinking = true;
        GenerateDebugInfo = true;

        TargetDependencies.Add("FluidPlugin");
        ExtraModules.Add("FluidTestModule");
    }
}
