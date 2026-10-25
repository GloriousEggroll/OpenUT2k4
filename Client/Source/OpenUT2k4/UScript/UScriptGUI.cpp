// The native half of UE2's menu system (XInterface / GUI2K4) for mods' menus run by the script VM: natives, subobject
// instancing, layout, drawing and input routing. See UScriptGUI.h.

#include "UScriptGUI.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"
#include "UScriptWorld.h"
#include "UScriptCanvas.h"

namespace
{
	FUSGUI* GCurrent = nullptr;

	/** UE2.5 CPF_Automated: `var automated GUIButton b_Ok;` (InitializeControls adds the component to Controls). */
	constexpr uint32 CPF_Automated = 0x80000000u;

	// XInterface.GUI enums
	enum : uint8 { MSAT_Blurry = 0, MSAT_Watched = 1, MSAT_Focused = 2, MSAT_Pressed = 3, MSAT_Disabled = 4 };
	enum : uint8 { TXTA_Left = 0, TXTA_Center = 1, TXTA_Right = 2 };
	enum : uint8 { ISTY_Normal = 0, ISTY_Stretched = 1, ISTY_Scaled = 2, ISTY_Bound = 3, ISTY_Justified = 4, ISTY_PartialScaled = 5, ISTY_Tiled = 6 };
	enum : uint8 { ICP_Normal = 0, ICP_Center = 1, ICP_Scaled = 2, ICP_Stretched = 3, ICP_Bound = 4 };
	enum : uint8 { MSTY_None = 0, MSTY_Normal = 1, MSTY_Alpha = 5, MSTY_Modulated = 4 };
	enum : uint8 { SCALE_All = 0, SCALE_X = 1, SCALE_Y = 2 };
	enum : uint8 { ORIENT_Vertical = 0, ORIENT_Horizontal = 1 };

	// ---------------------------------------------------------------------------------------------
	// Property access (by name: FUSClass keeps a name map of every property, inherited ones included)
	// ---------------------------------------------------------------------------------------------

	FUSProperty* Prop(const FUSObject* O, const TCHAR* Name, EUSType Type)
	{
		if (!O || !O->Data || !O->Class)
		{
			return nullptr;
		}
		FUSProperty* P = O->Class->FindPropertyByName(FName(Name));
		if (!P && Type == EUSType::Delegate)
		{
			// UE2 names a delegate's variable __OnClick__Delegate
			static TMap<const void*, FName> Names;
			FName* N = Names.Find(static_cast<const void*>(Name));
			if (!N)
			{
				N = &Names.Add(static_cast<const void*>(Name), FName(*FString::Printf(TEXT("__%s__Delegate"), Name)));
			}
			P = O->Class->FindPropertyByName(*N);
		}
		return P && P->Type == Type ? P : nullptr;
	}

	template <typename T>
	T* Ptr(const FUSObject* O, const TCHAR* Name, EUSType Type)
	{
		FUSProperty* P = Prop(O, Name, Type);
		return P ? reinterpret_cast<T*>(O->Data + P->Offset) : nullptr;
	}

	float GetF(const FUSObject* O, const TCHAR* N, float Def = 0.f) { const float* V = Ptr<float>(O, N, EUSType::Float); return V ? *V : Def; }
	int32 GetI(const FUSObject* O, const TCHAR* N, int32 Def = 0) { const int32* V = Ptr<int32>(O, N, EUSType::Int); return V ? *V : Def; }
	uint8 GetB(const FUSObject* O, const TCHAR* N, uint8 Def = 0) { const uint8* V = Ptr<uint8>(O, N, EUSType::Byte); return V ? *V : Def; }
	bool GetBool(const FUSObject* O, const TCHAR* N) { const uint32* V = Ptr<uint32>(O, N, EUSType::Bool); return V && *V != 0; }
	FUSObject* GetO(const FUSObject* O, const TCHAR* N)
	{
		FUSObject* const* V = Ptr<FUSObject*>(O, N, EUSType::Object);
		FUSObject* R = V ? *V : nullptr;
		return R && !R->IsPendingKill() ? R : nullptr;
	}
	FString GetS(const FUSObject* O, const TCHAR* N) { const FString* V = Ptr<FString>(O, N, EUSType::Str); return V ? *V : FString(); }
	FUSColor GetC(const FUSObject* O, const TCHAR* N)
	{
		FUSProperty* P = Prop(O, N, EUSType::Struct);
		return P && P->ElementSize >= int32(sizeof(FUSColor)) ? *reinterpret_cast<const FUSColor*>(O->Data + P->Offset) : FUSColor{ 255, 255, 255, 255 };
	}
	void SetF(FUSObject* O, const TCHAR* N, float V) { if (float* P = Ptr<float>(O, N, EUSType::Float)) { *P = V; } }
	void SetI(FUSObject* O, const TCHAR* N, int32 V) { if (int32* P = Ptr<int32>(O, N, EUSType::Int)) { *P = V; } }
	void SetB(FUSObject* O, const TCHAR* N, uint8 V) { if (uint8* P = Ptr<uint8>(O, N, EUSType::Byte)) { *P = V; } }
	void SetBool(FUSObject* O, const TCHAR* N, bool V) { if (uint32* P = Ptr<uint32>(O, N, EUSType::Bool)) { *P = V ? 1u : 0u; } }
	void SetO(FUSObject* O, const TCHAR* N, FUSObject* V) { if (FUSObject** P = Ptr<FUSObject*>(O, N, EUSType::Object)) { *P = V; } }
	void SetS(FUSObject* O, const TCHAR* N, const FString& V) { if (FString* P = Ptr<FString>(O, N, EUSType::Str)) { *P = V; } }

	/** A dynamic array of object references. */
	struct FObjArray
	{
		FUSScriptArray* A = nullptr;
		int32 Num() const { return A ? A->Num : 0; }
		FUSObject* operator[](int32 i) const
		{
			FUSObject* O = (A && i >= 0 && i < A->Num) ? reinterpret_cast<FUSObject* const*>(A->Data)[i] : nullptr;
			return O && !O->IsPendingKill() ? O : nullptr;
		}
	};
	FObjArray GetArr(const FUSObject* O, const TCHAR* N)
	{
		FUSProperty* P = Prop(O, N, EUSType::Array);
		if (!P || !P->Inner || (P->Inner->Type != EUSType::Object && P->Inner->Type != EUSType::Class) || P->Inner->ElementSize != int32(sizeof(FUSObject*)))
		{
			return FObjArray();
		}
		return FObjArray{ reinterpret_cast<FUSScriptArray*>(O->Data + P->Offset) };
	}

	/** Replaces an object array's contents. */
	void SetArr(FUSObject* O, const TCHAR* N, const TArray<FUSObject*>& Items)
	{
		FUSProperty* P = Prop(O, N, EUSType::Array);
		if (!P || !P->Inner || P->Inner->Type != EUSType::Object)
		{
			return;
		}
		FUSScriptArray& A = *reinterpret_cast<FUSScriptArray*>(O->Data + P->Offset);
		if (A.Max < Items.Num())
		{
			A.Data = static_cast<uint8*>(FMemory::Realloc(A.Data, int64(Items.Num()) * sizeof(FUSObject*), 16));
			A.Max = Items.Num();
		}
		if (Items.Num())
		{
			FMemory::Memcpy(A.Data, Items.GetData(), Items.Num() * sizeof(FUSObject*));
		}
		A.Num = Items.Num();
	}

	/** Bounds[4] / ClientBounds[4] / MouseOffset[4] ... */
	float* FloatArray(FUSObject* O, const TCHAR* N, int32 MinDim)
	{
		FUSProperty* P = Prop(O, N, EUSType::Float);
		return P && P->ArrayDim >= MinDim ? reinterpret_cast<float*>(O->Data + P->Offset) : nullptr;
	}

	// ---------------------------------------------------------------------------------------------
	// Classes
	// ---------------------------------------------------------------------------------------------

	struct FGUIClasses
	{
		bool bResolved = false;
		FUSClass *GUI = nullptr, *Component = nullptr, *Multi = nullptr, *Page = nullptr, *Label = nullptr, *Image = nullptr, *Button = nullptr,
			*GFXButton = nullptr, *CheckBox = nullptr, *EditBox = nullptr, *Slider = nullptr, *Combo = nullptr, *ListBoxBase = nullptr,
			*ListBase = nullptr, *List = nullptr, *MCList = nullptr, *MCListBox = nullptr, *MCHeader = nullptr, *ScrollBar = nullptr,
			*ScrollZone = nullptr, *Numeric = nullptr, *MenuOption = nullptr, *TabControl = nullptr, *TabButton = nullptr, *TabPanel = nullptr,
			*Panel = nullptr, *Section = nullptr, *ScrollText = nullptr, *ToolTip = nullptr, *Styles = nullptr, *Font = nullptr,
			*Controller = nullptr, *ContextMenu = nullptr, *VertList = nullptr;

		void Resolve()
		{
			if (bResolved)
			{
				return;
			}
			FUSRuntime& RT = FUSRuntime::Get();
			auto C = [&RT](const TCHAR* Name) { return RT.FindClass(FString(TEXT("XInterface.")) + Name); };
			GUI = C(TEXT("GUI"));
			if (!GUI)
			{
				return;
			}
			bResolved = true;
			Component = C(TEXT("GUIComponent"));
			Multi = C(TEXT("GUIMultiComponent"));
			Page = C(TEXT("GUIPage"));
			Label = C(TEXT("GUILabel"));
			Image = C(TEXT("GUIImage"));
			Button = C(TEXT("GUIButton"));
			GFXButton = C(TEXT("GUIGFXButton"));
			CheckBox = C(TEXT("GUICheckBoxButton"));
			EditBox = C(TEXT("GUIEditBox"));
			Slider = C(TEXT("GUISlider"));
			Combo = C(TEXT("GUIComboBox"));
			ListBoxBase = C(TEXT("GUIListBoxBase"));
			ListBase = C(TEXT("GUIListBase"));
			VertList = C(TEXT("GUIVertList"));
			List = C(TEXT("GUIList"));
			MCList = C(TEXT("GUIMultiColumnList"));
			MCListBox = C(TEXT("GUIMultiColumnListBox"));
			MCHeader = C(TEXT("GUIMultiColumnListHeader"));
			ScrollBar = C(TEXT("GUIScrollBarBase"));
			ScrollZone = C(TEXT("GUIScrollZoneBase"));
			Numeric = C(TEXT("GUINumericEdit"));
			MenuOption = C(TEXT("GUIMenuOption"));
			TabControl = C(TEXT("GUITabControl"));
			TabButton = C(TEXT("GUITabButton"));
			TabPanel = C(TEXT("GUITabPanel"));
			Panel = C(TEXT("GUIPanel"));
			Section = C(TEXT("GUISectionBackground"));
			ScrollText = C(TEXT("GUIScrollText"));
			ToolTip = C(TEXT("GUIToolTip"));
			Styles = C(TEXT("GUIStyles"));
			Font = C(TEXT("GUIFont"));
			Controller = C(TEXT("GUIController"));
			ContextMenu = C(TEXT("GUIContextMenu"));
		}
	};

	FGUIClasses& K()
	{
		static FGUIClasses Classes;
		Classes.Resolve();
		return Classes;
	}

	bool Is(const FUSObject* O, const FUSClass* C)
	{
		return O && C && O->Class && O->Data && O->Class->IsChildOf(C);
	}

	// ---------------------------------------------------------------------------------------------
	// Calling script: delegates (bound function, else the delegate's own body) and events
	// ---------------------------------------------------------------------------------------------

	struct FScriptCall
	{
		FUSObject* Target = nullptr;
		FUSFunction* Function = nullptr;
	};

	/** The function a delegate of Self calls: the bound one, or (bDefault) the delegate's own body. */
	FScriptCall FindDelegate(FUSObject* Self, const TCHAR* Name, bool bDefault)
	{
		FUSProperty* P = Prop(Self, Name, EUSType::Delegate);
		if (!P)
		{
			return FScriptCall();
		}
		const FUSDelegate& D = *reinterpret_cast<const FUSDelegate*>(Self->Data + P->Offset);
		if (D.Object && !D.Object->IsPendingKill() && D.Object->Data && !(D.Object->Flags & (USF_Field | USF_Placeholder)) && D.FunctionName != NAME_None)
		{
			if (FUSFunction* F = USVM::FindFunction(D.Object, D.FunctionName))
			{
				return FScriptCall{ D.Object, F };
			}
		}
		if (bDefault)
		{
			if (FUSFunction* F = USVM::FindFunction(Self, FName(Name)))
			{
				return FScriptCall{ Self, F };
			}
		}
		return FScriptCall();
	}

	bool IsBound(FUSObject* Self, const TCHAR* Name)
	{
		return FindDelegate(Self, Name, false).Function != nullptr;
	}

	/** Calls delegate Name of Self (Fill adds the parameters); the bool it returns (false for void ones). */
	template <typename FillFn>
	bool CallDelegate(FUSObject* Self, const TCHAR* Name, FillFn&& Fill, bool bDefault = true)
	{
		const FScriptCall C = FindDelegate(Self, Name, bDefault);
		if (!C.Function)
		{
			return false;
		}
		FUSParms Parms(C.Function);
		Fill(Parms);
		Parms.Call(C.Target);
		return C.Function->ReturnProperty && C.Function->ReturnProperty->Type == EUSType::Bool && Parms.ReturnBool();
	}

	/** Calls a script function / event of O by name. */
	template <typename FillFn>
	bool CallEvent(FUSObject* O, const TCHAR* Name, FillFn&& Fill)
	{
		FUSFunction* F = O ? USVM::FindFunction(O, FName(Name)) : nullptr;
		if (!F)
		{
			return false;
		}
		FUSParms Parms(F);
		Fill(Parms);
		Parms.Call(O);
		return F->ReturnProperty && F->ReturnProperty->Type == EUSType::Bool && Parms.ReturnBool();
	}

	bool CallEvent0(FUSObject* O, const TCHAR* Name)
	{
		return CallEvent(O, Name, [](FUSParms&) {});
	}

	void MenuStateChange(FUSObject* C, uint8 NewState)
	{
		if (C && GetB(C, TEXT("MenuState")) != NewState)
		{
			CallEvent(C, TEXT("MenuStateChange"), [NewState](FUSParms& P) { P.Byte(NewState); });
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Positions (GUIComponent ActualLeft / ActualWidth ... and their inverses)
	// ---------------------------------------------------------------------------------------------

	float ScreenW() { return GCurrent ? GCurrent->ResX : 1024.f; }
	float ScreenH() { return GCurrent ? GCurrent->ResY : 768.f; }

	/** Values in (-2, 2) are relative (to the owner when bScaleToParent, else the screen) unless bNeverScale. */
	bool IsRelative(const FUSObject* C, float V)
	{
		return V > -2.f && V < 2.f && !GetBool(C, TEXT("bNeverScale"));
	}

	float ActualDim(FUSObject* C, float V, bool bWidth, int32 Depth = 0);

	float OwnerDim(FUSObject* C, bool bWidth, int32 Depth)
	{
		FUSObject* Owner = GetO(C, TEXT("MenuOwner"));
		const uint8 ST = GetB(C, TEXT("ScalingType"));
		if (Owner && Depth < 32 && GetBool(C, TEXT("bScaleToParent")) && (bWidth ? ST != SCALE_Y : ST != SCALE_X))
		{
			return ActualDim(Owner, GetF(Owner, bWidth ? TEXT("WinWidth") : TEXT("WinHeight")), bWidth, Depth + 1);
		}
		return bWidth ? ScreenW() : ScreenH();
	}

	float ActualDim(FUSObject* C, float V, bool bWidth, int32 Depth)
	{
		return IsRelative(C, V) ? V * OwnerDim(C, bWidth, Depth) : V;
	}

	float ActualPos(FUSObject* C, float V, bool bLeft, int32 Depth = 0)
	{
		float R = IsRelative(C, V) ? V * OwnerDim(C, bLeft, Depth) : V;
		FUSObject* Owner = GetO(C, TEXT("MenuOwner"));
		const uint8 BT = GetB(C, TEXT("BoundingType"));
		if (Owner && Depth < 32 && GetBool(C, TEXT("bBoundToParent")) && (bLeft ? BT != SCALE_Y : BT != SCALE_X))
		{
			R += ActualPos(Owner, GetF(Owner, bLeft ? TEXT("WinLeft") : TEXT("WinTop")), bLeft, Depth + 1);
		}
		return R;
	}

	float RelativeDim(FUSObject* C, float Real, bool bWidth)
	{
		const float Base = OwnerDim(C, bWidth, 0);
		return Base > 0.f ? Real / Base : 0.f;
	}

	float RelativePos(FUSObject* C, float Real, bool bLeft)
	{
		FUSObject* Owner = GetO(C, TEXT("MenuOwner"));
		const uint8 BT = GetB(C, TEXT("BoundingType"));
		if (Owner && GetBool(C, TEXT("bBoundToParent")) && (bLeft ? BT != SCALE_Y : BT != SCALE_X))
		{
			Real -= ActualPos(Owner, GetF(Owner, bLeft ? TEXT("WinLeft") : TEXT("WinTop")), bLeft);
		}
		const float Base = OwnerDim(C, bLeft, 0);
		return Base > 0.f ? Real / Base : 0.f;
	}

	struct FBox4
	{
		float L = 0.f, T = 0.f, W = 0.f, H = 0.f;
		float R() const { return L + W; }
		float B() const { return T + H; }
		bool Contains(float X, float Y) const { return X >= L && X <= L + W && Y >= T && Y <= T + H; }
	};

	FBox4 BoundsOf(FUSObject* C)
	{
		const float* B = FloatArray(C, TEXT("Bounds"), 4);
		return B ? FBox4{ B[0], B[1], B[2] - B[0], B[3] - B[1] } : FBox4();
	}

	FBox4 ClientOf(FUSObject* C)
	{
		const float* B = FloatArray(C, TEXT("ClientBounds"), 4);
		return B ? FBox4{ B[0], B[1], B[2] - B[0], B[3] - B[1] } : BoundsOf(C);
	}

	/** Bounds and ClientBounds (the bounds less the style's border). */
	void SetBounds(FUSObject* C, const FBox4& R)
	{
		if (float* B = FloatArray(C, TEXT("Bounds"), 4))
		{
			B[0] = R.L;
			B[1] = R.T;
			B[2] = R.L + R.W;
			B[3] = R.T + R.H;
		}
		if (float* CB = FloatArray(C, TEXT("ClientBounds"), 4))
		{
			float Off[4] = { 0.f, 0.f, 0.f, 0.f };
			if (FUSObject* S = GetO(C, TEXT("Style")))
			{
				if (const int32* BO = Ptr<int32>(S, TEXT("BorderOffsets"), EUSType::Int))
				{
					for (int32 i = 0; i < 4; ++i)
					{
						Off[i] = float(BO[i]);
					}
				}
			}
			// never more border than half the box
			const float BX = FMath::Min(Off[0] + Off[2], R.W * 0.5f) / FMath::Max(1.f, Off[0] + Off[2]);
			const float BY = FMath::Min(Off[1] + Off[3], R.H * 0.5f) / FMath::Max(1.f, Off[1] + Off[3]);
			CB[0] = R.L + Off[0] * BX;
			CB[1] = R.T + Off[1] * BY;
			CB[2] = R.L + R.W - Off[2] * BX;
			CB[3] = R.T + R.H - Off[3] * BY;
		}
		SetBool(C, TEXT("bPositioned"), true);
	}

	void UpdateBounds(FUSObject* C)
	{
		FBox4 R;
		R.L = ActualPos(C, GetF(C, TEXT("WinLeft")), true);
		R.T = ActualPos(C, GetF(C, TEXT("WinTop")), false);
		R.W = ActualDim(C, GetF(C, TEXT("WinWidth")), true);
		R.H = ActualDim(C, GetF(C, TEXT("WinHeight")), false);
		SetBounds(C, R);
	}

	/** Positions a component UE2 lays out natively (a combo box's parts, a menu option's caption ...) in pixels. */
	void Place(FUSObject* C, float X, float Y, float W, float H)
	{
		if (!C)
		{
			return;
		}
		SetBool(C, TEXT("bNeverScale"), true);
		SetBool(C, TEXT("bBoundToParent"), false);
		SetBool(C, TEXT("bScaleToParent"), false);
		SetF(C, TEXT("WinLeft"), X);
		SetF(C, TEXT("WinTop"), Y);
		SetF(C, TEXT("WinWidth"), FMath::Max(0.f, W));
		SetF(C, TEXT("WinHeight"), FMath::Max(0.f, H));
	}

	// ---------------------------------------------------------------------------------------------
	// Drawing (the canvas backend: UScriptCanvas.h)
	// ---------------------------------------------------------------------------------------------

	FColor GModulate(255, 255, 255, 255);

	FColor ToColor(const FUSColor& C)
	{
		return FColor(uint8(C.R * GModulate.R / 255), uint8(C.G * GModulate.G / 255), uint8(C.B * GModulate.B / 255), uint8(C.A * GModulate.A / 255));
	}

	FColor Modulated(const FColor& C)
	{
		return FColor(uint8(C.R * GModulate.R / 255), uint8(C.G * GModulate.G / 255), uint8(C.B * GModulate.B / 255), uint8(C.A * GModulate.A / 255));
	}

	/** EMenuRenderStyle -> ERenderStyle (same values; None draws nothing). */
	uint8 RenderStyle(uint8 MenuStyle)
	{
		return MenuStyle == MSTY_None ? 0 : MenuStyle;
	}

	bool MatSize(FUSObject* Mat, float& U, float& V)
	{
		int32 IU = 0, IV = 0;
		IUSCanvasBackend* B = USCanvas::GetBackend();
		if (Mat && B && B->MaterialSize(Mat, IU, IV) && IU > 0 && IV > 0)
		{
			U = float(IU);
			V = float(IV);
			return true;
		}
		U = V = 0.f;
		return false;
	}

	void Tile(FUSObject* Mat, float X, float Y, float W, float H, float U, float V, float UL, float VL, const FColor& Color, uint8 Style)
	{
		IUSCanvasBackend* B = USCanvas::GetBackend();
		if (B && Mat && Style != 0 && W != 0.f && H != 0.f && Color.A > 0)
		{
			B->DrawTile(Mat, X, Y, W, H, U, V, UL, VL, Color, Style);
		}
	}

	void Fill(const FBox4& R, const FColor& Color)
	{
		IUSCanvasBackend* B = USCanvas::GetBackend();
		if (B && R.W > 0.f && R.H > 0.f && Color.A > 0)
		{
			B->DrawRect(R.L, R.T, R.W, R.H, Modulated(Color));
		}
	}

	/** UE2 UCanvas::DrawTileStretched: the corners 1:1, the middle rows and columns stretched. */
	void Stretched(FUSObject* Mat, const FBox4& R, const FColor& Color, uint8 Style)
	{
		float MW = 0.f, MH = 0.f;
		if (!MatSize(Mat, MW, MH))
		{
			return;
		}
		const float MidX = FMath::FloorToFloat(MW * 0.5f), MidY = FMath::FloorToFloat(MH * 0.5f);
		const float CW = FMath::Min(MidX, R.W * 0.5f), CH = FMath::Min(MidY, R.H * 0.5f);
		const float SW = R.W - CW * 2.f, SH = R.H - CH * 2.f;
		Tile(Mat, R.L, R.T, CW, CH, 0.f, 0.f, CW, CH, Color, Style);
		Tile(Mat, R.R() - CW, R.T, CW, CH, MW - CW, 0.f, CW, CH, Color, Style);
		Tile(Mat, R.L, R.B() - CH, CW, CH, 0.f, MH - CH, CW, CH, Color, Style);
		Tile(Mat, R.R() - CW, R.B() - CH, CW, CH, MW - CW, MH - CH, CW, CH, Color, Style);
		if (SW > 0.f)
		{
			Tile(Mat, R.L + CW, R.T, SW, CH, MidX, 0.f, 1.f, CH, Color, Style);
			Tile(Mat, R.L + CW, R.B() - CH, SW, CH, MidX, MH - CH, 1.f, CH, Color, Style);
		}
		if (SH > 0.f)
		{
			Tile(Mat, R.L, R.T + CH, CW, SH, 0.f, MidY, CW, 1.f, Color, Style);
			Tile(Mat, R.R() - CW, R.T + CH, CW, SH, MW - CW, MidY, CW, 1.f, Color, Style);
		}
		if (SW > 0.f && SH > 0.f)
		{
			Tile(Mat, R.L + CW, R.T + CH, SW, SH, MidX, MidY, 1.f, 1.f, Color, Style);
		}
	}

	/** A material drawn into R with a UE2 image style (GUIImage, GUIStyles, button graphics). Sub = X1, Y1, X2, Y2 (-1: whole). */
	void DrawMaterial(FUSObject* Mat, const FBox4& R, uint8 ImgStyle, const FColor& InColor, uint8 Style, uint8 Align = 0, const int32* Sub = nullptr)
	{
		float MW = 0.f, MH = 0.f;
		if (!Mat || R.W <= 0.f || R.H <= 0.f || Style == 0 || !MatSize(Mat, MW, MH))
		{
			return;
		}
		const FColor Color = Modulated(InColor);
		float U = 0.f, V = 0.f, UL = MW, VL = MH;
		const bool bSub = Sub && (Sub[0] >= 0 || Sub[1] >= 0 || Sub[2] >= 0 || Sub[3] >= 0);
		if (bSub)
		{
			U = float(FMath::Max(Sub[0], 0));
			V = float(FMath::Max(Sub[1], 0));
			UL = (Sub[2] >= 0 ? float(Sub[2]) : MW) - U;
			VL = (Sub[3] >= 0 ? float(Sub[3]) : MH) - V;
		}
		switch (ImgStyle)
		{
		case ISTY_Normal:
		case ISTY_Bound:
		{
			const float W = FMath::Min(FMath::Abs(UL), R.W), H = FMath::Min(FMath::Abs(VL), R.H);
			Tile(Mat, R.L, R.T, W, H, U, V, UL < 0.f ? -W : W, VL < 0.f ? -H : H, Color, Style);
			break;
		}
		case ISTY_Stretched:
		case ISTY_PartialScaled:
			if (bSub)
			{
				Tile(Mat, R.L, R.T, R.W, R.H, U, V, UL, VL, Color, Style);
			}
			else
			{
				Stretched(Mat, R, Color, Style);
			}
			break;
		case ISTY_Justified:
		{
			const float AU = FMath::Abs(UL), AV = FMath::Abs(VL);
			if (AU <= 0.f || AV <= 0.f)
			{
				break;
			}
			float W = R.W, H = R.H;
			if (AU / AV > R.W / R.H)
			{
				H = R.W * AV / AU;
			}
			else
			{
				W = R.H * AU / AV;
			}
			const float J = Align == 1 ? 0.5f : (Align == 2 ? 1.f : 0.f);
			Tile(Mat, R.L + (R.W - W) * J, R.T + (R.H - H) * J, W, H, U, V, UL, VL, Color, Style);
			break;
		}
		case ISTY_Tiled:
			Tile(Mat, R.L, R.T, R.W, R.H, U, V, R.W, R.H, Color, Style);
			break;
		case ISTY_Scaled:
		default:
			Tile(Mat, R.L, R.T, R.W, R.H, U, V, UL, VL, Color, Style);
			break;
		}
	}

	// ---- fonts ----

	/** GUIFont.GetFont(XRes): FontArrayNames[index] for the resolution (bFixedSize: the first). */
	FUSObject* ResolveGUIFont(FUSObject* GUIFont, float XRes)
	{
		if (!GUIFont || !GUIFont->Data)
		{
			return nullptr;
		}
		FUSProperty* Names = Prop(GUIFont, TEXT("FontArrayNames"), EUSType::Array);
		if (!Names || !Names->Inner || Names->Inner->Type != EUSType::Str)
		{
			return nullptr;
		}
		const FUSScriptArray* A = reinterpret_cast<const FUSScriptArray*>(GUIFont->Data + Names->Offset);
		if (A->Num <= 0)
		{
			return nullptr;
		}
		int32 Index = 0;
		if (!GetBool(GUIFont, TEXT("bFixedSize")))
		{
			Index = XRes <= 640.f ? 0 : XRes <= 800.f ? 1 : XRes <= 1024.f ? 2 : XRes <= 1280.f ? 3 : 4;
		}
		Index = FMath::Clamp(Index, 0, A->Num - 1);
		const FString& Path = *reinterpret_cast<const FString*>(A->Data + int64(Index) * Names->Inner->ElementSize);
		static TMap<FString, FUSObject*> Cache;
		if (FUSObject** Hit = Cache.Find(Path))
		{
			return *Hit;
		}
		FUSObject* F = FUSRuntime::Get().FindObject(Path);
		Cache.Add(Path, F);
		return F;
	}

	/** GUIController.GetMenuFont: the FontStack entry with that KeyName. */
	FUSObject* MenuFont(FUSObject* Controller, const FString& KeyName)
	{
		const FObjArray Fonts = GetArr(Controller, TEXT("FontStack"));
		for (int32 i = 0; i < Fonts.Num(); ++i)
		{
			if (Fonts[i] && GetS(Fonts[i], TEXT("KeyName")).Equals(KeyName, ESearchCase::IgnoreCase))
			{
				return Fonts[i];
			}
		}
		return nullptr;
	}

	FUSObject* ControllerOf(const FUSObject* C)
	{
		FUSObject* Ctl = GetO(C, TEXT("Controller"));
		return Ctl ? Ctl : (GCurrent ? GCurrent->GetController() : nullptr);
	}

	void TextSize(FUSObject* Font, const FString& Text, float& XL, float& YL)
	{
		XL = YL = 0.f;
		if (IUSCanvasBackend* B = USCanvas::GetBackend(); B && Font)
		{
			B->TextSize(Font, Text.IsEmpty() ? FString(TEXT(" ")) : Text, 1.f, 1.f, XL, YL);
			if (Text.IsEmpty())
			{
				XL = 0.f;
			}
		}
	}

	void DrawText(FUSObject* Font, const FString& Text, float X, float Y, const FColor& Color, uint8 Style)
	{
		if (IUSCanvasBackend* B = USCanvas::GetBackend(); B && Font && !Text.IsEmpty() && Color.A > 0)
		{
			B->DrawText(Font, Text, X, Y, 1.f, 1.f, Modulated(Color), Style == 0 ? 1 : Style);
		}
	}

	/** Text clipped to Width (characters cut from the end; colour codes kept whole). */
	FString ClipText(FUSObject* Font, const FString& Text, float Width)
	{
		float XL = 0.f, YL = 0.f;
		TextSize(Font, Text, XL, YL);
		if (XL <= Width || Text.IsEmpty())
		{
			return Text;
		}
		int32 Lo = 0, Hi = Text.Len();
		while (Lo < Hi)
		{
			const int32 Mid = (Lo + Hi + 1) / 2;
			TextSize(Font, Text.Left(Mid), XL, YL);
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

	/** Text aligned in R (vertically centred), clipped to R's width. */
	void TextInBox(FUSObject* Font, const FString& Text, const FBox4& R, uint8 Align, const FColor& Color, uint8 Style)
	{
		if (!Font || Text.IsEmpty())
		{
			return;
		}
		const FString Shown = ClipText(Font, Text, FMath::Max(0.f, R.W));
		float XL = 0.f, YL = 0.f;
		TextSize(Font, Shown, XL, YL);
		float X = R.L;
		if (Align == TXTA_Center)
		{
			X = R.L + (R.W - XL) * 0.5f;
		}
		else if (Align == TXTA_Right)
		{
			X = R.R() - XL;
		}
		DrawText(Font, Shown, FMath::RoundToFloat(X), FMath::RoundToFloat(R.T + (R.H - YL) * 0.5f), Color, Style);
	}

	/** Word wrapped lines of Text within Width ("|" and newlines break). */
	void WrapText(FUSObject* Font, const FString& Text, float Width, TArray<FString>& Out)
	{
		TArray<FString> Paragraphs;
		Text.Replace(TEXT("|"), TEXT("\n")).ParseIntoArray(Paragraphs, TEXT("\n"), false);
		for (const FString& Para : Paragraphs)
		{
			TArray<FString> Words;
			Para.ParseIntoArray(Words, TEXT(" "), true);
			FString Line;
			for (const FString& W : Words)
			{
				const FString Try = Line.IsEmpty() ? W : Line + TEXT(" ") + W;
				float XL = 0.f, YL = 0.f;
				TextSize(Font, Try, XL, YL);
				if (XL > Width && !Line.IsEmpty())
				{
					Out.Add(Line);
					Line = W;
				}
				else
				{
					Line = Try;
				}
			}
			Out.Add(Line);
		}
	}

	// ---- styles (GUIStyles.Draw / DrawText / TextSize) ----

	FUSObject* StyleFont(FUSObject* S, uint8 State, uint8 FontScale)
	{
		const int32 i = FMath::Clamp(int32(FontScale), 0, 2) * 5 + FMath::Clamp(int32(State), 0, 4);
		FUSObject* GF = nullptr;
		if (FUSProperty* P = Prop(S, TEXT("Fonts"), EUSType::Object); P && P->ArrayDim > i)
		{
			GF = reinterpret_cast<FUSObject* const*>(S->Data + P->Offset)[i];
		}
		if (!GF)
		{
			if (FUSProperty* P = Prop(S, TEXT("FontNames"), EUSType::Str); P && P->ArrayDim > i)
			{
				GF = MenuFont(ControllerOf(S), reinterpret_cast<const FString*>(S->Data + P->Offset)[i]);
			}
		}
		return ResolveGUIFont(GF, ScreenW());
	}

	template <typename T>
	T StateValue(FUSObject* S, const TCHAR* Name, EUSType Type, uint8 State, T Def)
	{
		FUSProperty* P = Prop(S, Name, Type);
		const int32 i = FMath::Clamp(int32(State), 0, 4);
		return P && P->ArrayDim > i ? reinterpret_cast<const T*>(S->Data + P->Offset)[i] : Def;
	}

	FUSColor StateColor(FUSObject* S, const TCHAR* Name, uint8 State)
	{
		FUSProperty* P = Prop(S, Name, EUSType::Struct);
		const int32 i = FMath::Clamp(int32(State), 0, 4);
		return P && P->ArrayDim > i && P->ElementSize >= int32(sizeof(FUSColor)) ? *reinterpret_cast<const FUSColor*>(S->Data + P->Offset + i * P->ElementSize)
			: FUSColor{ 255, 255, 255, 255 };
	}

	void StyleDraw(FUSObject* S, uint8 State, const FBox4& R)
	{
		if (!S || R.W <= 0.f || R.H <= 0.f)
		{
			return;
		}
		State = FMath::Min<uint8>(State, 4);
		if (IsBound(S, TEXT("OnDraw")) && GCurrent && CallDelegate(S, TEXT("OnDraw"), [&](FUSParms& P) { P.Object(GCurrent->CanvasObj).Byte(State).Float(R.L).Float(R.T).Float(R.W).Float(R.H); }))
		{
			return;
		}
		FUSObject* Img = StateValue<FUSObject*>(S, TEXT("Images"), EUSType::Object, State, nullptr);
		const uint8 ImgStyle = StateValue<uint8>(S, TEXT("ImgStyle"), EUSType::Byte, State, ISTY_Stretched);
		const uint8 RStyle = StateValue<uint8>(S, TEXT("RStyles"), EUSType::Byte, State, MSTY_Normal);
		const FUSColor IC = StateColor(S, TEXT("ImgColors"), State);
		DrawMaterial(Img, R, ImgStyle, FColor(IC.R, IC.G, IC.B, IC.A), RenderStyle(RStyle == MSTY_Normal ? MSTY_Alpha : RStyle));
	}

	void StyleText(FUSObject* S, uint8 State, const FBox4& R, uint8 Align, const FString& Text, uint8 FontScale)
	{
		if (!S || Text.IsEmpty())
		{
			return;
		}
		State = FMath::Min<uint8>(State, 4);
		if (IsBound(S, TEXT("OnDrawText")) && GCurrent
			&& CallDelegate(S, TEXT("OnDrawText"), [&](FUSParms& P) { P.Object(GCurrent->CanvasObj).Byte(State).Float(R.L).Float(R.T).Float(R.W).Float(R.H).Byte(Align).Str(Text).Byte(FontScale); }))
		{
			return;
		}
		const FUSColor FC = StateColor(S, TEXT("FontColors"), State);
		TextInBox(StyleFont(S, State, FontScale), Text, R, Align, FColor(FC.R, FC.G, FC.B, FC.A), MSTY_Alpha);
	}

	void StyleTextSize(FUSObject* S, uint8 State, const FString& Text, uint8 FontScale, float& XL, float& YL)
	{
		TextSize(StyleFont(S, FMath::Min<uint8>(State, 4), FontScale), Text, XL, YL);
	}

	/** Row height of a list: GetItemHeight when bound, else its style font's height. */
	float ItemHeightOf(FUSObject* L)
	{
		if (!L)
		{
			return 16.f;
		}
		if (IsBound(L, TEXT("GetItemHeight")) && GCurrent)
		{
			const FScriptCall C = FindDelegate(L, TEXT("GetItemHeight"), false);
			FUSParms Parms(C.Function);
			Parms.Object(GCurrent->CanvasObj);
			Parms.Call(C.Target);
			const float H = Parms.ReturnFloat();
			if (H > 0.f)
			{
				return H;
			}
		}
		float XL = 0.f, YL = 0.f;
		FUSObject* S = GetO(L, TEXT("Style"));
		StyleTextSize(S, MSAT_Blurry, TEXT("Wy"), GetB(L, TEXT("FontScale")), XL, YL);
		return YL > 0.f ? YL + 2.f : FMath::Max(12.f, ScreenH() * 0.025f);
	}

	// ---------------------------------------------------------------------------------------------
	// Subobject instancing
	// ---------------------------------------------------------------------------------------------

	struct FInstancer
	{
		FUSWorld* World = nullptr;
		TMap<TPair<FUSObject*, FUSObject*>, FUSObject*> Made;
		int32 Depth = 0;

		static FUSClass* ClassClass()
		{
			static FUSClass* C = FUSRuntime::Get().GetIntrinsicClass(TEXT("Class"));
			return C;
		}

		/** The class a subobject template was declared in ("Pkg.Class.Name" -> Pkg.Class). */
		static FUSClass* TemplateClass(FUSObject* T)
		{
			static TMap<FUSObject*, FUSClass*> Cache;
			if (FUSClass** Hit = Cache.Find(T))
			{
				return *Hit;
			}
			FUSClass* Result = nullptr;
			const FString Path = T->GetPathName();
			int32 Dot = INDEX_NONE;
			if (Path.FindLastChar(TEXT('.'), Dot) && Dot > 0)
			{
				const FString Outer = Path.Left(Dot);
				if (Outer.Contains(TEXT(".")))
				{
					Result = FUSRuntime::Get().FindClass(Outer);
				}
			}
			Cache.Add(T, Result);
			return Result;
		}

		static bool IsTemplate(FUSObject* V)
		{
			return V && (V->Flags & USF_Placeholder) && V->Class && USGUI::IsGUIClass(V->Class) && TemplateClass(V);
		}

		static FUSObject* OwnerFor(FUSObject* From, FUSClass* OuterClass)
		{
			for (FUSObject* O = From; O && OuterClass; O = O->Outer)
			{
				if (O->Data && O->Class && !(O->Flags & (USF_Placeholder | USF_Field)) && O->Class->IsChildOf(OuterClass))
				{
					return O;
				}
			}
			return nullptr;
		}

		FUSObject* Instance(FUSObject* From, FUSObject* T)
		{
			FUSObject* Owner = OwnerFor(From, TemplateClass(T));
			if (!Owner || Depth > 32)
			{
				return T;
			}
			if (FUSObject** Hit = Made.Find(TPair<FUSObject*, FUSObject*>(Owner, T)))
			{
				return *Hit;
			}
			FUSObject* N = World->ConstructObject(T->Class, Owner, NAME_None);
			if (!N)
			{
				return T;
			}
			Made.Add(TPair<FUSObject*, FUSObject*>(Owner, T), N);
			FString Path = T->GetPathName(), Pkg, Rel;
			if (Path.Split(TEXT("."), &Pkg, &Rel))
			{
				FUSLinker* L = FUSRuntime::Get().LoadPackage(FName(*Pkg));
				const int32 Ei = L ? L->FindExport(Rel, T->Class->GetName()) : INDEX_NONE;
				if (Ei != INDEX_NONE)
				{
					L->LoadInstanceProperties(Ei, N);
				}
			}
			++Depth;
			Fix(N);
			--Depth;
			return N;
		}

		void FixValue(FUSObject* Self, FUSProperty* P, uint8* Addr)
		{
			switch (P->Type)
			{
			case EUSType::Object:
			{
				FUSObject*& V = *reinterpret_cast<FUSObject**>(Addr);
				if (IsTemplate(V))
				{
					V = Instance(Self, V);
				}
				break;
			}
			case EUSType::Delegate:
			{
				FUSDelegate& D = *reinterpret_cast<FUSDelegate*>(Addr);
				if (D.FunctionName == NAME_None)
				{
					D.Object = nullptr;
				}
				else if (!D.Object)
				{
					D.Object = Self;
				}
				else if (IsTemplate(D.Object))
				{
					D.Object = Instance(Self, D.Object);
				}
				else if (D.Object->Class == ClassClass())
				{
					FUSObject* O = OwnerFor(Self, static_cast<FUSClass*>(D.Object));
					D.Object = O ? O : Self;
				}
				break;
			}
			case EUSType::Array:
			{
				FUSScriptArray* A = reinterpret_cast<FUSScriptArray*>(Addr);
				if (P->Inner && (P->Inner->Type == EUSType::Object || P->Inner->Type == EUSType::Delegate))
				{
					for (int32 i = 0; i < A->Num; ++i)
					{
						FixValue(Self, P->Inner, A->Data + int64(i) * P->Inner->ElementSize);
					}
				}
				break;
			}
			default:
				break;
			}
		}

		void Fix(FUSObject* O)
		{
			if (!O || !O->Data || !O->Class)
			{
				return;
			}
			for (FUSProperty* P : O->Class->RefProps)
			{
				for (int32 i = 0; i < P->ArrayDim; ++i)
				{
					FixValue(O, P, O->Data + P->Offset + i * P->ElementSize);
				}
			}
		}
	};

	// ---------------------------------------------------------------------------------------------
	// Natives
	// ---------------------------------------------------------------------------------------------

	FUSObject* PlayerOwnerOf(FUSObject* C)
	{
		FUSObject* Ctl = ControllerOf(C);
		FUSObject* VP = GetO(Ctl, TEXT("ViewportOwner"));
		return GetO(VP, TEXT("Actor"));
	}

	US_NATIVE(PlayerOwner)
	{
		USP_FINISH;
		US_RESULT(FUSObject*) = PlayerOwnerOf(Self);
	}

	US_NATIVE(CompSetTimer)
	{
		USP_GET_FLOAT(Interval);
		USP_GET_UBOOL_OPTX(bRepeat, false);
		USP_FINISH;
		SetF(Self, TEXT("TimerInterval"), Interval);
		SetF(Self, TEXT("TimerCountdown"), Interval);
		SetBool(Self, TEXT("bTimerRepeat"), bRepeat != 0);
		if (GCurrent)
		{
			if (Interval > 0.f)
			{
				GCurrent->Timers.Add(Self, FUSGUI::FTimer{ Interval, Interval, bRepeat != 0 });
			}
			else
			{
				GCurrent->Timers.Remove(Self);
			}
		}
	}

	US_NATIVE(CompKillTimer)
	{
		USP_FINISH;
		SetF(Self, TEXT("TimerInterval"), 0.f);
		SetF(Self, TEXT("TimerCountdown"), 0.f);
		if (GCurrent)
		{
			GCurrent->Timers.Remove(Self);
		}
	}

	/** ActualWidth / ActualHeight / ActualLeft / ActualTop (Val unless omitted: WinWidth ...). */
	template <int32 Which>
	void ActualNative(FUSObject* Self, FUSFrame& Stack, void* Result)
	{
		USP_GET_FLOAT_OPTX(Val, 0.f);
		USP_GET_UBOOL_OPTX(bForce, false);
		USP_FINISH;
		static const TCHAR* Names[4] = { TEXT("WinWidth"), TEXT("WinHeight"), TEXT("WinLeft"), TEXT("WinTop") };
		const float V = (!bForce && Val == 0.f) ? GetF(Self, Names[Which]) : Val;
		float R = 0.f;
		switch (Which)
		{
		case 0: R = ActualDim(Self, V, true); break;
		case 1: R = ActualDim(Self, V, false); break;
		case 2: R = ActualPos(Self, V, true); break;
		default: R = ActualPos(Self, V, false); break;
		}
		US_RESULT(float) = R;
	}

	/** RelativeLeft / RelativeTop / RelativeWidth / RelativeHeight: pixels back to WinLeft ... values. */
	template <int32 Which>
	void RelativeNative(FUSObject* Self, FUSFrame& Stack, void* Result)
	{
		USP_GET_FLOAT_OPTX(Real, 0.f);
		USP_GET_UBOOL_OPTX(bForce, false);
		USP_FINISH;
		const FBox4 B = BoundsOf(Self);
		float V = Real;
		if (!bForce && Real == 0.f)
		{
			V = Which == 0 ? B.L : Which == 1 ? B.T : Which == 2 ? B.W : B.H;
		}
		float R = 0.f;
		switch (Which)
		{
		case 0: R = RelativePos(Self, V, true); break;
		case 1: R = RelativePos(Self, V, false); break;
		case 2: R = RelativeDim(Self, V, true); break;
		default: R = RelativeDim(Self, V, false); break;
		}
		US_RESULT(float) = R;
	}

	/** Sets a component to a pixel rectangle, in the units it already uses (relative values stay relative). */
	void SetPixelRect(FUSObject* C, float X, float Y, float W, float H)
	{
		const bool bRel = IsRelative(C, GetF(C, TEXT("WinWidth"))) || IsRelative(C, GetF(C, TEXT("WinLeft")));
		if (bRel)
		{
			SetF(C, TEXT("WinLeft"), RelativePos(C, X, true));
			SetF(C, TEXT("WinTop"), RelativePos(C, Y, false));
			SetF(C, TEXT("WinWidth"), RelativeDim(C, W, true));
			SetF(C, TEXT("WinHeight"), RelativeDim(C, H, false));
		}
		else
		{
			SetF(C, TEXT("WinLeft"), X);
			SetF(C, TEXT("WinTop"), Y);
			SetF(C, TEXT("WinWidth"), W);
			SetF(C, TEXT("WinHeight"), H);
		}
	}

	/** GUIComponent.AutoPosition: components in a grid of NumberOfColumns inside the bounds (pixels). */
	void AutoPositionIn(const TArray<FUSObject*>& Comps, float L, float T, float R, float B, float LPad, float TPad, float RPad, float BPad, int32 Cols, float ColPad)
	{
		Cols = FMath::Max(1, Cols);
		const int32 N = Comps.Num();
		if (N == 0)
		{
			return;
		}
		const int32 Rows = (N + Cols - 1) / Cols;
		const float AreaW = (R - RPad) - (L + LPad), AreaH = (B - BPad) - (T + TPad);
		const float CellW = (AreaW - ColPad * (Cols - 1)) / Cols;
		const float CellH = AreaH / Rows;
		for (int32 i = 0; i < N; ++i)
		{
			FUSObject* C = Comps[i];
			if (!C)
			{
				continue;
			}
			const int32 Col = i / Rows, Row = i % Rows;
			const float H = FMath::Min(CellH, ActualDim(C, GetF(C, TEXT("WinHeight")), false));
			SetPixelRect(C, L + LPad + Col * (CellW + ColPad), T + TPad + Row * CellH + (CellH - H) * 0.5f, CellW, H);
		}
	}

	TArray<FUSObject*> ReadObjArray(FUSScriptArray* A)
	{
		TArray<FUSObject*> Out;
		for (int32 i = 0; A && i < A->Num; ++i)
		{
			Out.Add(reinterpret_cast<FUSObject* const*>(A->Data)[i]);
		}
		return Out;
	}

	US_NATIVE(AutoPosition)
	{
		USP_GET_ARRAY_REF(Comps, CompsProp);
		USP_GET_FLOAT(LeftBound);
		USP_GET_FLOAT(UpperBound);
		USP_GET_FLOAT(RightBound);
		USP_GET_FLOAT(LowerBound);
		USP_GET_FLOAT(LeftPad);
		USP_GET_FLOAT(UpperPad);
		USP_GET_FLOAT(RightPad);
		USP_GET_FLOAT(LowerPad);
		USP_GET_INT_OPTX(Cols, 1);
		USP_GET_FLOAT_OPTX(ColPad, 0.f);
		USP_FINISH;
		AutoPositionIn(ReadObjArray(Comps), LeftBound, UpperBound, RightBound, LowerBound, LeftPad, UpperPad, RightPad, LowerPad, Cols, ColPad);
		if (Comps == &CompsTemp && CompsTemp.Data)
		{
			FMemory::Free(CompsTemp.Data);
		}
		(void)CompsProp;
	}

	US_NATIVE(AutoPositionOn)
	{
		USP_GET_ARRAY_REF(Comps, CompsProp);
		USP_GET_OBJECT(Frame);
		USP_GET_FLOAT(LPerc);
		USP_GET_FLOAT(TPerc);
		USP_GET_FLOAT(RPerc);
		USP_GET_FLOAT(BPerc);
		USP_GET_INT_OPTX(Cols, 1);
		USP_GET_FLOAT_OPTX(ColPad, 0.f);
		USP_FINISH;
		if (Frame)
		{
			const FBox4 F = ClientOf(Frame);
			AutoPositionIn(ReadObjArray(Comps), F.L, F.T, F.R(), F.B(), LPerc * F.W, TPerc * F.H, RPerc * F.W, BPerc * F.H, Cols, ColPad * F.W);
		}
		if (Comps == &CompsTemp && CompsTemp.Data)
		{
			FMemory::Free(CompsTemp.Data);
		}
		(void)CompsProp;
	}

	US_NATIVE(UpdateOffset)
	{
		USP_GET_FLOAT(PosX);
		USP_GET_FLOAT(PosY);
		USP_GET_FLOAT(PosW);
		USP_GET_FLOAT(PosH);
		USP_FINISH;
		if (float* M = FloatArray(Self, TEXT("MouseOffset"), 4); M && GCurrent)
		{
			M[0] = GCurrent->GetMouseX() - PosX;
			M[1] = GCurrent->GetMouseY() - PosY;
			M[2] = PosW - GCurrent->GetMouseX();
			M[3] = PosH - GCurrent->GetMouseY();
		}
	}

	US_NATIVE(GetMenuPath)
	{
		USP_FINISH;
		FString Path;
		for (FUSObject* C = Self; C; C = GetO(C, TEXT("MenuOwner")))
		{
			Path = Path.IsEmpty() ? C->GetName() : C->GetName() + TEXT(".") + Path;
		}
		US_RESULT(FString) = Path;
	}

	US_NATIVE(SpecialHit)
	{
		USP_GET_UBOOL_OPTX(bForce, false);
		USP_FINISH;
		US_RESULT(uint32) = (GCurrent && BoundsOf(Self).Contains(GCurrent->GetMouseX(), GCurrent->GetMouseY())) ? 1u : 0u;
	}

	/** GUIMultiComponent.InitializeControls: the automated components into Controls (when empty, or bAlwaysAutomate). */
	US_NATIVE(InitializeControls)
	{
		USP_FINISH;
		FObjArray Existing = GetArr(Self, TEXT("Controls"));
		TArray<FUSObject*> Controls;
		if (!GetBool(Self, TEXT("bAlwaysAutomate")))
		{
			for (int32 i = 0; i < Existing.Num(); ++i)
			{
				Controls.Add(Existing[i]);
			}
		}
		if (Controls.Num() == 0)
		{
			TArray<FUSObject*> Auto;
			for (FUSProperty* P : Self->Class->AllProps)
			{
				if (!(P->PropertyFlags & CPF_Automated))
				{
					continue;
				}
				auto Add = [&Auto](FUSObject* C)
				{
					if (Is(C, K().Component) && !(C->Flags & USF_Placeholder) && !Auto.Contains(C))
					{
						Auto.Add(C);
					}
				};
				if (P->Type == EUSType::Object)
				{
					for (int32 i = 0; i < P->ArrayDim; ++i)
					{
						Add(reinterpret_cast<FUSObject* const*>(Self->Data + P->Offset)[i]);
					}
				}
				else if (P->Type == EUSType::Array && P->Inner && P->Inner->Type == EUSType::Object)
				{
					// `var automated array<GUIButton> b_Tabs;`
					const FUSScriptArray& A = *reinterpret_cast<const FUSScriptArray*>(Self->Data + P->Offset);
					for (int32 i = 0; i < A.Num; ++i)
					{
						Add(reinterpret_cast<FUSObject* const*>(A.Data)[i]);
					}
				}
			}
			// rendered (and hit) in RenderWeight order, as AppendComponent keeps Controls
			Auto.StableSort([](const FUSObject& A, const FUSObject& B) { return GetF(&A, TEXT("RenderWeight")) < GetF(&B, TEXT("RenderWeight")); });
			Controls = Auto;
		}
		SetArr(Self, TEXT("Controls"), Controls);
	}

	/** GUIMultiComponent.RemapComponents: Components = the controls with bTabStop, by TabOrder. */
	US_NATIVE(RemapComponents)
	{
		USP_FINISH;
		const FObjArray Controls = GetArr(Self, TEXT("Controls"));
		TArray<FUSObject*> Tab;
		for (int32 i = 0; i < Controls.Num(); ++i)
		{
			if (Controls[i] && GetBool(Controls[i], TEXT("bTabStop")))
			{
				Tab.Add(Controls[i]);
			}
		}
		Tab.StableSort([](const FUSObject& A, const FUSObject& B) { return GetI(&A, TEXT("TabOrder")) < GetI(&B, TEXT("TabOrder")); });
		SetArr(Self, TEXT("Components"), Tab);
	}

	US_NATIVE(FindComponentIndex)
	{
		USP_GET_OBJECT(Who);
		USP_FINISH;
		const FObjArray C = GetArr(Self, TEXT("Components"));
		int32 Found = -1;
		for (int32 i = 0; i < C.Num() && Who; ++i)
		{
			if (C[i] == Who)
			{
				Found = i;
				break;
			}
		}
		US_RESULT(int32) = Found;
	}

	// ---- GUIController ----

	US_NATIVE(GetMenuFont)
	{
		USP_GET_STR(FontName);
		USP_FINISH;
		US_RESULT(FUSObject*) = MenuFont(Self, FontName);
	}

	US_NATIVE(GetStyle)
	{
		USP_GET_STR(StyleName);
		USP_GET_BYTE_REF(FontScale);
		USP_FINISH;
		FUSObject* Found = nullptr;
		const FObjArray Styles = GetArr(Self, TEXT("StyleStack"));
		for (int32 i = 0; i < Styles.Num() && !StyleName.IsEmpty() && !Found; ++i)
		{
			FUSObject* S = Styles[i];
			if (!S)
			{
				continue;
			}
			if (GetS(S, TEXT("KeyName")).Equals(StyleName, ESearchCase::IgnoreCase))
			{
				Found = S;
				break;
			}
			// AlternateKeyName[0] is the smaller font of the style, [1] the larger one (UT2003 style names)
			if (FUSProperty* P = Prop(S, TEXT("AlternateKeyName"), EUSType::Str); P && P->ArrayDim >= 2)
			{
				const FString* Alt = reinterpret_cast<const FString*>(S->Data + P->Offset);
				for (int32 a = 0; a < 2; ++a)
				{
					if (!Alt[a].IsEmpty() && Alt[a].Equals(StyleName, ESearchCase::IgnoreCase))
					{
						Found = S;
						*FontScale = a == 0 ? 0 : 2;
						break;
					}
				}
			}
		}
		US_RESULT(FUSObject*) = Found;
	}

	US_NATIVE(GetCurrentRes)
	{
		USP_FINISH;
		US_RESULT(FString) = FString::Printf(TEXT("%dx%d"), int32(ScreenW()), int32(ScreenH()));
	}

	US_NATIVE(EmptyStr) { USSkipNativeParms(Stack); if (Result) { US_RESULT(FString) = FString(); } }
	US_NATIVE(ReturnFalse) { USSkipNativeParms(Stack); if (Result) { US_RESULT(uint32) = 0u; } }
	US_NATIVE(Quiet) { USSkipNativeParms(Stack); }

	US_NATIVE(SetMoveControl)
	{
		USP_GET_OBJECT(C);
		USP_FINISH;
		SetO(Self, TEXT("MoveControl"), C);
	}

	// ---- GUIStyles ----

	US_NATIVE(StylesDraw)
	{
		USP_GET_OBJECT(Canvas);
		USP_GET_BYTE(State);
		USP_GET_FLOAT(L);
		USP_GET_FLOAT(T);
		USP_GET_FLOAT(W);
		USP_GET_FLOAT(H);
		USP_FINISH;
		(void)Canvas;
		StyleDraw(Self, State, FBox4{ L, T, W, H });
	}

	US_NATIVE(StylesDrawText)
	{
		USP_GET_OBJECT(Canvas);
		USP_GET_BYTE(State);
		USP_GET_FLOAT(L);
		USP_GET_FLOAT(T);
		USP_GET_FLOAT(W);
		USP_GET_FLOAT(H);
		USP_GET_BYTE(Align);
		USP_GET_STR(Text);
		USP_GET_BYTE(FontScale);
		USP_FINISH;
		(void)Canvas;
		StyleText(Self, State, FBox4{ L, T, W, H }, Align, Text, FontScale);
	}

	US_NATIVE(StylesTextSize)
	{
		USP_GET_OBJECT(Canvas);
		USP_GET_BYTE(State);
		USP_GET_STR(Text);
		USP_GET_FLOAT_REF(XL);
		USP_GET_FLOAT_REF(YL);
		USP_GET_BYTE(FontScale);
		USP_FINISH;
		(void)Canvas;
		StyleTextSize(Self, State, Text, FontScale, *XL, *YL);
	}

	// ---- lists ----

	/** GUIList.SortList: Elements by CompareItem (bound) or by Item. */
	US_NATIVE(ListSortList)
	{
		USP_FINISH;
		FUSProperty* P = Prop(Self, TEXT("Elements"), EUSType::Array);
		if (!P || !P->Inner || P->Inner->Type != EUSType::Struct || !P->Inner->Struct)
		{
			return;
		}
		FUSScriptArray& A = *reinterpret_cast<FUSScriptArray*>(Self->Data + P->Offset);
		FUSProperty* Item = P->Inner->Struct->FindPropertyByName(TEXT("Item"));
		const int32 ES = P->Inner->ElementSize;
		if (A.Num < 2 || !Item)
		{
			return;
		}
		TArray<int32> Order;
		for (int32 i = 0; i < A.Num; ++i)
		{
			Order.Add(i);
		}
		const FScriptCall Cmp = FindDelegate(Self, TEXT("CompareItem"), false);
		Order.StableSort([&](int32 X, int32 Y)
		{
			if (Cmp.Function)
			{
				FUSParms Parms(Cmp.Function);
				P->Inner->CopyComplete(Parms.ParmAddr(0), A.Data + int64(X) * ES);
				P->Inner->CopyComplete(Parms.ParmAddr(1), A.Data + int64(Y) * ES);
				Parms.Call(Cmp.Target);
				return Parms.ReturnInt() < 0;
			}
			const FString& SX = *reinterpret_cast<const FString*>(A.Data + int64(X) * ES + Item->Offset);
			const FString& SY = *reinterpret_cast<const FString*>(A.Data + int64(Y) * ES + Item->Offset);
			return SX.Compare(SY, ESearchCase::IgnoreCase) < 0;
		});
		TArray<uint8> Copy;
		Copy.SetNumUninitialized(A.Num * ES);
		FMemory::Memcpy(Copy.GetData(), A.Data, int64(A.Num) * ES);
		for (int32 i = 0; i < A.Num; ++i)
		{
			FMemory::Memcpy(A.Data + int64(i) * ES, Copy.GetData() + int64(Order[i]) * ES, ES);
		}
	}

	/** GUIMultiColumnList: SortData (GetSortString of each item, by SortColumn / SortDescending) and InvSortData. */
	void MCSort(FUSObject* L)
	{
		FUSProperty* SD = Prop(L, TEXT("SortData"), EUSType::Array);
		FUSProperty* Inv = Prop(L, TEXT("InvSortData"), EUSType::Array);
		if (!SD || !SD->Inner || !SD->Inner->Struct || !Inv || !Inv->Inner)
		{
			return;
		}
		FUSProperty* SS = SD->Inner->Struct->FindPropertyByName(TEXT("SortString"));
		FUSProperty* SI = SD->Inner->Struct->FindPropertyByName(TEXT("SortItem"));
		if (!SS || !SI)
		{
			return;
		}
		const int32 Count = FMath::Max(0, GetI(L, TEXT("ItemCount")));
		TArray<TPair<FString, int32>> Items;
		const FScriptCall Get = FindDelegate(L, TEXT("GetSortString"), false);
		for (int32 i = 0; i < Count; ++i)
		{
			FString S;
			if (Get.Function && GetI(L, TEXT("SortColumn")) >= 0)
			{
				FUSParms Parms(Get.Function);
				Parms.Int(i);
				Parms.Call(Get.Target);
				S = Parms.ReturnStr();
			}
			Items.Add(TPair<FString, int32>(S, i));
		}
		if (GetI(L, TEXT("SortColumn")) >= 0)
		{
			const bool bDesc = GetBool(L, TEXT("SortDescending"));
			Items.StableSort([bDesc](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
			{
				const int32 C = A.Key.Compare(B.Key, ESearchCase::IgnoreCase);
				return bDesc ? C > 0 : C < 0;
			});
		}
		FUSScriptArray& A = *reinterpret_cast<FUSScriptArray*>(L->Data + SD->Offset);
		SD->DestroySingle(&A);
		FMemory::Memzero(&A, sizeof(A));
		const int32 ES = SD->Inner->ElementSize;
		if (Count > 0)
		{
			A.Data = static_cast<uint8*>(FMemory::Malloc(int64(Count) * ES, 16));
			FMemory::Memzero(A.Data, int64(Count) * ES);
			A.Num = A.Max = Count;
		}
		FUSScriptArray& IA = *reinterpret_cast<FUSScriptArray*>(L->Data + Inv->Offset);
		Inv->DestroySingle(&IA);
		FMemory::Memzero(&IA, sizeof(IA));
		if (Count > 0)
		{
			IA.Data = static_cast<uint8*>(FMemory::Malloc(int64(Count) * sizeof(int32), 16));
			IA.Num = IA.Max = Count;
		}
		for (int32 i = 0; i < Count; ++i)
		{
			uint8* E = A.Data + int64(i) * ES;
			*reinterpret_cast<FString*>(E + SS->Offset) = Items[i].Key;
			*reinterpret_cast<int32*>(E + SI->Offset) = Items[i].Value;
			reinterpret_cast<int32*>(IA.Data)[Items[i].Value] = i;
		}
		SetBool(L, TEXT("NeedsSorting"), false);
	}

	int32 MCSortItem(FUSObject* L, int32 Row)
	{
		FUSProperty* SD = Prop(L, TEXT("SortData"), EUSType::Array);
		if (!SD || !SD->Inner || !SD->Inner->Struct)
		{
			return Row;
		}
		const FUSScriptArray& A = *reinterpret_cast<const FUSScriptArray*>(L->Data + SD->Offset);
		FUSProperty* SI = SD->Inner->Struct->FindPropertyByName(TEXT("SortItem"));
		return SI && Row >= 0 && Row < A.Num ? *reinterpret_cast<const int32*>(A.Data + int64(Row) * SD->Inner->ElementSize + SI->Offset) : Row;
	}

	US_NATIVE(MCSortList) { USP_FINISH; MCSort(Self); }
	US_NATIVE(MCChangeSortOrder) { USP_FINISH; SetBool(Self, TEXT("SortDescending"), !GetBool(Self, TEXT("SortDescending"))); MCSort(Self); }
	US_NATIVE(MCMarkDirty) { USSkipNativeParms(Stack); SetBool(Self, TEXT("NeedsSorting"), true); }
	US_NATIVE(MCGetListIndex)
	{
		USP_GET_INT(ArrayIndex);
		USP_FINISH;
		FUSProperty* Inv = Prop(Self, TEXT("InvSortData"), EUSType::Array);
		const FUSScriptArray* A = Inv ? reinterpret_cast<const FUSScriptArray*>(Self->Data + Inv->Offset) : nullptr;
		US_RESULT(int32) = A && ArrayIndex >= 0 && ArrayIndex < A->Num ? reinterpret_cast<const int32*>(A->Data)[ArrayIndex] : ArrayIndex;
	}

	US_NATIVE(ToolTipSetTip)
	{
		USP_GET_STR(Tip);
		USP_FINISH;
		SetS(Self, TEXT("Text"), Tip);
	}

	US_NATIVE(GetMainMenuClass)
	{
		USP_FINISH;
		// the mod menus run in a game: closing the last one never opens UT2004's main menu
		US_RESULT(FString) = FString();
	}
}

// =================================================================================================
// The runner
// =================================================================================================

struct FUSGUI::FImpl
{
	FUSGUI& G;
	FUSObject* Hover = nullptr;
	FUSObject* Pressed = nullptr;
	bool bPressedOver = false;
	FUSObject* LastClicked = nullptr;
	double LastClickTime = -10.0;
	double Now = 0.0;
	double NextRepeat = 0.0;
	/** Combo box lists that are open: drawn over the page, hit first. */
	TArray<FUSObject*> Popups;
	TArray<FUSObject*> PrevPopups;
	int32 Frames = 0;
	int32 Opened = 0;
	FString LastTop;

	explicit FImpl(FUSGUI& InG) : G(InG) {}

	FUSObject* Ctl() const { return G.Controller; }

	FObjArray Stack() const { return GetArr(Ctl(), TEXT("MenuStack")); }

	FUSObject* TopPage() const
	{
		const FObjArray S = Stack();
		return S.Num() > 0 ? S[S.Num() - 1] : nullptr;
	}

	// ---- layout (UE2's native PreDraw of each widget class) --------------------------------------

	void LayoutMenuOption(FUSObject* C, const FBox4& B)
	{
		FUSObject* Label = GetO(C, TEXT("MyLabel"));
		FUSObject* Comp = GetO(C, TEXT("MyComponent"));
		if (GetBool(C, TEXT("bVerticalLayout")))
		{
			Place(Label, B.L, B.T, B.W, B.H * 0.5f);
			Place(Comp, B.L, B.T + B.H * 0.5f, B.W, B.H * 0.5f);
			return;
		}
		float CapW = GetF(C, TEXT("CaptionWidth"), 0.5f);
		CapW = CapW <= 1.f ? CapW * B.W : CapW;
		float CompW = GetF(C, TEXT("ComponentWidth"), -1.f);
		CompW = CompW < 0.f ? B.W - CapW : (CompW <= 1.f ? CompW * B.W : CompW);
		if (GetBool(C, TEXT("bSquare")))
		{
			CompW = B.H;
		}
		CompW = FMath::Clamp(CompW, 0.f, B.W);
		const bool bFlipped = GetBool(C, TEXT("bFlipped"));
		const uint8 Just = GetB(C, TEXT("ComponentJustification"), TXTA_Right);
		// the region the control sits in (the rest of the width), justified in it
		const float RegionL = bFlipped ? B.L : B.L + CapW;
		const float RegionW = FMath::Max(CompW, B.W - CapW);
		float CompL = RegionL;
		if (Just == TXTA_Right)
		{
			CompL = RegionL + RegionW - CompW;
		}
		else if (Just == TXTA_Center)
		{
			CompL = RegionL + (RegionW - CompW) * 0.5f;
		}
		Place(Label, bFlipped ? B.R() - CapW : B.L, B.T, CapW, B.H);
		Place(Comp, CompL, B.T, CompW, B.H);
	}

	void LayoutCombo(FUSObject* C, const FBox4& B)
	{
		FUSObject* Edit = GetO(C, TEXT("Edit"));
		FUSObject* Btn = GetO(C, TEXT("MyShowListBtn"));
		FUSObject* LB = GetO(C, TEXT("MyListBox"));
		const float BW = FMath::Min(B.H, B.W * 0.5f);
		Place(Edit, B.L, B.T, B.W - BW, B.H);
		Place(Btn, B.R() - BW, B.T, BW, B.H);
		if (LB)
		{
			FUSObject* L = GetO(LB, TEXT("MyList"));
			const int32 Rows = FMath::Clamp(GetI(L, TEXT("ItemCount")), 1, FMath::Max(1, GetI(C, TEXT("MaxVisibleItems"), 8)));
			const float IH = ItemHeightOf(L);
			float H = Rows * IH + 4.f;
			// above the box when it does not fit below
			const float Top = B.B() + H > ScreenH() && B.T - H >= 0.f ? B.T - H : B.B();
			Place(LB, B.L, Top, B.W, H);
		}
	}

	void LayoutListBox(FUSObject* C, const FBox4& B)
	{
		FUSObject* L = GetO(C, TEXT("MyList"));
		FUSObject* SB = GetO(C, TEXT("MyScrollBar"));
		FUSObject* Header = Is(C, K().MCListBox) && GetBool(C, TEXT("bDisplayHeader")) ? GetO(C, TEXT("Header")) : nullptr;
		const float IH = ItemHeightOf(L);
		const float HH = Header ? IH + 4.f : 0.f;
		const int32 IPP = FMath::Max(1, int32((B.H - HH) / FMath::Max(1.f, IH)));
		const bool bNeed = L && GetI(L, TEXT("ItemCount")) > IPP;
		const float SBW = bNeed ? FMath::Max(12.f, FMath::RoundToFloat(ScreenH() * 0.022f)) : 0.f;
		if (Header)
		{
			Place(Header, B.L, B.T, B.W - SBW, HH);
		}
		Place(L, B.L, B.T + HH, B.W - SBW, B.H - HH);
		if (SB)
		{
			SetBool(SB, TEXT("bVisible"), bNeed);
			Place(SB, B.R() - SBW, B.T + HH, SBW, B.H - HH);
		}
	}

	void LayoutScrollBar(FUSObject* C, const FBox4& B)
	{
		FUSObject* Dec = GetO(C, TEXT("MyDecreaseButton"));
		FUSObject* Inc = GetO(C, TEXT("MyIncreaseButton"));
		FUSObject* Zone = GetO(C, TEXT("MyScrollZone"));
		FUSObject* Grip = GetO(C, TEXT("MyGripButton"));
		FUSObject* L = GetO(C, TEXT("MyList"));
		if (L)
		{
			SetI(C, TEXT("ItemCount"), GetI(L, TEXT("ItemCount")));
			SetI(C, TEXT("ItemsPerPage"), GetI(L, TEXT("ItemsPerPage")));
			SetI(C, TEXT("BigStep"), GetI(L, TEXT("ItemsPerPage")) * FMath::Max(1, GetI(C, TEXT("Step"), 1)));
		}
		const int32 Count = GetI(C, TEXT("ItemCount")), IPP = GetI(C, TEXT("ItemsPerPage"));
		const bool bHorz = GetB(C, TEXT("Orientation")) == ORIENT_Horizontal;
		const float S = bHorz ? B.H : B.W;
		const float Len = bHorz ? B.W : B.H;
		const float ZoneLen = FMath::Max(0.f, Len - S * 2.f);
		const float Frac = Count > 0 ? FMath::Clamp(float(IPP) / float(Count), 0.f, 1.f) : 1.f;
		const float GripLen = FMath::Min(ZoneLen, FMath::Max(float(GetI(C, TEXT("MinGripPixels"), 12)), ZoneLen * Frac));
		SetF(C, TEXT("GripSize"), GripLen);
		const float Pos = FMath::Clamp(GetF(C, TEXT("GripPos")), 0.f, 1.f);
		if (bHorz)
		{
			Place(Dec, B.L, B.T, S, S);
			Place(Inc, B.R() - S, B.T, S, S);
			Place(Zone, B.L + S, B.T, ZoneLen, S);
			Place(Grip, B.L + S + (ZoneLen - GripLen) * Pos, B.T, GripLen, S);
		}
		else
		{
			Place(Dec, B.L, B.T, S, S);
			Place(Inc, B.L, B.B() - S, S, S);
			Place(Zone, B.L, B.T + S, S, ZoneLen);
			Place(Grip, B.L, B.T + S + (ZoneLen - GripLen) * Pos, S, GripLen);
		}
	}

	void LayoutNumeric(FUSObject* C, const FBox4& B)
	{
		const float SW = FMath::Min(B.W * 0.4f, FMath::RoundToFloat(B.H * 0.75f));
		Place(GetO(C, TEXT("MyEditBox")), B.L, B.T, B.W - SW, B.H);
		Place(GetO(C, TEXT("MySpinner")), B.R() - SW, B.T, SW, B.H);
	}

	float TabRows = 0.f;

	void LayoutTabs(FUSObject* C, const FBox4& B)
	{
		const FObjArray Tabs = GetArr(C, TEXT("TabStack"));
		float TabH = GetF(C, TEXT("TabHeight"), 0.08f);
		TabH = TabH < 2.f ? TabH * ScreenH() : TabH;
		TabH = FMath::Min(TabH, FMath::Max(B.H, 1.f));
		// rows of buttons as wide as their captions; bFillSpace stretches each row to the width
		struct FCell { FUSObject* Btn; float W; };
		TArray<TArray<FCell>> Rows;
		Rows.AddDefaulted();
		float X = 0.f;
		for (int32 i = 0; i < Tabs.Num(); ++i)
		{
			FUSObject* Btn = Tabs[i];
			if (!Btn || !GetBool(Btn, TEXT("bVisible")))
			{
				continue;
			}
			float XL = 0.f, YL = 0.f;
			StyleTextSize(GetO(Btn, TEXT("Style")), MSAT_Blurry, GetS(Btn, TEXT("Caption")), GetB(Btn, TEXT("FontScale"), 1), XL, YL);
			const float W = XL + TabH * 1.2f;
			if (X > 0.f && X + W > B.W)
			{
				Rows.AddDefaulted();
				X = 0.f;
			}
			Rows.Last().Add(FCell{ Btn, W });
			X += W;
		}
		const bool bFill = GetBool(C, TEXT("bFillSpace"));
		for (int32 r = 0; r < Rows.Num(); ++r)
		{
			float RowW = 0.f;
			for (const FCell& Cell : Rows[r])
			{
				RowW += Cell.W;
			}
			const float Extra = bFill && Rows[r].Num() ? (B.W - RowW) / Rows[r].Num() : 0.f;
			float CX = B.L;
			for (const FCell& Cell : Rows[r])
			{
				const float W = Cell.W + Extra;
				Place(Cell.Btn, CX, B.T + r * TabH, W, TabH);
				UpdateBounds(Cell.Btn);
				CX += W;
			}
		}
		const int32 NumRows = Rows.Last().Num() ? Rows.Num() : Rows.Num() - 1;
		TabRows = float(NumRows);
		if (GetBool(C, TEXT("bDockPanels")))
		{
			const float Top = B.T + NumRows * TabH;
			for (int32 i = 0; i < Tabs.Num(); ++i)
			{
				FUSObject* Panel = GetO(Tabs[i], TEXT("MyPanel"));
				if (!Panel)
				{
					continue;
				}
				const float H = GetBool(Panel, TEXT("bFillHeight")) ? ScreenH() - Top : ActualDim(Panel, GetF(Panel, TEXT("WinHeight")), false);
				Place(Panel, B.L, Top, B.W, H);
			}
		}
		// GUITabControl fades a new tab in over FadeInTime (FadeSound); here it shows at once
		if (FUSObject* Pending = GetO(C, TEXT("PendingTab")))
		{
			CallEvent(C, TEXT("MakeTabActive"), [Pending](FUSParms& P) { P.Object(Pending); });
		}
	}

	void LayoutList(FUSObject* L)
	{
		const FBox4 CB = ClientOf(L);
		const float IH = ItemHeightOf(L);
		SetF(L, TEXT("ItemHeight"), IH);
		SetF(L, TEXT("ItemWidth"), CB.W);
		SetI(L, TEXT("ItemsPerPage"), FMath::Max(0, int32(CB.H / FMath::Max(1.f, IH))));
		if (Is(L, K().MCList))
		{
			FUSProperty* SD = Prop(L, TEXT("SortData"), EUSType::Array);
			const int32 Have = SD ? reinterpret_cast<const FUSScriptArray*>(L->Data + SD->Offset)->Num : 0;
			if (GetBool(L, TEXT("NeedsSorting")) || Have != GetI(L, TEXT("ItemCount")))
			{
				MCSort(L);
			}
		}
		if (Is(L, K().ScrollText))
		{
			LayoutScrollText(L, CB);
		}
	}

	/** GUIScrollText: Content (and NewText added at the end) wrapped into StringElements at the list's width. */
	void LayoutScrollText(FUSObject* L, const FBox4& CB)
	{
		FString NewText = GetS(L, TEXT("NewText"));
		if (!NewText.IsEmpty())
		{
			FString Content = GetS(L, TEXT("Content"));
			Content = Content.IsEmpty() ? NewText : Content + GetS(L, TEXT("Separator")) + NewText;
			SetS(L, TEXT("Content"), Content);
			SetS(L, TEXT("NewText"), FString());
			SetBool(L, TEXT("bNewContent"), true);
		}
		if (!GetBool(L, TEXT("bNewContent")) && FMath::IsNearlyEqual(GetF(L, TEXT("oldWidth")), CB.W))
		{
			return;
		}
		SetF(L, TEXT("oldWidth"), CB.W);
		SetBool(L, TEXT("bNewContent"), false);
		FUSObject* Font = StyleFont(GetO(L, TEXT("Style")), MSAT_Blurry, GetB(L, TEXT("FontScale")));
		FString Content = GetS(L, TEXT("Content"));
		const FString Sep = GetS(L, TEXT("Separator"));
		if (!Sep.IsEmpty())
		{
			Content = Content.Replace(*Sep, TEXT("\n"));
		}
		TArray<FString> Lines;
		WrapText(Font, Content, FMath::Max(10.f, CB.W), Lines);
		FUSProperty* P = Prop(L, TEXT("StringElements"), EUSType::Array);
		if (P && P->Inner && P->Inner->Type == EUSType::Str)
		{
			FUSScriptArray& A = *reinterpret_cast<FUSScriptArray*>(L->Data + P->Offset);
			P->DestroySingle(&A);
			FMemory::Memzero(&A, sizeof(A));
			if (Lines.Num())
			{
				A.Data = static_cast<uint8*>(FMemory::Malloc(int64(Lines.Num()) * P->Inner->ElementSize, 16));
				FMemory::Memzero(A.Data, int64(Lines.Num()) * P->Inner->ElementSize);
				A.Num = A.Max = Lines.Num();
				for (int32 i = 0; i < Lines.Num(); ++i)
				{
					*reinterpret_cast<FString*>(A.Data + int64(i) * P->Inner->ElementSize) = Lines[i];
				}
			}
		}
		SetI(L, TEXT("ItemCount"), Lines.Num());
		if (GetBool(L, TEXT("bNoTeletype")))
		{
			SetI(L, TEXT("VisibleLines"), Lines.Num());
		}
	}

	void PreDraw(FUSObject* C, int32 Depth = 0)
	{
		if (!C || !GetBool(C, TEXT("bVisible")) || Depth > 40)
		{
			return;
		}
		UpdateBounds(C);
		if (IsBound(C, TEXT("OnPreDraw")))
		{
			ResetCanvas();
			if (CallDelegate(C, TEXT("OnPreDraw"), [this](FUSParms& P) { P.Object(G.CanvasObj); }, false))
			{
				UpdateBounds(C);
			}
		}
		const FBox4 B = BoundsOf(C);
		const FGUIClasses& KC = K();
		if (Is(C, KC.MenuOption))
		{
			LayoutMenuOption(C, B);
		}
		else if (Is(C, KC.Combo))
		{
			LayoutCombo(C, B);
		}
		else if (Is(C, KC.ListBoxBase))
		{
			LayoutListBox(C, B);
		}
		else if (Is(C, KC.ScrollBar))
		{
			LayoutScrollBar(C, B);
		}
		else if (Is(C, KC.Numeric))
		{
			LayoutNumeric(C, B);
		}
		else if (Is(C, KC.TabControl))
		{
			LayoutTabs(C, B);
		}
		else if (Is(C, KC.ListBase))
		{
			LayoutList(C);
		}
		else if (Is(C, KC.Slider) && GetF(C, TEXT("MarkerWidth")) <= 0.f)
		{
			// the knob's width (script's mouse maths uses it)
			SetF(C, TEXT("MarkerWidth"), FMath::Max(6.f, FMath::RoundToFloat(B.H * 0.6f)));
		}
		const FObjArray Controls = GetArr(C, TEXT("Controls"));
		for (int32 i = 0; i < Controls.Num(); ++i)
		{
			PreDraw(Controls[i], Depth + 1);
		}
	}

	// ---- drawing (UE2's native Draw of each widget class) -----------------------------------------

	void ResetCanvas()
	{
		FUSObject* C = G.CanvasObj;
		SetF(C, TEXT("OrgX"), 0.f);
		SetF(C, TEXT("OrgY"), 0.f);
		SetF(C, TEXT("ClipX"), ScreenW());
		SetF(C, TEXT("ClipY"), ScreenH());
		SetB(C, TEXT("Style"), 1);
		if (FUSColor* D = Ptr<FUSColor>(C, TEXT("DrawColor"), EUSType::Struct))
		{
			*D = FUSColor{ 255, 255, 255, 255 };
		}
	}

	uint8 State(FUSObject* C) const
	{
		return FMath::Min<uint8>(GetB(C, TEXT("MenuState")), 4);
	}

	void DrawLabel(FUSObject* C)
	{
		const FBox4 B = BoundsOf(C);
		if (!GetBool(C, TEXT("bTransparent")))
		{
			const FUSColor BC = GetC(C, TEXT("BackColor"));
			Fill(B, FColor(BC.R, BC.G, BC.B, BC.A));
		}
		const FString Caption = GetS(C, TEXT("Caption"));
		if (Caption.IsEmpty())
		{
			return;
		}
		const uint8 Align = GetB(C, TEXT("TextAlign"));
		FUSObject* S = GetO(C, TEXT("Style"));
		if (S && !GetBool(C, TEXT("bMultiLine")))
		{
			StyleText(S, State(C), ClientOf(C), Align, Caption, GetB(C, TEXT("FontScale"), 1));
			return;
		}
		FUSObject* Font = nullptr;
		FUSColor TC = GetC(C, State(C) == MSAT_Focused ? TEXT("FocusedTextColor") : TEXT("TextColor"));
		if (S)
		{
			Font = StyleFont(S, State(C), GetB(C, TEXT("FontScale"), 1));
			TC = StateColor(S, TEXT("FontColors"), State(C));
		}
		else
		{
			Font = ResolveGUIFont(MenuFont(ControllerOf(C), GetS(C, TEXT("TextFont"))), ScreenW());
		}
		const uint8 TStyle = RenderStyle(GetB(C, TEXT("TextStyle"), MSTY_Normal));
		const FColor Color(TC.R, TC.G, TC.B, TC.A);
		const float SX = GetF(C, TEXT("ShadowOffsetX")), SY = GetF(C, TEXT("ShadowOffsetY"));
		const FUSColor ShC = GetC(C, TEXT("ShadowColor"));
		auto Line = [&](const FString& Text, const FBox4& R)
		{
			if (SX != 0.f || SY != 0.f)
			{
				TextInBox(Font, Text, FBox4{ R.L + SX, R.T + SY, R.W, R.H }, Align, FColor(ShC.R, ShC.G, ShC.B, ShC.A), TStyle);
			}
			TextInBox(Font, Text, R, Align, Color, TStyle);
		};
		if (!GetBool(C, TEXT("bMultiLine")))
		{
			Line(Caption, B);
			return;
		}
		TArray<FString> Lines;
		WrapText(Font, Caption, B.W, Lines);
		float XL = 0.f, YL = 0.f;
		TextSize(Font, TEXT("Wy"), XL, YL);
		const float Total = YL * Lines.Num();
		const uint8 VA = GetB(C, TEXT("VertAlign"));
		float Y = VA == TXTA_Center ? B.T + (B.H - Total) * 0.5f : (VA == TXTA_Right ? B.B() - Total : B.T);
		for (const FString& L : Lines)
		{
			Line(L, FBox4{ B.L, Y, B.W, YL });
			Y += YL;
		}
	}

	void DrawImage(FUSObject* C)
	{
		const FBox4 B = BoundsOf(C);
		FUSObject* Img = GetO(C, TEXT("Image"));
		const int32 Sub[4] = { GetI(C, TEXT("X1"), -1), GetI(C, TEXT("Y1"), -1), GetI(C, TEXT("X2"), -1), GetI(C, TEXT("Y2"), -1) };
		const uint8 IS = GetB(C, TEXT("ImageStyle"));
		const uint8 RS = RenderStyle(GetB(C, TEXT("ImageRenderStyle"), MSTY_Alpha));
		if (FUSObject* Shadow = GetO(C, TEXT("DropShadow")))
		{
			const int32 DX = GetI(C, TEXT("DropShadowX")), DY = GetI(C, TEXT("DropShadowY"));
			if (DX || DY)
			{
				DrawMaterial(Shadow, FBox4{ B.L + DX, B.T + DY, B.W, B.H }, ISTY_Stretched, FColor::White, MSTY_Alpha);
			}
		}
		const FUSColor IC = GetC(C, TEXT("ImageColor"));
		DrawMaterial(Img, B, IS, FColor(IC.R, IC.G, IC.B, IC.A), RS, GetB(C, TEXT("ImageAlign")), Sub);
	}

	void DrawSection(FUSObject* C)
	{
		const FBox4 B = BoundsOf(C);
		FUSObject* Base = GetO(C, TEXT("HeaderBase"));
		DrawMaterial(Base ? Base : GetO(C, TEXT("Image")), B, ISTY_Stretched, FColor::White, MSTY_Alpha);
		const FString Caption = GetS(C, TEXT("Caption"));
		FUSObject* CS = GetO(C, TEXT("CaptionStyle"));
		if (!Caption.IsEmpty() && CS)
		{
			const float* IO = FloatArray(C, TEXT("ImageOffset"), 4);
			const float HeadH = FMath::Max(16.f, IO ? IO[1] * 0.75f : 30.f);
			StyleText(CS, MSAT_Blurry, FBox4{ B.L + 8.f, B.T + 2.f, B.W - 16.f, HeadH }, GetBool(C, TEXT("bAltCaption")) ? GetB(C, TEXT("AltCaptionAlign")) : TXTA_Center, Caption,
				GetB(C, TEXT("FontScale"), 1));
		}
	}

	void DrawButton(FUSObject* C)
	{
		FUSObject* S = GetO(C, TEXT("Style"));
		uint8 St = State(C);
		if (Is(C, K().TabButton) && GetBool(C, TEXT("bActive")) && St != MSAT_Disabled)
		{
			St = St == MSAT_Watched ? MSAT_Watched : MSAT_Focused;
		}
		StyleDraw(S, St, BoundsOf(C));
		const FString Caption = GetS(C, TEXT("Caption"));
		if (!Caption.IsEmpty())
		{
			StyleText(S, St, ClientOf(C), GetB(C, TEXT("CaptionAlign"), TXTA_Center), Caption, GetB(C, TEXT("FontScale"), 1));
		}
	}

	void DrawGFXButton(FUSObject* C)
	{
		FUSObject* S = GetO(C, TEXT("Style"));
		const uint8 St = State(C);
		const FBox4 B = BoundsOf(C);
		StyleDraw(S, St, B);
		const FBox4 R = GetBool(C, TEXT("bClientBound")) ? ClientOf(C) : B;
		FUSObject* Graphic = GetO(C, TEXT("Graphic"));
		if (Is(C, K().CheckBox))
		{
			FUSProperty* P = Prop(C, TEXT("CheckedOverlay"), EUSType::Object);
			const bool bChecked = GetBool(C, TEXT("bChecked"));
			const int32 i = bChecked ? St : (GetBool(C, TEXT("bAllOverlay")) ? 5 + St : -1);
			Graphic = P && i >= 0 && i < P->ArrayDim ? reinterpret_cast<FUSObject* const*>(C->Data + P->Offset)[i] : nullptr;
			if (!Graphic && bChecked)
			{
				Graphic = GetO(C, TEXT("Graphic"));
			}
			if (!bChecked && !GetBool(C, TEXT("bAllOverlay")))
			{
				Graphic = nullptr;
			}
		}
		if (!Graphic)
		{
			return;
		}
		const FUSColor IC = S ? StateColor(S, TEXT("ImgColors"), St) : FUSColor{ 255, 255, 255, 255 };
		const FColor Color(IC.R, IC.G, IC.B, IC.A);
		float MW = 0.f, MH = 0.f;
		MatSize(Graphic, MW, MH);
		switch (GetB(C, TEXT("Position")))
		{
		case ICP_Normal:
			DrawMaterial(Graphic, R, ISTY_Normal, Color, MSTY_Alpha);
			break;
		case ICP_Center:
		{
			const float W = FMath::Min(MW, R.W), H = FMath::Min(MH, R.H);
			DrawMaterial(Graphic, FBox4{ R.L + (R.W - W) * 0.5f, R.T + (R.H - H) * 0.5f, W, H }, ISTY_Scaled, Color, MSTY_Alpha);
			break;
		}
		case ICP_Stretched:
			DrawMaterial(Graphic, R, ISTY_Stretched, Color, MSTY_Alpha);
			break;
		default:
			DrawMaterial(Graphic, R, ISTY_Scaled, Color, MSTY_Alpha);
			break;
		}
	}

	void DrawEditBox(FUSObject* C)
	{
		FUSObject* S = GetO(C, TEXT("Style"));
		const uint8 St = State(C);
		StyleDraw(S, St, BoundsOf(C));
		FBox4 R = ClientOf(C);
		if (const int32* BO = Ptr<int32>(C, TEXT("BorderOffsets"), EUSType::Int))
		{
			R = FBox4{ R.L + BO[0], R.T + BO[1], FMath::Max(0.f, R.W - BO[0] - BO[2]), FMath::Max(0.f, R.H - BO[1] - BO[3]) };
		}
		FString Text = GetS(C, TEXT("TextStr"));
		if (GetBool(C, TEXT("bMaskText")))
		{
			Text = FString::ChrN(Text.Len(), TEXT('*'));
		}
		FUSObject* Font = StyleFont(S, St, GetB(C, TEXT("FontScale"), 1));
		const FUSColor FC = S ? StateColor(S, TEXT("FontColors"), St) : FUSColor{ 255, 255, 255, 255 };
		const bool bFocus = GetBool(C, TEXT("bHasFocus")) && !GetBool(C, TEXT("bReadOnly"));
		const int32 Caret = FMath::Clamp(GetI(C, TEXT("CaretPos")), 0, Text.Len());
		// the visible part: from FirstVis, moved so the caret stays inside
		// (not being edited: from the start)
		int32 First = bFocus ? FMath::Clamp(GetI(C, TEXT("FirstVis")), 0, Text.Len()) : 0;
		float XL = 0.f, YL = 0.f;
		if (Caret < First)
		{
			First = Caret;
		}
		TextSize(Font, Text.Mid(First, Caret - First), XL, YL);
		while (bFocus && First < Caret && XL > R.W - 4.f)
		{
			++First;
			TextSize(Font, Text.Mid(First, Caret - First), XL, YL);
		}
		SetI(C, TEXT("FirstVis"), First);
		const FString Shown = ClipText(Font, Text.Mid(First), R.W);
		TextSize(Font, Shown.IsEmpty() ? FString(TEXT("W")) : Shown, XL, YL);
		const float TY = FMath::RoundToFloat(R.T + (R.H - YL) * 0.5f);
		if (bFocus && GetBool(C, TEXT("bAllSelected")) && !Shown.IsEmpty())
		{
			float SW = 0.f, SH = 0.f;
			TextSize(Font, Shown, SW, SH);
			Fill(FBox4{ R.L, TY, SW, YL }, FColor(255, 255, 255, 60));
		}
		DrawText(Font, Shown, R.L, TY, FColor(FC.R, FC.G, FC.B, FC.A), MSTY_Alpha);
		if (bFocus && FMath::Fmod(Now, 1.0) < 0.6)
		{
			float CX = 0.f, CY = 0.f;
			TextSize(Font, Text.Mid(First, Caret - First), CX, CY);
			Fill(FBox4{ R.L + CX, TY, FMath::Max(1.f, YL * 0.08f), YL }, FColor(FC.R, FC.G, FC.B, 255));
		}
	}

	void DrawSlider(FUSObject* C)
	{
		const FBox4 B = BoundsOf(C);
		const uint8 St = State(C);
		FUSObject* Bar = GetO(C, TEXT("BarStyle"));
		StyleDraw(Bar ? Bar : GetO(C, TEXT("Style")), St, B);
		const float MinV = GetF(C, TEXT("MinValue")), MaxV = GetF(C, TEXT("MaxValue"), 100.f);
		const float Perc = MaxV > MinV ? FMath::Clamp((GetF(C, TEXT("Value")) - MinV) / (MaxV - MinV), 0.f, 1.f) : 0.f;
		const float MW = FMath::Max(1.f, GetF(C, TEXT("MarkerWidth"), 8.f));
		const float MX = B.L + (B.W - MW) * Perc;
		if (FUSObject* FillImg = GetO(C, TEXT("FillImage")))
		{
			DrawMaterial(FillImg, FBox4{ B.L, B.T, MX + MW * 0.5f - B.L, B.H }, ISTY_Stretched, FColor::White, MSTY_Alpha);
		}
		if (GetBool(C, TEXT("bShowMarker")))
		{
			StyleDraw(GetO(C, TEXT("Style")), St, FBox4{ MX, B.T, MW, B.H });
		}
		if (GetBool(C, TEXT("bShowCaption")))
		{
			FString Caption;
			const FScriptCall Cap = FindDelegate(C, TEXT("OnDrawCaption"), true);
			if (Cap.Function)
			{
				FUSParms Parms(Cap.Function);
				Parms.Call(Cap.Target);
				Caption = Parms.ReturnStr();
			}
			StyleText(GetO(C, TEXT("CaptionStyle")), St, B, TXTA_Center, Caption, GetB(C, TEXT("FontScale"), 1));
		}
	}

	/** GUIList rows (GUIListElem.Item), GUIScrollText lines, or OnDrawItem for each visible row. */
	void DrawList(FUSObject* L)
	{
		const FBox4 B = BoundsOf(L);
		const FBox4 CB = ClientOf(L);
		const uint8 St = State(L);
		FUSObject* S = GetO(L, TEXT("Style"));
		const int32 Count = GetI(L, TEXT("ItemCount"));
		if (Count == 0 && !GetBool(L, TEXT("bVisibleWhenEmpty")))
		{
			return;
		}
		StyleDraw(S, St, B);
		const float IH = FMath::Max(1.f, GetF(L, TEXT("ItemHeight"), 16.f));
		const int32 IPP = GetI(L, TEXT("ItemsPerPage"));
		const int32 Top = FMath::Max(0, GetI(L, TEXT("Top")));
		const int32 Index = GetI(L, TEXT("Index"), -1);
		FUSObject* Sel = GetO(L, TEXT("SelectedStyle"));
		FUSObject* SecStyle = GetO(L, TEXT("SectionStyle"));
		const uint8 FS = GetB(L, TEXT("FontScale"));
		const bool bOwnerDraw = IsBound(L, TEXT("OnDrawItem"));
		const bool bMC = Is(L, K().MCList);
		const bool bScrollText = Is(L, K().ScrollText);
		// the hot row (bHotTrack: a combo box's list follows the mouse)
		int32 Hot = -1;
		if (GetBool(L, TEXT("bHotTrack")) && CB.Contains(G.MouseX, G.MouseY))
		{
			Hot = Top + int32((G.MouseY - CB.T) / IH);
		}
		FUSProperty* Elements = Prop(L, bScrollText ? TEXT("StringElements") : TEXT("Elements"), EUSType::Array);
		const FUSScriptArray* EA = Elements ? reinterpret_cast<const FUSScriptArray*>(L->Data + Elements->Offset) : nullptr;
		FUSProperty* ItemP = Elements && Elements->Inner && Elements->Inner->Struct ? Elements->Inner->Struct->FindPropertyByName(TEXT("Item")) : nullptr;
		FUSProperty* SectionP = Elements && Elements->Inner && Elements->Inner->Struct ? Elements->Inner->Struct->FindPropertyByName(TEXT("bSection")) : nullptr;
		const int32 VisLines = bScrollText && !GetBool(L, TEXT("bNoTeletype")) ? GetI(L, TEXT("VisibleLines")) : MAX_int32;
		const int32 VisChars = GetI(L, TEXT("VisibleChars"));
		const uint8 Align = GetB(L, TEXT("TextAlign"));
		for (int32 Row = 0; Row < IPP && Top + Row < Count; ++Row)
		{
			const int32 i = Top + Row;
			const FBox4 R{ CB.L, CB.T + Row * IH, CB.W, IH };
			const bool bSelected = i == Index;
			if (bOwnerDraw)
			{
				ResetCanvas();
				CallDelegate(L, TEXT("OnDrawItem"), [&](FUSParms& P) { P.Object(G.CanvasObj).Int(i).Float(R.L).Float(R.T).Float(R.W).Float(R.H).Bool(bSelected).Bool(false); }, false);
				continue;
			}
			if (bMC)
			{
				continue;
			}
			FString Text;
			bool bSection = false;
			if (EA && i < EA->Num)
			{
				const uint8* E = EA->Data + int64(i) * Elements->Inner->ElementSize;
				if (bScrollText)
				{
					Text = *reinterpret_cast<const FString*>(E);
				}
				else if (ItemP)
				{
					Text = *reinterpret_cast<const FString*>(E + ItemP->Offset);
					bSection = SectionP && *reinterpret_cast<const uint32*>(E + SectionP->Offset) != 0;
				}
			}
			if (bScrollText)
			{
				if (i > VisLines)
				{
					break;
				}
				if (i == VisLines)
				{
					Text = Text.Left(VisChars);
				}
				StyleText(S, MSAT_Blurry, R, Align, Text, FS);
				continue;
			}
			if (bSection && SecStyle)
			{
				StyleDraw(SecStyle, MSAT_Blurry, R);
				StyleText(SecStyle, MSAT_Blurry, R, GetB(L, TEXT("SectionJustification"), TXTA_Center), Text, FS);
				continue;
			}
			if (bSelected || i == Hot)
			{
				FUSObject* SS = Sel ? Sel : S;
				const uint8 SelState = bSelected ? (St == MSAT_Focused || St == MSAT_Pressed ? MSAT_Focused : MSAT_Watched) : MSAT_Watched;
				StyleDraw(SS, SelState, R);
				StyleText(SS, SelState, R, Align, Text, FS);
				continue;
			}
			StyleText(S, St == MSAT_Disabled ? MSAT_Disabled : MSAT_Blurry, R, Align, Text, FS);
		}
	}

	void DrawMCHeader(FUSObject* C)
	{
		FUSObject* L = GetO(C, TEXT("MyList"));
		const FBox4 B = BoundsOf(C);
		FUSObject* S = GetO(C, TEXT("Style"));
		FUSProperty* WP = L ? Prop(L, TEXT("ColumnWidths"), EUSType::Array) : nullptr;
		FUSProperty* HP = L ? Prop(L, TEXT("ColumnHeadings"), EUSType::Array) : nullptr;
		if (!WP || !HP)
		{
			StyleDraw(S, MSAT_Blurry, B);
			return;
		}
		const FUSScriptArray& WA = *reinterpret_cast<const FUSScriptArray*>(L->Data + WP->Offset);
		const FUSScriptArray& HA = *reinterpret_cast<const FUSScriptArray*>(L->Data + HP->Offset);
		float X = B.L;
		const int32 SortCol = GetI(L, TEXT("SortColumn"), -1);
		for (int32 i = 0; i < WA.Num; ++i)
		{
			const float W = FMath::Min(reinterpret_cast<const float*>(WA.Data)[i], B.R() - X);
			if (W <= 0.f)
			{
				break;
			}
			const FBox4 R{ X, B.T, W, B.H };
			const uint8 St = i == SortCol ? MSAT_Focused : (R.Contains(G.MouseX, G.MouseY) ? MSAT_Watched : MSAT_Blurry);
			StyleDraw(S, St, R);
			if (i < HA.Num)
			{
				StyleText(S, St, FBox4{ R.L + 2.f, R.T, R.W - 4.f, R.H }, TXTA_Center, reinterpret_cast<const FString*>(HA.Data)[i], GetB(C, TEXT("FontScale")));
			}
			X += W;
		}
	}

	void DrawTabControl(FUSObject* C)
	{
		const FBox4 B = BoundsOf(C);
		if (GetBool(C, TEXT("bFillBackground")))
		{
			const FUSColor FC = GetC(C, TEXT("FillColor"));
			Fill(B, FColor(FC.R, FC.G, FC.B, FC.A));
		}
		float TabH = GetF(C, TEXT("TabHeight"), 0.08f);
		TabH = TabH < 2.f ? TabH * ScreenH() : TabH;
		if (FUSObject* BS = GetO(C, TEXT("BackgroundStyle")))
		{
			StyleDraw(BS, MSAT_Blurry, FBox4{ B.L, B.T + TabRows * TabH, B.W, FMath::Max(0.f, B.H - TabRows * TabH) });
		}
		else if (FUSObject* BI = GetO(C, TEXT("BackgroundImage")))
		{
			DrawMaterial(BI, FBox4{ B.L, B.T + TabRows * TabH, B.W, FMath::Max(0.f, B.H - TabRows * TabH) }, ISTY_Stretched, FColor::White, MSTY_Alpha);
		}
		const FObjArray Tabs = GetArr(C, TEXT("TabStack"));
		for (int32 i = 0; i < Tabs.Num(); ++i)
		{
			if (Tabs[i] && GetBool(Tabs[i], TEXT("bVisible")))
			{
				DrawButton(Tabs[i]);
			}
		}
	}

	void DrawSelf(FUSObject* C)
	{
		const FGUIClasses& KC = K();
		if (Is(C, KC.Page))
		{
			if (FUSObject* Bg = GetO(C, TEXT("Background")))
			{
				const FUSColor BC = GetC(C, TEXT("BackgroundColor"));
				DrawMaterial(Bg, BoundsOf(C), ISTY_Scaled, FColor(BC.R, BC.G, BC.B, BC.A), RenderStyle(GetB(C, TEXT("BackgroundRStyle"), MSTY_Normal)));
			}
		}
		else if (Is(C, KC.TabControl))
		{
			DrawTabControl(C);
		}
		else if (Is(C, KC.Panel))
		{
			if (FUSObject* Bg = GetO(C, TEXT("Background")))
			{
				DrawMaterial(Bg, BoundsOf(C), ISTY_Stretched, FColor::White, MSTY_Alpha);
			}
		}
		else if (Is(C, KC.ListBoxBase))
		{
			if (!Is(C, KC.MCListBox) || GetBool(C, TEXT("bFullHeightStyle")))
			{
				StyleDraw(GetO(C, TEXT("Style")), MSAT_Blurry, BoundsOf(C));
			}
		}
		else if (Is(C, KC.Multi))
		{
			// moComponents, combo boxes, scroll bars, numeric edits: their parts draw
		}
		else if (Is(C, KC.Label))
		{
			DrawLabel(C);
		}
		else if (Is(C, KC.Section))
		{
			DrawSection(C);
		}
		else if (Is(C, KC.Image))
		{
			DrawImage(C);
		}
		else if (Is(C, KC.EditBox))
		{
			DrawEditBox(C);
		}
		else if (Is(C, KC.GFXButton))
		{
			DrawGFXButton(C);
		}
		else if (Is(C, KC.Button))
		{
			DrawButton(C);
		}
		else if (Is(C, KC.Slider))
		{
			DrawSlider(C);
		}
		else if (Is(C, KC.ListBase))
		{
			DrawList(C);
		}
		else if (Is(C, KC.MCHeader))
		{
			DrawMCHeader(C);
		}
		else if (Is(C, KC.ScrollZone))
		{
			StyleDraw(GetO(C, TEXT("Style")), State(C), BoundsOf(C));
		}
		else if (Is(C, KC.ToolTip) || Is(C, KC.ContextMenu))
		{
			// not shown
		}
		else if (FUSObject* S = GetO(C, TEXT("Style")))
		{
			StyleDraw(S, State(C), BoundsOf(C));
		}
	}

	void Draw(FUSObject* C, int32 Depth = 0)
	{
		if (!C || !GetBool(C, TEXT("bVisible")) || Depth > 40)
		{
			return;
		}
		bool bSkip = false;
		if (IsBound(C, TEXT("OnDraw")))
		{
			ResetCanvas();
			bSkip = CallDelegate(C, TEXT("OnDraw"), [this](FUSParms& P) { P.Object(G.CanvasObj); }, false);
		}
		if (!bSkip)
		{
			DrawSelf(C);
		}
		if (IsBound(C, TEXT("OnRender")))
		{
			ResetCanvas();
			CallDelegate(C, TEXT("OnRender"), [this](FUSParms& P) { P.Object(G.CanvasObj); }, false);
		}
		const FObjArray Controls = GetArr(C, TEXT("Controls"));
		const bool bCombo = Is(C, K().Combo);
		FUSObject* PopupList = bCombo ? GetO(C, TEXT("MyListBox")) : nullptr;
		for (int32 i = 0; i < Controls.Num(); ++i)
		{
			FUSObject* Child = Controls[i];
			if (Child && Child == PopupList)
			{
				// an open combo list draws over the rest of the page
				if (GetBool(Child, TEXT("bVisible")))
				{
					Popups.Add(Child);
				}
				continue;
			}
			Draw(Child, Depth + 1);
		}
		if (IsBound(C, TEXT("OnRendered")))
		{
			ResetCanvas();
			CallDelegate(C, TEXT("OnRendered"), [this](FUSParms& P) { P.Object(G.CanvasObj); }, false);
		}
	}

	void DrawCursor()
	{
		const FObjArray Cursors = GetArr(Ctl(), TEXT("MouseCursors"));
		int32 Index = Hover ? GetI(Hover, TEXT("MouseCursorIndex")) : 0;
		Index = Index >= 0 && Index < Cursors.Num() ? Index : 0;
		FUSObject* Cursor = Cursors[Index];
		float MW = 0.f, MH = 0.f;
		if (Cursor && MatSize(Cursor, MW, MH))
		{
			const float S = FMath::Max(1.f, FMath::RoundToFloat(ScreenH() / 960.f));
			Tile(Cursor, G.MouseX, G.MouseY, MW * S, MH * S, 0.f, 0.f, MW, MH, FColor::White, MSTY_Alpha);
		}
		else
		{
			Fill(FBox4{ G.MouseX, G.MouseY, 3.f, 12.f }, FColor::White);
			Fill(FBox4{ G.MouseX, G.MouseY, 9.f, 3.f }, FColor::White);
		}
	}

	void DrawAll()
	{
		const FObjArray S = Stack();
		FUSObject* Top = TopPage();
		const FString TopName = Top ? Top->Class->GetPathName() : FString();
		if (TopName != LastTop)
		{
			// a new page: the mouse state of the old one goes
			LastTop = TopName;
			Hover = nullptr;
			Pressed = nullptr;
			PrevPopups.Reset();
			if (Top)
			{
				++Opened;
			}
		}
		++Frames;
		Popups.Reset();
		for (int32 i = 0; i < S.Num(); ++i)
		{
			FUSObject* Page = S[i];
			if (!Page)
			{
				continue;
			}
			PreDraw(Page);
			if (Page == Top && GetB(Page, TEXT("BackgroundRStyle")) == MSTY_Modulated && !GetO(Page, TEXT("Background")))
			{
				// a popup page darkens what is under it (UE2 modulates the world and the pages below by InactiveFadeColor)
				const FUSColor F = GetC(Page, TEXT("InactiveFadeColor"));
				Fill(FBox4{ 0.f, 0.f, ScreenW(), ScreenH() }, FColor(0, 0, 0, uint8(255 - FMath::Min<int32>(F.R, 255))));
			}
			const bool bLower = Page != Top;
			GModulate = bLower ? FColor(170, 170, 170, 255) : FColor(255, 255, 255, 255);
			const int32 PopupStart = Popups.Num();
			Draw(Page);
			for (int32 p = PopupStart; p < Popups.Num(); ++p)
			{
				Draw(Popups[p]);
			}
			if (bLower)
			{
				Popups.SetNum(PopupStart);
			}
			GModulate = FColor(255, 255, 255, 255);
		}
		PrevPopups = Popups;
		// the hovered control follows the layout of this frame
		UpdateHover();
		if (Top)
		{
			DrawCursor();
		}
	}

	// ---- hit testing and input ---------------------------------------------------------------------

	/**
	 * The control under (X, Y) in C. A multi-component that takes input only counts where none of its parts (and no later
	 * sibling's part) is hit: UTComp's empty tab control lies over its row of buttons.
	 */
	FUSObject* HitTest(FUSObject* C, float X, float Y, int32 Depth = 0) const
	{
		bool bWeak = false;
		return HitTestWeak(C, X, Y, Depth, bWeak);
	}

	FUSObject* HitTestWeak(FUSObject* C, float X, float Y, int32 Depth, bool& bOutWeak) const
	{
		bOutWeak = false;
		if (!C || !GetBool(C, TEXT("bVisible")) || Depth > 40)
		{
			return nullptr;
		}
		if (Is(C, K().TabControl))
		{
			const FObjArray Tabs = GetArr(C, TEXT("TabStack"));
			for (int32 i = Tabs.Num() - 1; i >= 0; --i)
			{
				if (Tabs[i] && GetBool(Tabs[i], TEXT("bVisible")) && BoundsOf(Tabs[i]).Contains(X, Y))
				{
					return Tabs[i];
				}
			}
		}
		const FObjArray Controls = GetArr(C, TEXT("Controls"));
		FUSObject* PopupList = Is(C, K().Combo) ? GetO(C, TEXT("MyListBox")) : nullptr;
		FUSObject* Fallback = nullptr;
		for (int32 i = Controls.Num() - 1; i >= 0; --i)
		{
			if (Controls[i] == PopupList)
			{
				continue;
			}
			bool bWeak = false;
			if (FUSObject* Hit = HitTestWeak(Controls[i], X, Y, Depth + 1, bWeak))
			{
				if (!bWeak)
				{
					return Hit;
				}
				Fallback = Fallback ? Fallback : Hit;
			}
		}
		if (Fallback)
		{
			bOutWeak = true;
			return Fallback;
		}
		if (GetBool(C, TEXT("bAcceptsInput")) && BoundsOf(C).Contains(X, Y))
		{
			bOutWeak = Is(C, K().Multi);
			return C;
		}
		return nullptr;
	}

	/** The control under the mouse on the top page (its open combo lists first); the page itself when none. */
	FUSObject* HitTop(float X, float Y) const
	{
		FUSObject* Top = TopPage();
		if (!Top)
		{
			return nullptr;
		}
		for (int32 i = PrevPopups.Num() - 1; i >= 0; --i)
		{
			if (FUSObject* Hit = HitTest(PrevPopups[i], X, Y))
			{
				return Hit;
			}
		}
		return HitTest(Top, X, Y);
	}

	bool InTree(FUSObject* C) const
	{
		FUSObject* Top = TopPage();
		for (FUSObject* O = C; O; O = GetO(O, TEXT("MenuOwner")))
		{
			if (O == Top)
			{
				return true;
			}
		}
		return false;
	}

	void UpdateHover()
	{
		if (Hover && (Hover->IsPendingKill() || !InTree(Hover)))
		{
			Hover = nullptr;
		}
		if (Pressed)
		{
			return;
		}
		FUSObject* H = HitTop(G.MouseX, G.MouseY);
		if (H && Is(H, K().Page))
		{
			H = nullptr;
		}
		if (H == Hover)
		{
			return;
		}
		if (Hover && GetB(Hover, TEXT("MenuState")) == MSAT_Watched)
		{
			MenuStateChange(Hover, GetBool(Hover, TEXT("bHasFocus")) ? MSAT_Focused : MSAT_Blurry);
		}
		Hover = H;
		SetO(Ctl(), TEXT("ActiveControl"), H);
		if (H && GetB(H, TEXT("MenuState")) != MSAT_Disabled && GetB(H, TEXT("MenuState")) != MSAT_Pressed)
		{
			MenuStateChange(H, MSAT_Watched);
		}
	}

	FUSObject* Focused() const
	{
		return GetO(Ctl(), TEXT("FocusedControl"));
	}

	void SetFocusTo(FUSObject* C)
	{
		if (!C)
		{
			return;
		}
		if (GetBool(C, TEXT("bNeverFocus")))
		{
			if (FUSObject* Instead = GetO(C, TEXT("FocusInstead")))
			{
				CallEvent(Instead, TEXT("SetFocus"), [](FUSParms& P) { P.Object(nullptr); });
			}
			return;
		}
		CallEvent(C, TEXT("SetFocus"), [](FUSParms& P) { P.Object(nullptr); });
	}

	void Click(FUSObject* C, bool bDouble)
	{
		if (bDouble && CallDelegate(C, TEXT("OnDblClick"), [C](FUSParms& P) { P.Object(C); }))
		{
			return;
		}
		const FScriptCall Bound = FindDelegate(C, TEXT("OnClick"), false);
		UE_LOG(LogUScript, Log, TEXT("UScriptGUI: click on %s (%s) -> %s"), *C->GetName(), *GetS(C, TEXT("Caption")),
			Bound.Function ? *FString::Printf(TEXT("%s.%s"), *Bound.Target->GetName(), *Bound.Function->GetName()) : TEXT("its default"));
		CallDelegate(C, TEXT("OnClick"), [C](FUSParms& P) { P.Object(C); });
	}

	bool MousePress()
	{
		FUSObject* C = HitTop(G.MouseX, G.MouseY);
		if (!C || Is(C, K().Page))
		{
			// the page's background: open combo lists close (their list's OnInvalidate)
			FUSObject* Page = TopPage();
			if (FUSObject* F = Focused())
			{
				CallDelegate(F, TEXT("OnInvalidate"), [Page](FUSParms& P) { P.Object(Page); });
			}
			for (FUSObject* Pop : PrevPopups)
			{
				if (FUSObject* L = GetO(Pop, TEXT("MyList")))
				{
					CallDelegate(L, TEXT("OnInvalidate"), [Page](FUSParms& P) { P.Object(Page); });
				}
			}
			return true;
		}
		if (GetB(C, TEXT("MenuState")) == MSAT_Disabled)
		{
			return true;
		}
		SetFocusTo(C);
		const bool bDouble = C == LastClicked && Now - LastClickTime < FMath::Max(0.2f, GetF(Ctl(), TEXT("DblClickWindow"), 0.5f));
		Pressed = C;
		bPressedOver = true;
		NextRepeat = Now + FMath::Max(0.1f, GetF(Ctl(), TEXT("ButtonRepeatDelay"), 0.25f));
		MenuStateChange(C, MSAT_Pressed);
		CallDelegate(C, TEXT("OnMousePressed"), [C](FUSParms& P) { P.Object(C).Bool(false); });
		if (GetBool(C, TEXT("bRepeatClick")) && !C->IsPendingKill())
		{
			Click(C, false);
		}
		LastClicked = C;
		LastClickTime = bDouble ? -10.0 : Now;
		if (bDouble && !GetBool(C, TEXT("bRepeatClick")))
		{
			PendingDouble = true;
		}
		return true;
	}

	bool PendingDouble = false;

	bool MouseRelease()
	{
		FUSObject* C = Pressed;
		Pressed = nullptr;
		const bool bDouble = PendingDouble;
		PendingDouble = false;
		if (!C || C->IsPendingKill())
		{
			return true;
		}
		const bool bOver = BoundsOf(C).Contains(G.MouseX, G.MouseY);
		CallDelegate(C, TEXT("OnMouseRelease"), [C](FUSParms& P) { P.Object(C); });
		if (GetBool(Ctl(), TEXT("bIgnoreNextRelease")))
		{
			SetBool(Ctl(), TEXT("bIgnoreNextRelease"), false);
		}
		else if ((bOver || GetBool(C, TEXT("bRequireReleaseClick"))) && !GetBool(C, TEXT("bRepeatClick")))
		{
			Click(C, bDouble);
		}
		if (!C->IsPendingKill() && GetB(C, TEXT("MenuState")) == MSAT_Pressed)
		{
			MenuStateChange(C, bOver ? MSAT_Watched : (GetBool(C, TEXT("bHasFocus")) ? MSAT_Focused : MSAT_Blurry));
		}
		return true;
	}

	/** OnKeyEvent along a chain (the control, then its owners up to the page). */
	bool KeyChain(FUSObject* From, uint8 Key, uint8 Action, float Delta)
	{
		for (FUSObject* C = From; C; C = GetO(C, TEXT("MenuOwner")))
		{
			if (GetB(C, TEXT("MenuState")) == MSAT_Disabled)
			{
				continue;
			}
			if (CallDelegate(C, TEXT("OnKeyEvent"), [Key, Action, Delta](FUSParms& P) { P.Byte(Key).Byte(Action).Float(Delta); }))
			{
				return true;
			}
		}
		return false;
	}

	bool Key(uint8 InKey, uint8 Action, float Delta)
	{
		FUSObject* C = Ctl();
		if (!C || !TopPage())
		{
			return false;
		}
		const bool bDown = Action == IST_Press || Action == IST_Hold;
		if (Action == IST_Press || Action == IST_Release)
		{
			if (uint8* KD = Ptr<uint8>(C, TEXT("KeyDown"), EUSType::Byte))
			{
				if (FUSProperty* P = Prop(C, TEXT("KeyDown"), EUSType::Byte); P && P->ArrayDim > InKey)
				{
					KD[InKey] = Action == IST_Press ? 1 : 0;
				}
			}
			if (InKey == IK_Shift || InKey == IK_LShift || InKey == IK_RShift)
			{
				SetBool(C, TEXT("ShiftPressed"), Action == IST_Press);
			}
			else if (InKey == IK_Ctrl || InKey == IK_LControl || InKey == IK_RControl)
			{
				SetBool(C, TEXT("CtrlPressed"), Action == IST_Press);
			}
			else if (InKey == IK_Alt)
			{
				SetBool(C, TEXT("AltPressed"), Action == IST_Press);
			}
		}
		if (InKey == IK_LeftMouse)
		{
			return Action == IST_Press ? MousePress() : (Action == IST_Release ? MouseRelease() : true);
		}
		if (InKey == IK_RightMouse)
		{
			if (Action == IST_Press)
			{
				if (FUSObject* H = HitTop(G.MouseX, G.MouseY); H && !Is(H, K().Page))
				{
					CallDelegate(H, TEXT("OnRightClick"), [H](FUSParms& P) { P.Object(H); });
				}
			}
			return true;
		}
		if (InKey == IK_MouseWheelUp || InKey == IK_MouseWheelDown)
		{
			// the wheel goes to what is under the mouse, then to the focused control
			FUSObject* H = HitTop(G.MouseX, G.MouseY);
			for (uint8 A : { uint8(IST_Press), uint8(IST_Release) })
			{
				if (!KeyChain(H, InKey, A, Delta))
				{
					KeyChain(Focused(), InKey, A, Delta);
				}
			}
			return true;
		}
		FUSObject* F = Focused();
		if (KeyChain(F ? F : TopPage(), InKey, Action, Delta))
		{
			return true;
		}
		if (!bDown || Action != IST_Press)
		{
			return true;
		}
		if (InKey == IK_Escape)
		{
			CallEvent(C, TEXT("CloseMenu"), [](FUSParms& P) { P.Bool(true); });
			return true;
		}
		if (InKey == IK_Tab || InKey == IK_Up || InKey == IK_Down)
		{
			const bool bBack = InKey == IK_Up || (InKey == IK_Tab && GetBool(C, TEXT("ShiftPressed")));
			if (F)
			{
				CallEvent(F, bBack ? TEXT("PrevControl") : TEXT("NextControl"), [](FUSParms& P) { P.Object(nullptr); });
			}
			else
			{
				CallEvent(TopPage(), bBack ? TEXT("FocusLast") : TEXT("FocusFirst"), [](FUSParms& P) { P.Object(nullptr); });
			}
			return true;
		}
		return true;
	}

	bool Type(uint8 InKey, const FString& Unicode)
	{
		if (!TopPage())
		{
			return false;
		}
		for (FUSObject* C = Focused(); C; C = GetO(C, TEXT("MenuOwner")))
		{
			if (CallDelegate(C, TEXT("OnKeyType"), [InKey, &Unicode](FUSParms& P) { P.Byte(InKey).Str(Unicode); }))
			{
				return true;
			}
		}
		UE_LOG(LogUScript, Verbose, TEXT("UScriptGUI: typed %d '%s' not taken (focus %s)"), InKey, *Unicode, Focused() ? *Focused()->GetName() : TEXT("none"));
		return true;
	}

	void MouseMove(float X, float Y)
	{
		const float DX = X - G.MouseX, DY = Y - G.MouseY;
		G.MouseX = X;
		G.MouseY = Y;
		FUSObject* C = Ctl();
		SetF(C, TEXT("LastMouseX"), GetF(C, TEXT("MouseX")));
		SetF(C, TEXT("LastMouseY"), GetF(C, TEXT("MouseY")));
		SetF(C, TEXT("MouseX"), X);
		SetF(C, TEXT("MouseY"), Y);
		if (Pressed && !Pressed->IsPendingKill() && (DX != 0.f || DY != 0.f) && GetBool(Pressed, TEXT("bCaptureMouse")))
		{
			CallDelegate(Pressed, TEXT("OnCapturedMouseMove"), [DX, DY](FUSParms& P) { P.Float(DX).Float(DY); });
		}
	}

	void Tick(float DeltaSeconds)
	{
		Now += DeltaSeconds;
		FUSObject* C = Ctl();
		if (!C)
		{
			return;
		}
		SetF(C, TEXT("RenderDelta"), DeltaSeconds);
		SetF(C, TEXT("LastRenderTime"), float(Now));
		// UE2 never opens the main menu from a game's menus
		SetBool(C, TEXT("MainNotWanted"), true);
		// timers: the component's TimerInterval is the truth (GUIScrollText sets it directly)
		TArray<FUSObject*> Due;
		for (auto It = G.Timers.CreateIterator(); It; ++It)
		{
			FUSObject* O = It.Key();
			if (!O || O->IsPendingKill())
			{
				It.RemoveCurrent();
				continue;
			}
			const float Interval = GetF(O, TEXT("TimerInterval"));
			if (Interval <= 0.f)
			{
				It.RemoveCurrent();
				continue;
			}
			It.Value().Interval = Interval;
			It.Value().Countdown -= DeltaSeconds;
			if (It.Value().Countdown <= 0.f)
			{
				Due.Add(O);
				if (It.Value().bRepeat || GetBool(O, TEXT("bTimerRepeat")))
				{
					It.Value().Countdown = FMath::Max(It.Value().Countdown + Interval, 0.001f);
				}
				else
				{
					It.RemoveCurrent();
					SetF(O, TEXT("TimerInterval"), 0.f);
				}
			}
		}
		for (FUSObject* O : Due)
		{
			if (!O->IsPendingKill())
			{
				CallEvent0(O, TEXT("Timer"));
			}
		}
		// held repeat buttons (scroll arrows, spinners) click again
		if (Pressed && !Pressed->IsPendingKill() && GetBool(Pressed, TEXT("bRepeatClick")) && Now >= NextRepeat)
		{
			NextRepeat = Now + 0.08;
			if (BoundsOf(Pressed).Contains(G.MouseX, G.MouseY))
			{
				FUSObject* P = Pressed;
				CallDelegate(P, TEXT("OnMousePressed"), [P](FUSParms& Parms) { Parms.Object(P).Bool(true); });
				if (Pressed == P)
				{
					Click(P, false);
				}
			}
		}
	}
};

// =================================================================================================
// FUSGUI
// =================================================================================================

FUSGUI::FUSGUI(FUSWorld* InWorld)
	: Impl(MakeUnique<FImpl>(*this)), World(InWorld)
{
	GCurrent = this;
}

FUSGUI::~FUSGUI()
{
	if (GCurrent == this)
	{
		GCurrent = nullptr;
	}
}

FUSGUI* FUSGUI::Current()
{
	return GCurrent;
}

bool FUSGUI::Init(FUSObject* Viewport, FUSObject* InteractionMaster, FString& OutError)
{
	FUSRuntime& RT = FUSRuntime::Get();
	if (!K().bResolved)
	{
		OutError = TEXT("XInterface is missing");
		return false;
	}
	FUSClass* Class = RT.FindClass(TEXT("GUI2K4.UT2K4GUIController"), &OutError);
	Class = Class ? Class : RT.FindClass(TEXT("XInterface.GUIController"), &OutError);
	if (!Class || (Class->ClassFlags & USFlags::CLASS_Abstract))
	{
		OutError = OutError.IsEmpty() ? TEXT("no GUIController class") : OutError;
		return false;
	}
	Controller = World->ConstructObject(Class, nullptr, FName(TEXT("GUIController0")));
	if (!Controller)
	{
		OutError = TEXT("cannot create the GUIController");
		return false;
	}
	USGUI::InstanceSubobjects(Controller);
	SetO(Controller, TEXT("ViewportOwner"), Viewport);
	SetO(Controller, TEXT("Master"), InteractionMaster);
	SetBool(Controller, TEXT("MainNotWanted"), true);
	if (FUSProperty* P = Viewport ? Viewport->FindProperty(TEXT("GUIController")) : nullptr; P && P->Type == EUSType::Object)
	{
		*reinterpret_cast<FUSObject**>(Viewport->Data + P->Offset) = Controller;
	}
	// GUIController.InitializeController without its player record / cache precaching: the styles, the fonts' controller
	int32 Styles = 0;
	FUSFunction* Register = USVM::FindFunction(Controller, TEXT("RegisterStyle"));
	for (const TCHAR* ListName : { TEXT("DefaultStyleNames"), TEXT("StyleNames") })
	{
		FUSProperty* P = Prop(Controller, ListName, EUSType::Array);
		if (!P || !P->Inner || P->Inner->Type != EUSType::Str || !Register)
		{
			continue;
		}
		TArray<FString> Names;
		const FUSScriptArray& A = *reinterpret_cast<const FUSScriptArray*>(Controller->Data + P->Offset);
		for (int32 i = 0; i < A.Num; ++i)
		{
			Names.Add(*reinterpret_cast<const FString*>(A.Data + int64(i) * P->Inner->ElementSize));
		}
		for (const FString& Name : Names)
		{
			FUSClass* SC = RT.FindClass(Name);
			if (!SC)
			{
				continue;
			}
			FUSParms Parms(Register);
			Parms.Object(SC);
			Parms.Call(Controller);
			Styles += Parms.ReturnBool() ? 1 : 0;
		}
	}
	const FObjArray Fonts = GetArr(Controller, TEXT("FontStack"));
	for (int32 i = 0; i < Fonts.Num(); ++i)
	{
		SetO(Fonts[i], TEXT("Controller"), Controller);
	}
	UE_LOG(LogUScript, Log, TEXT("UScriptGUI: %s ready (%d styles, %d fonts)"), *Class->GetPathName(), Styles, Fonts.Num());
	if (Styles == 0)
	{
		OutError = TEXT("no menu styles registered");
		return false;
	}
	return true;
}

bool FUSGUI::IsOpen() const
{
	return Controller && Impl->TopPage() != nullptr;
}

FString FUSGUI::TopMenuName() const
{
	FUSObject* Top = Impl->TopPage();
	return Top ? Top->Class->GetPathName() : FString();
}

void FUSGUI::Tick(float DeltaSeconds)
{
	GCurrent = this;
	Impl->Tick(DeltaSeconds);
}

void FUSGUI::Draw(FUSObject* InCanvasObj, float SizeX, float SizeY)
{
	if (!Controller)
	{
		return;
	}
	GCurrent = this;
	CanvasObj = InCanvasObj;
	ResX = FMath::Max(1.f, SizeX);
	ResY = FMath::Max(1.f, SizeY);
	if (int32* RX = Ptr<int32>(Controller, TEXT("ResX"), EUSType::Int))
	{
		*RX = int32(ResX);
	}
	if (int32* RY = Ptr<int32>(Controller, TEXT("ResY"), EUSType::Int))
	{
		*RY = int32(ResY);
	}
	Impl->DrawAll();
}

bool FUSGUI::KeyEvent(uint8 Key, uint8 Action, float Delta)
{
	GCurrent = this;
	return Impl->Key(Key, Action, Delta);
}

bool FUSGUI::KeyType(uint8 Key, const FString& Unicode)
{
	GCurrent = this;
	return Impl->Type(Key, Unicode);
}

void FUSGUI::MouseMove(float X, float Y)
{
	GCurrent = this;
	Impl->MouseMove(X, Y);
}

void FUSGUI::CloseAll()
{
	if (Controller && IsOpen())
	{
		CallEvent(Controller, TEXT("CloseAll"), [](FUSParms& P) { P.Bool(true).Bool(true); });
	}
}

FString FUSGUI::Describe() const
{
	return FString::Printf(TEXT("menus %s: %d opened, %d frames drawn, top %s"), Controller ? TEXT("ready") : TEXT("off"), Impl->Opened, Impl->Frames,
		IsOpen() ? *TopMenuName() : TEXT("none"));
}

namespace
{
	FString DescribeComponent(FUSObject* C)
	{
		FString Text = GetS(C, TEXT("Caption"));
		if (Text.IsEmpty())
		{
			Text = GetS(C, TEXT("TextStr"));
		}
		FString Value;
		if (Is(C, K().CheckBox))
		{
			Value = GetBool(C, TEXT("bChecked")) ? TEXT(" checked") : TEXT(" unchecked");
		}
		else if (Is(C, K().Slider))
		{
			Value = FString::Printf(TEXT(" value %g"), GetF(C, TEXT("Value")));
		}
		else if (Is(C, K().ListBase))
		{
			Value = FString::Printf(TEXT(" index %d of %d"), GetI(C, TEXT("Index")), GetI(C, TEXT("ItemCount")));
		}
		if (FUSProperty* P = Prop(C, TEXT("OnClick"), EUSType::Delegate))
		{
			const FUSDelegate& D = *reinterpret_cast<const FUSDelegate*>(C->Data + P->Offset);
			if (D.FunctionName != NAME_None)
			{
				Value += FString::Printf(TEXT(" OnClick=%s.%s"), D.Object ? *D.Object->GetName() : TEXT("None"), *D.FunctionName.ToString());
			}
		}
		const FBox4 B = BoundsOf(C);
		return FString::Printf(TEXT("%s %s \"%s\"%s state %d %s at %.0f,%.0f %.0fx%.0f"), *C->Class->GetName(), *C->GetName(), *Text, *Value,
			GetB(C, TEXT("MenuState")), GetBool(C, TEXT("bVisible")) ? TEXT("visible") : TEXT("hidden"), B.L, B.T, B.W, B.H);
	}

	void DumpTree(FUSObject* C, int32 Depth, FString& Out)
	{
		if (!C || Depth > 30)
		{
			return;
		}
		Out += FString::ChrN(Depth * 2, TEXT(' ')) + DescribeComponent(C) + TEXT("\n");
		const FObjArray Controls = GetArr(C, TEXT("Controls"));
		for (int32 i = 0; i < Controls.Num(); ++i)
		{
			DumpTree(Controls[i], Depth + 1, Out);
		}
		const FObjArray Tabs = GetArr(C, TEXT("TabStack"));
		for (int32 i = 0; i < Tabs.Num(); ++i)
		{
			DumpTree(Tabs[i], Depth + 1, Out);
		}
	}

	FUSObject* FindNamed(FUSObject* C, const FString& Name, int32 Depth)
	{
		if (!C || Depth > 30 || !GetBool(C, TEXT("bVisible")))
		{
			return nullptr;
		}
		const FObjArray Controls = GetArr(C, TEXT("Controls"));
		for (int32 i = 0; i < Controls.Num(); ++i)
		{
			if (FUSObject* Hit = FindNamed(Controls[i], Name, Depth + 1))
			{
				return Hit;
			}
		}
		const FObjArray Tabs = GetArr(C, TEXT("TabStack"));
		for (int32 i = 0; i < Tabs.Num(); ++i)
		{
			if (FUSObject* Hit = FindNamed(Tabs[i], Name, Depth + 1))
			{
				return Hit;
			}
		}
		if (C->GetName().Equals(Name, ESearchCase::IgnoreCase) || GetS(C, TEXT("Caption")).Equals(Name, ESearchCase::IgnoreCase))
		{
			return C;
		}
		return nullptr;
	}
}

FString FUSGUI::DumpTop() const
{
	FString Out;
	const FObjArray S = GetArr(Controller, TEXT("MenuStack"));
	if (S.Num() > 0)
	{
		DumpTree(S[S.Num() - 1], 0, Out);
	}
	return Out;
}

bool FUSGUI::ClickNamed(const FString& NameOrCaption)
{
	const FObjArray S = GetArr(Controller, TEXT("MenuStack"));
	FUSObject* C = S.Num() > 0 ? FindNamed(S[S.Num() - 1], NameOrCaption, 0) : nullptr;
	if (!C)
	{
		return false;
	}
	// a menu option's control (a check box's box) is what a click on it means
	FUSObject* Option = Is(C, K().MenuOption) ? C : (Is(GetO(C, TEXT("MenuOwner")), K().MenuOption) ? GetO(C, TEXT("MenuOwner")) : nullptr);
	if (FUSObject* Comp = Option ? GetO(Option, TEXT("MyComponent")) : nullptr)
	{
		C = Comp;
	}
	const FBox4 B = BoundsOf(C);
	GCurrent = this;
	Impl->MouseMove(B.L + B.W * 0.5f, B.T + B.H * 0.5f);
	Impl->UpdateHover();
	Impl->Key(IK_LeftMouse, IST_Press, 0.f);
	Impl->Key(IK_LeftMouse, IST_Release, 0.f);
	return true;
}

// =================================================================================================
// USGUI
// =================================================================================================

bool USGUI::IsGUIClass(const FUSClass* Class)
{
	const FUSClass* GUI = K().GUI;
	return GUI && Class && Class->IsChildOf(GUI);
}

void USGUI::InstanceSubobjects(FUSObject* Obj)
{
	if (!Obj || !Obj->Data || !Obj->World)
	{
		return;
	}
	FInstancer I;
	I.World = Obj->World;
	I.Fix(Obj);
}

namespace USNatives
{
	void RegisterGUI()
	{
		static const FUSNativeEntry Entries[] = {
			{ TEXT("GUIComponent"), TEXT("PlayerOwner"), &PlayerOwner }, { TEXT("GUIComponent"), TEXT("SetTimer"), &CompSetTimer },
			{ TEXT("GUIComponent"), TEXT("KillTimer"), &CompKillTimer }, { TEXT("GUIComponent"), TEXT("AutoPosition"), &AutoPosition },
			{ TEXT("GUIComponent"), TEXT("AutoPositionOn"), &AutoPositionOn }, { TEXT("GUIComponent"), TEXT("UpdateOffset"), &UpdateOffset },
			{ TEXT("GUIComponent"), TEXT("ActualWidth"), &ActualNative<0> }, { TEXT("GUIComponent"), TEXT("ActualHeight"), &ActualNative<1> },
			{ TEXT("GUIComponent"), TEXT("ActualLeft"), &ActualNative<2> }, { TEXT("GUIComponent"), TEXT("ActualTop"), &ActualNative<3> },
			{ TEXT("GUIComponent"), TEXT("RelativeLeft"), &RelativeNative<0> }, { TEXT("GUIComponent"), TEXT("RelativeTop"), &RelativeNative<1> },
			{ TEXT("GUIComponent"), TEXT("RelativeWidth"), &RelativeNative<2> }, { TEXT("GUIComponent"), TEXT("RelativeHeight"), &RelativeNative<3> },
			{ TEXT("GUIComponent"), TEXT("GetMenuPath"), &GetMenuPath }, { TEXT("GUIComponent"), TEXT("SpecialHit"), &SpecialHit },
			{ TEXT("GUIMultiComponent"), TEXT("InitializeControls"), &InitializeControls },
			{ TEXT("GUIMultiComponent"), TEXT("RemapComponents"), &RemapComponents },
			{ TEXT("GUIMultiComponent"), TEXT("FindComponentIndex"), &FindComponentIndex },
			{ TEXT("GUIController"), TEXT("GetMenuFont"), &GetMenuFont }, { TEXT("GUIController"), TEXT("GetStyle"), &GetStyle },
			{ TEXT("GUIController"), TEXT("GetCurrentRes"), &GetCurrentRes }, { TEXT("GUIController"), TEXT("SetRenderDevice"), &ReturnFalse },
			{ TEXT("GUIController"), TEXT("ResetDesigner"), &Quiet }, { TEXT("GUIController"), TEXT("ResetInput"), &Quiet },
			{ TEXT("GUIController"), TEXT("GetProfileList"), &Quiet }, { TEXT("GUIController"), TEXT("ResetKeyboard"), &Quiet },
			{ TEXT("GUIController"), TEXT("GetOGGList"), &Quiet }, { TEXT("GUIController"), TEXT("PlayInterfaceSound"), &Quiet },
			{ TEXT("GUIController"), TEXT("SetMoveControl"), &SetMoveControl }, { TEXT("GUIController"), TEXT("Profile"), &Quiet },
			{ TEXT("GUIController"), TEXT("GetDEMList"), &Quiet }, { TEXT("GUIController"), TEXT("GetDEMHeader"), &ReturnFalse },
			{ TEXT("GUIController"), TEXT("GetOwnageList"), &Quiet }, { TEXT("GUIController"), TEXT("SaveOwnageList"), &Quiet },
			{ TEXT("GUIController"), TEXT("GetWeaponList"), &Quiet }, { TEXT("GUIController"), TEXT("GetMapList"), &Quiet },
			{ TEXT("GUIController"), TEXT("LaunchURL"), &Quiet }, { TEXT("GUIController"), TEXT("CheckFirewall"), &ReturnFalse },
			{ TEXT("GUIController"), TEXT("AuthroizeFirewall"), &ReturnFalse }, { TEXT("GUIController"), TEXT("CheckForECE"), &ReturnFalse },
			{ TEXT("GUIController"), TEXT("GetMainMenuClass"), &GetMainMenuClass },
			{ TEXT("GUIStyles"), TEXT("Draw"), &StylesDraw }, { TEXT("GUIStyles"), TEXT("DrawText"), &StylesDrawText },
			{ TEXT("GUIStyles"), TEXT("TextSize"), &StylesTextSize },
			{ TEXT("GUIList"), TEXT("SortList"), &ListSortList },
			{ TEXT("GUIMultiColumnList"), TEXT("SortList"), &MCSortList }, { TEXT("GUIMultiColumnList"), TEXT("ChangeSortOrder"), &MCChangeSortOrder },
			{ TEXT("GUIMultiColumnList"), TEXT("UpdatedItem"), &MCMarkDirty }, { TEXT("GUIMultiColumnList"), TEXT("RemovedItem"), &MCMarkDirty },
			{ TEXT("GUIMultiColumnList"), TEXT("AddedItem"), &MCMarkDirty }, { TEXT("GUIMultiColumnList"), TEXT("GetListIndex"), &MCGetListIndex },
			{ TEXT("GUIToolTip"), TEXT("SetTip"), &ToolTipSetTip }, { TEXT("GUIScrollText"), TEXT("GetWordUnderCursor"), &EmptyStr },
			{ TEXT("GUITreeList"), TEXT("UpdateVisibleCount"), &Quiet }, { TEXT("GUISplitter"), TEXT("SplitterUpdatePositions"), &Quiet },
		};
		Register(Entries, UE_ARRAY_COUNT(Entries));
		USVM::GOnNewObject = [](FUSObject* Obj)
		{
			if (Obj && USGUI::IsGUIClass(Obj->Class))
			{
				USGUI::InstanceSubobjects(Obj);
			}
		};
	}
}

#endif // OPENUT2K4_USCRIPT_ENABLED
