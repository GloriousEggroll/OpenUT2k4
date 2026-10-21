#include "ArenaArtFX.h"
#include "Sound/SoundBase.h"
#include "Kismet/GameplayStatics.h"
#include "ArenaArt.h"
#include "ArenaFX.h"
#include "OpenUT2k4.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/StrongObjectPtr.h"

// =================================================================================================
// Recipes
// =================================================================================================
//
// Asset paths are relative to /Game/Replacements or /Game/Imported (UT2004 package names; groups are joined with
// '_' by the importer). Sizes are in cm. Effects are looked up by the names used in the weapon / projectile tables
// below and in the game-object code.

namespace ArenaArtFXPrivate
{
	using namespace ArenaArtFX;

	// ---- textures ----
	const TCHAR* const T_Exp2 = TEXT("Textures/ExplosionTex/Framed_exp2_frames");          // 4x4 fireball
	const TCHAR* const T_Exp7 = TEXT("Textures/ExplosionTex/Framed_exp7_frames");          // 4x4 fireball
	const TCHAR* const T_We1 = TEXT("Textures/ExplosionTex/Framed_we1_frames");            // 4x4 fireball
	const TCHAR* const T_Smoke = TEXT("Textures/ExplosionTex/Framed_SmokeReOrdered");      // 4x4 smoke (alpha)
	const TCHAR* const T_MuchSmoke = TEXT("Textures/AW-2004Particles/Fire_MuchSmoke1");    // 4x4 smoke (alpha)
	const TCHAR* const T_Fireball = TEXT("Textures/AW-2004Explosions/Fire_Fireball1");
	const TCHAR* const T_FlashWhite = TEXT("Textures/XEffects/Skins_MuzFlashWhite_t");
	const TCHAR* const T_FlashA = TEXT("Textures/XEffects/Skins_MuzFlashA_t");
	const TCHAR* const T_FlashRocket = TEXT("Textures/XEffects/Skins_MuzFlashRocket_t");
	const TCHAR* const T_FlashFlak = TEXT("Textures/XEffects/Skins_MuzFlashFlak_t");
	const TCHAR* const T_FlashLink = TEXT("Textures/XEffects/Skins_MuzFlashLink_t");
	const TCHAR* const T_FlashBio = TEXT("Textures/XEffects/Skins_MuzFlashBio_t");
	const TCHAR* const T_FlashShock = TEXT("Textures/XEffects/Skins_MuzShockFlash_t");
	const TCHAR* const T_Spark = TEXT("Textures/XEffects/Skins_pcl_Spark");                 // 1x2: streak, dot
	const TCHAR* const T_BlueSpark = TEXT("Textures/XEffects/Skins_pcl_BlueSpark");         // 1x2: streak, dot
	const TCHAR* const T_Tracer = TEXT("Textures/XEffects/Skins_TracerT");                  // 1x2: streak, dot
	const TCHAR* const T_FlakTrail = TEXT("Textures/XEffects/FlakTrailTex");                // 1x2: streak, dot
	const TCHAR* const T_RocketFlare = TEXT("Textures/XEffects/RocketFlare");
	const TCHAR* const T_LightningBolt = TEXT("Textures/XEffects/Skins_LightningBoltT");    // 1x2: bolt, ring
	const TCHAR* const T_LightningCharge = TEXT("Textures/XEffects/LightningChargeT");      // 2x2
	const TCHAR* const T_GreenSmoke = TEXT("Textures/XEffects/GreenSmokeTex");
	const TCHAR* const T_ShockCore = TEXT("Textures/XEffectMat/Shock_shock_core");
	const TCHAR* const T_ShockCoreLow = TEXT("Textures/XEffectMat/Shock_shock_core_low");
	const TCHAR* const T_ShockFlareA = TEXT("Textures/XEffectMat/Shock_shock_flare_a");
	const TCHAR* const T_ShockFlareB = TEXT("Textures/XEffectMat/Shock_shock_flare_b");
	const TCHAR* const T_ShockRingA = TEXT("Textures/XEffectMat/Shock_Shock_ring_a");
	const TCHAR* const T_ShockRingB = TEXT("Textures/XEffectMat/Shock_shock_ring_b");
	const TCHAR* const T_ShockCombo = TEXT("Textures/XEffectMat/Shock_ShockComboFlash");
	const TCHAR* const T_ShockSparkle = TEXT("Textures/XEffectMat/Shock_shock_sparkle");
	const TCHAR* const T_ShockElec = TEXT("Textures/XEffectMat/Shock_Shock_Elec_a");
	const TCHAR* const T_ShockBeam = TEXT("Textures/XWeapons_rc/Effects_ShockBeamTex");
	const TCHAR* const T_ShockBeamRed = TEXT("Textures/XWeapons_rc/Effects_ShockBeamRedTex");
	const TCHAR* const T_LinkBeam = TEXT("Textures/XEffectMat/Link_link_beam_green");
	const TCHAR* const T_LinkMuz = TEXT("Textures/XEffectMat/Link_link_muz_green");
	const TCHAR* const T_LinkRing = TEXT("Textures/XEffectMat/Link_link_ring_green");
	const TCHAR* const T_LinkSpark = TEXT("Textures/XEffectMat/Link_link_spark_green");     // 1x2: streak, dot
	const TCHAR* const T_LinkSplash = TEXT("Textures/XEffectMat/Link_Link_projectile_splash"); // 2x2
	const TCHAR* const T_GreyRing = TEXT("Textures/XEffectMat/Link_grey_ring");
	const TCHAR* const T_TrailBlur = TEXT("Textures/AW-2004Particles/Weapons_TrailBlur");
	const TCHAR* const T_SoftFlare = TEXT("Textures/EpicParticles/Flares_SoftFlare");
	const TCHAR* const T_LinkMuzYellow = TEXT("Textures/XEffectMat/Link_link_muz_yellow");
	const TCHAR* const T_LinkMuzBlue = TEXT("Textures/XEffectMat/Link_link_muz_blue");
	const TCHAR* const T_LinkMuzRed = TEXT("Textures/XEffectMat/Link_link_muz_red");
	const TCHAR* const T_LinkMuzGreen = TEXT("Textures/XEffectMat/Link_link_muz_green");
	const TCHAR* const T_Blood = TEXT("Textures/XEffects/Skins_pcl_Blooda");           // 4x4 splats
	const TCHAR* const T_BotSpark = TEXT("Textures/XEffects/Skins_BotSpark");           // 1x2: streak, dot

	FSpriteDesc Sprite(const TCHAR* Texture, float Size, float LifeTime, float EndScale = 1.f)
	{
		FSpriteDesc S;
		S.Texture = Texture;
		S.Size = FVector2D(Size, Size);
		S.LifeTime = LifeTime;
		S.EndScale = EndScale;
		return S;
	}

	FSpriteDesc Flipbook(const TCHAR* Texture, int32 Columns, int32 Rows, float Size, float LifeTime, float EndScale = 1.f)
	{
		FSpriteDesc S = Sprite(Texture, Size, LifeTime, EndScale);
		S.Columns = Columns;
		S.Rows = Rows;
		S.FadePower = 0.35f;
		return S;
	}

	/** One cell of a sheet (row-major), held for the whole life. */
	FSpriteDesc Cell(const TCHAR* Texture, int32 Columns, int32 Rows, int32 Index, float SizeX, float SizeY, float LifeTime)
	{
		FSpriteDesc S;
		S.Texture = Texture;
		S.Columns = Columns;
		S.Rows = Rows;
		S.Frames = 1;
		S.StartFrame = Index;
		S.Size = FVector2D(SizeX, SizeY);
		S.LifeTime = LifeTime;
		return S;
	}

	FSpriteDesc Smoke(float Size, float LifeTime, float Delay, const FLinearColor& Tint)
	{
		FSpriteDesc S = Flipbook(T_Smoke, 4, 4, Size, LifeTime, 1.8f);
		S.Blend = EBlend::Translucent;
		S.Tint = Tint;
		S.Delay = Delay;
		S.FadeIn = 0.08f;
		S.FadePower = 1.2f;
		S.Spin = 25.f;
		return S;
	}

	FSpriteDesc Ring(const TCHAR* Texture, float Size, float LifeTime, float EndScale, const FLinearColor& Tint, float Intensity)
	{
		FSpriteDesc S = Sprite(Texture, Size, LifeTime, EndScale);
		S.Facing = EFacing::Fixed;
		// Effects are spawned with X = surface normal; the quad's normal is its +Z.
		S.Rotation = FRotator(90.f, 0.f, 0.f);
		S.Offset = FVector(2.f, 0.f, 0.f);
		S.Tint = Tint;
		S.Intensity = Intensity;
		return S;
	}

	FSpriteDesc BeamQuad(const TCHAR* Texture, float Width, float LifeTime, bool bVertical, const FLinearColor& Tint, float Intensity)
	{
		FSpriteDesc S;
		S.Texture = Texture;
		S.Facing = EFacing::Beam;
		S.Size = FVector2D(0.f, Width);
		S.LifeTime = LifeTime;
		S.bVerticalTexture = bVertical;
		S.bRandomRoll = false;
		S.Tint = Tint;
		S.Intensity = Intensity;
		S.EndScale = 0.35f;
		S.FadePower = 1.4f;
		return S;
	}

	FSpriteDesc MuzzleSprite(const TCHAR* Texture, float Size, const FLinearColor& Tint = FLinearColor::White, float Intensity = 1.5f)
	{
		FSpriteDesc S = Sprite(Texture, Size, 0.08f, 1.35f);
		S.Tint = Tint;
		S.Intensity = Intensity;
		S.FadePower = 1.f;
		return S;
	}

	FEffectDesc Muzzle(const TCHAR* Name, const TCHAR* Texture, float Size, const FLinearColor& LightColor,
		const FLinearColor& Tint = FLinearColor::White, float Intensity = 1.5f)
	{
		FEffectDesc E;
		E.Name = Name;
		E.LifeTime = 0.09f;
		E.Sprites.Add(MuzzleSprite(Texture, Size, Tint, Intensity));
		E.LightColor = LightColor;
		E.LightIntensity = 2500.f;
		E.LightRadius = 350.f;
		return E;
	}

	TArray<FEffectDesc> BuildEffects()
	{
		TArray<FEffectDesc> L;
		const FLinearColor White = FLinearColor::White;
		const FLinearColor Orange(1.f, 0.55f, 0.2f);
		const FLinearColor ShockPurple(0.6f, 0.35f, 1.f);
		const FLinearColor LinkGreen(0.4f, 1.f, 0.3f);
		const FLinearColor InstaRed(1.f, 0.25f, 0.45f);
		const FLinearColor LightningBlue(0.45f, 0.65f, 1.f);
		const FLinearColor SmokeGrey(0.35f, 0.33f, 0.32f, 1.f);

		// ---------------- explosions (Scale = damage radius / 340) ----------------
		{
			FEffectDesc E;
			E.Name = TEXT("Explosion.Rocket");
			E.RefRadius = 340.f;
			E.LifeTime = 1.3f;
			E.Sprites.Add(Flipbook(T_Exp2, 4, 4, 300.f, 0.7f, 1.35f));
			FSpriteDesc Second = Flipbook(T_Exp2, 4, 4, 210.f, 0.6f, 1.3f);
			Second.Delay = 0.07f;
			Second.Offset = FVector(30.f, 25.f, 15.f);
			E.Sprites.Add(Second);
			FSpriteDesc Flash = Sprite(T_FlashWhite, 380.f, 0.14f, 1.4f);
			Flash.Tint = Orange;
			E.Sprites.Add(Flash);
			E.Sprites.Add(Smoke(240.f, 1.2f, 0.12f, SmokeGrey));
			E.LightColor = FLinearColor(1.f, 0.55f, 0.25f);
			E.LightIntensity = 40000.f;
			E.LightRadius = 1000.f;
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Explosion.Grenade");
			E.RefRadius = 260.f;
			E.LifeTime = 1.2f;
			E.Sprites.Add(Flipbook(T_Exp7, 4, 4, 280.f, 0.65f, 1.3f));
			FSpriteDesc Flash = Sprite(T_FlashWhite, 300.f, 0.12f, 1.4f);
			Flash.Tint = Orange;
			E.Sprites.Add(Flash);
			E.Sprites.Add(Smoke(220.f, 1.1f, 0.1f, SmokeGrey));
			E.LightColor = FLinearColor(1.f, 0.6f, 0.3f);
			E.LightIntensity = 30000.f;
			E.LightRadius = 900.f;
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Explosion.FlakShell");
			E.RefRadius = 300.f;
			E.LifeTime = 1.2f;
			E.Sprites.Add(Flipbook(T_We1, 4, 4, 300.f, 0.6f, 1.3f));
			FSpriteDesc Second = Flipbook(T_Exp2, 4, 4, 180.f, 0.55f, 1.4f);
			Second.Delay = 0.05f;
			Second.Offset = FVector(-25.f, -20.f, 20.f);
			E.Sprites.Add(Second);
			FSpriteDesc Flash = Sprite(T_FlashFlak, 340.f, 0.15f, 1.3f);
			E.Sprites.Add(Flash);
			E.Sprites.Add(Smoke(220.f, 1.1f, 0.12f, SmokeGrey));
			E.LightColor = FLinearColor(1.f, 0.5f, 0.2f);
			E.LightIntensity = 35000.f;
			E.LightRadius = 900.f;
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Explosion.Goo");
			E.RefRadius = 120.f;
			E.LifeTime = 0.9f;
			FSpriteDesc Burst = Sprite(T_FlashBio, 170.f, 0.3f, 1.5f);
			Burst.Intensity = 1.5f;
			E.Sprites.Add(Burst);
			FSpriteDesc Burst2 = Sprite(T_FlashBio, 120.f, 0.45f, 2.f);
			Burst2.Spin = 90.f;
			Burst2.Delay = 0.05f;
			E.Sprites.Add(Burst2);
			FSpriteDesc Cloud = Sprite(T_GreenSmoke, 220.f, 0.8f, 1.6f);
			Cloud.Intensity = 2.5f;
			Cloud.Delay = 0.05f;
			E.Sprites.Add(Cloud);
			E.LightColor = FLinearColor(0.4f, 1.f, 0.2f);
			E.LightIntensity = 12000.f;
			E.LightRadius = 500.f;
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Explosion.ShockCombo");
			E.RefRadius = 550.f;
			E.LifeTime = 1.f;
			FSpriteDesc Flash = Sprite(T_ShockCombo, 700.f, 0.35f, 1.6f);
			Flash.Intensity = 2.5f;
			E.Sprites.Add(Flash);
			FSpriteDesc Core = Sprite(T_ShockCore, 420.f, 0.6f, 1.5f);
			Core.Intensity = 2.f;
			E.Sprites.Add(Core);
			FSpriteDesc Wave = Sprite(T_ShockRingB, 200.f, 0.55f, 5.f);
			Wave.Intensity = 2.f;
			Wave.FadePower = 0.8f;
			E.Sprites.Add(Wave);
			FSpriteDesc Wave2 = Wave;
			Wave2.Texture = T_ShockRingA;
			Wave2.Delay = 0.08f;
			Wave2.EndScale = 4.f;
			E.Sprites.Add(Wave2);
			FSpriteDesc Elec = Sprite(T_ShockElec, 500.f, 0.5f, 1.4f);
			Elec.Spin = 120.f;
			E.Sprites.Add(Elec);
			E.Sprites.Add(Sprite(T_ShockSparkle, 300.f, 0.7f, 2.f));
			E.LightColor = ShockPurple;
			E.LightIntensity = 60000.f;
			E.LightRadius = 1600.f;
			L.Add(E);
		}

		// ---------------- impacts (Scale ~ 1) ----------------
		{
			FEffectDesc E;
			E.Name = TEXT("Impact.ShockBeam");
			E.LifeTime = 0.45f;
			E.Sprites.Add(Ring(T_ShockRingA, 50.f, 0.35f, 3.f, White, 2.f));
			FSpriteDesc Flare = Sprite(T_ShockFlareA, 110.f, 0.3f, 0.6f);
			Flare.Intensity = 2.f;
			E.Sprites.Add(Flare);
			E.Sprites.Add(Sprite(T_ShockSparkle, 70.f, 0.4f, 1.5f));
			E.LightColor = ShockPurple;
			E.LightIntensity = 6000.f;
			E.LightRadius = 400.f;
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Impact.ShockBall");
			E.LifeTime = 0.5f;
			FSpriteDesc Core = Sprite(T_ShockCore, 120.f, 0.35f, 1.8f);
			Core.Intensity = 2.f;
			E.Sprites.Add(Core);
			E.Sprites.Add(Sprite(T_ShockRingA, 80.f, 0.4f, 3.f));
			E.Sprites.Add(Sprite(T_ShockSparkle, 90.f, 0.45f, 1.8f));
			E.LightColor = ShockPurple;
			E.LightIntensity = 9000.f;
			E.LightRadius = 500.f;
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Impact.Bullet");
			E.LifeTime = 0.5f;
			FSpriteDesc Hit = Cell(T_Spark, 1, 2, 1, 36.f, 36.f, 0.1f);
			Hit.Intensity = 2.f;
			Hit.EndScale = 0.5f;
			E.Sprites.Add(Hit);
			FSpriteDesc Puff = Smoke(40.f, 0.45f, 0.f, FLinearColor(0.5f, 0.48f, 0.45f, 1.f));
			Puff.Offset = FVector(8.f, 0.f, 0.f);
			E.Sprites.Add(Puff);
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Impact.Link");
			E.LifeTime = 0.4f;
			FSpriteDesc Splash = Flipbook(T_LinkSplash, 2, 2, 90.f, 0.3f, 1.3f);
			Splash.Intensity = 1.5f;
			E.Sprites.Add(Splash);
			E.Sprites.Add(Ring(T_LinkRing, 40.f, 0.3f, 2.5f, White, 1.5f));
			FSpriteDesc Spark = Cell(T_LinkSpark, 1, 2, 1, 50.f, 50.f, 0.15f);
			Spark.Intensity = 2.f;
			E.Sprites.Add(Spark);
			E.LightColor = LinkGreen;
			E.LightIntensity = 4000.f;
			E.LightRadius = 300.f;
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Impact.LinkBeam");
			E.LifeTime = 0.2f;
			FSpriteDesc Splash = Flipbook(T_LinkSplash, 2, 2, 60.f, 0.2f, 1.3f);
			Splash.Intensity = 1.5f;
			E.Sprites.Add(Splash);
			FSpriteDesc Spark = Cell(T_LinkSpark, 1, 2, 1, 45.f, 45.f, 0.12f);
			Spark.Intensity = 2.f;
			E.Sprites.Add(Spark);
			L.Add(E);
		}
		{
			// xEffects.BloodExplosion: a burst of pcl_Blooda splats (4x4 sheet) when a body is gibbed.
			FEffectDesc E;
			E.Name = TEXT("Gib.Blood");
			E.LifeTime = 1.1f;
			E.RefRadius = 1.f;
			const FVector Offsets[] = { FVector(0, 0, 10), FVector(14, -10, 24), FVector(-12, 12, -6), FVector(6, 16, 38), FVector(-8, -14, 52), FVector(0, 0, -20) };
			for (int32 i = 0; i < UE_ARRAY_COUNT(Offsets); ++i)
			{
				FSpriteDesc Splat = Cell(T_Blood, 4, 4, (i * 5) % 16, 70.f + 12.f * i, 70.f + 12.f * i, 0.7f + 0.07f * i);
				Splat.Blend = EBlend::Translucent;
				Splat.EndScale = 2.2f;
				Splat.FadePower = 1.3f;
				Splat.Delay = 0.03f * i;
				Splat.Offset = Offsets[i];
				E.Sprites.Add(Splat);
			}
			L.Add(E);
		}
		{
			// xEffects.BotSparks for robot gibs: spark streaks and dots.
			FEffectDesc E;
			E.Name = TEXT("Gib.Sparks");
			E.LifeTime = 0.8f;
			E.RefRadius = 1.f;
			for (int32 i = 0; i < 6; ++i)
			{
				FSpriteDesc Spark = Cell(T_BotSpark, 1, 2, i % 2, i % 2 ? 40.f : 90.f, i % 2 ? 40.f : 22.f, 0.45f + 0.06f * i);
				Spark.Intensity = 3.f;
				Spark.EndScale = 1.6f;
				Spark.Offset = FVector(FMath::Cos(i * 1.1f) * 18.f, FMath::Sin(i * 1.1f) * 18.f, 10.f * i);
				E.Sprites.Add(Spark);
			}
			E.LightColor = FLinearColor(1.f, 0.55f, 0.2f);
			E.LightIntensity = 5000.f;
			E.LightRadius = 400.f;
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Impact.Lightning");
			E.LifeTime = 0.45f;
			FSpriteDesc Charge = Flipbook(T_LightningCharge, 2, 2, 140.f, 0.35f, 1.3f);
			Charge.Intensity = 2.f;
			E.Sprites.Add(Charge);
			FSpriteDesc Dot = Cell(T_BlueSpark, 1, 2, 1, 90.f, 90.f, 0.2f);
			Dot.Intensity = 2.f;
			E.Sprites.Add(Dot);
			E.LightColor = LightningBlue;
			E.LightIntensity = 8000.f;
			E.LightRadius = 450.f;
			// LightningBolt / NewLightningBolt play WeaponSounds.LightningGun.LightningGunImpact, a SoundGroup of the two
			// ProceduralSounds in PLightningGunImpact (settings from their exported properties).
			FEffectSound P1;
			P1.Sound = TEXT("Sounds/WeaponSounds/PLightningGunImpact_P1LightningGunImpact");
			P1.CookedBase = TEXT("Sounds/WeaponSounds/BaseImpactAndExplosions_BLightningGunImpact");
			P1.PitchVariance = 5.f;
			P1.VolumeVariance = 10.f;
			E.Sounds.Add(P1);
			FEffectSound P2;
			P2.Sound = TEXT("Sounds/WeaponSounds/PLightningGunImpact_P2LightningGunImpact");
			P2.CookedBase = TEXT("Sounds/WeaponSounds/BaseImpactAndExplosions_BLightningGunExplosion");
			P2.PitchModification = -5.f;
			P2.VolumeVariance = 10.f;
			E.Sounds.Add(P2);
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Impact.Insta");
			E.LifeTime = 0.5f;
			E.Sprites.Add(Ring(T_ShockRingA, 60.f, 0.4f, 3.f, InstaRed, 2.f));
			FSpriteDesc Flare = Sprite(T_ShockFlareA, 140.f, 0.35f, 0.6f);
			Flare.Tint = InstaRed;
			Flare.Intensity = 2.f;
			E.Sprites.Add(Flare);
			E.LightColor = InstaRed;
			E.LightIntensity = 8000.f;
			E.LightRadius = 450.f;
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Impact.Shard");
			E.LifeTime = 0.2f;
			FSpriteDesc Hit = Cell(T_FlakTrail, 1, 2, 1, 30.f, 30.f, 0.15f);
			Hit.Intensity = 2.f;
			E.Sprites.Add(Hit);
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Impact.Gauntlet");
			E.LifeTime = 0.35f;
			FSpriteDesc Flash = Sprite(T_FlashWhite, 120.f, 0.15f, 1.4f);
			Flash.Tint = Orange;
			E.Sprites.Add(Flash);
			E.Sprites.Add(Ring(T_GreyRing, 40.f, 0.3f, 4.f, Orange, 1.5f));
			E.LightColor = Orange;
			E.LightIntensity = 6000.f;
			E.LightRadius = 400.f;
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Impact.Goo");
			E.LifeTime = 0.3f;
			E.Sprites.Add(Sprite(T_FlashBio, 60.f, 0.25f, 1.6f));
			L.Add(E);
		}

		// ---------------- beams (spawned along start..end) ----------------
		{
			FEffectDesc E;
			E.Name = TEXT("Beam.Shock");
			E.LifeTime = 0.5f;
			E.Sprites.Add(BeamQuad(T_ShockBeam, 26.f, 0.45f, true, White, 1.8f));
			E.Sprites.Add(BeamQuad(T_ShockBeam, 70.f, 0.3f, true, ShockPurple, 0.5f));
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Beam.ShockCombo");
			E.LifeTime = 0.6f;
			E.Sprites.Add(BeamQuad(T_ShockBeam, 40.f, 0.55f, true, White, 2.2f));
			E.Sprites.Add(BeamQuad(T_ShockBeam, 110.f, 0.4f, true, ShockPurple, 0.6f));
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Beam.Insta");
			E.LifeTime = 0.65f;
			E.Sprites.Add(BeamQuad(T_ShockBeamRed, 30.f, 0.6f, true, White, 2.2f));
			E.Sprites.Add(BeamQuad(T_ShockBeamRed, 80.f, 0.4f, true, InstaRed, 0.6f));
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Beam.Lightning");
			E.LifeTime = 0.45f;
			FSpriteDesc Bolt = BeamQuad(T_LightningBolt, 70.f, 0.4f, false, White, 2.f);
			Bolt.Columns = 1;
			Bolt.Rows = 2;
			Bolt.Frames = 1;
			Bolt.EndScale = 1.f;
			E.Sprites.Add(Bolt);
			FSpriteDesc Bolt2 = Bolt;
			Bolt2.Size = FVector2D(0.f, 45.f);
			Bolt2.LifeTime = 0.25f;
			Bolt2.Tint = LightningBlue;
			E.Sprites.Add(Bolt2);
			FSpriteDesc Core = BeamQuad(T_BlueSpark, 14.f, 0.3f, false, White, 1.5f);
			Core.Columns = 1;
			Core.Rows = 2;
			Core.Frames = 1;
			E.Sprites.Add(Core);
			L.Add(E);
		}
		{
			FEffectDesc E;
			E.Name = TEXT("Tracer.Bullet");
			E.LifeTime = 0.12f;
			FSpriteDesc Streak = BeamQuad(T_Tracer, 9.f, 0.1f, false, FLinearColor(1.f, 0.85f, 0.6f), 2.f);
			Streak.Columns = 1;
			Streak.Rows = 2;
			Streak.Frames = 1;
			Streak.Size = FVector2D(220.f, 9.f);
			Streak.bTravel = true;
			Streak.EndScale = 1.f;
			Streak.FadePower = 0.f;
			E.Sprites.Add(Streak);
			L.Add(E);
		}
		{
			// Continuous beam pieces used by the live beam (not spawned as an effect).
			FEffectDesc E;
			E.Name = TEXT("Beam.Link");
			E.LifeTime = 0.f;
			FSpriteDesc Outer = BeamQuad(T_LinkBeam, 34.f, 0.f, true, White, 1.6f);
			Outer.TileLength = 160.f;
			Outer.Pan = -3.f;
			Outer.EndScale = 1.f;
			E.Sprites.Add(Outer);
			FSpriteDesc Inner = Outer;
			Inner.Size = FVector2D(0.f, 14.f);
			Inner.TileLength = 90.f;
			Inner.Pan = -5.f;
			Inner.Intensity = 2.f;
			E.Sprites.Add(Inner);
			L.Add(E);
		}

		// ---------------- muzzle flashes (attached; Scale ~ 1, first-person sizes) ----------------
		L.Add(Muzzle(TEXT("Muzzle.Gauntlet"), T_FlashWhite, 40.f, Orange, Orange));
		L.Add(Muzzle(TEXT("Muzzle.Carbine"), T_FlashA, 34.f, FLinearColor(1.f, 0.8f, 0.4f)));
		L.Add(Muzzle(TEXT("Muzzle.Minigun"), T_FlashA, 30.f, FLinearColor(1.f, 0.8f, 0.4f)));
		L.Add(Muzzle(TEXT("Muzzle.Goo"), T_FlashBio, 36.f, LinkGreen));
		L.Add(Muzzle(TEXT("Muzzle.Link"), T_FlashLink, 36.f, LinkGreen));
		L.Add(Muzzle(TEXT("Muzzle.Flak"), T_FlashFlak, 46.f, Orange));
		L.Add(Muzzle(TEXT("Muzzle.Rocket"), T_FlashRocket, 46.f, Orange));
		L.Add(Muzzle(TEXT("Muzzle.Lightning"), T_FlashWhite, 30.f, LightningBlue, LightningBlue));
		{
			FEffectDesc E = Muzzle(TEXT("Muzzle.Shock"), T_FlashShock, 40.f, ShockPurple);
			FSpriteDesc Flare = MuzzleSprite(T_ShockFlareB, 26.f);
			Flare.LifeTime = 0.12f;
			E.Sprites.Add(Flare);
			E.LifeTime = 0.13f;
			L.Add(E);
		}
		L.Add(Muzzle(TEXT("Muzzle.Insta"), T_FlashShock, 44.f, InstaRed, InstaRed));
		return L;
	}

	TArray<FProjectileArt> BuildProjectiles()
	{
		TArray<FProjectileArt> L;
		{
			FProjectileArt P;
			P.Style = TEXT("Rocket");
			P.RefSize = 9.f;
			FMeshDesc M;
			M.Mesh = TEXT("StaticMeshes/WeaponStaticMesh/RocketProj");
			M.Size = 46.f;
			M.Spin = 270.f;
			P.Meshes.Add(M);
			FSpriteDesc Flare = Sprite(T_RocketFlare, 70.f, 0.f);
			Flare.Size = FVector2D(80.f, 40.f);
			Flare.Offset = FVector(-26.f, 0.f, 0.f);
			Flare.bRandomRoll = false;
			Flare.Intensity = 1.5f;
			P.Sprites.Add(Flare);
			FSpriteDesc Trail = Sprite(T_TrailBlur, 0.f, 0.f);
			Trail.Facing = EFacing::Trail;
			Trail.Blend = EBlend::Translucent;
			Trail.Size = FVector2D(220.f, 34.f);
			Trail.Offset = FVector(-28.f, 0.f, 0.f);
			Trail.Tint = FLinearColor(0.75f, 0.72f, 0.7f, 1.f);
			Trail.Intensity = 0.8f;
			P.Sprites.Add(Trail);
			P.Explosion = TEXT("Explosion.Rocket");
			P.Impact = TEXT("Explosion.Rocket");
			P.LightScale = 1.f;
			L.Add(P);
		}
		{
			FProjectileArt P;
			P.Style = TEXT("Grenade");
			P.RefSize = 8.f;
			FMeshDesc M;
			M.Mesh = TEXT("StaticMeshes/WeaponStaticMesh/GrenadeMesh");
			M.Size = 20.f;
			M.Spin = 540.f;
			P.Meshes.Add(M);
			FSpriteDesc Glow = Cell(T_Spark, 1, 2, 1, 26.f, 26.f, 0.f);
			Glow.Intensity = 1.2f;
			P.Sprites.Add(Glow);
			P.Explosion = TEXT("Explosion.Grenade");
			P.Impact = TEXT("Explosion.Grenade");
			P.LightScale = 0.4f;
			L.Add(P);
		}
		{
			FProjectileArt P;
			P.Style = TEXT("FlakChunk");
			P.RefSize = 4.f;
			FMeshDesc M;
			M.Mesh = TEXT("StaticMeshes/WeaponStaticMesh/FlakChunk");
			M.Size = 9.f;
			M.Spin = 720.f;
			P.Meshes.Add(M);
			FSpriteDesc Trail = Cell(T_FlakTrail, 1, 2, 0, 70.f, 12.f, 0.f);
			Trail.Facing = EFacing::Trail;
			Trail.Intensity = 1.6f;
			P.Sprites.Add(Trail);
			FSpriteDesc Glow = Cell(T_FlakTrail, 1, 2, 1, 22.f, 22.f, 0.f);
			Glow.Intensity = 1.6f;
			P.Sprites.Add(Glow);
			P.Impact = TEXT("Impact.Shard");
			P.LightScale = 0.6f;
			L.Add(P);
		}
		{
			FProjectileArt P;
			P.Style = TEXT("FlakShell");
			P.RefSize = 11.f;
			FMeshDesc M;
			M.Mesh = TEXT("StaticMeshes/WeaponStaticMesh/FlakShell");
			M.Size = 24.f;
			M.Spin = 400.f;
			P.Meshes.Add(M);
			FSpriteDesc Trail = Cell(T_FlakTrail, 1, 2, 0, 110.f, 24.f, 0.f);
			Trail.Facing = EFacing::Trail;
			Trail.Intensity = 1.4f;
			P.Sprites.Add(Trail);
			FSpriteDesc Glow = Cell(T_FlakTrail, 1, 2, 1, 40.f, 40.f, 0.f);
			Glow.Intensity = 1.4f;
			P.Sprites.Add(Glow);
			P.Explosion = TEXT("Explosion.FlakShell");
			P.Impact = TEXT("Explosion.FlakShell");
			L.Add(P);
		}
		{
			FProjectileArt P;
			P.Style = TEXT("Goo");
			P.RefSize = 10.f;
			P.SkelMesh = TEXT("SkeletalMeshes/XWeapons_rc/GoopMesh");
			P.SkelMaterial = TEXT("Materials/XEffectMat/goop_GoopShader");
			P.FlyAnim = TEXT("Animations/XWeapons_rc/GoopMesh_Flying");
			P.StuckAnim = TEXT("Animations/XWeapons_rc/GoopMesh_Drip");
			P.SkelSize = 30.f;
			FMeshDesc M; // static fallback when the vertex-animated blob is missing
			M.Mesh = TEXT("StaticMeshes/AW-2004Particles/Weapons_AcidBlob");
			M.Size = 26.f;
			P.Meshes.Add(M);
			P.Explosion = TEXT("Explosion.Goo");
			P.Impact = TEXT("Explosion.Goo");
			P.LightScale = 0.5f;
			L.Add(P);
		}
		{
			FProjectileArt P;
			P.Style = TEXT("PulseCore");
			P.RefSize = 16.f;
			FSpriteDesc Core = Sprite(T_ShockCoreLow, 70.f, 0.f);
			Core.Intensity = 1.6f;
			Core.Spin = 90.f;
			P.Sprites.Add(Core);
			FSpriteDesc Flare = Sprite(T_ShockFlareA, 95.f, 0.f);
			Flare.Spin = -140.f;
			Flare.Intensity = 1.2f;
			P.Sprites.Add(Flare);
			P.Impact = TEXT("Impact.ShockBall");
			P.Explosion = TEXT("Impact.ShockBall");
			L.Add(P);
		}
		{
			FProjectileArt P;
			P.Style = TEXT("LinkBolt");
			P.RefSize = 7.f;
			FMeshDesc M;
			M.Mesh = TEXT("StaticMeshes/WeaponStaticMesh/LinkProjectile");
			M.Size = 56.f;
			P.Meshes.Add(M);
			FSpriteDesc Glow = Sprite(T_LinkMuz, 50.f, 0.f);
			Glow.Intensity = 1.2f;
			Glow.Spin = 200.f;
			P.Sprites.Add(Glow);
			P.Impact = TEXT("Impact.Link");
			P.Explosion = TEXT("Impact.Link");
			P.LightScale = 0.8f;
			L.Add(P);
		}
		// Invasion monster projectiles (SkaarjPack): KrallBolt / EliteKrallBolt (link bolt, yellow / blue), the Skaarj
		// energy balls (link_muz_green, IceSkaarj blue, FireSkaarj red) and the Gasbag's belched fireball.
		auto Bolt = [&L](const TCHAR* Style, const TCHAR* Texture, float Size, const TCHAR* Impact, const TCHAR* Explosion)
		{
			FProjectileArt P;
			P.Style = Style;
			P.RefSize = 10.f;
			FSpriteDesc Core = Sprite(Texture, Size, 0.f);
			Core.Intensity = 1.6f;
			Core.Spin = 240.f;
			P.Sprites.Add(Core);
			FSpriteDesc Halo = Sprite(Texture, Size * 1.7f, 0.f);
			Halo.Intensity = 0.6f;
			Halo.Spin = -120.f;
			P.Sprites.Add(Halo);
			P.Impact = Impact;
			P.Explosion = Explosion;
			P.LightScale = 0.8f;
			L.Add(P);
		};
		Bolt(TEXT("KrallBolt"), T_LinkMuzYellow, 60.f, TEXT("Impact.Link"), TEXT("Impact.Link"));
		Bolt(TEXT("EliteKrallBolt"), T_LinkMuzBlue, 60.f, TEXT("Impact.ShockBall"), TEXT("Impact.ShockBall"));
		Bolt(TEXT("SkaarjBolt"), T_LinkMuzGreen, 70.f, TEXT("Impact.Link"), TEXT("Impact.Link"));
		Bolt(TEXT("IceSkaarjBolt"), T_LinkMuzBlue, 70.f, TEXT("Impact.ShockBall"), TEXT("Impact.ShockBall"));
		Bolt(TEXT("FireSkaarjBolt"), T_LinkMuzRed, 80.f, TEXT("Explosion.Grenade"), TEXT("Explosion.Grenade"));
		{
			FProjectileArt P;
			P.Style = TEXT("GasbagBelch");
			P.RefSize = 22.f;
			FSpriteDesc Glow = Cell(T_FlakTrail, 1, 2, 1, 90.f, 90.f, 0.f);
			Glow.Intensity = 1.8f;
			Glow.Spin = 90.f;
			P.Sprites.Add(Glow);
			FSpriteDesc Fire = Flipbook(T_Exp2, 4, 4, 70.f, 0.6f);
			Fire.FrameRate = 16.f;
			Fire.bLoop = true;
			P.Sprites.Add(Fire);
			P.Impact = TEXT("Explosion.FlakShell");
			P.Explosion = TEXT("Explosion.FlakShell");
			P.LightScale = 1.2f;
			L.Add(P);
		}
		return L;
	}

	TArray<FWeaponFX> BuildWeaponFX()
	{
		auto W = [](const TCHAR* Class, uint8 Mode, const TCHAR* Muzzle, const TCHAR* Beam, const TCHAR* Impact, float MuzzleScale = 1.f)
		{
			FWeaponFX F;
			F.ClassName = Class;
			F.Mode = Mode;
			F.Muzzle = Muzzle;
			F.Beam = Beam;
			F.Impact = Impact;
			F.MuzzleScale = MuzzleScale;
			return F;
		};
		return {
			W(TEXT("Weap_Gauntlet"), 0, TEXT("Muzzle.Gauntlet"), nullptr, TEXT("Impact.Gauntlet")),
			W(TEXT("Weap_Carbine"), 0, TEXT("Muzzle.Carbine"), TEXT("Tracer.Bullet"), TEXT("Impact.Bullet")),
			W(TEXT("Weap_Carbine"), 1, TEXT("Muzzle.Carbine"), nullptr, nullptr, 1.4f),
			W(TEXT("Weap_GooLauncher"), 0, TEXT("Muzzle.Goo"), nullptr, nullptr),
			W(TEXT("Weap_GooLauncher"), 1, TEXT("Muzzle.Goo"), nullptr, nullptr, 1.4f),
			W(TEXT("Weap_PulseRifle"), 0, TEXT("Muzzle.Shock"), TEXT("Beam.Shock"), TEXT("Impact.ShockBeam")),
			W(TEXT("Weap_PulseRifle"), 1, TEXT("Muzzle.Shock"), nullptr, nullptr),
			W(TEXT("Weap_ArcLinker"), 0, TEXT("Muzzle.Link"), nullptr, nullptr),
			W(TEXT("Weap_ArcLinker"), 1, TEXT("Muzzle.Link"), TEXT("Beam.Link"), TEXT("Impact.LinkBeam")),
			W(TEXT("Weap_RotaryGun"), 0, TEXT("Muzzle.Minigun"), TEXT("Tracer.Bullet"), TEXT("Impact.Bullet")),
			W(TEXT("Weap_RotaryGun"), 1, TEXT("Muzzle.Minigun"), TEXT("Tracer.Bullet"), TEXT("Impact.Bullet")),
			W(TEXT("Weap_ShardCannon"), 0, TEXT("Muzzle.Flak"), nullptr, nullptr),
			W(TEXT("Weap_ShardCannon"), 1, TEXT("Muzzle.Flak"), nullptr, nullptr),
			W(TEXT("Weap_RocketLauncher"), 0, TEXT("Muzzle.Rocket"), nullptr, nullptr),
			W(TEXT("Weap_RocketLauncher"), 1, TEXT("Muzzle.Rocket"), nullptr, nullptr),
			W(TEXT("Weap_RailRifle"), 0, TEXT("Muzzle.Lightning"), TEXT("Beam.Lightning"), TEXT("Impact.Lightning")),
			W(TEXT("Weap_InstaRifle"), 0, TEXT("Muzzle.Insta"), TEXT("Beam.Insta"), TEXT("Impact.Insta")),
			W(TEXT("Weap_InstaRifle"), 1, TEXT("Muzzle.Insta"), TEXT("Beam.Insta"), TEXT("Impact.Insta")),
		};
	}

	const TArray<FEffectDesc>& Effects()
	{
		static const TArray<FEffectDesc> Table = BuildEffects();
		return Table;
	}

	const TArray<FProjectileArt>& Projectiles()
	{
		static const TArray<FProjectileArt> Table = BuildProjectiles();
		return Table;
	}

	const TArray<FWeaponFX>& WeaponFX()
	{
		static const TArray<FWeaponFX> Table = BuildWeaponFX();
		return Table;
	}

	template <typename T>
	UObject* LoadEngineObject(const TCHAR* Path, TStrongObjectPtr<UObject>& Cache, bool& bTried)
	{
		if (!bTried)
		{
			bTried = true;
			Cache.Reset(LoadObject<T>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet));
		}
		return Cache.Get();
	}

	UMaterialInterface* SpriteParent(EBlend Blend)
	{
		static TStrongObjectPtr<UObject> Cache[2];
		static bool bTried[2] = { false, false };
		const int32 I = Blend == EBlend::Additive ? 0 : 1;
		const TCHAR* Path = I == 0 ? TEXT("/Game/Materials/M_ArenaSprite.M_ArenaSprite") : TEXT("/Game/Materials/M_ArenaSpriteAlpha.M_ArenaSpriteAlpha");
		return Cast<UMaterialInterface>(LoadEngineObject<UMaterialInterface>(Path, Cache[I], bTried[I]));
	}
}

// =================================================================================================
// Lookups
// =================================================================================================

const ArenaArtFX::FEffectDesc* ArenaArtFX::FindEffect(FName Name)
{
	if (Name.IsNone())
	{
		return nullptr;
	}
	for (const FEffectDesc& E : ArenaArtFXPrivate::Effects())
	{
		if (Name == E.Name)
		{
			return &E;
		}
	}
	return nullptr;
}

const ArenaArtFX::FProjectileArt* ArenaArtFX::FindProjectileArt(FName Style)
{
	if (Style.IsNone() || !ArenaArt::IsEnabled())
	{
		return nullptr;
	}
	for (const FProjectileArt& P : ArenaArtFXPrivate::Projectiles())
	{
		if (Style == P.Style)
		{
			return &P;
		}
	}
	return nullptr;
}

const ArenaArtFX::FWeaponFX* ArenaArtFX::FindWeaponFX(const UClass* WeaponClass, uint8 Mode)
{
	if (!ArenaArt::IsEnabled())
	{
		return nullptr;
	}
	for (const UClass* C = WeaponClass; C; C = C->GetSuperClass())
	{
		const FString Name = C->GetName();
		for (const FWeaponFX& F : ArenaArtFXPrivate::WeaponFX())
		{
			if (F.Mode == Mode && Name == F.ClassName)
			{
				return &F;
			}
		}
	}
	return nullptr;
}

// =================================================================================================
// Materials and meshes
// =================================================================================================

UStaticMesh* ArenaArtFX::QuadMesh()
{
	static TStrongObjectPtr<UObject> Cache;
	static bool bTried = false;
	return Cast<UStaticMesh>(ArenaArtFXPrivate::LoadEngineObject<UStaticMesh>(TEXT("/Engine/BasicShapes/Plane.Plane"), Cache, bTried));
}

bool ArenaArtFX::IsSpriteMaterial(const UMaterialInterface* Material)
{
	if (!Material)
	{
		return false;
	}
	const UMaterialInterface* Base = const_cast<UMaterialInterface*>(Material)->GetBaseMaterial();
	return Base && (Base == ArenaArtFXPrivate::SpriteParent(EBlend::Additive) || Base == ArenaArtFXPrivate::SpriteParent(EBlend::Translucent));
}

UMaterialInstanceDynamic* ArenaArtFX::MakeMaterial(UObject* Outer, const TCHAR* Texture, EBlend Blend, const FLinearColor& Tint, float Intensity)
{
	if (!Texture || !ArenaArt::IsEnabled())
	{
		return nullptr;
	}
	UTexture* Tex = ArenaArt::Load<UTexture>(Texture);
	if (!Tex)
	{
		return nullptr;
	}
	UMaterialInterface* Parent = ArenaArtFXPrivate::SpriteParent(Blend);
	if (!Parent)
	{
		Parent = ArenaArt::Load<UMaterialInterface>(Blend == EBlend::Additive ? TEXT("Materials/Masters/M_ImportedAdditive") : TEXT("Materials/Masters/M_ImportedTranslucent"));
	}
	if (!Parent)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, Outer);
	if (MID)
	{
		MID->SetTextureParameterValue(TEXT("Base"), Tex);
		MID->SetVectorParameterValue(TEXT("Tint"), Tint);
		MID->SetScalarParameterValue(TEXT("Intensity"), Intensity);
	}
	return MID;
}

void ArenaArtFX::FitMesh(UStaticMeshComponent* Comp, float Size, const FRotator& Rotation, const FVector& Offset, bool bCenter)
{
	const UStaticMesh* Mesh = Comp ? Comp->GetStaticMesh() : nullptr;
	if (!Mesh)
	{
		return;
	}
	const FBoxSphereBounds B = Mesh->GetBounds();
	const float MaxDim = FMath::Max(1.f, float(B.BoxExtent.GetMax()) * 2.f);
	const float S = Size / MaxDim;
	const FVector Pivot = bCenter ? FVector(B.Origin) : FVector::ZeroVector;
	Comp->SetRelativeScale3D(FVector(S));
	Comp->SetRelativeRotation(Rotation);
	Comp->SetRelativeLocation(Offset - Rotation.RotateVector(Pivot * S));
}

UStaticMeshComponent* ArenaArtFX::AddMesh(AActor* Owner, USceneComponent* Parent, const FMeshDesc& Desc, float Scale)
{
	UStaticMesh* Mesh = Desc.Mesh ? ArenaArt::Load<UStaticMesh>(Desc.Mesh) : nullptr;
	if (!Owner || !Parent || !Mesh)
	{
		return nullptr;
	}
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Owner);
	C->SetMobility(EComponentMobility::Movable);
	C->SetStaticMesh(Mesh);
	C->SetupAttachment(Parent);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	C->SetGenerateOverlapEvents(false);
	C->SetCanEverAffectNavigation(false);
	C->SetCastShadow(false);
	C->SetReceivesDecals(false);
	FitMesh(C, Desc.Size * Scale, Desc.Rotation, Desc.Offset * Scale, Desc.bCenter);
	C->RegisterComponent();
	return C;
}

bool ArenaArtFX::GetViewLocation(const UWorld* World, FVector& OutLocation)
{
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (PC && PC->PlayerCameraManager)
	{
		OutLocation = PC->PlayerCameraManager->GetCameraCacheView().Location;
		return true;
	}
	return false;
}

// =================================================================================================
// Spawning
// =================================================================================================

namespace ArenaArtFXPrivate
{
	AArenaFXActor* SpawnEffectActor(UWorld* World, const FEffectDesc& E, const FVector& Location, const FRotator& Rotation, float Scale,
		const FLinearColor& Tint, const FVector* BeamStart, const FVector* BeamEnd, USceneComponent* AttachTo, const FVector& RelOffset,
		bool bOnlyOwnerSee, bool bOwnerNoSee, bool bFirstPerson)
	{
		if (!World || World->bIsTearingDown || !ArenaArt::IsEnabled())
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		Params.Owner = AttachTo ? AttachTo->GetOwner() : nullptr;
		AArenaFXActor* FX = World->SpawnActor<AArenaFXActor>(AArenaFXActor::StaticClass(), Location, Rotation, Params);
		if (!FX)
		{
			return nullptr;
		}
		if (AttachTo)
		{
			FX->AttachToComponent(AttachTo, FAttachmentTransformRules::KeepWorldTransform);
			FX->SetActorRelativeLocation(RelOffset);
			FX->SetActorRelativeRotation(FRotator::ZeroRotator);
		}

		float Life = E.LifeTime;
		for (const FSpriteDesc& S : E.Sprites)
		{
			Life = FMath::Max(Life, S.Delay + S.LifeTime);
		}
		const float LightScale = FMath::Clamp(Scale, 0.3f, 3.f);
		FX->InitFX(nullptr, E.LightColor * Tint, FMath::Max(0.05f, Life), FVector::OneVector, FVector::OneVector, 0.f,
			E.LightIntensity * LightScale, E.LightRadius * LightScale, bOnlyOwnerSee);

		int32 Built = 0;
		for (const FSpriteDesc& S : E.Sprites)
		{
			FSpriteDesc D = S;
			if (D.LifeTime <= 0.f)
			{
				D.LifeTime = Life;
			}
			UArenaSpriteComponent* C = NewObject<UArenaSpriteComponent>(FX);
			C->SetupAttachment(FX->Root);
			if (!C->SetupSprite(D, Scale, Tint))
			{
				C->DestroyComponent();
				continue;
			}
			C->SetOnlyOwnerSee(bOnlyOwnerSee);
			C->SetOwnerNoSee(bOwnerNoSee);
			if (bFirstPerson)
			{
				C->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
			}
			if (BeamStart && BeamEnd)
			{
				C->SetBeam(*BeamStart, *BeamEnd);
			}
			C->RegisterComponent();
			++Built;
		}
		for (const FMeshDesc& M : E.Meshes)
		{
			if (UStaticMeshComponent* C = AddMesh(FX, FX->Root, M, Scale))
			{
				C->SetOnlyOwnerSee(bOnlyOwnerSee);
				C->SetOwnerNoSee(bOwnerNoSee);
				if (bFirstPerson)
				{
					C->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
				}
				++Built;
			}
		}
		if (Built == 0)
		{
			FX->Destroy();
			return nullptr;
		}
		return FX;
	}
}

AArenaFXActor* ArenaArtFX::SpawnEffect(UWorld* World, FName Name, const FVector& Location, const FRotator& Rotation, float Scale, const FLinearColor& Tint)
{
	const FEffectDesc* E = FindEffect(Name);
	if (E && World && E->Sounds.Num() > 0 && ArenaArt::IsEnabled())
	{
		const FEffectSound& S = E->Sounds[FMath::RandRange(0, E->Sounds.Num() - 1)];
		if (USoundBase* Sound = S.Sound ? ArenaArt::LoadOneShotSound(S.Sound) : nullptr)
		{
			UGameplayStatics::PlaySoundAtLocation(World, Sound, Location);
		}
		else if (USoundBase* Base = S.CookedBase ? ArenaArt::LoadOneShotSound(S.CookedBase) : nullptr)
		{
			// UE2 UProceduralSound: modification +- a random share of the variance, percent
			const float Pitch = (100.f + S.PitchModification + FMath::FRandRange(-1.f, 1.f) * FMath::Abs(S.PitchVariance)) / 100.f;
			const float Volume = (100.f + S.VolumeModification + FMath::FRandRange(-1.f, 1.f) * FMath::Abs(S.VolumeVariance)) / 100.f;
			UGameplayStatics::PlaySoundAtLocation(World, Base, Location, Volume, Pitch);
		}
	}
	return E ? ArenaArtFXPrivate::SpawnEffectActor(World, *E, Location, Rotation, Scale, Tint, nullptr, nullptr, nullptr, FVector::ZeroVector, false, false, false) : nullptr;
}

AArenaFXActor* ArenaArtFX::SpawnBeamEffect(UWorld* World, FName Name, const FVector& Start, const FVector& End, float Scale)
{
	const FEffectDesc* E = FindEffect(Name);
	if (!E || FVector::DistSquared(Start, End) < 1.f)
	{
		return nullptr;
	}
	return ArenaArtFXPrivate::SpawnEffectActor(World, *E, End, (End - Start).Rotation(), Scale, FLinearColor::White, &Start, &End, nullptr, FVector::ZeroVector, false, false, false);
}

AArenaFXActor* ArenaArtFX::SpawnAttachedEffect(USceneComponent* AttachTo, FName Name, const FVector& RelativeOffset, float Scale, bool bOnlyOwnerSee, bool bOwnerNoSee, bool bFirstPerson)
{
	const FEffectDesc* E = FindEffect(Name);
	if (!E || !AttachTo)
	{
		return nullptr;
	}
	const FTransform T = AttachTo->GetComponentTransform();
	return ArenaArtFXPrivate::SpawnEffectActor(AttachTo->GetWorld(), *E, T.TransformPosition(RelativeOffset), T.Rotator(), Scale, FLinearColor::White,
		nullptr, nullptr, AttachTo, RelativeOffset, bOnlyOwnerSee, bOwnerNoSee, bFirstPerson);
}

// =================================================================================================
// UArenaSpriteComponent
// =================================================================================================

UArenaSpriteComponent::UArenaSpriteComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	bTickInEditor = false;
	Mobility = EComponentMobility::Movable;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	CastShadow = false;
	bReceivesDecals = false;
	bSelectable = false;
	SetComponentTickEnabled(true);
}

bool UArenaSpriteComponent::SetupSprite(const ArenaArtFX::FSpriteDesc& InDesc, float InScale, const FLinearColor& TintMul)
{
	using namespace ArenaArtFX;
	UStaticMesh* Quad = QuadMesh();
	if (!Quad)
	{
		return false;
	}
	UMaterialInstanceDynamic* MID = MakeMaterial(this, InDesc.Texture, InDesc.Blend, InDesc.Tint * TintMul, InDesc.Intensity);
	if (!MID)
	{
		return false;
	}
	bSpriteMaterial = IsSpriteMaterial(MID);
	const bool bFlipbook = InDesc.Columns * InDesc.Rows > 1;
	if (bFlipbook && !bSpriteMaterial)
	{
		return false; // the imported masters cannot pick a cell out of a sheet
	}

	Desc = InDesc;
	Desc.Tint = InDesc.Tint * TintMul;
	Scale = FMath::Max(0.01f, InScale);
	BaseIntensity = Desc.Intensity;
	Age = 0.f;
	Roll = Desc.bRandomRoll ? FMath::FRandRange(0.f, 360.f) : 0.f;
	SpriteMaterial = MID;
	SetStaticMesh(Quad);
	SetMaterial(0, MID);

	if (bSpriteMaterial)
	{
		MID->SetScalarParameterValue(TEXT("Columns"), float(FMath::Max(1, Desc.Columns)));
		MID->SetScalarParameterValue(TEXT("Rows"), float(FMath::Max(1, Desc.Rows)));
		MID->SetScalarParameterValue(TEXT("Frame"), float(Desc.StartFrame));
		if (Desc.Pan != 0.f)
		{
			MID->SetScalarParameterValue(Desc.bVerticalTexture ? TEXT("PanV") : TEXT("PanU"), Desc.Pan);
		}
	}
	LastFrame = -1.f;
	LastFade = -1.f;
	bConfigured = true;
	SetRelativeLocation(Desc.Offset * Scale);
	if (IsRegistered())
	{
		UpdateSprite(0.f);
	}
	return true;
}

void UArenaSpriteComponent::OnRegister()
{
	Super::OnRegister();
	if (bConfigured)
	{
		UpdateSprite(0.f);
	}
}

void UArenaSpriteComponent::SetBeam(const FVector& InStart, const FVector& InEnd)
{
	BeamStart = InStart;
	BeamEnd = InEnd;
	if (bConfigured && IsRegistered())
	{
		UpdateSprite(0.f);
	}
}

void UArenaSpriteComponent::SetIntensityScale(float InScale)
{
	IntensityScale = InScale;
	LastFade = -1.f;
}

void UArenaSpriteComponent::SetTint(const FLinearColor& InTint)
{
	Desc.Tint = InTint;
	if (SpriteMaterial)
	{
		SpriteMaterial->SetVectorParameterValue(TEXT("Tint"), InTint);
	}
}

void UArenaSpriteComponent::Restart()
{
	Age = 0.f;
	SetVisibility(true);
	SetComponentTickEnabled(true);
	LastFade = -1.f;
}

void UArenaSpriteComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (bConfigured)
	{
		UpdateSprite(DeltaTime);
	}
}

void UArenaSpriteComponent::UpdateSprite(float DeltaTime)
{
	using namespace ArenaArtFX;
	Age += DeltaTime;
	const float T = Age - Desc.Delay;
	if (T < 0.f)
	{
		if (IsVisible())
		{
			SetVisibility(false);
		}
		return;
	}
	const float Life = Desc.LifeTime;
	if (Desc.bLoop && Life > 0.f && T >= Life)
	{
		Age = Desc.Delay + FMath::Fmod(T, Life);
		LastFade = -1.f;
		UpdateSprite(0.f);
		return;
	}
	if (Life > 0.f && T >= Life)
	{
		SetVisibility(false);
		SetComponentTickEnabled(false);
		return;
	}
	if (!IsVisible())
	{
		SetVisibility(true);
	}

	const float A = Life > 0.f ? FMath::Clamp(T / Life, 0.f, 1.f) : 0.f;
	float Fade = 1.f;
	if (Life > 0.f && Desc.FadePower > 0.f)
	{
		Fade = FMath::Pow(1.f - A, Desc.FadePower);
	}
	if (Desc.FadeIn > 0.f)
	{
		Fade *= FMath::Clamp(T / Desc.FadeIn, 0.f, 1.f);
	}
	Fade *= IntensityScale;

	const float Grow = 1.f - FMath::Square(1.f - A);
	const float SizeMul = FMath::Lerp(1.f, Desc.EndScale, Grow) * Scale;
	const float W = FMath::Max(0.01f, Desc.Size.X * SizeMul);
	const float H = FMath::Max(0.01f, Desc.Size.Y * SizeMul * WidthScale);
	Roll = FMath::Fmod(Roll + Desc.Spin * DeltaTime, 360.f);

	// Material: fade and flipbook frame
	if (SpriteMaterial)
	{
		if (FMath::Abs(Fade - LastFade) > 0.004f)
		{
			LastFade = Fade;
			if (bSpriteMaterial || Desc.Blend == EBlend::Translucent)
			{
				SpriteMaterial->SetScalarParameterValue(TEXT("Opacity"), Fade);
			}
			else
			{
				SpriteMaterial->SetScalarParameterValue(TEXT("Intensity"), BaseIntensity * Fade);
			}
		}
		const int32 Cells = FMath::Max(1, Desc.Columns) * FMath::Max(1, Desc.Rows);
		if (bSpriteMaterial && Cells > 1)
		{
			const int32 N = Desc.Frames > 0 ? Desc.Frames : Cells;
			float F;
			if (Desc.FrameRate > 0.f)
			{
				F = FMath::Fmod(T * Desc.FrameRate, float(N));
			}
			else
			{
				F = FMath::Min(A * N, float(N) - 0.01f);
			}
			F = float(Desc.StartFrame) + FMath::FloorToFloat(F);
			if (F != LastFrame)
			{
				LastFrame = F;
				SpriteMaterial->SetScalarParameterValue(TEXT("Frame"), F);
			}
		}
	}

	FVector View;
	const bool bView = GetViewLocation(GetWorld(), View);

	auto PlaceAlong = [this, &View, bView, W, H](const FVector& S, const FVector& E, float Width)
	{
		const FVector Axis = E - S;
		const float Len = Axis.Size();
		if (Len < 0.5f)
		{
			SetWorldScale3D(FVector(0.001f));
			return;
		}
		const FVector Dir = Axis / Len;
		const FVector Mid = (S + E) * 0.5f;
		FVector ToView = bView ? (View - Mid) : FVector::UpVector;
		FVector N = ToView - Dir * FVector::DotProduct(ToView, Dir);
		if (!N.Normalize())
		{
			N = FMath::Abs(Dir.Z) < 0.9f ? FVector::UpVector : FVector::ForwardVector;
			N = (N - Dir * FVector::DotProduct(N, Dir)).GetSafeNormal();
		}
		FMatrix M;
		FVector ScaleV;
		if (Desc.bVerticalTexture)
		{
			// quad Y along the beam
			const FVector X = FVector::CrossProduct(Dir, N);
			M = FMatrix(X, Dir, N, FVector::ZeroVector);
			ScaleV = FVector(Width / 100.f, Len / 100.f, 1.f);
		}
		else
		{
			const FVector Y = FVector::CrossProduct(N, Dir);
			M = FMatrix(Dir, Y, N, FVector::ZeroVector);
			ScaleV = FVector(Len / 100.f, Width / 100.f, 1.f);
		}
		SetWorldLocationAndRotation(Mid, M.ToQuat());
		SetWorldScale3D(ScaleV);
		if (bSpriteMaterial && Desc.TileLength > 0.f)
		{
			SpriteMaterial->SetScalarParameterValue(Desc.bVerticalTexture ? TEXT("TileV") : TEXT("TileU"), FMath::Max(1.f, Len / (Desc.TileLength * Scale)));
		}
	};

	switch (Desc.Facing)
	{
	case EFacing::Camera:
	{
		const FVector Loc = GetComponentLocation();
		const FVector Dir = bView ? (View - Loc).GetSafeNormal() : FVector::UpVector;
		if (!Dir.IsNearlyZero())
		{
			const FQuat Base = FRotationMatrix::MakeFromZX(Dir, FMath::Abs(Dir.Z) < 0.95f ? FVector::UpVector : FVector::ForwardVector).ToQuat();
			// MakeFromZX keeps Z exactly and X as close to the hint as possible; that X is the quad's "up".
			const FQuat Spin(Dir, FMath::DegreesToRadians(Roll));
			SetWorldRotation(Spin * Base);
		}
		SetWorldScale3D(FVector(W / 100.f, H / 100.f, 1.f));
		break;
	}
	case EFacing::Beam:
	{
		FVector S = BeamStart;
		FVector E = BeamEnd;
		if (Desc.bTravel)
		{
			const FVector D = BeamEnd - BeamStart;
			const float Len = D.Size();
			if (Len > 1.f)
			{
				const FVector Dir = D / Len;
				const float Head = FMath::Min(Len, A * Len + W);
				const float Tail = FMath::Max(0.f, Head - W);
				S = BeamStart + Dir * Tail;
				E = BeamStart + Dir * Head;
			}
		}
		PlaceAlong(S, E, H);
		break;
	}
	case EFacing::Trail:
	{
		const USceneComponent* Parent = GetAttachParent();
		if (Parent)
		{
			const FTransform PT = Parent->GetComponentTransform();
			const FVector Fwd = PT.GetUnitAxis(EAxis::X);
			const FVector Start = PT.GetLocation() + PT.GetRotation().RotateVector(Desc.Offset * Scale);
			PlaceAlong(Start, Start - Fwd * W, H);
		}
		break;
	}
	case EFacing::Fixed:
	default:
	{
		const FQuat Rel = Desc.Rotation.Quaternion() * FQuat(FVector::UpVector, FMath::DegreesToRadians(Roll));
		SetRelativeRotation(Rel);
		SetRelativeScale3D(FVector(W / 100.f, H / 100.f, 1.f));
		break;
	}
	}
}

// =================================================================================================
// Debug gallery (arena.DebugFXGallery <seconds>)
// =================================================================================================

#include "ArenaProjectile.h"
#include "ArenaPickup.h"
#include "ArenaONS_PowerNode.h"
#include "ArenaFlag.h"
#include "ArenaBR_Ball.h"
#include "ArenaBR_Goal.h"
#include "ArenaDOM_ControlPoint.h"
#include "ArenaJumpPad.h"
#include "Camera/CameraActor.h"
#include "EngineUtils.h"
#include "ArenaWeapons.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"

namespace ArenaArtFXPrivate
{
	static TAutoConsoleVariable<float> CVarFXGallery(
		TEXT("arena.DebugFXGallery"), 0.f,
		TEXT("Debug: if > 0, every N seconds spawns all imported-art effects, beams and (slow, harmless) projectiles in a grid in front of the local player."),
		ECVF_Cheat);

	static TAutoConsoleVariable<FString> CVarFXFilter(
		TEXT("arena.DebugFXFilter"), TEXT(""),
		TEXT("Debug: only gallery effects whose name contains this text (\"Projectile\" = only projectiles)."),
		ECVF_Cheat);

	void SpawnGallery(UWorld* World)
	{
		const FString Filter = CVarFXFilter.GetValueOnGameThread();
		if (Filter == TEXT("ONS"))
		{
			// Onslaught check: one node of each state, then a camera cycling over nodes and cores.
			APlayerController* ViewPC = World->GetFirstPlayerController();
			static TWeakObjectPtr<UWorld> SetupFor;
			static TWeakObjectPtr<ACameraActor> Cam;
			static int32 ViewIndex = 0;
			TArray<AArenaONS_PowerNode*> Nodes, Cores;
			for (TActorIterator<AArenaONS_PowerNode> It(World); It; ++It)
			{
				(It->IsCore() ? Cores : Nodes).Add(*It);
			}
			Nodes.Sort([](const AArenaONS_PowerNode& X, const AArenaONS_PowerNode& Y) { return X.GetNodeIndex() < Y.GetNodeIndex(); });
			if (SetupFor.Get() != World && Nodes.Num() >= 3)
			{
				SetupFor = World;
				Nodes[1]->SetNetworkFlags(false, false, true, true);
				Nodes[1]->StartConstruction(0, nullptr);
				Nodes[2]->ForceOwner(1);
				if (Nodes.Num() > 3)
				{
					Nodes[3]->ForceOwner(0);
				}
				FActorSpawnParameters SP;
				SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, SP);
				ViewIndex = 0;
			}
			TArray<AArenaONS_PowerNode*> Targets = Nodes;
			Targets.Append(Cores);
			if (Targets.Num() == 0 || !Cam.IsValid() || !ViewPC)
			{
				return;
			}
			AArenaONS_PowerNode* Target = Targets[ViewIndex++ % Targets.Num()];
			const float Dist = Target->IsCore() ? 1700.f : 1050.f;
			const FVector Focus = Target->GetActorLocation() + FVector(0.f, 0.f, Target->IsCore() ? 350.f : 220.f);
			const FVector From = Focus + FRotator(0.f, 30.f + 70.f * ViewIndex, 0.f).Vector() * Dist + FVector(0.f, 0.f, Dist * 0.35f);
			Cam->SetActorLocationAndRotation(From, (Focus - From).Rotation());
			ViewPC->SetViewTarget(Cam.Get());
			UE_LOG(LogOpenUT2k4, Log, TEXT("DebugFXGallery: viewing %s %d state=%d team=%d"), Target->IsCore() ? TEXT("core") : TEXT("node"),
				Target->GetNodeIndex(), int32(Target->GetState()), Target->GetTeam());
			return;
		}
		if (Filter == TEXT("Objects"))
		{
			// Camera cycling over flags, bombing run ball / goals, domination points and jump pads.
			APlayerController* ViewPC = World->GetFirstPlayerController();
			static TWeakObjectPtr<ACameraActor> ObjCam;
			static int32 ObjIndex = 0;
			TArray<AActor*> Targets;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (Cast<AArenaFlag>(*It) || Cast<AArenaBR_Ball>(*It) || Cast<AArenaBR_Goal>(*It) || Cast<AArenaDOM_ControlPoint>(*It) || Cast<AArenaJumpPad>(*It))
				{
					Targets.Add(*It);
				}
			}
			auto Rank = [](const AActor* A) { return Cast<AArenaJumpPad>(A) ? 1 : 0; };
			Targets.StableSort([&Rank](const AActor& X, const AActor& Y) { return Rank(&X) < Rank(&Y); });
			if (!ObjCam.IsValid())
			{
				FActorSpawnParameters SP;
				SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				ObjCam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, SP);
			}
			if (Targets.Num() == 0 || !ObjCam.IsValid() || !ViewPC)
			{
				return;
			}
			AActor* Target = Targets[ObjIndex++ % Targets.Num()];
			FVector Focus = Target->GetActorLocation() + FVector(0.f, 0.f, 120.f);
			float Dist = 650.f;
			if (const AArenaBR_Goal* Goal = Cast<AArenaBR_Goal>(Target))
			{
				Dist = 1100.f;
			}
			if (Cast<AArenaDOM_ControlPoint>(Target))
			{
				Dist = 900.f;
				Focus.Z += 100.f;
			}
			const FVector From = Focus + FRotator(0.f, 20.f + 60.f * ObjIndex, 0.f).Vector() * Dist + FVector(0.f, 0.f, Dist * 0.3f);
			ObjCam->SetActorLocationAndRotation(From, (Focus - From).Rotation());
			ViewPC->SetViewTarget(ObjCam.Get());
			UE_LOG(LogOpenUT2k4, Log, TEXT("DebugFXGallery: viewing %s"), *Target->GetName());
			return;
		}
		static TWeakObjectPtr<UWorld> PickupsDoneFor;
		if (Filter == TEXT("Pickup") && PickupsDoneFor.Get() == World)
		{
			return;
		}
		APlayerController* PC = World->GetFirstPlayerController();
		if (!PC || !PC->PlayerCameraManager)
		{
			return;
		}
		const FMinimalViewInfo& View = PC->PlayerCameraManager->GetCameraCacheView();
		// Face the most open direction (and turn the player to it) so walls do not hide the gallery.
		float BestYaw = View.Rotation.Yaw;
		float BestDist = -1.f;
		for (int32 i = 0; i < 12; ++i)
		{
			const float Yaw = View.Rotation.Yaw + i * 30.f;
			FHitResult Hit;
			FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaFXGallery), false, PC->GetPawn());
			const FVector Dir = FRotator(0.f, Yaw, 0.f).Vector();
			const float Dist = World->LineTraceSingleByChannel(Hit, View.Location, View.Location + Dir * 3000.f, ECC_Visibility, QP) ? Hit.Distance : 3000.f;
			if (Dist > BestDist + 50.f)
			{
				BestDist = Dist;
				BestYaw = Yaw;
			}
		}
		PC->SetControlRotation(FRotator(0.f, BestYaw, 0.f));
		const FRotator Flat(0.f, BestYaw, 0.f);
		const FVector F = Flat.Vector();
		const FVector R = FRotationMatrix(Flat).GetUnitAxis(EAxis::Y);
		const FVector U = FVector::UpVector;
		const FVector Centre = View.Location + F * FMath::Clamp(BestDist * 0.6f, 350.f, 650.f) + U * -60.f;
		const float Period = CVarFXGallery.GetValueOnGameThread();

		int32 Cell = 0;
		int32 BeamRow = 0;
		for (const FEffectDesc& E : Effects())
		{
			const FString Name = E.Name;
			if (!Filter.IsEmpty() && !Name.Contains(Filter))
			{
				continue;
			}
			if (Name.StartsWith(TEXT("Beam.")) || Name.StartsWith(TEXT("Tracer.")))
			{
				const FVector S = Centre - R * 500.f + U * (-100.f + 50.f * BeamRow) - F * 150.f;
				SpawnBeamEffect(World, E.Name, S, S + R * 1400.f + F * 300.f, 1.f);
				++BeamRow;
				continue;
			}
			const int32 Col = Cell % 7;
			const int32 Row = Cell / 7;
			++Cell;
			const FVector L = Centre + R * ((Col - 3) * 130.f) + U * (130.f - Row * 120.f);
			const float S = Name.StartsWith(TEXT("Explosion.")) ? 0.3f : (Name.StartsWith(TEXT("Muzzle.")) ? 2.f : 0.8f);
			if (!SpawnEffect(World, E.Name, L, (-F).Rotation(), S))
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("DebugFXGallery: effect %s could not be built"), E.Name);
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("DebugFXGallery: spawned at t=%.2f (%d effects, %d beams)"), World->GetTimeSeconds(), Cell, BeamRow);

		if (Filter == TEXT("Pickup"))
		{
			// Every pickup kind in two rows on the floor in front of the player (once).
			PickupsDoneFor = World;
			PC->SetControlRotation(FRotator(-28.f, BestYaw, 0.f));
			FHitResult Floor;
			const FVector Base = View.Location + F * 330.f;
			const float FloorZ = World->LineTraceSingleByChannel(Floor, Base, Base - U * 1000.f, ECC_Visibility) ? Floor.ImpactPoint.Z : Base.Z - 100.f;
			auto Spawn = [World](const FVector& L, EArenaPickupType Type, TSubclassOf<AArenaWeapon> Weapon, bool bOnBase)
			{
				const FTransform T(FRotator::ZeroRotator, L);
				if (AArenaPickup* Pickup = World->SpawnActorDeferred<AArenaPickup>(AArenaPickup::StaticClass(), T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
				{
					Pickup->Setup(Type, Weapon);
					Pickup->SetOnBase(bOnBase);
					Pickup->FinishSpawning(T);
				}
			};
			const TArray<TSubclassOf<AArenaWeapon>> Weapons = ArenaWeaponList::AllWeapons();
			for (int32 i = 0; i < Weapons.Num(); ++i)
			{
				const FVector Row = FVector(Base.X, Base.Y, FloorZ + 60.f) + R * ((i - (Weapons.Num() - 1) * 0.5f) * 175.f);
				Spawn(Row + F * 230.f, EArenaPickupType::Weapon, Weapons[i], true);
				Spawn(Row, EArenaPickupType::Ammo, Weapons[i], false);
			}
			const EArenaPickupType Items[] = { EArenaPickupType::HealthVial, EArenaPickupType::HealthPack, EArenaPickupType::SuperHealth,
				EArenaPickupType::ShieldSmall, EArenaPickupType::ShieldLarge, EArenaPickupType::DoubleDamage, EArenaPickupType::Adrenaline };
			for (int32 i = 0; i < UE_ARRAY_COUNT(Items); ++i)
			{
				Spawn(FVector(Base.X, Base.Y, FloorZ + 60.f) + F * 480.f + R * ((i - 3) * 180.f), Items[i], nullptr, true);
			}
			return;
		}

		int32 P = 0;
		for (const FProjectileArt& A : Projectiles())
		{
			if (!Filter.IsEmpty() && Filter != TEXT("Projectile"))
			{
				break;
			}
			FArenaProjectileParams Params;
			Params.Style = A.Style;
			Params.Size = A.RefSize;
			Params.Speed = 120.f;
			Params.MaxSpeed = 120.f;
			Params.Damage = 0.f;
			Params.LifeSpan = FMath::Max(0.5f, Period - 0.1f);
			const FVector L = Centre + R * ((P - 3) * 130.f - 40.f) + U * 60.f;
			const FTransform T(R.Rotation(), L);
			if (AArenaProjectile* Proj = World->SpawnActorDeferred<AArenaProjectile>(AArenaProjectile::StaticClass(), T, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
			{
				Proj->Init(Params, nullptr, 0.f);
				Proj->FinishSpawning(T);
			}
			++P;
		}
	}

}

void ArenaArtFX::TickDebugGallery(UWorld* World)
{
	static double Next = 0.0;
	const float Period = ArenaArtFXPrivate::CVarFXGallery.GetValueOnGameThread();
	if (Period <= 0.f || !World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	if (Now >= Next || Now + Period < Next)
	{
		Next = Now + Period;
		ArenaArtFXPrivate::SpawnGallery(World);
	}
}
