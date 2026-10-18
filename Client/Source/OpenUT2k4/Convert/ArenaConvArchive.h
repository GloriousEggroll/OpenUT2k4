#pragma once

#include "CoreMinimal.h"

/**
 * Archives players download custom content in (ArenaUserContent): UE2 umod installers (.umod / .ut2mod) and .zip.
 * Both readers are small and self-contained (zip uses the engine's zlib; the engine's own zip reader lives in an
 * editor-only module). 7-Zip and RAR archives are not read: the player is asked to unpack them.
 *
 * Only the files the game can use are extracted, sorted by extension into the UE2 folder layout under OutDir
 * (Maps/, Textures/, StaticMeshes/, Sounds/, Music/, System/, Animations/, KarmaData/); archive paths are never
 * used as output paths (no path traversal). A .umod inside a .zip is unpacked too.
 */
namespace ArenaConvArchive
{
	struct FResult
	{
		bool bOk = false;
		/** Plain-language reason when bOk is false. */
		FString Error;
		/** Extracted files (absolute paths under OutDir). */
		TArray<FString> Files;
		/** Files in the archive that were not extracted (readme, unknown types). */
		int32 Skipped = 0;
	};

	/** UE2 folder for a file by its extension ("Maps", "Textures", ...); empty for files the game does not use. */
	FString FolderForExtension(const FString& Extension);

	/** True for .umod / .ut2mod / .zip. */
	bool IsSupportedArchive(const FString& Path);
	/** True for archive formats the game cannot unpack (.7z, .rar). */
	bool IsUnsupportedArchive(const FString& Path);

	/** Extracts a .umod / .ut2mod / .zip into OutDir. Cancel (optional) stops between files. */
	FResult Extract(const FString& Archive, const FString& OutDir, const TAtomic<bool>* Cancel = nullptr);

	/** UE2 umod installer: header check + file table (exposed for tests). */
	FResult ExtractUmod(const FString& Archive, const FString& OutDir, const TAtomic<bool>* Cancel = nullptr);
	FResult ExtractZip(const FString& Archive, const FString& OutDir, const TAtomic<bool>* Cancel = nullptr);
}
