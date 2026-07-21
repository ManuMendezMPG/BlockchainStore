// Vertical slice BlockchainStore - data structures exposed to Blueprint.
//
// Design notes:
// - priceWei is stored as an FString on purpose: 1 ETH = 10^18 wei, and prices
//   can easily exceed the int32/int64 range. For "display in UI" we do not
//   need arithmetic, only the text. If later we need to operate on wei,
//   it will be done with a dedicated BigInt/library, never with a float.
// - All UPROPERTY are BlueprintReadOnly: the UI shows them, it does not edit them.

#pragma once

#include "CoreMinimal.h"
#include "BlockchainStoreTypes.generated.h"

/** A catalog item, exactly as it arrives from GET /api/catalog. */
USTRUCT(BlueprintType)
struct FStoreCatalogItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	int32 ItemId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	FString Name;

	/** Price in wei as text (can be huge). */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	FString PriceWei;

	/** Price in ETH already formatted by the bridge, ready to display. */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	FString PriceEth;
};

/** An inventory entry: how many units of the item the current address owns. */
USTRUCT(BlueprintType)
struct FStoreInventoryEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	int32 ItemId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	FString Name;

	/** Balance (number of units) the player has of this item. */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	int32 Balance = 0;
};

/**
 * A medal from the Achievements contract, exactly as GET /api/progress resolves it.
 * Read-only: medals are earned on-chain, the UI only displays them.
 */
USTRUCT(BlueprintType)
struct FStoreMedal
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	int32 MedalId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	FString Name;

	/** True if the queried address owns this medal. */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	bool bOwned = false;

	/** Rarity (only applies to the Merchant, id 1). Empty if it does not apply or is locked. */
	UPROPERTY(BlueprintReadOnly, Category = "BlockchainStore")
	FString Rarity;
};
