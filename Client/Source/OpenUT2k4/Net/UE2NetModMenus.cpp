// Mods' menus on our side: drawing, input routing and mod key binds. See UE2NetModMenus.h.

#include "UE2NetModMenus.h"
#include "UE2NetLog.h"
#include "UE2NetPlay.h"
#include "UE2NetScriptHUD.h"
#include "../ArenaCharacter.h"
#include "../UScript/UScriptRuntime.h"
#include "../ArenaChat.h"
#include "../ArenaPlayerController.h"
#include "Engine/Console.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/HUD.h"
#include "HAL/IConsoleManager.h"

namespace UE2NetModMenusLocal
{
	// UE2 EInputAction
	constexpr uint8 IST_Press = 1;
	constexpr uint8 IST_Release = 3;

	TWeakObjectPtr<AArenaPlayerController> GPC;
	bool bOpenLast = false;

	bool ConsoleOpen()
	{
		const UGameViewportClient* VC = GEngine ? GEngine->GameViewport : nullptr;
		return VC && VC->ViewportConsole && VC->ViewportConsole->ConsoleActive();
	}

	/** The character a key types (US layout; letters by Shift). 0 for none. */
	TCHAR CharFor(const FKeyEvent& E)
	{
		const FKey K = E.GetKey();
		const bool bShift = E.IsShiftDown();
		if (E.IsControlDown() || E.IsCommandDown() || E.IsAltDown())
		{
			return 0;
		}
		// letters (the key's character is upper or lower case depending on the platform)
		const uint32 C = E.GetCharacter();
		const uint32 Upper = C >= 'a' && C <= 'z' ? C - 'a' + 'A' : C;
		if (Upper >= 'A' && Upper <= 'Z' && K.GetFName().ToString().Len() == 1)
		{
			return TCHAR(bShift ? Upper : Upper - 'A' + 'a');
		}
		struct FMap { FKey Key; TCHAR Plain, Shifted; };
		static const FMap Map[] = {
			{ EKeys::Zero, '0', ')' }, { EKeys::One, '1', '!' }, { EKeys::Two, '2', '@' }, { EKeys::Three, '3', '#' }, { EKeys::Four, '4', '$' },
			{ EKeys::Five, '5', '%' }, { EKeys::Six, '6', '^' }, { EKeys::Seven, '7', '&' }, { EKeys::Eight, '8', '*' }, { EKeys::Nine, '9', '(' },
			{ EKeys::SpaceBar, ' ', ' ' }, { EKeys::Hyphen, '-', '_' }, { EKeys::Equals, '=', '+' }, { EKeys::LeftBracket, '[', '{' },
			{ EKeys::RightBracket, ']', '}' }, { EKeys::Backslash, '\\', '|' }, { EKeys::Semicolon, ';', ':' }, { EKeys::Apostrophe, '\'', '"' },
			{ EKeys::Comma, ',', '<' }, { EKeys::Period, '.', '>' }, { EKeys::Slash, '/', '?' }, { EKeys::NumPadZero, '0', '0' },
			{ EKeys::NumPadOne, '1', '1' }, { EKeys::NumPadTwo, '2', '2' }, { EKeys::NumPadThree, '3', '3' }, { EKeys::NumPadFour, '4', '4' },
			{ EKeys::NumPadFive, '5', '5' }, { EKeys::NumPadSix, '6', '6' }, { EKeys::NumPadSeven, '7', '7' }, { EKeys::NumPadEight, '8', '8' },
			{ EKeys::NumPadNine, '9', '9' }, { EKeys::Decimal, '.', '.' }, { EKeys::Subtract, '-', '-' }, { EKeys::Add, '+', '+' },
			{ EKeys::Multiply, '*', '*' }, { EKeys::Divide, '/', '/' },
		};
		for (const FMap& M : Map)
		{
			if (M.Key == K)
			{
				return bShift ? M.Shifted : M.Plain;
			}
		}
		return 0;
	}

	class FModMenuInput : public IInputProcessor
	{
	public:
		static void Ensure()
		{
			static TSharedPtr<FModMenuInput> Instance;
			if (!Instance.IsValid() && FSlateApplication::IsInitialized())
			{
				Instance = MakeShared<FModMenuInput>();
				FSlateApplication::Get().RegisterInputPreProcessor(Instance, 0);
			}
		}

		static FUE2NetScriptHUD* MenuOwner()
		{
			FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
			return S && S->IsMenuOpen() && !ConsoleOpen() ? S : nullptr;
		}

		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

		virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& E) override
		{
			const FKey K = E.GetKey();
			if (K == EKeys::Tilde)
			{
				return false;
			}
			if (FUE2NetScriptHUD* S = MenuOwner())
			{
				if (const uint8 Code = UE2NetModMenus::KeyCode(K))
				{
					S->MenuKey(Code, IST_Press);
				}
				const TCHAR Ch = CharFor(E);
				UE_LOG(LogUE2Net, Verbose, TEXT("UE2NetModMenus: key %s (character %u) -> %d"), *K.ToString(), E.GetCharacter(), int32(Ch));
				if (Ch)
				{
					S->MenuType(Ch < 256 ? uint8(Ch) : 0, FString::Chr(Ch));
				}
				return true;
			}
			// a key a mod bound for itself, while playing (not typing, not in our menus)
			FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
			AArenaPlayerController* PC = GPC.Get();
			if (!S || E.IsRepeat() || ConsoleOpen() || ArenaChat::IsTyping() || (PC && PC->IsPauseMenuOpen()))
			{
				return false;
			}
			const FString Command = S->GetModBind(K.GetFName().ToString());
			if (Command.IsEmpty())
			{
				return false;
			}
			if (S->ConsoleExec(Command))
			{
				return true;
			}
			if (Command.StartsWith(TEXT("mutate "), ESearchCase::IgnoreCase))
			{
				if (FUE2NetPlay* P = FUE2NetPlay::Get())
				{
					P->Mutate(Command.Mid(7));
					return true;
				}
			}
			return false;
		}

		virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& E) override
		{
			const FKey K = E.GetKey();
			FUE2NetScriptHUD* S = K == EKeys::Tilde ? nullptr : MenuOwner();
			if (!S)
			{
				return false;
			}
			if (const uint8 Code = UE2NetModMenus::KeyCode(K))
			{
				S->MenuKey(Code, IST_Release);
			}
			return true;
		}

		virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& E) override
		{
			FUE2NetScriptHUD* S = MenuOwner();
			if (!S)
			{
				return false;
			}
			if (const uint8 Code = UE2NetModMenus::KeyCode(E.GetEffectingButton()))
			{
				S->MenuKey(Code, IST_Press);
			}
			return true;
		}

		virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& E) override
		{
			FUE2NetScriptHUD* S = MenuOwner();
			if (!S)
			{
				return false;
			}
			if (const uint8 Code = UE2NetModMenus::KeyCode(E.GetEffectingButton()))
			{
				S->MenuKey(Code, IST_Release);
			}
			return true;
		}

		virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& E) override
		{
			// Slate sends the second press of a double click as this: a press (the menus time double clicks themselves)
			return HandleMouseButtonDownEvent(SlateApp, E);
		}

		virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& E, const FPointerEvent* Gesture) override
		{
			FUE2NetScriptHUD* S = Gesture ? nullptr : MenuOwner();
			if (!S)
			{
				return false;
			}
			S->MenuKey(E.GetWheelDelta() > 0.f ? 0xEC : 0xED, IST_Press);
			return true;
		}

		virtual const TCHAR* GetDebugName() const override { return TEXT("UE2NetModMenus"); }
	};

	/** The cursor shown and our pawn's fire released while a mod menu is open (as our pause menu does). */
	void SetMenuMode(AArenaPlayerController* PC, bool bOpen)
	{
		if (!PC)
		{
			return;
		}
		if (bOpen)
		{
			if (PC->IsPauseMenuOpen())
			{
				PC->SetPauseMenuOpen(false);
			}
			if (AArenaCharacter* Char = Cast<AArenaCharacter>(PC->GetPawn()); Char && Char->IsAlive())
			{
				Char->StopFire(0);
				Char->StopFire(1);
			}
			// the menus draw UT2004's cursor themselves
			PC->bShowMouseCursor = true;
			PC->CurrentMouseCursor = EMouseCursor::None;
			FInputModeGameAndUI Mode;
			Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			Mode.SetHideCursorDuringCapture(false);
			PC->SetInputMode(Mode);
		}
		else if (!PC->IsPauseMenuOpen())
		{
			PC->bShowMouseCursor = false;
			PC->CurrentMouseCursor = PC->DefaultMouseCursor;
			PC->SetInputMode(FInputModeGameOnly());
		}
	}

	FAutoConsoleCommand CmdBind(TEXT("ModBind"), TEXT("ModBind <key> [command]: bind a key to a UT2004 server mod's command while playing there (e.g. ModBind F5 mymenu: UTComp's menu); no command removes the bind."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
			if (!S || Args.Num() == 0)
			{
				UE_LOG(LogUE2Net, Log, TEXT("ModBind: join a UT2004 server first"));
				return;
			}
			TArray<FString> Rest = Args;
			Rest.RemoveAt(0);
			S->SetModBind(Args[0], FString::Join(Rest, TEXT(" ")));
			UE_LOG(LogUE2Net, Log, TEXT("ModBind %s = \"%s\""), *Args[0], *FString::Join(Rest, TEXT(" ")));
		}));
#if !UE_BUILD_SHIPPING
	FAutoConsoleCommand CmdOpen(TEXT("ModMenuOpen"), TEXT("ModMenuOpen <Package.Class>: open a mod menu page (GUIController.OpenMenu)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
			const bool bOk = S && Args.Num() > 0 && S->OpenMenu(Args[0]);
			UE_LOG(LogUE2Net, Log, TEXT("ModMenuOpen %s: %s"), Args.Num() ? *Args[0] : TEXT(""), bOk ? TEXT("opened") : TEXT("failed"));
		}));
	FAutoConsoleCommand CmdDump(TEXT("ModMenuDump"), TEXT("ModMenuDump: log the top mod menu's components."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
			const FString Dump = S ? S->DumpMenus() : FString();
			TArray<FString> Lines;
			Dump.ParseIntoArrayLines(Lines);
			UE_LOG(LogUE2Net, Log, TEXT("ModMenuDump: %s (%d components)"), S ? *S->GetMenuName() : TEXT("no mirror"), Lines.Num());
			for (const FString& L : Lines)
			{
				UE_LOG(LogUE2Net, Log, TEXT("ModMenuDump:   %s"), *L);
			}
		}));
	FAutoConsoleCommand CmdClick(TEXT("ModMenuClick"), TEXT("ModMenuClick <component name or caption>: click a component of the top mod menu."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
			const FString Name = FString::Join(Args, TEXT(" "));
			const bool bOk = S && S->ClickMenu(Name);
			UE_LOG(LogUE2Net, Log, TEXT("ModMenuClick %s: %s"), *Name, bOk ? TEXT("clicked") : TEXT("not found"));
		}));
	FAutoConsoleCommand CmdKey(TEXT("ModMenuKey"), TEXT("ModMenuKey <EInputKey code>: press and release a key in the mod menus."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get(); S && Args.Num())
			{
				const uint8 Code = uint8(FCString::Strtoi(*Args[0], nullptr, 0));
				S->MenuKey(Code, IST_Press);
				S->MenuKey(Code, IST_Release);
			}
		}));
	FAutoConsoleCommand CmdType(TEXT("ModMenuType"), TEXT("ModMenuType <text>: type text into the focused control of the mod menus."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get())
			{
				for (const TCHAR C : FString::Join(Args, TEXT(" ")))
				{
					S->MenuType(C < 256 ? uint8(C) : 0, FString::Chr(C));
				}
			}
		}));
	FAutoConsoleCommand CmdDisasm(TEXT("ModDisasm"), TEXT("ModDisasm <Package.Class> <Function>: log a script function's bytecode (debugging mods)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FUSClass* C = Args.Num() >= 2 ? FUSRuntime::Get().FindClass(Args[0]) : nullptr;
			FUSFunction* F = C ? C->FindFunction(FName(*Args[1])) : nullptr;
			TArray<FString> Lines;
			if (F)
			{
				UE_LOG(LogUE2Net, Log, TEXT("ModDisasm: %s flags 0x%08x"), *F->GetPathName(), F->FunctionFlags);
			}
			(F ? USVM::Disassemble(F) : FString(TEXT("not found"))).ParseIntoArrayLines(Lines);
			for (const FString& L : Lines)
			{
				UE_LOG(LogUE2Net, Log, TEXT("ModDisasm: %s"), *L);
			}
			// the objects and names the bytecode refers to (crude: a reference token followed by a known index)
			if (F)
			{
				TSet<FString> Seen;
				const TArray<uint8>& B = F->Script;
				for (int32 i = 0; i + 4 < B.Num(); ++i)
				{
					const uint8 T = B[i];
					int32 V = 0;
					FMemory::Memcpy(&V, &B[i + 1], 4);
					FString What;
					if (T == 0x00 || T == 0x01 || T == 0x1C || T == 0x20 || T == 0x2E || T == 0x12)
					{
						if (FUSObject* O = V > 0 ? FUSRuntime::Get().ResolveRef(V) : nullptr)
						{
							What = FString::Printf(TEXT("%02X %X = %s"), T, V, *O->GetFullName());
						}
					}
					else if (T == 0x1B && V > 0)
					{
						const FName N = FUSRuntime::Get().GetName(V);
						What = N.IsNone() ? FString() : FString::Printf(TEXT("1B call %s"), *N.ToString());
					}
					if (!What.IsEmpty() && !Seen.Contains(What))
					{
						Seen.Add(What);
						UE_LOG(LogUE2Net, Log, TEXT("ModDisasm:   %04X %s"), i, *What);
					}
				}
			}
		}));
	FAutoConsoleCommand CmdRef(TEXT("ModRef"), TEXT("ModRef <hex index> [property]: the object of a script reference index (ModDisasm), or a property of our controller's mirror."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FUSObject* O = Args.Num() ? FUSRuntime::Get().ResolveRef(int32(FCString::Strtoi(*Args[0], nullptr, 16))) : nullptr;
			UE_LOG(LogUE2Net, Log, TEXT("ModRef %s: %s"), Args.Num() ? *Args[0] : TEXT(""), O ? *O->GetFullName() : TEXT("None"));
		}));
	FAutoConsoleCommand CmdPCProp(TEXT("ModPCProp"), TEXT("ModPCProp <property>: a property of our controller's mirror."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
			UE_LOG(LogUE2Net, Log, TEXT("ModPCProp %s = %s"), Args.Num() ? *Args[0] : TEXT(""), S && Args.Num() ? *S->DescribePCProperty(Args[0]) : TEXT("?"));
		}));
	FAutoConsoleCommand CmdClose(TEXT("ModMenuClose"), TEXT("ModMenuClose: close every mod menu."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get())
			{
				S->CloseMenus();
			}
		}));
#endif
}

uint8 UE2NetModMenus::KeyCode(const FKey& K)
{
	const FString N = K.GetFName().ToString();
	if (N.Len() == 1 && N[0] >= 'A' && N[0] <= 'Z')
	{
		return uint8(N[0]);
	}
	struct FMap { FKey Key; uint8 Code; };
	static const FMap Map[] = {
		{ EKeys::LeftMouseButton, 0x01 }, { EKeys::RightMouseButton, 0x02 }, { EKeys::MiddleMouseButton, 0x04 },
		{ EKeys::BackSpace, 0x08 }, { EKeys::Tab, 0x09 }, { EKeys::Enter, 0x0D }, { EKeys::LeftShift, 0x10 }, { EKeys::RightShift, 0x10 },
		{ EKeys::LeftControl, 0x11 }, { EKeys::RightControl, 0x11 }, { EKeys::LeftAlt, 0x12 }, { EKeys::RightAlt, 0x12 },
		{ EKeys::Pause, 0x13 }, { EKeys::CapsLock, 0x14 }, { EKeys::Escape, 0x1B }, { EKeys::SpaceBar, 0x20 }, { EKeys::PageUp, 0x21 },
		{ EKeys::PageDown, 0x22 }, { EKeys::End, 0x23 }, { EKeys::Home, 0x24 }, { EKeys::Left, 0x25 }, { EKeys::Up, 0x26 },
		{ EKeys::Right, 0x27 }, { EKeys::Down, 0x28 }, { EKeys::Insert, 0x2D }, { EKeys::Delete, 0x2E },
		{ EKeys::Zero, 0x30 }, { EKeys::One, 0x31 }, { EKeys::Two, 0x32 }, { EKeys::Three, 0x33 }, { EKeys::Four, 0x34 },
		{ EKeys::Five, 0x35 }, { EKeys::Six, 0x36 }, { EKeys::Seven, 0x37 }, { EKeys::Eight, 0x38 }, { EKeys::Nine, 0x39 },
		{ EKeys::NumPadZero, 0x60 }, { EKeys::NumPadOne, 0x61 }, { EKeys::NumPadTwo, 0x62 }, { EKeys::NumPadThree, 0x63 },
		{ EKeys::NumPadFour, 0x64 }, { EKeys::NumPadFive, 0x65 }, { EKeys::NumPadSix, 0x66 }, { EKeys::NumPadSeven, 0x67 },
		{ EKeys::NumPadEight, 0x68 }, { EKeys::NumPadNine, 0x69 }, { EKeys::Multiply, 0x6A }, { EKeys::Add, 0x6B }, { EKeys::Subtract, 0x6D },
		{ EKeys::Decimal, 0x6E }, { EKeys::Divide, 0x6F },
		{ EKeys::F1, 0x70 }, { EKeys::F2, 0x71 }, { EKeys::F3, 0x72 }, { EKeys::F4, 0x73 }, { EKeys::F5, 0x74 }, { EKeys::F6, 0x75 },
		{ EKeys::F7, 0x76 }, { EKeys::F8, 0x77 }, { EKeys::F9, 0x78 }, { EKeys::F10, 0x79 }, { EKeys::F11, 0x7A }, { EKeys::F12, 0x7B },
		{ EKeys::Semicolon, 0xBA }, { EKeys::Equals, 0xBB }, { EKeys::Comma, 0xBC }, { EKeys::Hyphen, 0xBD }, { EKeys::Period, 0xBE },
		{ EKeys::Slash, 0xBF }, { EKeys::Tilde, 0xC0 }, { EKeys::LeftBracket, 0xDB }, { EKeys::Backslash, 0xDC },
		{ EKeys::RightBracket, 0xDD }, { EKeys::Apostrophe, 0xDE }, { EKeys::MouseScrollUp, 0xEC }, { EKeys::MouseScrollDown, 0xED },
	};
	for (const FMap& M : Map)
	{
		if (M.Key == K)
		{
			return M.Code;
		}
	}
	return 0;
}

bool UE2NetModMenus::IsOpen()
{
	const FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
	return S && S->IsMenuOpen();
}

bool UE2NetModMenus::DrawAndUpdate(AHUD* Hud, UCanvas* Canvas)
{
	using namespace UE2NetModMenusLocal;
	FModMenuInput::Ensure();
	AArenaPlayerController* PC = Hud ? Cast<AArenaPlayerController>(Hud->PlayerOwner) : nullptr;
	GPC = PC;
	FUE2NetScriptHUD* S = FUE2NetScriptHUD::Get();
	const bool bOpen = S && S->IsMenuOpen();
	if (bOpen != bOpenLast)
	{
		bOpenLast = bOpen;
		SetMenuMode(PC, bOpen);
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetModMenus: %s"), bOpen ? *FString::Printf(TEXT("%s open"), *S->GetMenuName()) : TEXT("closed"));
	}
	if (!bOpen)
	{
		return false;
	}
	float X = 0.f, Y = 0.f;
	if (PC && PC->GetMousePosition(X, Y))
	{
		S->MenuMouse(X, Y);
	}
	return S->DrawMenus(Hud, Canvas);
}
