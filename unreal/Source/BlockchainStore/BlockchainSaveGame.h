// Vertical slice BlockchainStore - per-wallet SaveGame.
//
// CONCEPTUAL CHANGE: the source of truth for the inventory and the game state
// (health/mana) is THIS SaveGame, not the chain. The chain is used for the catalog,
// recording purchases, achievements and SEEDING the inventory the first time.
//
// One SaveGame per wallet: the slot is named "save_<address in lowercase>", so each
// address has its own independent game.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "BlockchainSaveGame.generated.h"

UCLASS()
class BLOCKCHAINSTORE_API UBlockchainSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/** Name chosen by the player the first time. */
	UPROPERTY()
	FString UserName;

	/** Player inventory: itemId -> quantity. Day-to-day source of truth. */
	UPROPERTY()
	TMap<int32, int32> Inventory;

	/** Session state. Restored as-is when re-entering with the wallet. */
	UPROPERTY()
	int32 CurrentHearts = 5;

	UPROPERTY()
	int32 MaxHearts = 5;

	UPROPERTY()
	int32 CurrentMana = 100;

	UPROPERTY()
	int32 MaxMana = 100;
};
