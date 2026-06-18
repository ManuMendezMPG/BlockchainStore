// Vertical slice BlockchainStore - cliente HTTP hacia el bridge.
//
// POR QUE UN GameInstanceSubsystem (y no un UObject suelto ni un Actor):
// - Vive durante toda la sesion de juego y SOBREVIVE a los cambios de nivel.
//   El estado importante (address de la wallet, BaseUrl, requestId en curso,
//   el FTimerHandle del polling) no se pierde al cargar otro mapa.
// - Lo instancia y lo destruye el engine; nadie tiene que hacer NewObject ni
//   guardarlo en un UPROPERTY para que el GC no se lo lleve.
// - Es accesible desde CUALQUIER Blueprint con un solo nodo
//   "Get GameInstance Subsystem -> BlockchainStoreClient", sin punteros que cablear.
// - Tiene acceso al GameInstance, y por tanto a un FTimerManager estable para el
//   polling (mas estable que el del World, que se recrea en cada cambio de mapa).
//
// Un UObject suelto exigiria gestionarle el ciclo de vida a mano y un World valido
// para los timers; un Actor obligaria a tenerlo colocado en un nivel. El subsystem
// evita ambos problemas para una "tuberia" global como esta.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "BlockchainStoreTypes.h"
#include "BlockchainStoreClient.generated.h"

// --- Delegates dinamicos multicast -------------------------------------------------
// POR QUE DINAMICOS (DECLARE_DYNAMIC_MULTICAST_*):
// - "Dynamic"  -> son asignables desde Blueprint (aparecen como nodos rojos "Bind
//   Event" / "Assign" y como pin de evento). Un delegate normal (no-dynamic) NO se
//   puede enganchar desde Blueprint.
// - "Multicast" -> varios widgets/objetos pueden suscribirse al mismo evento.
// - Marcados con UPROPERTY(BlueprintAssignable) en la clase para que la UI los vea.
// Asi la logica de red (este C++) no conoce a la UI: solo emite eventos y quien
// quiera reacciona. Desacople total entre tuberia y presentacion.

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCatalogUpdated, const TArray<FStoreCatalogItem>&, Items);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInventoryUpdated, const TArray<FStoreInventoryEntry>&, Items);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPurchaseStateChanged, const FString&, RequestId, const FString&, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPurchaseCompleted, const FString&, TxHash);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPurchaseFailed, const FString&, Reason);

/**
 * Cliente del BlockchainStore bridge. Toda la comunicacion HTTP con
 * http://localhost:8787 pasa por aqui. La firma real ocurre en el navegador
 * (MetaMask); Unreal solo dispara intents y hace polling del estado.
 */
UCLASS(BlueprintType)
class BLOCKCHAINSTORE_API UBlockchainStoreClient : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// ---- Configuracion -----------------------------------------------------------

	/** URL base del bridge. Editable; por defecto el bridge local. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BlockchainStore|Config")
	FString BaseUrl = TEXT("http://localhost:8787");

	/** Address de la wallet del jugador. De momento set manual; el login SIWE vendra despues. */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore|Config")
	FString WalletAddress;

	UFUNCTION(BlueprintCallable, Category = "BlockchainStore|Config")
	void SetWalletAddress(const FString& InAddress);

	UFUNCTION(BlueprintCallable, Category = "BlockchainStore|Config")
	void SetBaseUrl(const FString& InUrl);

	/** True si hay una compra en curso (intent enviado, polling activo). */
	UFUNCTION(BlueprintPure, Category = "BlockchainStore")
	bool IsPurchaseInProgress() const { return bPurchaseInProgress; }

	// ---- Acciones expuestas a Blueprint ------------------------------------------

	/** GET /api/catalog. Al completarse dispara OnCatalogUpdated. */
	UFUNCTION(BlueprintCallable, Category = "BlockchainStore")
	void FetchCatalog();

	/** GET /api/inventory?address=<WalletAddress>. Al completarse dispara OnInventoryUpdated. */
	UFUNCTION(BlueprintCallable, Category = "BlockchainStore")
	void FetchInventory();

	/**
	 * POST /api/purchase-intent y arranca el polling de estado.
	 * Emite OnPurchaseStateChanged en cada cambio, OnPurchaseCompleted(txHash) al
	 * terminar bien, OnPurchaseFailed(motivo) si error o timeout. Refresca inventario
	 * automaticamente al completar.
	 */
	UFUNCTION(BlueprintCallable, Category = "BlockchainStore")
	void BuyItem(int32 ItemId, int32 Quantity = 1);

	// ---- Eventos para la UI (BlueprintAssignable) --------------------------------

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnCatalogUpdated OnCatalogUpdated;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnInventoryUpdated OnInventoryUpdated;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnPurchaseStateChanged OnPurchaseStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnPurchaseCompleted OnPurchaseCompleted;

	UPROPERTY(BlueprintAssignable, Category = "BlockchainStore|Events")
	FOnPurchaseFailed OnPurchaseFailed;

	// ---- USubsystem --------------------------------------------------------------
	virtual void Deinitialize() override;

private:
	// ---- Helpers HTTP ------------------------------------------------------------

	/** Crea un request con metodo y URL ya puestos (y Content-Type json si hay body). */
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MakeRequest(const FString& Verb, const FString& Url) const;

	/** Devuelve true y rellena OutRoot si Response es 2xx y el body es JSON valido.
	 *  Si el body trae {"error": "..."} o el codigo no es 2xx, devuelve false y
	 *  rellena OutError con un motivo legible. */
	static bool TryParseJsonObject(FHttpResponsePtr Response, bool bSucceeded, TSharedPtr<class FJsonObject>& OutRoot, FString& OutError);

	// ---- Callbacks de respuesta --------------------------------------------------
	void HandleCatalogResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void HandleInventoryResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void HandlePurchaseIntentResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
	void HandlePurchaseStatusResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);

	// ---- Polling -----------------------------------------------------------------
	void PollPurchaseStatus();
	void StopPolling();

	// Estado de la compra en curso.
	FString CurrentRequestId;
	FString LastReportedStatus;
	FTimerHandle PollTimerHandle;
	int32 PollAttempts = 0;
	bool bPurchaseInProgress = false;

	// Polling cada 1.5s, hasta 40 intentos => ~60s de timeout antes de rendirse.
	static constexpr float PollIntervalSeconds = 1.5f;
	static constexpr int32 MaxPollAttempts = 40;
};
