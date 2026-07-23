// Vertical slice BlockchainStore - main game screen (LOGIC in C++).
//
// BindWidget PATTERN:
// - This C++ class (UMainScreenWidget) holds ALL the logic: it gets the subsystem,
//   subscribes to its events and paints inventory/store/medals/actions + health/mana.
// - The visual design lives in a Widget Blueprint that INHERITS from this class.
//
// SOURCE OF TRUTH: the inventory and the state (health/mana) come from a per-wallet
// UBlockchainSaveGame, NOT from the chain. The chain provides the catalog, medals, purchase
// records and the initial SEEDING of the inventory (first time with that wallet).

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MainScreenWidget.generated.h"

class UBlockchainStoreClient;
class UBlockchainSaveGame;
class UPanelWidget;
class UProgressBar;
class UTextBlock;
class UButton;
class UEditableTextBox;
class UTexture2D;
class USizeBox;
class UMainScreenWidget;
struct FStoreCatalogItem;
struct FStoreInventoryEntry;
struct FStoreMedal;

// Notice (no parameters) that the player requested to log out. The widget does NOT change
// screen: it only emits this; the Level Blueprint listens for it and returns to the login.
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnLogoutRequested);

/** Where the active selection came from (only one at a time). */
enum class ESelectionSource : uint8
{
	None,
	Inventory,
	Store,
};

/**
 * What a dynamically created button does when pressed. UButton's OnClicked
 * delegates carry no parameters; a per-button proxy remembers its action (+ ItemId if
 * applicable) and forwards it to the screen via HandleButtonAction.
 */
enum class EMainScreenAction : uint8
{
	Buy,             // buy ItemId x Quantity (on-chain)
	BuyFill,         // buy the arrows missing to fill the quiver (live quantity)
	SelectItem,      // select INVENTORY cell ItemId
	SelectStoreItem, // select STORE cell ItemId
	Battle,          // generic action
	Save,         // generic action
	Logout,       // generic action: log out
	UsePotion,    // contextual action on ItemId (potion)
	BreakBottle,  // contextual action on ItemId (empty bottle)
	ShootArrow,   // contextual action (arrow/quiver)
};

/** Visual style of a dynamically created button. */
enum class EButtonKind : uint8
{
	Compact,   // normal button (Buy, Battle, Use...): centered text, content-sized width
	ListRow,   // clickable list row (inventory): left-aligned text, cell-like appearance
};

UCLASS()
class UMainScreenButtonBinding : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	UMainScreenWidget* Screen = nullptr;

	EMainScreenAction Action = EMainScreenAction::Buy;
	int32 ItemId = 0;
	int32 Quantity = 1; // quantity to buy (for "Buy 5/10", etc.)

	UFUNCTION()
	void HandleClicked();
};

/**
 * Main screen: INVENTORY (from savegame, selectable) + STORE + MEDALS
 * + session ACTIONS, plus health/mana bars and status.
 */
UCLASS(Abstract)
class BLOCKCHAINSTORE_API UMainScreenWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// ---- Widgets hooked up from the Blueprint (BindWidget) -----------------------

	/** Inventory container. A grid of icons is created inside it by code. */
	UPROPERTY(meta = (BindWidget))
	UPanelWidget* InventoryContainer;

	/** Store container. A grid of icons is created inside it by code. */
	UPROPERTY(meta = (BindWidget))
	UPanelWidget* StoreContainer;

	/** Medals container (read-only, on-chain). */
	UPROPERTY(meta = (BindWidget))
	UPanelWidget* MedalsContainer;

	/** Actions container (generic + contextual for the selected item). */
	UPROPERTY(meta = (BindWidget))
	UPanelWidget* ActionsContainer;

	/** Mana bar (0..1). */
	UPROPERTY(meta = (BindWidget))
	UProgressBar* ManaBar;

	/** Container for the health hearts. */
	UPROPERTY(meta = (BindWidget))
	UPanelWidget* HeartsContainer;

	/** Status text (feedback for purchases/actions/errors). */
	UPROPERTY(meta = (BindWidget))
	UTextBlock* StatusText;

	// ---- Name popup (first time). OPTIONAL: if it does not exist in the Blueprint, a
	//      default name is used so the seeding can be tested without building the popup.

	/** Root of the name popup (to show/hide). */
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* NamePromptRoot;

	/** Text field where the player types their name. */
	UPROPERTY(meta = (BindWidgetOptional))
	UEditableTextBox* NameInput;

	/** OK button of the name popup. */
	UPROPERTY(meta = (BindWidgetOptional))
	UButton* ConfirmNameButton;

	// ---- Selection / stats state (mirror for the UI; the truth is in SaveData) --

	/** Selected item (-1 = none). Its context is given by SelectionSource. */
	UPROPERTY(BlueprintReadOnly, Category = "MainScreen|Selection")
	int32 SelectedItemId = -1;

	UPROPERTY(BlueprintReadOnly, Category = "MainScreen|Stats")
	int32 MaxHearts = 5;

	UPROPERTY(BlueprintReadOnly, Category = "MainScreen|Stats")
	int32 CurrentHearts = 5;

	UPROPERTY(BlueprintReadOnly, Category = "MainScreen|Stats")
	int32 MaxMana = 100;

	UPROPERTY(BlueprintReadOnly, Category = "MainScreen|Stats")
	int32 Mana = 100;

	// ---- API exposed to Blueprint ------------------------------------------------

	/** Starts the session with the authenticated wallet from the SIWE login: sets it in the
	 *  subsystem and loads (or seeds the first time) its savegame. The Level Blueprint calls it in
	 *  the login -> main screen flow, passing the login Address. */
	UFUNCTION(BlueprintCallable, Category = "MainScreen")
	void SetWalletAndStart(const FString& Address);

	/** Refreshes the ON-CHAIN data: catalog + medals. The inventory is NOT (it comes from the save). */
	UFUNCTION(BlueprintCallable, Category = "MainScreen")
	void RefreshAll();

	/** Selects an INVENTORY item (equivalent to clicking its cell). BP-callable. */
	UFUNCTION(BlueprintCallable, Category = "MainScreen|Selection")
	void SelectItem(int32 ItemId);

	/** Adjusts health (persists to the save if any). Clamp to [0, MaxHearts]. */
	UFUNCTION(BlueprintCallable, Category = "MainScreen|Stats")
	void SetHearts(int32 InCurrent, int32 InMax);

	/** Adjusts mana (persists to the save if any). Clamp to [0, MaxMana]. */
	UFUNCTION(BlueprintCallable, Category = "MainScreen|Stats")
	void SetMana(int32 InMana, int32 InMax);

	/** Shows a message in StatusText. */
	UFUNCTION(BlueprintCallable, Category = "MainScreen")
	void SetStatus(const FString& Message);

	// ---- Session actions (modify the savegame + autosave) ------------------------

	UFUNCTION(BlueprintCallable, Category = "MainScreen|Actions")
	void OnBattle();

	UFUNCTION(BlueprintCallable, Category = "MainScreen|Actions")
	void OnSave();

	UFUNCTION(BlueprintCallable, Category = "MainScreen|Actions")
	void OnUsePotion(int32 ItemId);

	UFUNCTION(BlueprintCallable, Category = "MainScreen|Actions")
	void OnBreakBottle(int32 ItemId);

	UFUNCTION(BlueprintCallable, Category = "MainScreen|Actions")
	void OnShootArrow();

	/**
	 * Logs out: autosave, clears the in-memory state, clears the client's auth
	 * and emits OnLogoutRequested. Does NOT change screen (the Level Blueprint does that).
	 */
	UFUNCTION(BlueprintCallable, Category = "MainScreen|Actions")
	void OnLogout();

	/** Emitted on logout. The Level Blueprint listens for it to return to the login. */
	UPROPERTY(BlueprintAssignable, Category = "MainScreen|Events")
	FOnLogoutRequested OnLogoutRequested;

	/** Single entry point for the dynamic buttons (called by the proxy). */
	void HandleButtonAction(EMainScreenAction Action, int32 ItemId, int32 Quantity = 1);

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

private:
	// ---- Subsystem event handlers (UFUNCTION => AddDynamic) ----------------------

	UFUNCTION()
	void HandleInventoryUpdated(const TArray<FStoreInventoryEntry>& Items);

	UFUNCTION()
	void HandleCatalogUpdated(const TArray<FStoreCatalogItem>& Items);

	UFUNCTION()
	void HandleProgressUpdated(const TArray<FStoreMedal>& Medals);

	UFUNCTION()
	void HandlePurchaseStateChanged(const FString& RequestId, const FString& Status);

	UFUNCTION()
	void HandlePurchaseCompleted(const FString& TxHash);

	UFUNCTION()
	void HandlePurchaseFailed(const FString& Reason);

	/** OK of the name popup. */
	UFUNCTION()
	void HandleConfirmName();

	// ---- Session / savegame ------------------------------------------------------

	/** Resolves (lazily) and caches the subsystem (may be requested before NativeConstruct). */
	UBlockchainStoreClient* GetStoreClient();

	/** Subscribes (idempotently) the handlers to the subsystem's events. */
	void EnsureSubscribed();

	/** Starts the session for a wallet: loads the save or starts the first-time flow. */
	void BeginSessionForWallet(const FString& Address);

	/** Tries to close the first-time flow (needs both name AND seed ready). */
	void TryFinalizeFirstTime();

	void ShowNamePrompt();
	void HideNamePrompt();

	// ---- Unified selection -------------------------------------------------------
	void SelectFromInventory(int32 ItemId);
	void SelectFromStore(int32 ItemId);

	// ---- Purchase (local validation against the savegame + launch on-chain) ------
	/** Validates the local rules (bottle for potions, capacity for arrows) and, if
	 *  they pass, launches the on-chain purchase of Quantity units. Rules moved from the contract. */
	void RequestBuy(int32 ItemId, int32 Quantity);

	/** Arrow capacity of the HIGHEST-capacity quiver owned (0 if none). */
	int32 GetBestQuiverCapacity() const;

	/** Recolors the cells (inventory + store) according to (SelectedItemId, SelectionSource). */
	void UpdateSelectionHighlight();

	// ---- Painting / synchronization ----------------------------------------------
	void RepaintInventoryFromSave();
	void RepaintActions();
	void RepaintHearts();
	void ApplyManaBar();
	void PushStatsToUI(); // copies SaveData -> UI fields + repaints bars/hearts

	// ---- Savegame inventory ------------------------------------------------------
	int32 GetItemQty(int32 ItemId) const;
	void AddItemQty(int32 ItemId, int32 Delta); // clamp >=0; removes the entry if it reaches 0

	// ---- Saving ------------------------------------------------------------------
	void Autosave();

	/** Creates a button (styled per Kind) with a label + proxy (Action, ItemId, Quantity). */
	UButton* MakeActionButton(const FString& Label, EMainScreenAction Action, int32 ItemId, TArray<UMainScreenButtonBinding*>& OutBindings, EButtonKind Kind = EButtonKind::Compact, int32 Quantity = 1);

	/** Creates a clickable grid CELL (icon + corner text), with its proxy.
	 *  Returns the square SizeBox ready to place in the grid; stores the inner
	 *  button in OutButtons/OutIds for the highlight. */
	USizeBox* MakeItemCell(int32 ItemId, const FString& CornerText, EMainScreenAction ClickAction,
		TArray<UMainScreenButtonBinding*>& OutBindings, TArray<UButton*>& OutButtons, TArray<int32>& OutIds, float CellSize);

	/** Item icon texture (cached). May return null if it does not load. */
	UTexture2D* GetIcon(int32 ItemId);

	/** Icon tint (red for health potion, blue for mana potion, white for the rest). */
	static FLinearColor GetIconTint(int32 ItemId);

	/** Readable name of an item by its id (fixed catalog from the contract). */
	static FString ItemName(int32 ItemId);

	// ---- State -------------------------------------------------------------------

	UPROPERTY()
	UBlockchainStoreClient* StoreClient = nullptr;

	/** Current game (source of truth for inventory + stats). */
	UPROPERTY()
	UBlockchainSaveGame* SaveData = nullptr;

	/** Save slot of the current wallet ("save_<address>"). */
	FString CurrentSlotName;

	/** itemId and quantity of the last launched purchase; used on confirmation
	 *  (OnPurchaseCompleted does not carry them). */
	int32 LastPurchasedItemId = -1;
	int32 LastPurchasedQty = 1;

	/** Context of the active selection (only one at a time). */
	ESelectionSource SelectionSource = ESelectionSource::None;

	/** Icon cache by itemId (referenced so they are not unloaded). */
	UPROPERTY()
	TMap<int32, UTexture2D*> IconCache;

	// First-time flow (name + seed can arrive in any order).
	bool bFirstTimeInProgress = false;
	bool bNameReady = false;
	bool bSeedReady = false;
	FString PendingName;
	TMap<int32, int32> PendingSeed;

	// Live proxies per grid: reset together with the ClearChildren of THEIR container.
	UPROPERTY()
	TArray<UMainScreenButtonBinding*> InventoryBindings;

	UPROPERTY()
	TArray<UMainScreenButtonBinding*> StoreBindings;

	UPROPERTY()
	TArray<UMainScreenButtonBinding*> ActionBindings;

	// Live cells (button + itemId) per grid, to recolor the selection highlight
	// without rebuilding the grid (avoids remaking the cell under its own click).
	UPROPERTY()
	TArray<UButton*> InventoryCellButtons;
	TArray<int32> InventoryCellItemIds;

	UPROPERTY()
	TArray<UButton*> StoreCellButtons;
	TArray<int32> StoreCellItemIds;
};
