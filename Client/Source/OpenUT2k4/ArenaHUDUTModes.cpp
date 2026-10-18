// The game modes' parts of UT2004's HUD: HudCCaptureTheFlag, HudCBombingRun, HudCDoubleDomination, ONSHUDOnslaught,
// HUD_Assault and HudInvasion (their ShowTeamScorePassA / ShowTeamScorePassC and helpers). Widget values are the stock
// class defaults; see ArenaHUDUT.cpp and ArenaHUDWidgets.h. The game rules gather their state and call these.

#include "ArenaHUD.h"
#include "ArenaHUDWidgets.h"
#include "ArenaArtHUD.h"
#include "ArenaContent.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaHudFonts.h"
#include "ArenaLevelBuilder.h"
#include "ArenaRuntimeMap.h"
#include "OpenUT2k4.h"

#include "CollisionQueryParams.h"
#include "Dom/JsonObject.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"


namespace ArenaHUDUTModesLocal
{
	using namespace ArenaHudUT;
	FColor Black150() { return FColor(0, 0, 0, 150); }

	/** UE2 colour arithmetic: saturating add, multiply by a float */
	FColor ColorAdd(const FColor& A, const FColor& B)
	{
		return FColor(uint8(FMath::Min(255, A.R + B.R)), uint8(FMath::Min(255, A.G + B.G)), uint8(FMath::Min(255, A.B + B.B)), uint8(FMath::Min(255, A.A + B.A)));
	}
	FColor ColorMul(const FColor& A, float F)
	{
		auto C = [F](uint8 V) { return uint8(FMath::Clamp(FMath::RoundToInt(V * F), 0, 255)); };
		return FColor(C(A.R), C(A.G), C(A.B), C(A.A));
	}

	/** World units (cm) per UE2 unit of the converted maps (map.json "scale") */
	constexpr float UUToCm = 2.f;

	/** The radar of an Onslaught map (LevelInfo.RadarMapImage and the radar range rules of ONSHUDOnslaught.PostBeginPlay). */
	struct FRadarInfo
	{
		FString MapName;
		FString ImagePath;
		bool bShowRadarMap = true;
		/** RadarRange in cm */
		float Range = 0.f;
	};

	FString TexturePathOf(const FString& ObjectPath)
	{
		// "Pkg.Group.Name" -> "Textures/Pkg/Group_Name", "Pkg.Name" -> "Textures/Pkg/Name"
		FString Pkg, Rest;
		if (!ObjectPath.Split(TEXT("."), &Pkg, &Rest))
		{
			return FString();
		}
		return FString::Printf(TEXT("Textures/%s/%s"), *Pkg, *Rest.Replace(TEXT("."), TEXT("_")));
	}

	const FRadarInfo& GetRadarInfo(const UWorld* World)
	{
		static FRadarInfo Info;
		const FString MapName = AArenaLevelBuilder::GetCurrentMapName(World);
		if (Info.MapName == MapName && !MapName.IsEmpty())
		{
			return Info;
		}
		Info = FRadarInfo();
		Info.MapName = MapName;
		FString Text;
		if (MapName.IsEmpty() || !FFileHelper::LoadFileToString(Text, *(ArenaRuntimeMap::GetMapDir(MapName) / TEXT("map.json"))))
		{
			return Info;
		}
		TSharedPtr<FJsonObject> Doc;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Doc) || !Doc.IsValid())
		{
			return Info;
		}
		// LevelInfo properties over the class defaults of the map
		bool bUseTerrain = true;
		double CustomRange = 10000.0;
		const TSharedPtr<FJsonObject>* Defaults = nullptr;
		const TSharedPtr<FJsonObject>* LevelDefaults = nullptr;
		if (Doc->TryGetObjectField(TEXT("class_defaults"), Defaults) && (*Defaults)->TryGetObjectField(TEXT("LevelInfo"), LevelDefaults))
		{
			(*LevelDefaults)->TryGetBoolField(TEXT("bUseTerrainForRadarRange"), bUseTerrain);
			(*LevelDefaults)->TryGetNumberField(TEXT("CustomRadarRange"), CustomRange);
			(*LevelDefaults)->TryGetBoolField(TEXT("bShowRadarMap"), Info.bShowRadarMap);
		}
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		FString PrimaryTerrainName;
		if (Doc->TryGetArrayField(TEXT("actors"), Actors))
		{
			for (const TSharedPtr<FJsonValue>& V : *Actors)
			{
				const TSharedPtr<FJsonObject> A = V.IsValid() ? V->AsObject() : nullptr;
				FString Class;
				const TSharedPtr<FJsonObject>* Props = nullptr;
				if (!A.IsValid() || !A->TryGetStringField(TEXT("class"), Class) || !A->TryGetObjectField(TEXT("properties"), Props))
				{
					continue;
				}
				if (Class == TEXT("LevelInfo"))
				{
					FString Image;
					if ((*Props)->TryGetStringField(TEXT("RadarMapImage"), Image))
					{
						Info.ImagePath = TexturePathOf(Image);
					}
					(*Props)->TryGetBoolField(TEXT("bUseTerrainForRadarRange"), bUseTerrain);
					(*Props)->TryGetNumberField(TEXT("CustomRadarRange"), CustomRange);
					(*Props)->TryGetBoolField(TEXT("bShowRadarMap"), Info.bShowRadarMap);
				}
				else if (Class == TEXT("TerrainInfo"))
				{
					// foreach AllActors(TerrainInfo): the last one, or the one tagged PrimaryTerrain
					FString Tag, Name;
					(*Props)->TryGetStringField(TEXT("Tag"), Tag);
					A->TryGetStringField(TEXT("name"), Name);
					if (PrimaryTerrainName.IsEmpty() || !PrimaryTerrainName.StartsWith(TEXT("!")))
					{
						PrimaryTerrainName = Tag == TEXT("PrimaryTerrain") ? TEXT("!") + Name : Name;
					}
				}
			}
		}
		PrimaryTerrainName.RemoveFromStart(TEXT("!"));
		float Range = 0.f;
		const TSharedPtr<FJsonObject>* Markers = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Terrains = nullptr;
		if (bUseTerrain && Doc->TryGetObjectField(TEXT("markers"), Markers) && (*Markers)->TryGetArrayField(TEXT("terrains"), Terrains))
		{
			for (const TSharedPtr<FJsonValue>& V : *Terrains)
			{
				const TSharedPtr<FJsonObject> T = V.IsValid() ? V->AsObject() : nullptr;
				FString Name, HeightmapFile;
				const TArray<TSharedPtr<FJsonValue>>* Scale = nullptr;
				if (!T.IsValid() || !T->TryGetStringField(TEXT("actor"), Name) || (Name != PrimaryTerrainName && !PrimaryTerrainName.IsEmpty())
					|| !T->TryGetStringField(TEXT("heightmap_file"), HeightmapFile) || !T->TryGetArrayField(TEXT("terrain_scale_raw"), Scale) || Scale->Num() < 1)
				{
					continue;
				}
				// TerrainMap.USize from the converted heightmap's json
				FString HmText;
				TSharedPtr<FJsonObject> Hm;
				const FString HmJson = FPaths::ChangeExtension(ArenaContent::GetCacheDir() / HeightmapFile, TEXT("json"));
				double USize = 0.0;
				if (FFileHelper::LoadFileToString(HmText, *HmJson) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(HmText), Hm) && Hm.IsValid()
					&& Hm->TryGetNumberField(TEXT("width"), USize))
				{
					Range = float(FMath::Abs((*Scale)[0]->AsNumber() * USize) / 2.0) * UUToCm;
				}
			}
		}
		if (Range <= 0.f && CustomRange > 0.0)
		{
			Range = float(FMath::Clamp(CustomRange, 500.0, 500000.0)) * UUToCm;
		}
		Info.Range = Range;
		UE_LOG(LogOpenUT2k4, Log, TEXT("HUD: radar of %s: image '%s', range %.0f cm"), *MapName, *Info.ImagePath, Info.Range);
		return Info;
	}

	/** Canvas.WorldToScreen; false behind the camera */
	bool UTProjectPoint(UCanvas* Canvas, const FVector& World, FVector2D& Out)
	{
		if (!Canvas)
		{
			return false;
		}
		const FVector P = Canvas->Project(World);
		Out = FVector2D(P.X, P.Y);
		return P.Z > 0.f;
	}
}


// =============================================================================================
// HudCCaptureTheFlag
// =============================================================================================

void AArenaHUD::DrawUTCaptureTheFlag(const uint8 FlagState[2], const FVector* BaseLocation[2])
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTModesLocal;
	const AArenaGameState* GS = GetArenaGS();
	if (!GS)
	{
		return;
	}
	const FHudFrame& F = UTFrame;
	const FColor Black = Black150();
	DrawSprite(F, Spr(ETex::Hud, 168, 211, 334, 255, 0.53f, DP_UpperRight, 0.5f, 0.f, -50, 10, Black, Black));
	DrawSprite(F, Spr(ETex::Hud, 168, 211, 334, 255, 0.53f, DP_UpperLeft, 0.5f, 0.f, 50, 10, Black, Black));
	DrawSprite(F, Spr(ETex::Hud, 119, 258, 173, 313, 0.53f, DP_UpperRight, 0.5f, 0.f, -35, 5));
	DrawSprite(F, Spr(ETex::Hud, 119, 258, 173, 313, 0.53f, DP_UpperLeft, 0.5f, 0.f, 35, 5));
	// NewFlagWidgets: the flag in each team's colour (the blue one mirrored)
	DrawSprite(F, Spr(ETex::Hud, 336, 129, 397, 170, 0.45f, DP_UpperRight, 0.5f, 0.f, -42, 8, Colors::HudRed, Colors::HudRed));
	DrawSprite(F, Spr(ETex::Hud, 397, 129, 339, 170, 0.45f, DP_UpperLeft, 0.5f, 0.f, 42, 8, Colors::HudBlue, Colors::HudBlue));
	DrawSprite(F, Spr(ETex::Hud, 435, 0, 508, 31, 0.4f, DP_UpperMiddle, 0.5f, 0.f, 0, 30));
	FNumericWidget Score[2] = { Num(0.54f, DP_MiddleRight, 0.5f, 0.f, -90, 32, 1), Num(0.54f, DP_MiddleLeft, 0.5f, 0.f, 90, 32, 1) };
	for (int32 Team = 0; Team < 2; ++Team)
	{
		Score[Team].Value = FMath::Min(GS->TeamScores[Team], 999);
		DrawNumeric(F, Score[Team]);
	}
	// the flag bases' direction dots (REDtmpPosX .. BLUEtmpScaleY)
	const float HS = F.HudScale;
	if (BaseLocation[0])
	{
		Draw2DLocationDot(F, UTRelativeYaw(*BaseLocation[0]), 0.5f - 0.058f * HS, 0.026f * HS, 0.02f * HS, 0.03f * HS, Colors::HudRed);
	}
	if (BaseLocation[1])
	{
		Draw2DLocationDot(F, UTRelativeYaw(*BaseLocation[1]), 0.5f + 0.044f * HS, 0.026f * HS, 0.02f * HS, 0.03f * HS, Colors::HudBlue);
	}
	for (int32 Team = 0; Team < 2; ++Team)
	{
		if (FlagState[Team] == 2)
		{
			// FlagHeldWidgets
			DrawSprite(F, Spr(ETex::HudPulse, 341, 211, 366, 271, 0.3f, Team == 0 ? DP_UpperRight : DP_UpperLeft, 0.5f, 0.f, Team == 0 ? -95 : 95, 25));
		}
		else if (FlagState[Team] == 3)
		{
			// FlagDownWidgets
			DrawSprite(F, Spr(ETex::HudPulse, 77, 271, 116, 311, 0.45f, Team == 0 ? DP_UpperRight : DP_UpperLeft, 0.5f, 0.f, Team == 0 ? -53 : 53, 40));
		}
	}
}

// =============================================================================================
// HudCBombingRun
// =============================================================================================

void AArenaHUD::DrawUTBombingRun(uint8 BallState, const FVector* BallLocation)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTModesLocal;
	const FHudFrame& F = UTFrame;
	DrawUTTeamScores(true, true);
	// BombBG, BombWidget.Widgets[BombState], BombWidgetStatus.Widgets[BombState]
	DrawSprite(F, Spr(ETex::Hud, 119, 258, 173, 313, 0.7f, DP_UpperMiddle, 0.5f, 0.f, 0, 0));
	const uint8 State = FMath::Min<uint8>(BallState, 3);
	FSpriteWidget Ball;
	switch (State)
	{
	case 1: Ball = Spr(ETex::Hud, 79, 223, 116, 264, 0.7f, DP_UpperMiddle, 0.5f, 0.f, 0, 10, Colors::HudRed, Colors::HudBlue); break;
	case 2: Ball = Spr(ETex::HudAlertSlow, 79, 223, 116, 264, 0.7f, DP_UpperMiddle, 0.5f, 0.f, 0, 10, Colors::HudBlue, Colors::HudRed); break;
	case 3: Ball = Spr(ETex::HudAlertSlow, 79, 223, 116, 264, 0.7f, DP_UpperMiddle, 0.5f, 0.f, 0, 10); break;
	default: Ball = Spr(ETex::Hud, 79, 223, 116, 264, 0.7f, DP_UpperMiddle, 0.5f, 0.f, 0, 5, FColor(255, 255, 255, 100), FColor(255, 255, 255, 100)); break;
	}
	Ball.ScaleMode = SM_None;
	DrawSprite(F, Ball);
	if (State == 2)
	{
		DrawSprite(F, Spr(ETex::HudPulse, 341, 211, 366, 271, 0.35f, DP_UpperMiddle, 0.5f, 0.f, 0, 30));
	}
	else if (State == 3)
	{
		DrawSprite(F, Spr(ETex::HudPulse, 77, 271, 116, 311, 0.53f, DP_UpperMiddle, 0.5f, 0.f, 0, 40));
	}
	if (BallLocation)
	{
		const float HS = F.HudScale;
		// LocationDot = HUDContent.Generic.HUDPulse, in the highlight colour
		Draw2DLocationDot(F, UTRelativeYaw(*BallLocation), 0.5f - 0.0075f * HS, 0.03f * HS, 0.027f * HS, 0.035f * HS, Colors::HighLight, ETex::HudPulse);
	}
}

// =============================================================================================
// HudCDoubleDomination
// =============================================================================================

void AArenaHUD::DrawUTDoubleDomination(const uint8 PointState[2], const FVector* PointLocation[2])
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTModesLocal;
	const AArenaGameState* GS = GetArenaGS();
	if (!GS)
	{
		return;
	}
	const FHudFrame& F = UTFrame;
	const float HS = F.HudScale;
	// SpaceOutTeamInfo: the score capsules move NewOffsetX (0.058) out from the centre; no versus icon
	const float Shift[2] = { -0.058f, 0.058f };
	const FColor Black = Black150();
	for (int32 i = 0; i < 2; ++i)
	{
		DrawSprite(F, Spr(ETex::Hud, 168, 211, 334, 255, 0.53f, i == 0 ? DP_UpperRight : DP_UpperLeft, 0.5f + Shift[i], 0.f, i == 0 ? -50 : 50, 10, Black, Black));
	}
	for (int32 i = 0; i < 2; ++i)
	{
		DrawSprite(F, Spr(ETex::Hud, 119, 258, 173, 313, 0.53f, i == 0 ? DP_UpperRight : DP_UpperLeft, 0.5f + Shift[i], 0.f, i == 0 ? -35 : 35, 5));
	}
	const FColor SymbolTints[2][2] = { { Colors::HudRed, FColor(255, 32, 32, 200) }, { FColor(0, 128, 255, 200), Colors::HudBlue } };
	for (int32 Team = 0; Team < 2; ++Team)
	{
		const FString& Symbol = GS->GetTeamSymbol(Team);
		if (!Symbol.IsEmpty())
		{
			const float S = 0.1f * F.TotalScale;
			const float Size = 257.f * S;
			float X = F.ScreenX(0.5f + Shift[Team]) + (Team == 0 ? -200.f : 200.f) * S;
			if (Team == 0)
			{
				X -= Size;
			}
			DrawTilePath(D, *Symbol, 256.f, 256.f, X, F.ScreenY(0.f) + 45.f * S, Size, Size, 0.f, 0.f, 256.f, 256.f, FLinearColor(SymbolTints[Team][F.TeamIndex]));
		}
	}
	for (int32 Team = 0; Team < 2; ++Team)
	{
		FNumericWidget Score = Num(0.54f, Team == 0 ? DP_MiddleRight : DP_MiddleLeft, 0.5f + Shift[Team], 0.f, Team == 0 ? -90 : 90, 32, 1);
		Score.Value = FMath::Min(GS->TeamScores[Team], 999);
		DrawNumeric(F, Score);
	}
	// SymbolGB discs, the turning DomRing in the point's colour, the letters A / B
	DrawSprite(F, Spr(ETex::Hud, 119, 258, 173, 313, 0.6f, DP_UpperRight, 0.5f, 0.f, -32, 7));
	DrawSprite(F, Spr(ETex::Hud, 119, 258, 173, 313, 0.6f, DP_UpperLeft, 0.5f, 0.f, 32, 7));
	// Points[i].Widgets[PointState].Tints: 0 neutral, 1 the viewer's team, 2 the other team, 3 disabled
	const FColor PointTints[4][2] = {
		{ FColor(195, 195, 195, 255), FColor(195, 195, 195, 150) },
		{ FColor(255, 32, 32, 200), FColor(0, 128, 255, 255) },
		{ FColor(0, 128, 255, 255), FColor(255, 32, 32, 200) },
		{ FColor(195, 195, 195, 55), FColor(195, 195, 195, 55) },
	};
	const FIntBox Letters[2] = { { 0, 451, 63, 511 }, { 64, 451, 125, 511 } };
	const int32 LetterOffX[2] = { -108, 110 };
	const int32 LetterOffY[2] = { -5, -4 };
	for (int32 i = 0; i < 2; ++i)
	{
		const uint8 St = FMath::Min<uint8>(PointState[i], 3);
		FColor RingTint = PointTints[St][F.TeamIndex];
		RingTint.A = 120;
		DrawSprite(F, Spr(ETex::DomRingRot, 0, 0, 127, 127, 0.3f, i == 0 ? DP_UpperRight : DP_UpperLeft, 0.5f, 0.f, i == 0 ? -55 : 55, 5, RingTint, RingTint));
	}
	for (int32 i = 0; i < 2; ++i)
	{
		const uint8 St = FMath::Min<uint8>(PointState[i], 3);
		FSpriteWidget Letter = Spr(ETex::Hud, Letters[i].X1, Letters[i].Y1, Letters[i].X2, Letters[i].Y2, 0.33f, DP_MiddleMiddle, 0.5f, 0.045f * HS,
			LetterOffX[i], LetterOffY[i], PointTints[St][0], PointTints[St][1]);
		Letter.ScaleMode = SM_None;
		DrawSprite(F, Letter);
	}
	if (PointLocation[0])
	{
		Draw2DLocationDot(F, UTRelativeYaw(*PointLocation[0]), 0.5f - 0.063f * HS, 0.035f * HS, 0.025f * HS, 0.035f * HS, Colors::HighLight);
	}
	if (PointLocation[1])
	{
		Draw2DLocationDot(F, UTRelativeYaw(*PointLocation[1]), 0.5f + 0.049f * HS, 0.035f * HS, 0.025f * HS, 0.035f * HS, Colors::HighLight);
	}
	for (int32 i = 0; i < 2; ++i)
	{
		if (PointState[i] == 2)
		{
			DrawSprite(F, Spr(ETex::HudPulse, 341, 211, 366, 271, 0.3f, i == 0 ? DP_UpperRight : DP_UpperLeft, 0.5f, 0.f, i == 0 ? -105 : 105, 40));
		}
	}
}

// =============================================================================================
// ONSHUDOnslaught
// =============================================================================================

void AArenaHUD::DrawUTNodeHealthBar(const FUTPowerNode& Node)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTModesLocal;
	// ONSHUDOnslaught.DrawHealthBar: 75 x 8 pixels (BorderBoxD, then ONSInterface-TX.HealthBar in red / gold / green),
	// HealthBarPosition 485 above the node, only within HealthBarViewDist 4000 and with a clear line of sight
	APlayerController* PC = PlayerOwner;
	UWorld* World = GetWorld();
	if (!PC || !World || !Canvas)
	{
		return;
	}
	FVector CamLoc;
	FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);
	const FVector Target = Node.Location + FVector(0.f, 0.f, Node.BarHeight > 0.f ? Node.BarHeight : 485.f * UUToCm);
	if (FVector::Dist(Target, CamLoc) > 4000.f * UUToCm || FVector::DotProduct(Target - CamLoc, CamRot.Vector()) < 0.f)
	{
		return;
	}
	FVector2D P;
	if (!UTProjectPoint(Canvas, Target, P) || P.X <= 0.f || P.X >= D.W() || P.Y <= 0.f || P.Y >= D.H())
	{
		return;
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(UTNodeHealthBar), false, PC->GetPawn());
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Target, CamLoc, ECC_Visibility, Q) && FVector::Dist(Hit.ImpactPoint, Target) > 150.f)
	{
		return;
	}
	const float Px = float(UIPixelScale(D.H()));
	const float W = 75.f * Px, H = 8.f * Px;
	DrawTileStretched(D, TEXT("Textures/InterfaceContent/Menu_BorderBoxD"), P.X - W * 0.5f, P.Y, W, H, FLinearColor::White);
	const float Pct = Node.MaxHealth > 0.f ? FMath::Clamp(Node.Health / Node.MaxHealth, 0.f, 1.f) : 0.f;
	const FColor C = Pct < 0.35f ? Colors::Red : (Pct < 0.7f ? Colors::Gold : Colors::Green);
	DrawTileStretched(D, TEXT("Textures/ONSInterface-TX/HealthBar"), P.X - W * 0.5f, P.Y, W * Pct, H, FLinearColor(C));
}

void AArenaHUD::DrawUTOnslaught(const TArray<FUTPowerNode>& Nodes, const TArray<TPair<int32, int32>>& Links, int32 ViewerTeam)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTModesLocal;
	const FHudFrame& F = UTFrame;
	const float T = Now();
	// ONSHUDOnslaught.Tick: ColorPercent = 0.5 + Cos(TimeSeconds * 4 * Pi * 0.5) * 0.5
	const float ColorPercent = 0.5f + FMath::Cos(T * 4.f * PI * 0.5f) * 0.5f;
	const FUTPowerNode* Cores[2] = { nullptr, nullptr };
	for (const FUTPowerNode& N : Nodes)
	{
		if (N.bFinalCore && (N.Team == 0 || N.Team == 1))
		{
			Cores[N.Team] = &N;
		}
	}

	// ---- world health bars (DrawRadarMap: every node with HasHealthBar, CoreStage 0 / 2 / 5) ----
	for (const FUTPowerNode& N : Nodes)
	{
		if (N.Stage == 0 || N.Stage == 2 || N.Stage == 5)
		{
			DrawUTNodeHealthBar(N);
		}
	}

	// ---- ShowTeamScorePassA: team capsules moved to 0.442 / 0.558, scores with two digits, symbols; cores ----
	const AArenaGameState* GS = GetArenaGS();
	const FColor Black = Black150();
	const float BoxX[2] = { 0.442f, 0.558f };
	for (int32 i = 0; i < 2; ++i)
	{
		DrawSprite(F, Spr(ETex::Hud, 168, 211, 334, 255, 0.53f, i == 0 ? DP_UpperRight : DP_UpperLeft, BoxX[i], 0.f, i == 0 ? -50 : 50, 10, Black, Black));
	}
	for (int32 i = 0; i < 2; ++i)
	{
		DrawSprite(F, Spr(ETex::Hud, 119, 258, 173, 313, 0.53f, i == 0 ? DP_UpperRight : DP_UpperLeft, BoxX[i], 0.f, i == 0 ? -35 : 35, 5));
	}
	if (GS)
	{
		const FColor SymbolTints[2][2] = { { Colors::HudRed, FColor(255, 32, 32, 200) }, { FColor(0, 128, 255, 200), Colors::HudBlue } };
		for (int32 Team = 0; Team < 2; ++Team)
		{
			const FString& Symbol = GS->GetTeamSymbol(Team);
			if (!Symbol.IsEmpty())
			{
				const float S = 0.1f * F.TotalScale;
				const float Size = 257.f * S;
				float X = F.ScreenX(BoxX[Team]) + (Team == 0 ? -200.f : 200.f) * S;
				if (Team == 0)
				{
					X -= Size;
				}
				DrawTilePath(D, *Symbol, 256.f, 256.f, X, F.ScreenY(0.f) + 45.f * S, Size, Size, 0.f, 0.f, 256.f, 256.f, FLinearColor(SymbolTints[Team][F.TeamIndex]));
			}
		}
		DrawSprite(F, Spr(ETex::Hud, 435, 0, 508, 31, 0.4f, DP_UpperMiddle, 0.5f, 0.f, 0, 30));
		for (int32 Team = 0; Team < 2; ++Team)
		{
			FNumericWidget Score = Num(0.54f, Team == 0 ? DP_MiddleRight : DP_MiddleLeft, BoxX[Team], 0.f, Team == 0 ? -90 : 90, 32, 2);
			Score.Value = FMath::Min(GS->TeamScores[Team], 999);
			DrawNumeric(F, Score);
		}
	}
	for (int32 X = 0; X < 2; ++X)
	{
		DrawSprite(F, Spr(ETex::Hud, 119, 258, 173, 313, 0.6f, X == 0 ? DP_UpperRight : DP_UpperLeft, 0.5f, 0.f, X == 0 ? -32 : 32, 7));
		if (Cores[X] && Cores[X]->Stage == 0)
		{
			const FColor CoreTints[2][2] = { { FColor(255, 100, 100, 200), FColor(255, 32, 32, 200) }, { FColor(0, 128, 255, 200), FColor(32, 210, 255, 200) } };
			DrawSprite(F, Spr(ETex::Hud, 365, 233, 432, 313, 0.25f, X == 0 ? DP_UpperRight : DP_UpperLeft, 0.5f, 0.f, X == 0 ? -110 : 110, 45,
				CoreTints[X][0], CoreTints[X][1]));
			FNumericWidget Health = Num(0.24f, DP_MiddleMiddle, 0.5f, 0.f, X == 0 ? -149 : 149, 88, 1);
			if (Cores[X]->bUnderAttack)
			{
				const uint8 GB = uint8(FMath::Clamp(255.f * ColorPercent, 0.f, 255.f));
				Health.Tints[F.TeamIndex] = FColor(255, GB, GB, 255);
			}
			Health.Value = FMath::RoundToInt(Cores[X]->MaxHealth > 0.f ? Cores[X]->Health / Cores[X]->MaxHealth * 100.f : 0.f);
			DrawNumeric(F, Health);
		}
	}

	// ---- ShowTeamScorePassC: the radar map (top right) ----
	const FRadarInfo& Radar = GetRadarInfo(GetWorld());
	if (!Radar.bShowRadarMap || Radar.Range <= 0.f)
	{
		return;
	}
	// RadarScale 0.2, RadarPosX 1, RadarPosY 0.1: RadarWidth = 0.5 * RadarScale * HudScale * ClipX (4:3 frame width here)
	const float RadarWidth = 0.5f * 0.2f * F.HudScale * F.FrameW;
	const float CX = F.RegionX0 + 1.f * F.RegionW - RadarWidth;
	const float CY = 0.1f * F.H + RadarWidth;
	const float MapScale = RadarWidth / Radar.Range;
	if (!Radar.ImagePath.IsEmpty())
	{
		// DrawMapImage: the whole image over [-RadarRange, RadarRange] around the world origin, alpha RadarTrans 255
		ArenaArtHUD::DrawTexture(D, *Radar.ImagePath, CX - RadarWidth, CY - RadarWidth, RadarWidth * 2.f, RadarWidth * 2.f, FLinearColor::White);
	}
	auto ToMap = [&](const FVector& L) { return FVector2D(CX + L.X * MapScale, CY + L.Y * MapScale); };
	const float CoreIconSize = 1.f * 16.f * F.FrameW * F.HudScale / 1600.f;
	// PowerLinks[i].Render: a line between the two nodes' map positions
	const FColor LinkColor[3] = { FColor(255, 0, 0, 255), FColor(0, 0, 255, 255), FColor(255, 255, 255, 255) };
	auto TeamColor = [&](int32 Team) { return LinkColor[(Team == 0 || Team == 1) ? Team : 2]; };
	for (const TPair<int32, int32>& L : Links)
	{
		if (!Nodes.IsValidIndex(L.Key) || !Nodes.IsValidIndex(L.Value))
		{
			continue;
		}
		const FUTPowerNode& A = Nodes[L.Key];
		const FUTPowerNode& B = Nodes[L.Value];
		FColor C0 = TeamColor(A.Team), C1 = TeamColor(B.Team), DrawColor;
		if (A.Stage == 4 && B.Stage == 0)
		{
			DrawColor = C1;
		}
		else if (B.Stage == 4 && A.Stage == 0)
		{
			DrawColor = C0;
		}
		else
		{
			if (A.Stage == 2 || A.Stage == 5)
			{
				C0 = ColorMul(C0, ColorPercent);
				C1 = ColorMul(C1, 1.f - ColorPercent);
			}
			else if (B.Stage == 2 || A.Stage == 5)
			{
				C0 = ColorMul(C0, 1.f - ColorPercent);
				C1 = ColorMul(C1, ColorPercent);
			}
			DrawColor = ColorAdd(C0, C1);
		}
		const FVector2D PA = ToMap(A.Location), PB = ToMap(B.Location);
		D.Line(PA.X, PA.Y, PB.X, PB.Y, FLinearColor(DrawColor), FMath::Max(1.f, float(UIPixelScale(D.H()))));
	}
	const TCHAR* Icons = TEXT("Textures/ONSInterface-TX/NewHUDicons");
	auto Icon = [&](const FVector2D& P, float Size, float U, float V, float UL, float VL, const FColor& C)
	{
		DrawTilePath(D, Icons, 128.f, 128.f, P.X - Size * 0.5f, P.Y - Size * 0.5f, Size, Size, U, V, UL, VL, FLinearColor(C));
	};
	for (const FUTPowerNode& N : Nodes)
	{
		const FVector2D P = ToMap(N.Location);
		FColor DrawColor = TeamColor(N.Team);
		// DrawAttackIcon: under attack, or an active node cut off from its core
		if (N.bUnderAttack || (N.Stage == 0 && N.bSevered))
		{
			const float Size = (N.bFinalCore ? 2.f : 1.f) * CoreIconSize * (2.5f + 1.5f * ColorPercent);
			const FColor C = (N.bSevered && N.Stage == 0)
				? ColorAdd(ColorMul(FColor(192, 192, 192, 255), ColorPercent), ColorMul(FColor(128, 128, 128, 255), 1.f - ColorPercent))
				: ColorAdd(ColorMul(FColor(244, 189, 0, 255), ColorPercent), ColorMul(FColor(234, 162, 11, 255), 1.f - ColorPercent));
			Icon(P, Size, 0.f, 64.f, 64.f, 64.f, C);
		}
		if (N.bFinalCore)
		{
			// DrawCoreIcon
			if (N.bAttackable)
			{
				DrawColor = ColorAdd(ColorMul(DrawColor, ColorPercent), ColorMul(ColorMul(DrawColor, 0.5f), 1.f - ColorPercent));
			}
			Icon(P, CoreIconSize * 3.f, 64.f, 0.f, 64.f, 64.f, DrawColor);
			continue;
		}
		// DrawNodeIcon
		if (N.Stage == 4 || N.Stage == 1)
		{
			if (N.bAttackable)
			{
				Icon(P, CoreIconSize * 0.75f, 0.f, 0.f, 32.f, 32.f, DrawColor);
			}
			else
			{
				Icon(P, CoreIconSize * 1.75f, 0.f, 32.f, 32.f, 32.f, DrawColor);
			}
		}
		else
		{
			if (N.Stage != 0)
			{
				DrawColor = ColorAdd(ColorMul(DrawColor, ColorPercent), ColorMul(LinkColor[2], 1.f - ColorPercent));
			}
			if (N.bAttackable)
			{
				Icon(P, CoreIconSize * 2.f, 32.f, 0.f, 32.f, 32.f, DrawColor);
			}
			else
			{
				Icon(P, CoreIconSize * 1.75f, 0.f, 32.f, 32.f, 32.f, DrawColor);
			}
		}
		// DrawNodeLabel: the node number in the HUD digits, yellow with a black outline
		if (N.NodeNum > 0 && N.NodeNum < 100)
		{
			const float LabelSize = 1.f * 16.f * F.FrameW * F.HudScale / 1600.f;
			const int32 Tens = N.NodeNum / 10;
			const int32 Ones = N.NodeNum % 10;
			auto Digit = [&](int32 Value, float X, float Y)
			{
				for (int32 Row = -1; Row < 2; ++Row)
				{
					for (int32 Col = -1; Col < 2; ++Col)
					{
						DrawTile(D, ETex::Hud, X + Col, Y + Row, LabelSize, LabelSize, float(39 * Value), 0.f, 38.f, 38.f, FLinearColor::Black);
					}
				}
				DrawTile(D, ETex::Hud, X, Y, LabelSize, LabelSize, float(39 * Value), 0.f, 38.f, 38.f, FLinearColor(FColor(255, 255, 0, 255)));
			};
			if (N.NodeNum > 9)
			{
				Digit(Tens, P.X - 1.5f * LabelSize * 1.5f, P.Y - LabelSize * 1.5f);
			}
			Digit(Ones, P.X - LabelSize * 1.5f, P.Y - LabelSize * 1.5f);
		}
	}
	// the viewer: ONSInterface-TX.CurrentPlayerIcon turned to the view yaw, 1.5 x the core icon size, green
	if (PlayerOwner)
	{
		FVector ViewLoc;
		FRotator ViewRot;
		PlayerOwner->GetPlayerViewPoint(ViewLoc, ViewRot);
		const APawn* Pawn = PlayerOwner->GetPawn();
		const FVector Loc = Pawn ? Pawn->GetActorLocation() : ViewLoc;
		if (FMath::Abs(Loc.X) < Radar.Range * 0.95f && FMath::Abs(Loc.Y) < Radar.Range * 0.95f)
		{
			const float Size = CoreIconSize * 1.5f;
			const FVector2D P = ToMap(Loc);
			DrawTilePath(D, TEXT("Textures/ONSInterface-TX/CurrentPlayerIcon"), 64.f, 64.f, P.X - Size * 0.5f, P.Y - Size * 0.5f, Size, Size,
				0.f, 0.f, 64.f, 64.f, FLinearColor(FColor(40, 255, 40, 255)), ViewRot.Yaw + 90.f);
		}
	}
	// the frame: ONSInterface-TX.MapBorderTEX over the map in grey 200
	DrawTilePath(D, TEXT("Textures/ONSInterface-TX/MapBorderTEX"), 256.f, 256.f, CX - RadarWidth, CY - RadarWidth, RadarWidth * 2.f, RadarWidth * 2.f,
		0.f, 0.f, 256.f, 256.f, FLinearColor(FColor(200, 200, 200, 255)));
	(void)ViewerTeam;
}

// =============================================================================================
// HUD_Assault.ShowTeamScorePassA
// =============================================================================================

void AArenaHUD::DrawUTAssault(int32 RoundTimeLeft, int32 ReinforceSeconds, bool bShowRoundTime)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTModesLocal;
	const FHudFrame& F = UTFrame;
	const FColor Black = Black150();
	float PosY = 0.f;
	const bool bReinforce = ReinforceSeconds >= 0;
	if (bShowRoundTime)
	{
		DrawSprite(F, Spr(ETex::Hud, 168, 211, 334, 255, 0.53f, DP_UpperLeft, 0.f, PosY, 0, 5, Black, Black));
		DrawSprite(F, Spr(ETex::Hud, 119, 258, 173, 313, 0.53f, DP_UpperLeft, 0.f, PosY, 5, 0));
		DrawSprite(F, Spr(ETex::Hud, 495, 91, 503, 112, 0.54f, DP_MiddleLeft, 0.f, PosY, 100, 28));
		DrawSprite(F, Spr(ETex::Hud, 150, 356, 184, 389, 0.53f, DP_MiddleLeft, 0.f, PosY, 15, 28));
		PosY += 0.06f * F.HudScale;
	}
	const float ReinforceY = PosY;
	if (bReinforce)
	{
		DrawSprite(F, Spr(ETex::Hud, 168, 211, 334, 255, 0.53f, DP_UpperLeft, 0.f, ReinforceY, 0, 5, Black, Black));
		DrawSprite(F, Spr(ETex::Hud, 119, 258, 173, 313, 0.53f, DP_UpperLeft, 0.f, ReinforceY, 5, 0));
		if (ReinforceSeconds < 1)
		{
			DrawSprite(F, Spr(ETex::HudPulse, 342, 438, 406, 502, 0.5f, DP_MiddleLeft, 0.f, ReinforceY, 1, 28, Colors::HighLightAS, Colors::HighLightAS));
		}
		DrawSprite(F, Spr(ETex::Hud, 111, 355, 144, 390, 0.53f, DP_MiddleLeft, 0.f, ReinforceY, 15, 28));
		PosY += 0.06f * F.HudScale;
	}
	if (bShowRoundTime)
	{
		const int32 Secs = FMath::Max(0, RoundTimeLeft);
		FNumericWidget Minutes = Num(0.54f, DP_MiddleLeft, 0.f, 0.f, 20, 27, 2, false);
		FNumericWidget Seconds = Num(0.54f, DP_MiddleLeft, 0.f, 0.f, 110, 27, 2, true);
		Minutes.Value = Secs / 60;
		Seconds.Value = Secs % 60;
		DrawNumeric(F, Minutes);
		DrawNumeric(F, Seconds);
	}
	if (bReinforce)
	{
		FNumericWidget Num2 = Num(0.54f, DP_MiddleLeft, 0.f, ReinforceY, 60, 27, 2, true);
		Num2.Value = FMath::Max(0, ReinforceSeconds);
		DrawNumeric(F, Num2);
	}
}

// =============================================================================================
// HudInvasion
// =============================================================================================

void AArenaHUD::DrawUTInvasionRadar(const TArray<FVector>& Monsters)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTModesLocal;
	const FHudFrame& F = UTFrame;
	// RadarPulse grows by 0.5 a second and wraps at 1 (HudInvasion.Tick)
	const float RadarPulse = FMath::Fmod(Now() * 0.5f, 1.f);
	const float RadarScale = 0.2f * F.HudScale;
	const float ClipX = F.FrameW;
	const float RadarWidth = 0.5f * RadarScale * ClipX;
	// RadarPosX 0.9 of the width: kept at the same distance from the right edge
	const float PosX = F.RegionX0 + F.RegionW - 0.1f * ClipX;
	const float PosY = 0.25f * F.H;
	const TCHAR* Skin = TexPath(ETex::SkinA);
	// the expanding pulse rings (InterfaceContent.HUD.SkinA 0,880 142 x 142, STY_Translucent)
	float PulseWidth = RadarScale * ClipX;
	const float Bright = FMath::Max(0.f, (1.f - 2.f * RadarPulse) * 255.f);
	DrawTilePath(D, Skin, 1024.f, 1024.f, PosX - 0.5f * PulseWidth, PosY + RadarWidth - 0.5f * PulseWidth, PulseWidth, PulseWidth, 0.f, 880.f, 142.f, 142.f,
		FLinearColor(FColor(uint8(Bright), 0, 0, 255)), 0.f, true);
	PulseWidth = RadarPulse * RadarScale * ClipX;
	DrawTilePath(D, Skin, 1024.f, 1024.f, PosX - 0.5f * PulseWidth, PosY + RadarWidth - 0.5f * PulseWidth, PulseWidth, PulseWidth, 0.f, 880.f, 142.f, 142.f,
		FLinearColor(FColor(255, 0, 0, 255)), 0.f, true);
	// AS_FX_TX.HUD.AssaultRadar in four mirrored quarters, in the viewer's team colour
	const TCHAR* Radar = TEXT("Textures/AS_FX_TX/HUD_AssaultRadar");
	const FLinearColor TC(Colors::Team(F.TeamIndex));
	DrawTilePath(D, Radar, 512.f, 512.f, PosX - RadarWidth, PosY + RadarWidth, RadarWidth, RadarWidth, 0.f, 512.f, 512.f, -512.f, TC);
	DrawTilePath(D, Radar, 512.f, 512.f, PosX, PosY + RadarWidth, RadarWidth, RadarWidth, 512.f, 512.f, -512.f, -512.f, TC);
	DrawTilePath(D, Radar, 512.f, 512.f, PosX - RadarWidth, PosY, RadarWidth, RadarWidth, 0.f, 0.f, 512.f, 512.f, TC);
	DrawTilePath(D, Radar, 512.f, 512.f, PosX, PosY, RadarWidth, RadarWidth, 512.f, 0.f, -512.f, 512.f, TC);
	// ShowTeamScorePassC: a dot per monster within 3000 (yellow, brightest on the pulse front), SkinA 838,238 144 x 144
	if (!PlayerOwner)
	{
		return;
	}
	FVector ViewLoc;
	FRotator ViewRot;
	PlayerOwner->GetPlayerViewPoint(ViewLoc, ViewRot);
	const APawn* Pawn = PlayerOwner->GetPawn();
	const FVector Start = Pawn ? Pawn->GetActorLocation() : ViewLoc;
	const float DotSize = 24.f * ClipX * F.HudScale / 1600.f;
	const float OffsetY = PosY + RadarWidth;
	const float MaxDist = 3000.f * RadarPulse;
	for (const FVector& M : Monsters)
	{
		const float Dist = FVector::Dist(Start, M) / UUToCm;
		if (Dist >= 3000.f)
		{
			continue;
		}
		const float Brightness = Dist < MaxDist ? 255.f - 255.f * FMath::Abs(Dist * 0.00033f - RadarPulse)
			: 255.f - 255.f * FMath::Abs(Dist * 0.00033f - RadarPulse - 1.f);
		const uint8 B = uint8(FMath::Clamp(Brightness, 0.f, 255.f));
		const float OffsetScale = RadarScale * Dist * 0.00017f;
		const float Angle = UTRelativeYaw(M);
		const float X = PosX + OffsetScale * ClipX * FMath::Sin(Angle) - 0.5f * DotSize;
		const float Y = OffsetY - OffsetScale * ClipX * FMath::Cos(Angle) - 0.5f * DotSize;
		DrawTilePath(D, Skin, 1024.f, 1024.f, X, Y, DotSize, DotSize, 838.f, 238.f, 144.f, 144.f, FLinearColor(FColor(B, B, 0, 255)), 0.f, true);
	}
}

// =============================================================================================
// HudBase.DrawTargeting
// =============================================================================================

void AArenaHUD::DrawUTTargeting(const FVector& WorldLocation)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTModesLocal;
	if (!PlayerOwner || !Canvas)
	{
		return;
	}
	FVector ViewLoc;
	FRotator ViewRot;
	PlayerOwner->GetPlayerViewPoint(ViewLoc, ViewRot);
	const FVector Dir = (WorldLocation - ViewLoc).GetSafeNormal();
	FVector2D P;
	if (FVector::DotProduct(Dir, ViewRot.Vector()) <= 0.6f || !UTProjectPoint(Canvas, WorldLocation, P))
	{
		return;
	}
	// TargetingSize 1 (BallLauncher): 96 x RatioX (= SizeX / 640, the 4:3 frame here), Reticle001 turning 26000 a second
	const float Size = 96.f * UTFrame.FrameW / 640.f;
	const float Rot = FMath::Fmod(Now() * 26000.f / 65536.f * 360.f, 360.f);
	DrawTilePath(D, TEXT("Textures/HudContent/Reticles_Reticle001"), 256.f, 256.f, P.X - Size * 0.5f, P.Y - Size * 0.5f, Size, Size, 0.f, 0.f, 256.f, 256.f,
		FLinearColor::White, Rot, true);
}

// =============================================================================================
// HudMutant.DrawHudPassA
// =============================================================================================

void AArenaHUD::DrawUTMutant(const FString& MutantName, const FVector* MutantLocation, bool bBottomFeeder)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTModesLocal;
	const FHudFrame& F = UTFrame;
	// ClipX fractions use the 4:3 frame width around the centre, ClipY fractions the height
	const float CW = F.FrameW;
	auto X = [&](float Frac) { return F.W * 0.5f + (Frac - 0.5f) * CW; };
	if (!MutantName.IsEmpty())
	{
		// CenterRadarBG: MutantSkins.HUD.big_circle (mirrored), TextureScale 0.8, centred, STY_Translucent
		const float S = 0.8f * F.TotalScale;
		const float Size = 256.f * S;
		DrawTilePath(D, TEXT("Textures/MutantSkins/HUD_big_circle"), 256.f, 256.f, F.W * 0.5f - Size * 0.5f, F.H * 0.5f - Size * 0.5f, Size, Size,
			255.f, 0.f, -255.f, 255.f, FLinearColor::White, 0.f, true);
		if (MutantLocation && PlayerOwner)
		{
			const APawn* Pawn = PlayerOwner->GetPawn();
			FVector ViewLoc;
			FRotator ViewRot;
			PlayerOwner->GetPlayerViewPoint(ViewLoc, ViewRot);
			const FVector Me = Pawn ? Pawn->GetActorLocation() : ViewLoc;
			const float VertDiff = (MutantLocation->Z - Me.Z) / UUToCm;
			// LevelRampRegion 500: below red, level white, above blue
			FColor Col;
			const FColor Above(0, 0, 255, 255), Level(255, 255, 255, 255), Below(255, 0, 0, 255);
			auto Lerp = [](float A, const FColor& C0, const FColor& C1)
			{
				return FColor(uint8(FMath::Lerp(float(C0.R), float(C1.R), A)), uint8(FMath::Lerp(float(C0.G), float(C1.G), A)), uint8(FMath::Lerp(float(C0.B), float(C1.B), A)), 255);
			};
			if (VertDiff > 500.f) Col = Above;
			else if (VertDiff > 0.f) Col = Lerp(VertDiff / 500.f, Level, Above);
			else if (VertDiff > -500.f) Col = Lerp(-VertDiff / 500.f, Level, Below);
			else Col = Below;
			const float Angle = UTRelativeYaw(*MutantLocation);
			const float Range = FMath::Max(0.f, FVector::Dist(*MutantLocation, Me) / UUToCm - 2.f * 25.f);
			// XCen 0.5, XRad 0.15, YCen 0.5, YRad 0.2, BigDotSize 0.01875, SmallDotSize 0.01125 (of ClipX)
			const float Big = 0.01875f * CW, Small = 0.01125f * CW;
			const TCHAR* Skin = TexPath(ETex::SkinA);
			auto Dot = [&](float A, float Size, const FColor& C)
			{
				const float DX = X(0.5f) + F.HudScale * 0.15f * CW * FMath::Sin(A) - 0.5f * Size;
				const float DY = 0.5f * F.H - F.HudScale * 0.2f * F.H * FMath::Cos(A) - 0.5f * Size;
				DrawTilePath(D, Skin, 1024.f, 1024.f, DX, DY, Size, Size, 838.f, 238.f, 144.f, 144.f, FLinearColor(C));
			};
			Dot(Angle, Big, Col);
			// RangeRampRegion 2500, MaxAngleDelta 1: two range dots closing in on the big one
			const float Delta = FMath::Min(2500.f, Range) / 2500.f * 1.f;
			Dot(Angle + Delta, Small, FColor(200, 200, 50, 255));
			Dot(Angle - Delta, Small, FColor(200, 200, 50, 255));
		}
		// the Mutant's name: BorderBoxD at MNOrigin (0.28, 0.008), MNSize (0.45, 0.06), LevelActionFont, CurrentMutantColor
		DrawTileStretched(D, TEXT("Textures/InterfaceContent/Menu_BorderBoxD"), X(0.28f), 0.008f * F.H, 0.45f * CW, 0.06f * F.H, FLinearColor::White);
		const int32 Font = ArenaHudFonts::MediumFontIndex(D.UTClipX());
		const FVector2D TS = D.TextSizeUT(Font, MutantName);
		// DrawTextJustified(name, centred, 0.3 .. 0.7 x 0.015 .. 0.065)
		D.TextUT(Font, MutantName, X(0.5f) - TS.X * 0.5f, 0.04f * F.H - TS.Y * 0.5f, FLinearColor(FColor(255, 0, 128, 255)));
	}
	if (bBottomFeeder)
	{
		// BFIOrigin (0.9, 0.64), BFISize (0.09, 0.1), BFIMargin 0.01: BorderBoxD and the pulsing bottom feeder icon
		const float BX = F.RegionX0 + F.RegionW - 0.1f * CW;
		DrawTileStretched(D, TEXT("Textures/InterfaceContent/Menu_BorderBoxD"), BX, 0.64f * F.H, 0.09f * CW, 0.1f * F.H, FLinearColor::White);
		const uint8 R = uint8(255.f * (0.5f + 0.5f * FMath::Cos(Now() * PI * 1.f)));
		DrawTilePath(D, TEXT("Textures/MutantSkins/HUD_BFeeder_icon"), 256.f, 256.f, BX + 0.01f * CW, (0.64f + 0.01f) * F.H, (0.09f - 0.02f) * CW,
			(0.1f - 0.02f) * F.H, 0.f, 0.f, 256.f, 256.f, FLinearColor(FColor(R, 255, 255, 255)));
	}
}

// =============================================================================================
// HUD_Assault.DrawVisibleObjective
// =============================================================================================

void AArenaHUD::DrawUTObjectiveMarker(const FVector& Location, int32 Type, int32 DefenderTeam, float Progress, int32 Key)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDUTModesLocal;
	UWorld* World = GetWorld();
	if (!PlayerOwner || !Canvas || !World)
	{
		return;
	}
	const FHudFrame& F = UTFrame;
	FVector CamLoc;
	FRotator CamRot;
	PlayerOwner->GetPlayerViewPoint(CamLoc, CamRot);
	FVector2D P;
	if (FVector::DotProduct(Location - CamLoc, CamRot.Vector()) <= 0.f || !UTProjectPoint(Canvas, Location, P)
		|| P.X <= 0.f || P.X >= F.W || P.Y <= 0.f || P.Y >= F.H)
	{
		return;
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(UTObjectiveSight), false, PlayerOwner->GetPawn());
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, CamLoc, Location, ECC_Visibility, Q) && FVector::Dist(Hit.ImpactPoint, Location) > 200.f)
	{
		return;
	}
	// non-use objectives: drawn OBJ_FadedOutTime (6 s, fading in the last second), then hidden until OBJ_ReAppearTime (7 s)
	const float Cycle = FMath::Fmod(Now() + Key * 1.37f, 7.f);
	float Fade = 1.f;
	if (Type != 3)
	{
		if (Cycle > 6.f)
		{
			return;
		}
		Fade = Cycle > 5.f ? 6.f - Cycle : 1.f;
	}
	const FColor TeamColor = DefenderTeam == 0 ? Colors::HudRed : (DefenderTeam == 1 ? Colors::HudBlue : Colors::Green);
	FColor ObjColor = ColorMul(TeamColor, Fade);
	ObjColor.A = uint8(127.f * Fade);
	// AppearEffect = 0.5 / FMin(DrawTime, 0.5): the icon shrinks in from big during its first half second
	const float Appear = 0.5f / FMath::Max(FMath::Min(Cycle, 0.5f), 0.05f);
	const float Res = F.FrameW / 640.f;
	const float Tile = 64.f * Res * FMath::Min(Appear, 4.f) * F.HudScale;
	// ObjectiveTypeIcon of the stock objective classes
	static const TCHAR* Icons[4] = {
		TEXT("Textures/AS_FX_TX/Icons_OBJ_Proximity_Tex"), // ProximityObjective
		TEXT("Textures/AS_FX_TX/Icons_HoldObjective"),     // HoldObjective / TriggeredObjective
		TEXT("Textures/AS_FX_TX/Icons_OBJ_Destroy_Tex"),   // DestroyableObjective
		TEXT("Textures/AS_FX_TX/Icons_OBJ_Use_Tex"),       // UseObjective (UT2004 drew a bracket box around it)
	};
	DrawTilePath(D, Icons[FMath::Clamp(Type, 0, 3)], 64.f, 64.f, P.X - Tile * 0.5f, P.Y - Tile * 0.5f, Tile, Tile, 0.f, 0.f, 64.f, 64.f, FLinearColor(ObjColor));
	// defenders: the green status ring (AS_FX_TX.HUD.OBJ_Status 191,191 64 x 64, 1.25 x)
	const AArenaPlayerState* MyPS = PlayerOwner->GetPlayerState<AArenaPlayerState>();
	if (MyPS && MyPS->Team == DefenderTeam)
	{
		const float S = 1.25f * F.HudScale * Res * 64.f;
		FColor G = Colors::Green;
		G.A = uint8(127.f * Fade);
		DrawTilePath(D, TEXT("Textures/AS_FX_TX/HUD_OBJ_Status"), 256.f, 256.f, P.X - S * 0.5f, P.Y - S * 0.5f, S, S, 191.f, 191.f, 64.f, 64.f, FLinearColor(G));
	}
	if (Progress > 0.f)
	{
		// DrawHealthBar: BorderBoxD 80 x 8 (x ResScale x HudScale) in the objective colour, the bar in the green-yellow-red ramp
		const float BW = 80.f * Res * F.HudScale, BH = 8.f * Res * F.HudScale;
		const float BY = P.Y - 48.f * Res;
		DrawTileStretched(D, TEXT("Textures/InterfaceContent/Menu_BorderBoxD"), P.X - BW * 0.5f, BY - BH * 0.5f, BW, BH, FLinearColor(ObjColor));
		const float Pct = FMath::Clamp(Progress, 0.f, 1.f);
		FColor Ramp;
		if (Pct < 0.34f)
		{
			Ramp = FColor(uint8(128 + 127 * FMath::Clamp(3.f * Pct, 0.f, 1.f)), 0, 0, 200);
		}
		else if (Pct < 0.67f)
		{
			Ramp = FColor(255, uint8(255 * FMath::Clamp(3.f * (Pct - 0.33f), 0.f, 1.f)), 0, 200);
		}
		else
		{
			Ramp = FColor(uint8(255 * FMath::Clamp(3.f * (1.f - Pct), 0.f, 1.f)), 255, 0, 200);
		}
		DrawTileStretched(D, TEXT("Textures/ONSInterface-TX/HealthBar"), P.X - BW * 0.49f, BY - BH * 0.3f, BW * Pct * 0.98f, BH * 0.6f, FLinearColor(Ramp));
	}
}
