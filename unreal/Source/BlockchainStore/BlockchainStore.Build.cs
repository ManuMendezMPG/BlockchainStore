// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class BlockchainStore : ModuleRules
{
	public BlockchainStore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput" });

		// HTTP to talk to the bridge; Json/JsonUtilities to parse the responses.
		PrivateDependencyModuleNames.AddRange(new string[] { "HTTP", "Json", "JsonUtilities" });

		// UMG for the UserWidgets in C++ (UMainScreenWidget) + Slate that UMG pulls in.
		PrivateDependencyModuleNames.AddRange(new string[] { "UMG", "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
