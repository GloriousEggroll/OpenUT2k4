#include "UE2NetNewNet.h"

#include "../UScript/UScriptCore.h"
#include "UE2NetConnection.h"
#include "UE2NetLog.h"

namespace UE2NetNewNetLocal
{
	bool ChainHas(const FUE2NetActor& A, const TCHAR* Name)
	{
		for (FUSClass* C = A.Class; C; C = C->GetSuperClass())
		{
			if (C->Name == Name)
			{
				return true;
			}
		}
		return false;
	}

	int32 GetRef(const FUE2NetActor& A, const TCHAR* Key)
	{
		const FUE2NetValue* V = A.Properties.Find(Key);
		return V && V->Kind == FUE2NetValue::EKind::Object && V->Obj.IsDynamic() ? V->Obj.Channel : 0;
	}

	/** A float class default (0 when the class has no such property). */
	float DefaultFloat(FUSClass* Class, const TCHAR* Key)
	{
		FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
		FUSProperty* Prop = Def ? Def->FindProperty(FName(Key)) : nullptr;
		if (!Prop || !Def->Data || Prop->Type != EUSType::Float)
		{
			return 0.f;
		}
		return *reinterpret_cast<const float*>(Def->Data + Prop->Offset);
	}

	FUE2NetValue VFloat(float F) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Float; V.F = F; return V; }
	FUE2NetValue VInt(int32 I) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Int; V.I = I; return V; }
	FUE2NetValue VByte(int32 I) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Byte; V.I = I & 255; return V; }
	FUE2NetValue VBool(bool B) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Bool; V.I = B ? 1 : 0; return V; }
	FUE2NetValue VActor(int32 Channel) { FUE2NetValue V; V.Kind = FUE2NetValue::EKind::Object; V.Obj.Channel = Channel; return V; }
}

FUE2NetNewNet::FUE2NetNewNet(FUE2NetConnection& InConnection)
	: Connection(InConnection)
{
}

void FUE2NetNewNet::ActorUpdated(const FUE2NetActor& A, const TArray<FString>& Changed, double GameTime)
{
	using namespace UE2NetNewNetLocal;
	if (A.bBroken || !A.Class)
	{
		return;
	}
	if (Changed.Contains(TEXT("ClientTimeStamp")) && ChainHas(A, TEXT("NewNet_TimeStamp")))
	{
		// NewNet_TimeStamp.Tick is not simulated: the client's copy is the last value received
		if (const FUE2NetValue* V = A.Properties.Find(TEXT("ClientTimeStamp")))
		{
			if (!bHaveFloatStamp)
			{
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetNewNet: the server runs NewNet (%s, float time stamps)"), *A.ClassPath);
			}
			FloatStampChannel = A.Channel;
			FloatStamp = V->F;
			bHaveFloatStamp = true;
		}
		return;
	}
	// the client's TimeStamp_Pawn is the first one it finds (foreach DynamicActors ... break): one channel is followed
	if ((Changed.Contains(TEXT("Rotation")) || Changed.Contains(TEXT("PawnPosition"))) && ChainHas(A, TEXT("TimeStamp_Pawn"))
		&& (!PawnStampChannel || PawnStampChannel == A.Channel))
	{
		// TimeStamp_Pawn.Tick (simulated): NewTimeStamp = (Rotation.Yaw + Rotation.Pitch * 256) / 256; a newer one
		// (or one that wrapped around) resets DT. The pawn has no mesh: its Rotation replicates as a plain rotator
		// (PawnPosition's rotation where a server sends that instead)
		TArray<FIntVector, TInlineAllocator<2>> Rotations;
		if (const FUE2NetValue* R = Changed.Contains(TEXT("Rotation")) ? A.Properties.Find(TEXT("Rotation")) : nullptr)
		{
			Rotations.Add(R->R);
		}
		const FUE2NetValue* V = Changed.Contains(TEXT("PawnPosition")) ? A.Properties.Find(TEXT("PawnPosition")) : nullptr;
		for (const TPair<FName, FUE2NetValue>& M : V ? V->Members : TArray<TPair<FName, FUE2NetValue>>())
		{
			if (M.Key == TEXT("Rotation"))
			{
				Rotations.Add(M.Value.R);
			}
		}
		for (const FIntVector& Rot : Rotations)
		{
			const int32 New = (Rot.Y + Rot.X * 256) / 256;
			if (!bHavePawnStamp)
			{
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetNewNet: the server runs NewNet (%s, tick counter time stamps)"), *A.ClassPath);
			}
			if (!bHavePawnStamp || New > PawnStamp || PawnStamp - New > 5000)
			{
				PawnStamp = New;
				PawnStampDT = 0.f;
			}
			PawnStampChannel = A.Channel;
			bHavePawnStamp = true;
		}
	}
}

void FUE2NetNewNet::ActorClosed(const FUE2NetActor& A)
{
	if (A.Channel == FloatStampChannel)
	{
		FloatStampChannel = 0;
	}
	if (A.Channel == PawnStampChannel)
	{
		PawnStampChannel = 0;
	}
	if (A.Channel == PingChannel)
	{
		PingChannel = 0;
		bPingReceived = true;
	}
}

bool FUE2NetNewNet::FunctionCall(const FUE2NetActor& A, FUSFunction* Function, double GameTime)
{
	if (!Function || Function->Name != TEXT("Pong") || A.Channel != PingChannel)
	{
		return false;
	}
	// NewNet_PRI.Pong
	bPingReceived = true;
	++NumPongs;
	++NumPings;
	const float Rtt = float(GameTime - PingSendTime);
	if (PingTweenTime > 0.f)
	{
		PredictedPing = (2.f * PredictedPing + Rtt) / PingTweenTime;
		if (NumPings < 8)
		{
			PredictedPing = Rtt;
		}
	}
	else
	{
		PredictedPing = (9.f * PredictedPing + Rtt) * 0.1f;
		if (NumPings < 30)
		{
			PredictedPing = Rtt;
		}
	}
	return true;
}

int32 FUE2NetNewNet::FindOwnNewNetPRI(int32 OwnPRIChannel) const
{
	using namespace UE2NetNewNetLocal;
	// our PRI's CustomReplicationInfo chain (LinkedReplicationInfo.NextReplicationInfo); the server only takes
	// calls on the one we own
	const TMap<int32, FUE2NetActor>& Actors = Connection.GetActors();
	const FUE2NetActor* PRI = OwnPRIChannel ? Actors.Find(OwnPRIChannel) : nullptr;
	int32 Ch = PRI ? GetRef(*PRI, TEXT("CustomReplicationInfo")) : 0;
	for (int32 Guard = 0; Ch && Guard < 32; ++Guard)
	{
		const FUE2NetActor* L = Actors.Find(Ch);
		if (!L)
		{
			return 0;
		}
		if (ChainHas(*L, TEXT("NewNet_PRI")))
		{
			return Ch;
		}
		Ch = GetRef(*L, TEXT("NextReplicationInfo"));
	}
	return 0;
}

void FUE2NetNewNet::Tick(double GameTime, float GameDelta, int32 OwnPRIChannel)
{
	using namespace UE2NetNewNetLocal;
	PawnStampDT += GameDelta;
	if (!PingChannel && OwnPRIChannel)
	{
		PingChannel = FindOwnNewNetPRI(OwnPRIChannel);
		if (PingChannel)
		{
			const FUE2NetActor* A = Connection.GetActors().Find(PingChannel);
			PingTweenTime = A ? DefaultFloat(A->Class, TEXT("PingTweenTime")) : 0.f;
			if (const FUE2NetActor* PRIActor = A)
			{
				// UTComp_ServerReplicationInfo.PingTweenTime overrides the class default (NewNet_PRI.PostBeginPlay)
				for (const TPair<int32, FUE2NetActor>& KV : Connection.GetActors())
				{
					if (ChainHas(KV.Value, TEXT("UTComp_ServerReplicationInfo")))
					{
						if (const FUE2NetValue* V = KV.Value.Properties.Find(TEXT("PingTweenTime")); V && V->F > 0.f)
						{
							PingTweenTime = V->F;
						}
					}
				}
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetNewNet: our %s is channel %d (ping every %.2f s)"), *PRIActor->ClassPath, PingChannel,
					PingTweenTime > 0.f ? PingTweenTime : 0.5f);
			}
		}
	}
	// NewNet_PRI.Tick (simulated): a ping when the last one came back and the tween time has passed
	if (PingChannel && bPingReceived && GameTime > PingSendTime + (PingTweenTime > 0.f ? PingTweenTime : 0.5f))
	{
		PingSendTime = GameTime;
		bPingReceived = false;
		Connection.SendFunction(PingChannel, FName(TEXT("Ping")), {});
	}
}

bool FUE2NetNewNet::IsNewNetWeapon(int32 WeaponChannel) const
{
	return WeaponChannel && Connection.FindReplicatedFunction(WeaponChannel, FName(TEXT("NewNet_ServerStartFire"))) != nullptr;
}

bool FUE2NetNewNet::HasOldStartFire(int32 WeaponChannel) const
{
	return WeaponChannel && Connection.FindReplicatedFunction(WeaponChannel, FName(TEXT("NewNet_OldServerStartFire"))) != nullptr;
}

bool FUE2NetNewNet::SendStartFire(int32 WeaponChannel, const FFire& Fire)
{
	using namespace UE2NetNewNetLocal;
	const FName Name(Fire.bOld ? TEXT("NewNet_OldServerStartFire") : TEXT("NewNet_ServerStartFire"));
	FUSFunction* Function = Connection.FindReplicatedFunction(WeaponChannel, Name);
	if (!Function)
	{
		return false;
	}
	TArray<FUE2NetValue> Params;
	FString Text;
	for (FUSProperty* P : Function->Parms)
	{
		const FString Key = P->Name.ToString();
		FUE2NetValue V;
		if (Key.Equals(TEXT("Mode"), ESearchCase::IgnoreCase))
		{
			V = VByte(Fire.Mode);
		}
		else if (Key.Equals(TEXT("ClientTimeStamp"), ESearchCase::IgnoreCase))
		{
			// a float (NewNet_TimeStamp's value) or a byte (TimeStamp_Pawn's counter)
			V = P->Type == EUSType::Float ? VFloat(FloatStamp) : VByte(PawnStamp);
		}
		else if (Key.Equals(TEXT("DT"), ESearchCase::IgnoreCase))
		{
			V = VFloat(PawnStampDT);
		}
		else if (Key.Equals(TEXT("avgPing"), ESearchCase::IgnoreCase))
		{
			V = VFloat(PredictedPing);
		}
		else if (Key == TEXT("R") && P->Type == EUSType::Struct)
		{
			// ReplicatedRotator { int Yaw, Pitch }: the controller's rotation
			V.Kind = FUE2NetValue::EKind::Struct;
			V.Members.Emplace(TEXT("Yaw"), VInt(Fire.Yaw));
			V.Members.Emplace(TEXT("Pitch"), VInt(Fire.Pitch));
		}
		else if (Key == TEXT("V") && P->Type == EUSType::Struct)
		{
			// ReplicatedVector { float X, Y, Z }: Pawn.Location + Pawn.EyePosition()
			V.Kind = FUE2NetValue::EKind::Struct;
			V.Members.Emplace(TEXT("X"), VFloat(float(Fire.EyeUU.X)));
			V.Members.Emplace(TEXT("Y"), VFloat(float(Fire.EyeUU.Y)));
			V.Members.Emplace(TEXT("Z"), VFloat(float(Fire.EyeUU.Z)));
		}
		else if (Key.Equals(TEXT("bBelievesHit"), ESearchCase::IgnoreCase))
		{
			V = VBool(Fire.HitChannel != 0);
		}
		else if (Key == TEXT("A") && (P->Type == EUSType::Object))
		{
			V = VActor(Fire.HitChannel);
		}
		else if (!WarnedParams.Contains(P->Name))
		{
			WarnedParams.Add(P->Name);
			UE_LOG(LogUE2Net, Warning, TEXT("UE2NetNewNet: %s has an unknown parameter %s (sent as zero)"), *Name.ToString(), *Key);
		}
		Params.Add(V);
		Text += FString::Printf(TEXT(" %s=%s"), *Key, *Connection.ValueToString(V));
	}
	if (!Connection.SendFunction(WeaponChannel, Name, Params))
	{
		return false;
	}
	++NumFires;
	NumOldFires += Fire.bOld ? 1 : 0;
	NumBelievedHits += Fire.HitChannel ? 1 : 0;
	UE_LOG(LogUE2Net, Verbose, TEXT("UE2NetNewNet: %s%s"), *Name.ToString(), *Text);
	return true;
}

FString FUE2NetNewNet::Describe() const
{
	if (!HasStamp() && NumFires == 0)
	{
		return FString();
	}
	return FString::Printf(TEXT("NewNet: %d fires (%d old, %d believed hits), stamp %s, ping %.0f ms (%d pongs)"), NumFires, NumOldFires,
		NumBelievedHits, bHaveFloatStamp ? *FString::Printf(TEXT("%.3f"), FloatStamp) : *FString::Printf(TEXT("%d + %.3f"), PawnStamp, PawnStampDT),
		PredictedPing * 1000.f, NumPongs);
}
