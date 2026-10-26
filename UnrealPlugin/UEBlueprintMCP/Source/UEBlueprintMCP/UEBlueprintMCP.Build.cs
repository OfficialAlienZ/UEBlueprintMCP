// Copyright (c) 2025 zolnoor. All rights reserved.

using UnrealBuildTool;

public class UEBlueprintMCP : ModuleRules
{
	/**
	 * Enhanced Input is optional. Decision order:
	 *  1. Env var UEBLUEPRINTMCP_ENHANCED_INPUT=0/1 forces it off/on.
	 *  2. The project's .uproject entry for EnhancedInput (enabled/disabled) wins if present.
	 *  3. Otherwise on by default for UE 5.1+ (plugin is enabled by default), off for UE 4.27.
	 */
	private bool ShouldUseEnhancedInput(ReadOnlyTargetRules Target)
	{
		string Override = System.Environment.GetEnvironmentVariable("UEBLUEPRINTMCP_ENHANCED_INPUT");
		if (Override == "0") return false;
		if (Override == "1") return true;

		if (Target.ProjectFile != null)
		{
			ProjectDescriptor Project = ProjectDescriptor.FromFile(Target.ProjectFile);
			if (Project != null && Project.Plugins != null)
			{
				foreach (PluginReferenceDescriptor Plugin in Project.Plugins)
				{
					if (Plugin.Name == "EnhancedInput")
					{
						return Plugin.bEnabled;
					}
				}
			}
		}

		return Target.Version.MajorVersion > 5 || (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 1);
	}

	public UEBlueprintMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"InputCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"EditorSubsystem",
			"BlueprintGraph",
			"Kismet",
			"KismetCompiler",
			"GraphEditor",
			"Json",
			"JsonUtilities",
			"Networking",
			"Sockets",
			"UMG",
			"UMGEditor",
			"EditorScriptingUtilities",
			"AssetTools",
			"MaterialEditor",     // For UMaterialEditingLibrary and material expression manipulation
			"RenderCore",         // For material shader compilation
		});

		bool bEnhancedInput = ShouldUseEnhancedInput(Target);
		PublicDefinitions.Add("MCP_WITH_ENHANCED_INPUT=" + (bEnhancedInput ? "1" : "0"));
		if (bEnhancedInput)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "EnhancedInput", "InputBlueprintNodes" });
		}

		// RTTI/exceptions for crash handling (Windows SEH only - disable on Mac to avoid linker errors)
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			bUseRTTI = true;
			bEnableExceptions = true;
		}
	}
}
