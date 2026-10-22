// Mod weapons of a UT2004 server shown with the mod's own assets. See UE2NetModWeapons.h.

#include "UE2NetModWeapons.h"
#include "UE2NetActors.h"
#include "UE2NetLog.h"
#include "UE2NetModContent.h"
#include "../ArenaArt.h"
#include "../ArenaContent.h"
#include "../ArenaRuntimeContent.h"
#include "../Convert/ArenaConvExporter.h"
#include "../UScript/UScriptCore.h"
#include "../UScript/UScriptRuntime.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundBase.h"

namespace UE2NetModWeaponsLocal
{
	/** UT2004's own script packages: their classes are stock, everything else is a mod's. */
	bool IsStockPackage(const FString& Pkg)
	{
		static const TSet<FString> Stock = { TEXT("core"), TEXT("engine"), TEXT("fire"), TEXT("ipdrv"), TEXT("uweb"), TEXT("editor"),
			TEXT("unrealgame"), TEXT("xgame"), TEXT("xweapons"), TEXT("xpickups"), TEXT("xeffects"), TEXT("xinterface"), TEXT("xadmin"),
			TEXT("xvoting"), TEXT("xwebadmin"), TEXT("gameplay"), TEXT("utclassic"), TEXT("onslaught"), TEXT("onslaughtfull"),
			TEXT("onslaughtbp"), TEXT("ut2k4assault"), TEXT("ut2k4assaultfull"), TEXT("bonuspack"), TEXT("skaarjpack"), TEXT("vehicles"),
			TEXT("gui2k4"), TEXT("ut2k4mod"), TEXT("ut2k4tabs"), TEXT("streamlinefx"), TEXT("ut2004upgrade") };
		return Stock.Contains(Pkg.ToLower());
	}

	FString PackageOf(const FString& Path)
	{
		FString Pkg;
		return Path.Split(TEXT("."), &Pkg, nullptr) ? Pkg : Path;
	}

	/** A class's default object (its defaults), loading the package. */
	struct FDefaults
	{
		FUSClass* Class = nullptr;
		FUSObject* Def = nullptr;
		explicit operator bool() const { return Def && Def->Data; }
	};
	FDefaults Defaults(const FString& ClassPath)
	{
		FDefaults D;
		D.Class = ClassPath.IsEmpty() ? nullptr : FUSRuntime::Get().FindClass(ClassPath);
		D.Def = D.Class ? D.Class->GetDefaultObject() : nullptr;
		return D;
	}

	FUSProperty* Prop(const FDefaults& D, const TCHAR* Name)
	{
		return D ? D.Def->FindProperty(Name) : nullptr;
	}

	/** An object or class reference of the defaults, as its path ("" for None). */
	FString ObjPath(const FDefaults& D, const TCHAR* Name, int32 Index = 0)
	{
		const FUSProperty* P = Prop(D, Name);
		if (!P || (P->Type != EUSType::Object && P->Type != EUSType::Class) || Index >= P->ArrayDim)
		{
			return FString();
		}
		const FUSObject* O = *reinterpret_cast<FUSObject**>(D.Def->Data + P->Offset + Index * P->ElementSize);
		return O ? O->GetPathName() : FString();
	}

	/** A dynamic array of object references (Skins), as paths. */
	TArray<FString> ObjPaths(const FDefaults& D, const TCHAR* Name)
	{
		TArray<FString> Out;
		const FUSProperty* P = Prop(D, Name);
		if (!P || P->Type != EUSType::Array || !P->Inner || P->Inner->Type != EUSType::Object)
		{
			return Out;
		}
		const FUSScriptArray& A = *reinterpret_cast<const FUSScriptArray*>(D.Def->Data + P->Offset);
		for (int32 i = 0; i < A.Num && A.Data; ++i)
		{
			const FUSObject* O = *reinterpret_cast<FUSObject* const*>(A.Data + i * P->Inner->ElementSize);
			Out.Add(O ? O->GetPathName() : FString());
		}
		return Out;
	}

	template <typename T>
	T Value(const FDefaults& D, const TCHAR* Name, const T& Default = T())
	{
		const FUSProperty* P = Prop(D, Name);
		return P && P->ElementSize >= int32(sizeof(T)) ? *reinterpret_cast<const T*>(D.Def->Data + P->Offset) : Default;
	}

	FString NameValue(const FDefaults& D, const TCHAR* Name)
	{
		const FUSProperty* P = Prop(D, Name);
		return P && P->Type == EUSType::Name ? reinterpret_cast<const FName*>(D.Def->Data + P->Offset)->ToString() : FString();
	}

	bool Same(const FUSVector& A, const FUSVector& B) { return A.X == B.X && A.Y == B.Y && A.Z == B.Z; }

	/** Actor.DrawScale x DrawScale3D (its X: the art's scale is uniform). */
	float DrawScaleOf(const FDefaults& D)
	{
		return Value<float>(D, TEXT("DrawScale"), 1.f) * Value<FUSVector>(D, TEXT("DrawScale3D"), FUSVector(1.f, 1.f, 1.f)).X;
	}
	bool Same(const FUSRotator& A, const FUSRotator& B) { return A.Pitch == B.Pitch && A.Yaw == B.Yaw && A.Roll == B.Roll; }

	/** "Pkg.Group.Name" -> "<Kind>/Pkg/Group_Name", the art path of a converted object. */
	FString ArtPath(const TCHAR* Kind, const FString& ObjectPath)
	{
		FString Pkg, Rest;
		if (!ObjectPath.Split(TEXT("."), &Pkg, &Rest))
		{
			return FString();
		}
		return FString::Printf(TEXT("%s/%s/%s"), Kind, *Pkg, *Rest.Replace(TEXT("."), TEXT("_")));
	}

	/** The animation set the converter recorded for a skeletal mesh ("Pkg.Anim"), from its cache json. */
	FString MeshAnimation(const FString& MeshPath)
	{
		FString Pkg, Rest;
		if (!MeshPath.Split(TEXT("."), &Pkg, &Rest))
		{
			return FString();
		}
		const FString Dir = ArenaContent::GetCacheDir() / TEXT("skeletalmeshes") / FConvExporter::Safe(Pkg);
		for (const FString& File : { Rest, Rest.Replace(TEXT("."), TEXT("_")) })
		{
			FString Text;
			TSharedPtr<FJsonObject> Json;
			if (FFileHelper::LoadFileToString(Text, *(Dir / (FConvExporter::Safe(File) + TEXT(".json"))))
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid())
			{
				return Json->GetStringField(TEXT("animation"));
			}
		}
		return FString();
	}

	/** Art strings live in the description: stores S and returns a pointer to it (null for empty). */
	const TCHAR* Keep(FString& Storage, const FString& S)
	{
		Storage = S;
		return Storage.IsEmpty() ? nullptr : *Storage;
	}

	/** Our stand-in and the stock UE2 class it reproduces, for a mod weapon or attachment class. */
	UClass* StandIn(const FString& UE2Class, FString& OutStockWeapon)
	{
		UClass* Native = UE2NetActors::WeaponClassFor(UE2Class);
		OutStockWeapon = Native ? ArenaWeaponHand::UE2WeaponClass(Native) : FString();
		return OutStockWeapon.IsEmpty() ? nullptr : Native;
	}

	TUniquePtr<UE2NetModWeapons::FModWeapon> BuildWeapon(const FString& UE2Class)
	{
		using namespace UE2NetModWeapons;
		FString StockClass;
		UClass* Native = StandIn(UE2Class, StockClass);
		const ArenaArt::FWeaponArt* Base = Native ? ArenaArt::FindWeaponArt(Native) : nullptr;
		const FDefaults M = Defaults(UE2Class);
		const FDefaults S = Defaults(StockClass);
		if (!Native || !Base || !M || !S)
		{
			return nullptr;
		}
		TUniquePtr<FModWeapon> W = MakeUnique<FModWeapon>();
		W->UE2Class = UE2Class;
		W->Native = Native;
		W->StockClass = StockClass;
		W->Art = *Base;
		auto Note = [&W](const FString& N) { W->Notes.Add(N); };

		// first-person mesh, its animation set and skins (Weapon.Mesh / Skins)
		const FString Mesh = ObjPath(M, TEXT("Mesh"));
		if (!Mesh.Equals(ObjPath(S, TEXT("Mesh")), ESearchCase::IgnoreCase) && !Mesh.IsEmpty())
		{
			const FString Path = ArtPath(TEXT("SkeletalMeshes"), Mesh);
			const FString Anim = MeshAnimation(Mesh);
			if (ArenaArt::Load<USkeletalMesh>(Path) && !Anim.IsEmpty())
			{
				W->Art.FirstPersonMesh = Keep(W->FirstPersonMesh, Path);
				W->Art.AnimPrefix = Keep(W->AnimPrefix, ArtPath(TEXT("Animations"), Anim) + TEXT("_"));
				// the animation names the mod's code plays (Weapon.SelectAnim / PutDownAnim, IdleAnim)
				struct FAnim { const TCHAR* Prop; const TCHAR** Art; FString* Storage; };
				for (const FAnim& A : { FAnim{ TEXT("SelectAnim"), &W->Art.SelectAnim, &W->SelectAnim },
					FAnim{ TEXT("PutDownAnim"), &W->Art.PutDownAnim, &W->PutDownAnim }, FAnim{ TEXT("IdleAnim"), &W->Art.IdleAnim, &W->IdleAnim } })
				{
					const FString N = NameValue(M, A.Prop);
					if (!N.IsEmpty() && N != TEXT("None"))
					{
						*A.Art = Keep(*A.Storage, N);
					}
				}
				Note(FString::Printf(TEXT("first-person mesh %s"), *Mesh));
			}
			else
			{
				Note(FString::Printf(TEXT("first-person mesh %s not converted: %s's"), *Mesh, *StockClass));
			}
		}
		const TArray<FString> Skins = ObjPaths(M, TEXT("Skins"));
		if (Skins != ObjPaths(S, TEXT("Skins")) && Skins.Num() > 0)
		{
			W->Skins = Skins;
			Note(FString::Printf(TEXT("first-person skins %s"), *FString::Join(Skins, TEXT(", "))));
		}

		// view placement (Weapon.PlayerViewOffset / SmallViewOffset / PlayerViewPivot / DrawScale / DisplayFOV / Centered*)
		const FUSVector Offset = Value<FUSVector>(M, TEXT("PlayerViewOffset"));
		const FUSVector Small = Value<FUSVector>(M, TEXT("SmallViewOffset"));
		const FUSRotator Pivot = Value<FUSRotator>(M, TEXT("PlayerViewPivot"));
		const float DrawScale = DrawScaleOf(M);
		const float DisplayFOV = Value<float>(M, TEXT("DisplayFOV"), 60.f);
		if (!Same(Offset, Value<FUSVector>(S, TEXT("PlayerViewOffset"))) || !Same(Small, Value<FUSVector>(S, TEXT("SmallViewOffset")))
			|| !Same(Pivot, Value<FUSRotator>(S, TEXT("PlayerViewPivot"))) || DrawScale != DrawScaleOf(S)
			|| DisplayFOV != Value<float>(S, TEXT("DisplayFOV"), 60.f) || Value<float>(M, TEXT("CenteredOffsetY")) != Value<float>(S, TEXT("CenteredOffsetY"))
			|| Value<int32>(M, TEXT("CenteredYaw")) != Value<int32>(S, TEXT("CenteredYaw")) || Value<int32>(M, TEXT("CenteredRoll")) != Value<int32>(S, TEXT("CenteredRoll")))
		{
			W->bView = true;
			W->View.bValid = true;
			W->View.Offset = Offset.ToFVector();
			W->View.SmallOffset = FVector(Small.X, Small.Y, Small.Z).IsZero() ? W->View.Offset : Small.ToFVector();
			W->View.CenteredOffsetY = Value<float>(M, TEXT("CenteredOffsetY"));
			W->View.CenteredYaw = Value<int32>(M, TEXT("CenteredYaw"));
			W->View.CenteredRoll = Value<int32>(M, TEXT("CenteredRoll"));
			W->View.DisplayFOV = DisplayFOV > 1.f ? DisplayFOV : 60.f;
			W->Art.SetViewFromSource(Offset.ToFVector(), FIntVector(Pivot.Pitch, Pivot.Yaw, Pivot.Roll), DrawScale, W->View.DisplayFOV);
			Note(TEXT("view placement"));
		}

		// fire modes: FireSound, FireRate, FireAnim (WeaponFire defaults)
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			const FDefaults MF = Defaults(ObjPath(M, TEXT("FireModeClass"), Mode));
			const FDefaults SF = Defaults(ObjPath(S, TEXT("FireModeClass"), Mode));
			if (!MF || !SF)
			{
				continue;
			}
			const FString Sound = ObjPath(MF, TEXT("FireSound"));
			if (!Sound.Equals(ObjPath(SF, TEXT("FireSound")), ESearchCase::IgnoreCase))
			{
				const FString Path = ArtPath(TEXT("Sounds"), Sound);
				if (!Sound.IsEmpty() && ArenaArt::LoadOneShotSound(Path))
				{
					W->Art.FireSound[Mode] = Keep(W->FireSound[Mode], Path);
					Note(FString::Printf(TEXT("fire %d sound %s"), Mode, *Sound));
				}
				else
				{
					Note(FString::Printf(TEXT("fire %d sound %s not converted: %s's"), Mode, Sound.IsEmpty() ? TEXT("None") : *Sound, *StockClass));
				}
			}
			const float Rate = Value<float>(MF, TEXT("FireRate"), -1.f);
			if (Rate > 0.f && Rate != Value<float>(SF, TEXT("FireRate"), -1.f))
			{
				W->FireRate[Mode] = Rate;
				Note(FString::Printf(TEXT("fire %d rate %.2f s"), Mode, Rate));
			}
			const FString FireAnim = NameValue(MF, TEXT("FireAnim"));
			if (W->Art.FirstPersonMesh != Base->FirstPersonMesh && !FireAnim.IsEmpty() && FireAnim != TEXT("None"))
			{
				W->Art.FireAnim[Mode] = Keep(W->FireAnim[Mode], FireAnim);
			}
		}

		// weapon bar icon: IconCoords on the stock HUD atlas, or on the mod's own texture (a plain Texture: the bar draws
		// a region of it)
		const FString Icon = ObjPath(M, TEXT("IconMaterial"));
		const FIntVector4 Coords = Value<FIntVector4>(M, TEXT("IconCoords"));
		if (Coords != Value<FIntVector4>(S, TEXT("IconCoords")) || !Icon.Equals(ObjPath(S, TEXT("IconMaterial")), ESearchCase::IgnoreCase))
		{
			const FString IconArt = ArtPath(TEXT("Textures"), Icon);
			if (Icon.Equals(TEXT("HUDContent.Generic.HUD"), ESearchCase::IgnoreCase))
			{
				W->bIcon = true;
				W->IconCoords = FVector4f(Coords.X, Coords.Y, Coords.Z, Coords.W);
				Note(TEXT("weapon bar icon"));
			}
			else if (!IconArt.IsEmpty() && UE2NetModContent::EnsureConverted(Icon, TEXT("Texture")) && ArenaArt::Load<UTexture2D>(IconArt))
			{
				W->bIcon = true;
				W->IconCoords = FVector4f(Coords.X, Coords.Y, Coords.Z, Coords.W);
				W->IconTexture = IconArt;
				Note(FString::Printf(TEXT("weapon bar icon %s"), *Icon));
			}
			else
			{
				Note(FString::Printf(TEXT("weapon bar icon %s: not a converted texture, %s's"), *Icon, *StockClass));
			}
		}
		return W;
	}

	TUniquePtr<UE2NetModWeapons::FModAttachment> BuildAttachment(const FString& UE2Class)
	{
		using namespace UE2NetModWeapons;
		FString StockWeapon;
		UClass* Native = StandIn(UE2Class, StockWeapon);
		const ArenaArt::FWeaponArt* Base = Native ? ArenaArt::FindWeaponArt(Native) : nullptr;
		const FDefaults M = Defaults(UE2Class);
		const FDefaults S = Defaults(ObjPath(Defaults(StockWeapon), TEXT("AttachmentClass")));
		if (!Base || !M || !S)
		{
			return nullptr;
		}
		TUniquePtr<FModAttachment> A = MakeUnique<FModAttachment>();
		A->UE2Class = UE2Class;
		A->Art = *Base;
		const FString Mesh = ObjPath(M, TEXT("Mesh"));
		if (!Mesh.Equals(ObjPath(S, TEXT("Mesh")), ESearchCase::IgnoreCase) && !Mesh.IsEmpty())
		{
			const FString Path = ArtPath(TEXT("SkeletalMeshes"), Mesh);
			if (ArenaArt::Load<USkeletalMesh>(Path))
			{
				A->Art.ThirdPersonMesh = Keep(A->ThirdPersonMesh, Path);
				A->Notes.Add(FString::Printf(TEXT("third-person mesh %s"), *Mesh));
			}
			else
			{
				// a static mesh attachment (DrawType DT_StaticMesh) or one that did not convert
				A->Notes.Add(FString::Printf(TEXT("third-person mesh %s not a converted skeletal mesh: the stand-in's"), *Mesh));
			}
		}
		const FUSVector Loc = Value<FUSVector>(M, TEXT("RelativeLocation"));
		const FUSRotator Rot = Value<FUSRotator>(M, TEXT("RelativeRotation"));
		const float Scale = DrawScaleOf(M);
		if (!Same(Loc, Value<FUSVector>(S, TEXT("RelativeLocation"))) || !Same(Rot, Value<FUSRotator>(S, TEXT("RelativeRotation")))
			|| Scale != DrawScaleOf(S) || A->Art.ThirdPersonMesh != Base->ThirdPersonMesh)
		{
			A->Art.SetHandFromSource(Loc.ToFVector(), FIntVector(Rot.Pitch, Rot.Yaw, Rot.Roll), Scale);
			A->Notes.Add(TEXT("third-person placement"));
		}
		const TArray<FString> Skins = ObjPaths(M, TEXT("Skins"));
		if (Skins != ObjPaths(S, TEXT("Skins")) && Skins.Num() > 0)
		{
			A->Skins = Skins;
			A->Notes.Add(FString::Printf(TEXT("third-person skins %s"), *FString::Join(Skins, TEXT(", "))));
		}
		return A;
	}

	template <typename T>
	const T* FindOrBuild(TMap<FString, TUniquePtr<T>>& Cache, const FString& UE2Class, TUniquePtr<T> (*Build)(const FString&), const TCHAR* What)
	{
		const FString Key = UE2Class.ToLower();
		if (const TUniquePtr<T>* Hit = Cache.Find(Key))
		{
			return Hit->Get();
		}
		TUniquePtr<T> Built = UE2Class.Contains(TEXT(".")) && !IsStockPackage(PackageOf(UE2Class)) ? Build(UE2Class) : nullptr;
		if (Built)
		{
			UE_LOG(LogUE2Net, Log, TEXT("ModWeapons: %s %s: %s"), What, *UE2Class,
				Built->Notes.Num() ? *FString::Join(Built->Notes, TEXT("; ")) : TEXT("nothing of its own (the stand-in's assets)"));
		}
		return Cache.Add(Key, MoveTemp(Built)).Get();
	}
}

using namespace UE2NetModWeaponsLocal;

const UE2NetModWeapons::FModWeapon* UE2NetModWeapons::FindWeapon(const FString& UE2Class)
{
	static TMap<FString, TUniquePtr<FModWeapon>> Cache;
	return FindOrBuild(Cache, UE2Class, &BuildWeapon, TEXT("weapon"));
}

const UE2NetModWeapons::FModAttachment* UE2NetModWeapons::FindAttachment(const FString& UE2Class)
{
	static TMap<FString, TUniquePtr<FModAttachment>> Cache;
	return FindOrBuild(Cache, UE2Class, &BuildAttachment, TEXT("attachment"));
}

UStaticMesh* UE2NetModWeapons::FindPickupMesh(const FString& UE2Class)
{
	static TMap<FString, TWeakObjectPtr<UStaticMesh>> Cache;
	const FString Key = UE2Class.ToLower();
	if (const TWeakObjectPtr<UStaticMesh>* Hit = Cache.Find(Key))
	{
		return Hit->Get();
	}
	UStaticMesh* Result = nullptr;
	if (UE2Class.Contains(TEXT(".")) && !IsStockPackage(PackageOf(UE2Class)))
	{
		// the pickup's StaticMesh against its nearest stock ancestor's (ProSniperPickup extends ClassicSniperRiflePickup)
		const FDefaults M = Defaults(UE2Class);
		FUSClass* Stock = M.Class ? M.Class->GetSuperClass() : nullptr;
		while (Stock && !IsStockPackage(PackageOf(Stock->GetPathName())))
		{
			Stock = Stock->GetSuperClass();
		}
		const FDefaults S = Defaults(Stock ? Stock->GetPathName() : FString());
		const FString Mesh = ObjPath(M, TEXT("StaticMesh"));
		if (M && S && !Mesh.IsEmpty() && !Mesh.Equals(ObjPath(S, TEXT("StaticMesh")), ESearchCase::IgnoreCase))
		{
			UE2NetModContent::EnsureConverted(Mesh, TEXT("StaticMesh"));
			Result = ArenaRuntimeContent::LoadStaticMeshByObjectPath(Mesh);
			UE_LOG(LogUE2Net, Log, TEXT("ModWeapons: pickup %s: mesh %s%s"), *UE2Class, *Mesh, Result ? TEXT("") : TEXT(" not converted (ours)"));
		}
	}
	Cache.Add(Key, Result);
	return Result;
}

bool UE2NetModWeapons::IsStockClass(const FString& ClassPath)
{
	return IsStockPackage(PackageOf(ClassPath));
}

TArray<UMaterialInterface*> UE2NetModWeapons::LoadSkins(const TArray<FString>& Skins)
{
	TArray<UMaterialInterface*> Out;
	for (const FString& S : Skins)
	{
		if (!S.IsEmpty())
		{
			UE2NetModContent::EnsureConverted(S, FString());
		}
		Out.Add(S.IsEmpty() ? nullptr : ArenaRuntimeContent::LoadMaterialByObjectPath(S));
	}
	return Out;
}

const TArray<UE2NetModWeapons::FClientAttachment>& UE2NetModWeapons::FindClientAttachments(const FString& ClassPath)
{
	static TMap<FString, TArray<FClientAttachment>> Cache;
	const FString Key = ClassPath.ToLower();
	if (const TArray<FClientAttachment>* Hit = Cache.Find(Key))
	{
		return *Hit;
	}
	TArray<FClientAttachment>& Out = Cache.Add(Key);
	const FDefaults M = Defaults(ClassPath);
	FUSRuntime& RT = FUSRuntime::Get();
	if (!M || IsStockPackage(PackageOf(ClassPath)) || !RT.ActorClass)
	{
		return Out;
	}
	// the class's instance variables holding a client-only actor with a mesh of its own
	TMap<FUSProperty*, FUSClass*> Holders;
	for (FUSProperty* P : M.Class->AllProps)
	{
		FUSClass* C = P && P->Type == EUSType::Object && P->ArrayDim == 1 ? P->PropertyClass : nullptr;
		if (!C || !C->EnsureLoaded() || !C->IsChildOf(RT.ActorClass) || IsStockPackage(PackageOf(C->GetPathName())))
		{
			continue;
		}
		const FDefaults A = Defaults(C->GetPathName());
		const uint8 DrawType = Value<uint8>(A, TEXT("DrawType"), 0);
		// (hidden until its code shows it, as BastionShield: not drawn)
		if (A && Value<uint8>(A, TEXT("RemoteRole"), 1) == 0 && Value<uint32>(A, TEXT("bHidden"), 0) == 0 && ((DrawType == 2 && !ObjPath(A, TEXT("Mesh")).IsEmpty()) || (DrawType == 8 && !ObjPath(A, TEXT("StaticMesh")).IsEmpty())))
		{
			Holders.Add(P, C);
		}
	}
	if (Holders.Num() == 0)
	{
		return Out;
	}
	// AttachToBone(<holder>, <bone>) in the code of the mod's classes: the two arguments are an instance variable and an
	// instance variable of type name or a name constant (UE2 bytecode: token, 4-byte reference)
	auto Int32At = [](const TArray<uint8>& S, int32 i) { int32 V; FMemory::Memcpy(&V, S.GetData() + i, 4); return V; };
	TSet<FUSClass*> Found;
	for (const TPair<FName, FUSFunction*>& F : M.Class->FuncMap)
	{
		const FUSFunction* Fn = F.Value;
		if (!Fn || Fn->Script.Num() < 10 || !Fn->Outer || IsStockPackage(PackageOf(Fn->Outer->GetPathName())))
		{
			continue;
		}
		const TArray<uint8>& S = Fn->Script;
		for (int32 i = 0; i + 10 <= S.Num(); ++i)
		{
			if (S[i] != USToken::InstanceVariable || (S[i + 5] != USToken::InstanceVariable && S[i + 5] != USToken::NameConst))
			{
				continue;
			}
			FUSProperty* Holder = static_cast<FUSProperty*>(RT.ResolveRef(Int32At(S, i + 1)));
			FUSClass* const* Spawned = Holder ? Holders.Find(Holder) : nullptr;
			if (!Spawned || Found.Contains(*Spawned))
			{
				continue;
			}
			FName Bone;
			if (S[i + 5] == USToken::NameConst)
			{
				Bone = RT.GetName(Int32At(S, i + 6));
			}
			else if (FUSObject* BP = RT.ResolveRef(Int32At(S, i + 6)); BP && BP->Class && BP->Class->Name == TEXT("NameProperty"))
			{
				Bone = FName(*NameValue(M, *BP->GetName()));
			}
			if (Bone.IsNone())
			{
				continue;
			}
			Found.Add(*Spawned);
			const FDefaults A = Defaults((*Spawned)->GetPathName());
			FClientAttachment At;
			At.UE2Class = (*Spawned)->GetPathName();
			At.bSkeletal = Value<uint8>(A, TEXT("DrawType"), 0) == 2;
			const FString MeshObj = ObjPath(A, At.bSkeletal ? TEXT("Mesh") : TEXT("StaticMesh"));
			UE2NetModContent::EnsureConverted(MeshObj, At.bSkeletal ? TEXT("SkeletalMesh") : TEXT("StaticMesh"));
			At.Mesh = ArtPath(At.bSkeletal ? TEXT("SkeletalMeshes") : TEXT("StaticMeshes"), MeshObj);
			At.Bone = Bone;
			const FUSVector S3 = Value<FUSVector>(A, TEXT("DrawScale3D"), FUSVector(1.f, 1.f, 1.f));
			At.Scale = FVector(S3.X, S3.Y, S3.Z) * Value<float>(A, TEXT("DrawScale"), 1.f);
			const FUSVector L = Value<FUSVector>(A, TEXT("RelativeLocation"));
			At.Offset = FVector(L.X, L.Y, L.Z);
			const FUSRotator R = Value<FUSRotator>(A, TEXT("RelativeRotation"));
			At.Rotation = FRotator(R.Pitch * 360.f / 65536.f, R.Yaw * 360.f / 65536.f, R.Roll * 360.f / 65536.f);
			At.Skins = ObjPaths(A, TEXT("Skins"));
			At.RedSkins = ObjPaths(A, TEXT("RedSkins"));
			At.BlueSkins = ObjPaths(A, TEXT("BlueSkins"));
			UE_LOG(LogUE2Net, Log, TEXT("ModWeapons: %s attaches %s (%s) to bone %s in %s, scale %s"), *ClassPath, *At.UE2Class, *MeshObj, *Bone.ToString(),
				*Fn->GetName(), *At.Scale.ToCompactString());
			Out.Add(MoveTemp(At));
		}
	}
	return Out;
}

const UE2NetModWeapons::FLook& UE2NetModWeapons::FindLook(const FString& ClassPath)
{
	static TMap<FString, FLook> Cache;
	const FString Key = ClassPath.ToLower();
	if (const FLook* Hit = Cache.Find(Key))
	{
		return *Hit;
	}
	auto Read = [](const FDefaults& D)
	{
		FLook L;
		if (!D)
		{
			return L;
		}
		L.Skins = ObjPaths(D, TEXT("Skins"));
		L.RedSkin = ObjPath(D, TEXT("RedSkin"));
		L.BlueSkin = ObjPath(D, TEXT("BlueSkin"));
		const FUSVector S3 = Value<FUSVector>(D, TEXT("DrawScale3D"), FUSVector(1.f, 1.f, 1.f));
		L.Scale = FVector(S3.X, S3.Y, S3.Z) * Value<float>(D, TEXT("DrawScale"), 1.f);
		const FUSVector P = Value<FUSVector>(D, TEXT("PrePivot"));
		L.PrePivot = FVector(P.X, P.Y, P.Z);
		return L;
	};
	const FDefaults M = Defaults(ClassPath);
	FLook Look = Read(M);
	if (M && !IsStockPackage(PackageOf(ClassPath)))
	{
		// against the nearest stock ancestor (FlareRaptor extends ONSAttackCraft): our art rig stands in only for the stock look
		FUSClass* Stock = M.Class->GetSuperClass();
		while (Stock && !IsStockPackage(PackageOf(Stock->GetPathName())))
		{
			Stock = Stock->GetSuperClass();
		}
		const FLook S = Read(Defaults(Stock ? Stock->GetPathName() : FString()));
		auto SameList = [](const TArray<FString>& A, const TArray<FString>& B)
		{
			for (int32 i = 0; i < FMath::Max(A.Num(), B.Num()); ++i)
			{
				if (!(A.IsValidIndex(i) ? A[i] : FString()).Equals(B.IsValidIndex(i) ? B[i] : FString(), ESearchCase::IgnoreCase))
				{
					return false;
				}
			}
			return true;
		};
		Look.bOwn = !Stock || !SameList(Look.Skins, S.Skins) || !Look.RedSkin.Equals(S.RedSkin, ESearchCase::IgnoreCase)
			|| !Look.BlueSkin.Equals(S.BlueSkin, ESearchCase::IgnoreCase) || !Look.Scale.Equals(S.Scale, 1e-3) || !Look.PrePivot.Equals(S.PrePivot, 1e-2);
		if (Look.bOwn)
		{
			UE_LOG(LogUE2Net, Log, TEXT("ModWeapons: look of %s: skins [%s], red %s, blue %s, scale %s, pre-pivot %s"), *ClassPath,
				*FString::Join(Look.Skins, TEXT(", ")), *Look.RedSkin, *Look.BlueSkin, *Look.Scale.ToCompactString(), *Look.PrePivot.ToCompactString());
		}
	}
	return Cache.Add(Key, MoveTemp(Look));
}
