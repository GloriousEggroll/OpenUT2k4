#pragma once

#include "CoreMinimal.h"

/**
 * Hardware ray tracing for Lumen (player setting UArenaUserSettings::RayTracing: 0 = Auto, 1 = On, 2 = Off).
 *
 * Runtime-built meshes (ArenaRuntimeContent, ArenaRuntimeMap) have no mesh distance fields and no Lumen cards, so
 * software Lumen only sees them through screen traces: everything off screen counts as open sky. Ray traced Lumen
 * uses the meshes' ray tracing geometry (built by UStaticMesh::BuildFromMeshDescriptions whenever ray tracing is
 * allowed) and hit lighting, so occlusion and bounce light match the cooked levels.
 *
 * The project enables r.RayTracing with r.RayTracing.EnableOnDemand (Config/DefaultEngine.ini), so the setting can
 * switch r.RayTracing.Enable and r.Lumen.HardwareRayTracing while playing. Without hardware / RHI support
 * (older GPUs, SM5, Metal without ray tracing, -noraytracing) Lumen uses software tracing and ArenaImportedLighting
 * compensates for the missing distance fields on runtime levels.
 *
 * Full Ray Tracing (separate setting, see IsFullActive) adds ray traced direct lighting and hit lighting.
 */
namespace ArenaRayTracing
{
	/** The project, the RHI and the GPU allow ray tracing (IsRayTracingAllowed()). */
	bool IsSupported();
	/** Whether the current setting resolves to ray traced Lumen (always false when not supported). */
	bool IsActive();
	/**
	 * Push the setting to the render cvars and refresh the lighting compensation of the loaded imported level.
	 * Called at startup (UArenaBrightnessSubsystem) and when the setting changes. Does nothing in the editor.
	 * Non-shipping: -ArenaRayTracing=0|1|2 overrides the saved setting (measurement runs).
	 */
	void Apply();
	/** Display text for a setting value ("Auto (On)", "On", "Off", "Off (unsupported)"). */
	FString GetChoiceText(int32 Choice);
	/** Number of setting values (Auto, On, Off). */
	constexpr int32 NumChoices = 3;

	/**
	 * Full Ray Tracing (UArenaUserSettings::FullRayTracing, 0 = Off, 1 = On), on top of ray traced Lumen: MegaLights
	 * direct lighting with ray traced shadows for the map lights and the sun, Lumen hit lighting for GI and
	 * reflections, rougher and multi-bounce reflections, front layer translucency reflections. Never the path
	 * tracer. Switching it off restores the previous render settings exactly. Non-shipping: -ArenaFullRT=0|1.
	 */
	bool IsFullActive();
	/** Full Ray Tracing can be switched on (ray traced Lumen is active). */
	bool CanUseFull();
	/** Display text: "On", "Off", or "Off (needs ray tracing)" when CanUseFull() is false. */
	FString GetFullChoiceText(int32 Choice);

	/**
	 * Non-shipping test hooks: -ArenaFrameStats=<from>-<to> records frame times between those world seconds of the
	 * first game world and logs average / median / 99th percentile frame and GPU times once;
	 * -ArenaToggleFullRT=<sec>,<sec>,... flips the Full Ray Tracing setting at those world seconds (live, saved).
	 * -ArenaExecAt=<sec>:<command>;... runs console commands at those world seconds.
	 */
	void StartTestHooks();
}
