#include "UE2Server.h"

#if OPENUT2K4_UE2SERVER_BUILD

#include "UE2ServerConnection.h"
#include "UE2ServerUplink.h"
#include "UE2ServerValidator.h"
#include "UE2ServerWorld.h"
#include "UE2NetLog.h"
#include "../ArenaContent.h"
#include "../ArenaGameMode.h"
#include "../ArenaLevelBuilder.h"
#include "../ArenaUserSettings.h"
#include "../Convert/ArenaConvPackage.h"
#include "../UScript/ArenaUScript.h"
#include "../UScript/UScriptRuntime.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/FileManager.h"
#include "IPAddress.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

namespace UE2ServerLocal
{
	TUniquePtr<FUE2Server> GServer;
	/** UE2 package flag: never sent to clients (UPackageMap::AddLinker skips these). */
	constexpr uint32 UE2PkgServerSideOnly = 0x00000004;

	/** UT2004's own [Engine.GameEngine] ServerPackages (Default.ini of the retail install), when no ini is found. */
	const TCHAR* DefaultServerPackages[] = {
		TEXT("Core"), TEXT("Engine"), TEXT("Fire"), TEXT("Editor"), TEXT("IpDrv"), TEXT("UWeb"), TEXT("GamePlay"), TEXT("UnrealGame"),
		TEXT("XEffects"), TEXT("XPickups"), TEXT("XGame"), TEXT("XWeapons"), TEXT("XInterface"), TEXT("Vehicles"), TEXT("TeamSymbols_UT2003"),
		TEXT("TeamSymbols_UT2004"), TEXT("BonusPack"), TEXT("SkaarjPack_rc"), TEXT("SkaarjPack"), TEXT("UTClassic"), TEXT("UT2k4Assault"),
		TEXT("Onslaught"), TEXT("GUI2K4"), TEXT("UT2k4AssaultFull"), TEXT("OnslaughtFull"), TEXT("xVoting"), TEXT("OnslaughtBP"), TEXT("AssaultBP"),
	};

	/** ServerPackages of the install's ini ([Engine.GameEngine]), in order. */
	TArray<FString> ReadServerPackages(const FString& Install)
	{
		TArray<FString> Out;
		for (const TCHAR* Ini : { TEXT("UT2004.ini"), TEXT("Default.ini") })
		{
			FString Text;
			if (Install.IsEmpty() || !FFileHelper::LoadFileToString(Text, *FPaths::Combine(Install, TEXT("System"), Ini)))
			{
				continue;
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines);
			bool bSection = false;
			for (const FString& L : Lines)
			{
				const FString T = L.TrimStartAndEnd();
				if (T.StartsWith(TEXT("[")))
				{
					bSection = T.Equals(TEXT("[Engine.GameEngine]"), ESearchCase::IgnoreCase);
					continue;
				}
				FString Key, Value;
				if (bSection && T.Split(TEXT("="), &Key, &Value) && Key.TrimEnd().Equals(TEXT("ServerPackages"), ESearchCase::IgnoreCase))
				{
					Out.AddUnique(Value.TrimStartAndEnd());
				}
			}
			if (Out.Num() > 0)
			{
				break;
			}
		}
		if (Out.Num() == 0)
		{
			for (const TCHAR* P : DefaultServerPackages)
			{
				Out.Add(P);
			}
		}
		return Out;
	}

	/** UE2 game class of a mode id. */
	FString GameClassOfMode(const FString& ModeId)
	{
		static const TMap<FString, FString> Classes = {
			{ TEXT("DM"), TEXT("XGame.xDeathMatch") }, { TEXT("TDM"), TEXT("XGame.xTeamGame") }, { TEXT("CTF"), TEXT("XGame.xCTFGame") },
			{ TEXT("BR"), TEXT("XGame.xBombingRun") }, { TEXT("DOM"), TEXT("XGame.xDoubleDom") }, { TEXT("ONS"), TEXT("Onslaught.ONSOnslaughtGame") },
			{ TEXT("AS"), TEXT("UT2k4Assault.ASGameInfo") }, { TEXT("INV"), TEXT("SkaarjPack.Invasion") }, { TEXT("LMS"), TEXT("BonusPack.xLastManStandingGame") },
			{ TEXT("MUT"), TEXT("BonusPack.xMutantGame") },
		};
		const FString* C = Classes.Find(ModeId.ToUpper());
		return C ? *C : FString(TEXT("XGame.xDeathMatch"));
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

	/** "?Key=Value" of a URL's options (case-insensitive key), empty when missing. */
	FString UrlOption(const FString& Url, const TCHAR* Key)
	{
		TArray<FString> Parts;
		Url.ParseIntoArray(Parts, TEXT("?"));
		for (const FString& P : Parts)
		{
			FString K, V;
			if (P.Split(TEXT("="), &K, &V) && K.Equals(Key, ESearchCase::IgnoreCase))
			{
				return V;
			}
		}
		return FString();
	}
}

using namespace UE2ServerLocal;

namespace UE2ServerBans
{
	TFunction<TArray<FString>&()> GetList;
	TFunction<void()> SaveList;
	TArray<FString>& List() { return GetList ? GetList() : UArenaUserSettings::Get()->HostBannedPlayers; }
	void Save() { if (SaveList) { SaveList(); } else { UArenaUserSettings::Get()->Save(); } }
}


// =================================================================================================
// Start / stop
// =================================================================================================

FUE2Server* FUE2Server::Get()
{
	return GServer.Get();
}

bool FUE2Server::Start(UWorld* World, const FUE2ServerSettings& InSettings, FString& OutError)
{
	Stop(TEXT("a new server starts"));
	TUniquePtr<FUE2Server> S(new FUE2Server());
	if (!S->Init(World, InSettings, OutError))
	{
		UE_LOG(LogUE2Net, Warning, TEXT("UE2Server: not started: %s"), *OutError);
		return false;
	}
	GServer = MoveTemp(S);
	return true;
}

void FUE2Server::Stop(const FString& Reason)
{
	if (!GServer)
	{
		return;
	}
	TUniquePtr<FUE2Server> S = MoveTemp(GServer);
	for (TUniquePtr<FUE2ServerConnection>& C : S->Connections)
	{
		C->Close(Reason.IsEmpty() ? TEXT("server shut down") : Reason);
	}
	S->LogSummary();
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server: stopped%s%s"), Reason.IsEmpty() ? TEXT("") : TEXT(": "), *Reason);
}

FUE2Server::FUE2Server()
	: Random(int32(FPlatformTime::Cycles()))
{
}

FUE2Server::~FUE2Server()
{
	FWorldDelegates::OnWorldPreActorTick.Remove(PreTickHandle);
	FWorldDelegates::OnWorldPostActorTick.Remove(PostTickHandle);
	// the world layer first: it destroys our remote players' controllers
	ServerWorld.Reset();
	Connections.Reset();
	Uplink.Reset();
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	delete Files;
	Files = nullptr;
}

double FUE2Server::GetTime() const
{
	return FPlatformTime::Seconds();
}

int32 FUE2Server::NextChallenge()
{
	return Random.RandRange(1, MAX_int32 - 1);
}

bool FUE2Server::Init(UWorld* InWorld, const FUE2ServerSettings& InSettings, FString& OutError)
{
	World = InWorld;
	Settings = InSettings;
	bTrace = FParse::Param(FCommandLine::Get(), TEXT("UE2ServerTrace"));
	const AArenaGameMode* GM = InWorld ? InWorld->GetAuthGameMode<AArenaGameMode>() : nullptr;
	if (!GM)
	{
		OutError = TEXT("no match is running");
		return false;
	}
	MapName = AArenaLevelBuilder::GetMapNameFromLayoutId(GM->GetLayoutId());
	if (MapName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("the layout %s is not a UT2004 map (UT2004 clients need the stock map)"), *GM->GetLayoutId());
		return false;
	}
	GameClass = GameClassOfMode(GM->GetModeId());
	FString Err;
	if (!ArenaUScript::EnsureRuntime(&Err))
	{
		OutError = FString::Printf(TEXT("the UnrealScript runtime is not available (%s)"), *Err);
		return false;
	}
	BuildPackageList();
	if (UsesLines.Num() == 0 || !UsesLines[0].Contains(FString::Printf(TEXT("PKG=%s "), *MapName)))
	{
		OutError = FString::Printf(TEXT("the map package %s is not in the UT2004 install"), *MapName);
		return false;
	}

	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	Socket = SS ? SS->CreateSocket(NAME_DGram, TEXT("UE2Server"), false) : nullptr;
	if (!Socket)
	{
		OutError = TEXT("cannot create a UDP socket");
		return false;
	}
	Socket->SetNonBlocking(true);
	int32 Actual = 0;
	Socket->SetReceiveBufferSize(1 << 21, Actual);
	Socket->SetSendBufferSize(1 << 21, Actual);
	TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
	FString Bind;
	if (FParse::Value(FCommandLine::Get(), TEXT("UE2ServerBind="), Bind))
	{
		bool bValid = false;
		Addr->SetIp(*Bind, bValid);
	}
	else
	{
		Addr->SetAnyAddress();
	}
	Addr->SetPort(Settings.Port);
	if (!Socket->Bind(*Addr))
	{
		OutError = FString::Printf(TEXT("cannot bind UDP port %d (in use?)"), Settings.Port);
		return false;
	}
	ServerWorld = MakeUnique<FUE2ServerWorld>(*this, InWorld);
	PreTickHandle = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FUE2Server::PreActorTick);
	PostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FUE2Server::PostActorTick);
	StartTime = GetTime();
	NextSummary = StartTime + 30.0;
	if (Settings.bAdvertise)
	{
		Uplink = MakeUnique<FUE2MasterUplink>(*this);
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server: hosting %s (%s) for UT2004 clients on UDP %d: %d packages, %s, max %d players / %d spectators%s%s"),
		*MapName, *GameClass, Settings.Port, UsesLines.Num(), Settings.bListen ? TEXT("listen") : TEXT("dedicated"), Settings.MaxPlayers, Settings.MaxSpectators,
		Settings.Password.IsEmpty() ? TEXT("") : TEXT(", password"), Settings.bAdvertise ? TEXT(", master server uplink on") : TEXT(", no master server uplink"));
	return true;
}

// =================================================================================================
// Packages (USES): the map and its imports, then the ServerPackages and theirs (UPackageMap::AddLinker order)
// =================================================================================================

void FUE2Server::BuildPackageList()
{
	const FString Install = ArenaContent::FindUT2004Install();
	Files = new FConvPackageManager(Install.IsEmpty() ? FPaths::Combine(ArenaContent::GetCacheDir(), TEXT("scripts")) : Install);
	{
		TArray<FString> Extra;
		IFileManager::Get().FindFilesRecursive(Extra, *FPaths::Combine(ArenaContent::GetCacheDir(), TEXT("scripts")), TEXT("*.u"), true, false, false);
		// custom maps and their packages (the custom content folder): clients need the same files
		const FString UserDir = ArenaContent::GetUserContentDir();
		if (!UserDir.IsEmpty() && IFileManager::Get().DirectoryExists(*UserDir))
		{
			for (const TCHAR* Ext : { TEXT("*.u"), TEXT("*.ut2"), TEXT("*.utx"), TEXT("*.usx"), TEXT("*.uax"), TEXT("*.ukx") })
			{
				IFileManager::Get().FindFilesRecursive(Extra, *UserDir, Ext, true, false, false);
			}
		}
		Files->AddPackageFiles(Extra);
	}
	AddPackage(MapName, 0);
	// ?ServerPackages=A,B,C (a dedicated server's own list) replaces the install's [Engine.GameEngine] ServerPackages
	TArray<FString> ServerPackages;
	if (const AArenaGameMode* GM = World.IsValid() ? World->GetAuthGameMode<AArenaGameMode>() : nullptr)
	{
		UGameplayStatics::ParseOption(TEXT("?") + GM->GetMatchOptions(), TEXT("ServerPackages")).ParseIntoArray(ServerPackages, TEXT(","));
	}
	if (ServerPackages.Num() == 0)
	{
		ServerPackages = ReadServerPackages(Install);
	}
	for (const FString& P : ServerPackages)
	{
		AddPackage(P.TrimStartAndEnd(), 0);
	}
	// the game class's package (a ServerPackage for every stock game)
	FString GamePackage;
	if (GameClass.Split(TEXT("."), &GamePackage, nullptr))
	{
		AddPackage(GamePackage, 0);
	}
	for (const FString& Line : UsesLines)
	{
		MasterMap.AddUses(Line.Mid(5));
	}
	MasterMap.MatchLocalPackages();
	MasterMap.Compute();
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server: package map %d packages, %d objects, %d names (install %s)"), UsesLines.Num(), MasterMap.GetMaxObjectIndex(),
		MasterMap.GetMaxNameIndex(), Install.IsEmpty() ? TEXT("not found") : *Install);
}

void FUE2Server::AddPackage(const FString& Name, int32 Depth)
{
	const FString Key = Name.ToLower();
	if (Name.IsEmpty() || AddedPackages.Contains(Key) || Depth > 64)
	{
		return;
	}
	AddedPackages.Add(Key);
	const FString Path = Files->FindFile(Name);
	FUE2PackageHeader Header;
	FString Err;
	if (Path.IsEmpty() || !UE2ReadPackageHeader(Path, Header, &Err))
	{
		// native-only packages of the engine have no file; anything else is missing from the install
		UE_LOG(LogUE2Net, Verbose, TEXT("UE2Server: package %s not found (%s)"), *Name, *Err);
		return;
	}
	if (Header.PackageFlags & UE2PkgServerSideOnly)
	{
		return;
	}
	const int64 Size = IFileManager::Get().FileSize(*Path);
	const FString FileName = FPaths::GetCleanFilename(Path);
	// the 3369 server's form (FNAME unquoted); 3374 clients read both
	UsesLines.Add(FString::Printf(TEXT("USES GUID=%s PKG=%s FLAGS=%u SIZE=%lld GEN=%d FNAME=%s"), *UE2GuidToString(Header.Guid), *FPaths::GetBaseFilename(Path),
		Header.PackageFlags, Size, Header.Generations.Num(), *FileName));
	FConvPackage* Pkg = Files->OpenFile(Path);
	if (!Pkg)
	{
		return;
	}
	TArray<FString> Imports;
	for (const FConvImport& Imp : Pkg->Imports)
	{
		if (Imp.PackageIndex == 0 && Imp.ClassName.Equals(TEXT("Package"), ESearchCase::IgnoreCase))
		{
			Imports.Add(Imp.Name);
		}
	}
	for (const FString& I : Imports)
	{
		AddPackage(I, Depth + 1);
	}
}

bool FUE2Server::HasPackageFile(const FString& Name) const
{
	return Files && !Files->FindFile(Name).IsEmpty();
}

FUE2ObjectRef FUE2Server::FindStatic(const FString& Path)
{
	if (const FUE2ObjectRef* Hit = StaticCache.Find(Path))
	{
		return *Hit;
	}
	FUE2ObjectRef Ref;
	if (!MasterMap.FindObjectByPath(Path, Ref))
	{
		UE_LOG(LogUE2Net, Verbose, TEXT("UE2Server: no object %s in the package map"), *Path);
	}
	StaticCache.Add(Path, Ref);
	return Ref;
}

FUSClass* FUE2Server::FindClass(const FString& Path)
{
	FString Err;
	FUSClass* C = FUSRuntime::Get().FindClass(Path, &Err);
	if (C && !C->EnsureLoaded())
	{
		C = nullptr;
	}
	return C;
}

const FUE2ClassNetCache* FUE2Server::GetNetCache(FUSClass* Class)
{
	return MasterMap.GetClassNetCache(Class);
}

// =================================================================================================
// Actors
// =================================================================================================

FUE2SvActor* FUE2Server::CreateActor(const FString& ClassPath, FName Kind)
{
	FUSClass* Class = FindClass(ClassPath);
	const FUE2ClassNetCache* Cache = Class ? GetNetCache(Class) : nullptr;
	const FUE2ObjectRef Ref = FindStatic(ClassPath);
	if (!Class || !Cache || !Ref.IsStatic())
	{
		static TSet<FString> Warned;
		if (!Warned.Contains(ClassPath))
		{
			Warned.Add(ClassPath);
			UE_LOG(LogUE2Net, Warning, TEXT("UE2Server: class %s is not available (not replicated)"), *ClassPath);
		}
		return nullptr;
	}
	TUniquePtr<FUE2SvActor> A = MakeUnique<FUE2SvActor>();
	A->Id = NextActorId++;
	A->Class = Class;
	A->NetCache = Cache;
	A->ClassPath = ClassPath;
	A->Archetype = Ref;
	A->Kind = Kind;
	A->SeenFrame = Frame;
	FUSPropRef Initial;
	if (Initial.Resolve(Class, TEXT("bNetInitialRotation")))
	{
		A->bNetInitialRotation = Initial.Get<uint32>(Class->GetDefaultObject()) != 0;
	}
	FUSPropRef Temp;
	if (Temp.Resolve(Class, TEXT("bNetTemporary")))
	{
		A->bNetTemporary = Temp.Get<uint32>(Class->GetDefaultObject()) != 0;
	}
	FUSPropRef Always;
	if (Always.Resolve(Class, TEXT("bAlwaysRelevant")))
	{
		A->bAlwaysRelevant = Always.Get<uint32>(Class->GetDefaultObject()) != 0;
	}
	FUE2SvActor* Out = A.Get();
	Actors.Add(Out->Id, MoveTemp(A));
	return Out;
}

FUE2SvActor* FUE2Server::CreateMapActor(const FString& ExportPath, const FString& ClassPath, FName Kind)
{
	const FUE2ObjectRef Ref = FindStatic(ExportPath);
	// the map actor's own class (from the map package) unless the caller names one
	FUSClass* Class = ClassPath.IsEmpty() ? (Ref.IsStatic() ? MasterMap.GetInstanceClass(Ref) : nullptr) : FindClass(ClassPath);
	if (Class)
	{
		Class->EnsureLoaded();
	}
	const FUE2ClassNetCache* Cache = Class ? GetNetCache(Class) : nullptr;
	if (!Class || !Cache || !Ref.IsStatic())
	{
		UE_LOG(LogUE2Net, Verbose, TEXT("UE2Server: map actor %s (%s) not available"), *ExportPath, *ClassPath);
		return nullptr;
	}
	TUniquePtr<FUE2SvActor> A = MakeUnique<FUE2SvActor>();
	A->Id = NextActorId++;
	A->Class = Class;
	A->NetCache = Cache;
	A->ClassPath = ClassPath.IsEmpty() ? Class->GetPathName() : ClassPath;
	A->Archetype = Ref;
	A->bStatic = true;
	A->Kind = Kind;
	A->SeenFrame = Frame;
	FUE2SvActor* Out = A.Get();
	Actors.Add(Out->Id, MoveTemp(A));
	return Out;
}

void FUE2Server::DestroyActor(int32 Id)
{
	for (TUniquePtr<FUE2ServerConnection>& C : Connections)
	{
		C->CloseActorChannel(Id);
		C->FinishedActors.Remove(Id);
	}
	Actors.Remove(Id);
}

FUSProperty* FUE2Server::ResolveKey(FUSClass* Class, const FString& Key, int32& OutElement, int32& OutRepIndex)
{
	TMap<FString, FResolvedKey>& ByKey = KeyCache.FindOrAdd(Class);
	if (const FResolvedKey* Hit = ByKey.Find(Key))
	{
		OutElement = Hit->Element;
		OutRepIndex = Hit->RepIndex;
		return Hit->Prop;
	}
	FResolvedKey R;
	FString Name = Key;
	int32 Element = 0;
	int32 Bracket = INDEX_NONE;
	if (Key.FindChar(TEXT('['), Bracket))
	{
		Name = Key.Left(Bracket);
		Element = FCString::Atoi(*Key.Mid(Bracket + 1));
	}
	FUSProperty* Prop = Class ? Class->FindPropertyByName(FName(*Name)) : nullptr;
	const FUE2ClassNetCache* Cache = Class ? GetNetCache(Class) : nullptr;
	const int32 RepIndex = (Prop && Cache) ? Cache->GetIndexOf(Prop) : INDEX_NONE;
	if (Prop && RepIndex != INDEX_NONE && Element >= 0 && Element < Prop->ArrayDim)
	{
		R.Prop = Prop;
		R.Element = Element;
		R.RepIndex = RepIndex;
	}
	else
	{
		UE_LOG(LogUE2Net, Warning, TEXT("UE2Server: %s.%s is not a replicated property"), Class ? *Class->GetName() : TEXT("?"), *Key);
	}
	ByKey.Add(Key, R);
	OutElement = R.Element;
	OutRepIndex = R.RepIndex;
	return R.Prop;
}

bool FUE2Server::GetDefaultValue(FUSClass* Class, FUSProperty* Prop, int32 Element, FUE2NetValue& Out)
{
	FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
	if (!Def || !Def->Data || !Prop || Element < 0 || Element >= Prop->ArrayDim)
	{
		return false;
	}
	const uint8* Mem = Def->Data + Prop->Offset + Element * Prop->ElementSize;
	switch (Prop->Type)
	{
	case EUSType::Byte: Out = UE2Codec::Byte(*Mem); return true;
	case EUSType::Int: Out = UE2Codec::Int(*reinterpret_cast<const int32*>(Mem)); return true;
	case EUSType::Bool: Out = UE2Codec::Bool(*reinterpret_cast<const uint32*>(Mem) != 0); return true;
	case EUSType::Float: Out = UE2Codec::Float(*reinterpret_cast<const float*>(Mem)); return true;
	case EUSType::Str: Out = UE2Codec::Str(*reinterpret_cast<const FString*>(Mem)); return true;
	case EUSType::Name: Out = UE2Codec::Name(*reinterpret_cast<const FName*>(Mem)); return true;
	case EUSType::Object:
	case EUSType::Class:
	{
		const FUSObject* O = *reinterpret_cast<FUSObject* const*>(Mem);
		if (!O)
		{
			Out = UE2Codec::None();
			return true;
		}
		const FUE2ObjectRef Ref = FindStatic(O->GetPathName());
		if (!Ref.IsStatic())
		{
			return false;
		}
		Out = UE2Codec::Static(Ref);
		return true;
	}
	case EUSType::Struct:
		if (Prop->Struct && Prop->Struct->Name == TEXT("Vector"))
		{
			const FUSVector* V = reinterpret_cast<const FUSVector*>(Mem);
			Out = UE2Codec::Vec(FVector3f(V->X, V->Y, V->Z));
			return true;
		}
		if (Prop->Struct && Prop->Struct->Name == TEXT("Rotator"))
		{
			const FUSRotator* R = reinterpret_cast<const FUSRotator*>(Mem);
			Out = UE2Codec::Rot(FIntVector(R->Pitch, R->Yaw, R->Roll));
			return true;
		}
		return false;
	default:
		return false;
	}
}

FUE2ServerConnection* FUE2Server::FindConnection(int32 ConnId) const
{
	for (const TUniquePtr<FUE2ServerConnection>& C : Connections)
	{
		if (C->GetId() == ConnId)
		{
			return C.Get();
		}
	}
	return nullptr;
}

bool FUE2Server::CallClient(int32 ConnId, int32 ActorId, FName Function, const TArray<FUE2NetValue>& Params, bool bForceUnreliable)
{
	FUE2ServerConnection* C = FindConnection(ConnId);
	FUE2SvActor* A = FindActor(ActorId);
	if (!C || !A || !C->IsJoined())
	{
		return false;
	}
	return C->CallFunction(*A, Function, Params, A->OwnerConn == ConnId, bForceUnreliable);
}

void FUE2Server::CallAllClients(TFunctionRef<int32(int32 ConnId)> GetActor, FName Function, const TArray<FUE2NetValue>& Params)
{
	for (const TUniquePtr<FUE2ServerConnection>& C : Connections)
	{
		if (C->IsJoined())
		{
			if (const int32 ActorId = GetActor(C->GetId()))
			{
				CallClient(C->GetId(), ActorId, Function, Params);
			}
		}
	}
}

int32 FUE2Server::GetNumPlayers(bool bCountSpectators) const
{
	int32 N = 0;
	for (const TUniquePtr<FUE2ServerConnection>& C : Connections)
	{
		if (!C->IsClosed() && C->GetState() != FUE2ServerConnection::EState::Hello && (bCountSpectators || !C->bSpectatorOnly))
		{
			++N;
		}
	}
	return N;
}

// =================================================================================================
// Connections
// =================================================================================================

void FUE2Server::ConnectionLogin(FUE2ServerConnection& Conn, const FString& Url, FString& OutFailCode, FString& OutFailText)
{
	FString Name = DecodeOption(UrlOption(Url, TEXT("Name"))).Replace(TEXT("_"), TEXT(" ")).Left(20).TrimStartAndEnd();
	if (Name.IsEmpty())
	{
		Name = TEXT("Player");
	}
	Conn.PlayerName = Name;
	Conn.bSpectatorOnly = FCString::Atoi(*UrlOption(Url, TEXT("SpectatorOnly"))) != 0;
	// the host's ban list, by the player id from AUTH (UE2 AccessControl.bBanByID)
	if (!Conn.PlayerHashId.IsEmpty())
	{
		for (const FString& Entry : UE2ServerBans::List())
		{
			if (Entry.StartsWith(Conn.PlayerHashId + TEXT("|")) || Entry == Conn.PlayerHashId)
			{
				OutFailCode = TEXT("LOCALBAN");
				OutFailText = TEXT("You are banned from this server.");
				return;
			}
		}
	}
	if (!Settings.Password.IsEmpty())
	{
		const FString Given = DecodeOption(UrlOption(Url, TEXT("Password")));
		if (Given.IsEmpty())
		{
			OutFailCode = TEXT("NEEDPW");
			OutFailText = TEXT("Need password");
			return;
		}
		if (Given != Settings.Password)
		{
			OutFailCode = TEXT("WRONGPW");
			OutFailText = TEXT("Wrong password");
			return;
		}
	}
	int32 Players = 0, Spectators = 0;
	for (const TUniquePtr<FUE2ServerConnection>& C : Connections)
	{
		if (C.Get() != &Conn && !C->IsClosed() && (C->GetState() == FUE2ServerConnection::EState::Welcomed || C->IsJoined()))
		{
			++(C->bSpectatorOnly ? Spectators : Players);
		}
	}
	// the local players of a listen server and the bots do not count, as in UT2004 (bots make room)
	if (Conn.bSpectatorOnly ? Spectators >= Settings.MaxSpectators : Players >= Settings.MaxPlayers)
	{
		OutFailCode = TEXT("SERVERFULL");
		OutFailText = TEXT("Server is full.");
		return;
	}
	// OpenUT2k4 Validator (off unless the dedicated server installs it): combined bans by IP / UT2004 id
	UE2ServerValidator::CheckLogin(Conn, Url, OutFailCode, OutFailText);
}

void FUE2Server::ConnectionJoined(FUE2ServerConnection& Conn)
{
	if (ServerWorld)
	{
		ServerWorld->PlayerJoined(Conn);
	}
	UE2ServerValidator::PlayerJoined(Conn);
}

void FUE2Server::ConnectionClosed(FUE2ServerConnection& Conn, const FString& Reason)
{
	UE2ServerValidator::PlayerLeft(Conn);
	if (ServerWorld)
	{
		ServerWorld->PlayerLeft(Conn);
	}
	if (Uplink)
	{
		Uplink->ConnectionClosed(Conn.GetId());
	}
}

void FUE2Server::ClientFunction(FUE2ServerConnection& Conn, int32 ActorId, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
{
	if (ServerWorld)
	{
		ServerWorld->ClientFunction(Conn, ActorId, Function, Params);
	}
}

// =================================================================================================
// Tick
// =================================================================================================

void FUE2Server::PreActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds)
{
	if (InWorld != World.Get())
	{
		return;
	}
	// client moves first: the remote pawns move before the world ticks, like UE2's TickDispatch
	ReceivePackets(GetTime());
}

void FUE2Server::PostActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds)
{
	if (InWorld != World.Get())
	{
		return;
	}
	const double Now = GetTime();
	++Frame;
	UE2ServerValidator::Tick(Now);
	if (ServerWorld)
	{
		ServerWorld->Refresh(DeltaSeconds);
		ServerWorld->SendAdjustments();
	}
	Replicate(Now);
	for (TUniquePtr<FUE2ServerConnection>& C : Connections)
	{
		C->Tick(Now);
	}
	Connections.RemoveAll([](const TUniquePtr<FUE2ServerConnection>& C) { return C->IsClosed(); });
	if (Uplink)
	{
		Uplink->Tick(Now);
	}
	if (Now >= NextSummary)
	{
		NextSummary = Now + 30.0;
		LogSummary();
	}
}

void FUE2Server::ReceivePackets(double Now)
{
	if (!Socket)
	{
		return;
	}
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	uint8 Buffer[4096];
	for (int32 Guard = 0; Guard < 4000; ++Guard)
	{
		TSharedRef<FInternetAddr> From = SS->CreateInternetAddr();
		int32 Read = 0;
		if (!Socket->RecvFrom(Buffer, sizeof(Buffer), Read, *From) || Read <= 0)
		{
			break;
		}
		FUE2ServerConnection* Conn = nullptr;
		for (TUniquePtr<FUE2ServerConnection>& C : Connections)
		{
			if (!C->IsClosed() && C->GetAddr()->CompareEndpoints(*From))
			{
				Conn = C.Get();
				break;
			}
		}
		if (!Conn)
		{
			if (Connections.Num() >= 128)
			{
				continue;
			}
			Connections.Add(MakeUnique<FUE2ServerConnection>(*this, From, NextConnId++));
			Conn = Connections.Last().Get();
			UE_LOG(LogUE2Net, Log, TEXT("UE2Server: new connection %d from %s"), Conn->GetId(), *From->ToString(true));
		}
		Conn->ReceivedRawPacket(Buffer, Read, Now);
	}
}

bool FUE2Server::LineOfSight(const FVector3f& From, const FVector3f& To) const
{
	UWorld* W = World.Get();
	if (!W)
	{
		return true;
	}
	// the level's static geometry only (UE2 FastTrace: BSP and static meshes)
	FCollisionQueryParams Params(SCENE_QUERY_STAT(UE2ServerRelevancy), false);
	return !W->LineTraceTestByObjectType(FVector(From) * 2.0, FVector(To) * 2.0, FCollisionObjectQueryParams(ECC_WorldStatic), Params);
}

bool FUE2Server::IsRelevant(const FUE2SvActor& Actor, FUE2ServerConnection& Conn, const FVector3f& ViewLoc, double Now)
{
	const bool bOwner = Actor.OwnerConn != 0 && Actor.OwnerConn == Conn.GetId();
	if (Actor.bOnlyRelevantToOwner)
	{
		return bOwner;
	}
	if (bOwner || Actor.bAlwaysRelevant || Actor.bStatic)
	{
		return true;
	}
	if (Actor.RelevancyParent)
	{
		return Conn.ChannelOfActor(Actor.RelevancyParent) != 0;
	}
	if (Actor.CullDistance > 0.f && FVector3f::DistSquared(Actor.Location, ViewLoc) > FMath::Square(Actor.CullDistance))
	{
		return false;
	}
	if (!Actor.bUseLineOfSight)
	{
		return true;
	}
	// UE2 ServerTickClients: visible from the viewer (to the actor or a little above it), kept relevant 5 s after that
	FUE2ServerConnection::FVisibility& V = Conn.Visibility.FindOrAdd(Actor.Id);
	if (Now - V.LastCheck > 0.2)
	{
		V.LastCheck = Now;
		if (FVector3f::DistSquared(Actor.Location, ViewLoc) < FMath::Square(300.f) || LineOfSight(ViewLoc, Actor.Location)
			|| LineOfSight(ViewLoc, Actor.Location + FVector3f(0.f, 0.f, 40.f)))
		{
			V.LastVisible = Now;
		}
	}
	return Now - V.LastVisible < 5.0;
}

void FUE2Server::Replicate(double Now)
{
	struct FCandidate { FUE2SvActor* Actor; double Score; bool bEssential; };
	for (TUniquePtr<FUE2ServerConnection>& C : Connections)
	{
		if (!C->IsJoined())
		{
			continue;
		}
		const int32 ConnId = C->GetId();
		FVector3f View = FVector3f::ZeroVector;
		if (ServerWorld)
		{
			ServerWorld->GetViewLocation(ConnId, View);
		}
		// the client's NETSPEED: refill the budget, charge it with everything queued since (calls included)
		const double Rate = FMath::Max(1800.0, double(C->NetSpeed));
		if (C->BudgetTime <= 0.0)
		{
			C->BudgetTime = Now;
			C->Budget = Rate * 0.5;
			C->BudgetCharged = C->QueuedBytes;
		}
		C->Budget = FMath::Min(Rate * 0.5, C->Budget + Rate * (Now - C->BudgetTime));
		C->BudgetTime = Now;
		C->Budget -= double(C->QueuedBytes - C->BudgetCharged);
		C->BudgetCharged = C->QueuedBytes;

		TArray<FCandidate> Candidates;
		Candidates.Reserve(Actors.Num());
		// parents before children (attachments follow their pawn)
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			for (TPair<int32, TUniquePtr<FUE2SvActor>>& KV : Actors)
			{
				FUE2SvActor* A = KV.Value.Get();
				if ((A->RelevancyParent != 0) != (Pass == 1))
				{
					continue;
				}
				FUE2ServerConnection::FActorChannel* AC = C->FindChannelOfActor(A->Id);
				const bool bOwner = A->OwnerConn != 0 && A->OwnerConn == ConnId;
				if (!IsRelevant(*A, *C, View, Now))
				{
					if (AC && (A->bOnlyRelevantToOwner || A->RelevancyParent || Now - AC->LastRelevant > 5.0))
					{
						C->CloseActorChannel(A->Id);
					}
					continue;
				}
				if (AC)
				{
					AC->LastRelevant = Now;
				}
				const double Since = AC ? Now - AC->LastUpdate : 10.0;
				if (AC && Since < A->UpdateInterval)
				{
					continue;
				}
				// the client's own controller and pawn, and the game info, always go out
				const bool bEssential = (bOwner && (A->Kind == TEXT("PC") || A->Kind == TEXT("Pawn"))) || A->Kind == TEXT("GRI") || A->Kind == TEXT("LevelInfo");
				Candidates.Add({ A, A->NetPriority * (Since + A->UpdateInterval) * (bOwner ? 2.0 : 1.0), bEssential });
			}
		}
		Candidates.StableSort([](const FCandidate& X, const FCandidate& Y)
		{
			if (X.bEssential != Y.bEssential)
			{
				return X.bEssential;
			}
			return X.Score > Y.Score;
		});
		int32 Skipped = 0;
		for (const FCandidate& Cand : Candidates)
		{
			if (C->Budget <= 0.0 && !Cand.bEssential)
			{
				++Skipped;
				continue;
			}
			const int64 Before = C->QueuedBytes;
			C->ReplicateActor(*Cand.Actor, Now, Cand.Actor->OwnerConn == ConnId);
			C->Budget -= double(C->QueuedBytes - Before);
			C->BudgetCharged = C->QueuedBytes;
		}
		if (Skipped > 0)
		{
			++C->SaturatedTicks;
		}
		// torn off actors (dead pawns) close a moment after the client got bTearOff; the client keeps them
		TArray<int32> ToClose;
		for (const TPair<int32, FUE2ServerConnection::FActorChannel>& AC : C->GetActorChannels())
		{
			if (AC.Value.bTornOff && Now - AC.Value.LastUpdate > 2.0)
			{
				ToClose.Add(AC.Key);
			}
		}
		for (int32 Id : ToClose)
		{
			C->CloseActorChannel(Id);
			C->FinishedActors.Add(Id);
		}
		// visibility records of actors that are gone
		for (auto It = C->Visibility.CreateIterator(); It; ++It)
		{
			if (!Actors.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
	}
}

void FUE2Server::LogSummary()
{
	const double Now = GetTime();
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server summary: %.0f s, %d connections, %d actors%s"), Now - StartTime, Connections.Num(), Actors.Num(),
		ServerWorld ? *(TEXT(", ") + ServerWorld->DescribeSummary()) : TEXT(""));
	for (const TUniquePtr<FUE2ServerConnection>& C : Connections)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2Server summary: [%d] %s %s: packets in %d out %d, bytes in %lld out %lld (netspeed %d, saturated ticks %d), channels %d, actors opened %d, properties %d, calls sent %d received %d, errors %d"),
			C->GetId(), *C->PlayerName, *C->GetAddrString(), C->PacketsIn, C->PacketsOut, C->BytesIn, C->BytesOut, C->NetSpeed, C->SaturatedTicks,
			C->GetActorChannels().Num(), C->ActorsOpened, C->PropertiesSent, C->CallsSent, C->CallsReceived, C->BunchErrors);
	}
}

void ArenaHost::SetUE2BanStore(TFunction<TArray<FString>&()> GetList, TFunction<void()> SaveList)
{
	UE2ServerBans::GetList = MoveTemp(GetList);
	UE2ServerBans::SaveList = MoveTemp(SaveList);
}

void ArenaHost::UE2ServerSay(const FString& Text)
{
	if (FUE2Server* S = FUE2Server::Get())
	{
		if (FUE2ServerWorld* W = S->GetServerWorld())
		{
			W->OnChat(nullptr, Text, false);
		}
	}
}

bool ArenaHost::IsUE2Hosting()
{
	return FUE2Server::Get() != nullptr;
}

TArray<ArenaHost::FUE2HostPlayer> ArenaHost::GetUE2Players()
{
	TArray<FUE2HostPlayer> Out;
	if (FUE2Server* S = FUE2Server::Get())
	{
		for (const TUniquePtr<FUE2ServerConnection>& C : S->GetConnections())
		{
			if (!C->IsJoined())
			{
				continue;
			}
			FUE2HostPlayer P;
			P.ConnId = C->GetId();
			P.Name = C->PlayerName;
			P.PingMs = C->PingMs;
			P.IdShort = C->PlayerHashId.Left(8);
			P.bSpectator = C->bSpectatorOnly;
			Out.Add(MoveTemp(P));
		}
	}
	return Out;
}

bool ArenaHost::KickUE2Player(int32 ConnId, bool bBan)
{
	FUE2Server* S = FUE2Server::Get();
	FUE2ServerConnection* C = S ? S->FindConnection(ConnId) : nullptr;
	if (!C || C->IsClosed())
	{
		return false;
	}
	if (bBan)
	{
		UE2ServerValidator::PlayerBanned(*C, TEXT("host ban"));
	}
	if (bBan && !C->PlayerHashId.IsEmpty())
	{
		TArray<FString>& Bans = UE2ServerBans::List();
		const FString Entry = C->PlayerHashId + TEXT("|") + C->PlayerName.Replace(TEXT("|"), TEXT(""));
		if (!Bans.ContainsByPredicate([&C](const FString& E) { return E.StartsWith(C->PlayerHashId + TEXT("|")); }))
		{
			Bans.Add(Entry);
			UE2ServerBans::Save();
		}
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2Server: %s %s by the host"), *C->PlayerName, bBan ? TEXT("banned") : TEXT("kicked"));
	// UE2 AccessControl: the client is told why, then the connection closes
	C->Close(bBan ? TEXT("banned by the host") : TEXT("kicked by the host"), FString(),
		bBan ? TEXT("You have been banned from this server.") : TEXT("You have been kicked from this server."));
	return true;
}

bool ArenaHost::IsUE2IdBanned(const FString& Id)
{
	return !Id.IsEmpty() && UE2ServerBans::List().ContainsByPredicate([&Id](const FString& E) { return E == Id || E.StartsWith(Id + TEXT("|"), ESearchCase::IgnoreCase); });
}

TArray<FString> ArenaHost::GetUE2BanList()
{
	TArray<FString> Out;
	for (const FString& E : UE2ServerBans::List())
	{
		FString Id, Name;
		if (!E.Split(TEXT("|"), &Id, &Name))
		{
			Id = E;
		}
		Out.Add(FString::Printf(TEXT("%s (%s...)"), *Name, *Id.Left(8)));
	}
	return Out;
}

void ArenaHost::UnbanUE2(int32 Index)
{
	TArray<FString>& Bans = UE2ServerBans::List();
	if (Bans.IsValidIndex(Index))
	{
		Bans.RemoveAt(Index);
		UE2ServerBans::Save();
	}
}

#else

bool ArenaHost::IsUE2Hosting()
{
	return false;
}

TArray<ArenaHost::FUE2HostPlayer> ArenaHost::GetUE2Players() { return {}; }
bool ArenaHost::KickUE2Player(int32, bool) { return false; }
bool ArenaHost::IsUE2IdBanned(const FString&) { return false; }
TArray<FString> ArenaHost::GetUE2BanList() { return {}; }
void ArenaHost::UnbanUE2(int32) {}
void ArenaHost::SetUE2BanStore(TFunction<TArray<FString>&()>, TFunction<void()>) {}
void ArenaHost::UE2ServerSay(const FString&) {}

#endif // OPENUT2K4_UE2SERVER_BUILD
