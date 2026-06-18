// Vertical slice BlockchainStore - implementacion del cliente HTTP.

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
	// Lectura tolerante de campos: el bridge podria usar "name" o "nombre", "id"
	// o "itemId", etc. Asi el parseo no se rompe por un nombre de campo distinto.

	FString GetStringField(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys)
	{
		for (const TCHAR* Key : Keys)
		{
			const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
			if (Value.IsValid() && !Value->IsNull())
			{
				// Acepta tanto string como numero (lo serializa a texto).
				if (Value->Type == EJson::String)
				{
					return Value->AsString();
				}
				if (Value->Type == EJson::Number)
				{
					// Sin decimales para precios enteros tipo wei.
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
	UE_LOG(LogBlockchainStore, Log, TEXT("WalletAddress establecida: %s"), *WalletAddress);
}

void UBlockchainStoreClient::SetBaseUrl(const FString& InUrl)
{
	FString Trimmed = InUrl.TrimStartAndEnd();
	// Quita la barra final para poder concatenar "/api/..." sin dobles barras.
	while (Trimmed.EndsWith(TEXT("/")))
	{
		Trimmed.LeftChopInline(1);
	}
	BaseUrl = Trimmed;
}

void UBlockchainStoreClient::Deinitialize()
{
	StopPolling();
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
		OutError = TEXT("Sin respuesta del bridge (conexion fallida o servidor caido).");
		return false;
	}

	const int32 Code = Response->GetResponseCode();
	const FString Body = Response->GetContentAsString();

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
	{
		OutError = FString::Printf(TEXT("Respuesta no es JSON valido (HTTP %d): %s"), Code, *Body.Left(200));
		return false;
	}

	// El servidor puede devolver {"error": "..."} incluso con 200.
	FString ServerError;
	if (OutRoot->TryGetStringField(TEXT("error"), ServerError) && !ServerError.IsEmpty())
	{
		OutError = ServerError;
		return false;
	}

	if (Code < 200 || Code >= 300)
	{
		OutError = FString::Printf(TEXT("HTTP %d del bridge: %s"), Code, *Body.Left(200));
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
	// Forma real: {"items":[{"id":0,"name":"Espada","priceWei":"...","priceEth":"0.01"}, ...]}
	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!TryParseJsonObject(Response, bSucceeded, Root, Error))
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("FetchCatalog fallo: %s"), *Error);
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
			Item.PriceWei = GetStringField(Obj, { TEXT("priceWei") }); // string enorme, se guarda tal cual
			Item.PriceEth = GetStringField(Obj, { TEXT("priceEth") }); // string decimal "0.01"
			Items.Add(Item);
		}
	}

	UE_LOG(LogBlockchainStore, Log, TEXT("Catalogo recibido: %d items"), Items.Num());
	OnCatalogUpdated.Broadcast(Items);
}

// ---------------------------------------------------------------------------------
// FetchInventory -> GET /api/inventory?address=...
// ---------------------------------------------------------------------------------

void UBlockchainStoreClient::FetchInventory()
{
	if (WalletAddress.IsEmpty())
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("FetchInventory: WalletAddress vacio. Llama a SetWalletAddress primero."));
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
	// Forma real: {"address":"0x..","items":[{"id":0,"name":"Espada","quantity":"14"}, ...]}
	// OJO: "quantity" llega como STRING ("14"); GetIntField lo convierte con Atoi.
	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!TryParseJsonObject(Response, bSucceeded, Root, Error))
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("FetchInventory fallo: %s"), *Error);
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

	UE_LOG(LogBlockchainStore, Log, TEXT("Inventario recibido: %d entradas"), Entries.Num());
	OnInventoryUpdated.Broadcast(Entries);
}

// ---------------------------------------------------------------------------------
// BuyItem -> POST /api/purchase-intent + polling de /api/purchase-status
// ---------------------------------------------------------------------------------

void UBlockchainStoreClient::BuyItem(int32 ItemId, int32 Quantity)
{
	if (WalletAddress.IsEmpty())
	{
		OnPurchaseFailed.Broadcast(TEXT("No hay wallet conectada (WalletAddress vacio)."));
		return;
	}
	if (bPurchaseInProgress)
	{
		OnPurchaseFailed.Broadcast(TEXT("Ya hay una compra en curso; espera a que termine."));
		return;
	}
	if (Quantity < 1)
	{
		Quantity = 1;
	}

	// Construye el body JSON: {address, itemId, quantity}.
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
		UE_LOG(LogBlockchainStore, Warning, TEXT("purchase-intent fallo: %s"), *Error);
		OnPurchaseFailed.Broadcast(Error);
		return;
	}

	CurrentRequestId = GetStringField(Root, { TEXT("requestId") });
	const FString Status = GetStringField(Root, { TEXT("status") });

	if (CurrentRequestId.IsEmpty())
	{
		bPurchaseInProgress = false;
		OnPurchaseFailed.Broadcast(TEXT("El bridge no devolvio requestId."));
		return;
	}

	UE_LOG(LogBlockchainStore, Log, TEXT("purchase-intent OK requestId=%s status=%s"), *CurrentRequestId, *Status);

	// Emite el primer estado y arranca el polling.
	LastReportedStatus = Status.IsEmpty() ? TEXT("pending") : Status;
	OnPurchaseStateChanged.Broadcast(CurrentRequestId, LastReportedStatus);

	UGameInstance* GI = GetGameInstance();
	if (!GI)
	{
		bPurchaseInProgress = false;
		OnPurchaseFailed.Broadcast(TEXT("Sin GameInstance para arrancar el polling."));
		return;
	}

	// Timer repetitivo en el TimerManager del GameInstance (sobrevive cambios de mapa).
	GI->GetTimerManager().SetTimer(
		PollTimerHandle, this, &UBlockchainStoreClient::PollPurchaseStatus,
		PollIntervalSeconds, /*bLoop=*/true, /*FirstDelay=*/PollIntervalSeconds);
}

// ---------------------------------------------------------------------------------
// Polling: cada PollIntervalSeconds dispara un GET /api/purchase-status.
//
// FTimerHandle + FTimerManager::SetTimer(..., bLoop=true) programa una llamada
// recurrente a PollPurchaseStatus. Cada tick lanza UN request HTTP asincrono; su
// callback (HandlePurchaseStatusResponse) decide si seguir o parar. Cuando llega
// done/error o se agota MaxPollAttempts, ClearTimer detiene el ciclo. No hay
// bloqueo del hilo de juego en ningun momento: el timer solo "despierta" para
// lanzar la siguiente peticion.
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
		UE_LOG(LogBlockchainStore, Warning, TEXT("purchase-status timeout tras %d intentos"), MaxPollAttempts);
		OnPurchaseFailed.Broadcast(TEXT("Timeout esperando la confirmacion de la compra."));
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
	// OJO: NO usamos TryParseJsonObject aqui. En este endpoint "error" es un campo
	// de datos normal (null en exito, mensaje cuando status=="error"), siempre con
	// HTTP 200. La forma real es:
	//   {"status":"done","txHash":"0x..","error":null,"itemId":0,"quantity":1}
	// Solo tratamos como "fallo puntual de red" (reintentable) el que no haya
	// respuesta o el body no sea JSON.
	if (!bSucceeded || !Response.IsValid())
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("poll #%d sin respuesta (reintentara)"), PollAttempts);
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const FString Body = Response->GetContentAsString();
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogBlockchainStore, Warning, TEXT("poll #%d JSON invalido (reintentara): %s"), PollAttempts, *Body.Left(200));
		return;
	}

	const FString Status = GetStringField(Root, { TEXT("status") }).ToLower();
	// txHash puede venir null (en pending/signing); GetStringField devuelve "" si es null.
	const FString TxHash = GetStringField(Root, { TEXT("txHash") });

	// Notifica solo cuando el estado cambia, para no spamear a la UI.
	if (!Status.IsEmpty() && Status != LastReportedStatus)
	{
		LastReportedStatus = Status;
		OnPurchaseStateChanged.Broadcast(CurrentRequestId, Status);
	}

	if (Status == TEXT("done"))
	{
		StopPolling();
		bPurchaseInProgress = false;
		UE_LOG(LogBlockchainStore, Log, TEXT("Compra completada. txHash=%s"), *TxHash);
		OnPurchaseCompleted.Broadcast(TxHash);
		// Refresca el inventario tras la compra.
		FetchInventory();
	}
	else if (Status == TEXT("error"))
	{
		StopPolling();
		bPurchaseInProgress = false;
		FString Reason = GetStringField(Root, { TEXT("error") });
		if (Reason.IsEmpty())
		{
			Reason = TEXT("La compra fallo en el bridge.");
		}
		UE_LOG(LogBlockchainStore, Warning, TEXT("Compra fallida: %s"), *Reason);
		OnPurchaseFailed.Broadcast(Reason);
	}
	// pending / signing -> seguimos haciendo polling.
}

void UBlockchainStoreClient::StopPolling()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(PollTimerHandle);
	}
	PollTimerHandle.Invalidate();
}
