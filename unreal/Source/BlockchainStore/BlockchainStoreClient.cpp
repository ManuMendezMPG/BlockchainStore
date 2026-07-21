// Vertical slice BlockchainStore - HTTP client implementation.

#include "BlockchainStoreClient.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/GameInstance.h"
#include "TimerManager.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlockchainStore, Log, All);

namespace
{
	// Tolerant field reading: the bridge might use "name" or "nombre", "id"
	// or "itemId", etc. This way parsing does not break due to a different field name.

	FString GetStringField(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys)
	{
		for (const TCHAR* Key : Keys)
		{
			const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
			if (Value.IsValid() && !Value->IsNull())
			{
				// Accepts both string and number (serializes it to text).
				if (Value->Type == EJson::String)
				{
					return Value->AsString();
				}
				if (Value->Type == EJson::Number)
				{
					// No decimals for integer wei-type prices.
					return FString::Printf(TEXT("%lld"), static_cast<int64>(Value->AsNumber()));
				}
			}
		}
		return FString();
	}

	int32 GetIntField(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys, int32 Default = 0)
	{
		for (const TCHAR* Key : Keys)
		{
			const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
			if (Value.IsValid() && !Value->IsNull())
			{
				if (Value->Type == EJson::Number)
				{
					return static_cast<int32>(Value->AsNumber());
				}
				if (Value->Type == EJson::String)
				{
					return FCString::Atoi(*Value->AsString());
				}
			}
		}
		return Default;
	}
}

// ---------------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------------

void UBlockchainStoreClient::SetWalletAddress(const FString& InAddress)
{
	WalletAddress = InAddress.TrimStartAndEnd();
	UE_LOG(LogBlockchainStore, Log, TEXT("WalletAddress set: %s"), *WalletAddress);
}

void UBlockchainStoreClient::SetBaseUrl(const FString& InUrl)
{
	FString Trimmed = InUrl.TrimStartAndEnd();
	// Remove the trailing slash so "/api/..." can be concatenated without double slashes.
	while (Trimmed.EndsWith(TEXT("/")))
	{
		Trimmed.LeftChopInline(1);
	}
	BaseUrl = Trimmed;
}

void UBlockchainStoreClient::Deinitialize()
{
	StopPolling();
	StopLoginPolling();
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------------
// Helpers HTTP / JSON
// ---------------------------------------------------------------------------------

TSharedRef<IHttpRequest, ESPMode::ThreadSafe> UBlockchainStoreClient::MakeRequest(const FString& Verb, const FString& Url) const
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	if (Verb == TEXT("POST") || Verb == TEXT("PUT"))
	{
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	}
	return Request;
}

bool UBlockchainStoreClient::TryParseJsonObject(FHttpResponsePtr Response, bool bSucceeded, TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
{
	if (!bSucceeded || !Response.IsValid())
	{
		OutError = TEXT("No response from the bridge (connection failed or server down).");
		return false;
	}

	const int32 Code = Response->GetResponseCode();
	const FString Body = Response->GetContentAsString();

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
	{
		OutError = FString::Printf(TEXT("Response is not valid JSON (HTTP %d): %s"), Code, *Body.Left(200));
		return false;
	}

	// The server may return {"error": "..."} even with a 200.
	FString ServerError;
	if (OutRoot->TryGetStringField(TEXT("error"), ServerError) && !ServerError.IsEmpty())
	{
		OutError = ServerError;
		return false;
	}

	if (Code < 200 || Code >= 300)
	{
		OutError = FString::Printf(TEXT("HTTP %d from the bridge: %s"), Code, *Body.Left(200));
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------------
// FetchCatalog -> GET /api/catalog
// ---------------------------------------------------------------------------------

void UBlockchainStoreClient::FetchCatalog()
{
	const FString Url = BaseUrl + TEXT("/api/catalog");
	UE_LOG(LogBlockchainStore, Log, TEXT("GET %s"), *Url);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("GET"), Url);
	Request->OnProcessRequestComplete().BindUObject(this, &UBlockchainStoreClient::HandleCatalogResponse);
	Request->ProcessRequest();
}

void UBlockchainStoreClient::HandleCatalogResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
{
	// Actual shape: {"items":[{"id":0,"name":"Espada","priceWei":"...","priceEth":"0.01"}, ...]}
	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!TryParseJsonObject(Response, bSucceeded, Root, Error))
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("FetchCatalog failed: %s"), *Error);
		OnCatalogUpdated.Broadcast(TArray<FStoreCatalogItem>());
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	TArray<FStoreCatalogItem> Items;
	if (Root->TryGetArrayField(TEXT("items"), Array) && Array)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FStoreCatalogItem Item;
			Item.ItemId = GetIntField(Obj, { TEXT("id") });
			Item.Name = GetStringField(Obj, { TEXT("name") });
			Item.PriceWei = GetStringField(Obj, { TEXT("priceWei") }); // huge string, stored as-is
			Item.PriceEth = GetStringField(Obj, { TEXT("priceEth") }); // decimal string "0.01"
			Items.Add(Item);
		}
	}

	UE_LOG(LogBlockchainStore, Log, TEXT("Catalog received: %d items"), Items.Num());
	OnCatalogUpdated.Broadcast(Items);
}

// ---------------------------------------------------------------------------------
// FetchInventory -> GET /api/inventory?address=...
// ---------------------------------------------------------------------------------

void UBlockchainStoreClient::FetchInventory()
{
	if (WalletAddress.IsEmpty())
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("FetchInventory: WalletAddress is empty. Call SetWalletAddress first."));
		OnInventoryUpdated.Broadcast(TArray<FStoreInventoryEntry>());
		return;
	}

	const FString Url = BaseUrl + TEXT("/api/inventory?address=") + WalletAddress;
	UE_LOG(LogBlockchainStore, Log, TEXT("GET %s"), *Url);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("GET"), Url);
	Request->OnProcessRequestComplete().BindUObject(this, &UBlockchainStoreClient::HandleInventoryResponse);
	Request->ProcessRequest();
}

void UBlockchainStoreClient::HandleInventoryResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
{
	// Actual shape: {"address":"0x..","items":[{"id":0,"name":"Espada","quantity":"14"}, ...]}
	// NOTE: "quantity" arrives as a STRING ("14"); GetIntField converts it with Atoi.
	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!TryParseJsonObject(Response, bSucceeded, Root, Error))
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("FetchInventory failed: %s"), *Error);
		OnInventoryUpdated.Broadcast(TArray<FStoreInventoryEntry>());
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	TArray<FStoreInventoryEntry> Entries;
	if (Root->TryGetArrayField(TEXT("items"), Array) && Array)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FStoreInventoryEntry Entry;
			Entry.ItemId = GetIntField(Obj, { TEXT("id") });
			Entry.Name = GetStringField(Obj, { TEXT("name") });
			Entry.Balance = GetIntField(Obj, { TEXT("quantity") }); // "14" (string) -> 14
			Entries.Add(Entry);
		}
	}

	UE_LOG(LogBlockchainStore, Log, TEXT("Inventory received: %d entries"), Entries.Num());
	OnInventoryUpdated.Broadcast(Entries);
}

// ---------------------------------------------------------------------------------
// FetchProgress -> GET /api/progress?address=...
//
// The Achievements contract has 3 fixed medals (Arquero 0, Mercader 1 with
// rarity, Coleccionista 2). The bridge returns the ones the address OWNS (+ the
// Mercader's rarity); here we cross that response with the canonical list of 3 to
// ALWAYS emit all three, marked owned/locked, so the UI does not need to know the
// medal catalog.
// ---------------------------------------------------------------------------------

void UBlockchainStoreClient::FetchProgress()
{
	if (WalletAddress.IsEmpty())
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("FetchProgress: WalletAddress is empty. Call SetWalletAddress first."));
		OnProgressUpdated.Broadcast(TArray<FStoreMedal>());
		return;
	}

	const FString Url = BaseUrl + TEXT("/api/progress?address=") + WalletAddress;
	UE_LOG(LogBlockchainStore, Log, TEXT("GET %s"), *Url);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("GET"), Url);
	Request->OnProcessRequestComplete().BindUObject(this, &UBlockchainStoreClient::HandleProgressResponse);
	Request->ProcessRequest();
}

void UBlockchainStoreClient::HandleProgressResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
{
	// Canonical medal catalog (id -> name). FALLBACK ONLY: the display name comes from
	// the bridge's "name" field; this is used just for BuildDefault and for any entry
	// where the bridge omits "name". Kept in English for consistency with the bridge.
	static const TCHAR* MedalNames[] = { TEXT("Archer"), TEXT("Merchant"), TEXT("Collector") };
	constexpr int32 NumMedals = 3;

	// Default output: all 3 locked. Filled in with whatever the bridge returns.
	auto BuildDefault = []() -> TArray<FStoreMedal>
	{
		TArray<FStoreMedal> Out;
		for (int32 i = 0; i < NumMedals; ++i)
		{
			FStoreMedal M;
			M.MedalId = i;
			M.Name = MedalNames[i];
			M.bOwned = false;
			Out.Add(M);
		}
		return Out;
	};

	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!TryParseJsonObject(Response, bSucceeded, Root, Error))
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("FetchProgress failed: %s"), *Error);
		OnProgressUpdated.Broadcast(BuildDefault());
		return;
	}

	TArray<FStoreMedal> Medals = BuildDefault();

	// Expected shape: {"address":"0x..","medals":[{"id":1,"owned":true,"rarity":"Gold"}, ...]}
	// Tolerant: "owned" absent => assumed owned (it comes in the list of owned ones);
	// "rarity"/"rareza" is only used for the Mercader.
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Root->TryGetArrayField(TEXT("medals"), Array) && Array)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}

			const int32 Id = GetIntField(Obj, { TEXT("id"), TEXT("medalId") }, -1);
			if (!Medals.IsValidIndex(Id))
			{
				continue; // id outside the canonical catalog
			}

			// The bridge is the single source of truth for the display name (English).
			// Only fall back to MedalNames[] when the JSON omits "name".
			const FString Name = GetStringField(Obj, { TEXT("name") });
			if (!Name.IsEmpty())
			{
				Medals[Id].Name = Name;
			}

			bool bOwned = true; // present in the list => owned unless stated otherwise
			Obj->TryGetBoolField(TEXT("owned"), bOwned);
			Medals[Id].bOwned = bOwned;

			if (bOwned)
			{
				Medals[Id].Rarity = GetStringField(Obj, { TEXT("rarity"), TEXT("rareza") });
			}
		}
	}

	UE_LOG(LogBlockchainStore, Log, TEXT("Progress received: %d/%d medals unlocked"),
		Medals.FilterByPredicate([](const FStoreMedal& M) { return M.bOwned; }).Num(), NumMedals);
	OnProgressUpdated.Broadcast(Medals);
}

// ---------------------------------------------------------------------------------
// BuyItem -> POST /api/purchase-intent + polling of /api/purchase-status
// ---------------------------------------------------------------------------------

void UBlockchainStoreClient::BuyItem(int32 ItemId, int32 Quantity)
{
	if (WalletAddress.IsEmpty())
	{
		OnPurchaseFailed.Broadcast(TEXT("No wallet connected (WalletAddress is empty)."));
		return;
	}
	if (bPurchaseInProgress)
	{
		OnPurchaseFailed.Broadcast(TEXT("A purchase is already in progress; wait for it to finish."));
		return;
	}
	if (Quantity < 1)
	{
		Quantity = 1;
	}

	// Build the JSON body: {address, itemId, quantity}.
	const TSharedRef<FJsonObject> BodyObj = MakeShared<FJsonObject>();
	BodyObj->SetStringField(TEXT("address"), WalletAddress);
	BodyObj->SetNumberField(TEXT("itemId"), ItemId);
	BodyObj->SetNumberField(TEXT("quantity"), Quantity);

	FString BodyStr;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(BodyObj, Writer);

	const FString Url = BaseUrl + TEXT("/api/purchase-intent");
	UE_LOG(LogBlockchainStore, Log, TEXT("POST %s body=%s"), *Url, *BodyStr);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("POST"), Url);
	Request->SetContentAsString(BodyStr);
	Request->OnProcessRequestComplete().BindUObject(this, &UBlockchainStoreClient::HandlePurchaseIntentResponse);

	bPurchaseInProgress = true;
	CurrentRequestId.Empty();
	LastReportedStatus.Empty();
	PollAttempts = 0;

	Request->ProcessRequest();
}

void UBlockchainStoreClient::HandlePurchaseIntentResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
{
	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!TryParseJsonObject(Response, bSucceeded, Root, Error))
	{
		bPurchaseInProgress = false;
		UE_LOG(LogBlockchainStore, Warning, TEXT("purchase-intent failed: %s"), *Error);
		OnPurchaseFailed.Broadcast(Error);
		return;
	}

	CurrentRequestId = GetStringField(Root, { TEXT("requestId") });
	const FString Status = GetStringField(Root, { TEXT("status") });

	if (CurrentRequestId.IsEmpty())
	{
		bPurchaseInProgress = false;
		OnPurchaseFailed.Broadcast(TEXT("The bridge did not return a requestId."));
		return;
	}

	UE_LOG(LogBlockchainStore, Log, TEXT("purchase-intent OK requestId=%s status=%s"), *CurrentRequestId, *Status);

	// Emit the first state and start polling.
	LastReportedStatus = Status.IsEmpty() ? TEXT("pending") : Status;
	OnPurchaseStateChanged.Broadcast(CurrentRequestId, LastReportedStatus);

	UGameInstance* GI = GetGameInstance();
	if (!GI)
	{
		bPurchaseInProgress = false;
		OnPurchaseFailed.Broadcast(TEXT("No GameInstance to start polling."));
		return;
	}

	// Repeating timer on the GameInstance's TimerManager (survives map changes).
	GI->GetTimerManager().SetTimer(
		PollTimerHandle, this, &UBlockchainStoreClient::PollPurchaseStatus,
		PollIntervalSeconds, /*bLoop=*/true, /*FirstDelay=*/PollIntervalSeconds);
}

// ---------------------------------------------------------------------------------
// Polling: every PollIntervalSeconds fires a GET /api/purchase-status.
//
// FTimerHandle + FTimerManager::SetTimer(..., bLoop=true) schedules a recurring
// call to PollPurchaseStatus. Each tick launches ONE asynchronous HTTP request; its
// callback (HandlePurchaseStatusResponse) decides whether to continue or stop. When
// done/error arrives or MaxPollAttempts is exhausted, ClearTimer stops the cycle.
// The game thread is never blocked: the timer only "wakes up" to launch the next
// request.
// ---------------------------------------------------------------------------------

void UBlockchainStoreClient::PollPurchaseStatus()
{
	if (CurrentRequestId.IsEmpty())
	{
		StopPolling();
		return;
	}

	if (++PollAttempts > MaxPollAttempts)
	{
		StopPolling();
		bPurchaseInProgress = false;
		UE_LOG(LogBlockchainStore, Warning, TEXT("purchase-status timeout after %d attempts"), MaxPollAttempts);
		OnPurchaseFailed.Broadcast(TEXT("Timed out waiting for purchase confirmation."));
		return;
	}

	const FString Url = BaseUrl + TEXT("/api/purchase-status?requestId=") + CurrentRequestId;
	UE_LOG(LogBlockchainStore, Verbose, TEXT("poll #%d GET %s"), PollAttempts, *Url);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("GET"), Url);
	Request->OnProcessRequestComplete().BindUObject(this, &UBlockchainStoreClient::HandlePurchaseStatusResponse);
	Request->ProcessRequest();
}

void UBlockchainStoreClient::HandlePurchaseStatusResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
{
	// NOTE: We do NOT use TryParseJsonObject here. On this endpoint "error" is a
	// normal data field (null on success, message when status=="error"), always with
	// HTTP 200. The actual shape is:
	//   {"status":"done","txHash":"0x..","error":null,"itemId":0,"quantity":1}
	// We only treat a missing response or a non-JSON body as a "transient network
	// failure" (retryable).
	if (!bSucceeded || !Response.IsValid())
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("poll #%d no response (will retry)"), PollAttempts);
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const FString Body = Response->GetContentAsString();
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("poll #%d invalid JSON (will retry): %s"), PollAttempts, *Body.Left(200));
		return;
	}

	const FString Status = GetStringField(Root, { TEXT("status") }).ToLower();
	// txHash may come back null (in pending/signing); GetStringField returns "" if null.
	const FString TxHash = GetStringField(Root, { TEXT("txHash") });

	// Notify only when the state changes, to avoid spamming the UI.
	if (!Status.IsEmpty() && Status != LastReportedStatus)
	{
		LastReportedStatus = Status;
		OnPurchaseStateChanged.Broadcast(CurrentRequestId, Status);
	}

	if (Status == TEXT("done"))
	{
		StopPolling();
		bPurchaseInProgress = false;
		UE_LOG(LogBlockchainStore, Log, TEXT("Purchase completed. txHash=%s"), *TxHash);
		OnPurchaseCompleted.Broadcast(TxHash);
		// Refresh the inventory after the purchase.
		FetchInventory();
	}
	else if (Status == TEXT("error"))
	{
		StopPolling();
		bPurchaseInProgress = false;
		FString Reason = GetStringField(Root, { TEXT("error") });
		if (Reason.IsEmpty())
		{
			Reason = TEXT("The purchase failed on the bridge.");
		}
		UE_LOG(LogBlockchainStore, Warning, TEXT("Purchase failed: %s"), *Reason);
		OnPurchaseFailed.Broadcast(Reason);
	}
	// pending / signing -> keep polling.
}

void UBlockchainStoreClient::StopPolling()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(PollTimerHandle);
	}
	PollTimerHandle.Invalidate();
}

// ---------------------------------------------------------------------------------
// Login SIWE -> POST /api/siwe/login-intent + polling of /api/siwe/login-status
//
// Same skeleton as BuyItem: an "intent" call that returns a requestId, then a
// repeating FTimerHandle (same PollIntervalSeconds/MaxPollAttempts) that polls the
// state until done/error/timeout. Its own state (LoginRequestId, LastLoginStatus,
// LoginPollTimerHandle, LoginPollAttempts) so as not to clobber the purchase state:
// login and purchase could overlap and each has its own independent polling cycle.
// ---------------------------------------------------------------------------------

void UBlockchainStoreClient::Login(const FString& Address)
{
	const FString CleanAddress = Address.TrimStartAndEnd();
	if (CleanAddress.IsEmpty())
	{
		OnLoginFailed.Broadcast(TEXT("Cannot start login: address is empty."));
		return;
	}
	if (bLoginInProgress)
	{
		OnLoginFailed.Broadcast(TEXT("A login is already in progress; wait for it to finish."));
		return;
	}

	// Build the JSON body: {address}.
	const TSharedRef<FJsonObject> BodyObj = MakeShared<FJsonObject>();
	BodyObj->SetStringField(TEXT("address"), CleanAddress);

	FString BodyStr;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(BodyObj, Writer);

	const FString Url = BaseUrl + TEXT("/api/siwe/login-intent");
	UE_LOG(LogBlockchainStore, Log, TEXT("POST %s body=%s"), *Url, *BodyStr);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("POST"), Url);
	Request->SetContentAsString(BodyStr);
	Request->OnProcessRequestComplete().BindUObject(this, &UBlockchainStoreClient::HandleLoginIntentResponse);

	bLoginInProgress = true;
	LoginRequestId.Empty();
	LastLoginStatus.Empty();
	PendingLoginAddress = CleanAddress;
	LoginPollAttempts = 0;

	Request->ProcessRequest();
}

void UBlockchainStoreClient::HandleLoginIntentResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
{
	// Actual shape: {"requestId":"...","message":"..."}. The "message" is the SIWE text
	// that the web will have signed in MetaMask; Unreal does not need it, only the requestId.
	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!TryParseJsonObject(Response, bSucceeded, Root, Error))
	{
		bLoginInProgress = false;
		UE_LOG(LogBlockchainStore, Warning, TEXT("login-intent failed: %s"), *Error);
		OnLoginFailed.Broadcast(Error);
		return;
	}

	LoginRequestId = GetStringField(Root, { TEXT("requestId") });
	if (LoginRequestId.IsEmpty())
	{
		bLoginInProgress = false;
		OnLoginFailed.Broadcast(TEXT("The bridge did not return a requestId for the login."));
		return;
	}

	UE_LOG(LogBlockchainStore, Log, TEXT("login-intent OK requestId=%s"), *LoginRequestId);

	// Initial state: waiting for the signature in the web/MetaMask.
	LastLoginStatus = TEXT("pending");
	OnLoginStateChanged.Broadcast(LastLoginStatus);

	UGameInstance* GI = GetGameInstance();
	if (!GI)
	{
		bLoginInProgress = false;
		OnLoginFailed.Broadcast(TEXT("No GameInstance to start the login polling."));
		return;
	}

	GI->GetTimerManager().SetTimer(
		LoginPollTimerHandle, this, &UBlockchainStoreClient::PollLoginStatus,
		PollIntervalSeconds, /*bLoop=*/true, /*FirstDelay=*/PollIntervalSeconds);
}

void UBlockchainStoreClient::PollLoginStatus()
{
	if (LoginRequestId.IsEmpty())
	{
		StopLoginPolling();
		return;
	}

	if (++LoginPollAttempts > MaxPollAttempts)
	{
		StopLoginPolling();
		bLoginInProgress = false;
		UE_LOG(LogBlockchainStore, Warning, TEXT("login-status timeout after %d attempts"), MaxPollAttempts);
		OnLoginFailed.Broadcast(TEXT("Timed out waiting for the login signature."));
		return;
	}

	const FString Url = BaseUrl + TEXT("/api/siwe/login-status?requestId=") + LoginRequestId;
	UE_LOG(LogBlockchainStore, Verbose, TEXT("login poll #%d GET %s"), LoginPollAttempts, *Url);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("GET"), Url);
	Request->OnProcessRequestComplete().BindUObject(this, &UBlockchainStoreClient::HandleLoginStatusResponse);
	Request->ProcessRequest();
}

void UBlockchainStoreClient::HandleLoginStatusResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
{
	// Same as purchase-status: "error" is a data field (message when
	// status=="error"), not an HTTP failure. We only treat a missing response or
	// invalid JSON as a retryable failure.
	// Actual shape: {"status":"pending|signing|done|error","address":"0x..","error":null}
	if (!bSucceeded || !Response.IsValid())
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("login poll #%d no response (will retry)"), LoginPollAttempts);
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const FString Body = Response->GetContentAsString();
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("login poll #%d invalid JSON (will retry): %s"), LoginPollAttempts, *Body.Left(200));
		return;
	}

	const FString Status = GetStringField(Root, { TEXT("status") }).ToLower();

	// Notify only when the state changes, to avoid spamming the UI.
	if (!Status.IsEmpty() && Status != LastLoginStatus)
	{
		LastLoginStatus = Status;
		OnLoginStateChanged.Broadcast(Status);
	}

	if (Status == TEXT("done"))
	{
		StopLoginPolling();
		bLoginInProgress = false;

		// The status carries the verified address; otherwise we use the one we sent in the intent.
		FString VerifiedAddress = GetStringField(Root, { TEXT("address") });
		if (VerifiedAddress.IsEmpty())
		{
			VerifiedAddress = PendingLoginAddress;
		}

		// UNIFICATION: the active wallet becomes the authenticated one, so that
		// FetchInventory/BuyItem operate with that wallet from now on.
		AuthenticatedAddress = VerifiedAddress;
		WalletAddress = VerifiedAddress;
		bIsAuthenticated = true;

		UE_LOG(LogBlockchainStore, Log, TEXT("SIWE login completed. address=%s"), *VerifiedAddress);
		OnLoginCompleted.Broadcast(VerifiedAddress);
	}
	else if (Status == TEXT("error"))
	{
		StopLoginPolling();
		bLoginInProgress = false;
		FString Reason = GetStringField(Root, { TEXT("error") });
		if (Reason.IsEmpty())
		{
			Reason = TEXT("Login failed on the bridge.");
		}
		UE_LOG(LogBlockchainStore, Warning, TEXT("Login failed: %s"), *Reason);
		OnLoginFailed.Broadcast(Reason);
	}
	// pending / signing -> keep polling.
}

void UBlockchainStoreClient::StopLoginPolling()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(LoginPollTimerHandle);
	}
	LoginPollTimerHandle.Invalidate();
}

// ---------------------------------------------------------------------------------
// Logout: clears the authentication. SIWE is stateless, no need to notify the bridge.
// ---------------------------------------------------------------------------------

void UBlockchainStoreClient::Logout()
{
	// Cancel any polling/timer in progress (login or purchase) so as not to receive
	// callbacks after closing the session.
	StopPolling();
	StopLoginPolling();
	bPurchaseInProgress = false;
	bLoginInProgress = false;
	CurrentRequestId.Empty();
	LoginRequestId.Empty();
	PendingLoginAddress.Empty();

	// Clear the authentication state and the active wallet.
	bIsAuthenticated = false;
	AuthenticatedAddress.Empty();
	WalletAddress.Empty();

	UE_LOG(LogBlockchainStore, Log, TEXT("Logout: session closed, client ready for a new login."));
}
