// UScript object instances: construction from class defaults, destruction and reference cleanup.

#include "UScriptWorld.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"

FUSWorld::FUSWorld() = default;

FUSWorld::~FUSWorld()
{
	for (FUSObject* O : Objects)
	{
		if (O->Data && O->Class)
		{
			O->Class->DestroyValue(O->Data);
			FMemory::Free(O->Data);
			O->Data = nullptr;
		}
		delete O;
	}
	Objects.Reset();
}

FUSWorld& FUSWorld::Transient()
{
	static FUSWorld* W = new FUSWorld();
	return *W;
}

FUSObject* FUSWorld::ConstructObject(FUSClass* Class, FUSObject* Outer, FName Name)
{
	if (!Class || !Class->EnsureLoaded())
	{
		return nullptr;
	}
	FUSObject* O = new FUSObject();
	O->Class = Class;
	O->Outer = Outer;
	O->World = this;
	O->Flags = USF_Transient;
	if (FUSRuntime::Get().ActorClass && Class->IsChildOf(FUSRuntime::Get().ActorClass))
	{
		O->Flags |= USF_Actor;
	}
	if (Name == NAME_None)
	{
		int32& Counter = NameCounters.FindOrAdd(Class->Name);
		Name = FName(Class->Name, Counter++ + 1);
	}
	O->Name = Name;
	const int32 Size = FMath::Max(Class->PropertiesSize, 8);
	O->Data = static_cast<uint8*>(FMemory::Malloc(Size, 16));
	FMemory::Memzero(O->Data, Size);
	if (Class->Defaults.Num() >= Class->PropertiesSize)
	{
		for (FUSProperty* P : Class->AllProps)
		{
			P->CopyComplete(O->Data + P->Offset, Class->Defaults.GetData() + P->Offset);
		}
	}
	O->StateFrame = new FUSStateFrame();
	O->StateFrame->StateNode = Class;
	O->StateFrame->CodeNode = Class;
	O->Index = Objects.Add(O);
	return O;
}

void FUSWorld::MarkPendingKill(FUSObject* Obj)
{
	if (Obj && !Obj->IsPendingKill() && Obj->World == this)
	{
		Obj->Flags |= USF_PendingKill;
		++PendingKillCount;
	}
}

void FUSWorld::CleanupDestroyed()
{
	if (PendingKillCount == 0)
	{
		return;
	}
	for (FUSObject* O : Objects)
	{
		if (O->IsPendingKill() || !O->Data || !O->Class)
		{
			continue;
		}
		for (FUSProperty* P : O->Class->RefProps)
		{
			for (int32 i = 0; i < P->ArrayDim; ++i)
			{
				P->CleanupRefs(O->Data + P->Offset + i * P->ElementSize);
			}
		}
		if (O->StateFrame && O->StateFrame->LatentObject && O->StateFrame->LatentObject->IsPendingKill())
		{
			O->StateFrame->LatentObject = nullptr;
		}
		if (O->Outer && O->Outer->IsPendingKill())
		{
			O->Outer = nullptr;
		}
	}
	int32 Write = 0;
	for (int32 Read = 0; Read < Objects.Num(); ++Read)
	{
		FUSObject* O = Objects[Read];
		if (O->IsPendingKill())
		{
			if (O->Data && O->Class)
			{
				O->Class->DestroyValue(O->Data);
				FMemory::Free(O->Data);
				O->Data = nullptr;
			}
			delete O;
			continue;
		}
		O->Index = Write;
		Objects[Write++] = O;
	}
	Objects.SetNum(Write);
	PendingKillCount = 0;
}

FUSObject* FUSWorld::SpawnActor(FUSClass* Class, FUSObject* Owner, FName Tag, const FUSVector* Location, const FUSRotator* Rotation, FUSObject* Spawner)
{
	return ConstructObject(Class, nullptr, NAME_None);
}

bool FUSWorld::DestroyActor(FUSObject* Actor)
{
	MarkPendingKill(Actor);
	return true;
}

#endif // OPENUT2K4_USCRIPT_ENABLED
