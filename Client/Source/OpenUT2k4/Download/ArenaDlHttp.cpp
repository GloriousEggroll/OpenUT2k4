#include "ArenaDlHttp.h"
#include "ArenaDlSha256.h"
#include "../OpenUT2k4.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"

namespace ArenaDlHttpLocal
{
	const TCHAR* UserAgent = TEXT("OpenUT2k4-ContentDownload/1.0");

	/** Shared between the worker thread and the HTTP thread's delegates for one range request. */
	struct FRequestState
	{
		FCriticalSection Lock;
		/** Null once the worker gave up on the request (writes are then refused). */
		IFileHandle* File = nullptr;
		/** Absolute file position the response body should start at, and where writing stops. */
		int64 Start = 0;
		int64 End = 0;
		/** Size of the whole file (a 200 response may be written up to here). */
		int64 Total = 0;
		/** Bytes of the response body seen so far. */
		int64 Received = 0;
		/** Bytes written to the file. */
		TAtomic<int64> Written{ 0 };
		TAtomic<int32> Status{ 0 };
		TAtomic<bool> bWriteError{ false };
		TAtomic<bool> bDone{ false };
		TAtomic<bool> bConnected{ false };
		FString ContentRange;
		FEvent* DoneEvent = nullptr;

		FRequestState() { DoneEvent = FPlatformProcess::GetSynchEventFromPool(true); }
		~FRequestState() { FPlatformProcess::ReturnSynchEventToPool(DoneEvent); }

		/** Body data from the HTTP thread. Returns false to abort the transfer. */
		bool Receive(const uint8* Data, int64 Len)
		{
			FScopeLock Guard(&Lock);
			if (!File)
			{
				return false;
			}
			const int32 Code = Status;
			if (Code != 0 && Code != 200 && Code != 206)
			{
				return false; // error page: never write it into the file
			}
			// a 200 response is the whole file from byte 0: skip what we already have
			const int64 StreamBase = (Code == 200) ? 0 : Start;
			int64 Abs = StreamBase + Received;
			Received += Len;
			const int64 WriteFrom = Start + Written;
			if (Abs + Len <= WriteFrom)
			{
				return true;
			}
			if (Abs < WriteFrom)
			{
				Data += WriteFrom - Abs;
				Len -= WriteFrom - Abs;
				Abs = WriteFrom;
			}
			if (Abs != WriteFrom)
			{
				return false;
			}
			const int64 Room = (Code == 200 ? Total : End) - Abs;
			if (Room <= 0)
			{
				return false;
			}
			const int64 N = FMath::Min(Len, Room);
			if (!File->Write(Data, N))
			{
				bWriteError = true;
				return false;
			}
			Written += N;
			return N == Len;
		}
	};

	/** "<size> <sha256>" of the file a .part belongs to. */
	FString PartTag(const FArenaDlSource& S)
	{
		return FString::Printf(TEXT("%lld %s"), S.Size, *S.Sha256.ToLower());
	}

	FString HostOf(const FString& Url)
	{
		FString Rest = Url;
		int32 Pos = Rest.Find(TEXT("://"));
		if (Pos != INDEX_NONE)
		{
			Rest.RightChopInline(Pos + 3);
		}
		Rest.Split(TEXT("/"), &Rest, nullptr);
		return Rest.IsEmpty() ? Url : Rest;
	}
}

bool FArenaHttpDownload::Verify(const FString& Path, const FArenaDlSource& Source, const TAtomic<bool>& bCancel,
	TFunction<void(const FProgress&)> OnProgress, FString& OutError)
{
	const int64 Size = IFileManager::Get().FileSize(*Path);
	if (Size != Source.Size)
	{
		OutError = FString::Printf(TEXT("%s has %lld bytes, expected %lld"), *FPaths::GetCleanFilename(Path), Size, Source.Size);
		return false;
	}
	double LastReport = 0.0;
	FProgress P;
	P.Phase = TEXT("verify");
	P.Total = Size;
	P.Url = Source.Url;
	const FString Hash = FArenaSha256::HashFile(Path, [&](int64 Done, int64 Total)
	{
		const double Now = FPlatformTime::Seconds();
		if (OnProgress && (Now - LastReport > 0.25 || Done == Total))
		{
			LastReport = Now;
			P.Done = Done;
			OnProgress(P);
		}
		return !bCancel;
	});
	if (bCancel)
	{
		OutError = TEXT("Cancelled");
		return false;
	}
	if (Hash.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Cannot read %s"), *Path);
		return false;
	}
	if (!Hash.Equals(Source.Sha256, ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("SHA-256 mismatch: got %s, expected %s"), *Hash, *Source.Sha256);
		return false;
	}
	return true;
}

bool FArenaHttpDownload::Download(const TArray<FArenaDlSource>& Sources, const FString& DestPath, const TAtomic<bool>& bCancel,
	TFunction<void(const FProgress&)> OnProgress, TFunction<void(const FString&)> OnMessage, FString& OutError)
{
	using namespace ArenaDlHttpLocal;
	IFileManager& FM = IFileManager::Get();
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	const FString PartPath = DestPath + TEXT(".part");
	const FString TagPath = DestPath + TEXT(".part.json");
	auto Message = [&OnMessage](const FString& Text)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("Download: %s"), *Text);
		if (OnMessage)
		{
			OnMessage(Text);
		}
	};
	FM.MakeDirectory(*FPaths::GetPath(DestPath), true);

	// already downloaded?
	if (FM.FileExists(*DestPath))
	{
		const int64 Size = FM.FileSize(*DestPath);
		for (const FArenaDlSource& S : Sources)
		{
			if (S.Size == Size)
			{
				FString VerifyError;
				if (Verify(DestPath, S, bCancel, OnProgress, VerifyError))
				{
					Message(FString::Printf(TEXT("%s is already downloaded and verified"), *DestPath));
					return true;
				}
				if (bCancel)
				{
					OutError = TEXT("Cancelled");
					return false;
				}
				Message(FString::Printf(TEXT("existing %s is not valid (%s), downloading again"), *DestPath, *VerifyError));
				break;
			}
		}
		FM.Delete(*DestPath, false, true, true);
	}

	FString LastError = TEXT("No download source");
	for (int32 SourceIndex = 0; SourceIndex < Sources.Num(); ++SourceIndex)
	{
		const FArenaDlSource& Source = Sources[SourceIndex];
		// a .part only continues the same file
		FString Tag;
		FFileHelper::LoadFileToString(Tag, *TagPath);
		if (FM.FileExists(*PartPath) && Tag.TrimStartAndEnd() != PartTag(Source))
		{
			Message(TEXT("discarding a partial download of a different file"));
			FM.Delete(*PartPath, false, true, true);
		}
		FFileHelper::SaveStringToFile(PartTag(Source), *TagPath);
		if (FM.FileExists(*PartPath) && FM.FileSize(*PartPath) > Source.Size)
		{
			FM.Delete(*PartPath, false, true, true);
		}
		int64 Pos = FMath::Max<int64>(0, FM.FileSize(*PartPath));
		if (Pos > 0)
		{
			Message(FString::Printf(TEXT("resuming at %lld of %lld bytes from %s"), Pos, Source.Size, *HostOf(Source.Url)));
		}
		else
		{
			Message(FString::Printf(TEXT("downloading %lld bytes from %s"), Source.Size, *Source.Url));
		}

		// speed: samples of (time, position) over the last ~5 seconds
		TArray<TPair<double, int64>> Samples;
		auto Report = [&](int64 Done)
		{
			const double Now = FPlatformTime::Seconds();
			Samples.Add({ Now, Done });
			while (Samples.Num() > 2 && Now - Samples[0].Key > 5.0)
			{
				Samples.RemoveAt(0);
			}
			if (OnProgress)
			{
				FProgress P;
				P.Done = Done;
				P.Total = Source.Size;
				P.Url = Source.Url;
				const double Dt = Samples.Last().Key - Samples[0].Key;
				P.BytesPerSecond = Dt > 0.5 ? double(Samples.Last().Value - Samples[0].Value) / Dt : 0.0;
				OnProgress(P);
			}
		};

		int32 Failures = 0;
		while (Pos < Source.Size && Failures < 3)
		{
			if (bCancel)
			{
				OutError = TEXT("Cancelled");
				return false;
			}
			TUniquePtr<IFileHandle> File(PF.OpenWrite(*PartPath, true, false));
			if (!File)
			{
				OutError = FString::Printf(TEXT("Cannot write %s"), *PartPath);
				return false;
			}
			File->Seek(Pos);
			TSharedRef<FRequestState, ESPMode::ThreadSafe> State = MakeShared<FRequestState, ESPMode::ThreadSafe>();
			State->File = File.Get();
			State->Start = Pos;
			State->End = FMath::Min(Source.Size, Pos + ChunkSize);
			State->Total = Source.Size;

			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
			Request->SetURL(Source.Url);
			Request->SetVerb(TEXT("GET"));
			Request->SetHeader(TEXT("User-Agent"), UserAgent);
			Request->SetHeader(TEXT("Accept-Encoding"), TEXT("identity"));
			Request->SetHeader(TEXT("Range"), FString::Printf(TEXT("bytes=%lld-%lld"), State->Start, State->End - 1));
			Request->SetActivityTimeout(60.f);
			Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);
			Request->OnStatusCodeReceived().BindLambda([State](FHttpRequestPtr, int32 Code) { State->Status = Code; });
			Request->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda([State](void* Ptr, int64& InOutLength)
			{
				if (!State->Receive(static_cast<const uint8*>(Ptr), InOutLength))
				{
					InOutLength = 0;
				}
			}));
			Request->OnProcessRequestComplete().BindLambda([State](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				if (Response.IsValid())
				{
					if (State->Status == 0)
					{
						State->Status = Response->GetResponseCode();
					}
					FScopeLock Guard(&State->Lock);
					State->ContentRange = Response->GetHeader(TEXT("Content-Range"));
				}
				State->bConnected = bConnected;
				State->bDone = true;
				State->DoneEvent->Trigger();
			});
			if (!Request->ProcessRequest())
			{
				OutError = TEXT("Cannot start an HTTP request (is the HTTP module available?)");
				return false;
			}
			// wait, reporting progress, until the request ends or the user cancels
			bool bCancelled = false;
			while (!State->DoneEvent->Wait(FTimespan::FromMilliseconds(250)))
			{
				Report(Pos + State->Written);
				if (bCancel && !bCancelled)
				{
					bCancelled = true;
					Request->CancelRequest();
				}
			}
			{
				FScopeLock Guard(&State->Lock);
				State->File = nullptr;
			}
			const int32 Code = State->Status;
			const int64 Got = State->Written;
			if (Code != 200 && Code != 206 && Got > 0)
			{
				// status arrived late and was an error: drop what was written
				File->Truncate(Pos);
			}
			else
			{
				Pos += Got;
			}
			File->Flush();
			File.Reset();
			Report(Pos);
			if (bCancel)
			{
				OutError = TEXT("Cancelled");
				return false;
			}
			if (State->bWriteError)
			{
				OutError = FString::Printf(TEXT("Writing %s failed (disk full?)"), *PartPath);
				return false;
			}
			if (Got > 0 && (Code == 200 || Code == 206))
			{
				Failures = 0;
				continue;
			}
			++Failures;
			LastError = Code ? FString::Printf(TEXT("%s answered HTTP %d"), *HostOf(Source.Url), Code)
				: FString::Printf(TEXT("cannot connect to %s"), *HostOf(Source.Url));
			if (Code == 404 || Code == 403 || Code == 410 || Code == 416)
			{
				break; // not there: no point retrying this mirror
			}
			Message(FString::Printf(TEXT("%s, retrying (%d/3)"), *LastError, Failures));
			for (int32 I = 0; I < 8 && !bCancel; ++I)
			{
				FPlatformProcess::Sleep(0.25f);
			}
		}
		if (Pos < Source.Size)
		{
			if (SourceIndex + 1 < Sources.Num())
			{
				Message(FString::Printf(TEXT("%s; trying the next mirror"), *LastError));
			}
			continue;
		}

		FString VerifyError;
		if (!Verify(PartPath, Source, bCancel, OnProgress, VerifyError))
		{
			if (bCancel)
			{
				OutError = TEXT("Cancelled");
				return false;
			}
			LastError = FString::Printf(TEXT("download from %s is damaged (%s)"), *HostOf(Source.Url), *VerifyError);
			Message(LastError);
			FM.Delete(*PartPath, false, true, true);
			continue;
		}
		if (!FM.Move(*DestPath, *PartPath, true, true))
		{
			OutError = FString::Printf(TEXT("Cannot rename %s"), *PartPath);
			return false;
		}
		FM.Delete(*TagPath, false, true, true);
		Message(FString::Printf(TEXT("%s downloaded and verified (SHA-256 %s)"), *DestPath, *Source.Sha256));
		return true;
	}
	OutError = LastError;
	return false;
}
