// Vertical slice BlockchainStore - HTTP client to the bridge.
//
// WHY A GameInstanceSubsystem (and not a loose UObject or an Actor):
// - It lives for the entire game session and SURVIVES level changes.
//   The important state (wallet address, BaseUrl, in-flight requestId,
//   the polling FTimerHandle) is not lost when loading another map.
// - The engine instantiates and destroys it; nobody has to call NewObject or
//   store it in a UPROPERTY so the GC does not collect it.
// - It is accessible from ANY Blueprint with a single node
//   "Get GameInstance Subsystem -> BlockchainStoreClient", with no pointers to wire up.
// - It has access to the GameInstance, and therefore to a stable FTimerManager for
//   polling (more stable than the World's, which is recreated on every map change).
//
// A loose UObject would require managing its lifecycle by hand and a valid World
// for the timers; an Actor would require placing it in a level. The subsystem
// avoids both problems for a global "pipeline" like this one.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "BlockchainStoreTypes.h"
#include "BlockchainStoreClient.generated.h"

// --- Dynamic multicast delegates ---------------------------------------------------
// WHY DYNAMIC (DECLARE_DYNAMIC_MULTICAST_*):
// - "Dynamic"  -> they are assignable from Blueprint (they appear as red "Bind
//   Event" / "Assign" nodes and as an event pin). A normal (non-dynamic) delegate
//   CANNOT be hooked up from Blueprint.
// - "Multicast" -> several widgets/objects can subscribe to the same event.
// - Marked with UPROPERTY(BlueprintAssignable) in the class so the UI sees them.
// This way the network logic (this C++) does not know about the UI: it only emits
// events and whoever wants reacts. Full decoupling between pipeline and presentation.

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCatalogUpdated, const TArray<FStoreCatalogItem>&, Items);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInventoryUpdated, const TArray<FStoreInventoryEntry>&, Items);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnProgressUpdated, const TArray<FStoreMedal>&, Medals);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPurchaseStateChanged, const FString&, RequestId, const FString&, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPurchaseCompleted, const FString&, TxHash);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPurchaseFailed, const FString&, Reason);

// SIWE login: same pattern as purchases (intent + status polling). The signature
// (personal_sign) happens in the web+MetaMask; Unreal only launches the intent and polls.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLoginStateChanged, const FString&, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLoginCompleted, const FString&, Address);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLoginFailed, const FString&, Reason);

/**
 * BlockchainStore bridge client. All HTTP communication with
 * http://localhost:8787 goes through here. The actual signature happens in the browser
 * (MetaMask); Unreal only fires intents and polls the status.
 */
UCLASS(BlueprintType)
class BLOCKCHAINSTORE_API UBlockchainStoreClient : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// ---- Configuration -----------------------------------------------------------

	/** Base URL of the bridge. Editable; defaults to the local bridge. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BlockchainStore|Config")
	FString BaseUrl = TEXT("http://localhost:8787");

	/**
	 * Address of the active wallet used by FetchInventory/BuyItem. It can be set
	 * manually with SetWalletAddress, but after a successful SIWE login it becomes UNIFIED
	 * with AuthenticatedAddress: subsequent operations use the authenticated wallet.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore|Config")
	FString WalletAddress;

	/** Address verified by SIWE (empty if nobody has logged in). Source of truth for the login. */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore|Auth")
	FString AuthenticatedAddress;

	UFUNCTION(BlueprintCallable, Category = "BlockchainStore|Config")
	void SetWalletAddress(const FString& InAddress);

	UFUNCTION(BlueprintCallable, Category = "BlockchainStore|Config")
	void SetBaseUrl(const FString& InUrl);

	/** True if there is a purchase in progress (intent sent, polling active). */
	UFUNCTION(BlueprintPure, Category = "BlockchainStore")
	bool IsPurchaseInProgress() const { return bPurchaseInProgress; }

	/** True if there is a SIWE login in progress (intent sent, polling active). */
	UFUNCTION(BlueprintPure, Category = "BlockchainStore|Auth")
	bool IsLoginInProgress() const { return bLoginInProgress; }

	/** True when a SIWE login completed successfully. */
	UFUNCTION(BlueprintPure, Category = "BlockchainStore|Auth")
	bool IsAuthenticated() const { return bIsAuthenticated; }

	/** Address authenticated by SIWE (empty if nobody has logged in). */
	UFUNCTION(BlueprintPure, Category = "BlockchainStore|Auth")
	FString GetAuthenticatedAddress() const { return AuthenticatedAddress; }

	// ---- Actions exposed to Blueprint --------------------------------------------

	/** GET /api/catalog. On completion fires OnCatalogUpdated. */
	UFUNCTION(BlueprintCallable, Category = "BlockchainStore")
	void FetchCatalog();

	/** GET /api/inventory?address=<WalletAddress>. On completion fires OnInventoryUpdated. */
	UFUNCTION(BlueprintCallable, Category = "BlockchainStore")
	void FetchInventory();

	/** GET /api/progress?address=<WalletAddress>. On completion fires OnProgressUpdated
	 *  with the 3 medals (earned/locked) and the Merchant rarity if applicable. */
	UFUNCTION(BlueprintCallable, Category = "BlockchainStore")
	void FetchProgress();

	/**
	 * POST /api/purchase-intent and starts the status polling.
	 * Emits OnPurchaseStateChanged on every change, OnPurchaseCompleted(txHash) on
	 * successful completion, OnPurchaseFailed(reason) on error or timeout. Refreshes the
	 * inventory automatically on completion.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlockchainStore")
	void BuyItem(int32 ItemId, int32 Quantity = 1);

	/**
	 * SIWE login. POST /api/siwe/login-intent with the address and starts polling
	 * /api/siwe/login-status. The signature happens in the web+MetaMask; Unreal does not touch keys.
	 * Emits OnLoginStateChanged(status) on pending/signing, OnLoginCompleted(address) on
	 * verification (setting AuthenticatedAddress and WalletAddress), and OnLoginFailed(reason)
	 * on error or timeout.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlockchainStore|Auth")
	void Login(const FString& Address);

	/**
	 * Closes the SIWE session: sets IsAuthenticated to false, clears
	 * AuthenticatedAddress/WalletAddress and cancels any in-flight login or purchase
	 * polling/timer. It does NOT call the bridge (SIWE is stateless: each login is a new
	 * signature). After this, the client is ready for a new Login.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlockchainStore|Auth")
	void Logout();

	// ---- Events for the UI (BlueprintAssignable) ---------------------------------

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnCatalogUpdated OnCatalogUpdated;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnInventoryUpdated OnInventoryUpdated;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnProgressUpdated OnProgressUpdated;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnPurchaseStateChanged OnPurchaseStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnPurchaseCompleted OnPurchaseCompleted;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnPurchaseFailed OnPurchaseFailed;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnLoginStateChanged OnLoginStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnLoginCompleted OnLoginCompleted;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnLoginFailed OnLoginFailed;

	// ---- USubsystem --------------------------------------------------------------
	virtual void Deinitialize() override;

private:
	// ---- HTTP helpers ------------------------------------------------------------

	/** Creates a request with method and URL already set (and Content-Type json if there is a body). */
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MakeRequest(const FString& Verb, const FString& Url) const;

	/** Returns true and fills OutRoot if Response is 2xx and the body is valid JSON.
	 *  If the body carries {"error": "..."} or the code is not 2xx, returns false and
	 *  fills OutError with a readable reason. */
	static bool TryParseJsonObject(FHttpResponsePtr Response, bool bSucceeded, TSharedPtr<class FJsonObject>& OutRoot, FString& OutError);

	// ---- Response callbacks ------------------------------------------------------
	void HandleCatalogResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void HandleInventoryResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void HandleProgressResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void HandlePurchaseIntentResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void HandlePurchaseStatusResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void HandleLoginIntentResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void HandleLoginStatusResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);

	// ---- Polling -----------------------------------------------------------------
	void PollPurchaseStatus();
	void StopPolling();
	void PollLoginStatus();
	void StopLoginPolling();

	// State of the in-flight purchase.
	FString CurrentRequestId;
	FString LastReportedStatus;
	FTimerHandle PollTimerHandle;
	int32 PollAttempts = 0;
	bool bPurchaseInProgress = false;

	// State of the in-flight SIWE login (parallel to purchases, same polling pattern).
	FString LoginRequestId;
	FString LastLoginStatus;
	FString PendingLoginAddress; // address sent in the intent; fallback if the status does not carry it.
	FTimerHandle LoginPollTimerHandle;
	int32 LoginPollAttempts = 0;
	bool bLoginInProgress = false;
	bool bIsAuthenticated = false;

	// Polling every 1.5s, up to 40 attempts => ~60s timeout before giving up.
	// Shared by purchases and login (the user has to sign in MetaMask).
	static constexpr float PollIntervalSeconds = 1.5f;
	static constexpr int32 MaxPollAttempts = 40;
};
