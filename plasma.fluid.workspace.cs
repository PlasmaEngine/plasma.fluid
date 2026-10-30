// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaFluidWorkspace : WorkspaceRules
{
    public PlasmaFluidWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("FluidPlugin");
    }
}
