#include "ArenaBrightness.h"
#include "ArenaUserSettings.h"
#include "ArenaRayTracing.h"
#include "SceneViewExtension.h"
#include "SceneView.h"
#include "Engine/World.h"

class FArenaBrightnessViewExtension : public FSceneViewExtensionBase
{
public:
	FArenaBrightnessViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override
	{
		// Called on the game thread after the final post process settings are resolved.
		const float Offset = FMath::Clamp(UArenaUserSettings::Get()->Brightness, UArenaUserSettings::MinBrightness, UArenaUserSettings::MaxBrightness);
		InView.FinalPostProcessSettings.AutoExposureBias += Offset;
	}

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
	{
		const UWorld* World = Context.GetWorld();
		return World && World->IsGameWorld() && FMath::Abs(UArenaUserSettings::Get()->Brightness) > KINDA_SMALL_NUMBER;
	}
};

void UArenaBrightnessSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Extension = FSceneViewExtensions::NewExtension<FArenaBrightnessViewExtension>();
	// The ray tracing setting is a display setting too: apply the saved choice before the first frame.
	ArenaRayTracing::Apply();
	ArenaRayTracing::StartTestHooks();
}

void UArenaBrightnessSubsystem::Deinitialize()
{
	Extension.Reset();
	Super::Deinitialize();
}
