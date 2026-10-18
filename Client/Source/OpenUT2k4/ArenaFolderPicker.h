#pragma once

#include "CoreMinimal.h"

/**
 * Native "choose a folder" dialog for the game (the setup screen's "Browse..."), without the editor-only
 * DesktopPlatform module, so it works in Shipping builds:
 *   Linux:   SDL3's SDL_ShowOpenFolderDialog (the engine's SDL: xdg-desktop-portal FileChooser, which works on
 *            Wayland / X11, KDE / GNOME and inside Flatpak, else zenity), then kdialog / zenity started directly
 *            when SDL has no backend.
 *   Windows: IFileOpenDialog with FOS_PICKFOLDERS on a worker thread (COM apartment of its own).
 *   Mac:     NSOpenPanel (canChooseDirectories) on the Cocoa main thread, as a non-blocking panel.
 *
 * Never blocks the game thread: Open() returns at once and the callback runs later on the game thread.
 * A fullscreen game window is switched to a window while the dialog is open (Linux: both fullscreen modes, since
 * compositors keep fullscreen windows above new dialogs; Windows / Mac: exclusive fullscreen only) and restored
 * when it closes.
 */
namespace ArenaFolderPicker
{
	struct FResult
	{
		/** A folder was chosen (Path is set). */
		bool bChosen = false;
		/** The player closed the dialog without choosing. */
		bool bCancelled = false;
		/** No dialog could be shown at all (Error says why); the caller should offer typing the path instead. */
		bool bUnavailable = false;
		FString Path;
		FString Error;
		/** Which dialog was used ("xdg-desktop-portal / zenity (SDL)", "kdialog", "Windows", "macOS"), for logs. */
		FString Backend;
	};

	/**
	 * Opens the folder dialog. Title is the window title, StartDir the folder it opens in (may be empty).
	 * Returns false (and never calls Callback) when a dialog is already open. Game thread.
	 */
	OPENUT2K4_API bool Open(const FString& Title, const FString& StartDir, TFunction<void(const FResult&)> Callback);

	/** True while a dialog is open. */
	OPENUT2K4_API bool IsOpen();
}
