// Ration Seal Store: a drop-in add-on for the DS3 Mod Loader (API 3).
//  - Offers (sealOffers, API 3): the loader lists the three Ration Seal packs the Steam catalogue lacks, and the mod's
//    three circuit packs, three Schematics and eight part unlocks, each time the store opens. The loader owns the
//    catalogue, the pack-type and the money-checkout hooks (that code is off limits to mods); it refuses any money
//    checkout for these offers and lists none unless that refusal is in. The game's own code then handles seal price,
//    balance check, deduction and contents.
//  - Seal price (FUN_00513b00): the circuit packs' prices, and test overrides from seal_store.ini [debug].
//  - BuyWithSeals (FUN_0051dd70): logs pack type, price, result, rolled contents; with resource_pack_parts=1, adds one
//    part from the Resource Pack's own (never used) part tables to a Resource Pack roll. A circuit pack is charged as a
//    stock Resource Pack at the circuit pack's price, and the roll is replaced with a circuit marker.
//  - ApplyRecord (FUN_00519fb0): for a circuit marker, gives the circuit to the player charged instead (seals refunded
//    if the game adds none), so the game never reads the marker as an Ultra pack.
//  - PackPopup (FUN_0051a0c0): shows the circuit, or that it was not delivered, for a circuit marker.
//  - AddCategory / ListCategory / CategoryText (FUN_0051be90 / FUN_00526db0 / FUN_00b6a6e0): "upgrade circuits",
//    "schematics" and "weapon parts" store categories next to "resource packs", each listing only its own seal packs.
//  - DescribeOffer (FUN_00509260): for seal packs, blanks the money price and drops the money button from the hint.
//  - AddPcButtons (FUN_004fa570): for seal packs, adds only "back" and the seal button to the mouse button bar.
//  - RefreshItem (FUN_00517700): for seal packs, hides the "price" row (mPointsCost) and moves the "ration seals" row
//    (mCoinsCost) up into its place, keeping the row's seal balance (coinReserve) where it was; for everything else,
//    puts all three back.
//  - ResourcePopup (mid-function, 0x51A1F9 in FUN_0051a0c0): lists that part in the pack-contents popup, which never
//    lists parts for a Resource Pack.
// Only the seal-pack offer IDs are ever listed. Real offers and entitlements are never touched.
//
// Not reloadable on purpose (no DS3ModShutdown): the mid-function hook can't go through the loader's counting stubs,
// a purchase spans BuyWithSeals and ApplyRecord, and DescribeOffer hands the game pointers to this DLL's strings.
// Settings in seal_store.ini (the [packs] rows in mod.ini are read at every store open, so they apply at once);
// pity counts in ds3_seals_state.ini in the game folder, written only when a circuit purchase changes them; log in
// seal_store.log next to this mod.

#include "../ds3mod.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

const DS3ModHost* g_host = nullptr;

// ---- This mod's own log: plugins\SealStore\seal_store.log. Append, a dated header per session, emptied past 1 MB. ----
HANDLE g_logFile = INVALID_HANDLE_VALUE;
SRWLOCK g_logLock = SRWLOCK_INIT;

void LogInit(const wchar_t* modDir)
{
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\seal_store.log", modDir);
    g_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(g_logFile, &size) && size.QuadPart > 1024 * 1024) SetEndOfFile(g_logFile);  // pointer is at 0
    SetFilePointer(g_logFile, 0, nullptr, FILE_END);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char header[64];
    int n = sprintf_s(header, "\r\n===== session %04u-%02u-%02u =====\r\n", t.wYear, t.wMonth, t.wDay);
    DWORD written;
    WriteFile(g_logFile, header, static_cast<DWORD>(n), &written, nullptr);
}

void Log(const char* fmt, ...)
{
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = sprintf_s(line, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    // Leaves room for "\r\n"; a line too long is cut (the call then returns -1).
    int m = _vsnprintf_s(line + n, sizeof(line) - n - 2, _TRUNCATE, fmt, args);
    va_end(args);
    n = m < 0 ? static_cast<int>(strlen(line)) : n + m;
    line[n++] = '\r';
    line[n++] = '\n';
    AcquireSRWLockExclusive(&g_logLock);
    DWORD written;
    WriteFile(g_logFile, line, static_cast<DWORD>(n), &written, nullptr);
    ReleaseSRWLockExclusive(&g_logLock);
}

// ---- Settings: seal_store.ini next to this mod (host->iniPath). The [packs] keys are the Mods panel rows in
// mod.ini; [debug] is test-only and never shown in the panel. ----
struct Config {
    // [packs] resource_pack_parts=1: Resource Pack purchases also give one part from the pack's cut part tables.
    bool resourcePackParts = true;
    // [packs] circuit_packs=1: sell the Upgrade Circuit Pack, Elite Upgrade Circuit and Masterwork Circuit Pack.
    bool circuitPacks = true;
    // [packs] schematics=1: also sell the Advanced, Elite and Masterwork Schematics. Needs circuit_packs=1.
    bool schematics = true;
    // [packs] part_unlocks=1: sell the MK-V and MK-II part Schematics. Needs circuit_packs=1.
    bool partUnlocks = true;
    // [debug] test-only seal price overrides. 0 = the normal prices (Resource 10, Ultra 30, Epic 60, Upgrade Circuit
    // Pack 20, Elite Upgrade Circuit 60, Masterwork Circuit Pack 80, Advanced / Elite / Masterwork Schematic
    // 40 / 100 / 150, MK-V parts 60, MK-II parts 80).
    int resourcePrice = 0;
    int ultraPrice = 0;
    int epicPrice = 0;
    int circuitPrice = 0;
    int eliteCircuitPrice = 0;
    int masterworkPrice = 0;
    int advancedSchematicPrice = 0;
    int eliteSchematicPrice = 0;
    int masterworkSchematicPrice = 0;
    int mkvPartPrice = 0;
    int mkiiPartPrice = 0;
    // [debug] test-only pity overrides, in packs. 0 = normal (5 and 5).
    int pityElitePacks = 0;
    int pityMasterworkPacks = 0;
    // [debug] dumps (read-only): what the stock packs contain, every loaded circuit, every weapon-part entry.
    bool dumpPacks = false;
    bool dumpCircuits = false;
    bool dumpParts = false;
    // [debug] keep_money_button=1: the money price row and "purchase" button stay on seal packs, so a test can check
    // that the loader refuses that checkout.
    bool keepMoneyButton = false;
    // ds3_seals_state.ini in the game folder: the pity counts. Written only when a circuit purchase changes them.
    wchar_t statePath[MAX_PATH] = {};
};
Config g_config;

bool ReadFlag(const wchar_t* section, const wchar_t* key, bool def)
{
    return GetPrivateProfileIntW(section, key, def ? 1 : 0, g_host->iniPath) != 0;
}

int ReadCount(const wchar_t* section, const wchar_t* key)
{
    int v = static_cast<int>(GetPrivateProfileIntW(section, key, 0, g_host->iniPath));
    return v < 0 ? 0 : v;
}

void ReadSettings()
{
    Config& c = g_config;
    c.resourcePackParts = ReadFlag(L"packs", L"resource_pack_parts", true);
    c.circuitPacks = ReadFlag(L"packs", L"circuit_packs", true);
    c.schematics = ReadFlag(L"packs", L"schematics", true);
    c.partUnlocks = ReadFlag(L"packs", L"part_unlocks", true);
    c.resourcePrice = ReadCount(L"debug", L"resource_price");
    c.ultraPrice = ReadCount(L"debug", L"ultra_price");
    c.epicPrice = ReadCount(L"debug", L"epic_price");
    c.circuitPrice = ReadCount(L"debug", L"circuit_price");
    c.eliteCircuitPrice = ReadCount(L"debug", L"elite_circuit_price");
    c.masterworkPrice = ReadCount(L"debug", L"masterwork_price");
    c.advancedSchematicPrice = ReadCount(L"debug", L"advanced_schematic_price");
    c.eliteSchematicPrice = ReadCount(L"debug", L"elite_schematic_price");
    c.masterworkSchematicPrice = ReadCount(L"debug", L"masterwork_schematic_price");
    c.mkvPartPrice = ReadCount(L"debug", L"mkv_part_price");
    c.mkiiPartPrice = ReadCount(L"debug", L"mkii_part_price");
    c.pityElitePacks = ReadCount(L"debug", L"pity_elite_packs");
    c.pityMasterworkPacks = ReadCount(L"debug", L"pity_masterwork_packs");
    c.dumpPacks = ReadFlag(L"debug", L"dump_packs", false);
    c.dumpCircuits = ReadFlag(L"debug", L"dump_circuits", false);
    c.dumpParts = ReadFlag(L"debug", L"dump_parts", false);
    c.keepMoneyButton = ReadFlag(L"debug", L"keep_money_button", false);
}

constexpr const char* kUltraOfferId = "OFB-EAST:109546252";  // pack type 3 (Silver)
constexpr const char* kEpicOfferId = "OFB-EAST:109546253";   // pack type 4 (Gold)
// Pack type 2 (Bronze) has no offer in this build; the game only maps the two IDs above. This ID is the mod's own, so
// it can never match a real EA offer. OfferStatus gives it type 2 and makes it seal-purchasable.
constexpr const char* kResourceOfferId = "DS3SEALS:RESOURCE_PACK";
constexpr int kResourcePackType = 2;
// The mod's circuit packs. Their types exist only in the store entry and the price lookup; the game's purchase routine
// only ever sees type 2 (an unknown type there would charge and then fail).
constexpr const char* kCircuitOfferId = "DS3SEALS:CIRCUIT_PACK";
constexpr const char* kEliteCircuitOfferId = "DS3SEALS:ELITE_CIRCUIT";
constexpr const char* kMasterworkOfferId = "DS3SEALS:MASTERWORK_PACK";
constexpr int kCircuitPackType = 5;
constexpr int kEliteCircuitType = 6;
constexpr int kMasterworkPackType = 7;
constexpr int kCircuitPrice = 20;
constexpr int kEliteCircuitPrice = 60;
constexpr int kMasterworkPrice = 80;
constexpr const char* kCircuitPackName = "Upgrade Circuit Pack";
constexpr const char* kEliteCircuitName = "Elite Upgrade Circuit";
constexpr const char* kMasterworkPackName = "Masterwork Circuit Pack";
// The Schematics: each unlocks one circuit of its tier the save has not found, recorded as found the way a world pickup
// records it.
constexpr const char* kAdvancedSchematicOfferId = "DS3SEALS:ADVANCED_SCHEMATIC";
constexpr const char* kEliteSchematicOfferId = "DS3SEALS:ELITE_SCHEMATIC";
constexpr const char* kMasterworkSchematicOfferId = "DS3SEALS:MASTERWORK_SCHEMATIC";
constexpr int kAdvancedSchematicType = 8;
constexpr int kEliteSchematicType = 9;
constexpr int kMasterworkSchematicType = 10;
constexpr int kAdvancedSchematicPrice = 40;
constexpr int kEliteSchematicPrice = 100;
constexpr int kMasterworkSchematicPrice = 150;
constexpr const char* kAdvancedSchematicName = "Advanced Schematic";
constexpr const char* kEliteSchematicName = "Elite Schematic";
constexpr const char* kMasterworkSchematicName = "Masterwork Schematic";
// Part unlocks: each gives one MK-V or MK-II weapon part of its type that the save does not have, through the game's
// own Ultra/Epic part grant. Types 11-14 MK-V, 15-18 MK-II, each attachment / tip / tool / frame.
constexpr int kFirstPartType = 11;
constexpr int kPartGroups = 8;
constexpr int kMkvPartPrice = 60;
constexpr int kMkiiPartPrice = 80;

enum class OfferKind { CircuitPack, Schematic, Part };

// The mod's own offers: store ID, name, pack type, normal price, the [debug] price that overrides it, and what it is.
struct ModOffer {
    const char* id;
    const char* name;
    int type;
    int price;
    int Config::*testPrice;
    OfferKind kind;
};
constexpr ModOffer kModOffers[] = {
    {kCircuitOfferId, kCircuitPackName, kCircuitPackType, kCircuitPrice, &Config::circuitPrice, OfferKind::CircuitPack},
    {kEliteCircuitOfferId, kEliteCircuitName, kEliteCircuitType, kEliteCircuitPrice, &Config::eliteCircuitPrice,
     OfferKind::CircuitPack},
    {kMasterworkOfferId, kMasterworkPackName, kMasterworkPackType, kMasterworkPrice, &Config::masterworkPrice,
     OfferKind::CircuitPack},
    {kAdvancedSchematicOfferId, kAdvancedSchematicName, kAdvancedSchematicType, kAdvancedSchematicPrice,
     &Config::advancedSchematicPrice, OfferKind::Schematic},
    {kEliteSchematicOfferId, kEliteSchematicName, kEliteSchematicType, kEliteSchematicPrice,
     &Config::eliteSchematicPrice, OfferKind::Schematic},
    {kMasterworkSchematicOfferId, kMasterworkSchematicName, kMasterworkSchematicType, kMasterworkSchematicPrice,
     &Config::masterworkSchematicPrice, OfferKind::Schematic},
    {"DS3SEALS:MKV_ATTACHMENT", "MK-V Attachment Schematic", 11, kMkvPartPrice, &Config::mkvPartPrice, OfferKind::Part},
    {"DS3SEALS:MKV_TIP", "MK-V Tip Schematic", 12, kMkvPartPrice, &Config::mkvPartPrice, OfferKind::Part},
    {"DS3SEALS:MKV_TOOL", "MK-V Tool Schematic", 13, kMkvPartPrice, &Config::mkvPartPrice, OfferKind::Part},
    {"DS3SEALS:MKV_FRAME", "MK-V Frame Schematic", 14, kMkvPartPrice, &Config::mkvPartPrice, OfferKind::Part},
    {"DS3SEALS:MKII_ATTACHMENT", "MK-II Attachment Schematic", 15, kMkiiPartPrice, &Config::mkiiPartPrice,
     OfferKind::Part},
    {"DS3SEALS:MKII_TIP", "MK-II Tip Schematic", 16, kMkiiPartPrice, &Config::mkiiPartPrice, OfferKind::Part},
    {"DS3SEALS:MKII_TOOL", "MK-II Tool Schematic", 17, kMkiiPartPrice, &Config::mkiiPartPrice, OfferKind::Part},
    {"DS3SEALS:MKII_FRAME", "MK-II Frame Schematic", 18, kMkiiPartPrice, &Config::mkiiPartPrice, OfferKind::Part},
};

const ModOffer* ModOfferById(const char* id)
{
    for (const ModOffer& o : kModOffers) {
        if (std::strcmp(id, o.id) == 0) return &o;
    }
    return nullptr;
}

const ModOffer* ModOfferByType(int type)
{
    for (const ModOffer& o : kModOffers) {
        if (o.type == type) return &o;
    }
    return nullptr;
}

// Store entries, as the loader's OfferStatus hook sets them for these offers (src/sealoffers.cpp): status 4 =
// seal-purchasable. Read here for the categories.
constexpr int kStatusSealPurchasable = 4;
constexpr uintptr_t kStoreManagerPtrVa = 0x011D3B80;  // store manager: entries at +0x10, stride 0x9B0, count +0x26C10
constexpr uintptr_t kStoreEntriesOffset = 0x10;
constexpr uintptr_t kStoreEntryStride = 0x9B0;
constexpr uintptr_t kStoreCountOffset = 0x26C10;
constexpr uintptr_t kEntryTypeOffset = 0x4;

// FUN_00513b00: __cdecl int GetSealPrice(int packType)
constexpr uintptr_t kSealPriceVa = 0x00513B00;
constexpr unsigned char kSealPriceBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x8B, 0x45, 0x08, 0x56, 0x33, 0xF6};

// FUN_0051dd70: __cdecl bool BuyWithSeals(int packType, unsigned price). Deducts seals and rolls contents. Hooked to
// route the mod's own purchases and add the Resource Pack part.
constexpr uintptr_t kBuyWithSealsVa = 0x0051DD70;
constexpr unsigned char kBuyWithSealsBytes[] = {0x55, 0x8B, 0xEC, 0x53, 0x33, 0xDB, 0x80, 0x3D,
                                                0xE7, 0x69, 0x1E, 0x01, 0x0F};
// FUN_00509260: __thiscall void DescribeOffer(int index, Str* name, Str* desc, Str* moneyPrice, Str* buttons,
//                                             bool* downloadable, bool* sealPurchasable). Only caller: 0x5177D9.
// moneyPrice becomes mDLCPointsPrice (the "price 0.00" row); buttons is the prompt bar. Cosmetic only.
constexpr uintptr_t kDescribeOfferVa = 0x00509260;
constexpr unsigned char kDescribeOfferBytes[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x64, 0xA1, 0x00,
                                                 0x00, 0x00, 0x00, 0x68, 0xB3, 0x1F, 0xCD, 0x00};
// The game's bar for seal-purchasable offers is "[RDLeft]$menu_button_purchase_mtx  [MenuA]$menu_button_purchase"
// plus "  [MenuB]$ui_back_lc". This keeps the seal button and back, and drops the money button.
constexpr const char* kSealOnlyButtons = "[RDLeft]$menu_button_purchase_mtx  [MenuB]$ui_back_lc";

// FUN_004fa570: __thiscall void AddPcButtons() — the store's mouse buttons (vtable 0xD4898C slot 6; this = screen+0x160).
// It adds "back", then for seal-purchasable items (this+0x161 = mIsMTX) a money "purchase" plus the seal button.
constexpr uintptr_t kAddPcButtonsVa = 0x004FA570;
constexpr unsigned char kAddPcButtonsBytes[] = {0x56, 0x8B, 0xF1, 0xE8, 0xF8, 0x8F, 0x13, 0x00,
                                                0x80, 0xBE, 0x61, 0x01, 0x00, 0x00, 0x00, 0x6A};
constexpr uintptr_t kIsMtxOffset = 0x161;
// FUN_00633570: __thiscall void AddBackButton(). FUN_00633510: __thiscall void AddButton(id, labelHash, pad).
constexpr uintptr_t kAddBackButtonVa = 0x00633570;
constexpr unsigned char kAddBackButtonBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x56, 0x6A, 0x0B, 0x8B, 0xF1};
constexpr uintptr_t kAddButtonVa = 0x00633510;
constexpr unsigned char kAddButtonBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x45, 0x08, 0x88, 0x45, 0xFB};
constexpr uint32_t kSealButtonId = 0x65787432;    // 'ext2'
constexpr uint32_t kSealButtonLabel = 0x83E02920; // hashed $menu_button_purchase_mtx
constexpr int kSealButtonPad = 10;

// FUN_00517700: __thiscall void RefreshItem(item, ?, unsigned index, const char* kind), ret 10h. Fills the detail
// fields (mIsMTX at screen+0x2C1, prices, …) and runs the store script's UpdateNonMenuItems.
constexpr uintptr_t kRefreshItemVa = 0x00517700;
constexpr unsigned char kRefreshItemBytes[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x30, 0x26,
                                               0xCD, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};
constexpr uintptr_t kScreenIsMtxOffset = 0x2C1;
// Native access to the store movie by path, as the game uses for mMask._height / ._visible.
// FUN_00a46f70: __cdecl void GetNumber(path, float* out) — 0 when the path does not resolve.
// FUN_00a48680: __cdecl void SetBool(path, value). FUN_00a485d0: __cdecl void SetNumber(path, float).
// (Not FUN_00a47460: it only works on text fields and crashes on a movie clip such as the price row.)
constexpr uintptr_t kGetNumberVa = 0x00A46F70;
constexpr unsigned char kGetNumberBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0x56, 0x57, 0x6A, 0x00, 0xE8};
constexpr uintptr_t kSetBoolVa = 0x00A48680;
constexpr unsigned char kSetBoolBytes[] = {0x55, 0x8B, 0xEC, 0x56, 0x6A, 0x00, 0x6A, 0x08};
constexpr uintptr_t kSetNumberVa = 0x00A485D0;
constexpr unsigned char kSetNumberBytes[] = {0x55, 0x8B, 0xEC, 0x56, 0x6A, 0x00, 0x6A, 0x08, 0xFF, 0x15};
// The "price" row is the clip mPointsCost and the "ration seals" row its sibling mCoinsCost (store script
// RefreshDLCitemInfo). In game the price row was found at FE17.level1.menu.mNonMenuItems.mPointsCost.
constexpr const char* kSealRowName = "mCoinsCost";
constexpr const char* kPriceRowCandidates[] = {
    "FE17.level1.menu.mNonMenuItems.mDLCDesc.mPointsCost",
    "FE17.level1.menu.mNonMenuItems.mPointsCost",
    "FE17.level1.menu.mPointsCost",
};

// Game global that BuyWithSeals fills in.
constexpr uintptr_t kRolledItemsVa = 0x011E69E4;  // 3 value bytes, then 3 slot bytes (0x0F = empty)

using FreeFn = void(__cdecl*)(void*);

struct StringHolder {
    const char* ptr;
    uint32_t length;
    uint32_t capacity;  // equals length in every real record
    FreeFn free;
};

struct SealPack {
    const char* offerId;
    const char* name;
    const char* description;
};

constexpr SealPack kSealPacks[] = {
    {kResourceOfferId, "Resource Pack", "A small pack of resources, bought with Ration Seals."},
    {kUltraOfferId, "Ultra Weapon and Resource Pack", "A pack of weapon parts and resources, bought with Ration Seals."},
    {kEpicOfferId, "Epic Weapon and Resource Pack",
     "A larger pack of weapon parts and resources, bought with Ration Seals."},
    {kCircuitOfferId, kCircuitPackName, ""},  // description changes with the pity counts
    {kEliteCircuitOfferId, kEliteCircuitName,
     "One random Elite circuit you have found (+3 in one stat)."},
    {kMasterworkOfferId, kMasterworkPackName, ""},  // description changes with the pity count
    {kAdvancedSchematicOfferId, kAdvancedSchematicName, "Unlocks an Advanced circuit (+2) you have not found yet."},
    {kEliteSchematicOfferId, kEliteSchematicName, "Unlocks an Elite circuit (+3 in one stat) you have not found yet."},
    {kMasterworkSchematicOfferId, kMasterworkSchematicName,
     "Unlocks a Masterwork circuit (+3/+3) you have not found yet."},
    {"DS3SEALS:MKV_ATTACHMENT", "MK-V Attachment Schematic", "Unlocks an MK-V attachment you do not have yet."},
    {"DS3SEALS:MKV_TIP", "MK-V Tip Schematic", "Unlocks an MK-V tip you do not have yet."},
    {"DS3SEALS:MKV_TOOL", "MK-V Tool Schematic", "Unlocks an MK-V tool you do not have yet."},
    {"DS3SEALS:MKV_FRAME", "MK-V Frame Schematic", "Unlocks an MK-V frame you do not have yet."},
    {"DS3SEALS:MKII_ATTACHMENT", "MK-II Attachment Schematic", "Unlocks an MK-II attachment you do not have yet."},
    {"DS3SEALS:MKII_TIP", "MK-II Tip Schematic", "Unlocks an MK-II tip you do not have yet."},
    {"DS3SEALS:MKII_TOOL", "MK-II Tool Schematic", "Unlocks an MK-II tool you do not have yet."},
    {"DS3SEALS:MKII_FRAME", "MK-II Frame Schematic", "Unlocks an MK-II frame you do not have yet."},
};

// __fastcall with a dummy EDX matches __thiscall + stack args with callee cleanup.
using SealPriceFn = int(__cdecl*)(int packType);
using BuyWithSealsFn = bool(__cdecl*)(int packType, unsigned price);
using DescribeOfferFn = void(__fastcall*)(void* self, void* edx, int index, StringHolder* name, StringHolder* desc,
                                          StringHolder* moneyPrice, StringHolder* buttons, bool* downloadable,
                                          bool* sealPurchasable);

SealPriceFn g_origSealPrice = nullptr;
BuyWithSealsFn g_origBuyWithSeals = nullptr;
DescribeOfferFn g_origDescribeOffer = nullptr;
using AddPcButtonsFn = void(__fastcall*)(void* self, void* edx);
using AddButtonFn = void(__fastcall*)(void* self, void* edx, uint32_t id, uint32_t labelHash, int pad);
AddPcButtonsFn g_origAddPcButtons = nullptr;
const auto g_addBackButton = reinterpret_cast<AddPcButtonsFn>(kAddBackButtonVa);
const auto g_addButton = reinterpret_cast<AddButtonFn>(kAddButtonVa);
using RefreshItemFn = void(__fastcall*)(void* self, void* edx, void* item, int unknown, unsigned index,
                                        const char* kind);
using GetNumberFn = void(__cdecl*)(const char* path, float* out);
using SetBoolFn = void(__cdecl*)(const char* path, bool value);
using SetNumberFn = void(__cdecl*)(const char* path, float value);
RefreshItemFn g_origRefreshItem = nullptr;
const auto g_getNumber = reinterpret_cast<GetNumberFn>(kGetNumberVa);
const auto g_setBool = reinterpret_cast<SetBoolFn>(kSetBoolVa);
const auto g_setNumber = reinterpret_cast<SetNumberFn>(kSetNumberVa);
char g_priceRowVisible[96] = {};  // "<price row>._visible" once found; empty until then
char g_sealRowY[96] = {};         // "<seal row>._y"; empty if the seal row was not found
char g_balanceY[128] = {};        // "<seal row>.coinReserve._y": the seal balance, a child of the seal row
float g_balanceY0 = 0.0f;
float g_priceRowY = 0.0f;         // where the seal row moves to for seal packs
float g_sealRowY0 = 0.0f;         // where the seal row goes back to
bool g_priceRowMissLogged = false;
LONG g_storeOpens = 0;

constexpr const char* kResourcePartsDescription =
    "A small pack of resources and one standard weapon part, bought with Ration Seals.";

// Strings this mod hands the game (DescribeOffer) are static; the game calls this instead of its own free.
void __cdecl NoFree(void*) {}

StringHolder MakeString(const char* s)
{
    auto len = static_cast<uint32_t>(std::strlen(s));
    return {s, len, len, &NoFree};
}

// Copies a game string for logging/comparison. Returns false if the pointer can't be read.
bool SafeCopyString(const char* src, char* dst, size_t dstSize)
{
    __try {
        size_t i = 0;
        for (; i + 1 < dstSize && src[i]; ++i) {
            unsigned char c = static_cast<unsigned char>(src[i]);
            dst[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
        }
        dst[i] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dst[0] = '\0';
        return false;
    }
}

bool SafeReadU32(uintptr_t addr, uint32_t* out)
{
    __try {
        *out = *reinterpret_cast<const uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Set once every piece the circuit packs need is in: price, purchase, grant and popup.
bool g_circuitPacksReady = false;
// Set once the SealPrice hook is in: the mod's own types need it, and so do the [debug] test prices.
bool g_sealPriceHooked = false;

const char* CircuitPackDescription();
const char* MasterworkPackDescription();
void RefreshCircuitAvailability();
bool ModOfferAvailable(const char* offerId);

void DumpPacks();
void AddCutPart();
void DumpCircuits();
void DumpParts();

bool PriceOverride()
{
    const Config& c = g_config;
    return c.resourcePrice > 0 || c.ultraPrice > 0 || c.epicPrice > 0 || c.circuitPrice > 0 ||
           c.eliteCircuitPrice > 0 || c.masterworkPrice > 0 || c.advancedSchematicPrice > 0 ||
           c.eliteSchematicPrice > 0 || c.masterworkSchematicPrice > 0 || c.mkvPartPrice > 0 || c.mkiiPartPrice > 0;
}

// The [packs] switches, read live: a switched-off kind is simply not listed at the next store open.
bool SwitchedOn(const ModOffer& offer)
{
    if (!g_config.circuitPacks) return false;
    if (offer.kind == OfferKind::Schematic) return g_config.schematics;
    if (offer.kind == OfferKind::Part) return g_config.partUnlocks;
    return true;
}

int StockPackType(const char* id)
{
    if (std::strcmp(id, kResourceOfferId) == 0) return kResourcePackType;
    if (std::strcmp(id, kUltraOfferId) == 0) return 3;
    if (std::strcmp(id, kEpicOfferId) == 0) return 4;
    return 0;
}

// The loader calls this each time the bench store opens (sealOffers, API 3): which offers to list now. The loader
// checks and copies each one, fills the money fields itself, and refuses any money checkout for them.
int __cdecl ListOffers(DS3SealOffer* out, int max, void*)
{
    LONG call = InterlockedIncrement(&g_storeOpens);
    Log("store open #%ld", call);
    if (call == 1 && g_config.dumpPacks) DumpPacks();
    if (g_config.dumpCircuits) DumpCircuits();
    if (g_config.dumpParts) DumpParts();
    // A test price that cannot be applied lists nothing rather than test silently at normal prices.
    if (PriceOverride() && !g_sealPriceHooked) {
        Log("*** [debug] test prices are set but the price hook is not in; nothing listed ***");
        return 0;
    }
    if (g_circuitPacksReady) RefreshCircuitAvailability();
    int n = 0;
    for (const SealPack& pack : kSealPacks) {
        const ModOffer* offer = ModOfferById(pack.offerId);
        if (offer && (!g_circuitPacksReady || !SwitchedOn(*offer) || !ModOfferAvailable(pack.offerId))) continue;
        if (n >= max) {
            Log("  no room for %s (%d offers already)", pack.offerId, n);
            break;
        }
        bool withPart = g_config.resourcePackParts && std::strcmp(pack.offerId, kResourceOfferId) == 0;
        const char* description = withPart ? kResourcePartsDescription : pack.description;
        if (std::strcmp(pack.offerId, kCircuitOfferId) == 0) description = CircuitPackDescription();
        if (std::strcmp(pack.offerId, kMasterworkOfferId) == 0) description = MasterworkPackDescription();
        out[n].id = pack.offerId;
        out[n].name = pack.name;
        out[n].description = description;
        out[n].packType = offer ? offer->type : StockPackType(pack.offerId);
        ++n;
    }
    Log("  %d offer(s) listed", n);
    return n;
}

int __cdecl HookSealPrice(int packType)
{
    if (const ModOffer* offer = ModOfferByType(packType))
        return g_config.*offer->testPrice > 0 ? g_config.*offer->testPrice : offer->price;
    int price = g_origSealPrice(packType);
    int override = 0;
    if (packType == kResourcePackType)
        override = g_config.resourcePrice;
    else if (packType == 1 || packType == 3)
        override = g_config.ultraPrice;
    else if (packType == 4)
        override = g_config.epicPrice;
    if (override > 0) return override;
    return price;
}

bool BuyCircuit(int packType, unsigned price);
bool BuyPart(const ModOffer* offer, unsigned price);

bool __cdecl HookBuyWithSeals(int packType, unsigned price)
{
    Log("BuyWithSeals: packType=%d price=%u", packType, price);
    if (const ModOffer* offer = ModOfferByType(packType)) {
        bool bought = offer->kind == OfferKind::Part ? BuyPart(offer, price) : BuyCircuit(packType, price);
        return bought;
    }
    bool ok = g_origBuyWithSeals(packType, price);
    Log("BuyWithSeals -> %s", ok ? "OK (seals deducted, contents rolled)" : "refused by game");
    if (ok && packType == kResourcePackType && g_config.resourcePackParts) AddCutPart();
    return ok;
}

// Frees the game's string with its own free function and points the holder at a static string instead.
void ReplaceString(StringHolder* s, const char* text)
{
    if (s->ptr && s->free) s->free(const_cast<char*>(s->ptr));
    *s = MakeString(text);
}

void __fastcall HookDescribeOffer(void* self, void* edx, int index, StringHolder* name, StringHolder* desc,
                                  StringHolder* moneyPrice, StringHolder* buttons, bool* downloadable,
                                  bool* sealPurchasable)
{
    g_origDescribeOffer(self, edx, index, name, desc, moneyPrice, buttons, downloadable, sealPurchasable);
    // On Steam only our packs are seal-purchasable, and the loader refuses their money checkout anyway.
    if (!sealPurchasable || !*sealPurchasable || !moneyPrice || !buttons) return;
    if (!g_config.keepMoneyButton) {
        ReplaceString(moneyPrice, "");
        ReplaceString(buttons, kSealOnlyButtons);
    }
    // The pity packs' descriptions show the counts as they are now, not as they were at store open.
    char shownName[64];
    if (g_circuitPacksReady && desc && name && name->ptr && SafeCopyString(name->ptr, shownName, sizeof(shownName))) {
        if (std::strcmp(shownName, kCircuitPackName) == 0) ReplaceString(desc, CircuitPackDescription());
        if (std::strcmp(shownName, kMasterworkPackName) == 0) ReplaceString(desc, MasterworkPackDescription());
    }
}

// Seal packs get "back" and "purchase with ration seals" only; the money "purchase" button is left out.
void __fastcall HookAddPcButtons(void* self, void* edx)
{
    if (g_config.keepMoneyButton || !*(reinterpret_cast<const uint8_t*>(self) + kIsMtxOffset)) {
        g_origAddPcButtons(self, edx);
        return;
    }
    g_addBackButton(self, edx);
    g_addButton(self, edx, kSealButtonId, kSealButtonLabel, kSealButtonPad);
}

// The seal row is the price row's sibling. Records where it sits so it can move up into the price row's place.
void FindSealRow(const char* priceRowPath)
{
    const char* lastDot = std::strrchr(priceRowPath, '.');
    if (!lastDot) return;
    char base[96], widthPath[96];
    sprintf_s(base, "%.*s.%s", static_cast<int>(lastDot - priceRowPath), priceRowPath, kSealRowName);
    sprintf_s(widthPath, "%s._width", base);
    float width = 0.0f;
    g_getNumber(widthPath, &width);
    if (width <= 0.0f) {
        Log("seal row %s not found; it stays where it is", base);
        return;
    }
    sprintf_s(g_sealRowY, "%s._y", base);
    g_getNumber(g_sealRowY, &g_sealRowY0);
    Log("seal row found: %s (width %.1f, y %.1f)", base, width, g_sealRowY0);

    // The seal balance in the bottom-left corner belongs to the seal row; moving the row up would drag it into the
    // item list, so it gets moved back down by the same amount.
    char balance[128];
    sprintf_s(balance, "%s.coinReserve", base);
    sprintf_s(widthPath, "%s._width", balance);
    width = 0.0f;
    g_getNumber(widthPath, &width);
    if (width <= 0.0f) {
        Log("seal balance %s not found; it moves with the seal row", balance);
        return;
    }
    sprintf_s(g_balanceY, "%s._y", balance);
    g_getNumber(g_balanceY, &g_balanceY0);
    Log("seal balance found: %s (width %.1f, y %.1f)", balance, width, g_balanceY0);
}

// Finds the price row; retried on each refresh until the store movie has it. Runs on the game's UI thread, like the
// game's own calls.
void FindPriceRow()
{
    for (const char* path : kPriceRowCandidates) {
        char widthPath[96];
        sprintf_s(widthPath, "%s._width", path);
        float width = 0.0f;
        g_getNumber(widthPath, &width);
        if (width > 0.0f) {
            sprintf_s(g_priceRowVisible, "%s._visible", path);
            char yPath[96];
            sprintf_s(yPath, "%s._y", path);
            g_getNumber(yPath, &g_priceRowY);
            Log("price row found: %s (width %.1f, y %.1f)", path, width, g_priceRowY);
            FindSealRow(path);
            return;
        }
    }
    if (!g_priceRowMissLogged) Log("price row not found yet; it stays visible until it is");
    g_priceRowMissLogged = true;
}

// The game shows the "ration seals" row only for seal-purchasable items; this hides the "price" row for exactly those.
void __fastcall HookRefreshItem(void* self, void* edx, void* item, int unknown, unsigned index, const char* kind)
{
    g_origRefreshItem(self, edx, item, unknown, index, kind);
    if (!g_priceRowVisible[0]) FindPriceRow();
    if (!g_priceRowVisible[0]) return;
    bool sealPack = !g_config.keepMoneyButton && *(reinterpret_cast<const uint8_t*>(self) + kScreenIsMtxOffset) != 0;
    g_setBool(g_priceRowVisible, !sealPack);
    if (g_sealRowY[0]) g_setNumber(g_sealRowY, sealPack ? g_priceRowY : g_sealRowY0);
    if (g_balanceY[0]) g_setNumber(g_balanceY, sealPack ? g_balanceY0 + (g_sealRowY0 - g_priceRowY) : g_balanceY0);
}

bool BytesMatch(uintptr_t va, const unsigned char* expected, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(va), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return std::memcmp(reinterpret_cast<void*>(va), expected, len) == 0;
}

// Through the loader (host->hook): it checks the bytes, the range and the claims, then creates and enables the hook.
bool CreateHookChecked(const char* name, uintptr_t va, const unsigned char* bytes, size_t len, void* detour, void** orig)
{
    if (!g_host->hook(g_host, name, static_cast<unsigned>(va), bytes, static_cast<unsigned>(len), detour, orig)) {
        Log("%s @ %08X: not installed (the reason is in ds3_mods.log)", name, static_cast<unsigned>(va));
        return false;
    }
    return true;
}

// ---- dump_packs (debug, read-only): what the stock packs contain. ----

struct Guid {
    uint32_t d[4];
};

// FUN_00997ab0: __thiscall void NameToGuid(const char* assetName), this = Guid* out.
constexpr uintptr_t kNameToGuidVa = 0x00997AB0;
constexpr unsigned char kNameToGuidBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x83, 0xEC,
                                              0x28, 0x56, 0x8B, 0xF1, 0x85, 0xC0, 0x74, 0x7E};
// Asset lookups, all __cdecl data* Lookup(const Guid*), 0 if missing or of another class. Same prologue for all.
constexpr uintptr_t kPackItemVa = 0x004CDE60;   // pack: resource ranges +0x2D0, part table refs +0x300
constexpr uintptr_t kPackStoreVa = 0x0050A710;  // pack: seal price +0x14C
constexpr uintptr_t kPackRollVa = 0x0050A780;   // pack: base part count +0x350, extra-part chance +0x360
constexpr uintptr_t kPartTableVa = 0x006DF080;  // part table: 16 entries at +0x30, stride 0x14
constexpr uintptr_t kPartItemVa = 0x004CDDF0;   // part entry: item +0x2D0, extra grants +0x2F0..+0x2FC
constexpr uintptr_t kCircuitVa = 0x0043FDA0;    // circuit (class FUN_004f2560): bonuses +0x2D0 reload, +0x2D4 rate
                                                // of fire, +0x2D8 damage, +0x2DC efficiency (floats); levels +0x2E0..
constexpr unsigned char kLookupBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0x8B, 0x11,
                                          0x83, 0xEC, 0x24, 0x33, 0xC0, 0x81, 0xFA, 0xBA};

// FUN_005d29f0: __cdecl uint32 ItemNameId(const Guid*) — the text ID of the name the UI shows (store data +0x134,
// else +0x298 of FUN_0043fe80's data). Used by the weapon-name formatter FUN_005d9030.
constexpr uintptr_t kItemNameVa = 0x005D29F0;
constexpr unsigned char kItemNameBytes[] = {0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x7D, 0x08,
                                            0x57, 0x33, 0xF6, 0xE8, 0x10, 0x7D, 0xF3, 0xFF};
// FUN_00b6a250: __thiscall const char* LookupText(uint32 id), this = *(0x011D1A00) — binary search of the loaded
// string tables; 0 if the ID is unknown. Read-only.
constexpr uintptr_t kLookupTextVa = 0x00B6A250;
constexpr unsigned char kLookupTextBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x8B, 0x51,
                                              0x34, 0x53, 0x8B, 0x5D, 0x08, 0x33, 0xC0, 0x56};
constexpr uintptr_t kStringTablesPtrVa = 0x011D1A00;

using NameToGuidFn = void(__fastcall*)(Guid* out, void* edx, const char* name);
using ItemNameFn = uint32_t(__cdecl*)(const Guid* guid);
using LookupTextFn = const char*(__fastcall*)(void* tables, void* edx, uint32_t id);
using LookupFn = const uint8_t*(__cdecl*)(const Guid* guid);

struct StockPack {
    const char* label;
    const char* asset;
};
constexpr StockPack kStockPacks[] = {
    {"Resource (type 2)", "XNOONAN023_CDACC95_E8C57D6_v3"},
    {"Ultra (types 1, 3)", "XNOONAN023_CDACCA8_0CE7AA2_v3"},
    {"Epic (type 4)", "XNOONAN023_CDACCA8_6573F26_v3"},
};

bool IsNullGuid(const Guid& g)
{
    bool zero = !g.d[0] && !g.d[1] && !g.d[2] && !g.d[3];
    bool bad = g.d[0] == 0xBADBADBA && g.d[1] == 0xBEEFBEEF && g.d[2] == 0xEAC15A55 && g.d[3] == 0x91100911;
    return zero || bad;
}

const uint8_t* SafeLookup(uintptr_t fn, const Guid& g)
{
    if (IsNullGuid(g)) return nullptr;
    __try {
        return reinterpret_cast<LookupFn>(fn)(&g);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("    lookup %08X faulted", static_cast<unsigned>(fn));
        return nullptr;
    }
}

bool SafeReadGuid(const uint8_t* p, Guid* out)
{
    __try {
        std::memcpy(out, p, sizeof(*out));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool g_itemNameOk = false;

// The game's text for a string ID into buf ("" if unknown). Only called once the bytes have been checked.
void LookupText(uint32_t id, char* buf, size_t size)
{
    buf[0] = '\0';
    uint32_t tables = 0;
    if (!id || !g_itemNameOk || !SafeReadU32(kStringTablesPtrVa, &tables) || !tables) return;
    const char* text = nullptr;
    __try {
        text = reinterpret_cast<LookupTextFn>(kLookupTextVa)(reinterpret_cast<void*>(tables), nullptr, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(buf, size, "(fault)");
        return;
    }
    if (text && !SafeCopyString(text, buf, size)) strcpy_s(buf, size, "(unreadable)");
}

// The item's UI name: its text ID and the text ("" if none).
uint32_t ItemName(const Guid& g, char* buf, size_t size)
{
    buf[0] = '\0';
    if (!g_itemNameOk || IsNullGuid(g)) return 0;
    uint32_t id = 0;
    __try {
        id = reinterpret_cast<ItemNameFn>(kItemNameVa)(&g);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(buf, size, "(fault)");
        return 0;
    }
    LookupText(id, buf, size);
    return id;
}

// Logs up to a few readable strings that the object's dwords point at (names, localisation keys), to tell parts apart.
void LogStrings(const char* indent, const uint8_t* obj, uintptr_t from, uintptr_t to)
{
    int found = 0;
    for (uintptr_t off = from; off < to && found < 6; off += 4) {
        uint32_t p = 0;
        if (!SafeReadU32(reinterpret_cast<uintptr_t>(obj) + off, &p) || p < 0x10000) continue;
        char s[64];
        if (!SafeCopyString(reinterpret_cast<const char*>(p), s, sizeof(s))) continue;
        size_t len = std::strlen(s);
        if (len < 4 || std::strchr(s, '?')) continue;
        Log("%s+%03X \"%s\"", indent, static_cast<unsigned>(off), s);
        ++found;
    }
}

// Checks the bytes of every game function the pack code calls. Also decides whether names can be looked up.
bool PackFunctionsOk(const char* who)
{
    if (!BytesMatch(kNameToGuidVa, kNameToGuidBytes, sizeof(kNameToGuidBytes))) {
        Log("%s: NameToGuid bytes do not match; skipped", who);
        return false;
    }
    constexpr uintptr_t kLookups[] = {kPackItemVa, kPackStoreVa, kPackRollVa, kPartTableVa, kPartItemVa, kCircuitVa};
    for (uintptr_t fn : kLookups) {
        if (!BytesMatch(fn, kLookupBytes, sizeof(kLookupBytes))) {
            Log("%s: lookup %08X bytes do not match; skipped", who, static_cast<unsigned>(fn));
            return false;
        }
    }
    g_itemNameOk = BytesMatch(kItemNameVa, kItemNameBytes, sizeof(kItemNameBytes)) &&
                   BytesMatch(kLookupTextVa, kLookupTextBytes, sizeof(kLookupTextBytes)) &&
                   BytesMatch(0x0043FE80, kLookupBytes, sizeof(kLookupBytes));
    return true;
}

bool PackGuid(const char* asset, Guid* out)
{
    __try {
        reinterpret_cast<NameToGuidFn>(kNameToGuidVa)(out, nullptr, asset);
        return !IsNullGuid(*out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float AsFloat(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Random number in [0, 1) for the part pick; xorshift seeded once from the performance counter.
float Random01()
{
    static uint32_t state = 0;
    if (!state) {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        state = static_cast<uint32_t>(qpc.QuadPart) | 1;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state >> 8) / 16777216.0f;
}

// Cut Resource Pack parts (resource_pack_parts=1). The Resource Pack asset has four part tables but a part count of 0.
// After a Resource Pack purchase this picks one part from those tables by the asset's own weights (only tables and
// parts the game has loaded) and writes it into slot 2 of the pending roll; slot 1 keeps the 0x0E Resource Pack
// marker so FUN_00519fb0 still applies the Resource Pack's resources and resolves the part from its tables.
// The pending roll is only applied to this save; seal purchases are recorded as "FFFFFF" with no contents.
// Only the 13 standard base-game parts below can be picked: a table entry that is not on this list (DLC or anything
// else) is never given. These are the part entries in the Resource Pack's own tables (read in game 2026-11-14;
// names from the game's text): table 0 attachments, table 1 tips, table 3 tools. Table 2 holds circuits, which are
// not part entries and are skipped anyway.
struct CutPart {
    const char* name;
    Guid entry;
};
constexpr CutPart kCutParts[] = {
    {"Full Zoom Scope", {{0x48CD4FC5, 0x14912703, 0x4D314E4F, 0x4B554F42}}},
    {"Electric Charge", {{0x48CD4A6D, 0x51D9C8AF, 0x4D314E4F, 0x4B554F42}}},
    {"Acid Bath", {{0x48CD4A6C, 0xA79CFD9E, 0x4D314E4F, 0x4B554F42}}},
    {"Damage Support", {{0x48CD4A6D, 0x33BDCB1E, 0x4D314E4F, 0x4B554F42}}},
    {"Stasis Coating", {{0x48CD4A6D, 0xB6B922E2, 0x4D314E4F, 0x4B554F42}}},
    {"Medic Support", {{0x48CD4FC5, 0xEB652489, 0x4D314E4F, 0x4B554F42}}},
    {"Repeater", {{0x33CCBB5C, 0x097CB036, 0x4E4F4F4E, 0x32304E41}}},
    {"Rail Accelerator", {{0x48CD1D8D, 0x0610CCF1, 0x4D314E4F, 0x4B554F42}}},
    {"Directed Ejection Field", {{0x33CCB9D1, 0x2A655FDB, 0x4E4F4F4E, 0x32304E41}}},
    {"Rip Core", {{0x48CD1D8F, 0xA2CA8600, 0x4D314E4F, 0x4B554F42}}},
    {"Pneumatic Torch", {{0x48CD1D8F, 0x8F44C532, 0x4D314E4F, 0x4B554F42}}},
    {"Telemetry Spike", {{0x48CD1D8F, 0x3F9D53BA, 0x4D314E4F, 0x4B554F42}}},
    {"Hydraulic Engine", {{0x33CCBB5C, 0x296A8C06, 0x4E4F4F4E, 0x32304E41}}},
};

bool IsCutPart(const Guid& entry)
{
    for (const CutPart& p : kCutParts)
        if (std::memcmp(&p.entry, &entry, sizeof(entry)) == 0) return true;
    return false;
}

void AddCutPart()
{
    auto* rolled = reinterpret_cast<uint8_t*>(kRolledItemsVa);
    if (rolled[0] || rolled[1] || rolled[2] || rolled[3] != 0x0E || rolled[4] != 0x0F || rolled[5] != 0x0F) {
        Log("cut part: roll is not a plain Resource Pack; left as is");
        return;
    }
    static int checked = -1;
    if (checked < 0) checked = PackFunctionsOk("cut part") ? 1 : 0;
    Guid pack{};
    const uint8_t* item = nullptr;
    if (!checked || !PackGuid(kStockPacks[0].asset, &pack) || !(item = SafeLookup(kPackItemVa, pack))) {
        Log("cut part: Resource Pack asset unavailable; resources only");
        return;
    }

    // Two passes over the same candidates: sum the weights, then walk to the drawn one.
    float total = 0.0f, target = -1.0f;
    for (int pass = 0; pass < 2; ++pass) {
        float run = 0.0f;
        for (uint8_t t = 0; t < 4; ++t) {
            Guid tg{};
            uint32_t tw = 0;
            const uint8_t* ref = item + 0x300 + t * 0x14;
            if (!SafeReadGuid(ref, &tg) || !SafeReadU32(reinterpret_cast<uintptr_t>(ref) + 0x10, &tw)) continue;
            const uint8_t* table = SafeLookup(kPartTableVa, tg);
            if (!table || AsFloat(tw) <= 0.0f) continue;
            // Within a table: share the table's weight by entry weight.
            float tableSum = 0.0f;
            for (uint8_t e = 0; e < 16; ++e) {
                Guid pg{};
                uint32_t ew = 0;
                const uint8_t* entry = table + 0x30 + e * 0x14;
                if (!SafeReadGuid(entry, &pg) || !SafeReadU32(reinterpret_cast<uintptr_t>(entry) + 0x10, &ew)) continue;
                if (IsCutPart(pg) && SafeLookup(kPartItemVa, pg) && AsFloat(ew) > 0.0f) tableSum += AsFloat(ew);
            }
            for (uint8_t e = 0; e < 16 && tableSum > 0.0f; ++e) {
                Guid pg{};
                uint32_t ew = 0;
                const uint8_t* entry = table + 0x30 + e * 0x14;
                if (!SafeReadGuid(entry, &pg) || !SafeReadU32(reinterpret_cast<uintptr_t>(entry) + 0x10, &ew)) continue;
                if (!IsCutPart(pg) || !SafeLookup(kPartItemVa, pg) || AsFloat(ew) <= 0.0f) continue;
                run += AsFloat(tw) * AsFloat(ew) / tableSum;
                if (pass == 1 && run > target) {
                    char name[96];
                    ItemName(pg, name, sizeof(name));
                    rolled[1] = e;
                    rolled[4] = t;
                    Log("cut part: added table %u entry %X \"%s\" (chance %.1f%%)", t, e, name,
                        100.0f * AsFloat(tw) * AsFloat(ew) / tableSum / total);
                    return;
                }
            }
        }
        total = run;
        if (total <= 0.0f) {
            Log("cut part: none of the 13 listed parts loaded in the Resource Pack tables; resources only");
            return;
        }
        target = Random01() * total;
    }
}

// Pack-contents popup FUN_0051a0c0: for a Resource Pack (record slot type 0x0E) it lists the resources, sets the title
// and shows the popup without ever listing parts. This hook sits on that branch at 0x51A1F9, just before the show
// call (mov edx,[esi]; mov eax,[edx+4Ch]; mov ecx,esi; call eax), with ESI = popup and EBX = the 6-byte record.
constexpr uintptr_t kResourcePopupShowVa = 0x0051A1F9;
constexpr unsigned char kResourcePopupShowBytes[] = {0x8B, 0x16, 0x8B, 0x42, 0x4C, 0x8B, 0xCE, 0xFF, 0xD0,
                                                     0x5F, 0x5E, 0x5B};
// FUN_00513d10: __stdcall bool PartFromRecord(record, slot, const Guid* pack, Guid* out) — the part's table entry.
constexpr uintptr_t kPartFromRecordVa = 0x00513D10;
constexpr unsigned char kPartFromRecordBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10, 0x83, 0xEC,
                                                  0x10, 0x56, 0x8B, 0x75, 0x14, 0x50, 0xC7, 0x46};
// FUN_00513ee0: __stdcall void AddPartRow(popup, const Guid* pack, Guid part (by value), int row, const char* label).
// The popup's own row builder for Ultra/Epic parts.
constexpr uintptr_t kAddPartRowVa = 0x00513EE0;
constexpr unsigned char kAddPartRowBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0x7D, 0x10, 0x00, 0x56,
                                              0x8B, 0x35, 0x00, 0x1A, 0x1D, 0x01, 0x75, 0x12};

using PartFromRecordFn = uint32_t(__stdcall*)(const uint8_t* record, unsigned slot, const Guid* pack, Guid* out);
using AddPartRowFn = void(__stdcall*)(void* popup, const Guid* pack, uint32_t g0, uint32_t g1, uint32_t g2,
                                      uint32_t g3, int row, const char* label);
void* g_resourcePopupResume = nullptr;

// Adds the cut part (slot 2 of a Resource Pack record, see AddCutPart) as the popup's first part row.
void __cdecl AddResourcePartRow(void* popup, const uint8_t* record)
{
    __try {
        if (!popup || !record || record[3] != 0x0E || record[4] >= 4 || record[1] >= 16) return;
        Guid pack{};
        if (!PackGuid(kStockPacks[0].asset, &pack)) return;
        Guid part = {{0xBADBADBA, 0xBEEFBEEF, 0xEAC15A55, 0x91100911}};
        if (!(reinterpret_cast<PartFromRecordFn>(kPartFromRecordVa)(record, 1, &pack, &part) & 0xFF)) return;
        reinterpret_cast<AddPartRowFn>(kAddPartRowVa)(popup, &pack, part.d[0], part.d[1], part.d[2], part.d[3], 0,
                                                      "$fe_mtx_random_part");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("cut part: popup row faulted");
    }
}

__declspec(naked) void HookResourcePopupStub()
{
    __asm {
        pushad
        pushfd
        push ebx // record
        push esi // popup
        call AddResourcePartRow
        add esp, 8
        popfd
        popad
        jmp dword ptr [g_resourcePopupResume]
    }
}

// ---- dump_circuits (debug, read-only): every circuit the game has loaded. ----
// The asset manager *(0x011D1908) keeps loaded assets in 4096 hash buckets at +0x80 (FUN_00a6f780). Entry: GUID at
// +0x0C, its hash ((g0*33 + g1)*33 + g2)*33 + g3 at +0x1C, next entry at +0x34.
constexpr uintptr_t kAssetManagerPtrVa = 0x011D1908;
constexpr int kAssetBuckets = 4096;

Guid g_seenCircuits[1024];
int g_seenCircuitCount = 0;

bool SeenCircuit(const Guid& g)
{
    for (int i = 0; i < g_seenCircuitCount; ++i) {
        if (std::memcmp(&g_seenCircuits[i], &g, sizeof(g)) == 0) return true;
    }
    if (g_seenCircuitCount < 1024) g_seenCircuits[g_seenCircuitCount++] = g;
    return false;
}

// Level as the UI shows it: the level field, minus for a penalty. Rate of fire (index 1) is stored negative for a
// bonus (shorter time between shots); the others are stored positive for a bonus.
int ShownLevel(int stat, uint32_t bonusBits, uint32_t level)
{
    float bonus = AsFloat(bonusBits);
    bool penalty = stat == 1 ? bonus > 0.0f : bonus < 0.0f;
    return penalty ? -static_cast<int>(level) : static_cast<int>(level);
}

void DumpCircuits()
{
    static int checked = -1;
    if (checked < 0) checked = PackFunctionsOk("circuit dump") ? 1 : 0;
    uint32_t manager = 0;
    if (!checked || !SafeReadU32(kAssetManagerPtrVa, &manager) || !manager) return;

    int assets = 0, circuits = 0, added = 0;
    for (int b = 0; b < kAssetBuckets; ++b) {
        uint32_t entry = 0;
        if (!SafeReadU32(manager + 0x80 + b * 4, &entry)) return;
        for (int depth = 0; entry && depth < 256; ++depth) {
            Guid g{};
            uint32_t hash = 0, next = 0;
            if (!SafeReadGuid(reinterpret_cast<const uint8_t*>(entry) + 0x0C, &g) || !SafeReadU32(entry + 0x1C, &hash) ||
                !SafeReadU32(entry + 0x34, &next))
                break;
            ++assets;
            uint32_t expect = ((g.d[0] * 33 + g.d[1]) * 33 + g.d[2]) * 33 + g.d[3];
            const uint8_t* circuit = expect == hash ? SafeLookup(kCircuitVa, g) : nullptr;
            if (circuit) {
                ++circuits;
                if (!SeenCircuit(g)) {
                    ++added;
                    uint32_t f[4] = {}, lv[4] = {};
                    for (int i = 0; i < 4; ++i) {
                        SafeReadU32(reinterpret_cast<uintptr_t>(circuit) + 0x2D0 + i * 4, &f[i]);
                        SafeReadU32(reinterpret_cast<uintptr_t>(circuit) + 0x2E0 + i * 4, &lv[i]);
                    }
                    char name[96];
                    ItemName(g, name, sizeof(name));
                    Log("  circuit %08X-%08X-%08X-%08X \"%s\": reload %+d, rate of fire %+d, damage %+d, clip %+d "
                        "(%.4f %.4f %.4f %.4f)",
                        g.d[0], g.d[1], g.d[2], g.d[3], name, ShownLevel(0, f[0], lv[0]), ShownLevel(1, f[1], lv[1]),
                        ShownLevel(2, f[2], lv[2]), ShownLevel(3, f[3], lv[3]), AsFloat(f[0]), AsFloat(f[1]), AsFloat(f[2]),
                        AsFloat(f[3]));
                }
            }
            entry = next;
        }
    }
    Log("circuit dump: %d assets loaded, %d circuits, %d new this time", assets, circuits, added);
}

void DumpPacks()
{
    if (!PackFunctionsOk("pack dump")) return;
    Log("===== pack dump (debug, read-only)%s =====", g_itemNameOk ? "" : " - item names unavailable");
    for (const StockPack& pack : kStockPacks) {
        Guid g{};
        if (!PackGuid(pack.asset, &g)) {
            Log("pack %s: no asset GUID", pack.label);
            continue;
        }
        Log("pack %s %s guid=%08X-%08X-%08X-%08X", pack.label, pack.asset, g.d[0], g.d[1], g.d[2], g.d[3]);

        uint32_t v = 0, w = 0;
        if (const uint8_t* store = SafeLookup(kPackStoreVa, g)) {
            if (SafeReadU32(reinterpret_cast<uintptr_t>(store) + 0x14C, &v)) Log("  seal price %u", v);
        }
        if (const uint8_t* roll = SafeLookup(kPackRollVa, g)) {
            if (SafeReadU32(reinterpret_cast<uintptr_t>(roll) + 0x350, &v) &&
                SafeReadU32(reinterpret_cast<uintptr_t>(roll) + 0x360, &w)) {
                float chance;
                std::memcpy(&chance, &w, sizeof(chance));
                Log("  parts: %u, +1 with chance %.3f (max 3)", v, chance);
            }
        }
        const uint8_t* item = SafeLookup(kPackItemVa, g);
        if (!item) {
            Log("  pack item asset not found");
            continue;
        }
        for (int r = 0; r < 6; ++r) {
            uintptr_t at = reinterpret_cast<uintptr_t>(item) + 0x2D0 + r * 8;
            if (SafeReadU32(at, &v) && SafeReadU32(at + 4, &w))
                Log("  resource %d: %u-%u, grants %u", r, v, w, (v + w) / 2);
        }
        for (int t = 0; t < 4; ++t) {
            Guid tg{};
            uint32_t tail = 0;
            const uint8_t* ref = item + 0x300 + t * 0x14;
            if (!SafeReadGuid(ref, &tg)) continue;
            SafeReadU32(reinterpret_cast<uintptr_t>(ref) + 0x10, &tail);
            if (IsNullGuid(tg)) {
                Log("  part table %d: none", t);
                continue;
            }
            const uint8_t* table = SafeLookup(kPartTableVa, tg);
            Log("  part table %d: %08X-%08X-%08X-%08X tail=%08X%s", t, tg.d[0], tg.d[1], tg.d[2], tg.d[3], tail,
                table ? "" : " (not loaded)");
            if (!table) continue;
            LogStrings("    table ", table, 0, 0x30);
            for (int e = 0; e < 16; ++e) {
                Guid pg{};
                uint32_t extra = 0;
                const uint8_t* entry = table + 0x30 + e * 0x14;
                if (!SafeReadGuid(entry, &pg) || IsNullGuid(pg)) continue;
                SafeReadU32(reinterpret_cast<uintptr_t>(entry) + 0x10, &extra);
                const uint8_t* part = SafeLookup(kPartItemVa, pg);
                Guid sub{};
                uint32_t g0 = 0, g1 = 0, n0 = 0, n1 = 0;
                if (part) {
                    SafeReadGuid(part + 0x2D0, &sub);
                    SafeReadU32(reinterpret_cast<uintptr_t>(part) + 0x2F0, &g0);
                    SafeReadU32(reinterpret_cast<uintptr_t>(part) + 0x2F4, &g1);
                    SafeReadU32(reinterpret_cast<uintptr_t>(part) + 0x2F8, &n0);
                    SafeReadU32(reinterpret_cast<uintptr_t>(part) + 0x2FC, &n1);
                }
                Log("    [%X/%X] %08X-%08X-%08X-%08X w=%08X item=%08X-%08X-%08X-%08X grants=%08X:%u %08X:%u%s", t, e,
                    pg.d[0], pg.d[1], pg.d[2], pg.d[3], extra, sub.d[0], sub.d[1], sub.d[2], sub.d[3], g0, n0, g1, n1,
                    part ? "" : " (not a part entry)");
                // Circuits are their own class; FUN_00513dd0 spawns such an entry directly.
                if (const uint8_t* circuit = SafeLookup(kCircuitVa, pg)) {
                    uint32_t f[4] = {}, lv[4] = {};
                    for (int i = 0; i < 4; ++i) {
                        SafeReadU32(reinterpret_cast<uintptr_t>(circuit) + 0x2D0 + i * 4, &f[i]);
                        SafeReadU32(reinterpret_cast<uintptr_t>(circuit) + 0x2E0 + i * 4, &lv[i]);
                    }
                    Log("      circuit: reload %+.3f  rate of fire %+.3f  damage %+.3f  efficiency %+.3f  "
                        "levels %d/%d/%d/%d",
                        AsFloat(f[0]), AsFloat(f[1]), AsFloat(f[2]), AsFloat(f[3]), static_cast<int>(lv[0]),
                        static_cast<int>(lv[1]), static_cast<int>(lv[2]), static_cast<int>(lv[3]));
                }
                // Names from the entry, the spawned item and the inventory hashes; whichever the game knows.
                char entryName[96], itemName[96], g0Text[96], g1Text[96];
                uint32_t entryId = ItemName(pg, entryName, sizeof(entryName));
                uint32_t itemId = ItemName(sub, itemName, sizeof(itemName));
                LookupText(g0, g0Text, sizeof(g0Text));
                LookupText(g1, g1Text, sizeof(g1Text));
                Log("      names: entry %08X \"%s\" | item %08X \"%s\" | hash0 \"%s\" | hash1 \"%s\"", entryId, entryName,
                    itemId, itemName, g0Text, g1Text);
            }
        }
    }
    Log("===== end of pack dump =====");
}

// ---- Circuit packs. ----
// Tiers: Standard, Advanced, Elite, and Masterwork (the six +3/+3 Elites: the Masterwork Circuit Pack and Schematic).
// Equal odds for every stat combination in a tier (where the game has two assets with the same stats, one of them
// stands for both).
// - Upgrade Circuit Pack: Standard 50% / Advanced 40% / Elite 10%; the 5th pack in a row without an Elite gives one.
// - Elite Upgrade Circuit: one Elite, outside the counts.
// - Masterwork Circuit Pack: Elite 85% / Masterwork 15%; the 5th pack in a row without a Masterwork gives one.
// The counts live in ds3_seals_state.ini, not in the save.

struct CircuitDef {
    uint32_t d0, d1;                    // asset ID; the last two dwords are the main set's 4D314E4F-4B554F42
    int8_t reload, rate, damage, clip;  // levels as the UI shows them, minus = penalty
};
constexpr uint32_t kCircuitSetD2 = 0x4D314E4F;
constexpr uint32_t kCircuitSetD3 = 0x4B554F42;

// From sdk/CIRCUITS.md. Each is checked against the loaded asset (class and levels) before it can be picked.
constexpr CircuitDef kStandardCircuits[] = {
    {0x48CD1CD1, 0xF835E27B, 0, 0, 0, 1}, {0x48CD1CD3, 0x024629FB, 0, 0, 1, 0}, {0x48CD1CD3, 0x3E7177D4, 0, 0, 1, 1},
    {0x48CD1CD3, 0x36192D35, 0, 1, 1, 0}, {0x48CD1CD3, 0x11FA3052, 1, 0, 1, 0}, {0x48CD1CD3, 0x1B579590, 0, 1, 0, 0},
    {0x48CD1CD3, 0x0D0F5D89, 0, 1, 0, 1}, {0x48CD1CD3, 0x4304645D, 1, 1, 0, 0}, {0x48CD1CD3, 0x07827A3A, 1, 0, 0, 0},
    {0x48CD1CD3, 0x4913E884, 1, 0, 0, 1},
};
constexpr CircuitDef kAdvancedCircuits[] = {
    {0x48CD1CD3, 0xA6F25382, 0, 0, 0, 2},  {0x48CD1CD3, 0xCAED25A8, 0, 2, 1, 0},  {0x48CD1CD3, 0xBAB67E79, 2, 0, 1, 0},
    {0x48CD1CD3, 0x917151F8, 0, 0, 2, 0},  {0x48CD1CD3, 0xB486EE60, 0, 0, 2, 1},  {0x48CD1CD4, 0x04B4D159, 0, 0, 2, 2},
    {0x48CD1CD3, 0x66B6E109, 0, 0, 2, -1}, {0x48CD1CD3, 0xB01916FA, 0, 1, 2, 0},  {0x48CD1CD3, 0xFF82455A, 0, 2, 2, 0},
    {0x48CD1CD3, 0x6186C61A, 0, -1, 2, 0}, {0x48CD1CD3, 0xABCF9162, 1, 0, 2, 0},  {0x48CD1CD3, 0xF9E7F433, 2, 0, 2, 0},
    {0x48CD1CD3, 0x5CAC476C, -1, 0, 2, 0}, {0x48CD1CD3, 0x7E7ED43F, 0, 0, -1, 2}, {0x48CD1CD3, 0x70B93D10, 0, 2, -1, 0},
    {0x48CD1CD3, 0x6BE240E9, 2, 0, -1, 0}, {0x48CD1CD3, 0xD9824727, 0, 1, 0, 2},  {0x48CD1CD3, 0xBFFFC75D, 2, 1, 0, 0},
    {0x48CD1CD3, 0xA27846CC, 0, 2, 0, 0},  {0x48CD1CD4, 0x12FABF61, 0, 2, 0, 2},  {0x48CD1CD3, 0x7A43A6E5, 0, 2, 0, -1},
    {0x48CD1CD3, 0xD00264E7, 1, 2, 0, 0},  {0x48CD1CD4, 0x0AA9C7F2, 2, 2, 0, 0},  {0x48CD1CD3, 0x75A4D32E, -1, 2, 0, 0},
    {0x48CD1CD3, 0x8D0EC57C, 0, -1, 0, 2}, {0x48CD1CD3, 0x836476C6, 2, -1, 0, 0}, {0x48CD1CD3, 0xD4AC3566, 1, 0, 0, 2},
    {0x48CD1CD3, 0x95F6A998, 2, 0, 0, 0},  {0x48CD1CD3, 0xC5C2FE50, 2, 0, 0, 1},  {0x48CD1CD4, 0x0E9A07B1, 2, 0, 0, 2},
    {0x48CD1CD3, 0x9CE1782C, 2, 0, 0, -1}, {0x48CD1CD3, 0x87B68816, -1, 0, 0, 2},
};
constexpr CircuitDef kEliteCircuits[] = {
    {0x48CD1CD2, 0xAD462CFB, 0, 0, 0, 3}, {0x48CD1CD2, 0xE4F412A8, 0, 0, 1, 3}, {0x48CD1CD2, 0xD50FFA66, 0, 3, 1, 0},
    {0x48CD1CD2, 0xC41031B1, 3, 0, 1, 0}, {0x48CD1CD4, 0x40FE0836, 0, 0, 2, 3}, {0x48CD1CD4, 0x3235A067, 0, 3, 2, 0},
    {0x48CD1CD4, 0x2226F49E, 3, 0, 2, 0}, {0x48CD1CD2, 0x381D666E, 0, 0, 3, 0}, {0x48CD1CD2, 0xBF688420, 0, 0, 3, 1},
    {0x48CD1CD4, 0x585F47BB, 0, 0, 3, 3}, {0x48CD1CD2, 0xBA6BFDA3, 0, 1, 3, 0}, {0x48CD1CD2, 0xFBBF9E08, 0, 2, 3, 0},
    {0x48CD1CD4, 0x538A9593, 0, 3, 3, 0}, {0x48CD1CD2, 0xB3B4002C, 1, 0, 3, 0}, {0x48CD1CD2, 0xF6C93233, 2, 0, 3, 0},
    {0x48CD1CD4, 0x4DA4455A, 3, 0, 3, 0}, {0x48CD1CD2, 0xF0CAC57C, 0, 1, 0, 3}, {0x48CD1CD2, 0xCAE1D250, 3, 1, 0, 0},
    {0x48CD1CD4, 0x49141803, 0, 2, 0, 3}, {0x48CD1CD4, 0x277F8026, 3, 2, 0, 0}, {0x48CD1CD2, 0x4E575C74, 0, 3, 0, 0},
    {0x48CD1CD2, 0xDF24A665, 0, 3, 0, 1}, {0x48CD1CD4, 0x3CBFA677, 0, 3, 0, 2}, {0x48CD1CD4, 0x675E2A3F, 0, 3, 0, 3},
    {0x48CD1CD2, 0xD9EAC736, 1, 3, 0, 0}, {0x48CD1CD4, 0x382CB9EE, 2, 3, 0, 0}, {0x48CD1CD4, 0x5D425221, 3, 3, 0, 0},
    {0x48CD1CD2, 0xEAED4F1D, 1, 0, 0, 3}, {0x48CD1CD4, 0x44F57414, 2, 0, 0, 3}, {0x48CD1CD2, 0x40A04494, 3, 0, 0, 0},
    {0x48CD1CD2, 0xCFEFBE5F, 3, 0, 0, 1}, {0x48CD1CD4, 0x2D1D7AE5, 3, 0, 0, 2}, {0x48CD1CD4, 0x61DC91DA, 3, 0, 0, 3},
};

enum class TopFilter { Any, NoTop, TopOnly };  // +3/+3 circuits: allowed, left out, or the only ones

struct CircuitTier {
    const char* name;
    const CircuitDef* list;
    int count;
    TopFilter filter;
};
constexpr int kEliteListCount = sizeof(kEliteCircuits) / sizeof(CircuitDef);
constexpr CircuitTier kStandardTier = {"Standard", kStandardCircuits, sizeof(kStandardCircuits) / sizeof(CircuitDef),
                                       TopFilter::Any};
constexpr CircuitTier kAdvancedTier = {"Advanced", kAdvancedCircuits, sizeof(kAdvancedCircuits) / sizeof(CircuitDef),
                                       TopFilter::Any};
constexpr CircuitTier kEliteTier = {"Elite", kEliteCircuits, kEliteListCount, TopFilter::NoTop};
constexpr CircuitTier kMasterworkTier = {"Masterwork", kEliteCircuits, kEliteListCount, TopFilter::TopOnly};

constexpr int kElitePityPacks = 5;
constexpr int kMasterworkPityPacks = 5;
constexpr uint8_t kCircuitMarker = 0x0D;  // slot type 0 of a circuit pack's roll; the game never writes it

Guid CircuitGuid(const CircuitDef& c)
{
    return {{c.d0, c.d1, kCircuitSetD2, kCircuitSetD3}};
}

bool IsTop(const CircuitDef& c)
{
    return (c.reload == 3) + (c.rate == 3) + (c.damage == 3) + (c.clip == 3) >= 2;
}

bool InTier(const CircuitTier& tier, const CircuitDef& c)
{
    return tier.filter == TopFilter::Any || (tier.filter == TopFilter::TopOnly) == IsTop(c);
}

// Loaded, of the circuit class, and with exactly the expected levels.
bool CircuitLoaded(const CircuitDef& c)
{
    const uint8_t* circuit = SafeLookup(kCircuitVa, CircuitGuid(c));
    if (!circuit) return false;
    const int expect[4] = {c.reload, c.rate, c.damage, c.clip};
    for (int i = 0; i < 4; ++i) {
        uint32_t bonus = 0, level = 0;
        if (!SafeReadU32(reinterpret_cast<uintptr_t>(circuit) + 0x2D0 + i * 4, &bonus) ||
            !SafeReadU32(reinterpret_cast<uintptr_t>(circuit) + 0x2E0 + i * 4, &level) ||
            ShownLevel(i, bonus, level) != expect[i])
            return false;
    }
    return true;
}

// The save's discovered list at player +0xFBC (FUN_005b0b40): entries at +0x04, count at +0x08, 0x9DC bytes each,
// starting with the asset GUID. The bench only shows circuits on it (every circuit given that was not on
// it vanished, on both test saves). A world pickup adds to the inventory and then here (FUN_00469ab0 calls
// FUN_0059ef70(this = player + 0xFBC, &guid, 1, 0)); a store grant (FUN_00513dd0) only adds to the inventory, so the
// packs only give circuits already on it. Read-only: 1 = listed, 0 = not, -1 = the list could not be read.
constexpr uintptr_t kDiscoveredOffset = 0xFBC;
constexpr uint32_t kDiscoveredStride = 0x9DC;

int OnDiscoveredList(const uint8_t* player, const Guid& want)
{
    uintptr_t list = reinterpret_cast<uintptr_t>(player) + kDiscoveredOffset;
    uint32_t entries = 0, count = 0;
    if (!player || !SafeReadU32(list + 4, &entries) || !SafeReadU32(list + 8, &count) || count > 100000 ||
        (count && !entries))
        return -1;
    for (uint32_t i = 0; i < count; ++i) {
        Guid g{};
        if (!SafeReadGuid(reinterpret_cast<const uint8_t*>(entries + i * kDiscoveredStride), &g)) return -1;
        if (std::memcmp(&g, &want, sizeof(g)) == 0) return 1;
    }
    return 0;
}

int KnownToSave(const uint8_t* player, const CircuitDef& c)
{
    return OnDiscoveredList(player, CircuitGuid(c));
}

// Byte +0x10 of a discovered record is the circuit's "new" icon (hovering one cleared only that byte).
// FUN_0059ef70 sets it when it first creates the record, so a circuit already discovered (all the packs give) would
// arrive without one. Sets it for a circuit just delivered.
// The bench main menu's "new" badges are bits of the player flags (+0x594): 0x80000 for circuits (FUN_005b0b40, circuit
// class 0x011D9FA0) and 0x20000 for weapon parts (FUN_005b0b40 and FUN_004ce0e0). The game sets them alongside the
// item's own flag; the mod does the same.
constexpr uint32_t kNewCircuitsBadge = 0x80000;
constexpr uint32_t kNewPartsBadge = 0x20000;

void SetBenchBadge(uintptr_t player, uint32_t bit)
{
    if (!player) return;
    __try {
        *reinterpret_cast<uint32_t*>(player + 0x594) |= bit;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Sets the "new" byte of the item's discovered record and the given bench badge. False if the item is not listed.
bool MarkRecordNew(const uint8_t* player, const Guid& want, uint32_t badge)
{
    uintptr_t list = reinterpret_cast<uintptr_t>(player) + kDiscoveredOffset;
    uint32_t entries = 0, count = 0;
    if (!player || !SafeReadU32(list + 4, &entries) || !SafeReadU32(list + 8, &count) || count > 100000) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* rec = reinterpret_cast<uint8_t*>(entries + i * kDiscoveredStride);
        Guid g{};
        if (!SafeReadGuid(rec, &g)) return false;
        if (std::memcmp(&g, &want, sizeof(g)) != 0) continue;
        __try {
            rec[0x10] = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        SetBenchBadge(reinterpret_cast<uintptr_t>(player), badge);
        return true;
    }
    return false;
}

bool MarkCircuitNew(const uint8_t* player, const CircuitDef& c)
{
    return MarkRecordNew(player, CircuitGuid(c), kNewCircuitsBadge);
}

// Frames and attachments are items too (part entry +0x2D0), recorded on the discovered list like circuits, and the
// bench reads and clears their "new" icon from the same byte (FUN_005f2b60
// / FUN_005f2a90, element kind 3 -> FUN_00590f20 IsNew / FUN_00590f80 ClearNew, this = player + 0xFBC). FUN_005b0b40
// sets the byte and badge 0x20000 for their two item classes only when it creates the record, so a frame or attachment
// the save already has (a repeat from a pack) arrives with neither. GUID lookups of those two classes, same prologue as
// the other lookups: FUN_0043fc50 (class 0x01221E98) and FUN_0043fd30 (class 0x0120BFC0).
constexpr uintptr_t kPartItemLookups[] = {0x0043FC50, 0x0043FD30};
bool g_partItemLookupsOk = false;

bool IsPartItem(const Guid& g)
{
    if (!g_partItemLookupsOk) return false;
    for (uintptr_t fn : kPartItemLookups) {
        if (SafeLookup(fn, g)) return true;
    }
    return false;
}

// The player's item inventory (FUN_00705320, this = player + 0xF0C), where the store's grant puts frames and
// attachments: entries at +0xF10, count +0xF14, 0x28 bytes each, GUID first, stack count +0x14.
constexpr uintptr_t kInventoryEntriesOffset = 0xF10;
constexpr uint32_t kInventoryStride = 0x28;
constexpr uint32_t kInventoryCountField = 0x14;
constexpr uint32_t kMaxInventory = 512;

struct InventoryCounts {
    bool ok;
    uint32_t count;
    Guid guid[kMaxInventory];
    uint32_t n[kMaxInventory];
};

bool ReadInventory(uintptr_t player, InventoryCounts* out)
{
    out->ok = false;
    out->count = 0;
    uint32_t entries = 0, count = 0;
    if (!player || !SafeReadU32(player + kInventoryEntriesOffset, &entries) ||
        !SafeReadU32(player + kInventoryEntriesOffset + 4, &count) || count > kMaxInventory || (count && !entries))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t e = entries + i * kInventoryStride;
        if (!SafeReadGuid(reinterpret_cast<const uint8_t*>(e), &out->guid[i]) ||
            !SafeReadU32(e + kInventoryCountField, &out->n[i]))
            return false;
    }
    out->count = count;
    out->ok = true;
    return true;
}

// Marks new every frame or attachment whose stack in the inventory grew since `before`. Returns how many.
int MarkNewPartItems(uintptr_t player, const InventoryCounts& before)
{
    static InventoryCounts after;
    if (!before.ok || !g_partItemLookupsOk || !ReadInventory(player, &after)) return 0;
    int marked = 0;
    for (uint32_t i = 0; i < after.count; ++i) {
        uint32_t had = 0;
        for (uint32_t j = 0; j < before.count; ++j) {
            if (std::memcmp(&before.guid[j], &after.guid[i], sizeof(Guid)) == 0) {
                had = before.n[j];
                break;
            }
        }
        if (after.n[i] <= had || !IsPartItem(after.guid[i])) continue;
        char name[96];
        ItemName(after.guid[i], name, sizeof(name));
        if (MarkRecordNew(reinterpret_cast<const uint8_t*>(player), after.guid[i], kNewPartsBadge)) {
            ++marked;
            Log("  \"%s\" (%08X-%08X-%08X-%08X) marked new", name, after.guid[i].d[0], after.guid[i].d[1],
                after.guid[i].d[2], after.guid[i].d[3]);
        } else {
            Log("  \"%s\" is not on the discovered list; not marked", name);
        }
    }
    return marked;
}

// Advanced circuits the game also has under a second asset with the same stats (sdk/CIRCUITS.md; the Elites have
// none). A save that found the other asset already has these stats, so a Schematic does not unlock them.
struct CircuitTwin {
    uint32_t d0, d1;          // the listed circuit
    uint32_t twinD0, twinD1;  // the other asset
};
constexpr CircuitTwin kCircuitTwins[] = {
    {0x48CD1CD3, 0xBAB67E79, 0x48CD1CD3, 0xEC5973FD}, {0x48CD1CD3, 0xB486EE60, 0x48CD1CD3, 0xE715826C},
    {0x48CD1CD3, 0xB01916FA, 0x48CD1CD3, 0xE26B8ADC}, {0x48CD1CD3, 0xFF82455A, 0x48CD1CD4, 0x1D391481},
    {0x48CD1CD3, 0xABCF9162, 0x48CD1CD3, 0xDD7286E6}, {0x48CD1CD3, 0xF9E7F433, 0x48CD1CD4, 0x17CBEF02},
    {0x48CD1CD3, 0xBFFFC75D, 0x48CD1CD3, 0xF07B7362}, {0x48CD1CD3, 0xC5C2FE50, 0x48CD1CD3, 0xF4BF0563},
};

// Neither the circuit nor its twin is on the save's discovered list (false if the list could not be read).
bool Undiscovered(const uint8_t* player, const CircuitDef& c)
{
    if (KnownToSave(player, c) != 0) return false;
    for (const CircuitTwin& t : kCircuitTwins) {
        if (t.d0 == c.d0 && t.d1 == c.d1) return KnownToSave(player, {t.twinD0, t.twinD1, 0, 0, 0, 0}) == 0;
    }
    return true;
}

uint8_t* LocalPlayer();

// Loaded as expected, and discovered by this save (circuit packs) or not yet discovered (Schematics).
bool Eligible(const CircuitTier& tier, const CircuitDef& c, const uint8_t* player, bool undiscovered)
{
    return InTier(tier, c) && CircuitLoaded(c) &&
           (undiscovered ? Undiscovered(player, c) : KnownToSave(player, c) == 1);
}

int CountEligible(const CircuitTier& tier, const uint8_t* player, bool undiscovered = false)
{
    int n = 0;
    for (int i = 0; i < tier.count; ++i) n += Eligible(tier, tier.list[i], player, undiscovered);
    return n;
}

// Equal odds over the tier's eligible circuits for the local player. 0 if none.
const CircuitDef* PickFrom(const CircuitTier& tier, bool undiscovered = false)
{
    const uint8_t* player = LocalPlayer();
    int eligible = CountEligible(tier, player, undiscovered);
    if (!eligible) return nullptr;
    int target = static_cast<int>(Random01() * eligible);
    for (int i = 0; i < tier.count; ++i) {
        if (Eligible(tier, tier.list[i], player, undiscovered) && target-- == 0) return &tier.list[i];
    }
    return nullptr;
}

// Set once the Schematics' extra piece (recording a circuit as found) checks out too.
bool g_schematicsReady = false;

// How many circuits of each tier this save could get from the packs (discovered), and from the Schematics (not yet
// discovered); set at each store open and purchase.
struct CircuitAvailability {
    int standard, advanced, elite, masterwork;
    int newAdvanced, newElite, newMasterwork;
    int missingParts[kPartGroups];  // per part unlock: MK-V then MK-II, attachment / tip / tool / frame
};
CircuitAvailability g_available = {};

int CountMissingParts(int group, uintptr_t player);
bool g_partsReady = false;  // set once the part grant checks out too

void RefreshCircuitAvailability()
{
    const uint8_t* player = LocalPlayer();
    CircuitAvailability a = {CountEligible(kStandardTier, player),        CountEligible(kAdvancedTier, player),
                             CountEligible(kEliteTier, player),           CountEligible(kMasterworkTier, player),
                             CountEligible(kAdvancedTier, player, true),  CountEligible(kEliteTier, player, true),
                             CountEligible(kMasterworkTier, player, true), {}};
    for (int g = 0; g < kPartGroups && g_partsReady; ++g)
        a.missingParts[g] = CountMissingParts(g, reinterpret_cast<uintptr_t>(player));
    if (std::memcmp(&a, &g_available, sizeof(a)) != 0 || !player) {
        Log("circuits this save has discovered: Standard %d, Advanced %d, Elite %d, Masterwork %d; not yet: Advanced "
            "%d, Elite %d, Masterwork %d%s",
            a.standard, a.advanced, a.elite, a.masterwork, a.newAdvanced, a.newElite, a.newMasterwork,
            player ? "" : " (no player)");
        const int* m = a.missingParts;
        if (g_partsReady)
            Log("parts this save does not have: MK-V attachment %d, tip %d, tool %d, frame %d; MK-II attachment %d, "
                "tip %d, tool %d, frame %d",
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
    }
    g_available = a;
    Log("store open: player %p", player);
}

// A circuit offer is shown only when it has something to give: the Upgrade Circuit Pack needs a Standard (a tier not
// discovered yet falls back to the one below), the Elite Upgrade Circuit an Elite, the Masterwork pack both tiers, and
// a Schematic a circuit of its tier the save has not found.
bool ModOfferAvailable(const char* offerId)
{
    const CircuitAvailability& a = g_available;
    if (std::strcmp(offerId, kCircuitOfferId) == 0) return a.standard > 0;
    if (std::strcmp(offerId, kEliteCircuitOfferId) == 0) return a.elite > 0;
    if (std::strcmp(offerId, kMasterworkOfferId) == 0) return a.elite > 0 && a.masterwork > 0;
    const ModOffer* offer = ModOfferById(offerId);
    if (offer && offer->kind == OfferKind::Part)
        return g_partsReady && a.missingParts[offer->type - kFirstPartType] > 0;
    if (!g_schematicsReady) return false;
    if (std::strcmp(offerId, kAdvancedSchematicOfferId) == 0) return a.newAdvanced > 0;
    if (std::strcmp(offerId, kEliteSchematicOfferId) == 0) return a.newElite > 0;
    if (std::strcmp(offerId, kMasterworkSchematicOfferId) == 0) return a.newMasterwork > 0;
    return false;
}

// "Damage +3, Clip +3"
void CircuitText(const CircuitDef& c, char* buf, size_t size)
{
    const struct {
        const char* name;
        int level;
    } stats[] = {{"Damage", c.damage}, {"Rate of fire", c.rate}, {"Reload", c.reload}, {"Clip", c.clip}};
    buf[0] = '\0';
    size_t used = 0;
    for (const auto& s : stats) {
        if (!s.level) continue;
        int n = sprintf_s(buf + used, size - used, "%s%s %+d", used ? ", " : "", s.name, s.level);
        if (n > 0) used += static_cast<size_t>(n);
    }
}

// Upgrade Circuit Packs bought since the last Elite; Masterwork Circuit Packs bought since the last Masterwork.
struct Pity {
    int elite;
    int masterwork;
};
Pity g_pity = {};

int ElitePityPacks()
{
    return g_config.pityElitePacks > 0 ? g_config.pityElitePacks : kElitePityPacks;
}

int MasterworkPityPacks()
{
    return g_config.pityMasterworkPacks > 0 ? g_config.pityMasterworkPacks : kMasterworkPityPacks;
}

// A count read from the file, kept in 0..kMaxPityCount; anything at or above the threshold already means "guaranteed".
constexpr int kMaxPityCount = 1000;

int LoadCount(const wchar_t* key)
{
    int v = static_cast<int>(GetPrivateProfileIntW(L"pity", key, 0, g_config.statePath));
    return v < 0 ? 0 : v > kMaxPityCount ? kMaxPityCount : v;
}

void LoadPity()
{
    g_pity.elite = LoadCount(L"elite_packs");
    g_pity.masterwork = LoadCount(L"masterwork_packs");
    Log("pity counts: %d Upgrade Circuit Pack(s) since an Elite, %d Masterwork Circuit Pack(s) since a Masterwork",
        g_pity.elite, g_pity.masterwork);
}

// Both counts in one write. Replacing the whole section also drops an old test build's top_packs key.
void SavePity()
{
    wchar_t section[64];
    // The section format is key=value\0key=value\0\0: each '|' becomes a \0, and swprintf_s adds the last one.
    int n = swprintf_s(section, L"elite_packs=%d|masterwork_packs=%d|", g_pity.elite, g_pity.masterwork);
    for (int i = 0; i < n; ++i) {
        if (section[i] == L'|') section[i] = L'\0';
    }
    if (!WritePrivateProfileSectionW(L"pity", section, g_config.statePath))
        Log("pity counts could not be saved (error %lu); they last until the game closes", GetLastError());
}

int PacksLeft(int threshold, int count)
{
    return threshold - count < 1 ? 1 : threshold - count;
}

char g_circuitPackDescription[200] = {};
char g_masterworkPackDescription[200] = {};

const char* CircuitPackDescription()
{
    if (g_available.advanced == 0) {
        strcpy_s(g_circuitPackDescription, "Standard only. Better tiers join once you find them.");
        return g_circuitPackDescription;
    }
    if (g_available.elite == 0) {
        strcpy_s(g_circuitPackDescription, "Standard 50%, Advanced 50%. Elites join once you find one.");
        return g_circuitPackDescription;
    }
    int left = PacksLeft(ElitePityPacks(), g_pity.elite);
    sprintf_s(g_circuitPackDescription,
              "Standard 50%%, Advanced 40%%, Elite 10%%. Elite guaranteed within %d pack%s.",
              left, left == 1 ? "" : "s");
    return g_circuitPackDescription;
}

const char* MasterworkPackDescription()
{
    int left = PacksLeft(MasterworkPityPacks(), g_pity.masterwork);
    sprintf_s(g_masterworkPackDescription,
              "Elite 85%%, Masterwork (+3/+3) 15%%. Masterwork guaranteed within %d pack%s.",
              left, left == 1 ? "" : "s");
    return g_masterworkPackDescription;
}

// FUN_00436170: __cdecl Player* GetPlayer(int index). FUN_0051dd70 charges GetPlayer(0).
constexpr uintptr_t kGetPlayerVa = 0x00436170;
constexpr unsigned char kGetPlayerBytes[] = {0x55, 0x8B, 0xEC, 0xA1, 0x78, 0x3B, 0x1D, 0x01,
                                             0x57, 0x33, 0xFF, 0x85, 0xC0, 0x74, 0x0B, 0x8B};
// FUN_006ced30: __thiscall void AddResource(int index, uint32 amount), this = the player's resource block; saturates.
constexpr uintptr_t kAddResourceVa = 0x006CED30;
constexpr unsigned char kAddResourceBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x83, 0xF8,
                                               0x06, 0x73, 0x24, 0x8B, 0x55, 0x0C, 0x85, 0xD2};
// FUN_00705320: __thiscall uint32 GiveItem(const Guid* item, uint32 count), this = the player's inventory, ret 8.
// Returns how many were added: 0 when there is no room or no entry can be made (FUN_006f9eb0).
constexpr uintptr_t kGiveItemVa = 0x00705320;
constexpr unsigned char kGiveItemBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28, 0x8B, 0x45,
                                            0x08, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x08, 0x33};
// Player layout, as FUN_0051dd70 and FUN_0041e730 use it: the resource block is +0xEA4 when bit 13 of +0x594 is set,
// else +0xE3C; the inventory FUN_00513dd0 gives items to is +0xF0C. Seals are resource 5.
constexpr uintptr_t kPlayerFlagsOffset = 0x594;
constexpr uintptr_t kResourceBlockAltOffset = 0xEA4;
constexpr uintptr_t kResourceBlockOffset = 0xE3C;
constexpr uintptr_t kInventoryOffset = 0xF0C;
constexpr int kSealResource = 5;

using GetPlayerFn = uint8_t*(__cdecl*)(int index);
using AddResourceFn = void(__fastcall*)(void* block, void* edx, int index, uint32_t amount);
using GiveItemFn = uint32_t(__fastcall*)(void* inventory, void* edx, const Guid* item, uint32_t count);

enum class Delivery { Given, Refunded, Failed };

struct PartDef;

struct CircuitGrant {
    const CircuitDef* def;  // 0 = none (or a part, below)
    const CircuitTier* tier;
    const char* title;
    const char* label;
    Delivery delivery;
    const char* partName;  // a part unlock's part instead of a circuit
};
CircuitGrant g_shownCircuit = {};  // the last one delivered (or not), for the popup

// A circuit (or part) paid for and waiting for the game's apply step, the point where the game gives stock-pack items.
// Giving it inside the purchase did not last: the game reported it added, but it never showed in the inventory.
struct PendingCircuit {
    CircuitGrant pick;  // def = 0 and no part: nothing waiting
    int packType;
    unsigned price;
    int eliteNext, masterworkNext;  // the pity counts if this circuit is delivered and is not the guaranteed tier
    uintptr_t player;               // the player charged
    const PartDef* part;            // a part unlock's part
};
PendingCircuit g_pending = {};
const char* g_earlierRefund = nullptr;  // set when a purchase never applied was refunded; the next popup's title says so

bool Waiting(const PendingCircuit& p)
{
    return p.pick.def || p.part;
}

uint8_t* LocalPlayer()
{
    __try {
        return reinterpret_cast<GetPlayerFn>(kGetPlayerVa)(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// How many circuits the game added to the player's inventory (0 or 1), or -1 if the call faulted.
int GiveCircuit(uint8_t* player, const CircuitDef& c)
{
    Guid g = CircuitGuid(c);
    __try {
        return static_cast<int>(
            reinterpret_cast<GiveItemFn>(kGiveItemVa)(player + kInventoryOffset, nullptr, &g, 1));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// FUN_0059ef70: __thiscall void RecordFound(const Guid* item, char skipCheck, char notNew), this = the discovered list
// (player + 0xFBC), ret 0xC. Builds a record (GUID, +0x10 = new unless notNew, +0x11 = skipCheck) and hands it to
// FUN_005b0b40, which adds it if the item is not listed yet. A world pickup (FUN_00469ab0) calls it with (1, 0) after
// adding the item to the inventory; the Schematics do the same.
constexpr uintptr_t kRecordFoundVa = 0x0059EF70;
constexpr unsigned char kRecordFoundBytes[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x8B, 0x60,
                                               0xCD, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};

using RecordFoundFn = void(__fastcall*)(void* list, void* edx, const Guid* item, char skipCheck, char notNew);

// False if the call faulted; whether it recorded anything is for KnownToSave to say.
bool RecordFound(uint8_t* player, const CircuitDef& c)
{
    Guid g = CircuitGuid(c);
    __try {
        reinterpret_cast<RecordFoundFn>(kRecordFoundVa)(player + kDiscoveredOffset, nullptr, &g, 1, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Gives the seals back to the block the purchase took them from.
bool RefundSeals(uint8_t* player, unsigned price)
{
    __try {
        bool alt = (*reinterpret_cast<const uint32_t*>(player + kPlayerFlagsOffset) >> 13) & 1;
        uint8_t* block = player + (alt ? kResourceBlockAltOffset : kResourceBlockOffset);
        reinterpret_cast<AddResourceFn>(kAddResourceVa)(block, nullptr, kSealResource, price);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Refunds a purchase only while its payer is still the local player, so a stale pointer is never written through.
bool RefundPayer(const PendingCircuit& p)
{
    uint8_t* payer = reinterpret_cast<uint8_t*>(p.player);
    return payer && payer == LocalPlayer() && RefundSeals(payer, p.price);
}

// A purchase still waiting was never applied by the game; give its seals back rather than lose them.
void RefundUndelivered()
{
    if (Waiting(g_pending)) {
        bool refunded = RefundPayer(g_pending);
        Log("%s: the previous purchase was never delivered; %u seals %s", g_pending.pick.title, g_pending.price,
            refunded ? "refunded" : "NOT REFUNDED (payer is gone)");
        g_earlierRefund = refunded ? "Earlier purchase refunded" : "Earlier purchase lost: see log";
        g_pending = {};
    }
    g_shownCircuit = {};
}

// The Resource Pack asset the charge goes through, checked first so the game cannot charge and then fail to roll.
bool ChargeReady()
{
    Guid resource{};
    return g_circuitPacksReady && PackGuid(kStockPacks[0].asset, &resource) && SafeLookup(kPackRollVa, resource);
}

// Charges as a stock Resource Pack at this offer's price and turns the roll into the circuit marker.
bool ChargeAsMarker(const char* pack, unsigned price)
{
    if (!g_origBuyWithSeals(kResourcePackType, price)) {
        Log("%s -> refused by game", pack);
        return false;
    }
    auto* rolled = reinterpret_cast<uint8_t*>(kRolledItemsVa);
    rolled[0] = rolled[1] = rolled[2] = 0;
    rolled[3] = kCircuitMarker;
    rolled[4] = rolled[5] = 0x0F;
    return true;
}

// A circuit pack or Schematic purchase, from HookBuyWithSeals. The circuit is picked first, so a failed pick charges
// nothing. The charge is the game's own (ChargeAsMarker), and the circuit waits for the game's apply step
// (HookApplyRecord), which gives it and then shows the popup.
bool BuyCircuit(int packType, unsigned price)
{
    const ModOffer* offer = ModOfferByType(packType);
    const char* pack = offer->name;
    RefundUndelivered();
    if (!ChargeReady()) {
        Log("%s: not available; refused, nothing charged", pack);
        return false;
    }

    // Only circuits this save has discovered (see KnownToSave), or for a Schematic only those it has not; an offer with
    // nothing to give is refused.
    RefreshCircuitAvailability();
    if (!ModOfferAvailable(offer->id)) {
        Log("%s: this save has %s; refused, nothing charged", pack,
            offer->kind == OfferKind::Schematic ? "found every circuit it could unlock"
                                                : "not discovered a circuit it could give");
        return false;
    }

    // The tier first (guarantee, else the pack's odds), then a circuit in it. In the Upgrade Circuit Pack a tier the
    // save has not discovered yet gives its share to the tier below (Elite to Advanced, Advanced to Standard), and
    // with no Elite the guarantee count waits.
    CircuitGrant pick = {nullptr, &kEliteTier, pack, "Random circuit", Delivery::Failed};
    bool elitesOpen = g_available.elite > 0, advancedOpen = g_available.advanced > 0;
    int eliteNext = elitesOpen ? g_pity.elite + 1 : g_pity.elite, masterworkNext = g_pity.masterwork + 1;
    if (offer->kind == OfferKind::Schematic) {
        pick.tier = packType == kAdvancedSchematicType ? &kAdvancedTier
                    : packType == kEliteSchematicType  ? &kEliteTier
                                                       : &kMasterworkTier;
        pick.label = "New circuit";
    } else if (packType == kEliteCircuitType) {
        pick.label = "Elite circuit";
    } else if (packType == kMasterworkPackType) {
        if (masterworkNext >= MasterworkPityPacks()) {
            pick.tier = &kMasterworkTier;
            pick.label = "Guaranteed Masterwork";
        } else if (Random01() < 0.15f) {
            pick.tier = &kMasterworkTier;
        }
    } else if (elitesOpen && eliteNext >= ElitePityPacks()) {
        pick.label = "Guaranteed Elite";
    } else {
        float r = Random01();
        pick.tier = r < 0.5f ? &kStandardTier : r < 0.9f || !elitesOpen ? &kAdvancedTier : &kEliteTier;
        if (pick.tier == &kAdvancedTier && !advancedOpen) pick.tier = &kStandardTier;
    }
    pick.def = PickFrom(*pick.tier, offer->kind == OfferKind::Schematic);
    if (!pick.def) {
        Log("%s: no eligible %s circuit; refused, nothing charged", pack, pick.tier->name);
        return false;
    }

    if (!ChargeAsMarker(pack, price)) return false;
    g_pending = {pick, packType, price, eliteNext, masterworkNext, reinterpret_cast<uintptr_t>(LocalPlayer()), nullptr};
    char text[96];
    CircuitText(*pick.def, text, sizeof(text));
    Log("%s -> charged %u seals; %s \"%s\" %08X-%08X (%s) waits for the store to apply it", pack, price,
        pick.tier->name, text, pick.def->d0, pick.def->d1, pick.label);
    return true;
}

// Gives the waiting circuit to its payer, who must be the store's own player (the one the game gives stock packs to);
// a circuit is never given to anyone else. If the game adds none, the seals go back and the pity counts stay.
void DeliverCircuit(PendingCircuit p, uint32_t storePlayer)
{
    CircuitGrant& pick = p.pick;
    char text[96];
    CircuitText(*pick.def, text, sizeof(text));
    uint8_t* payer = reinterpret_cast<uint8_t*>(p.player);
    int added;
    if (!payer || storePlayer != p.player) {
        Log("%s: player charged %p, store's player %08X - DIFFERENT PLAYERS; not given", pick.title, payer,
            storePlayer);
        added = 0;
    } else {
        added = GiveCircuit(payer, *pick.def);
    }
    // A Schematic's circuit is then recorded as found, as a world pickup's is. If that does not take, the circuit stays
    // in the inventory, where the bench does not show it, and the seals go back.
    const ModOffer* offer = ModOfferByType(p.packType);
    if (offer && offer->kind == OfferKind::Schematic && added >= 1 &&
        !(RecordFound(payer, *pick.def) && KnownToSave(payer, *pick.def) == 1)) {
        Log("%s: \"%s\" was added but could not be recorded as found", pick.title, text);
        added = 0;
    }
    if (added >= 1 && KnownToSave(payer, *pick.def) != 1)
        Log("%s: \"%s\" is not on this save's discovered list; it will not show at the bench", pick.title, text);
    if (added >= 1) {
        pick.delivery = Delivery::Given;
        if (!MarkCircuitNew(payer, *pick.def)) Log("%s: could not mark \"%s\" new", pick.title, text);
        if (p.packType == kCircuitPackType) {
            g_pity.elite = pick.tier == &kEliteTier ? 0 : p.eliteNext;
            SavePity();
        } else if (p.packType == kMasterworkPackType) {
            g_pity.masterwork = pick.tier == &kMasterworkTier ? 0 : p.masterworkNext;
            SavePity();
        }
        uint32_t flags = 0;
        SafeReadU32(p.player + kPlayerFlagsOffset, &flags);
        Log("%s -> OK (%u seals): %s \"%s\" given to player %p (%s; player flags %08X, %s resource block); pity now "
            "%d/%d toward an Elite, %d/%d toward a Masterwork",
            pick.title, p.price, pick.tier->name, text, payer, pick.label, flags,
            (flags >> 13) & 1 ? "alternate" : "normal", g_pity.elite, ElitePityPacks(), g_pity.masterwork,
            MasterworkPityPacks());
    } else if (added == 0) {
        pick.delivery = RefundPayer(p) ? Delivery::Refunded : Delivery::Failed;
        Log("%s: no circuit added (%s \"%s\"); %s; pity unchanged", pick.title, pick.tier->name, text,
            pick.delivery == Delivery::Refunded ? "seals refunded" : "REFUND FAILED");
    } else {
        // The outcome is unknown, so neither a refund nor a second try.
        pick.delivery = Delivery::Failed;
        Log("%s: GIVING THE CIRCUIT FAULTED (%s \"%s\", player %p); %u seals charged, nothing refunded, pity unchanged",
            pick.title, pick.tier->name, text, payer, p.price);
    }
    g_shownCircuit = pick;
}

// FUN_00519fb0: __thiscall void ApplyRecord(const uint8_t* record), this = the store handler. Called by handler case 7
// for the stored records and the pending roll; gives resources, then each part through FUN_00513dd0 (which gives to
// the handler's player at +0x60).
constexpr uintptr_t kApplyRecordVa = 0x00519FB0;
constexpr unsigned char kApplyRecordBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x38, 0x53, 0x56,
                                               0x57, 0x8B, 0xF9, 0x8B, 0x4D, 0x08, 0x8A, 0x41};
constexpr uintptr_t kHandlerPlayerOffset = 0x60;

using ApplyRecordFn = void(__fastcall*)(void* handler, void* edx, const uint8_t* record);
ApplyRecordFn g_origApplyRecord = nullptr;

// The part lists at player +0xF74 and +0xF80 (FUN_004ce0e0): 0x18-byte entries of hash, uint16 locked (+4), uint16
// new (+6) and four counts (+8). The bench shows the "new" icon while +6 is set and clears it on hover.
// Every entry starts with it set, so a part unlocked for the first time shows as new, but more of one already seen do
// not. After a seal pack, each part whose count went up is marked new again.
constexpr uintptr_t kPartListOffsets[2] = {0xF74, 0xF80};
constexpr uint32_t kPartEntrySize = 0x18;
constexpr uint32_t kMaxPartEntries = 256;

struct PartCounts {
    uint32_t entries, count;
    uint32_t sum[kMaxPartEntries];
};

bool ReadPartCounts(uintptr_t player, uintptr_t listOffset, PartCounts* out)
{
    out->entries = out->count = 0;
    if (!player || !SafeReadU32(player + listOffset, &out->entries) ||
        !SafeReadU32(player + listOffset + 4, &out->count) || !out->entries || out->count > kMaxPartEntries)
        return false;
    for (uint32_t i = 0; i < out->count; ++i) {
        out->sum[i] = 0;
        for (int c = 0; c < 4; ++c) {
            uint32_t v = 0;
            if (!SafeReadU32(out->entries + i * kPartEntrySize + 8 + c * 4, &v)) return false;
            out->sum[i] += v;
        }
    }
    return true;
}

int MarkNewParts(uintptr_t player, const PartCounts (&before)[2])
{
    int marked = 0;
    for (int l = 0; l < 2; ++l) {
        PartCounts after;
        if (!before[l].entries || !ReadPartCounts(player, kPartListOffsets[l], &after) ||
            after.entries != before[l].entries || after.count != before[l].count)
            continue;
        for (uint32_t i = 0; i < after.count; ++i) {
            if (after.sum[i] <= before[l].sum[i]) continue;
            auto* entry = reinterpret_cast<uint16_t*>(after.entries + i * kPartEntrySize);
            __try {
                if (entry[2] == 0) {  // unlocked
                    entry[3] = 1;
                    ++marked;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    return marked;
}

// ---- dump_parts (debug, read-only): every weapon-part entry the game has loaded, and this save's state for each. ----
// A part entry (FUN_004cddf0) is what the packs' part tables list and FUN_00513dd0 grants: +0x2D0 an item to spawn and
// record as found (frames, attachments), or +0x2F0 / +0x2F4 a hash unlocked in the part list at player +0xF74 (tools)
// / +0xF80 (tips), with the slot at +0x2F8 / +0x2FC. Logged once per player; the part-list entries no loaded part entry
// names are listed after.
constexpr int kMaxDumpHashes = 512;

// The part-list entry for a hash: locked (+4), new (+6), four counts (+8). False if not in the list.
bool FindPartEntry(uintptr_t player, uintptr_t listOffset, uint32_t hash, uint16_t* locked, uint16_t* isNew,
                   uint32_t counts[4])
{
    uint32_t entries = 0, count = 0;
    if (!SafeReadU32(player + listOffset, &entries) || !SafeReadU32(player + listOffset + 4, &count) ||
        count > kMaxPartEntries)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t e = entries + i * kPartEntrySize;
        uint32_t h = 0, flags = 0;
        if (!SafeReadU32(e, &h) || h != hash || !SafeReadU32(e + 4, &flags)) continue;
        *locked = static_cast<uint16_t>(flags);
        *isNew = static_cast<uint16_t>(flags >> 16);
        for (int c = 0; c < 4; ++c) SafeReadU32(e + 8 + c * 4, &counts[c]);
        return true;
    }
    return false;
}

void DumpParts()
{
    static uintptr_t dumpedFor = 0;
    static int checked = -1;
    if (checked < 0) checked = PackFunctionsOk("part dump") ? 1 : 0;
    uintptr_t player = reinterpret_cast<uintptr_t>(LocalPlayer());
    uint32_t manager = 0;
    if (!checked || !player || player == dumpedFor || !SafeReadU32(kAssetManagerPtrVa, &manager) || !manager) return;
    dumpedFor = player;

    Log("===== part dump (debug, read-only), player %08X =====", static_cast<unsigned>(player));
    static uint32_t named[2][kMaxDumpHashes];
    int namedCount[2] = {}, parts = 0;
    for (int b = 0; b < kAssetBuckets; ++b) {
        uint32_t entry = 0;
        if (!SafeReadU32(manager + 0x80 + b * 4, &entry)) return;
        for (int depth = 0; entry && depth < 256; ++depth) {
            Guid g{};
            uint32_t hash = 0, next = 0;
            if (!SafeReadGuid(reinterpret_cast<const uint8_t*>(entry) + 0x0C, &g) || !SafeReadU32(entry + 0x1C, &hash) ||
                !SafeReadU32(entry + 0x34, &next))
                break;
            entry = next;
            uint32_t expect = ((g.d[0] * 33 + g.d[1]) * 33 + g.d[2]) * 33 + g.d[3];
            const uint8_t* part = expect == hash ? SafeLookup(kPartItemVa, g) : nullptr;
            if (!part) continue;
            ++parts;
            char name[96], line[512];
            ItemName(g, name, sizeof(name));
            int used = sprintf_s(line, "  part %08X-%08X-%08X-%08X \"%s\":", g.d[0], g.d[1], g.d[2], g.d[3], name);
            Guid spawn{};
            if (SafeReadGuid(part + 0x2D0, &spawn) && !IsNullGuid(spawn) && used > 0) {
                char spawnName[96];
                ItemName(spawn, spawnName, sizeof(spawnName));
                used += sprintf_s(line + used, sizeof(line) - used, " spawns %08X-%08X-%08X-%08X \"%s\" found=%d",
                                  spawn.d[0], spawn.d[1], spawn.d[2], spawn.d[3], spawnName,
                                  OnDiscoveredList(reinterpret_cast<const uint8_t*>(player), spawn));
            }
            for (int l = 0; l < 2 && used > 0; ++l) {
                uint32_t h = 0, slot = 0;
                SafeReadU32(reinterpret_cast<uintptr_t>(part) + 0x2F0 + l * 4, &h);
                SafeReadU32(reinterpret_cast<uintptr_t>(part) + 0x2F8 + l * 4, &slot);
                if (!h) continue;
                if (namedCount[l] < kMaxDumpHashes) named[l][namedCount[l]++] = h;
                uint16_t locked = 0, isNew = 0;
                uint32_t counts[4] = {};
                if (FindPartEntry(player, kPartListOffsets[l], h, &locked, &isNew, counts))
                    used += sprintf_s(line + used, sizeof(line) - used,
                                      " %s %08X slot %u: locked=%u new=%u counts %u/%u/%u/%u", l ? "tip" : "tool", h,
                                      slot, locked, isNew, counts[0], counts[1], counts[2], counts[3]);
                else
                    used += sprintf_s(line + used, sizeof(line) - used, " %s %08X slot %u: NOT IN LIST",
                                      l ? "tip" : "tool", h, slot);
            }
            Log("%s", line);
        }
    }
    for (int l = 0; l < 2; ++l) {
        uint32_t entries = 0, count = 0;
        if (!SafeReadU32(player + kPartListOffsets[l], &entries) ||
            !SafeReadU32(player + kPartListOffsets[l] + 4, &count) || count > kMaxPartEntries)
            continue;
        int unnamed = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t e = entries + i * kPartEntrySize;
            uint32_t h = 0, flags = 0, c[4] = {};
            if (!SafeReadU32(e, &h) || !SafeReadU32(e + 4, &flags)) continue;
            bool found = false;
            for (int n = 0; n < namedCount[l] && !found; ++n) found = named[l][n] == h;
            if (found) continue;
            ++unnamed;
            for (int k = 0; k < 4; ++k) SafeReadU32(e + 8 + k * 4, &c[k]);
            Log("  %s list entry %u %08X (no loaded part entry): locked=%u new=%u counts %u/%u/%u/%u",
                l ? "tip" : "tool", i, h, flags & 0xFFFF, flags >> 16, c[0], c[1], c[2], c[3]);
        }
        Log("  %s list: %u entries, %d without a loaded part entry", l ? "tip" : "tool", count, unnamed);
    }
    Log("part dump: %d part entries loaded", parts);
}

// ---- Part unlocks. ----
// Each part unlock gives one MK-V or MK-II weapon part of its kind that the save does not have, through the game's
// own Ultra/Epic part grant FUN_00513dd0, from these lists only. MK-V parts are the Ultra/Epic pack pool; MK-II
// parts are NG+ drops. The part-list entries no loaded part entry names (DLC or cut) are never given.
// One part entry per part (some parts have more, under other ID families). Frames and attachments spawn an item and
// record it as found; tools and tips unlock a part-list entry by hash.
enum class PartKind { Attachment, Tip, Tool, Frame };

struct PartDef {
    const char* name;
    PartKind kind;
    uint32_t entry0, entry1;  // the part entry; the tier gives the last two dwords
    uint32_t spawn0, spawn1;  // frames, attachments: the item, in the main set (kCircuitSetD2/D3); else 0
    uint32_t hash;            // tools, tips: the part-list hash; else 0
};
constexpr uint32_t kMkvEntryD2 = 0x484C4143, kMkvEntryD3 = 0x304E554F;
constexpr uint32_t kMkiiEntryD2 = 0x4A315357, kMkiiEntryD3 = 0x41484346;

constexpr PartDef kMkvParts[] = {
    {"MK-V Acid Bath", PartKind::Attachment, 0x31CDB251, 0x20B87870, 0x48CD816A, 0x0283CACD, 0},
    {"MK-V Ammo Box", PartKind::Attachment, 0x31CDB251, 0x8A5D4227, 0x48CD816A, 0x6C523DA5, 0},
    {"MK-V Ammo Support", PartKind::Attachment, 0x31CDB251, 0x2E00C61A, 0x48CD816A, 0x204DFB63, 0},
    {"MK-V Ammo Sweeper", PartKind::Attachment, 0x31CDB251, 0x751C3170, 0x48CD816A, 0x152F79B6, 0},
    {"MK-V Damage Support", PartKind::Attachment, 0x31CDB251, 0x3609DD42, 0x48CD816A, 0x2B7A390A, 0},
    {"MK-V Electric Charge", PartKind::Attachment, 0x31CDB251, 0x3B376010, 0x48CD816A, 0x369908D9, 0},
    {"MK-V Explosion Amplifier", PartKind::Attachment, 0x31CDB251, 0x418D64E5, 0x48CD816A, 0x41023FA2, 0},
    {"MK-V Flame Glaze", PartKind::Attachment, 0x31CDB251, 0x49CB21F6, 0x48CD8166, 0x9E1C7093, 0},
    {"MK-V Full Zoom Scope", PartKind::Attachment, 0x31CDB251, 0x606CDD24, 0x48CD816A, 0x4C130555, 0},
    {"MK-V Medic Support", PartKind::Attachment, 0x31CDB251, 0x7ED1CB10, 0x48CD816A, 0x565C573D, 0},
    {"MK-V Safety Guard", PartKind::Attachment, 0x31CDB251, 0x9167A77A, 0x48CD816A, 0x7693EE3B, 0},
    {"MK-V Scope", PartKind::Attachment, 0x31CDB251, 0x98FEC007, 0x48CD816A, 0x7F84AC31, 0},
    {"MK-V Stasis Amplifier", PartKind::Attachment, 0x31CDB251, 0x9EC3CBC6, 0x48CD816A, 0x88A247AD, 0},
    {"MK-V Stasis Coating", PartKind::Attachment, 0x31CDB251, 0xAEA39E2C, 0x48CD816A, 0x9263A16A, 0},
    {"MK-V Stasis Support", PartKind::Attachment, 0x31CDB251, 0xD435E3A0, 0x48CD816A, 0x9B9E89A6, 0},
    {"MK-V Compact Conic Dispersal", PartKind::Tip, 0x31CDB257, 0x3B6FFAE0, 0, 0, 0xF316E138},
    {"MK-V Compact Directed Ejection Field", PartKind::Tip, 0x31CDB257, 0x48E2445E, 0, 0, 0xE0730D80},
    {"MK-V Compact Directed Suspension Field", PartKind::Tip, 0x31CDB257, 0x6F3CF5CA, 0, 0, 0x0EEC2E06},
    {"MK-V Compressor", PartKind::Tip, 0x31CDB257, 0x27BADE34, 0, 0, 0x51FFAEDA},
    {"MK-V Conic Dispersal", PartKind::Tip, 0x31CDB257, 0x33F53EB4, 0, 0, 0x42CA0DF0},
    {"MK-V Diffraction Torus", PartKind::Tip, 0x31CDB257, 0x2E0687D6, 0, 0, 0xEFCC2B3C},
    {"MK-V Directed Ejection Field", PartKind::Tip, 0x31CDB257, 0x4227D40E, 0, 0, 0x3F99ABA8},
    {"MK-V Directed Suspension Field", PartKind::Tip, 0x31CDB257, 0x68D67AC8, 0, 0, 0x2793E6A2},
    {"MK-V Precision Tip", PartKind::Tip, 0x31CDB257, 0x4F0E8A64, 0, 0, 0x00C5E701},
    {"MK-V Rail Accelerator", PartKind::Tip, 0x31CDB257, 0x554EC670, 0, 0, 0x2189E16D},
    {"MK-V Repeater", PartKind::Tip, 0x31CDB257, 0x5A3E913C, 0, 0, 0x7E155D17},
    {"MK-V Canister Recovery Module", PartKind::Tool, 0x31CDB258, 0x7756A50B, 0, 0, 0x742E752A},
    {"MK-V Electrocution Module", PartKind::Tool, 0x31CDB258, 0x6B2CA325, 0, 0, 0xE002E5ED},
    {"MK-V Explosive Module", PartKind::Tool, 0x31CDB258, 0x71356F68, 0, 0, 0x47A940DA},
    {"MK-V Rotator Cuff Module", PartKind::Tool, 0x31CDB258, 0x7DDD50A5, 0, 0, 0x84538881},
    {"MK-V Compact Supercharged Frame", PartKind::Frame, 0x31CDB253, 0x2B669308, 0x48CD818C, 0x4A677D58, 0},
    {"MK-V Heavy Supercharged Frame", PartKind::Frame, 0x31CDB253, 0x3A1AFD24, 0x48CD818C, 0x55B48A46, 0},
};
constexpr PartDef kMkiiParts[] = {
    {"MK-II Acid Bath", PartKind::Attachment, 0x42CDB7AB, 0x8E85BD5B, 0x48CD8166, 0xA36F7DB7, 0},
    {"MK-II Ammo Box", PartKind::Attachment, 0x42CDB7AC, 0xC8A3D338, 0x48CD816A, 0x5BDAE24E, 0},
    {"MK-II Ammo Support", PartKind::Attachment, 0x42CDB7AB, 0xAEF269B5, 0x48CD816A, 0x1AFC9BFA, 0},
    {"MK-II Ammo Sweeper", PartKind::Attachment, 0x42CDB7AC, 0x845E77E4, 0x48CD816A, 0x100A0D6D, 0},
    {"MK-II Damage Support", PartKind::Attachment, 0x42CDB7AB, 0xC5CFF877, 0x48CD816A, 0x271C197B, 0},
    {"MK-II Electric Charge", PartKind::Attachment, 0x42CDB7AB, 0xDDD2533B, 0x48CD816A, 0x322F9E60, 0},
    {"MK-II Explosion Amplifier", PartKind::Attachment, 0x42CDB7AB, 0xF7DE0041, 0x48CD816A, 0x3BF2A5D8, 0},
    {"MK-II Flame Glaze", PartKind::Attachment, 0x42CDB7AC, 0x0A4E229A, 0x48CD8166, 0x99895CF9, 0},
    {"MK-II Full Zoom Scope", PartKind::Attachment, 0x42CDB7AC, 0x671FE9CC, 0x48CD816A, 0x46A172E9, 0},
    {"MK-II Medic Support", PartKind::Attachment, 0x42CDB7AC, 0x98A17F16, 0x48CD816A, 0x513208D4, 0},
    {"MK-II Safety Guard", PartKind::Attachment, 0x42CDB7AC, 0xDDAECFE1, 0x48CD816A, 0x7235F5BD, 0},
    {"MK-II Scope", PartKind::Attachment, 0x42CDB7AD, 0x05E89D3D, 0x48CD816A, 0x7B268CA2, 0},
    {"MK-II Stasis Amplifier", PartKind::Attachment, 0x42CDB7AD, 0x16489BE3, 0x48CD816A, 0x84318A04, 0},
    {"MK-II Stasis Coating", PartKind::Attachment, 0x42CDB7AD, 0x267B5FF7, 0x48CD816A, 0x8D098F38, 0},
    {"MK-II Stasis Support", PartKind::Attachment, 0x42CDB7AD, 0x386B498D, 0x48CD816A, 0x97A0DC0F, 0},
    {"MK-II Compact Conic Dispersal", PartKind::Tip, 0x42CDB7B1, 0x27DF382A, 0, 0, 0x2B164A85},
    {"MK-II Compact Directed Ejection Field", PartKind::Tip, 0x42CDB7B1, 0x4E1B82CE, 0, 0, 0xF890D2CD},
    {"MK-II Compact Directed Suspension Field", PartKind::Tip, 0x42CDB7B1, 0xD7A7778F, 0, 0, 0x7E761053},
    {"MK-II Compressor", PartKind::Tip, 0x42CDB7B0, 0xFD60814E, 0, 0, 0xD82E6727},
    {"MK-II Conic Dispersal", PartKind::Tip, 0x42CDB7B1, 0x3CBA6D20, 0, 0, 0xD68E9B3D},
    {"MK-II Diffraction Torus", PartKind::Tip, 0x42CDB7B1, 0x1085E0B0, 0, 0, 0x8FBED289},
    {"MK-II Directed Ejection Field", PartKind::Tip, 0x42CDB7B1, 0x5CCF05BC, 0, 0, 0x05FADCF5},
    {"MK-II Directed Suspension Field", PartKind::Tip, 0x42CDB7B1, 0xE65637B1, 0, 0, 0x9B933AEF},
    {"MK-II Precision Tip", PartKind::Tip, 0x42CDB7B1, 0x6E458800, 0, 0, 0x2A8CFBCE},
    {"MK-II Rail Accelerator", PartKind::Tip, 0x42CDB7B1, 0x80331030, 0, 0, 0x6A22803A},
    {"MK-II Repeater", PartKind::Tip, 0x42CDB7B1, 0xBF4AA7A6, 0, 0, 0x1656C6E4},
    {"MK-II Canister Recovery Module", PartKind::Tool, 0x42CDB7AE, 0x360AABC3, 0, 0, 0x6ABF0577},
    {"MK-II Electrocution Module", PartKind::Tool, 0x42CDB7AE, 0x011AB277, 0, 0, 0xB36144BA},
    {"MK-II Explosive Module", PartKind::Tool, 0x42CDB7AE, 0x266DDFCA, 0, 0, 0x6E2EF927},
    {"MK-II Rotator Cuff Module", PartKind::Tool, 0x42CDB7AE, 0x45FF8B7D, 0, 0, 0x3E61DD4E},
    {"MK-II Overclocked Compact Frame", PartKind::Frame, 0x42CDB7AD, 0x86FE3259, 0x48CD818C, 0x44D67B3E, 0},
    {"MK-II Overclocked Heavy Frame", PartKind::Frame, 0x42CDB7AD, 0xE5485A55, 0x48CD818C, 0x4F901E06, 0},
};
constexpr int kMkiiPartCount = sizeof(kMkiiParts) / sizeof(PartDef);

// FUN_00513dd0: __thiscall void GrantPart(const Guid* partEntry), this = the store handler (player at +0x60), ret 4.
constexpr uintptr_t kGrantPartVa = 0x00513DD0;
constexpr unsigned char kGrantPartBytes[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0x8B,
                                             0x06, 0x85, 0xC0, 0x75, 0x13, 0x39, 0x46, 0x04};
using GrantPartFn = void(__fastcall*)(void* handler, void* edx, const Guid* partEntry);

bool IsMkii(const PartDef& p)
{
    return &p >= kMkiiParts && &p < kMkiiParts + kMkiiPartCount;
}

Guid PartEntryGuid(const PartDef& p)
{
    return IsMkii(p) ? Guid{{p.entry0, p.entry1, kMkiiEntryD2, kMkiiEntryD3}}
                     : Guid{{p.entry0, p.entry1, kMkvEntryD2, kMkvEntryD3}};
}

Guid PartSpawnGuid(const PartDef& p)
{
    return {{p.spawn0, p.spawn1, kCircuitSetD2, kCircuitSetD3}};
}

// The part unlock offer a part belongs to: 0-3 MK-V, 4-7 MK-II, each attachment / tip / tool / frame.
int PartGroup(const PartDef& p)
{
    return (IsMkii(p) ? 4 : 0) + static_cast<int>(p.kind);
}

// Loaded, and its part entry still names the expected item or hash.
bool PartLoaded(const PartDef& p)
{
    const uint8_t* part = SafeLookup(kPartItemVa, PartEntryGuid(p));
    if (!part) return false;
    if (p.hash) {
        uint32_t h = 0;
        return SafeReadU32(reinterpret_cast<uintptr_t>(part) + (p.kind == PartKind::Tool ? 0x2F0 : 0x2F4), &h) &&
               h == p.hash;
    }
    Guid spawn{}, want = PartSpawnGuid(p);
    return SafeReadGuid(part + 0x2D0, &spawn) && std::memcmp(&spawn, &want, sizeof(spawn)) == 0;
}

// 1 = the save has it (found, or unlocked in the part list), 0 = not, -1 = unknown (no list entry, or unreadable).
int PartOwned(uintptr_t player, const PartDef& p)
{
    if (!player) return -1;
    if (p.hash) {
        uint16_t locked = 0, isNew = 0;
        uint32_t counts[4] = {};
        uintptr_t list = kPartListOffsets[p.kind == PartKind::Tool ? 0 : 1];
        if (!FindPartEntry(player, list, p.hash, &locked, &isNew, counts)) return -1;
        return locked ? 0 : 1;
    }
    return OnDiscoveredList(reinterpret_cast<const uint8_t*>(player), PartSpawnGuid(p));
}

template <typename Fn>
void ForEachPart(Fn fn)
{
    for (const PartDef& p : kMkvParts) fn(p);
    for (const PartDef& p : kMkiiParts) fn(p);
}

bool PartEligible(int group, const PartDef& p, uintptr_t player)
{
    return PartGroup(p) == group && PartLoaded(p) && PartOwned(player, p) == 0;
}

int CountMissingParts(int group, uintptr_t player)
{
    int n = 0;
    ForEachPart([&](const PartDef& p) { n += PartEligible(group, p, player); });
    return n;
}

// Equal odds over the group's parts the local player does not have. 0 if none.
const PartDef* PickPart(int group)
{
    uintptr_t player = reinterpret_cast<uintptr_t>(LocalPlayer());
    int eligible = CountMissingParts(group, player);
    if (!eligible) return nullptr;
    int target = static_cast<int>(Random01() * eligible);
    const PartDef* picked = nullptr;
    ForEachPart([&](const PartDef& p) {
        if (!picked && PartEligible(group, p, player) && target-- == 0) picked = &p;
    });
    return picked;
}

// A part unlock purchase, from HookBuyWithSeals; the same steps as BuyCircuit. The part waits for the apply step.
bool BuyPart(const ModOffer* offer, unsigned price)
{
    const char* pack = offer->name;
    RefundUndelivered();
    if (!g_partsReady || !ChargeReady()) {
        Log("%s: not available; refused, nothing charged", pack);
        return false;
    }
    RefreshCircuitAvailability();
    const PartDef* part = PickPart(offer->type - kFirstPartType);
    if (!part) {
        Log("%s: this save has every part it could unlock; refused, nothing charged", pack);
        return false;
    }
    if (!ChargeAsMarker(pack, price)) return false;
    CircuitGrant pick = {nullptr, nullptr, pack, "New part", Delivery::Failed, part->name};
    g_pending = {pick, offer->type, price, 0, 0, reinterpret_cast<uintptr_t>(LocalPlayer()), part};
    Log("%s -> charged %u seals; \"%s\" waits for the store to apply it", pack, price, part->name);
    return true;
}

// Grants the waiting part to its payer through the game's grant, only if the store's player is the payer and still
// lacks it, then checks that it took: found (frames, attachments) or unlocked (tools, tips). Otherwise the seals go
// back. A fault in the grant is logged and not refunded, as the outcome is unknown.
void DeliverPart(PendingCircuit p, void* handler, uint32_t storePlayer)
{
    CircuitGrant& pick = p.pick;
    const PartDef& part = *p.part;
    bool faulted = false, given = false;
    if (!p.player || storePlayer != p.player) {
        Log("%s: player charged %08X, store's player %08X - DIFFERENT PLAYERS; not given", pick.title,
            static_cast<unsigned>(p.player), storePlayer);
    } else if (PartOwned(p.player, part) != 0) {
        Log("%s: \"%s\" is no longer missing on this save; not given", pick.title, part.name);
    } else {
        static PartCounts before[2];
        for (int l = 0; l < 2; ++l) {
            if (!ReadPartCounts(p.player, kPartListOffsets[l], &before[l])) before[l].entries = 0;
        }
        Guid entry = PartEntryGuid(part);
        __try {
            reinterpret_cast<GrantPartFn>(kGrantPartVa)(handler, nullptr, &entry);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            faulted = true;
        }
        given = !faulted && PartOwned(p.player, part) == 1;
        if (given) {
            MarkNewParts(p.player, before);
            SetBenchBadge(p.player, kNewPartsBadge);
            // A frame or attachment: its discovered record's "new" byte, which the game sets only on a record it
            // creates (it should have here, as the part was missing; this makes sure).
            if (!part.hash && !MarkRecordNew(reinterpret_cast<const uint8_t*>(p.player), PartSpawnGuid(part),
                                             kNewPartsBadge))
                Log("%s: could not mark \"%s\" new", pick.title, part.name);
        }
    }
    if (given) {
        pick.delivery = Delivery::Given;
        Log("%s -> OK (%u seals): \"%s\" given to player %08X", pick.title, p.price, part.name,
            static_cast<unsigned>(p.player));
    } else if (faulted) {
        pick.delivery = Delivery::Failed;
        Log("%s: GRANTING \"%s\" FAULTED; %u seals charged, nothing refunded", pick.title, part.name, p.price);
    } else {
        pick.delivery = RefundPayer(p) ? Delivery::Refunded : Delivery::Failed;
        Log("%s: \"%s\" not given; %s", pick.title, part.name,
            pick.delivery == Delivery::Refunded ? "seals refunded" : "REFUND FAILED");
    }
    g_shownCircuit = pick;
}

// A circuit marker never reaches the game, which would read it as an Ultra pack; the waiting circuit is given instead,
// once. A marker with nothing waiting (already given) does nothing. Any other record goes to the game, and the parts
// it gave are marked new.
void __fastcall HookApplyRecord(void* handler, void* edx, const uint8_t* record)
{
    if (!record || record[3] != kCircuitMarker) {
        uint32_t player = 0;
        SafeReadU32(reinterpret_cast<uintptr_t>(handler) + kHandlerPlayerOffset, &player);
        static PartCounts before[2];
        static InventoryCounts items;
        for (int l = 0; l < 2; ++l) {
            if (!ReadPartCounts(player, kPartListOffsets[l], &before[l])) before[l].entries = 0;
        }
        ReadInventory(player, &items);
        g_origApplyRecord(handler, edx, record);
        if (int marked = MarkNewParts(player, before)) {
            SetBenchBadge(player, kNewPartsBadge);
            Log("pack applied: %d tool(s) / tip(s) marked new", marked);
        }
        if (int marked = MarkNewPartItems(player, items))
            Log("pack applied: %d frame(s) / attachment(s) marked new", marked);
        return;
    }
    if (!Waiting(g_pending)) return;
    uint32_t storePlayer = 0;
    SafeReadU32(reinterpret_cast<uintptr_t>(handler) + kHandlerPlayerOffset, &storePlayer);
    PendingCircuit p = g_pending;
    g_pending = {};
    if (p.part)
        DeliverPart(p, handler, storePlayer);
    else
        DeliverCircuit(p, storePlayer);
}

// FUN_0051a0c0: __thiscall void PackPopup(unsigned index), this = the store handler. Index < *(0x011E64E8) picks a
// stored record (0x011DEFB8 + 6 * index), anything else the pending roll. The popup is *(*(0x011D19D4) + 0x1C) + 0x94:
// title text at +0x94; resource rows 0-4 at +0x138 + 0x34 * row (flag byte, text +0x04, amount +0x28); part rows at
// +0x23C + 0x6C * row (flags word, name +0x04, label +0x48); show = vtable +0x4C. Text objects: vtable +0x1C sets text.
constexpr uintptr_t kPackPopupVa = 0x0051A0C0;
constexpr unsigned char kPackPopupBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x83, 0xEC,
                                             0x44, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x3B, 0x05};
constexpr uintptr_t kStoredRecordsVa = 0x011DEFB8;
constexpr uintptr_t kStoredRecordCountVa = 0x011E64E8;
constexpr uintptr_t kFrontEndPtrVa = 0x011D19D4;

using PackPopupFn = void(__fastcall*)(void* handler, void* edx, unsigned index);
using SetTextFn = void(__fastcall*)(void* text, void* edx, const char* s);
using ShowFn = void(__fastcall*)(void* popup, void* edx);
PackPopupFn g_origPackPopup = nullptr;

// Each popup call on its own, so one that faults does not stop the rest; a fault logs the object, the game function its
// vtable +0x1C points at, the exception and where it happened.
bool TrySetText(const char* what, uint8_t* text, const char* s)
{
    uint32_t vtable = 0, fn = 0;
    if (!SafeReadU32(reinterpret_cast<uintptr_t>(text), &vtable) || !vtable || !SafeReadU32(vtable + 0x1C, &fn)) {
        Log("circuit popup: %s: no text object at %p", what, text);
        return false;
    }
    DWORD code = 0;
    void* at = nullptr;
    __try {
        reinterpret_cast<SetTextFn>(fn)(text, nullptr, s);
        return true;
    } __except (code = GetExceptionCode(), at = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                EXCEPTION_EXECUTE_HANDLER) {
        Log("circuit popup: %s (object %p, vtable %08X, set-text %08X, \"%s\") faulted: %08lX at %p", what, text, vtable,
            fn, s, code, at);
        return false;
    }
}

// The popup as the game fills it for a part, with the circuit (or unlocked part) as the only row and no resources.
void ShowCircuitPopup()
{
    uint32_t frontEnd = 0, screen = 0;
    if (!SafeReadU32(kFrontEndPtrVa, &frontEnd) || !frontEnd || !SafeReadU32(frontEnd + 0x1C, &screen) || !screen) {
        Log("circuit popup: no popup to fill");
        return;
    }
    uint8_t* popup = reinterpret_cast<uint8_t*>(screen) + 0x94;
    char stats[96] = {}, text[128];
    if (g_shownCircuit.def) CircuitText(*g_shownCircuit.def, stats, sizeof(stats));
    // Short, so the stats fit the row (it cuts off at about 40 characters and does not scroll).
    if (g_shownCircuit.delivery == Delivery::Given && !g_shownCircuit.def)
        strcpy_s(text, g_shownCircuit.partName);
    else if (g_shownCircuit.delivery == Delivery::Given)
        sprintf_s(text, "%s: %s", g_shownCircuit.tier->name, stats);
    else if (g_shownCircuit.delivery == Delivery::Refunded)
        strcpy_s(text, "Not delivered: seals refunded");
    else
        strcpy_s(text, "Not delivered: see seal_store.log");
    int set = 0, tried = 0;
    // Five resource rows (no row for seals); a sixth would be part row 0.
    for (int r = 0; r < 5; ++r) {
        uint8_t* row = popup + 0x138 + r * 0x34;
        char what[32];
        sprintf_s(what, "resource row %d", r);
        tried += 2;
        set += TrySetText(what, row + 0x04, "") + TrySetText(what, row + 0x28, "");
        row[0] = 0;
    }
    for (int r = 0; r < 3; ++r) {
        uint8_t* row = popup + 0x23C + r * 0x6C;
        char what[32];
        sprintf_s(what, "part row %d", r);
        *reinterpret_cast<uint16_t*>(row) = r == 0 ? 0x0101 : 0x0001;  // as FUN_00513ee0 marks a part / no part
        tried += 2;
        set += TrySetText(what, row + 0x04, r == 0 ? text : "");
        if (r == 0) {
            // Labels are localisation keys in the game's own calls; fall back to one if plain text faults.
            set += TrySetText("part label", row + 0x48, g_shownCircuit.label) ||
                   TrySetText("part label (key)", row + 0x48, "$fe_mtx_random_part");
        } else {
            set += TrySetText(what, row + 0x48, "");
        }
    }
    ++tried;
    set += TrySetText("title", popup + 0x94, g_earlierRefund ? g_earlierRefund : g_shownCircuit.title);
    g_earlierRefund = nullptr;
    DWORD code = 0;
    void* at = nullptr;
    __try {
        reinterpret_cast<ShowFn>((*reinterpret_cast<void***>(popup))[0x4C / 4])(popup, nullptr);
        Log("circuit popup shown (%d of %d texts set): %s", set, tried, text);
    } __except (code = GetExceptionCode(), at = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                EXCEPTION_EXECUTE_HANDLER) {
        Log("circuit popup: show faulted: %08lX at %p", code, at);
    }
}

void __fastcall HookPackPopup(void* handler, void* edx, unsigned index)
{
    uint32_t stored = 0;
    SafeReadU32(kStoredRecordCountVa, &stored);
    const auto* record = index < stored ? reinterpret_cast<const uint8_t*>(kStoredRecordsVa) + index * 6
                                        : reinterpret_cast<const uint8_t*>(kRolledItemsVa);
    if (record[3] != kCircuitMarker) {
        g_origPackPopup(handler, edx, index);
        return;
    }
    if (!g_shownCircuit.def && !g_shownCircuit.partName) {
        Log("circuit popup: no circuit delivered yet; not shown");
        return;
    }
    ShowCircuitPopup();
}

// ---- Store categories. ----
// The bench store's categories are (text ID, status mask) pairs, added by FUN_0052e950 through FUN_0051be90 (only when
// some entry's status matches the mask). FUN_00526db0 lists the entries whose status (entry +0x2C, the OfferStatus
// result) matches the current category's mask. Every seal pack has status 4, so all of them land in "resource packs"
// (0xC7B49E15, mask 4), which the game adds only outside co-op. The mod adds its own categories right after that one,
// with the same mask, and while one of them (or "resource packs") is being listed, the other seal packs' status is 2
// for the length of the call. 2 is always in the visible-status mask FUN_005094e0 builds, so the entry indices the
// list keeps do not move, and no category lists status 2 outside co-op. The ownership masks are never touched.
// FUN_0051be90: __thiscall int AddCategory(uint32 textId, uint32 mask), this = the store screen, ret 8.
constexpr uintptr_t kAddCategoryVa = 0x0051BE90;
constexpr unsigned char kAddCategoryBytes[] = {0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x0C, 0x56,
                                               0x57, 0x8B, 0xF1, 0x8B, 0x0D, 0x30, 0x1A, 0x1D};
// FUN_00526db0: __thiscall void ListCategory(), this = the store screen: the current category is +0x388, the
// categories {textId, mask} at *(+0x414), their count at +0x418.
constexpr uintptr_t kListCategoryVa = 0x00526DB0;
constexpr unsigned char kListCategoryBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x53, 0x56,
                                                0x57, 0x8B, 0xF9, 0x33, 0xDB, 0x39, 0x9F, 0x24};
// FUN_00b6a6e0: the text lookup the category names go through, a thunk to LookupText (FUN_00b6a250).
constexpr uintptr_t kCategoryTextVa = 0x00B6A6E0;
constexpr unsigned char kCategoryTextBytes[] = {0x55, 0x8B, 0xEC, 0x5D, 0xE9, 0x67, 0xFB, 0xFF, 0xFF};
constexpr uintptr_t kCurrentCategoryOffset = 0x388;
constexpr uintptr_t kCategoriesOffset = 0x414;
constexpr uintptr_t kCategoryCountOffset = 0x418;
constexpr uintptr_t kEntryStatusOffset = 0x2C;
constexpr uint32_t kResourcePacksCategory = 0xC7B49E15;
constexpr uint32_t kSetAsideStatus = 2;

enum class StoreGroup { None, Resource, Circuits, Schematics, Parts };

struct StoreCategory {
    uint32_t textId;  // the mod's own IDs; the game's string tables do not use them
    const char* text;
    StoreGroup group;
};
constexpr StoreCategory kStoreCategories[] = {
    {0x5EA15C01, "upgrade circuits", StoreGroup::Circuits},
    {0x5EA15C02, "schematics", StoreGroup::Schematics},
    {0x5EA15C03, "weapon parts", StoreGroup::Parts},
};

StoreGroup GroupOfType(uint32_t type)
{
    if (type >= kFirstPartType && type < kFirstPartType + kPartGroups) return StoreGroup::Parts;
    if (type >= kAdvancedSchematicType && type <= kMasterworkSchematicType) return StoreGroup::Schematics;
    if (type >= kCircuitPackType && type <= kMasterworkPackType) return StoreGroup::Circuits;
    if (type >= 1 && type <= 4) return StoreGroup::Resource;
    return StoreGroup::None;
}

// Calls fn(entry) for each store entry that is a seal pack (status 4).
template <typename Fn>
void ForEachSealEntry(Fn fn)
{
    uint32_t manager = 0, count = 0;
    if (!SafeReadU32(kStoreManagerPtrVa, &manager) || !manager || !SafeReadU32(manager + kStoreCountOffset, &count) ||
        count > 64)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t entry = manager + kStoreEntriesOffset + i * kStoreEntryStride;
        uint32_t status = 0;
        if (SafeReadU32(entry + kEntryStatusOffset, &status) && status == kStatusSealPurchasable) fn(entry);
    }
}

bool GroupHasOffers(StoreGroup group)
{
    bool found = false;
    ForEachSealEntry([&](uintptr_t entry) {
        uint32_t type = 0;
        if (SafeReadU32(entry + kEntryTypeOffset, &type) && GroupOfType(type) == group) found = true;
    });
    return found;
}

using AddCategoryFn = int(__fastcall*)(void* screen, void* edx, uint32_t textId, uint32_t mask);
using ListCategoryFn = void(__fastcall*)(void* screen, void* edx);
using CategoryTextFn = const char*(__fastcall*)(void* tables, void* edx, uint32_t id);
AddCategoryFn g_origAddCategory = nullptr;
ListCategoryFn g_origListCategory = nullptr;
CategoryTextFn g_origCategoryText = nullptr;

int __fastcall HookAddCategory(void* screen, void* edx, uint32_t textId, uint32_t mask)
{
    int index = g_origAddCategory(screen, edx, textId, mask);
    if (textId != kResourcePacksCategory || mask != kStatusSealPurchasable || index < 0) return index;
    for (const StoreCategory& c : kStoreCategories) {
        if (GroupHasOffers(c.group)) g_origAddCategory(screen, edx, c.textId, kStatusSealPurchasable);
    }
    return index;
}

// The group the current category stands for: Resource for "resource packs", the mod's own, or None (the game's list
// as is).
StoreGroup CurrentGroup(uintptr_t screen)
{
    uint32_t index = 0, count = 0, categories = 0, textId = 0;
    if (!SafeReadU32(screen + kCurrentCategoryOffset, &index) || !SafeReadU32(screen + kCategoryCountOffset, &count) ||
        !SafeReadU32(screen + kCategoriesOffset, &categories) || index >= count || !categories ||
        !SafeReadU32(categories + index * 8, &textId))
        return StoreGroup::None;
    if (textId == kResourcePacksCategory) return StoreGroup::Resource;
    for (const StoreCategory& c : kStoreCategories) {
        if (c.textId == textId) return c.group;
    }
    return StoreGroup::None;
}

// All three category hooks are in. Without that the hooks made so far pass through: with no categories added, the
// "resource packs" category must keep listing every seal pack.
bool g_categoriesReady = false;

void __fastcall HookListCategory(void* screen, void* edx)
{
    StoreGroup show = CurrentGroup(reinterpret_cast<uintptr_t>(screen));
    uintptr_t setAside[64];
    int n = 0;
    if (g_categoriesReady && show != StoreGroup::None) {
        ForEachSealEntry([&](uintptr_t entry) {
            uint32_t type = 0;
            if (n < 64 && SafeReadU32(entry + kEntryTypeOffset, &type) && GroupOfType(type) != show) {
                *reinterpret_cast<uint32_t*>(entry + kEntryStatusOffset) = kSetAsideStatus;
                setAside[n++] = entry;
            }
        });
    }
    g_origListCategory(screen, edx);
    for (int i = 0; i < n; ++i) *reinterpret_cast<uint32_t*>(setAside[i] + kEntryStatusOffset) = kStatusSealPurchasable;
}

const char* __fastcall HookCategoryText(void* tables, void* edx, uint32_t id)
{
    for (const StoreCategory& c : kStoreCategories) {
        if (c.textId == id) return c.text;
    }
    return g_origCategoryText(tables, edx, id);
}

// Every hook goes through the loader (host->hook), which checks bytes, ranges and claims and enables it at once. The
// pieces that belong together are tried in order, and a feature is only offered once all of its pieces are in;
// hooks made for a feature that ends up unavailable pass straight through to the game.
bool InstallStore()
{
    // The seal purchase and price: the loader lists this mod's own pack types only while it holds both.
    bool buyHooked = CreateHookChecked("BuyWithSeals", kBuyWithSealsVa, kBuyWithSealsBytes, sizeof(kBuyWithSealsBytes),
                                       reinterpret_cast<void*>(&HookBuyWithSeals),
                                       reinterpret_cast<void**>(&g_origBuyWithSeals));
    if (!buyHooked) Log("BuyWithSeals hook unavailable");
    g_sealPriceHooked = CreateHookChecked("SealPrice", kSealPriceVa, kSealPriceBytes, sizeof(kSealPriceBytes),
                                          reinterpret_cast<void*>(&HookSealPrice),
                                          reinterpret_cast<void**>(&g_origSealPrice));

    // Cosmetic; without them the money row and button stay, and the loader's checkout hook still refuses them.
    if (!CreateHookChecked("DescribeOffer", kDescribeOfferVa, kDescribeOfferBytes, sizeof(kDescribeOfferBytes),
                           reinterpret_cast<void*>(&HookDescribeOffer),
                           reinterpret_cast<void**>(&g_origDescribeOffer))) {
        Log("money price and button stay visible");
    }
    if (!(BytesMatch(kAddBackButtonVa, kAddBackButtonBytes, sizeof(kAddBackButtonBytes)) &&
          BytesMatch(kAddButtonVa, kAddButtonBytes, sizeof(kAddButtonBytes)) &&
          CreateHookChecked("AddPcButtons", kAddPcButtonsVa, kAddPcButtonsBytes, sizeof(kAddPcButtonsBytes),
                            reinterpret_cast<void*>(&HookAddPcButtons),
                            reinterpret_cast<void**>(&g_origAddPcButtons)))) {
        Log("money purchase button stays visible");
    }
    if (!(BytesMatch(kGetNumberVa, kGetNumberBytes, sizeof(kGetNumberBytes)) &&
          BytesMatch(kSetBoolVa, kSetBoolBytes, sizeof(kSetBoolBytes)) &&
          BytesMatch(kSetNumberVa, kSetNumberBytes, sizeof(kSetNumberBytes)) &&
          CreateHookChecked("RefreshItem", kRefreshItemVa, kRefreshItemBytes, sizeof(kRefreshItemBytes),
                            reinterpret_cast<void*>(&HookRefreshItem),
                            reinterpret_cast<void**>(&g_origRefreshItem)))) {
        Log("price row stays visible");
    }
    // Cosmetic; without it the cut part is still granted, just not listed in the pack-contents popup. Installed
    // whatever resource_pack_parts says (a live setting): with no part in the record it adds no row.
    if (!(BytesMatch(kNameToGuidVa, kNameToGuidBytes, sizeof(kNameToGuidBytes)) &&
          BytesMatch(kPartFromRecordVa, kPartFromRecordBytes, sizeof(kPartFromRecordBytes)) &&
          BytesMatch(kAddPartRowVa, kAddPartRowBytes, sizeof(kAddPartRowBytes)) &&
          CreateHookChecked("ResourcePopup", kResourcePopupShowVa, kResourcePopupShowBytes,
                            sizeof(kResourcePopupShowBytes), reinterpret_cast<void*>(&HookResourcePopupStub),
                            reinterpret_cast<void**>(&g_resourcePopupResume)))) {
        Log("cut part will not be listed in the pack popup");
    }

    // The circuit packs need every piece; without one of them they are never listed. The [packs] switches are read
    // at each store open, so these hooks go in whatever they say.
    g_circuitPacksReady =
        buyHooked && g_sealPriceHooked && PackFunctionsOk("circuit packs") &&
        BytesMatch(kGetPlayerVa, kGetPlayerBytes, sizeof(kGetPlayerBytes)) &&
        BytesMatch(kGiveItemVa, kGiveItemBytes, sizeof(kGiveItemBytes)) &&
        BytesMatch(kAddResourceVa, kAddResourceBytes, sizeof(kAddResourceBytes)) &&
        CreateHookChecked("ApplyRecord", kApplyRecordVa, kApplyRecordBytes, sizeof(kApplyRecordBytes),
                          reinterpret_cast<void*>(&HookApplyRecord), reinterpret_cast<void**>(&g_origApplyRecord)) &&
        CreateHookChecked("PackPopup", kPackPopupVa, kPackPopupBytes, sizeof(kPackPopupBytes),
                          reinterpret_cast<void*>(&HookPackPopup), reinterpret_cast<void**>(&g_origPackPopup));
    if (g_circuitPacksReady)
        LoadPity();
    else
        Log("circuit packs unavailable");
    // The Schematics also record the circuit as found; the part unlocks also call the game's part grant.
    g_schematicsReady = g_circuitPacksReady && BytesMatch(kRecordFoundVa, kRecordFoundBytes, sizeof(kRecordFoundBytes));
    if (g_circuitPacksReady && !g_schematicsReady) Log("Schematics unavailable");
    g_partsReady = g_circuitPacksReady && BytesMatch(kGrantPartVa, kGrantPartBytes, sizeof(kGrantPartBytes));
    if (g_circuitPacksReady && !g_partsReady) Log("part unlocks unavailable");
    // Cosmetic: frames and attachments a pack gives are marked new. Without the lookups they arrive unmarked.
    g_partItemLookupsOk = true;
    for (uintptr_t fn : kPartItemLookups)
        g_partItemLookupsOk = g_partItemLookupsOk && BytesMatch(fn, kLookupBytes, sizeof(kLookupBytes));
    if (!g_partItemLookupsOk) Log("frame / attachment lookups do not match; packs will not mark them new");

    // Cosmetic: the mod's packs get their own store categories. All three pieces or none; AddCategory (the one that
    // adds them) goes in last, so without it nothing is added, and ListCategory passes through.
    if (g_circuitPacksReady) {
        g_categoriesReady = BytesMatch(kAddCategoryVa, kAddCategoryBytes, sizeof(kAddCategoryBytes)) &&
                            BytesMatch(kListCategoryVa, kListCategoryBytes, sizeof(kListCategoryBytes)) &&
                            BytesMatch(kCategoryTextVa, kCategoryTextBytes, sizeof(kCategoryTextBytes)) &&
                            CreateHookChecked("CategoryText", kCategoryTextVa, kCategoryTextBytes,
                                              sizeof(kCategoryTextBytes), reinterpret_cast<void*>(&HookCategoryText),
                                              reinterpret_cast<void**>(&g_origCategoryText)) &&
                            CreateHookChecked("ListCategory", kListCategoryVa, kListCategoryBytes,
                                              sizeof(kListCategoryBytes), reinterpret_cast<void*>(&HookListCategory),
                                              reinterpret_cast<void**>(&g_origListCategory)) &&
                            CreateHookChecked("AddCategory", kAddCategoryVa, kAddCategoryBytes,
                                              sizeof(kAddCategoryBytes), reinterpret_cast<void*>(&HookAddCategory),
                                              reinterpret_cast<void**>(&g_origAddCategory));
        if (!g_categoriesReady) Log("store categories unavailable; all seal packs stay under resource packs");
    }

    // Last: ask the loader to list the offers. It installs its money-checkout refusal first and lists nothing without
    // it; it lists types 5+ only while this mod holds the BuyWithSeals and SealPrice hooks made above.
    if (!g_host->sealOffers(g_host, &ListOffers, nullptr)) {
        Log("the loader refused to list seal offers (see ds3_mods.log); the store stays the stock one");
        return false;
    }
    Log("offers registered with the loader; it refuses any money checkout for them");
    if (PriceOverride()) {
        const Config& c = g_config;
        Log("*** TEST MODE: seal price override active (Resource %d, Ultra %d, Epic %d, Upgrade Circuit Pack %d, Elite "
            "Upgrade Circuit %d, Masterwork Circuit Pack %d, Schematics %d / %d / %d, MK-V parts %d, MK-II parts %d; 0 = "
            "normal price). Not for normal play. ***",
            c.resourcePrice, c.ultraPrice, c.epicPrice, c.circuitPrice, c.eliteCircuitPrice, c.masterworkPrice,
            c.advancedSchematicPrice, c.eliteSchematicPrice, c.masterworkSchematicPrice, c.mkvPartPrice,
            c.mkiiPartPrice);
    }
    if ((g_config.pityElitePacks > 0 || g_config.pityMasterworkPacks > 0) && g_circuitPacksReady) {
        Log("*** TEST MODE: pity override active (Elite by pack %d, Masterwork by pack %d; 0 = normal %d / %d). Not "
            "for normal play. ***",
            g_config.pityElitePacks, g_config.pityMasterworkPacks, kElitePityPacks, kMasterworkPityPacks);
    }
    if (g_config.keepMoneyButton) Log("*** TEST MODE: keep_money_button=1, the money row and button stay. ***");
    return true;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl DS3ModInit(const DS3ModHost* host)
{
    g_host = host;
    LogInit(host->modDir);
    // The version comes from mod.ini, the one place it is written.
    wchar_t manifest[MAX_PATH], version[16] = L"?";
    if (swprintf_s(manifest, L"%s\\mod.ini", host->modDir) > 0)
        GetPrivateProfileStringW(L"mod", L"version", L"?", version, 16, manifest);
    Log("Ration Seal Store %ls on DS3 Mod Loader %s (API %u)", version, host->proxyVersion ? host->proxyVersion : "?",
        host->apiVersion);
    if (host->apiVersion < 3 || !DS3MOD_HAS(host, sealOffers)) {
        Log("this loader is too old: the store needs DS3 Mod Loader 1.0.0 or later (API 3); nothing installed");
        host->log(host, "needs DS3 Mod Loader 1.0.0 or later (API 3); nothing installed");
        return 1;
    }
    swprintf_s(g_config.statePath, L"%s\\ds3_seals_state.ini", host->gameDir);
    ReadSettings();
    const bool ok = InstallStore();
    host->log(host, ok ? "Ration Seal Store ready; details in plugins\\SealStore\\seal_store.log"
                       : "Ration Seal Store NOT installed; see plugins\\SealStore\\seal_store.log");
    return ok ? 0 : 1;
}

// After the Mods panel (or an edit in Notepad) changed seal_store.ini: the [packs] switches apply at the next store
// open, the [debug] values at once.
extern "C" __declspec(dllexport) void __cdecl DS3ModSettingsChanged(void)
{
    if (!g_host) return;
    ReadSettings();
    Log("settings re-read: resource_pack_parts=%d circuit_packs=%d schematics=%d part_unlocks=%d",
        g_config.resourcePackParts, g_config.circuitPacks, g_config.schematics, g_config.partUnlocks);
}
