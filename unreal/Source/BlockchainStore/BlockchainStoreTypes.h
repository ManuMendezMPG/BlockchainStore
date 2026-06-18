// Vertical slice BlockchainStore - estructuras de datos expuestas a Blueprint.
//
// Notas de diseno:
// - priceWei se guarda como FString a proposito: 1 ETH = 10^18 wei, y los precios
//   pueden superar el rango de int32/int64 sin problema. Para "mostrar en UI" no
//   necesitamos aritmetica, solo el texto. Si mas adelante hace falta operar con
//   wei, se hara con una BigInt/lib dedicada, nunca con float.
// - Todos los UPROPERTY son BlueprintReadOnly: la UI los muestra, no los edita.

#pragma once

#include "CoreMinimal.h"
#include "BlockchainStoreTypes.generated.h"

/** Un item del catalogo, tal y como llega de GET /api/catalog. */
USTRUCT(BlueprintType)
struct FStoreCatalogItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	int32 ItemId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	FString Name;

	/** Precio en wei como texto (puede ser enorme). */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	FString PriceWei;

	/** Precio en ETH ya formateado por el bridge, listo para mostrar. */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	FString PriceEth;
};

/** Una entrada de inventario: cuantas unidades del item posee el address actual. */
USTRUCT(BlueprintType)
struct FStoreInventoryEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	int32 ItemId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	FString Name;

	/** Balance (numero de unidades) que el jugador tiene de este item. */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	int32 Balance = 0;
};
