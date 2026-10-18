#include "ArenaServerConfig.h"
#include "../OpenUT2k4.h"
#include "../ArenaContent.h"
#include "../ArenaLevelBuilder.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace ArenaServerConfigLocal
{
	struct FGameType
	{
		const TCHAR* Mode;
		const TCHAR* Class;
		/** The stock map list section of UT2004.ini / Default.ini. */
		const TCHAR* MapList;
	};
	static const FGameType GameTypes[] = {
		{ TEXT("DM"), TEXT("XGame.xDeathMatch"), TEXT("XInterface.MapListDeathMatch") },
		{ TEXT("TDM"), TEXT("XGame.xTeamGame"), TEXT("XInterface.MapListTeamDeathMatch") },
		{ TEXT("CTF"), TEXT("XGame.xCTFGame"), TEXT("XInterface.MapListCaptureTheFlag") },
		{ TEXT("BR"), TEXT("XGame.xBombingRun"), TEXT("XInterface.MapListBombingRun") },
		{ TEXT("DOM"), TEXT("XGame.xDoubleDom"), TEXT("XInterface.MapListDoubleDomination") },
		{ TEXT("LMS"), TEXT("BonusPack.xLastManStandingGame"), TEXT("BonusPack.MapListLastManStanding") },
		{ TEXT("MUT"), TEXT("BonusPack.xMutantGame"), TEXT("BonusPack.MapListMutant") },
		{ TEXT("ONS"), TEXT("Onslaught.ONSOnslaughtGame"), TEXT("Onslaught.ONSMapListOnslaught") },
		{ TEXT("AS"), TEXT("UT2k4Assault.ASGameInfo"), TEXT("UT2k4Assault.ASMapList") },
		{ TEXT("INV"), TEXT("SkaarjPack.Invasion"), TEXT("SkaarjPack.MapListSkaarjInvasion") },
		// Vehicle CTF: stock class, CTF rules on VCTF- maps (no stock maps, no map list). Played as CTF here; vehicles
		// are not replicated to UT2004 clients yet (Docs/UE2Net.md)
		{ TEXT("CTF"), TEXT("XGame.xVehicleCTFGame"), TEXT("") },
	};

	const FGameType* FindByMode(const FString& Mode)
	{
		for (const FGameType& G : GameTypes)
		{
			if (Mode.Equals(G.Mode, ESearchCase::IgnoreCase))
			{
				return &G;
			}
		}
		return nullptr;
	}

	const FGameType* FindByClass(const FString& Class)
	{
		FString Short = Class;
		Class.Split(TEXT("."), nullptr, &Short, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		for (const FGameType& G : GameTypes)
		{
			FString GShort;
			FString(G.Class).Split(TEXT("."), nullptr, &GShort);
			if (Class.Equals(G.Class, ESearchCase::IgnoreCase) || Short.Equals(GShort, ESearchCase::IgnoreCase))
			{
				return &G;
			}
		}
		return nullptr;
	}

	FString DecodeOption(const FString& In)
	{
		FString Out;
		for (int32 i = 0; i < In.Len(); ++i)
		{
			if (In[i] == '%' && i + 2 < In.Len() && FChar::IsHexDigit(In[i + 1]) && FChar::IsHexDigit(In[i + 2]))
			{
				Out.AppendChar(TCHAR(FParse::HexNumber(*In.Mid(i + 1, 2))));
				i += 2;
			}
			else
			{
				Out.AppendChar(In[i]);
			}
		}
		return Out;
	}

	/** "DM-Rankin.ut2" / "DM-Rankin?LinkSetup=Default" / "dm-rankin" -> "DM-Rankin" as the content cache spells it. */
	FString CleanMapName(const FString& In)
	{
		FString Name = In.TrimStartAndEnd().TrimQuotes();
		int32 Q = INDEX_NONE;
		if (Name.FindChar('?', Q))
		{
			Name.LeftInline(Q);
		}
		if (Name.EndsWith(TEXT(".ut2"), ESearchCase::IgnoreCase))
		{
			Name.LeftChopInline(4);
		}
		FArenaLayoutInfo Info;
		if (!Name.IsEmpty() && AArenaLevelBuilder::FindLayout(Name, Info) && !Info.MapName.IsEmpty())
		{
			return Info.MapName;
		}
		return Name;
	}

	/** "-Name=Value" switch of the command line, matched as a whole token (FParse::Value would also find "-UE2Port=" for "port="). */
	bool SwitchValue(const TCHAR* CommandLine, const TCHAR* Name, FString& Out)
	{
		const FString Prefix = FString(TEXT("-")) + Name + TEXT("=");
		const TCHAR* Cursor = CommandLine;
		FString Token;
		bool bFound = false;
		while (FParse::Token(Cursor, Token, false))
		{
			if (Token.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				Out = Token.Mid(Prefix.Len()).TrimQuotes();
				bFound = true;
			}
		}
		return bFound;
	}

	bool SwitchValue(const TCHAR* CommandLine, const TCHAR* Name, int32& Out)
	{
		FString V;
		if (SwitchValue(CommandLine, Name, V) && V.IsNumeric())
		{
			Out = FCString::Atoi(*V);
			return true;
		}
		return false;
	}

	/** Splits "A,B , C" lists. */
	TArray<FString> SplitList(const FString& In)
	{
		TArray<FString> Parts;
		In.ParseIntoArray(Parts, TEXT(","), true);
		for (FString& P : Parts)
		{
			P.TrimStartAndEndInline();
		}
		Parts.RemoveAll([](const FString& P) { return P.IsEmpty(); });
		return Parts;
	}
}


// ---------------------------------------------------------------------------------------------
// FArenaIniFile

bool FArenaIniFile::Load(const FString& InPath)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *InPath))
	{
		return false;
	}
	Path = InPath;
	return LoadFromString(Text);
}

bool FArenaIniFile::LoadFromString(const FString& Text)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	TArray<FEntry>* Current = nullptr;
	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line[0] == ';' || Line[0] == '#')
		{
			continue;
		}
		if (Line[0] == '[')
		{
			int32 End = INDEX_NONE;
			if (Line.FindChar(']', End))
			{
				Current = &Sections.FindOrAdd(Line.Mid(1, End - 1).TrimStartAndEnd().ToLower());
			}
			continue;
		}
		FString Key, Value;
		if (!Current || !Line.Split(TEXT("="), &Key, &Value))
		{
			continue;
		}
		Key.TrimStartAndEndInline();
		// UE ini array prefixes (+Key= / .Key=) read as plain repeated keys; "Key[3]=" as "Key"
		Key.RemoveFromStart(TEXT("+"));
		Key.RemoveFromStart(TEXT("."));
		int32 Bracket = INDEX_NONE;
		if (Key.FindChar('[', Bracket))
		{
			Key.LeftInline(Bracket);
		}
		Value.TrimStartAndEndInline();
		if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}
		Current->Add({ Key.ToLower(), Value });
	}
	bLoaded = true;
	return true;
}

bool FArenaIniFile::HasSection(const FString& Section) const
{
	return Sections.Contains(Section.ToLower());
}

bool FArenaIniFile::Get(const FString& Section, const FString& Key, FString& Out) const
{
	const TArray<FEntry>* S = Sections.Find(Section.ToLower());
	if (!S)
	{
		return false;
	}
	const FString K = Key.ToLower();
	for (int32 i = S->Num() - 1; i >= 0; --i)
	{
		if ((*S)[i].Key == K)
		{
			Out = (*S)[i].Value;
			return true;
		}
	}
	return false;
}

TArray<FString> FArenaIniFile::GetAll(const FString& Section, const FString& Key) const
{
	TArray<FString> Out;
	if (const TArray<FEntry>* S = Sections.Find(Section.ToLower()))
	{
		const FString K = Key.ToLower();
		for (const FEntry& E : *S)
		{
			if (E.Key == K)
			{
				Out.Add(E.Value);
			}
		}
	}
	return Out;
}

bool FArenaIniFile::ParseBool(const FString& Value, bool bDefault)
{
	const FString V = Value.TrimStartAndEnd();
	if (V.Equals(TEXT("true"), ESearchCase::IgnoreCase) || V.Equals(TEXT("yes"), ESearchCase::IgnoreCase) || V.Equals(TEXT("on"), ESearchCase::IgnoreCase) || V == TEXT("1"))
	{
		return true;
	}
	if (V.Equals(TEXT("false"), ESearchCase::IgnoreCase) || V.Equals(TEXT("no"), ESearchCase::IgnoreCase) || V.Equals(TEXT("off"), ESearchCase::IgnoreCase) || V == TEXT("0"))
	{
		return false;
	}
	return bDefault;
}

// ---------------------------------------------------------------------------------------------
// FArenaServerConfig

FString FArenaServerConfig::ModeIdFromGameClass(const FString& GameClass)
{
	if (const ArenaServerConfigLocal::FGameType* G = ArenaServerConfigLocal::FindByClass(GameClass))
	{
		return G->Mode;
	}
	if (const ArenaServerConfigLocal::FGameType* G = ArenaServerConfigLocal::FindByMode(GameClass))
	{
		return G->Mode;
	}
	return FString();
}

FString FArenaServerConfig::GameClassFromModeId(const FString& ModeId)
{
	const ArenaServerConfigLocal::FGameType* G = ArenaServerConfigLocal::FindByMode(ModeId);
	return G ? FString(G->Class) : FString();
}

FString FArenaServerConfig::StockMapListSection(const FString& ModeId)
{
	const ArenaServerConfigLocal::FGameType* G = ArenaServerConfigLocal::FindByMode(ModeId);
	return G ? FString(G->MapList) : FString();
}

FString FArenaServerConfig::EncodeOption(const FString& In)
{
	FString Out;
	for (const TCHAR C : In)
	{
		if (C == '%' || C == '?' || C == '=' || C == '#' || C == '&' || C == ' ' || C == '"' || C == '\'' || C == '/' || C == '\\' || C == ',' || C == '|' || C < 32)
		{
			Out += FString::Printf(TEXT("%%%02X"), uint32(C) & 0xFF);
		}
		else
		{
			Out.AppendChar(C);
		}
	}
	return Out;
}

FString FArenaServerConfig::FindDefaultIni()
{
	// packaged: <package root>/Server/OpenUT2k4Server.ini (Server/build-*: Linux / Windows <root>/OpenUT2k4/Binaries/<Platform>/,
	// Mac <root>/OpenUT2k4Server.app/Contents/MacOS/); a source tree or the editor binary: Server/Package/OpenUT2k4Server.ini
	const TArray<FString> Candidates = {
		FString(FPlatformProcess::BaseDir()) / TEXT("../../../Server/OpenUT2k4Server.ini"),
		FPaths::RootDir() / TEXT("Server/OpenUT2k4Server.ini"),
		FPaths::ProjectDir() / TEXT("../Server/Package/OpenUT2k4Server.ini"),
	};
	for (const FString& C : Candidates)
	{
		const FString Full = FPaths::ConvertRelativePathToFull(C);
		if (IFileManager::Get().FileExists(*Full))
		{
			return Full;
		}
	}
	return FString();
}

bool FArenaServerConfig::Load(const TCHAR* CommandLine, FString& OutError)
{
	// ---- the command line: UCC style tokens ("server", the URL, ini=, log=) and switches ----
	FString Url;
	FString IniOverride;
	{
		const TCHAR* Cursor = CommandLine;
		FString Token;
		while (FParse::Token(Cursor, Token, false))
		{
			if (Token.IsEmpty() || Token[0] == '-')
			{
				continue;
			}
			if (Token.Equals(TEXT("server"), ESearchCase::IgnoreCase) || Token.Equals(TEXT("OpenUT2k4"), ESearchCase::IgnoreCase) || Token.EndsWith(TEXT(".uproject")))
			{
				continue;
			}
			if (Token.StartsWith(TEXT("ini="), ESearchCase::IgnoreCase))
			{
				IniOverride = Token.Mid(4);
				continue;
			}
			if (Token.Contains(TEXT("=")) && !Token.Contains(TEXT("?")))
			{
				continue; // log=server.log (the engine reads LOG= itself), userini= ...
			}
			if (Url.IsEmpty())
			{
				Url = Token;
			}
		}
	}
	ArenaServerConfigLocal::SwitchValue(CommandLine, TEXT("ServerConfig"), IniOverride);

	// ---- the ini layers ----
	Layers.Reset();
	const FString DefaultIni = FindDefaultIni();
	if (!DefaultIni.IsEmpty())
	{
		FArenaIniFile& F = Layers.AddDefaulted_GetRef();
		F.Load(DefaultIni);
	}
	if (!IniOverride.IsEmpty())
	{
		const FString Full = FPaths::ConvertRelativePathToFull(IniOverride);
		FArenaIniFile& F = Layers.AddDefaulted_GetRef();
		if (!F.Load(Full))
		{
			OutError = FString::Printf(TEXT("cannot read the server ini %s"), *Full);
			return false;
		}
	}
	FString Profile;
	if (ArenaServerConfigLocal::SwitchValue(CommandLine, TEXT("ServerProfile"), Profile) && !Profile.IsEmpty())
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(Profile);
		if (!IFileManager::Get().DirectoryExists(*Dir))
		{
			OutError = FString::Printf(TEXT("the server profile folder %s does not exist"), *Dir);
			return false;
		}
		// Server.ini (our layout) when the profile has one, else a plain UT2004.ini
		bool bAny = false;
		const bool bHasServerIni = IFileManager::Get().FileExists(*(Dir / TEXT("Server.ini")));
		for (const TCHAR* Name : { TEXT("UT2004.ini"), TEXT("Server.ini") })
		{
			if (bHasServerIni && FCString::Stricmp(Name, TEXT("UT2004.ini")) == 0)
			{
				continue;
			}
			FArenaIniFile F;
			if (F.Load(Dir / Name))
			{
				Layers.Add(MoveTemp(F));
				bAny = true;
			}
		}
		if (!bAny)
		{
			OutError = FString::Printf(TEXT("the server profile folder %s has no Server.ini"), *Dir);
			return false;
		}
	}

	// ---- the game class first: per-game sections depend on it ----
	for (const FArenaIniFile& F : Layers)
	{
		FString V;
		if (F.Get(TEXT("OpenUT2k4.Server"), TEXT("GameType"), V) && !V.IsEmpty())
		{
			GameClass = V;
		}
	}
	{
		const FString Game = UrlOption(Url, TEXT("game"));
		if (!Game.IsEmpty())
		{
			GameClass = Game;
		}
	}
	ModeId = ModeIdFromGameClass(GameClass);
	if (ModeId.IsEmpty())
	{
		Warnings.Add(FString::Printf(TEXT("unknown game type %s: playing Deathmatch"), *GameClass));
		ModeId = TEXT("DM");
		GameClass = TEXT("XGame.xDeathMatch");
	}
	else if (const ArenaServerConfigLocal::FGameType* G = ArenaServerConfigLocal::FindByClass(GameClass))
	{
		GameClass = G->Class; // "xCTFGame" -> "XGame.xCTFGame"
	}
	else if (const ArenaServerConfigLocal::FGameType* M = ArenaServerConfigLocal::FindByMode(ModeId))
	{
		GameClass = M->Class; // our id ("CTF") given as the game type
	}
	if (GameClass.Equals(TEXT("XGame.xVehicleCTFGame"), ESearchCase::IgnoreCase))
	{
		Warnings.Add(TEXT("XGame.xVehicleCTFGame is played with the CTF rules; vehicles are not replicated to UT2004 clients yet"));
	}

	for (const FArenaIniFile& F : Layers)
	{
		ApplyIni(F);
		LoadedFiles.Add(F.GetPath());
	}
	ApplyURL(Url);

	// ---- switches ----
	ArenaServerConfigLocal::SwitchValue(CommandLine, TEXT("port"), Port);
	ArenaServerConfigLocal::SwitchValue(CommandLine, TEXT("multihome"), BindAddress);
	if (FParse::Param(CommandLine, TEXT("lanplay")))
	{
		bLANServer = true;
	}
	if (FParse::Param(CommandLine, TEXT("NoUplink")))
	{
		bUplink = false;
	}
	ArenaServerConfigLocal::SwitchValue(CommandLine, TEXT("ServerName"), ServerName);
	ArenaServerConfigLocal::SwitchValue(CommandLine, TEXT("AdminConsolePort"), AdminConsolePort);
	Port = FMath::Clamp(Port, 1, 65534);
	MaxPlayers = FMath::Clamp(MaxPlayers, 1, 64);
	MaxSpectators = FMath::Clamp(MaxSpectators, 0, 32);
	MinPlayers = FMath::Clamp(MinPlayers, 0, 64);
	BotSkill = FMath::Clamp(BotSkill, 0, 7);
	TickRate = FMath::Clamp(TickRate, 10, 120);

	for (const FString& Actor : ServerActors)
	{
		// the stock server actors and what stands in for them here
		if (!Actor.Equals(TEXT("IpDrv.MasterServerUplink"), ESearchCase::IgnoreCase) && !Actor.Equals(TEXT("UWeb.WebServer"), ESearchCase::IgnoreCase)
			&& !Actor.Equals(TEXT("IpDrv.UdpBeacon"), ESearchCase::IgnoreCase) && !Actor.StartsWith(TEXT("IpServer."), ESearchCase::IgnoreCase))
		{
			Warnings.Add(FString::Printf(TEXT("ServerActors=%s is not supported (only the stock uplink, beacon, query and web server actors have native equivalents)"), *Actor));
		}
	}
	return true;
}

FString FArenaServerConfig::UrlOption(const FString& Url, const FString& Key)
{
	TArray<FString> Parts;
	Url.ParseIntoArray(Parts, TEXT("?"), true);
	FString Result;
	for (int32 i = 1; i < Parts.Num(); ++i)
	{
		FString K, V;
		if (Parts[i].Split(TEXT("="), &K, &V) && K.Equals(Key, ESearchCase::IgnoreCase))
		{
			Result = ArenaServerConfigLocal::DecodeOption(V);
		}
	}
	return Result;
}

void FArenaServerConfig::ApplyGameSection(const FArenaIniFile& Ini)
{
	// UT2004 keeps game settings in the game class's section, its parents' and [Engine.GameInfo]
	TArray<FString> SectionsToRead = { TEXT("Engine.GameInfo"), TEXT("UnrealGame.DeathMatch"), TEXT("UnrealGame.TeamGame") };
	SectionsToRead.Add(GameClass);
	for (const FString& Section : SectionsToRead)
	{
		FString V;
		if (Section == TEXT("UnrealGame.TeamGame") && !(ModeId == TEXT("TDM") || ModeId == TEXT("CTF") || ModeId == TEXT("BR") || ModeId == TEXT("DOM") || ModeId == TEXT("ONS") || ModeId == TEXT("AS")))
		{
			continue;
		}
		if (Ini.Get(Section, TEXT("MaxPlayers"), V)) MaxPlayers = FCString::Atoi(*V);
		if (Ini.Get(Section, TEXT("MaxSpectators"), V)) MaxSpectators = FCString::Atoi(*V);
		if (Ini.Get(Section, TEXT("TimeLimit"), V)) TimeLimit = FCString::Atoi(*V);
		if (Ini.Get(Section, TEXT("GoalScore"), V)) GoalScore = FCString::Atoi(*V);
		if (Ini.Get(Section, TEXT("MinPlayers"), V)) MinPlayers = FCString::Atoi(*V);
		if (Ini.Get(Section, TEXT("GameDifficulty"), V)) BotSkill = FMath::RoundToInt(FCString::Atof(*V));
	}
}

void FArenaServerConfig::ApplyIni(const FArenaIniFile& Ini)
{
	FString V;
	const TCHAR* S = TEXT("OpenUT2k4.Server");
	if (Ini.Get(S, TEXT("Map"), V)) Map = V;
	{
		const TArray<FString> List = Ini.GetAll(S, TEXT("Maps"));
		if (List.Num() > 0)
		{
			Maps.Reset();
			for (const FString& M : List)
			{
				Maps.Append(ArenaServerConfigLocal::SplitList(M));
			}
			MapsSource = Ini.GetPath();
		}
	}
	if (Ini.Get(S, TEXT("Mutators"), V)) Mutators = ArenaServerConfigLocal::SplitList(V);
	if (Ini.Get(S, TEXT("BotsWhenEmpty"), V)) bBotsWhenEmpty = FArenaIniFile::ParseBool(V, bBotsWhenEmpty);
	if (Ini.Get(S, TEXT("ExtraOptions"), V)) ExtraOptions = V;
	if (Ini.Get(S, TEXT("TickRate"), V)) TickRate = FCString::Atoi(*V);
	if (Ini.Get(S, TEXT("UseServerPackages"), V)) bUseServerPackages = FArenaIniFile::ParseBool(V, bUseServerPackages);
	if (Ini.Get(S, TEXT("LANServer"), V)) bLANServer = FArenaIniFile::ParseBool(V, bLANServer);

	ApplyGameSection(Ini);
	// [OpenUT2k4.Server] wins over the UT2004 sections of the same file
	if (Ini.Get(S, TEXT("MinPlayers"), V)) MinPlayers = FCString::Atoi(*V);
	if (Ini.Get(S, TEXT("BotSkill"), V)) BotSkill = FCString::Atoi(*V);
	if (Ini.Get(S, TEXT("TimeLimit"), V)) TimeLimit = FCString::Atoi(*V);
	if (Ini.Get(S, TEXT("GoalScore"), V)) GoalScore = FCString::Atoi(*V);

	if (Ini.Get(TEXT("URL"), TEXT("Port"), V)) Port = FCString::Atoi(*V);
	if (Ini.Get(TEXT("IpDrv.TcpNetDriver"), TEXT("NetServerMaxTickRate"), V)) TickRate = FCString::Atoi(*V);
	if (Ini.Get(TEXT("IpDrv.MasterServerUplink"), TEXT("DoUplink"), V)) bUplink = FArenaIniFile::ParseBool(V, bUplink);

	if (Ini.Get(TEXT("Engine.AccessControl"), TEXT("AdminPassword"), V)) AdminPassword = V;
	if (Ini.Get(TEXT("Engine.AccessControl"), TEXT("GamePassword"), V)) GamePassword = V;
	for (const FString& Ban : Ini.GetAll(TEXT("Engine.AccessControl"), TEXT("BannedIDs")))
	{
		// UT2004: "<id> <player name>"
		FString Id = Ban.TrimStartAndEnd(), Name;
		Ban.TrimStartAndEnd().Split(TEXT(" "), &Id, &Name);
		if (!Id.IsEmpty())
		{
			BannedIDs.AddUnique(Id + TEXT("|") + Name.TrimStartAndEnd());
		}
	}

	const TCHAR* GRI = TEXT("Engine.GameReplicationInfo");
	if (Ini.Get(GRI, TEXT("ServerName"), V)) ServerName = V;
	if (Ini.Get(GRI, TEXT("ShortName"), V)) ShortName = V;
	if (Ini.Get(GRI, TEXT("AdminName"), V)) AdminName = V;
	if (Ini.Get(GRI, TEXT("AdminEmail"), V)) AdminEmail = V;
	{
		TArray<FString> Lines;
		if (Ini.Get(GRI, TEXT("MessageOfTheDay"), V) && !V.IsEmpty())
		{
			Lines.Add(V);
		}
		for (int32 i = 1; i <= 4; ++i)
		{
			if (Ini.Get(GRI, FString::Printf(TEXT("MOTDLine%d"), i), V) && !V.IsEmpty())
			{
				Lines.Add(V);
			}
		}
		if (Lines.Num() > 0)
		{
			MOTD = Lines;
		}
	}

	{
		const TArray<FString> Actors = Ini.GetAll(TEXT("Engine.GameEngine"), TEXT("ServerActors"));
		if (Actors.Num() > 0)
		{
			ServerActors = Actors;
		}
		const TArray<FString> Packages = Ini.GetAll(TEXT("Engine.GameEngine"), TEXT("ServerPackages"));
		if (Packages.Num() > 0)
		{
			ServerPackages = Packages;
		}
	}

	const TCHAR* Admin = TEXT("OpenUT2k4.AdminConsole");
	if (Ini.Get(Admin, TEXT("bEnabled"), V)) bAdminConsole = FArenaIniFile::ParseBool(V, bAdminConsole);
	if (Ini.Get(Admin, TEXT("ListenAddress"), V)) AdminConsoleAddress = V;
	if (Ini.Get(Admin, TEXT("Port"), V)) AdminConsolePort = FCString::Atoi(*V);

	// relative paths in an ini are relative to that ini's folder
	const FString Base = FPaths::GetPath(Ini.GetPath());
	const auto PathValue = [&Base](const FString& In)
	{
		FString P = In;
		if (P.StartsWith(TEXT("~/")))
		{
			P = FString(FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"))) / P.Mid(2);
		}
		return FPaths::IsRelative(P) && !Base.IsEmpty() ? FPaths::ConvertRelativePathToFull(Base / P) : P;
	};
	const TCHAR* Paths = TEXT("OpenUT2k4.Paths");
	if (Ini.Get(Paths, TEXT("ContentCache"), V) && !V.IsEmpty()) ContentCache = PathValue(V);
	if (Ini.Get(Paths, TEXT("UT2004Dir"), V) && !V.IsEmpty()) UT2004Dir = PathValue(V);
	if (Ini.Get(Paths, TEXT("UserContent"), V) && !V.IsEmpty()) UserContent = PathValue(V);
}

void FArenaServerConfig::ApplyURL(const FString& Url)
{
	if (Url.IsEmpty())
	{
		return;
	}
	TArray<FString> Parts;
	Url.ParseIntoArray(Parts, TEXT("?"), false);
	if (Parts.Num() > 0 && !Parts[0].IsEmpty())
	{
		Map = Parts[0];
	}
	for (int32 i = 1; i < Parts.Num(); ++i)
	{
		FString K, V;
		if (!Parts[i].Split(TEXT("="), &K, &V))
		{
			K = Parts[i];
			V = TEXT("1");
		}
		V = ArenaServerConfigLocal::DecodeOption(V);
		if (K.Equals(TEXT("game"), ESearchCase::IgnoreCase)) continue; // read first (Load)
		else if (K.Equals(TEXT("MaxPlayers"), ESearchCase::IgnoreCase)) MaxPlayers = FCString::Atoi(*V);
		else if (K.Equals(TEXT("MaxSpectators"), ESearchCase::IgnoreCase)) MaxSpectators = FCString::Atoi(*V);
		else if (K.Equals(TEXT("MinPlayers"), ESearchCase::IgnoreCase) || K.Equals(TEXT("NumBots"), ESearchCase::IgnoreCase) || K.Equals(TEXT("Bots"), ESearchCase::IgnoreCase)) MinPlayers = FCString::Atoi(*V);
		else if (K.Equals(TEXT("GoalScore"), ESearchCase::IgnoreCase) || K.Equals(TEXT("Goal"), ESearchCase::IgnoreCase)) GoalScore = FCString::Atoi(*V);
		else if (K.Equals(TEXT("TimeLimit"), ESearchCase::IgnoreCase) || K.Equals(TEXT("Time"), ESearchCase::IgnoreCase)) TimeLimit = FCString::Atoi(*V);
		else if (K.Equals(TEXT("GameDifficulty"), ESearchCase::IgnoreCase) || K.Equals(TEXT("Difficulty"), ESearchCase::IgnoreCase) || K.Equals(TEXT("Skill"), ESearchCase::IgnoreCase)) BotSkill = FMath::RoundToInt(FCString::Atof(*V));
		else if (K.Equals(TEXT("Mutator"), ESearchCase::IgnoreCase)) Mutators = ArenaServerConfigLocal::SplitList(V);
		else if (K.Equals(TEXT("GamePassword"), ESearchCase::IgnoreCase)) GamePassword = V;
		else if (K.Equals(TEXT("AdminPassword"), ESearchCase::IgnoreCase)) AdminPassword = V;
		else if (K.Equals(TEXT("AdminName"), ESearchCase::IgnoreCase)) AdminName = V;
		else if (K.Equals(TEXT("ServerName"), ESearchCase::IgnoreCase)) ServerName = V;
		else if (K.Equals(TEXT("Port"), ESearchCase::IgnoreCase)) Port = FCString::Atoi(*V);
		else if (K.Equals(TEXT("LANServer"), ESearchCase::IgnoreCase)) bLANServer = FArenaIniFile::ParseBool(V, true);
		else if (K.Equals(TEXT("MapList"), ESearchCase::IgnoreCase)) { Maps = ArenaServerConfigLocal::SplitList(V); MapsSource = TEXT("command line"); }
		else
		{
			// anything else goes to the game as it is (?Translocator=1, ?WeaponStay=...)
			ExtraOptions += TEXT("?") + Parts[i];
		}
	}
}

void FArenaServerConfig::ResolveMaps()
{
	if (Maps.Num() == 0)
	{
		// the install's own stock map list for this game type (the dedicated server's UT2004.ini / Default.ini)
		const FString Install = UT2004Dir.IsEmpty() ? ArenaContent::FindUT2004Install() : UT2004Dir;
		const FString Section = StockMapListSection(ModeId);
		for (const TCHAR* Name : { TEXT("UT2004.ini"), TEXT("Default.ini") })
		{
			if (Install.IsEmpty() || Section.IsEmpty())
			{
				break;
			}
			// System/ may be spelt differently on case-sensitive file systems
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *Install, Name, true, false, false);
			Found.RemoveAll([&Install](const FString& F) { return !FPaths::GetPath(F).Equals(Install / TEXT("System"), ESearchCase::IgnoreCase); });
			FArenaIniFile Ini;
			if (Found.Num() > 0 && Ini.Load(Found[0]))
			{
				for (const FString& M : Ini.GetAll(Section, TEXT("Maps")))
				{
					Maps.Add(M);
				}
				if (Maps.Num() > 0)
				{
					MapsSource = Found[0] + TEXT(" [") + Section + TEXT("]");
					break;
				}
			}
		}
	}
	// names as the content cache spells them; only maps that can be played
	TArray<FString> Playable;
	TArray<FString> Missing;
	for (const FString& M : Maps)
	{
		const FString Name = ArenaServerConfigLocal::CleanMapName(M);
		FArenaLayoutInfo Info;
		if (AArenaLevelBuilder::FindLayout(Name, Info))
		{
			Playable.AddUnique(Name);
		}
		else if (!Name.IsEmpty())
		{
			Missing.Add(Name);
		}
	}
	if (Missing.Num() > 0)
	{
		Warnings.Add(FString::Printf(TEXT("maps not in the content cache, left out of the rotation: %s"), *FString::Join(Missing, TEXT(", "))));
	}
	Maps = Playable;
	if (!Map.IsEmpty())
	{
		Map = ArenaServerConfigLocal::CleanMapName(Map);
	}
	if (Maps.Num() == 0)
	{
		// every converted map of the game type's prefix (what UE2ServerHooks::TravelToNextMap rotates through)
		FString Prefix = ModeId;
		if (ModeId == TEXT("TDM") || ModeId == TEXT("LMS") || ModeId == TEXT("MUT") || ModeId == TEXT("INV"))
		{
			Prefix = TEXT("DM");
		}
		else if (GameClass.Equals(TEXT("XGame.xVehicleCTFGame"), ESearchCase::IgnoreCase))
		{
			Prefix = TEXT("VCTF");
		}
		for (const FArenaLayoutInfo& L : AArenaLevelBuilder::GetLayouts())
		{
			if ((!L.bCustom || Prefix == TEXT("VCTF")) && L.GameTypePrefix.Equals(Prefix, ESearchCase::IgnoreCase))
			{
				Maps.AddUnique(L.MapName);
			}
		}
		Maps.Sort();
		MapsSource = TEXT("every converted map of the game type");
	}
	FArenaLayoutInfo MapInfo;
	if (!Map.IsEmpty() && !AArenaLevelBuilder::FindLayout(Map, MapInfo))
	{
		Warnings.Add(FString::Printf(TEXT("map %s is not in the content cache: starting with the rotation's first map"), *Map));
		Map.Reset();
	}
	if (Map.IsEmpty() && Maps.Num() > 0)
	{
		Map = Maps[0];
	}
}

FString FArenaServerConfig::BuildMatchURL(const FString& MapName) const
{
	const FString Layout = AArenaLevelBuilder::MakeLayoutId(MapName);
	FString Url = AArenaLevelBuilder::GetLevelForLayout(Layout);
	Url += FString::Printf(TEXT("?Mode=%s?Layout=%s"), *ModeId, *Layout);
	// bots: the dedicated server adds them as players join (MinPlayers); none to start with unless they play alone
	Url += FString::Printf(TEXT("?Bots=%d?Skill=%d"), bBotsWhenEmpty ? MinPlayers : 0, BotSkill);
	if (GoalScore >= 0)
	{
		Url += FString::Printf(TEXT("?Goal=%d"), GoalScore);
	}
	if (TimeLimit >= 0)
	{
		Url += FString::Printf(TEXT("?Time=%d"), TimeLimit);
	}
	if (Mutators.Num() > 0)
	{
		Url += TEXT("?Mutator=") + FString::Join(Mutators, TEXT(","));
	}
	// our UE2 server (Net/UE2Server.h), dedicated: no player of its own
	Url += FString::Printf(TEXT("?UE2Host=1?UE2Port=%d?UE2Dedicated=1"), Port);
	Url += FString::Printf(TEXT("?MaxPlayers=%d?MaxSpectators=%d"), MaxPlayers, MaxSpectators);
	Url += TEXT("?ServerName=") + EncodeOption(ServerName.Left(64));
	if (!ShortName.IsEmpty())
	{
		Url += TEXT("?ShortName=") + EncodeOption(ShortName.Left(32));
	}
	if (!AdminName.IsEmpty())
	{
		Url += TEXT("?AdminName=") + EncodeOption(AdminName.Left(64));
	}
	if (!AdminEmail.IsEmpty())
	{
		Url += TEXT("?AdminEmail=") + EncodeOption(AdminEmail.Left(64));
	}
	if (MOTD.Num() > 0)
	{
		TArray<FString> Lines;
		for (const FString& L : MOTD)
		{
			Lines.Add(EncodeOption(L.Left(128)));
		}
		// lines joined by '|' (the '|' of each line is encoded)
		Url += TEXT("?MOTD=") + FString::Join(Lines, TEXT("|"));
	}
	if (!GamePassword.IsEmpty())
	{
		Url += TEXT("?GamePassword=") + EncodeOption(GamePassword.Left(32));
	}
	Url += FString::Printf(TEXT("?LANServer=%d?Advertise=%d"), bLANServer ? 1 : 0, bUplink ? 1 : 0);
	if (bUseServerPackages && ServerPackages.Num() > 0)
	{
		// replaces the install's [Engine.GameEngine] ServerPackages in the clients' package list (USES)
		Url += TEXT("?ServerPackages=") + FString::Join(ServerPackages, TEXT(","));
	}
	if (Maps.Num() > 1)
	{
		Url += TEXT("?MapList=") + FString::Join(Maps, TEXT(","));
	}
	Url += ExtraOptions;
	return Url;
}
