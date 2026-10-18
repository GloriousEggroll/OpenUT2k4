#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "ArenaBrightness.generated.h"

class FArenaBrightnessViewExtension;

/**
 * Applies the player's Brightness setting (UArenaUserSettings::Brightness, in exposure stops) to every game view.
 * A scene view extension adds the value to the final exposure bias after all post process volumes and camera
 * settings are blended, so it works the same with manual and automatic exposure, on procedural and imported
 * maps and in the menu. Editor viewports are left alone. Default 0 = no change.
 */
UCLASS()
class OPENUT2K4_API UArenaBrightnessSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	TSharedPtr<FArenaBrightnessViewExtension, ESPMode::ThreadSafe> Extension;
};
