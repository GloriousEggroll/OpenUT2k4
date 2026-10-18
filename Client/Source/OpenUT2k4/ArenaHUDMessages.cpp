// AArenaHUD: UT2004 local messages and console kill lines (layout rules in ArenaLocalMessages.h).
#include "ArenaHUD.h"
#include "ArenaChat.h"
#include "ArenaHudFonts.h"
#include "ArenaLocalMessages.h"
#include "ArenaPlayerController.h"

#include "Engine/Canvas.h"

namespace ArenaHUDMessagesLocal
{
	/** HudBase.GetScreenCoords: HudCanvasScale placement, then the DrawPivot offset (EDrawPivot 0..8). */
	FVector2D ScreenCoords(float PosX, float PosY, uint8 Pivot, const FVector2D& Size, float ClipX, float ClipY)
	{
		using namespace ArenaLocalMessages;
		float X = PosX * HudCanvasScale * ClipX + (1.f - HudCanvasScale) * 0.5f * ClipX;
		float Y = PosY * HudCanvasScale * ClipY + (1.f - HudCanvasScale) * 0.5f * ClipY;
		switch (Pivot)
		{
		case 1: X -= Size.X * 0.5f; break;                          // upper middle
		case 2: X -= Size.X; break;                                 // upper right
		case 3: X -= Size.X; Y -= Size.Y * 0.5f; break;             // middle right
		case 4: X -= Size.X; Y -= Size.Y; break;                    // lower right
		case 5: X -= Size.X * 0.5f; Y -= Size.Y; break;             // lower middle
		case 6: Y -= Size.Y; break;                                 // lower left
		case 7: Y -= Size.Y * 0.5f; break;                          // middle left
		case 8: X -= Size.X * 0.5f; Y -= Size.Y * 0.5f; break;      // middle middle
		default: break;                                             // upper left
		}
		return FVector2D(X, Y);
	}
}

void AArenaHUD::DrawLocalMessages()
{
	using namespace ArenaLocalMessages;
	using namespace ArenaHUDMessagesLocal;
	const AArenaPlayerController* PC = GetArenaPC();
	if (!PC || !Canvas)
	{
		return;
	}
	const TArray<FArenaCenterMessage>& Msgs = PC->GetLocalMessages();
	if (Msgs.Num() == 0)
	{
		return;
	}
	const float ClipX = D.W();
	const float ClipY = D.H();
	const float T = Now();

	// LayoutMessage: font from the message's font size and the canvas width, text size in that font
	struct FLaidOut
	{
		int32 Font = 0;
		FVector2D Size = FVector2D::ZeroVector;
		bool bDrawn = false;
	};
	TArray<FLaidOut> Laid;
	Laid.SetNum(Msgs.Num());
	for (int32 i = 0; i < Msgs.Num(); ++i)
	{
		Laid[i].Font = ArenaHudFonts::FontSizeIndex(D.UTClipX(), Msgs[i].FontSize); // font by screen height (FArenaDraw::UTClipX)
		Laid[i].Size = D.TextSizeUT(Laid[i].Font, Msgs[i].Text);
	}

	// DrawMessage; returns the text height as a fraction of the canvas (the stack step)
	auto DrawOne = [&](int32 i, float PosX, float PosY) -> float
	{
		const FArenaCenterMessage& M = Msgs[i];
		const FStyle& Style = M.GetStyle();
		float Alpha = 1.f;
		if (Style.bFadeMessage && M.Duration > 0.f)
		{
			Alpha = FMath::Clamp((M.Time + M.Duration - T) / M.Duration, 0.f, 1.f);
		}
		const FVector2D P = ScreenCoords(PosX, PosY, Style.DrawPivot, Laid[i].Size, ClipX, ClipY);
		D.TextUT(Laid[i].Font, M.Text, P.X, P.Y, FArenaDraw::UTColor(Style.DrawColor, Alpha));
		Laid[i].bDrawn = true;
		return Laid[i].Size.Y / ClipY;
	};

	// DisplayLocalMessages pass 2: messages sharing position, pivot and stack mode form one stack, oldest first
	for (int32 i = 0; i < Msgs.Num(); ++i)
	{
		if (Laid[i].bDrawn)
		{
			continue;
		}
		const FStyle& First = Msgs[i].GetStyle();
		const float PosX = First.PosX;
		float PosY = First.PosY;
		if (First.StackMode == EStackMode::None)
		{
			DrawOne(i, PosX, PosY);
			continue;
		}
		for (int32 j = i; j < Msgs.Num(); ++j)
		{
			const FStyle& S = Msgs[j].GetStyle();
			if (Laid[j].bDrawn || S.PosX != First.PosX || S.PosY != First.PosY || S.DrawPivot != First.DrawPivot || S.StackMode != First.StackMode)
			{
				continue;
			}
			const float Step = DrawOne(j, PosX, PosY);
			PosY += S.StackMode == EStackMode::Up ? -Step : Step;
		}
	}
}

void AArenaHUD::DrawKillFeed()
{
	using namespace ArenaLocalMessages;
	const AArenaPlayerController* PC = GetArenaPC();
	if (!PC || !Canvas)
	{
		return;
	}
	// HUD.DisplayMessages: the live console lines (the player controller drops them after the message lifetime)
	TArray<const FArenaKillMessage*> Lines;
	for (const FArenaKillMessage& M : PC->GetKillMessages())
	{
		if (!M.ConsoleText.IsEmpty())
		{
			Lines.Add(&M);
		}
	}
	while (Lines.Num() > ConsoleMessageCount)
	{
		Lines.RemoveAt(0);
	}
	const bool bTyping = ArenaChat::IsTyping();
	if (Lines.Num() == 0 && !bTyping)
	{
		return;
	}
	const float SizeX = D.W();
	const float SizeY = D.H();
	// UnrealScript int / float conversions as in the original (truncation)
	// x in the 4:3 frame of the screen height from the left edge, like the other left anchored HUD widgets (ArenaHUDWidgets.h)
	float RegionX0 = 0.f, RegionW = SizeX;
	ArenaHudUT::HudRegion(SizeX, SizeY, RegionX0, RegionW); // Settings > HUD Width
	const float FrameX = FMath::Min(RegionW, SizeY * 4.f / 3.f);
	const int32 XPos = int32(RegionX0) + int32(float(int32(ConsoleMessagePosX * HudCanvasScale * FrameX)) + (1.f - HudCanvasScale) / 2.f * FrameX);
	int32 YPos = int32(float(int32(ConsoleMessagePosY * HudCanvasScale * SizeY)) + (1.f - HudCanvasScale) / 2.f * SizeY);
	const int32 Font = ArenaHudFonts::ConsoleFontIndex(D.UTClipX());
	const float YL = D.TextSizeUT(Font, TEXT("A")).Y;
	YPos -= int32(YL * float(Lines.Num()) + 1.f);
	YPos -= int32(YL);
	const FLinearColor Color = FArenaDraw::UTColor(ConsoleKillColor());
	for (const FArenaKillMessage* M : Lines)
	{
		D.TextUT(Font, M->ConsoleText, float(XPos), float(YPos), M->ConsoleColor.A > 0 ? FArenaDraw::UTColor(M->ConsoleColor) : Color);
		YPos += int32(YL);
	}
	if (bTyping)
	{
		// HUD.DrawTypingPrompt: one line above ConsoleMessagePosY (under the messages), HUD.ConsoleColor
		D.TextUT(Font, ArenaChat::GetPrompt(), float(XPos), float(YPos), FArenaDraw::UTColor(FColor(153, 216, 253, 255)));
	}
}
