#pragma once

#include "CoreMinimal.h"

class AArenaPlayerController;
class APlayerState;
struct FKey;

/**
 * In-game chat, as UT2004 has it (Engine.Console state Typing, HUD.DrawTypingPrompt, PlayerController.Say / TeamSay):
 *
 *  - Talk (T) / TeamTalk (R, the stock DefUser.ini bindings; Y as well), open the typing line with "Say " / "TeamSay " typed;
 *    Enter runs the line as a console command (so any console command can be typed), Escape clears it and then
 *    closes it, Up / Down step through the last 16 lines, Left / Right / Home / End / Backspace / Delete edit.
 *    The prompt "(> text_" is drawn by the HUD one line under the console messages (AArenaHUD::DrawKillFeed).
 *  - the exec functions Say / TeamSay of AArenaPlayerController call Send:
 *      on a UT2004 server (Net/UE2NetPlay): ServerSay / ServerTeamSay; the server's TeamMessage shows the line
 *      hosting UT2004 clients (Net/UE2Server): relayed to them (UE2ServerHooks::Chat) and shown here
 *      offline: shown here (bots do not read chat)
 *  - ShowChat: a received line in the console message area, HUD.Message's "Name: text".
 *
 * Test runs never chat on public servers: with -saveddirsuffix=, -AutoMenuScript= or -unattended on the command
 * line, Send refuses a UT2004 server that is not on this machine or the local network.
 */
namespace ArenaChat
{
	/** PlayerController.Say / TeamSay: Text is cut to 128 characters. */
	OPENUT2K4_API void Send(AArenaPlayerController* PC, const FString& Text, bool bTeam);
	/**
	 * HudBase.Message(PRI, Text, 'Say' / 'TeamSay'): "Name: text" / "Name(location): text" in the console area of PC,
	 * in the stock colours. Location overrides the speaker's location for team chat (a UT2004 server's PRI).
	 */
	OPENUT2K4_API void ShowChat(AArenaPlayerController* PC, const APlayerState* From, const FString& Text, bool bTeam, const FString* Location = nullptr);
	/** PlayerReplicationInfo.GetLocationName for a local player state: volume / zone name, "Dead", "Spectating". */
	OPENUT2K4_API FString GetLocationName(const APlayerState* PS);

	/** Console.Talk / TeamTalk: opens the typing line for PC with "Say " / "TeamSay ". */
	OPENUT2K4_API void OpenTyping(AArenaPlayerController* PC, bool bTeam);
	OPENUT2K4_API void CloseTyping();
	OPENUT2K4_API bool IsTyping();
	/** The prompt as HUD.DrawTypingPrompt draws it: "(> " + text with the cursor. */
	OPENUT2K4_API FString GetPrompt();

	/** Typing line input (the focus widget's events; test hooks feed the same). True when used. */
	OPENUT2K4_API bool HandleKey(const FKey& Key);
	OPENUT2K4_API void HandleChar(TCHAR Char);

	/** True when the address (host or host:port) is this machine or a private network (test chat is allowed there). */
	OPENUT2K4_API bool IsLocalAddress(const FString& Address);
}
