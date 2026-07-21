// Vertical slice BlockchainStore - main screen implementation.

#include "MainScreenWidget.h"

#include "BlockchainStoreClient.h"
#include "BlockchainStoreTypes.h"
#include "BlockchainSaveGame.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/Border.h"
#include "Components/SizeBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SlateWrapperTypes.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Kismet/GameplayStatics.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogMainScreen, Log, All);

namespace
{
	// Stable contract IDs (previously detected by name, which is fragile).
	// 0 Sword, 1 Shield, 2 Bow, 3 Quiver5, 4 Quiver10, 5 Quiver20, 6 Arrow,
	// 7 Empty bottle, 8 Life potion, 9 Mana potion.
	constexpr int32 ITEM_ARCO         = 2;
	constexpr int32 ITEM_CARCAJ5      = 3;
	constexpr int32 ITEM_CARCAJ10     = 4;
	constexpr int32 ITEM_CARCAJ20     = 5;
	constexpr int32 ITEM_FLECHA       = 6;
	constexpr int32 ITEM_BOTELLA      = 7;
	constexpr int32 ITEM_POCION_VIDA  = 8;
	constexpr int32 ITEM_POCION_MANA  = 9;

	bool IsPotion(int32 Id) { return Id == ITEM_POCION_VIDA || Id == ITEM_POCION_MANA; }
	bool IsQuiver(int32 Id) { return Id == ITEM_CARCAJ5 || Id == ITEM_CARCAJ10 || Id == ITEM_CARCAJ20; }

	// Arrow capacity of each quiver. In the inventory only ONE quiver slot is shown,
	// with the capacity of the LARGEST one you have (id 5 > id 4 > id 3).
	int32 QuiverCapacity(int32 Id)
	{
		switch (Id)
		{
		case ITEM_CARCAJ5:  return 5;
		case ITEM_CARCAJ10: return 10;
		case ITEM_CARCAJ20: return 20;
		default:            return 0;
		}
	}
}

// =================================================================================
// STYLE. Everything parameterized here for easy tuning: colors, paddings, sizes.
// Only affects the LOOK of the widgets created by code; the logic does not change.
// =================================================================================
namespace Style
{
	// --- Colors (RGBA 0..1) ---
	static const FLinearColor TextColor        (0.92f, 0.90f, 0.84f, 1.0f); // bone, readable over dark
	static const FLinearColor TextLockedColor  (0.55f, 0.55f, 0.58f, 1.0f); // locked medal (dimmed)
	static const FLinearColor TextOwnedColor   (0.55f, 0.85f, 0.55f, 1.0f); // unlocked medal (greenish)
	static const FLinearColor RowBgColor       (0.05f, 0.06f, 0.09f, 0.55f); // cell: dark semitransparent
	static const FLinearColor RowSelectedColor (0.18f, 0.42f, 0.82f, 0.75f); // selected row: blue
	static const FLinearColor ButtonColor      (0.16f, 0.20f, 0.28f, 1.0f);  // normal button

	// --- Font sizes ---
	static const float RowFontSize    = 14.0f; // inventory / medal rows
	static const float StoreFontSize  = 14.0f; // store rows
	static const float ButtonFontSize = 13.0f; // button text

	// --- Widths (px) so the lists do NOT take up the full width ---
	static const float ListRowWidth  = 260.0f; // inventory / medals
	static const float StoreRowWidth = 320.0f; // store (text + Buy button)

	// --- Paddings (left, top, right, bottom) ---
	static const FMargin RowContentPadding   (8.0f, 5.0f, 8.0f, 5.0f); // cell interior
	static const FMargin RowSlotPadding      (0.0f, 3.0f, 0.0f, 3.0f); // spacing between rows
	static const FMargin ButtonContentPadding(12.0f, 4.0f, 12.0f, 4.0f); // button interior
	static const FMargin ButtonSlotPadding   (4.0f, 3.0f, 4.0f, 3.0f);   // spacing between buttons
	static const FMargin StoreLabelPadding   (0.0f, 0.0f, 8.0f, 0.0f);   // gap between text and button

	// --- Icon grids (inventory / store) ---
	static const float   CellSize          = 280.0f;                 // square cell (px)
	static const int32   InventoryColumns  = 4;                     // inventory columns
	static const float   CellSpacing       = 4.0f;                  // spacing between cells
	static const FMargin CellIconPadding   (2.0f, 2.0f, 2.0f, 2.0f);// icon margin inside the cell
	static const FMargin CellCornerPadding (0.0f, 0.0f, 5.0f, 3.0f);// position of the corner text
	static const float   CellCornerFontSize= 13.0f;                 // "x17" / price
	static const FLinearColor CellBgColor       = RowBgColor;       // cell background
	static const FLinearColor CellSelectedColor = RowSelectedColor; // selected cell

	// --- Icon tints ---
	static const FLinearColor IconTintDefault  (1.00f, 1.00f, 1.00f, 1.0f); // no tint
	static const FLinearColor PotionLifeTint   (1.00f, 0.35f, 0.30f, 1.0f); // red (life potion, id 8)
	static const FLinearColor PotionManaTint   (0.40f, 0.60f, 1.00f, 1.0f); // blue (mana potion, id 9)
}

namespace
{
	// Icon paths by itemId. If something fails to load, the cell falls back to text (name).
	FString IconPathForItem(int32 Id)
	{
		switch (Id)
		{
		case 0: return TEXT("/Game/Icons/Sword");
		case 1: return TEXT("/Game/Icons/Shield");
		case 2: return TEXT("/Game/Icons/Bow");
		case 3:
		case 4:
		case 5: return TEXT("/Game/Icons/Quiver");   // the 3 quivers share the same icon
		case 6: return TEXT("/Game/Icons/Arrow");
		case 7: return TEXT("/Game/Icons/EmptyBottle");
		case 8: return TEXT("/Game/Icons/LifePotion");
		case 9: return TEXT("/Game/Icons/ManaPotion");
		default: return FString();
		}
	}
}

namespace
{
	// Applies left alignment + padding to the slot, whatever the type of container
	// panel is (the user chooses VerticalBox/ScrollBox/etc. in the Blueprint).
	void StyleSlotLeft(UPanelSlot* Slot, const FMargin& Padding)
	{
		if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
		{
			VS->SetHorizontalAlignment(HAlign_Left);
			VS->SetPadding(Padding);
		}
		else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
		{
			HS->SetVerticalAlignment(VAlign_Top);
			HS->SetPadding(Padding);
		}
		else if (UScrollBoxSlot* SS = Cast<UScrollBoxSlot>(Slot))
		{
			SS->SetHorizontalAlignment(HAlign_Left);
			SS->SetPadding(Padding);
		}
		else if (UWrapBoxSlot* WS = Cast<UWrapBoxSlot>(Slot))
		{
			WS->SetHorizontalAlignment(HAlign_Left);
			WS->SetPadding(Padding);
		}
	}

	// Styled TextBlock: size, color and justification.
	UTextBlock* MakeStyledText(UWidgetTree* Tree, const FString& InText, float FontSize, const FLinearColor& Color, ETextJustify::Type Justify)
	{
		UTextBlock* T = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		T->SetText(FText::FromString(InText));
		FSlateFontInfo Font = T->GetFont();
		Font.Size = FMath::RoundToInt(FontSize); // int fits whether Size is int32 or float
		T->SetFont(Font);
		T->SetColorAndOpacity(FSlateColor(Color));
		T->SetJustification(Justify);
		return T;
	}

	// Wraps a widget in a SizeBox with fixed width (so the row does not stretch).
	USizeBox* WrapWidth(UWidgetTree* Tree, UWidget* Inner, float Width)
	{
		USizeBox* Box = Tree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(Width);
		Box->AddChild(Inner);
		return Box;
	}

	// Cell: Border with semitransparent background + inner padding, wrapping the content.
	UBorder* MakeCell(UWidgetTree* Tree, UWidget* Content, const FLinearColor& Bg)
	{
		UBorder* B = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		B->SetBrushColor(Bg);
		B->SetPadding(Style::RowContentPadding);
		B->SetHorizontalAlignment(HAlign_Fill);
		B->SetVerticalAlignment(VAlign_Center);
		B->AddChild(Content);
		return B;
	}
}

// ---------------------------------------------------------------------------------
// UMainScreenButtonBinding: forwards the dynamic button's click to the screen.
// ---------------------------------------------------------------------------------

void UMainScreenButtonBinding::HandleClicked()
{
	if (Screen)
	{
		Screen->HandleButtonAction(Action, ItemId, Quantity);
	}
}

// ---------------------------------------------------------------------------------
// Item names (fixed catalog)
// ---------------------------------------------------------------------------------

FString UMainScreenWidget::ItemName(int32 ItemId)
{
	switch (ItemId)
	{
	case 0: return TEXT("Sword");
	case 1: return TEXT("Shield");
	case ITEM_ARCO:        return TEXT("Bow");
	case ITEM_CARCAJ5:     return TEXT("Quiver (5)");
	case ITEM_CARCAJ10:    return TEXT("Quiver (10)");
	case ITEM_CARCAJ20:    return TEXT("Quiver (20)");
	case ITEM_FLECHA:      return TEXT("Arrow");
	case ITEM_BOTELLA:     return TEXT("Empty bottle");
	case ITEM_POCION_VIDA: return TEXT("Life potion");
	case ITEM_POCION_MANA: return TEXT("Mana potion");
	default: return FString::Printf(TEXT("Item %d"), ItemId);
	}
}

FLinearColor UMainScreenWidget::GetIconTint(int32 ItemId)
{
	if (ItemId == ITEM_POCION_VIDA) return Style::PotionLifeTint;
	if (ItemId == ITEM_POCION_MANA) return Style::PotionManaTint;
	return Style::IconTintDefault;
}

UTexture2D* UMainScreenWidget::GetIcon(int32 ItemId)
{
	// Cache: even null entries are stored so we don't retry on every repaint.
	if (UTexture2D** Cached = IconCache.Find(ItemId))
	{
		return *Cached;
	}

	UTexture2D* Tex = nullptr;
	const FString Path = IconPathForItem(ItemId);
	if (!Path.IsEmpty())
	{
		// LoadObject needs the FULL object path: "/Game/Icons/Sword.Sword".
		// We derive it from the package (asset name = last segment after the '/').
		FString ObjectPath = Path;
		int32 SlashIndex;
		if (Path.FindLastChar(TEXT('/'), SlashIndex))
		{
			const FString AssetName = Path.RightChop(SlashIndex + 1);
			ObjectPath = Path + TEXT(".") + AssetName;
		}

		// Robust runtime load; if the asset does not exist, LoadObject returns null (no crash).
		Tex = LoadObject<UTexture2D>(nullptr, *ObjectPath);
		if (!Tex)
		{
			UE_LOG(LogMainScreen, Warning, TEXT("Could not load icon '%s' (item %d); text will be used."), *ObjectPath, ItemId);
		}
	}

	IconCache.Add(ItemId, Tex);
	return Tex;
}

// ---------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------

UBlockchainStoreClient* UMainScreenWidget::GetStoreClient()
{
	if (!StoreClient)
	{
		if (UGameInstance* GI = GetGameInstance())
		{
			StoreClient = GI->GetSubsystem<UBlockchainStoreClient>();
		}
	}
	return StoreClient;
}

void UMainScreenWidget::EnsureSubscribed()
{
	if (UBlockchainStoreClient* SC = GetStoreClient())
	{
		// AddUniqueDynamic is idempotent: it's fine to call it multiple times.
		SC->OnInventoryUpdated.AddUniqueDynamic(this, &UMainScreenWidget::HandleInventoryUpdated);
		SC->OnCatalogUpdated.AddUniqueDynamic(this, &UMainScreenWidget::HandleCatalogUpdated);
		SC->OnProgressUpdated.AddUniqueDynamic(this, &UMainScreenWidget::HandleProgressUpdated);
		SC->OnPurchaseStateChanged.AddUniqueDynamic(this, &UMainScreenWidget::HandlePurchaseStateChanged);
		SC->OnPurchaseCompleted.AddUniqueDynamic(this, &UMainScreenWidget::HandlePurchaseCompleted);
		SC->OnPurchaseFailed.AddUniqueDynamic(this, &UMainScreenWidget::HandlePurchaseFailed);
	}
}

void UMainScreenWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (!GetStoreClient())
	{
		UE_LOG(LogMainScreen, Warning, TEXT("BlockchainStoreClient not found; the screen will not be able to request data."));
		SetStatus(TEXT("No connection to the subsystem."));
	}
	EnsureSubscribed();

	// OK button of the name popup (if the Blueprint has it).
	if (ConfirmNameButton)
	{
		ConfirmNameButton->OnClicked.AddUniqueDynamic(this, &UMainScreenWidget::HandleConfirmName);
	}
	// The popup is only shown if SetWalletAndStart (called before NativeConstruct)
	// detected a first time; otherwise, hidden.
	if (bFirstTimeInProgress)
	{
		ShowNamePrompt();
	}
	else
	{
		HideNamePrompt();
	}

	RepaintHearts();
	ApplyManaBar();
	RepaintActions();

	// Chain data (catalog + medals). The inventory NO: it comes from the savegame.
	RefreshAll();
}

void UMainScreenWidget::NativeDestruct()
{
	if (StoreClient)
	{
		StoreClient->OnInventoryUpdated.RemoveDynamic(this, &UMainScreenWidget::HandleInventoryUpdated);
		StoreClient->OnCatalogUpdated.RemoveDynamic(this, &UMainScreenWidget::HandleCatalogUpdated);
		StoreClient->OnProgressUpdated.RemoveDynamic(this, &UMainScreenWidget::HandleProgressUpdated);
		StoreClient->OnPurchaseStateChanged.RemoveDynamic(this, &UMainScreenWidget::HandlePurchaseStateChanged);
		StoreClient->OnPurchaseCompleted.RemoveDynamic(this, &UMainScreenWidget::HandlePurchaseCompleted);
		StoreClient->OnPurchaseFailed.RemoveDynamic(this, &UMainScreenWidget::HandlePurchaseFailed);
	}

	Super::NativeDestruct();
}

// ---------------------------------------------------------------------------------
// Session entry (wallet) + savegame load/seed
// ---------------------------------------------------------------------------------

void UMainScreenWidget::SetWalletAndStart(const FString& Address)
{
	UBlockchainStoreClient* SC = GetStoreClient();
	if (!SC)
	{
		UE_LOG(LogMainScreen, Warning, TEXT("SetWalletAndStart: no subsystem (GameInstance not available yet)."));
		return;
	}

	SC->SetWalletAddress(Address);
	EnsureSubscribed(); // needed to catch the seed (OnInventoryUpdated) if it's the first time.
	BeginSessionForWallet(Address);
}

void UMainScreenWidget::BeginSessionForWallet(const FString& Address)
{
	CurrentSlotName = TEXT("save_") + Address.ToLower();

	if (UGameplayStatics::DoesSaveGameExist(CurrentSlotName, 0))
	{
		// Known wallet: load. The save IS the inventory and the state.
		SaveData = Cast<UBlockchainSaveGame>(UGameplayStatics::LoadGameFromSlot(CurrentSlotName, 0));
		if (SaveData)
		{
			HideNamePrompt();
			PushStatsToUI();
			RepaintInventoryFromSave();
			SetStatus(FString::Printf(TEXT("Welcome back, %s"), *SaveData->UserName));
			UE_LOG(LogMainScreen, Log, TEXT("Savegame loaded from slot '%s' (%d items)"), *CurrentSlotName, SaveData->Inventory.Num());
			return;
		}
		UE_LOG(LogMainScreen, Warning, TEXT("Savegame of '%s' corrupt; treated as first time."), *CurrentSlotName);
	}

	// First time: ask for name + seed inventory from the chain.
	bFirstTimeInProgress = true;
	bNameReady = false;
	bSeedReady = false;
	PendingName.Empty();
	PendingSeed.Reset();

	SetStatus(TEXT("First time: seeding inventory from the chain..."));
	ShowNamePrompt();

	if (UBlockchainStoreClient* SC = GetStoreClient())
	{
		SC->FetchInventory(); // the response arrives at HandleInventoryUpdated (seed mode).
	}
}

void UMainScreenWidget::TryFinalizeFirstTime()
{
	if (!bFirstTimeInProgress || !bNameReady || !bSeedReady)
	{
		return; // missing pieces (name and/or seed)
	}

	SaveData = NewObject<UBlockchainSaveGame>(this);
	SaveData->UserName = PendingName;
	SaveData->Inventory = PendingSeed;
	SaveData->MaxHearts = 5;
	SaveData->CurrentHearts = 5;
	SaveData->MaxMana = 100;
	SaveData->CurrentMana = 100;

	UGameplayStatics::SaveGameToSlot(SaveData, CurrentSlotName, 0);

	bFirstTimeInProgress = false;
	HideNamePrompt();
	PushStatsToUI();
	RepaintInventoryFromSave();
	SetStatus(FString::Printf(TEXT("Welcome, %s! Inventory seeded (%d items)."), *PendingName, SaveData->Inventory.Num()));
	UE_LOG(LogMainScreen, Log, TEXT("Savegame created in slot '%s'"), *CurrentSlotName);
}

void UMainScreenWidget::HandleConfirmName()
{
	PendingName = NameInput ? NameInput->GetText().ToString().TrimStartAndEnd() : FString();
	if (PendingName.IsEmpty())
	{
		PendingName = TEXT("Player");
	}
	bNameReady = true;
	TryFinalizeFirstTime();
}

void UMainScreenWidget::ShowNamePrompt()
{
	if (NamePromptRoot)
	{
		NamePromptRoot->SetVisibility(ESlateVisibility::Visible);
	}
	else if (ConfirmNameButton == nullptr)
	{
		// The Blueprint has no popup: use a default name so the test is not blocked.
		UE_LOG(LogMainScreen, Warning, TEXT("No name popup in the Blueprint; using default name."));
		PendingName = TEXT("Player");
		bNameReady = true;
		TryFinalizeFirstTime();
	}
}

void UMainScreenWidget::HideNamePrompt()
{
	if (NamePromptRoot)
	{
		NamePromptRoot->SetVisibility(ESlateVisibility::Collapsed);
	}
}

// ---------------------------------------------------------------------------------
// Refresh of chain data (catalog + medals). Inventory NO.
// ---------------------------------------------------------------------------------

void UMainScreenWidget::RefreshAll()
{
	if (UBlockchainStoreClient* SC = GetStoreClient())
	{
		SC->FetchCatalog();
		SC->FetchProgress();
	}
}

// ---------------------------------------------------------------------------------
// Helper: button with label + proxy (Action, ItemId)
// ---------------------------------------------------------------------------------

UButton* UMainScreenWidget::MakeActionButton(const FString& Label, EMainScreenAction Action, int32 ItemId, TArray<UMainScreenButtonBinding*>& OutBindings, EButtonKind Kind, int32 Quantity)
{
	const bool bListRow = (Kind == EButtonKind::ListRow);

	UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	// List row -> cell background; normal button -> button color.
	Button->SetBackgroundColor(bListRow ? Style::RowBgColor : Style::ButtonColor);

	// Text: left-aligned in list rows, centered in normal buttons.
	UTextBlock* Text = MakeStyledText(WidgetTree, Label,
		bListRow ? Style::RowFontSize : Style::ButtonFontSize,
		Style::TextColor,
		bListRow ? ETextJustify::Left : ETextJustify::Center);

	// The button's content slot controls the inner padding and alignment.
	UPanelSlot* ContentSlot = Button->AddChild(Text);
	if (UButtonSlot* BS = Cast<UButtonSlot>(ContentSlot))
	{
		BS->SetPadding(bListRow ? Style::RowContentPadding : Style::ButtonContentPadding);
		BS->SetHorizontalAlignment(bListRow ? HAlign_Left : HAlign_Center);
		BS->SetVerticalAlignment(VAlign_Center);
	}

	UMainScreenButtonBinding* Binding = NewObject<UMainScreenButtonBinding>(this);
	Binding->Screen = this;
	Binding->Action = Action;
	Binding->ItemId = ItemId;
	Binding->Quantity = Quantity;
	Button->OnClicked.AddDynamic(Binding, &UMainScreenButtonBinding::HandleClicked);
	OutBindings.Add(Binding);

	return Button;
}

// ---------------------------------------------------------------------------------
// MakeItemCell: clickable square cell with icon + corner text (quantity/price).
// Structure: SizeBox(CellSize) -> Button(cell bg) -> Overlay[ Image icon | Text corner ]
// If the icon fails to load, it falls back to text with the item name (no crash).
// ---------------------------------------------------------------------------------

USizeBox* UMainScreenWidget::MakeItemCell(int32 ItemId, const FString& CornerText, EMainScreenAction ClickAction,
	TArray<UMainScreenButtonBinding*>& OutBindings, TArray<UButton*>& OutButtons, TArray<int32>& OutIds)
{
	UButton* Cell = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	Cell->SetBackgroundColor(Style::CellBgColor);

	UOverlay* Ov = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	// The content must not capture the mouse: this way the click ALWAYS reaches the cell button.
	Ov->SetVisibility(ESlateVisibility::HitTestInvisible);

	// Icon (or fallback text if it fails to load).
	if (UTexture2D* Tex = GetIcon(ItemId))
	{
		UImage* Icon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		Icon->SetBrushFromTexture(Tex);
		Icon->SetColorAndOpacity(GetIconTint(ItemId));
		if (UOverlaySlot* IS = Ov->AddChildToOverlay(Icon))
		{
			IS->SetHorizontalAlignment(HAlign_Fill);
			IS->SetVerticalAlignment(VAlign_Fill);
			IS->SetPadding(Style::CellIconPadding);
		}
	}
	else
	{
		UTextBlock* Fallback = MakeStyledText(WidgetTree, ItemName(ItemId), Style::CellCornerFontSize, Style::TextColor, ETextJustify::Center);
		Fallback->SetAutoWrapText(true);
		if (UOverlaySlot* FS = Ov->AddChildToOverlay(Fallback))
		{
			FS->SetHorizontalAlignment(HAlign_Fill);
			FS->SetVerticalAlignment(VAlign_Center);
			FS->SetPadding(Style::CellIconPadding);
		}
	}

	// Corner text: quantity ("x17") or price ("0.01"), bottom-right.
	if (!CornerText.IsEmpty())
	{
		UTextBlock* Corner = MakeStyledText(WidgetTree, CornerText, Style::CellCornerFontSize, Style::TextColor, ETextJustify::Right);
		if (UOverlaySlot* CS = Ov->AddChildToOverlay(Corner))
		{
			CS->SetHorizontalAlignment(HAlign_Right);
			CS->SetVerticalAlignment(VAlign_Bottom);
			CS->SetPadding(Style::CellCornerPadding);
		}
	}

	// The overlay fills the button.
	if (UButtonSlot* BS = Cast<UButtonSlot>(Cell->AddChild(Ov)))
	{
		BS->SetPadding(FMargin(0.f));
		BS->SetHorizontalAlignment(HAlign_Fill);
		BS->SetVerticalAlignment(VAlign_Fill);
	}

	// Click proxy (same pattern).
	UMainScreenButtonBinding* Binding = NewObject<UMainScreenButtonBinding>(this);
	Binding->Screen = this;
	Binding->Action = ClickAction;
	Binding->ItemId = ItemId;
	Cell->OnClicked.AddDynamic(Binding, &UMainScreenButtonBinding::HandleClicked);
	OutBindings.Add(Binding);

	OutButtons.Add(Cell);
	OutIds.Add(ItemId);

	// Fixed-size square cell.
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetWidthOverride(Style::CellSize);
	Box->SetHeightOverride(Style::CellSize);
	Box->AddChild(Cell);
	return Box;
}

// ---------------------------------------------------------------------------------
// Inventory: GRID of icons, painted FROM THE SAVEGAME (not from the chain).
// ---------------------------------------------------------------------------------

void UMainScreenWidget::RepaintInventoryFromSave()
{
	if (!InventoryContainer)
	{
		return;
	}

	InventoryContainer->ClearChildren();
	InventoryBindings.Reset();
	InventoryCellButtons.Reset();
	InventoryCellItemIds.Reset();

	if (!SaveData)
	{
		return;
	}

	// Grid of InventoryColumns columns inside the container.
	UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass());
	Grid->SetSlotPadding(FMargin(Style::CellSpacing));
	InventoryContainer->AddChild(Grid);

	// --- Unified QUIVER + ARROWS model ---
	// On-chain there are separate items (quiver 3/4/5, arrow 6), but in the game
	// inventory they are shown as ONE single slot: the quiver of the LARGEST capacity
	// you have, with "arrows/capacity". The smaller quivers and the arrow do NOT have their own slot.
	int32 BestQuiverId = -1;
	int32 BestCapacity = 0;
	for (int32 QId : { ITEM_CARCAJ5, ITEM_CARCAJ10, ITEM_CARCAJ20 })
	{
		if (GetItemQty(QId) > 0)
		{
			const int32 Cap = QuiverCapacity(QId);
			if (Cap > BestCapacity)
			{
				BestCapacity = Cap;
				BestQuiverId = QId;
			}
		}
	}
	const int32 Arrows = GetItemQty(ITEM_FLECHA);

	// --- STEP 1: build the ORDERED list of cells to show (id + corner text),
	// after filtering out the arrow and collapsing the quivers into a single slot. This way the cells
	// actually painted are already "compacted": no positions reserved by id.
	TArray<int32>   Ids;
	SaveData->Inventory.GetKeys(Ids);
	Ids.Sort();

	TArray<int32>   CellIds;      // id used to create/select the cell
	TArray<FString> CellCorners;  // corner text of that cell
	bool bQuiverAdded = false;
	for (int32 Id : Ids)
	{
		if (SaveData->Inventory[Id] <= 0)
		{
			continue;
		}
		if (Id == ITEM_FLECHA)
		{
			continue; // the arrow is never its own slot (it goes inside the quiver)
		}
		if (IsQuiver(Id))
		{
			// All quivers collapse into ONE single entry (the one with the largest capacity).
			if (!bQuiverAdded && BestQuiverId != -1)
			{
				const int32 Shown = FMath::Min(Arrows, BestCapacity);
				CellIds.Add(BestQuiverId); // when selected, RepaintActions (IsQuiver) shows "Shoot"
				CellCorners.Add(FString::Printf(TEXT("%d/%d"), Shown, BestCapacity));
				bQuiverAdded = true;
			}
			continue; // no individual quiver takes a slot
		}
		CellIds.Add(Id);
		CellCorners.Add(FString::Printf(TEXT("x%d"), SaveData->Inventory[Id]));
	}

	// --- STEP 2: place the cells CONSECUTIVELY, left->right and top->bottom,
	// with no gaps. The position depends only on the cell index, not on the itemId.
	for (int32 CellIndex = 0; CellIndex < CellIds.Num(); ++CellIndex)
	{
		USizeBox* Cell = MakeItemCell(CellIds[CellIndex], CellCorners[CellIndex],
			EMainScreenAction::SelectItem, InventoryBindings, InventoryCellButtons, InventoryCellItemIds);
		Grid->AddChildToUniformGrid(Cell, CellIndex / Style::InventoryColumns, CellIndex % Style::InventoryColumns);
	}

	// If the selected item (from inventory) is no longer there, clear the selection.
	if (SelectionSource == ESelectionSource::Inventory && SelectedItemId != -1 && GetItemQty(SelectedItemId) <= 0)
	{
		SelectedItemId = -1;
		SelectionSource = ESelectionSource::None;
		RepaintActions();
	}

	UpdateSelectionHighlight();
}

// Recolors BOTH grids according to the active selection (only one at a time).
void UMainScreenWidget::UpdateSelectionHighlight()
{
	for (int32 i = 0; i < InventoryCellButtons.Num(); ++i)
	{
		if (UButton* Cell = InventoryCellButtons[i])
		{
			const bool bSel = (SelectionSource == ESelectionSource::Inventory)
				&& InventoryCellItemIds.IsValidIndex(i) && InventoryCellItemIds[i] == SelectedItemId;
			Cell->SetBackgroundColor(bSel ? Style::CellSelectedColor : Style::CellBgColor);
		}
	}
	for (int32 i = 0; i < StoreCellButtons.Num(); ++i)
	{
		if (UButton* Cell = StoreCellButtons[i])
		{
			const bool bSel = (SelectionSource == ESelectionSource::Store)
				&& StoreCellItemIds.IsValidIndex(i) && StoreCellItemIds[i] == SelectedItemId;
			Cell->SetBackgroundColor(bSel ? Style::CellSelectedColor : Style::CellBgColor);
		}
	}
}

// HandleInventoryUpdated NO longer paints the day-to-day inventory: it's only used to
// SEED the first time (it captures the on-chain inventory as the seed).
void UMainScreenWidget::HandleInventoryUpdated(const TArray<FStoreInventoryEntry>& Items)
{
	if (!bFirstTimeInProgress)
	{
		return; // outside the seed flow, the chain does not paint inventory.
	}

	PendingSeed.Reset();
	for (const FStoreInventoryEntry& Entry : Items)
	{
		if (Entry.Balance > 0)
		{
			PendingSeed.Add(Entry.ItemId, Entry.Balance);
		}
	}
	bSeedReady = true;
	UE_LOG(LogMainScreen, Log, TEXT("Seed ready: %d items from the chain"), PendingSeed.Num());
	TryFinalizeFirstTime();
}

// ---------------------------------------------------------------------------------
// Catalog -> GRID of icons in StoreContainer (on-chain). NO Buy button per
// cell: the cell is SELECTED (the purchase is triggered from the actions panel).
// ---------------------------------------------------------------------------------

void UMainScreenWidget::HandleCatalogUpdated(const TArray<FStoreCatalogItem>& Items)
{
	if (!StoreContainer)
	{
		return;
	}

	StoreContainer->ClearChildren();
	StoreBindings.Reset();
	StoreCellButtons.Reset();
	StoreCellItemIds.Reset();

	// WrapBox: fixed-size cells that wrap on their own according to the available width.
	UWrapBox* Wrap = WidgetTree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass());
	Wrap->SetInnerSlotPadding(FVector2D(Style::CellSpacing, Style::CellSpacing));
	StoreContainer->AddChild(Wrap);

	for (const FStoreCatalogItem& Item : Items)
	{
		// Corner text = price in ETH (same icon as in inventory).
		FString Corner = Item.PriceEth;

		// The three quivers (3/4/5) share the Quiver icon: we also show their CAPACITY
		// (5/10/20) to tell them apart at a glance. Only in the store, only quivers.
		if (IsQuiver(Item.ItemId))
		{
			Corner = FString::Printf(TEXT("x%d\n%s"), QuiverCapacity(Item.ItemId), *Item.PriceEth);
		}

		USizeBox* Cell = MakeItemCell(Item.ItemId, Corner, EMainScreenAction::SelectStoreItem,
			StoreBindings, StoreCellButtons, StoreCellItemIds);
		Wrap->AddChildToWrapBox(Cell);
	}

	UpdateSelectionHighlight();
	UE_LOG(LogMainScreen, Log, TEXT("Store painted: %d items"), Items.Num());
}

// ---------------------------------------------------------------------------------
// Progress -> medals in MedalsContainer (on-chain, read-only)
// ---------------------------------------------------------------------------------

void UMainScreenWidget::HandleProgressUpdated(const TArray<FStoreMedal>& Medals)
{
	if (!MedalsContainer)
	{
		return;
	}

	MedalsContainer->ClearChildren();

	for (const FStoreMedal& Medal : Medals)
	{
		FString Line = FString::Printf(TEXT("%s - %s"),
			*Medal.Name, Medal.bOwned ? TEXT("unlocked") : TEXT("locked"));

		if (Medal.bOwned && !Medal.Rarity.IsEmpty())
		{
			Line += FString::Printf(TEXT(" (%s)"), *Medal.Rarity);
		}

		// Greenish text if unlocked, dimmed if locked; inside a fixed-width cell.
		const FLinearColor Color = Medal.bOwned ? Style::TextOwnedColor : Style::TextLockedColor;
		UTextBlock* Text = MakeStyledText(WidgetTree, Line, Style::RowFontSize, Color, ETextJustify::Left);
		UBorder* Cell = MakeCell(WidgetTree, Text, Style::RowBgColor);
		USizeBox* Wrapped = WrapWidth(WidgetTree, Cell, Style::ListRowWidth);
		StyleSlotLeft(MedalsContainer->AddChild(Wrapped), Style::RowSlotPadding);
	}

	UE_LOG(LogMainScreen, Log, TEXT("Medals painted: %d"), Medals.Num());
}

// ---------------------------------------------------------------------------------
// UNIFIED selection (inventory / store) + context-based actions panel
// ---------------------------------------------------------------------------------

void UMainScreenWidget::SelectItem(int32 ItemId)
{
	// BP-callable API: equivalent to clicking an inventory cell.
	SelectFromInventory(ItemId);
}

void UMainScreenWidget::SelectFromInventory(int32 ItemId)
{
	SelectedItemId = ItemId;
	SelectionSource = ESelectionSource::Inventory;
	SetStatus(FString::Printf(TEXT("Inventory: %s"), *ItemName(ItemId)));
	UpdateSelectionHighlight(); // highlights without rebuilding the grid (safe during the click)
	RepaintActions();
}

void UMainScreenWidget::SelectFromStore(int32 ItemId)
{
	SelectedItemId = ItemId;
	SelectionSource = ESelectionSource::Store;
	SetStatus(FString::Printf(TEXT("Store: %s"), *ItemName(ItemId)));
	UpdateSelectionHighlight();
	RepaintActions();
}

void UMainScreenWidget::RepaintActions()
{
	if (!ActionsContainer)
	{
		return;
	}

	ActionsContainer->ClearChildren();
	ActionBindings.Reset();

	// --- GENERIC actions, always visible ---
	StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Battle"), EMainScreenAction::Battle, 0, ActionBindings)), Style::ButtonSlotPadding);
	StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Save"), EMainScreenAction::Save, 0, ActionBindings)), Style::ButtonSlotPadding);
	StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Log out"), EMainScreenAction::Logout, 0, ActionBindings)), Style::ButtonSlotPadding);

	// --- Contextual, depending on WHERE the selection comes from ---
	if (SelectionSource == ESelectionSource::Inventory && SelectedItemId != -1)
	{
		// Use actions on the inventory item (by stable ID).
		if (IsPotion(SelectedItemId))
		{
			StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Use"), EMainScreenAction::UsePotion, SelectedItemId, ActionBindings)), Style::ButtonSlotPadding);
		}
		else if (SelectedItemId == ITEM_BOTELLA)
		{
			StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Break"), EMainScreenAction::BreakBottle, SelectedItemId, ActionBindings)), Style::ButtonSlotPadding);
		}
		else if (SelectedItemId == ITEM_FLECHA || IsQuiver(SelectedItemId))
		{
			StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Shoot"), EMainScreenAction::ShootArrow, SelectedItemId, ActionBindings)), Style::ButtonSlotPadding);
		}
	}
	else if (SelectionSource == ESelectionSource::Store && SelectedItemId != -1)
	{
		if (SelectedItemId == ITEM_FLECHA)
		{
			// Quick arrow purchase: fixed quantities + "Fill". Each button carries its
			// quantity baked into the proxy (Quantity); "Fill" computes it on press
			// (BuyFill) so it doesn't go stale. All go through the capacity validation.
			StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Buy 1"),  EMainScreenAction::Buy, ITEM_FLECHA, ActionBindings, EButtonKind::Compact, 1)),  Style::ButtonSlotPadding);
			StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Buy 5"),  EMainScreenAction::Buy, ITEM_FLECHA, ActionBindings, EButtonKind::Compact, 5)),  Style::ButtonSlotPadding);
			StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Buy 10"), EMainScreenAction::Buy, ITEM_FLECHA, ActionBindings, EButtonKind::Compact, 10)), Style::ButtonSlotPadding);
			StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Fill"),     EMainScreenAction::BuyFill, ITEM_FLECHA, ActionBindings)), Style::ButtonSlotPadding);
		}
		else
		{
			// Other items: a single "Buy" (quantity 1), triggers the on-chain purchase.
			StyleSlotLeft(ActionsContainer->AddChild(MakeActionButton(TEXT("Buy"), EMainScreenAction::Buy, SelectedItemId, ActionBindings)), Style::ButtonSlotPadding);
		}
	}
}

// ---------------------------------------------------------------------------------
// Dispatcher for the dynamic buttons
// ---------------------------------------------------------------------------------

void UMainScreenWidget::HandleButtonAction(EMainScreenAction Action, int32 ItemId, int32 Quantity)
{
	switch (Action)
	{
	case EMainScreenAction::Buy:
		RequestBuy(ItemId, Quantity);
		break;

	case EMainScreenAction::BuyFill:
	{
		// "Fill": buys the arrows missing to reach capacity. Quantity computed
		// ON PRESS (not baked) to reflect the current state of the savegame.
		const int32 Capacity = GetBestQuiverCapacity();
		if (Capacity <= 0)
		{
			SetStatus(TEXT("You need a quiver to buy arrows."));
			break;
		}
		const int32 Missing = Capacity - GetItemQty(ITEM_FLECHA);
		if (Missing <= 0)
		{
			SetStatus(TEXT("Quiver full"));
			break;
		}
		RequestBuy(ITEM_FLECHA, Missing);
		break;
	}

	case EMainScreenAction::SelectItem:      SelectFromInventory(ItemId); break;
	case EMainScreenAction::SelectStoreItem: SelectFromStore(ItemId); break;
	case EMainScreenAction::Battle:     OnBattle(); break;
	case EMainScreenAction::Save:       OnSave(); break;
	case EMainScreenAction::Logout:     OnLogout(); break;
	case EMainScreenAction::UsePotion:  OnUsePotion(ItemId); break;
	case EMainScreenAction::BreakBottle:OnBreakBottle(ItemId); break;
	case EMainScreenAction::ShootArrow: OnShootArrow(); break;
	}
}

// ---------------------------------------------------------------------------------
// RequestBuy: validates the BUSINESS RULES (moved from the contract to Unreal, against the
// SAVEGAME) and, if they pass, triggers the on-chain purchase of Quantity units.
// ---------------------------------------------------------------------------------

void UMainScreenWidget::RequestBuy(int32 ItemId, int32 Quantity)
{
	if (Quantity < 1)
	{
		return;
	}

	// POTION (8/9): requires >=1 empty bottle (id 7). Rule moved from the contract; it's
	// counted from the SAVEGAME (same source that paints the inventory), not from the on-chain balance.
	if (IsPotion(ItemId))
	{
		const int32 Bottles = GetItemQty(ITEM_BOTELLA);
		if (Bottles < 1)
		{
			SetStatus(FString::Printf(TEXT("You need 1 empty bottle to buy a potion; you have %d."), Bottles));
			return; // the on-chain purchase is NOT triggered
		}
	}
	// ARROW (6): capacity is limited by the quiver. Rule moved from the contract; validated
	// here against the savegame: current_arrows + quantity_to_buy <= capacity.
	else if (ItemId == ITEM_FLECHA)
	{
		const int32 Capacity = GetBestQuiverCapacity();
		if (Capacity <= 0)
		{
			SetStatus(TEXT("You need a quiver to buy arrows."));
			return;
		}
		const int32 Current = GetItemQty(ITEM_FLECHA);
		if (Current + Quantity > Capacity)
		{
			// A partial number is not bought: the whole thing is aborted (more predictable for the player).
			SetStatus(FString::Printf(TEXT("Won't fit: your quiver holds %d, you have %d, you're requesting %d."), Capacity, Current, Quantity));
			return;
		}
	}

	if (UBlockchainStoreClient* SC = GetStoreClient())
	{
		// OnPurchaseCompleted carries neither id nor quantity; we remember them to reconcile.
		LastPurchasedItemId = ItemId;
		LastPurchasedQty = Quantity;
		SetStatus(FString::Printf(TEXT("Buying %d x %s..."), Quantity, *ItemName(ItemId)));
		SC->BuyItem(ItemId, Quantity); // BuyItem accepts a quantity: bought all at once, not one by one.
	}
}

int32 UMainScreenWidget::GetBestQuiverCapacity() const
{
	int32 Best = 0;
	for (int32 QId : { ITEM_CARCAJ5, ITEM_CARCAJ10, ITEM_CARCAJ20 })
	{
		if (GetItemQty(QId) > 0)
		{
			Best = FMath::Max(Best, QuiverCapacity(QId));
		}
	}
	return Best;
}

// ---------------------------------------------------------------------------------
// Session actions (modify SaveData + autosave + repaint)
// ---------------------------------------------------------------------------------

void UMainScreenWidget::OnBattle()
{
	if (!SaveData)
	{
		SetStatus(TEXT("No save loaded."));
		return;
	}

	const int32 DHearts = FMath::RandRange(1, 2);
	const int32 DMana = FMath::RandRange(20, 50);
	SaveData->CurrentHearts = FMath::Max(0, SaveData->CurrentHearts - DHearts);
	SaveData->CurrentMana = FMath::Max(0, SaveData->CurrentMana - DMana);

	PushStatsToUI();
	Autosave();

	if (SaveData->CurrentHearts == 0)
	{
		SetStatus(TEXT("Defeat"));
	}
	else
	{
		SetStatus(FString::Printf(TEXT("Battle! -%d health, -%d mana"), DHearts, DMana));
	}
}

void UMainScreenWidget::OnSave()
{
	Autosave();
	SetStatus(TEXT("Game saved"));
}

void UMainScreenWidget::OnLogout()
{
	// a) AUTOSAVE: don't lose progress on exit.
	Autosave();

	// b) Clears the widget's in-memory state (left in a neutral state).
	SaveData = nullptr;
	CurrentSlotName.Empty();
	SelectedItemId = -1;
	SelectionSource = ESelectionSource::None;
	LastPurchasedItemId = -1;
	LastPurchasedQty = 1;
	CurrentHearts = 0; MaxHearts = 0;
	Mana = 0; MaxMana = 0;

	if (InventoryContainer) { InventoryContainer->ClearChildren(); }
	if (StoreContainer)     { StoreContainer->ClearChildren(); }
	if (MedalsContainer)    { MedalsContainer->ClearChildren(); }
	if (ActionsContainer)   { ActionsContainer->ClearChildren(); }
	InventoryBindings.Reset();
	StoreBindings.Reset();
	ActionBindings.Reset();
	InventoryCellButtons.Reset();  InventoryCellItemIds.Reset();
	StoreCellButtons.Reset();      StoreCellItemIds.Reset();
	RepaintHearts();   // with Max=0 no hearts are painted
	ApplyManaBar();    // bar at 0
	SetStatus(TEXT("Logged out."));

	// c) Clears the authentication in the client (SIWE is stateless; does not call the bridge).
	if (UBlockchainStoreClient* SC = GetStoreClient())
	{
		SC->Logout();
	}

	// d) Notify. The widget does NOT change screen; the Level Blueprint listens to this and
	//    returns to the login.
	OnLogoutRequested.Broadcast();
}

void UMainScreenWidget::OnUsePotion(int32 ItemId)
{
	if (!SaveData)
	{
		return;
	}
	if (!IsPotion(ItemId))
	{
		return;
	}
	if (GetItemQty(ItemId) < 1)
	{
		SetStatus(TEXT("You don't have that potion."));
		return;
	}

	FString Msg;
	if (ItemId == ITEM_POCION_VIDA)
	{
		SaveData->CurrentHearts = FMath::Min(SaveData->MaxHearts, SaveData->CurrentHearts + 1);
		Msg = TEXT("You drink a life potion (+1 heart)");
	}
	else // ITEM_POCION_MANA
	{
		SaveData->CurrentMana = FMath::Min(SaveData->MaxMana, SaveData->CurrentMana + 50);
		Msg = TEXT("You drink a mana potion (+50)");
	}

	AddItemQty(ItemId, -1);       // consume the potion
	AddItemQty(ITEM_BOTELLA, +1); // leave an empty bottle

	PushStatsToUI();
	RepaintInventoryFromSave();
	Autosave();
	SetStatus(Msg);
}

void UMainScreenWidget::OnBreakBottle(int32 ItemId)
{
	if (!SaveData)
	{
		return;
	}
	if (GetItemQty(ITEM_BOTELLA) < 1)
	{
		SetStatus(TEXT("You have no empty bottles."));
		return;
	}

	AddItemQty(ITEM_BOTELLA, -1);
	RepaintInventoryFromSave();
	Autosave();
	SetStatus(TEXT("You break an empty bottle."));
}

void UMainScreenWidget::OnShootArrow()
{
	if (!SaveData)
	{
		return;
	}
	if (GetItemQty(ITEM_FLECHA) < 1)
	{
		SetStatus(TEXT("No arrows"));
		return;
	}

	AddItemQty(ITEM_FLECHA, -1);
	RepaintInventoryFromSave();
	Autosave();
	SetStatus(FString::Printf(TEXT("You shot an arrow (%d left)"), GetItemQty(ITEM_FLECHA)));
}

// ---------------------------------------------------------------------------------
// Purchase feedback + reconciliation with the savegame
// ---------------------------------------------------------------------------------

void UMainScreenWidget::HandlePurchaseStateChanged(const FString& RequestId, const FString& Status)
{
	SetStatus(FString::Printf(TEXT("Purchase: %s"), *Status));
}

void UMainScreenWidget::HandlePurchaseCompleted(const FString& TxHash)
{
	// The on-chain purchase was confirmed: reconcile the savegame (+quantity to the purchased item).
	if (SaveData && LastPurchasedItemId >= 0)
	{
		AddItemQty(LastPurchasedItemId, LastPurchasedQty);

		// Buying a potion CONSUMES an empty bottle (local rule, see RequestBuy).
		// Potions are bought one by one, so 1 bottle is consumed.
		// The clamp in AddItemQty prevents negatives if the bottle was used up in the meantime.
		if (IsPotion(LastPurchasedItemId))
		{
			AddItemQty(ITEM_BOTELLA, -1);
		}

		RepaintInventoryFromSave();
		Autosave();
		SetStatus(FString::Printf(TEXT("Purchase OK: +%d %s"), LastPurchasedQty, *ItemName(LastPurchasedItemId)));
	}
	else
	{
		SetStatus(FString::Printf(TEXT("Purchase OK (%s)"), *TxHash.Left(10)));
	}
	LastPurchasedItemId = -1;
	LastPurchasedQty = 1;

	// The medals may have changed with the purchase; refresh chain only.
	RefreshAll();
}

void UMainScreenWidget::HandlePurchaseFailed(const FString& Reason)
{
	LastPurchasedItemId = -1;
	LastPurchasedQty = 1;
	SetStatus(FString::Printf(TEXT("Purchase failed: %s"), *Reason));
}

// ---------------------------------------------------------------------------------
// Savegame inventory (helpers)
// ---------------------------------------------------------------------------------

int32 UMainScreenWidget::GetItemQty(int32 ItemId) const
{
	if (SaveData)
	{
		if (const int32* Found = SaveData->Inventory.Find(ItemId))
		{
			return *Found;
		}
	}
	return 0;
}

void UMainScreenWidget::AddItemQty(int32 ItemId, int32 Delta)
{
	if (!SaveData)
	{
		return;
	}
	int32& Qty = SaveData->Inventory.FindOrAdd(ItemId);
	Qty = FMath::Max(0, Qty + Delta);
	if (Qty == 0)
	{
		SaveData->Inventory.Remove(ItemId); // we don't leave zero entries
	}
}

// ---------------------------------------------------------------------------------
// Health / mana
// ---------------------------------------------------------------------------------

void UMainScreenWidget::SetHearts(int32 InCurrent, int32 InMax)
{
	MaxHearts = FMath::Max(0, InMax);
	CurrentHearts = FMath::Clamp(InCurrent, 0, MaxHearts);
	if (SaveData)
	{
		SaveData->MaxHearts = MaxHearts;
		SaveData->CurrentHearts = CurrentHearts;
		Autosave();
	}
	RepaintHearts();
}

void UMainScreenWidget::SetMana(int32 InMana, int32 InMax)
{
	MaxMana = FMath::Max(1, InMax);
	Mana = FMath::Clamp(InMana, 0, MaxMana);
	if (SaveData)
	{
		SaveData->MaxMana = MaxMana;
		SaveData->CurrentMana = Mana;
		Autosave();
	}
	ApplyManaBar();
}

void UMainScreenWidget::PushStatsToUI()
{
	if (SaveData)
	{
		MaxHearts = SaveData->MaxHearts;
		CurrentHearts = SaveData->CurrentHearts;
		MaxMana = SaveData->MaxMana;
		Mana = SaveData->CurrentMana;
	}
	RepaintHearts();
	ApplyManaBar();
}

void UMainScreenWidget::SetStatus(const FString& Message)
{
	if (StatusText)
	{
		StatusText->SetText(FText::FromString(Message));
	}
}

void UMainScreenWidget::RepaintHearts()
{
	if (!HeartsContainer)
	{
		return;
	}

	HeartsContainer->ClearChildren();

	for (int32 i = 0; i < MaxHearts; ++i)
	{
		UTextBlock* Heart = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Heart->SetText(FText::FromString(i < CurrentHearts ? TEXT("♥") : TEXT("♡")));
		HeartsContainer->AddChild(Heart);
	}
}

void UMainScreenWidget::ApplyManaBar()
{
	if (ManaBar)
	{
		ManaBar->SetPercent(MaxMana > 0 ? static_cast<float>(Mana) / static_cast<float>(MaxMana) : 0.f);
	}
}

// ---------------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------------

void UMainScreenWidget::Autosave()
{
	if (SaveData && !CurrentSlotName.IsEmpty())
	{
		UGameplayStatics::SaveGameToSlot(SaveData, CurrentSlotName, 0);
	}
}
