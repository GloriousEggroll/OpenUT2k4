#pragma once

#include "CoreMinimal.h"
#include "UScriptCore.h"

class FUSWorld;

/**
 * The native half of UE2's menu system (XInterface / GUI2K4: GUIController, GUIComponent, GUIStyles and the widget
 * classes), so a mod's own menus run as their UnrealScript wrote them: UTComp's settings menu, a mod's voting or
 * configuration pages (Docs/UE2Net.md "Mod menus").
 *
 * What is script stays script: the menu stack (GUIController.OpenMenu / ReplaceMenu / CloseMenu / CreateMenu), the
 * styles registry (RegisterStyle), focus (SetFocus / LoseFocus / NextControl), every widget's InitComponent, value
 * handling and key handling (GUIEditBox.InternalOnKeyType, GUISlider.InternalCapturedMouseMove ...), the mod's
 * pages and their delegates. This file is what UE2's C++ did:
 *  - the natives of GUIController, GUIComponent, GUIMultiComponent, GUIStyles, GUIList, GUIMultiColumnList,
 *    GUIToolTip (position helpers ActualLeft / RelativeTop ..., timers, InitializeControls and the automated
 *    components, the style lookup, the style drawing);
 *  - instancing: a GUI object made with `new` gets its own copies of its class's component subobjects (UE2's
 *    "instanced" GUI classes), with delegates bound to the copies and their owners;
 *  - each frame: PreDraw (bounds of every component from WinLeft / WinTop / WinWidth / WinHeight, bBoundToParent,
 *    bScaleToParent, then the layout UE2 did natively: moComponents' caption and control, combo boxes, list boxes and
 *    their scroll bars, numeric edits, tab controls) and Draw (labels, images, buttons, check boxes, edit boxes,
 *    sliders, lists, multi-column lists, scroll text, tab buttons, section backgrounds, page backgrounds; the OnPreDraw
 *    / OnDraw / OnRender delegates around them) through the script Canvas bridge (UScriptCanvas.h);
 *  - input: the mouse (hover, press, release, click, double click, repeat clicks, captured moves, the wheel) and keys
 *    (OnKeyEvent / OnKeyType along the focus chain, Tab, Escape closes the top menu) as UE2's controller routed them.
 *
 * Not done: tool tips (their text is kept), context menus, drag and drop between lists, the designer, animations
 * (Animate / Resize jump to their end), interface sounds.
 */
class FUSGUI
{
public:
	explicit FUSGUI(FUSWorld* InWorld);
	~FUSGUI();
	FUSGUI(const FUSGUI&) = delete;
	FUSGUI& operator=(const FUSGUI&) = delete;

	/**
	 * Creates the controller (GUI2K4.UT2K4GUIController, as UT2004.ini's GUIController) for Viewport (Engine.Player:
	 * its GUIController, the controller's ViewportOwner / Master) and runs its InitializeController (the styles and fonts).
	 */
	bool Init(FUSObject* Viewport, FUSObject* InteractionMaster, FString& OutError);
	FUSObject* GetController() const { return Controller; }
	/** A menu is on the stack. */
	bool IsOpen() const;
	/** Class path of the top menu ("" when none). */
	FString TopMenuName() const;

	/** Timers, repeat clicks, the controller's RenderDelta. */
	void Tick(float DeltaSeconds);
	/** Lays out and draws the open menus (and the mouse cursor) on Canvas; the canvas backend must be set. */
	void Draw(FUSObject* CanvasObj, float SizeX, float SizeY);

	/** UE2 EInputKey / EInputAction (IST_Press 1, IST_Hold 2, IST_Release 3). True when the menus took it. */
	bool KeyEvent(uint8 Key, uint8 Action, float Delta = 0.f);
	/** A typed character (UE2 KeyType). */
	bool KeyType(uint8 Key, const FString& Unicode);
	/** Mouse position in canvas pixels. */
	void MouseMove(float X, float Y);
	float GetMouseX() const { return MouseX; }
	float GetMouseY() const { return MouseY; }
	/** Closes every menu (GUIController.CloseAll). */
	void CloseAll();

	FString Describe() const;
	/** Test hooks: the component tree of the top menu (class, name, caption, value, bounds), one line each. */
	FString DumpTop() const;
	/** Test hook: a left click at the centre of the top menu's component with that object name or caption. */
	bool ClickNamed(const FString& NameOrCaption);
	/** The GUI whose natives are running (one per client mirror). */
	static FUSGUI* Current();

	/** UE2 EInputKey codes used by the menus. */
	enum EKey : uint8
	{
		IK_LeftMouse = 0x01, IK_RightMouse = 0x02, IK_MiddleMouse = 0x04, IK_Backspace = 0x08, IK_Tab = 0x09, IK_Enter = 0x0D,
		IK_Shift = 0x10, IK_Ctrl = 0x11, IK_Alt = 0x12, IK_Escape = 0x1B, IK_Space = 0x20, IK_PageUp = 0x21, IK_PageDown = 0x22,
		IK_End = 0x23, IK_Home = 0x24, IK_Left = 0x25, IK_Up = 0x26, IK_Right = 0x27, IK_Down = 0x28, IK_Delete = 0x2E,
		IK_LShift = 0xA0, IK_RShift = 0xA1, IK_LControl = 0xA2, IK_RControl = 0xA3,
		IK_MouseWheelUp = 0xEC, IK_MouseWheelDown = 0xED,
	};
	enum EAction : uint8 { IST_None = 0, IST_Press = 1, IST_Hold = 2, IST_Release = 3, IST_Axis = 4 };

	// ---- used by the natives (UScriptGUI.cpp) ----
	struct FTimer
	{
		float Interval = 0.f;
		float Countdown = 0.f;
		bool bRepeat = false;
	};
	TMap<FUSObject*, FTimer> Timers;
	float ResX = 1024.f, ResY = 768.f;
	FUSObject* CanvasObj = nullptr;
	/** Plays an interface sound (a Sound object of the controller: ClickSound, EditSound ...); set by the host. */
	TFunction<void(FUSObject* Sound)> PlaySound;
	/** GUIController.PlayInterfaceSound(EClickSound): CS_Click, CS_Edit, CS_Up, CS_Down, CS_Drag, CS_Fade, CS_Hover. */
	void PlayInterfaceSound(uint8 ClickSound);

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
	FUSWorld* World = nullptr;
	FUSObject* Controller = nullptr;
	float MouseX = 0.f, MouseY = 0.f;
};

namespace USGUI
{
	/**
	 * UE2's instanced GUI classes: Obj (just made with `new`, or the controller) gets its own copies of the component
	 * subobjects its class defaults name (Begin Object ... End Object in defaultproperties), recursively, delegates of
	 * the copies bound to the copies or to the object that owns them.
	 */
	void InstanceSubobjects(FUSObject* Obj);
	/** True for XInterface.GUI and its subclasses (the classes UE2 instances subobjects for). */
	bool IsGUIClass(const FUSClass* Class);
}
