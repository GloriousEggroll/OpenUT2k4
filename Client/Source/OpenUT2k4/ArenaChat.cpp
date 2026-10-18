// In-game chat: the UT2004 typing line, Say / TeamSay routing and chat lines (see ArenaChat.h).
#include "ArenaChat.h"
#include "ArenaGameState.h"
#include "ArenaPlayerController.h"
#include "ArenaPlayerState.h"
#include "ArenaWorldZones.h"
#include "OpenUT2k4.h"
#include "Net/UE2NetPlay.h"
#include "Net/UE2ServerWorld.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerState.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SLeafWidget.h"

namespace ArenaChatLocal
{
	/** Console.History: the last 16 lines typed. */
	constexpr int32 HistorySize = 16;

	struct FState
	{
		TWeakObjectPtr<AArenaPlayerController> PC;
		bool bTyping = false;
		FString TypedStr;
		int32 TypedStrPos = 0;
		TArray<FString> History;
		int32 HistoryCur = -1;
		/** Console.bIgnoreKeys: characters of the key that opened the line are dropped until the next key press. */
		bool bIgnoreChars = false;
		TSharedPtr<SWidget> Sink;
		TSharedPtr<SWidget> SinkHolder;
	};

	FState& State()
	{
		static FState S;
		return S;
	}

	/** Invisible widget that holds keyboard focus while the typing line is open (characters with the keyboard layout and IME). */
	class SArenaChatSink : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SArenaChatSink) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&) {}
		virtual bool SupportsKeyboardFocus() const override { return true; }
		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1.f, 1.f); }
		virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&, int32 LayerId, const FWidgetStyle&, bool) const override
		{
			return LayerId;
		}
		virtual FReply OnKeyDown(const FGeometry&, const FKeyEvent& Event) override
		{
			// the console (~) and function keys stay with the game
			const FKey K = Event.GetKey();
			static const FKey PassThrough[] = { EKeys::Tilde, EKeys::F1, EKeys::F2, EKeys::F3, EKeys::F4, EKeys::F5, EKeys::F6, EKeys::F7,
				EKeys::F8, EKeys::F9, EKeys::F10, EKeys::F11, EKeys::F12 };
			bool bPass = K.IsModifierKey();
			for (const FKey& P : PassThrough)
			{
				bPass |= K == P;
			}
			if (bPass)
			{
				return FReply::Unhandled();
			}
			ArenaChat::HandleKey(K);
			return FReply::Handled();
		}
		virtual FReply OnKeyUp(const FGeometry&, const FKeyEvent&) override { return FReply::Handled(); }
		virtual FReply OnKeyChar(const FGeometry&, const FCharacterEvent& Event) override
		{
			if (!Event.GetModifierKeys().IsControlDown() && !Event.GetModifierKeys().IsCommandDown())
			{
				ArenaChat::HandleChar(Event.GetCharacter());
			}
			return FReply::Handled();
		}
		virtual void OnFocusLost(const FFocusEvent&) override
		{
			// another window or widget took the keyboard (alt-tab, a menu): the line stays open, focus comes back on the next frame
		}
	};

	void AddSink()
	{
		FState& S = State();
		if (!FSlateApplication::IsInitialized() || !GEngine || !GEngine->GameViewport)
		{
			return;
		}
		if (!S.Sink.IsValid())
		{
			S.Sink = SNew(SArenaChatSink);
			S.SinkHolder = SNew(SBox).HAlign(HAlign_Left).VAlign(VAlign_Top).WidthOverride(1.f).HeightOverride(1.f)[S.Sink.ToSharedRef()];
			GEngine->GameViewport->AddViewportWidgetContent(S.SinkHolder.ToSharedRef(), 1001);
		}
		FSlateApplication::Get().SetAllUserFocus(S.Sink, EFocusCause::SetDirectly);
	}

	void RemoveSink()
	{
		FState& S = State();
		if (S.Sink.IsValid() && FSlateApplication::IsInitialized() && GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->RemoveViewportWidgetContent(S.SinkHolder.ToSharedRef());
			FSlateApplication::Get().SetAllUserFocusToGameViewport();
		}
		S.Sink.Reset();
		S.SinkHolder.Reset();
	}

	/** GameInfo.StripColor: removes the colour codes (chr(27) and three colour bytes). */
	FString StripColor(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (int32 i = 0; i < In.Len(); ++i)
		{
			if (In[i] == TCHAR(27))
			{
				i += 3;
				continue;
			}
			Out.AppendChar(In[i]);
		}
		return Out;
	}

	/** Test runs (other agents, scripts) must not chat on public servers. */
	bool IsTestRun()
	{
		const TCHAR* Cmd = FCommandLine::Get();
		return FCString::Stristr(Cmd, TEXT("-saveddirsuffix=")) || FCString::Stristr(Cmd, TEXT("AutoMenuScript=")) || FParse::Param(Cmd, TEXT("unattended"));
	}

	/** Keeps the typing line focused while it is open (a menu or alt-tab may take focus). */
	struct FFocusTicker
	{
		FFocusTicker()
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				FState& S = State();
				if (!S.bTyping)
				{
					return true;
				}
				AArenaPlayerController* PC = S.PC.Get();
				if (!PC || !PC->GetWorld() || PC->IsPauseMenuOpen())
				{
					ArenaChat::CloseTyping();
					return true;
				}
				if (S.Sink.IsValid() && FSlateApplication::IsInitialized() && !S.Sink->HasKeyboardFocus())
				{
					const TSharedPtr<SWindow> Active = FSlateApplication::Get().GetActiveTopLevelWindow();
					if (Active.IsValid() && GEngine && GEngine->GameViewport && Active == GEngine->GameViewport->GetWindow())
					{
						FSlateApplication::Get().SetAllUserFocus(S.Sink, EFocusCause::SetDirectly);
					}
				}
				return true;
			}));
		}
	};
	static FFocusTicker GFocusTicker;
}

using namespace ArenaChatLocal;

bool ArenaChat::IsLocalAddress(const FString& Address)
{
	FString Host = Address;
	int32 Colon = INDEX_NONE;
	if (Host.FindLastChar(TEXT(':'), Colon) && Host.Find(TEXT(":")) == Colon)
	{
		Host = Host.Left(Colon);
	}
	Host.TrimStartAndEndInline();
	if (Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	TArray<FString> Parts;
	Host.ParseIntoArray(Parts, TEXT("."));
	if (Parts.Num() != 4)
	{
		return false;
	}
	int32 B[4];
	for (int32 i = 0; i < 4; ++i)
	{
		if (!Parts[i].IsNumeric())
		{
			return false;
		}
		B[i] = FCString::Atoi(*Parts[i]);
	}
	return B[0] == 127 || B[0] == 10 || (B[0] == 192 && B[1] == 168) || (B[0] == 172 && B[1] >= 16 && B[1] <= 31) || (B[0] == 169 && B[1] == 254);
}

FString ArenaChat::GetLocationName(const APlayerState* PS)
{
	// PlayerReplicationInfo.GetLocationName: the pawn's volume / zone LocationName, else Dead / Spectating
	const APawn* Pawn = PS ? PS->GetPawn() : nullptr;
	if (!Pawn)
	{
		const AArenaPlayerState* APS = Cast<AArenaPlayerState>(PS);
		return APS && !APS->IsSpectator() && !APS->IsOnlyASpectator() ? FString(TEXT("Dead")) : FString(TEXT("Spectating"));
	}
	const UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(Pawn);
	return Zones ? Zones->GetLocationNameAt(Pawn->GetActorLocation()) : FString();
}

void ArenaChat::ShowChat(AArenaPlayerController* PC, const APlayerState* From, const FString& Text, bool bTeam, const FString* Location)
{
	if (!PC || Text.IsEmpty())
	{
		return;
	}
	// HudBase.Message: 'Say' -> SayMessagePlus "Name: text", 'TeamSay' -> TeamSayMessagePlus "Name(location): text";
	// console colours SayMessagePlus.GetConsoleColor (DrawColor yellow, RedTeamColor / BlueTeamColor for team
	// members) and TeamSayMessagePlus.DrawColor (light green), as the 3374 client shows them
	const AArenaPlayerState* APS = Cast<AArenaPlayerState>(From);
	const AArenaGameState* GS = PC->GetWorld() ? PC->GetWorld()->GetGameState<AArenaGameState>() : nullptr;
	const int32 Team = APS && GS && GS->IsTeamGame() ? APS->Team : -1;
	FString Line = Text;
	FColor Color(255, 255, 0, 255);
	if (bTeam)
	{
		const FString Loc = Location ? *Location : GetLocationName(From);
		if (From)
		{
			Line = From->GetPlayerName() + (Loc.IsEmpty() ? FString() : TEXT("(") + Loc + TEXT(")")) + TEXT(": ") + Text;
		}
		Color = FColor(128, 255, 128, 255);
	}
	else
	{
		if (From)
		{
			Line = From->GetPlayerName() + TEXT(": ") + Text;
		}
		if (Team == 0)
		{
			Color = FColor(255, 64, 64, 255);
		}
		else if (Team == 1)
		{
			Color = FColor(64, 192, 255, 255);
		}
	}
	PC->AddConsoleLine(Line, Color);
}

void ArenaChat::Send(AArenaPlayerController* PC, const FString& InText, bool bTeam)
{
	if (!PC)
	{
		return;
	}
	const FString Text = StripColor(InText.Left(128)).TrimStartAndEnd();
	if (Text.IsEmpty())
	{
		return;
	}
	// a UT2004 server we play on
	if (FUE2NetPlay* Net = FUE2NetPlay::Get())
	{
		const FString Address = Net->GetServerAddress();
		if (IsTestRun() && !IsLocalAddress(Address))
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Chat: not sent to %s (test run on a public server)"), *Address);
			return;
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Chat: %s%s to the UT2004 server"), *Text, bTeam ? TEXT(" (team)") : TEXT(""));
		Net->Say(Text, bTeam);
		return;
	}
	// PlayerController.ServerTeamSay: team chat is plain chat in a game without teams
	const AArenaGameState* GS = PC->GetWorld() ? PC->GetWorld()->GetGameState<AArenaGameState>() : nullptr;
	const bool bTeamSay = bTeam && GS && GS->IsTeamGame();
	APlayerState* PS = PC->PlayerState;
	UE_LOG(LogOpenUT2k4, Log, TEXT("Chat: %s: %s%s"), PS ? *PS->GetPlayerName() : TEXT("?"), *Text, bTeamSay ? TEXT(" (team)") : TEXT(""));
	// hosting UT2004 clients: they get the line (BroadcastHandler.Broadcast / BroadcastTeam)
	UE2ServerHooks::Chat(PS, Text, bTeamSay);
	// local players (split screen has one): the speaker and, for team chat, the speaker's team
	if (UWorld* W = PC->GetWorld())
	{
		const AArenaPlayerState* MyPS = Cast<AArenaPlayerState>(PS);
		for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It; ++It)
		{
			AArenaPlayerController* Local = Cast<AArenaPlayerController>(It->Get());
			if (!Local || !Local->IsLocalController())
			{
				continue;
			}
			const AArenaPlayerState* LPS = Local->GetPlayerState<AArenaPlayerState>();
			if (bTeamSay && Local != PC && (!LPS || !MyPS || LPS->Team != MyPS->Team))
			{
				continue;
			}
			ShowChat(Local, PS, Text, bTeamSay);
		}
	}
}

void ArenaChat::OpenTyping(AArenaPlayerController* PC, bool bTeam)
{
	if (!PC || !PC->IsLocalController())
	{
		return;
	}
	FState& S = State();
	S.PC = PC;
	S.bTyping = true;
	S.TypedStr = bTeam ? TEXT("TeamSay ") : TEXT("Say ");
	S.TypedStrPos = S.TypedStr.Len();
	S.HistoryCur = S.History.Num();
	S.bIgnoreChars = true;
	// keys held when the line opens would stay down for the game (their release goes to the typing line)
	PC->FlushPressedKeys();
	AddSink();
}

void ArenaChat::CloseTyping()
{
	FState& S = State();
	if (!S.bTyping)
	{
		return;
	}
	S.bTyping = false;
	S.TypedStr.Reset();
	S.TypedStrPos = 0;
	RemoveSink();
}

bool ArenaChat::IsTyping()
{
	return State().bTyping;
}

FString ArenaChat::GetPrompt()
{
	const FState& S = State();
	// HUD.DrawTypingPrompt: "(>" @ Left(Text, Pos) $ chr(4) $ (Pos < Len ? Mid(Text, Pos) : "_"); chr(4) has no glyph
	const int32 Pos = FMath::Clamp(S.TypedStrPos, 0, S.TypedStr.Len());
	return TEXT("(> ") + S.TypedStr.Left(Pos) + (Pos < S.TypedStr.Len() ? S.TypedStr.Mid(Pos) : FString(TEXT("_")));
}

void ArenaChat::HandleChar(TCHAR Char)
{
	FState& S = State();
	if (!S.bTyping || S.bIgnoreChars || Char < 0x20 || Char == 0x7f)
	{
		return;
	}
	if (S.TypedStr.Len() >= 512)
	{
		return;
	}
	S.TypedStr.InsertAt(S.TypedStrPos, Char);
	++S.TypedStrPos;
}

bool ArenaChat::HandleKey(const FKey& Key)
{
	FState& S = State();
	if (!S.bTyping)
	{
		return false;
	}
	S.bIgnoreChars = false;
	if (Key == EKeys::Escape)
	{
		if (!S.TypedStr.IsEmpty())
		{
			S.TypedStr.Reset();
			S.TypedStrPos = 0;
			S.HistoryCur = S.History.Num();
		}
		else
		{
			CloseTyping();
		}
	}
	else if (Key == EKeys::Enter || Key == EKeys::Gamepad_FaceButton_Bottom)
	{
		const FString Line = S.TypedStr.TrimStartAndEnd();
		AArenaPlayerController* PC = S.PC.Get();
		CloseTyping();
		if (!Line.IsEmpty())
		{
			S.History.Add(Line);
			if (S.History.Num() > HistorySize)
			{
				S.History.RemoveAt(0);
			}
			if (PC)
			{
				// Console.ConsoleCommand(TypedStr): Say / TeamSay go straight to Send (the engine's console commands of
				// the same name would take them first), anything else runs as a console command
				FString Word, Rest;
				if (!Line.Split(TEXT(" "), &Word, &Rest))
				{
					Word = Line;
				}
				if (Word.Equals(TEXT("Say"), ESearchCase::IgnoreCase) || Word.Equals(TEXT("TeamSay"), ESearchCase::IgnoreCase))
				{
					Send(PC, Rest, Word.Equals(TEXT("TeamSay"), ESearchCase::IgnoreCase));
				}
				else
				{
					PC->ConsoleCommand(Line, true);
				}
			}
		}
	}
	else if (Key == EKeys::Up || Key == EKeys::Down)
	{
		if (S.History.Num() > 0)
		{
			S.HistoryCur = Key == EKeys::Up ? FMath::Max(0, S.HistoryCur - 1) : FMath::Min(S.History.Num(), S.HistoryCur + 1);
			S.TypedStr = S.History.IsValidIndex(S.HistoryCur) ? S.History[S.HistoryCur] : FString();
			S.TypedStrPos = S.TypedStr.Len();
		}
	}
	else if (Key == EKeys::BackSpace)
	{
		if (S.TypedStrPos > 0)
		{
			S.TypedStr.RemoveAt(S.TypedStrPos - 1);
			--S.TypedStrPos;
		}
	}
	else if (Key == EKeys::Delete)
	{
		if (S.TypedStrPos < S.TypedStr.Len())
		{
			S.TypedStr.RemoveAt(S.TypedStrPos);
		}
	}
	else if (Key == EKeys::Left)
	{
		S.TypedStrPos = FMath::Max(0, S.TypedStrPos - 1);
	}
	else if (Key == EKeys::Right)
	{
		S.TypedStrPos = FMath::Min(S.TypedStr.Len(), S.TypedStrPos + 1);
	}
	else if (Key == EKeys::Home)
	{
		S.TypedStrPos = 0;
	}
	else if (Key == EKeys::End)
	{
		S.TypedStrPos = S.TypedStr.Len();
	}
	return true;
}
