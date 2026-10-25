// UScript natives of UE2's Canvas (Engine.Canvas), the HUD widget natives (XInterface.HudBase), HUD lines and material
// sizes, drawn through the game's canvas backend (UScriptCanvas.h). The layout rules (cursor, wrapping, clipping, the
// widget placement) are UE2's; the backend only draws rectangles and lines of text in pixels.

#include "UScriptCanvas.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	IUSCanvasBackend* GBackend = nullptr;
	int32 GDrawCount = 0;

	/** ERenderStyle */
	constexpr uint8 STY_None = 0;

	/** Canvas properties (resolved once from Engine.Canvas). */
	struct FCanvasProps
	{
		bool bResolved = false;
		FUSPropRef Font, FontScaleX, FontScaleY, SpaceX, SpaceY, OrgX, OrgY, ClipX, ClipY, CurX, CurY, Z, Style, CurYL, DrawColor,
			bCenter, bNoSmooth, SizeX, SizeY, ColorModulate, bForceAlpha, ForcedAlpha;

		void Resolve(FUSClass* C)
		{
			if (bResolved || !C)
			{
				return;
			}
			bResolved = true;
			Font.Resolve(C, TEXT("Font"));
			FontScaleX.Resolve(C, TEXT("FontScaleX"));
			FontScaleY.Resolve(C, TEXT("FontScaleY"));
			SpaceX.Resolve(C, TEXT("SpaceX"));
			SpaceY.Resolve(C, TEXT("SpaceY"));
			OrgX.Resolve(C, TEXT("OrgX"));
			OrgY.Resolve(C, TEXT("OrgY"));
			ClipX.Resolve(C, TEXT("ClipX"));
			ClipY.Resolve(C, TEXT("ClipY"));
			CurX.Resolve(C, TEXT("CurX"));
			CurY.Resolve(C, TEXT("CurY"));
			Z.Resolve(C, TEXT("Z"));
			Style.Resolve(C, TEXT("Style"));
			CurYL.Resolve(C, TEXT("CurYL"));
			DrawColor.Resolve(C, TEXT("DrawColor"));
			bCenter.Resolve(C, TEXT("bCenter"));
			bNoSmooth.Resolve(C, TEXT("bNoSmooth"));
			SizeX.Resolve(C, TEXT("SizeX"));
			SizeY.Resolve(C, TEXT("SizeY"));
			ColorModulate.Resolve(C, TEXT("ColorModulate"));
			bForceAlpha.Resolve(C, TEXT("bForceAlpha"));
			ForcedAlpha.Resolve(C, TEXT("ForcedAlpha"));
		}
	};

	FCanvasProps& CP(FUSObject* Canvas)
	{
		static FCanvasProps P;
		if (!P.bResolved && Canvas && Canvas->Class)
		{
			// the Canvas class of Engine (a subclass passed as Self resolves the same offsets)
			FUSClass* C = Canvas->Class;
			for (int32 Guard = 0; C && C->Name != TEXT("Canvas") && Guard < 16; ++Guard)
			{
				C = C->GetSuperClass();
			}
			P.Resolve(C ? C : Canvas->Class);
		}
		return P;
	}

	bool IsCanvas(FUSObject* O)
	{
		return O && O->Data && O->IsA(FName(TEXT("Canvas")));
	}

	/** DrawColor with ColorModulate and the forced alpha, as UE2's canvas applies them. */
	FColor FinalColor(FUSObject* C)
	{
		FCanvasProps& P = CP(C);
		const FUSColor D = P.DrawColor.Get<FUSColor>(C);
		FUSPlane M = P.ColorModulate.Get<FUSPlane>(C);
		auto Mod = [](uint8 V, float S) { return uint8(FMath::Clamp(FMath::RoundToInt(float(V) * S), 0, 255)); };
		FColor Out(Mod(D.R, M.X), Mod(D.G, M.Y), Mod(D.B, M.Z), Mod(D.A, M.W));
		if (P.bForceAlpha.Get<uint32>(C) & 1)
		{
			Out.A = uint8(FMath::Clamp(FMath::RoundToInt(P.ForcedAlpha.Get<float>(C)), 0, 255));
		}
		return Out;
	}

	bool MaterialSize(FUSObject* Mat, float& U, float& V)
	{
		int32 IU = 0, IV = 0;
		if (Mat && GBackend && GBackend->MaterialSize(Mat, IU, IV))
		{
			U = float(IU);
			V = float(IV);
			return true;
		}
		U = V = 0.f;
		return false;
	}

	void Tile(FUSObject* C, FUSObject* Mat, float X, float Y, float XL, float YL, float U, float V, float UL, float VL)
	{
		FCanvasProps& P = CP(C);
		const uint8 Style = P.Style.Get<uint8>(C);
		if (!GBackend || !Mat || Style == STY_None)
		{
			return;
		}
		++GDrawCount;
		GBackend->DrawTile(Mat, X, Y, XL, YL, U, V, UL, VL, FinalColor(C), Style);
	}

	/** Removes the '&' hot key marks ("&&" stays one '&'). */
	FString StripHotKeys(const FString& S)
	{
		FString Out;
		Out.Reserve(S.Len());
		for (int32 i = 0; i < S.Len(); ++i)
		{
			if (S[i] == TEXT('&') && i + 1 < S.Len())
			{
				++i;
			}
			Out.AppendChar(S[i]);
		}
		return Out;
	}

	void Measure(FUSObject* C, const FString& Text, float& XL, float& YL)
	{
		FCanvasProps& P = CP(C);
		XL = YL = 0.f;
		if (GBackend)
		{
			GBackend->TextSize(P.Font.Get<FUSObject*>(C), Text, P.FontScaleX.Get<float>(C), P.FontScaleY.Get<float>(C), XL, YL);
		}
	}

	/** UE2 colour codes: ESC and three colour bytes. */
	int32 VisibleLen(const FString& S)
	{
		int32 N = 0;
		for (int32 i = 0; i < S.Len(); ++i)
		{
			if (S[i] == 27)
			{
				i += 3;
				continue;
			}
			++N;
		}
		return N;
	}

	/** Splits Text into lines no wider than Width (UE2 WrappedPrint: at spaces, a word wider than a line stays whole; '\n' breaks). */
	void WrapLines(FUSObject* C, const FString& Text, float Width, TArray<FString>& OutLines)
	{
		TArray<FString> Paragraphs;
		Text.ParseIntoArray(Paragraphs, TEXT("\n"), false);
		if (Paragraphs.Num() == 0)
		{
			Paragraphs.Add(FString());
		}
		for (const FString& Para : Paragraphs)
		{
			FString Line;
			int32 i = 0;
			while (i <= Para.Len())
			{
				// next word (with its leading spaces)
				int32 j = i;
				while (j < Para.Len() && Para[j] == TEXT(' '))
				{
					++j;
				}
				while (j < Para.Len() && Para[j] != TEXT(' '))
				{
					++j;
				}
				const FString Word = Para.Mid(i, j - i);
				if (Word.IsEmpty())
				{
					break;
				}
				const FString Test = Line + Word;
				float XL = 0.f, YL = 0.f;
				Measure(C, Test, XL, YL);
				// half a pixel of slack: text placed at ClipX - its own StrLen width must not wrap from float rounding
				if (XL > Width + 0.5f && !Line.IsEmpty())
				{
					OutLines.Add(Line);
					Line = Word.TrimStart();
				}
				else
				{
					Line = Test;
				}
				i = j;
			}
			OutLines.Add(Line);
		}
	}

	/** UE2 UCanvas::WrappedPrint: draws (when bDraw) and measures the wrapped text at the cursor. */
	void WrappedPrint(FUSObject* C, const FString& Text, bool bDraw, float& OutXL, float& OutYL)
	{
		FCanvasProps& P = CP(C);
		const float CurX = P.CurX.Get<float>(C), CurY = P.CurY.Get<float>(C);
		const float OrgX = P.OrgX.Get<float>(C), OrgY = P.OrgY.Get<float>(C);
		const float ClipX = P.ClipX.Get<float>(C);
		const bool bCenter = (P.bCenter.Get<uint32>(C) & 1) != 0;
		TArray<FString> Lines;
		WrapLines(C, Text, FMath::Max(1.f, ClipX - CurX), Lines);
		OutXL = OutYL = 0.f;
		const FColor Color = FinalColor(C);
		const uint8 Style = P.Style.Get<uint8>(C);
		for (const FString& Line : Lines)
		{
			float XL = 0.f, YL = 0.f;
			Measure(C, Line.IsEmpty() ? FString(TEXT(" ")) : Line, XL, YL);
			if (Line.IsEmpty())
			{
				XL = 0.f;
			}
			if (bDraw && GBackend && !Line.IsEmpty() && Style != STY_None)
			{
				const float X = bCenter ? OrgX + CurX + (ClipX - CurX - XL) * 0.5f : OrgX + CurX;
				++GDrawCount;
				GBackend->DrawText(P.Font.Get<FUSObject*>(C), Line, X, OrgY + CurY + OutYL, P.FontScaleX.Get<float>(C), P.FontScaleY.Get<float>(C), Color, Style);
			}
			OutXL = FMath::Max(OutXL, XL);
			OutYL += YL;
		}
	}

	/** The longest prefix of Text that fits in Width (UE2 ClippedPrint draws the characters that fit). */
	FString ClipToWidth(FUSObject* C, const FString& Text, float Width)
	{
		float XL = 0.f, YL = 0.f;
		Measure(C, Text, XL, YL);
		if (XL <= Width)
		{
			return Text;
		}
		int32 Lo = 0, Hi = Text.Len();
		while (Lo < Hi)
		{
			const int32 Mid = (Lo + Hi + 1) / 2;
			Measure(C, Text.Left(Mid), XL, YL);
			if (XL <= Width)
			{
				Lo = Mid;
			}
			else
			{
				Hi = Mid - 1;
			}
		}
		return Text.Left(Lo);
	}

	void ArraySetStrings(FUSScriptArray* A, FUSProperty* Prop, const TArray<FString>& Strings)
	{
		if (!A || !Prop || Prop->Type != EUSType::Array || !Prop->Inner || Prop->Inner->Type != EUSType::Str)
		{
			return;
		}
		const int32 ES = Prop->Inner->ElementSize;
		for (int32 i = 0; i < A->Num; ++i)
		{
			Prop->Inner->DestroySingle(A->Data + int64(i) * ES);
		}
		A->Num = 0;
		if (Strings.Num() > A->Max)
		{
			A->Data = static_cast<uint8*>(FMemory::Realloc(A->Data, int64(Strings.Num()) * ES, 16));
			A->Max = Strings.Num();
		}
		FMemory::Memzero(A->Data, int64(Strings.Num()) * ES);
		A->Num = Strings.Num();
		for (int32 i = 0; i < Strings.Num(); ++i)
		{
			*reinterpret_cast<FString*>(A->Data + int64(i) * ES) = Strings[i];
		}
	}

	/** A struct parameter by reference: the variable's memory (or a temporary copy). */
	struct FStructRef
	{
		alignas(16) uint8 Temp[1024];
		uint8* Addr = nullptr;
	};

	/** Member of a script struct by name. */
	template <typename T>
	T Member(FUSStruct* S, const uint8* Base, const TCHAR* Name, int32 Index = 0, T Default = T())
	{
		FUSProperty* P = S ? S->FindPropertyByName(FName(Name)) : nullptr;
		if (!P || !Base || Index < 0 || Index >= P->ArrayDim)
		{
			return Default;
		}
		T V;
		FMemory::Memcpy(&V, Base + P->Offset + Index * P->ElementSize, sizeof(T));
		return V;
	}

	const uint8* MemberAddr(FUSStruct* S, const uint8* Base, const TCHAR* Name, int32 Index = 0)
	{
		FUSProperty* P = S ? S->FindPropertyByName(FName(Name)) : nullptr;
		return (P && Base && Index >= 0 && Index < P->ArrayDim) ? Base + P->Offset + Index * P->ElementSize : nullptr;
	}

	/** The struct type of a parameter of a native (from its declaration). */
	FUSStruct* ParmStruct(const TCHAR* ClassPath, const TCHAR* Function, int32 Parm)
	{
		FUSClass* C = FUSRuntime::Get().FindClass(ClassPath);
		FUSFunction* F = C ? C->FindFunction(FName(Function)) : nullptr;
		return (F && F->Parms.IsValidIndex(Parm)) ? F->Parms[Parm]->Struct : nullptr;
	}

	/** EDrawPivot as fractions of the size to move left / up. */
	void PivotShift(uint8 Pivot, float& SX, float& SY)
	{
		static const float XS[9] = { 0.f, 0.5f, 1.f, 1.f, 1.f, 0.5f, 0.f, 0.f, 0.5f };
		static const float YS[9] = { 0.f, 0.f, 0.f, 0.5f, 1.f, 1.f, 1.f, 0.5f, 0.5f };
		SX = Pivot < 9 ? XS[Pivot] : 0.f;
		SY = Pivot < 9 ? YS[Pivot] : 0.f;
	}

	/** HUD values the widget natives use (HUD.HudScale, HudCanvasScale, ResScaleX / Y, HudBase.TeamIndex). */
	struct FWidgetFrame
	{
		float HudScale = 1.f, HudCanvasScale = 0.95f, ResScaleX = 1.f, ResScaleY = 1.f;
		int32 TeamIndex = 1;
		float ClipX = 640.f, ClipY = 480.f;
	};

	FWidgetFrame WidgetFrame(FUSObject* Hud, FUSObject* C)
	{
		static FUSPropRef HudScale, HudCanvasScale, ResScaleX, ResScaleY, TeamIndex;
		static bool bResolved = false;
		if (!bResolved)
		{
			bResolved = true;
			FUSClass* HudClass = FUSRuntime::Get().FindClass(TEXT("Engine.HUD"));
			FUSClass* HudBase = FUSRuntime::Get().FindClass(TEXT("XInterface.HudBase"));
			HudScale.Resolve(HudClass, TEXT("HudScale"));
			HudCanvasScale.Resolve(HudClass, TEXT("HudCanvasScale"));
			ResScaleX.Resolve(HudClass, TEXT("ResScaleX"));
			ResScaleY.Resolve(HudClass, TEXT("ResScaleY"));
			TeamIndex.Resolve(HudBase, TEXT("TeamIndex"));
		}
		FWidgetFrame F;
		const bool bHud = Hud && Hud->Data && Hud->IsA(FName(TEXT("HUD")));
		if (bHud)
		{
			F.HudScale = HudScale.Get<float>(Hud, 1.f);
			F.HudCanvasScale = HudCanvasScale.Get<float>(Hud, 0.95f);
			F.ResScaleX = ResScaleX.Get<float>(Hud, 1.f);
			F.ResScaleY = ResScaleY.Get<float>(Hud, 1.f);
		}
		if (Hud && Hud->Data && Hud->IsA(FName(TEXT("HudBase"))))
		{
			F.TeamIndex = FMath::Clamp(TeamIndex.Get<int32>(Hud, 1), 0, 1);
		}
		FCanvasProps& P = CP(C);
		F.ClipX = P.ClipX.Get<float>(C);
		F.ClipY = P.ClipY.Get<float>(C);
		if (!bHud && F.ClipY > 0.f)
		{
			F.ResScaleX = F.ClipX / 640.f;
			F.ResScaleY = F.ClipY / 480.f;
		}
		// widgets keep their aspect ratio like OldUnreal's 3374 HUD (HUD.PostRender: ResScaleX = ResScaleY =
		// Min(SizeX / 640, SizeY / 480)); retail 3369 scripts stretch them across a wide screen
		F.ResScaleX = F.ResScaleY = FMath::Min(F.ResScaleX, F.ResScaleY);
		return F;
	}

	/** Draws with a temporary canvas colour / style (the widget natives leave the canvas as it was). */
	void TileWith(FUSObject* C, FUSObject* Mat, uint8 Style, const FUSColor& Color, float X, float Y, float XL, float YL, float U, float V, float UL, float VL)
	{
		FCanvasProps& P = CP(C);
		const FUSColor OldColor = P.DrawColor.Get<FUSColor>(C);
		const uint8 OldStyle = P.Style.Get<uint8>(C);
		P.DrawColor.Set<FUSColor>(C, Color);
		P.Style.Set<uint8>(C, Style);
		Tile(C, Mat, X, Y, XL, YL, U, V, UL, VL);
		P.DrawColor.Set<FUSColor>(C, OldColor);
		P.Style.Set<uint8>(C, OldStyle);
	}
}

void USCanvas::SetBackend(IUSCanvasBackend* Backend)
{
	GBackend = Backend;
}

IUSCanvasBackend* USCanvas::GetBackend()
{
	return GBackend;
}

int32 USCanvas::GetDrawCount()
{
	return GDrawCount;
}

void USCanvas::ResetDrawCount()
{
	GDrawCount = 0;
}

namespace USCanvasNatives
{
	// ---- text ----
	US_NATIVE(StrLen)
	{
		USP_GET_STR(Text);
		USP_GET_FLOAT_REF(XL);
		USP_GET_FLOAT_REF(YL);
		USP_FINISH;
		*XL = *YL = 0.f;
		if (IsCanvas(Self))
		{
			// UE2 measures from CurX 0 (the whole ClipX wide) and puts the cursor back
			FCanvasProps& P = CP(Self);
			const float OldX = P.CurX.Get<float>(Self), OldY = P.CurY.Get<float>(Self);
			P.CurX.Set<float>(Self, 0.f);
			P.CurY.Set<float>(Self, 0.f);
			WrappedPrint(Self, Text, false, *XL, *YL);
			P.CurX.Set<float>(Self, OldX);
			P.CurY.Set<float>(Self, OldY);
		}
	}

	US_NATIVE(DrawText)
	{
		USP_GET_STR(Text);
		USP_GET_UBOOL_OPTX(bCR, false);
		USP_FINISH;
		if (!IsCanvas(Self))
		{
			return;
		}
		FCanvasProps& P = CP(Self);
		float XL = 0.f, YL = 0.f;
		WrappedPrint(Self, Text, true, XL, YL);
		P.CurX.Set<float>(Self, P.CurX.Get<float>(Self) + XL);
		P.CurYL.Set<float>(Self, FMath::Max(P.CurYL.Get<float>(Self), YL));
		if (bCR & 1)
		{
			P.CurX.Set<float>(Self, 0.f);
			P.CurY.Set<float>(Self, P.CurY.Get<float>(Self) + P.CurYL.Get<float>(Self));
			P.CurYL.Set<float>(Self, 0.f);
		}
	}

	US_NATIVE(DrawTextClipped)
	{
		USP_GET_STR(Text);
		USP_GET_UBOOL_OPTX(bCheckHotKey, false);
		USP_FINISH;
		if (!IsCanvas(Self) || !GBackend)
		{
			return;
		}
		FCanvasProps& P = CP(Self);
		const FString Shown = ClipToWidth(Self, (bCheckHotKey & 1) ? StripHotKeys(Text) : Text, P.ClipX.Get<float>(Self) - P.CurX.Get<float>(Self));
		if (!Shown.IsEmpty() && P.Style.Get<uint8>(Self) != STY_None)
		{
			++GDrawCount;
			GBackend->DrawText(P.Font.Get<FUSObject*>(Self), Shown, P.OrgX.Get<float>(Self) + P.CurX.Get<float>(Self), P.OrgY.Get<float>(Self) + P.CurY.Get<float>(Self),
				P.FontScaleX.Get<float>(Self), P.FontScaleY.Get<float>(Self), FinalColor(Self), P.Style.Get<uint8>(Self));
		}
	}

	US_NATIVE(TextSize)
	{
		USP_GET_STR(Text);
		USP_GET_FLOAT_REF(XL);
		USP_GET_FLOAT_REF(YL);
		USP_FINISH;
		*XL = *YL = 0.f;
		if (IsCanvas(Self))
		{
			Measure(Self, Text, *XL, *YL);
		}
	}

	US_NATIVE(DrawTextJustified)
	{
		USP_GET_STR(Text);
		USP_GET_BYTE(Justification);
		USP_GET_FLOAT(X1);
		USP_GET_FLOAT(Y1);
		USP_GET_FLOAT(X2);
		USP_GET_FLOAT(Y2);
		USP_FINISH;
		if (!IsCanvas(Self) || !GBackend)
		{
			return;
		}
		FCanvasProps& P = CP(Self);
		const FString Shown = ClipToWidth(Self, Text, X2 - X1);
		float XL = 0.f, YL = 0.f;
		Measure(Self, Shown, XL, YL);
		float X = X1;
		if (Justification == 1)
		{
			X = X1 + (X2 - X1 - XL) * 0.5f;
		}
		else if (Justification == 2)
		{
			X = X2 - XL;
		}
		const float Y = Y1 + (Y2 - Y1 - YL) * 0.5f;
		if (!Shown.IsEmpty() && P.Style.Get<uint8>(Self) != STY_None)
		{
			++GDrawCount;
			GBackend->DrawText(P.Font.Get<FUSObject*>(Self), Shown, P.OrgX.Get<float>(Self) + X, P.OrgY.Get<float>(Self) + Y, P.FontScaleX.Get<float>(Self),
				P.FontScaleY.Get<float>(Self), FinalColor(Self), P.Style.Get<uint8>(Self));
		}
	}

	US_NATIVE(WrapStringToArray)
	{
		USP_GET_STR(Text);
		USP_GET_ARRAY_REF(Out, OutProp);
		USP_GET_FLOAT(DX);
		USP_GET_STR_OPTX(EOL, FString());
		USP_FINISH;
		TArray<FString> Lines;
		if (IsCanvas(Self))
		{
			FString Src = Text;
			if (!EOL.IsEmpty())
			{
				Src.ReplaceInline(*EOL, TEXT("\n"), ESearchCase::CaseSensitive);
			}
			WrapLines(Self, Src, FMath::Max(1.f, DX), Lines);
		}
		ArraySetStrings(Out, OutProp, Lines);
	}

	US_NATIVE(WrapText)
	{
		USP_GET_STR_REF(Text);
		USP_GET_STR_REF(Line);
		USP_GET_FLOAT(DX);
		USP_GET_OBJECT(Font);
		USP_GET_FLOAT_OPTX(FontScaleX, 1.f);
		USP_FINISH;
		// static: the first line of Text that fits DX goes to Line and is removed from Text
		*Line = FString();
		if (Text->IsEmpty())
		{
			return;
		}
		int32 Cut = Text->Len();
		if (GBackend)
		{
			int32 LastSpace = INDEX_NONE;
			for (int32 i = 1; i <= Text->Len(); ++i)
			{
				float XL = 0.f, YL = 0.f;
				GBackend->TextSize(Font, Text->Left(i), FontScaleX, 1.f, XL, YL);
				if (XL > DX)
				{
					Cut = LastSpace > 0 ? LastSpace : FMath::Max(1, i - 1);
					break;
				}
				if ((*Text)[i - 1] == TEXT(' '))
				{
					LastSpace = i;
				}
			}
		}
		*Line = Text->Left(Cut);
		*Text = Text->Mid(Cut);
	}

	// ---- tiles ----
	US_NATIVE(DrawTile)
	{
		USP_GET_OBJECT(Mat);
		USP_GET_FLOAT(XL);
		USP_GET_FLOAT(YL);
		USP_GET_FLOAT(U);
		USP_GET_FLOAT(V);
		USP_GET_FLOAT(UL);
		USP_GET_FLOAT(VL);
		USP_FINISH;
		if (!IsCanvas(Self))
		{
			return;
		}
		FCanvasProps& P = CP(Self);
		Tile(Self, Mat, P.OrgX.Get<float>(Self) + P.CurX.Get<float>(Self), P.OrgY.Get<float>(Self) + P.CurY.Get<float>(Self), XL, YL, U, V, UL, VL);
		P.CurX.Set<float>(Self, P.CurX.Get<float>(Self) + XL + P.SpaceX.Get<float>(Self));
		P.CurYL.Set<float>(Self, FMath::Max(P.CurYL.Get<float>(Self), YL));
	}

	US_NATIVE(DrawTileClipped)
	{
		USP_GET_OBJECT(Mat);
		USP_GET_FLOAT(XL);
		USP_GET_FLOAT(YL);
		USP_GET_FLOAT(U);
		USP_GET_FLOAT(V);
		USP_GET_FLOAT(UL);
		USP_GET_FLOAT(VL);
		USP_FINISH;
		if (!IsCanvas(Self))
		{
			return;
		}
		FCanvasProps& P = CP(Self);
		float CurX = P.CurX.Get<float>(Self), CurY = P.CurY.Get<float>(Self);
		const float ClipX = P.ClipX.Get<float>(Self), ClipY = P.ClipY.Get<float>(Self);
		// UE2 UCanvas::execDrawTileClipped
		if (CurX < 0.f && XL != 0.f)
		{
			const float Cut = CurX * UL / XL;
			U -= Cut;
			UL += Cut;
			XL += CurX;
			CurX = 0.f;
		}
		if (CurY < 0.f && YL != 0.f)
		{
			const float Cut = CurY * VL / YL;
			V -= Cut;
			VL += Cut;
			YL += CurY;
			CurY = 0.f;
		}
		if (XL > ClipX - CurX && XL != 0.f)
		{
			UL += (ClipX - CurX - XL) * UL / XL;
			XL = ClipX - CurX;
		}
		if (YL > ClipY - CurY && YL != 0.f)
		{
			VL += (ClipY - CurY - YL) * VL / YL;
			YL = ClipY - CurY;
		}
		if (XL > 0.f && YL > 0.f)
		{
			Tile(Self, Mat, P.OrgX.Get<float>(Self) + CurX, P.OrgY.Get<float>(Self) + CurY, XL, YL, U, V, UL, VL);
		}
		P.CurX.Set<float>(Self, CurX + XL + P.SpaceX.Get<float>(Self));
		P.CurY.Set<float>(Self, CurY);
		P.CurYL.Set<float>(Self, FMath::Max(P.CurYL.Get<float>(Self), YL));
	}

	/** UE2 UCanvas::DrawTileStretched: the corners 1:1 in texels, the middle rows and columns stretched. */
	void Stretched(FUSObject* C, FUSObject* Mat, float XL, float YL)
	{
		FCanvasProps& P = CP(C);
		float MW = 0.f, MH = 0.f;
		if (!MaterialSize(Mat, MW, MH) || MW <= 0.f || MH <= 0.f)
		{
			return;
		}
		const float X = P.OrgX.Get<float>(C) + P.CurX.Get<float>(C);
		const float Y = P.OrgY.Get<float>(C) + P.CurY.Get<float>(C);
		const float AW = FMath::Abs(XL), AH = FMath::Abs(YL);
		const float MidX = FMath::FloorToFloat(MW * 0.5f), MidY = FMath::FloorToFloat(MH * 0.5f);
		const float CW = FMath::Min(MidX, AW * 0.5f), CH = FMath::Min(MidY, AH * 0.5f);
		const float SW = AW - CW * 2.f, SH = AH - CH * 2.f;
		// corners
		Tile(C, Mat, X, Y, CW, CH, 0.f, 0.f, CW, CH);
		Tile(C, Mat, X + AW - CW, Y, CW, CH, MW - CW, 0.f, CW, CH);
		Tile(C, Mat, X, Y + AH - CH, CW, CH, 0.f, MH - CH, CW, CH);
		Tile(C, Mat, X + AW - CW, Y + AH - CH, CW, CH, MW - CW, MH - CH, CW, CH);
		if (SW > 0.f)
		{
			Tile(C, Mat, X + CW, Y, SW, CH, MidX, 0.f, 1.f, CH);
			Tile(C, Mat, X + CW, Y + AH - CH, SW, CH, MidX, MH - CH, 1.f, CH);
		}
		if (SH > 0.f)
		{
			Tile(C, Mat, X, Y + CH, CW, SH, 0.f, MidY, CW, 1.f);
			Tile(C, Mat, X + AW - CW, Y + CH, CW, SH, MW - CW, MidY, CW, 1.f);
		}
		if (SW > 0.f && SH > 0.f)
		{
			Tile(C, Mat, X + CW, Y + CH, SW, SH, MidX, MidY, 1.f, 1.f);
		}
	}

	US_NATIVE(DrawTileStretched)
	{
		USP_GET_OBJECT(Mat);
		USP_GET_FLOAT(XL);
		USP_GET_FLOAT(YL);
		USP_FINISH;
		if (IsCanvas(Self))
		{
			Stretched(Self, Mat, XL, YL);
		}
	}

	US_NATIVE(DrawTileScaled)
	{
		USP_GET_OBJECT(Mat);
		USP_GET_FLOAT(XScale);
		USP_GET_FLOAT(YScale);
		USP_FINISH;
		float MW = 0.f, MH = 0.f;
		if (!IsCanvas(Self) || !MaterialSize(Mat, MW, MH))
		{
			return;
		}
		FCanvasProps& P = CP(Self);
		const float XL = MW * XScale, YL = MH * YScale;
		Tile(Self, Mat, P.OrgX.Get<float>(Self) + P.CurX.Get<float>(Self), P.OrgY.Get<float>(Self) + P.CurY.Get<float>(Self), XL, YL, 0.f, 0.f, MW, MH);
		P.CurX.Set<float>(Self, P.CurX.Get<float>(Self) + XL + P.SpaceX.Get<float>(Self));
		P.CurYL.Set<float>(Self, FMath::Max(P.CurYL.Get<float>(Self), YL));
	}

	US_NATIVE(DrawTileJustified)
	{
		USP_GET_OBJECT(Mat);
		USP_GET_BYTE(Justification);
		USP_GET_FLOAT(XL);
		USP_GET_FLOAT(YL);
		USP_FINISH;
		float MW = 0.f, MH = 0.f;
		if (!IsCanvas(Self) || !MaterialSize(Mat, MW, MH) || MW <= 0.f || MH <= 0.f || XL <= 0.f || YL <= 0.f)
		{
			return;
		}
		FCanvasProps& P = CP(Self);
		float X = P.OrgX.Get<float>(Self) + P.CurX.Get<float>(Self);
		float Y = P.OrgY.Get<float>(Self) + P.CurY.Get<float>(Self);
		float W = XL, H = YL;
		// keep the material's aspect ratio inside the box, placed by the justification (0 left / top, 1 centre, 2 right / bottom)
		const float J = Justification == 1 ? 0.5f : (Justification == 2 ? 1.f : 0.f);
		if (MW / MH > XL / YL)
		{
			H = XL * MH / MW;
			Y += (YL - H) * J;
		}
		else
		{
			W = YL * MW / MH;
			X += (XL - W) * J;
		}
		Tile(Self, Mat, X, Y, W, H, 0.f, 0.f, MW, MH);
	}

	US_NATIVE(DrawTilePartialStretched)
	{
		USP_GET_OBJECT(Mat);
		USP_GET_FLOAT(XL);
		USP_GET_FLOAT(YL);
		USP_FINISH;
		if (IsCanvas(Self))
		{
			Stretched(Self, Mat, XL, YL);
		}
	}

	// ---- 3D ----
	US_NATIVE(WorldToScreen)
	{
		USP_GET_VECTOR(World);
		USP_FINISH;
		US_RESULT(FUSVector) = GBackend ? GBackend->WorldToScreen(World) : FUSVector();
	}

	US_NATIVE(GetCameraLocation)
	{
		USP_GET_VECTOR_REF(Loc);
		USP_GET_ROTATOR_REF(Rot);
		USP_FINISH;
		if (GBackend)
		{
			GBackend->GetCamera(*Loc, *Rot);
		}
	}

	/** DrawActor, DrawPortal, DrawScreenActor, SetScreenLight ...: our renderer draws the world and the first person weapon. */
	US_NATIVE(Ignore)
	{
		USSkipNativeParms(Stack);
	}

	/** Interaction.Initialize: UE2 sets up the interaction's state and calls its Initialized event. */
	US_NATIVE(InteractionInitialize)
	{
		USP_FINISH;
		if (FUSFunction* F = Self ? USVM::FindFunction(Self, TEXT("Initialized")) : nullptr)
		{
			FUSParms Parms(F);
			Parms.Call(Self);
		}
	}

	// ---- HUD lines (Engine.HUD) ----
	US_NATIVE(DrawCanvasLine)
	{
		USP_GET_FLOAT(X1);
		USP_GET_FLOAT(Y1);
		USP_GET_FLOAT(X2);
		USP_GET_FLOAT(Y2);
		USP_GET_STRUCT(FUSColor, Color);
		USP_FINISH;
		if (GBackend)
		{
			++GDrawCount;
			GBackend->DrawLine(X1, Y1, X2, Y2, FColor(Color.R, Color.G, Color.B, Color.A));
		}
	}

	US_NATIVE(StaticDrawCanvasLine)
	{
		USP_GET_OBJECT(C);
		USP_GET_FLOAT(X1);
		USP_GET_FLOAT(Y1);
		USP_GET_FLOAT(X2);
		USP_GET_FLOAT(Y2);
		USP_GET_STRUCT(FUSColor, Color);
		USP_FINISH;
		if (GBackend)
		{
			++GDrawCount;
			GBackend->DrawLine(X1, Y1, X2, Y2, FColor(Color.R, Color.G, Color.B, Color.A));
		}
	}

	US_NATIVE(Draw3DLine)
	{
		USP_GET_VECTOR(Start);
		USP_GET_VECTOR(End);
		USP_GET_STRUCT(FUSColor, Color);
		USP_FINISH;
		if (GBackend)
		{
			const FUSVector A = GBackend->WorldToScreen(Start), B = GBackend->WorldToScreen(End);
			if (A.Z > 0.f && B.Z > 0.f)
			{
				GBackend->DrawLine(A.X, A.Y, B.X, B.Y, FColor(Color.R, Color.G, Color.B, Color.A));
			}
		}
	}

	// ---- materials ----
	US_NATIVE(MaterialUSize)
	{
		USP_FINISH;
		float U = 0.f, V = 0.f;
		MaterialSize(Self, U, V);
		US_RESULT(int32) = int32(U);
	}

	US_NATIVE(MaterialVSize)
	{
		USP_FINISH;
		float U = 0.f, V = 0.f;
		MaterialSize(Self, U, V);
		US_RESULT(int32) = int32(V);
	}

	// ---- HUD widgets (XInterface.HudBase) ----
	/** HudBase.DrawSpriteWidget: TextureCoords scaled by TextureScale, HudCanvasScale, ResScaleX / Y and HudScale, placed at
	 * Pos in the HudCanvasScale part of the canvas plus Offset, moved by the DrawPivot, cropped by ScaleMode / Scale. */
	US_NATIVE(DrawSpriteWidget)
	{
		USP_GET_OBJECT(C);
		FStructRef W;
		USVM::GPropAddr = nullptr;
		FMemory::Memzero(W.Temp, sizeof(W.Temp));
		US_STEP_PARM(W.Temp);
		W.Addr = USVM::GPropAddr ? USVM::GPropAddr : W.Temp;
		USP_FINISH;
		static FUSStruct* S = ParmStruct(TEXT("XInterface.HudBase"), TEXT("DrawSpriteWidget"), 1);
		FUSStruct* Box = nullptr;
		if (S)
		{
			FUSProperty* TC = S->FindPropertyByName(TEXT("TextureCoords"));
			Box = TC ? TC->Struct : nullptr;
		}
		if (!S || !Box || !IsCanvas(C))
		{
			return;
		}
		const FWidgetFrame F = WidgetFrame(Self, C);
		const uint8* B = MemberAddr(S, W.Addr, TEXT("TextureCoords"));
		const int32 X1 = Member<int32>(Box, B, TEXT("X1")), Y1 = Member<int32>(Box, B, TEXT("Y1"));
		const int32 X2 = Member<int32>(Box, B, TEXT("X2")), Y2 = Member<int32>(Box, B, TEXT("Y2"));
		const float TS = Member<float>(S, W.Addr, TEXT("TextureScale"));
		const float SX = TS * F.HudCanvasScale * F.ResScaleX * F.HudScale;
		const float SY = TS * F.HudCanvasScale * F.ResScaleY * F.HudScale;
		float XL = float(FMath::Abs(X2 - X1) + 1) * SX;
		float YL = float(FMath::Abs(Y2 - Y1) + 1) * SY;
		float X = Member<float>(S, W.Addr, TEXT("PosX")) * F.HudCanvasScale * F.ClipX + (1.f - F.HudCanvasScale) * 0.5f * F.ClipX + float(Member<int32>(S, W.Addr, TEXT("OffsetX"))) * SX;
		float Y = Member<float>(S, W.Addr, TEXT("PosY")) * F.HudCanvasScale * F.ClipY + (1.f - F.HudCanvasScale) * 0.5f * F.ClipY + float(Member<int32>(S, W.Addr, TEXT("OffsetY"))) * SY;
		float PX = 0.f, PY = 0.f;
		PivotShift(Member<uint8>(S, W.Addr, TEXT("DrawPivot")), PX, PY);
		X -= XL * PX;
		Y -= YL * PY;
		float U = float(X1), V = float(Y1), UL = float(X2 - X1), VL = float(Y2 - Y1);
		const float Sc = FMath::Clamp(Member<float>(S, W.Addr, TEXT("Scale")), 0.f, 1.f);
		switch (Member<uint8>(S, W.Addr, TEXT("ScaleMode")))
		{
		case 4: // SM_Right: keeps the left part
			XL *= Sc;
			UL *= Sc;
			break;
		case 3: // SM_Left: keeps the right part
			X += XL * (1.f - Sc);
			U += UL * (1.f - Sc);
			XL *= Sc;
			UL *= Sc;
			break;
		case 1: // SM_Up: keeps the bottom part
			Y += YL * (1.f - Sc);
			V += VL * (1.f - Sc);
			YL *= Sc;
			VL *= Sc;
			break;
		case 2: // SM_Down: keeps the top part
			YL *= Sc;
			VL *= Sc;
			break;
		default:
			break;
		}
		if (XL <= 0.f || YL <= 0.f)
		{
			return;
		}
		const FUSColor Tint = Member<FUSColor>(S, W.Addr, TEXT("Tints"), F.TeamIndex);
		TileWith(C, Member<FUSObject*>(S, W.Addr, TEXT("WidgetTexture")), Member<uint8>(S, W.Addr, TEXT("RenderStyle")), Tint, X, Y, XL, YL, U, V, UL, VL);
	}

	/** HudBase.DrawNumericWidget: Value in the DigitSet's cells (MinDigitCount padding: zeroes or blank cells), placed like a sprite. */
	US_NATIVE(DrawNumericWidget)
	{
		USP_GET_OBJECT(C);
		FStructRef W, D;
		USVM::GPropAddr = nullptr;
		FMemory::Memzero(W.Temp, sizeof(W.Temp));
		US_STEP_PARM(W.Temp);
		W.Addr = USVM::GPropAddr ? USVM::GPropAddr : W.Temp;
		USVM::GPropAddr = nullptr;
		FMemory::Memzero(D.Temp, sizeof(D.Temp));
		US_STEP_PARM(D.Temp);
		D.Addr = USVM::GPropAddr ? USVM::GPropAddr : D.Temp;
		USP_FINISH;
		static FUSStruct* SN = ParmStruct(TEXT("XInterface.HudBase"), TEXT("DrawNumericWidget"), 1);
		static FUSStruct* SD = ParmStruct(TEXT("XInterface.HudBase"), TEXT("DrawNumericWidget"), 2);
		FUSStruct* Box = nullptr;
		if (SD)
		{
			FUSProperty* TC = SD->FindPropertyByName(TEXT("TextureCoords"));
			Box = TC ? TC->Struct : nullptr;
		}
		if (!SN || !SD || !Box || !IsCanvas(C))
		{
			return;
		}
		const FWidgetFrame F = WidgetFrame(Self, C);
		const int32 Value = Member<int32>(SN, W.Addr, TEXT("Value"));
		const FString Str = FString::FromInt(Value);
		const int32 MinDigits = Member<int32>(SN, W.Addr, TEXT("MinDigitCount"));
		const bool bZeroes = Member<int32>(SN, W.Addr, TEXT("bPadWithZeroes")) != 0;
		TArray<int32, TInlineAllocator<16>> Cells; // digit index, -1 blank
		for (int32 Pad = MinDigits - Str.Len(); Pad > 0; --Pad)
		{
			Cells.Add(bZeroes ? 0 : -1);
		}
		for (TCHAR Ch : Str)
		{
			Cells.Add(Ch == TEXT('-') ? 10 : int32(Ch - TEXT('0')));
		}
		const float TS = Member<float>(SN, W.Addr, TEXT("TextureScale"));
		const float SX = TS * F.HudCanvasScale * F.ResScaleX * F.HudScale;
		const float SY = TS * F.HudCanvasScale * F.ResScaleY * F.HudScale;
		auto CellBox = [&](int32 Index, int32& X1, int32& Y1, int32& X2, int32& Y2)
		{
			const uint8* B = MemberAddr(SD, D.Addr, TEXT("TextureCoords"), FMath::Clamp(Index, 0, 10));
			X1 = Member<int32>(Box, B, TEXT("X1"));
			Y1 = Member<int32>(Box, B, TEXT("Y1"));
			X2 = Member<int32>(Box, B, TEXT("X2"));
			Y2 = Member<int32>(Box, B, TEXT("Y2"));
		};
		float TotalW = 0.f, MaxH = 0.f;
		for (int32 Cell : Cells)
		{
			int32 X1, Y1, X2, Y2;
			CellBox(Cell < 0 ? 0 : Cell, X1, Y1, X2, Y2);
			TotalW += float(FMath::Abs(X2 - X1) + 1) * SX;
			MaxH = FMath::Max(MaxH, float(FMath::Abs(Y2 - Y1) + 1) * SY);
		}
		float X = Member<float>(SN, W.Addr, TEXT("PosX")) * F.HudCanvasScale * F.ClipX + (1.f - F.HudCanvasScale) * 0.5f * F.ClipX + float(Member<int32>(SN, W.Addr, TEXT("OffsetX"))) * SX;
		float Y = Member<float>(SN, W.Addr, TEXT("PosY")) * F.HudCanvasScale * F.ClipY + (1.f - F.HudCanvasScale) * 0.5f * F.ClipY + float(Member<int32>(SN, W.Addr, TEXT("OffsetY"))) * SY;
		float PX = 0.f, PY = 0.f;
		PivotShift(Member<uint8>(SN, W.Addr, TEXT("DrawPivot")), PX, PY);
		X -= TotalW * PX;
		Y -= MaxH * PY;
		FUSObject* Mat = Member<FUSObject*>(SD, D.Addr, TEXT("DigitTexture"));
		const FUSColor Tint = Member<FUSColor>(SN, W.Addr, TEXT("Tints"), F.TeamIndex);
		const uint8 Style = Member<uint8>(SN, W.Addr, TEXT("RenderStyle"));
		for (int32 Cell : Cells)
		{
			int32 X1, Y1, X2, Y2;
			CellBox(Cell < 0 ? 0 : Cell, X1, Y1, X2, Y2);
			const float CW = float(FMath::Abs(X2 - X1) + 1) * SX;
			if (Cell >= 0)
			{
				TileWith(C, Mat, Style, Tint, X, Y, CW, float(FMath::Abs(Y2 - Y1) + 1) * SY, float(X1), float(Y1), float(X2 - X1), float(Y2 - Y1));
			}
			X += CW;
		}
	}

	// ---- client-side helpers HUD and menu code reaches ----
	US_NATIVE(EmptyString)
	{
		USSkipNativeParms(Stack);
		if (Result)
		{
			US_RESULT(FString) = FString();
		}
	}

	/**
	 * CacheManager.GetCrosshairList: the Crosshair= records of the install's .ucl cache files (CacheRecords.ucl and the
	 * mods'), in file order, as UE2's cache manager lists them for HudCDeathmatch.SetCustomCrosshairs.
	 */
	US_NATIVE(GetCrosshairList)
	{
		USP_GET_ARRAY_REF(Out, OutProp);
		USP_FINISH;
		struct FRec { FString Name, Texture; };
		static TArray<FRec> Records;
		static bool bRead = false;
		if (!bRead)
		{
			bRead = true;
			TArray<FString> Dirs = { FUSRuntime::Get().GetGameDir() / TEXT("System") };
			for (const FString& F : FUSRuntime::Get().GetExtraFolders())
			{
				Dirs.AddUnique(F / TEXT("System"));
			}
			TSet<FString> Seen;
			for (const FString& Dir : Dirs)
			{
				TArray<FString> Files;
				IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.ucl")), true, false);
				Files.Sort();
				for (const FString& File : Files)
				{
					TArray<FString> Lines;
					FFileHelper::LoadFileToStringArray(Lines, *(Dir / File));
					for (const FString& Line : Lines)
					{
						FRec R;
						if (Line.StartsWith(TEXT("Crosshair=")) && FParse::Value(*Line, TEXT("FriendlyName="), R.Name) && FParse::Value(*Line, TEXT("CrosshairTexture="), R.Texture))
						{
							R.Texture.RemoveFromEnd(TEXT(")"));
							if (!Seen.Contains(R.Texture.ToLower()))
							{
								Seen.Add(R.Texture.ToLower());
								Records.Add(R);
							}
						}
					}
				}
			}
		}
		FUSProperty* Inner = OutProp && OutProp->Type == EUSType::Array ? OutProp->Inner : nullptr;
		FUSStruct* S = Inner ? Inner->Struct : nullptr;
		if (!Out || !S)
		{
			return;
		}
		const int32 ES = Inner->ElementSize;
		for (int32 i = 0; i < Out->Num; ++i)
		{
			Inner->DestroySingle(Out->Data + int64(i) * ES);
		}
		Out->Data = static_cast<uint8*>(FMemory::Realloc(Out->Data, int64(FMath::Max(1, Records.Num())) * ES, 16));
		Out->Max = FMath::Max(1, Records.Num());
		Out->Num = Records.Num();
		FMemory::Memzero(Out->Data, int64(Records.Num()) * ES);
		FUSClass* TexClass = FUSRuntime::Get().FindClass(TEXT("Engine.Texture"));
		FUSProperty* PName = S->FindPropertyByName(TEXT("FriendlyName"));
		FUSProperty* PTex = S->FindPropertyByName(TEXT("CrosshairTexture"));
		FUSProperty* PIndex = S->FindPropertyByName(TEXT("RecordIndex"));
		for (int32 i = 0; i < Records.Num(); ++i)
		{
			uint8* E = Out->Data + int64(i) * ES;
			if (PName)
			{
				*reinterpret_cast<FString*>(E + PName->Offset) = Records[i].Name;
			}
			if (PTex)
			{
				*reinterpret_cast<FUSObject**>(E + PTex->Offset) = FUSRuntime::Get().GetPlaceholder(Records[i].Texture, TexClass);
			}
			if (PIndex)
			{
				*reinterpret_cast<int32*>(E + PIndex->Offset) = i;
			}
		}
	}

	/** GUIFont.GetFont(XRes): FontArrayNames[index for XRes] (GUIFont.LoadFont / FontArrayFonts are not kept). */
	US_NATIVE(GUIFontGetFont)
	{
		USP_GET_INT(XRes);
		USP_FINISH;
		FUSObject* Font = nullptr;
		if (Self && Self->Data)
		{
			if (FUSProperty* Names = Self->FindProperty(TEXT("FontArrayNames")))
			{
				// GUIFont.GetFont: one font per resolution (640 / 800 / 1024 / 1280 / bigger: the menus' Verdana 8 to 16),
				// bFixedSize the first
				const FUSScriptArray* A = reinterpret_cast<const FUSScriptArray*>(Self->Data + Names->Offset);
				FUSProperty* FixedProp = Self->FindProperty(TEXT("bFixedSize"));
				const bool bFixed = FixedProp && (*reinterpret_cast<const uint32*>(Self->Data + FixedProp->Offset) & 1);
				int32 Index = 0;
				if (!bFixed)
				{
					Index = XRes <= 640 ? 0 : XRes <= 800 ? 1 : XRes <= 1024 ? 2 : XRes <= 1280 ? 3 : 4;
				}
				if (Names->Type == EUSType::Array && A->Num > 0 && Names->Inner && Names->Inner->Type == EUSType::Str)
				{
					Index = FMath::Clamp(Index, 0, A->Num - 1);
					const FString& Path = *reinterpret_cast<const FString*>(A->Data + int64(Index) * Names->Inner->ElementSize);
					Font = FUSRuntime::Get().FindObject(Path);
				}
			}
		}
		US_RESULT(FUSObject*) = Font;
	}
}

namespace USNatives
{
	void RegisterCanvas()
	{
		using namespace USCanvasNatives;
		static const FUSNativeEntry Entries[] = {
			{ TEXT("Canvas"), TEXT("StrLen"), &StrLen }, { TEXT("Canvas"), TEXT("DrawText"), &DrawText },
			{ TEXT("Canvas"), TEXT("DrawTextClipped"), &DrawTextClipped }, { TEXT("Canvas"), TEXT("TextSize"), &TextSize },
			{ TEXT("Canvas"), TEXT("DrawTextJustified"), &DrawTextJustified }, { TEXT("Canvas"), TEXT("WrapStringToArray"), &WrapStringToArray },
			{ TEXT("Canvas"), TEXT("WrapText"), &WrapText },
			{ TEXT("Canvas"), TEXT("DrawTile"), &DrawTile }, { TEXT("Canvas"), TEXT("DrawTileClipped"), &DrawTileClipped },
			{ TEXT("Canvas"), TEXT("DrawTileStretched"), &DrawTileStretched }, { TEXT("Canvas"), TEXT("DrawTileScaled"), &DrawTileScaled },
			{ TEXT("Canvas"), TEXT("DrawTileJustified"), &DrawTileJustified }, { TEXT("Canvas"), TEXT("DrawTilePartialStretched"), &DrawTilePartialStretched },
			{ TEXT("Canvas"), TEXT("WorldToScreen"), &WorldToScreen }, { TEXT("Canvas"), TEXT("GetCameraLocation"), &GetCameraLocation },
			{ TEXT("Canvas"), TEXT("DrawActor"), &Ignore }, { TEXT("Canvas"), TEXT("DrawActorClipped"), &Ignore },
			{ TEXT("Canvas"), TEXT("DrawPortal"), &Ignore }, { TEXT("Canvas"), TEXT("DrawScreenActor"), &Ignore },
			{ TEXT("Canvas"), TEXT("SetScreenLight"), &Ignore }, { TEXT("Canvas"), TEXT("SetScreenProjector"), &Ignore },
			{ TEXT("Canvas"), TEXT("Clear"), &Ignore },
			{ TEXT("HUD"), TEXT("DrawCanvasLine"), &DrawCanvasLine }, { TEXT("HUD"), TEXT("StaticDrawCanvasLine"), &StaticDrawCanvasLine },
			{ TEXT("HUD"), TEXT("Draw3DLine"), &Draw3DLine },
			{ TEXT("Material"), TEXT("MaterialUSize"), &MaterialUSize }, { TEXT("Material"), TEXT("MaterialVSize"), &MaterialVSize },
			{ TEXT("HudBase"), TEXT("DrawSpriteWidget"), &DrawSpriteWidget }, { TEXT("HudBase"), TEXT("DrawNumericWidget"), &DrawNumericWidget },
			// client code around HUDs and scoreboards: no effect on what they draw
			{ TEXT("Interaction"), TEXT("Initialize"), &InteractionInitialize }, { TEXT("FileLog"), TEXT("Logf"), &Ignore },
			{ TEXT("FileLog"), TEXT("OpenLog"), &Ignore }, { TEXT("FileLog"), TEXT("CloseLog"), &Ignore },
			{ TEXT("Actor"), TEXT("GetAnimParams"), &Ignore }, { TEXT("PlayerController"), TEXT("GetDefaultURL"), &EmptyString },
			{ TEXT("GUIFont"), TEXT("GetFont"), &GUIFontGetFont }, { TEXT("CacheManager"), TEXT("GetCrosshairList"), &GetCrosshairList },
		};
		Register(Entries, UE_ARRAY_COUNT(Entries));
	}
}

#endif // OPENUT2K4_USCRIPT_ENABLED
