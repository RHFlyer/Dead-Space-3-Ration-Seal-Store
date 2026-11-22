/* ds3mod.h: what a drop-in DLL mod can ask the DS3 Mod Loader (dinput8.dll, "the proxy") to do for it. API version 3.
 *
 * You only need this one file. Include it, export DS3ModInit, and the proxy hands you a DS3ModHost: a table of
 * functions that read the game, draw on screen, and change the game's code safely. sdk\README.md explains every one
 * in plain words, with a walkthrough from the sample mod to your own.
 *
 * A mod is a 32-bit DLL (file name ending in .asi or .dll) in its own folder, plugins\<YourMod>\, next to a
 * mod.ini manifest (see sdk\README.md). The proxy loads it after the game's code is decrypted, on the game's first
 * DirectInput8Create, and only while its [modules] switch in ds3_mods.ini is on.
 *
 * Nothing here is required: a plain DLL with a DllMain loads and runs too. But DllMain runs under the loader lock,
 * where installing hooks or starting threads that wait is unsafe, so do the real work in DS3ModInit instead.
 * Both exports are plain C (__cdecl), so they keep their names with no .def file:
 *
 *   extern "C" __declspec(dllexport) int __cdecl DS3ModInit(const DS3ModHost* host);
 *       Called once, right after the DLL is loaded, on the game's thread, outside the loader lock. Return 0 if the
 *       mod is fine; anything else is logged as a failure (the DLL stays loaded: the proxy cannot know what is safe
 *       to undo). `host` and everything it points to stay valid until the game exits.
 *
 *   extern "C" __declspec(dllexport) void __cdecl DS3ModSettingsChanged(void);
 *       Called after the in-game Mods panel wrote one of the mod's settings to its ini. Re-read the ini here and the
 *       panel says "applied now" for your settings instead of "applies after restart". Runs on the game's render
 *       thread, inside a frame: keep it short and do not block.
 *
 *   extern "C" __declspec(dllexport) void __cdecl DS3ModShutdown(void);
 *       Optional (API 2.1). Exporting it says "this mod can be swapped for a new build while the game
 *       runs": the proxy then loads the DLL from a copy in %TEMP%\ds3mods, so your build can overwrite the file in
 *       plugins\<YourMod>\, and when that file changes it calls DS3ModShutdown on the game's thread, unloads the
 *       old build and loads the new one (DS3ModInit again, with the same host). Before calling it, the proxy has
 *       already stopped your frame callback and switched off every hook made with `hook`, and waited until none of
 *       them is running; afterwards it removes those hooks and puts back the bytes of every `patch`.
 *       What you must do in it: let go of everything else, because the old build is never called again (the proxy
 *       leaves it mapped until the game exits, or frees it with [mods] hot_reload_free=1). Stop every thread
 *       you started AND WAIT for it to end; undo anything you changed yourself (your own hooks, VirtualProtect
 *       writes, vtable slots, window procedures, timers); take back any pointer to your code or data that you gave
 *       the game. Pointers you hold to game objects are fine. The new build starts with fresh globals.
 *       Because the DLL runs from a copy, find your files through host->modDir / host->iniPath, never through
 *       GetModuleFileName.
 *
 * Versions. The proxy fills `size` and `apiVersion`. New fields are only ever added at the END of DS3ModHost, so a
 * mod built against an older ds3mod.h keeps working with a newer proxy. A mod built against this file that runs on an
 * OLDER proxy gets a shorter table: check `host->apiVersion >= 2` (or DS3MOD_HAS below) before using a v2 field.
 *
 *   API 1:   size .. inputBlocked
 *   API 2:   hook, patch, gameState, localPlayer, getResource, setResource, resourceName, itemCount, getItem,
 *            itemName, onFrame
 *   API 2.1: no new fields (apiVersion is still 2); the optional export DS3ModShutdown (hot reload), and settings
 *            ini edits made outside the panel reach DS3ModSettingsChanged
 *   API 3:   sealOffers (sell things in the bench store for Ration Seals, never for money); DS3Frame.setFont and
 *            DS3Frame.gradient (check DS3FRAME_HAS: test builds before 1.0.0 called themselves API 3 without them)
 *   DS3 Mod Loader 1.0.0 has all of these (API 3).
 *
 * Hard lines, for every mod made with this SDK: never change paid DLC, entitlements, ownership, or the DRM wrapper,
 * and never read the process environment or memory you don't need (it holds EA login tokens). hook and patch refuse
 * the areas of the game known to hold that code, but that list is not complete: the rule stands on its own.
 */
#ifndef DS3MOD_H
#define DS3MOD_H

#include <stddef.h> /* offsetof */
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS3MOD_API_VERSION 3

/* True if the proxy's table is long enough to have `field` (it may be older than this header). */
#define DS3MOD_HAS(host, field) ((host)->size >= offsetof(DS3ModHost, field) + sizeof((host)->field))
/* The same for the frame your frame callback gets, e.g. DS3FRAME_HAS(frame, setFont). */
#define DS3FRAME_HAS(frame, field) ((frame)->size >= offsetof(DS3Frame, field) + sizeof((frame)->field))

/* ---- Small types ---- */

/* Colours are 0xAARRGGBB: alpha (255 = solid, 0 = invisible), red, green, blue. DS3_RGBA(255, 200, 0, 255) = gold. */
#define DS3_RGBA(r, g, b, a) \
    ((unsigned)(((unsigned)(a) & 0xFF) << 24 | ((unsigned)(r) & 0xFF) << 16 | ((unsigned)(g) & 0xFF) << 8 | \
                ((unsigned)(b) & 0xFF)))
#define DS3_WHITE 0xFFFFFFFFu
#define DS3_BLACK 0xFF000000u

/* What the game is doing right now (gameState, DS3Frame.state). */
#define DS3_STATE_NO_PLAYER 0 /* main menu or a loading screen: there is no player to read */
#define DS3_STATE_PLAYING 1   /* in a level, with a player */
#define DS3_STATE_PAUSED 2    /* in a level, with the pause menu open */

/* The six resources (getResource, setResource, resourceName). */
#define DS3_TUNGSTEN 0
#define DS3_SEMICONDUCTOR 1
#define DS3_SCRAP_METAL 2
#define DS3_SOMATIC_GEL 3
#define DS3_TRANSDUCER 4
#define DS3_RATION_SEALS 5
#define DS3_RESOURCE_COUNT 6

/* The player's two item lists (itemCount, getItem). */
#define DS3_ITEMS_CONSUMABLES 0   /* ammo, med and stasis packs, and some mission items (e.g. "Probe Frame") */
#define DS3_ITEMS_KEY_AND_PARTS 1 /* suits, weapon parts, circuits, key items */

/* The game's id for a kind of item (a GUID). Two stacks of the same item have the same id. */
typedef struct DS3Guid {
    unsigned d[4];
} DS3Guid;

/* One entry of an item list (getItem). */
typedef struct DS3Item {
    DS3Guid id;
    unsigned count; /* how many are in this stack */
    char name[64];  /* the game's own name for it, e.g. "AMMO CLIP"; "" if it has none. Printable ASCII only. */
} DS3Item;

/* ---- Drawing: what onFrame's callback gets ---- */

/* Passed to your frame callback (onFrame) once per frame, on the game's render thread, which is the game's own main
 * thread. Its drawing functions only work inside that callback: they add to what the proxy draws over the game at the
 * end of the frame, so nothing you draw stays on screen by itself. Draw it again every frame you want it shown.
 * Coordinates are pixels: (0, 0) is the top-left corner of the screen, (width, height) the bottom-right. */
typedef struct DS3Frame {
    unsigned size;       /* sizeof(DS3Frame) as the proxy built it; fields are only ever added at the end */
    float width, height; /* the screen (the game's back buffer), in pixels */
    unsigned timeMs;     /* a millisecond clock (GetTickCount); use the difference between two frames */
    float deltaSeconds;  /* seconds since the previous frame; 0 on the first */
    int state;           /* DS3_STATE_*, as gameState() would return now */

    /* Draws `text` with its top-left corner at (x, y) in `color`. Returns the x where the text ends, so you can draw
     * more after it. Plain ASCII; anything else shows as '?'. */
    float(__cdecl* text)(const struct DS3Frame* frame, float x, float y, const char* text, unsigned color);
    /* Draws a filled box. Use an alpha below 255 for a see-through background, e.g. DS3_RGBA(0, 0, 0, 160). */
    void(__cdecl* rect)(const struct DS3Frame* frame, float x, float y, float w, float h, unsigned color);
    /* How wide `text` would be, in pixels, at the current text size. */
    float(__cdecl* textWidth)(const struct DS3Frame* frame, const char* text);
    /* Sets the text size (letter height in pixels, 8 to 160) for the text drawn after it, this frame. Returns the line
     * height: add it to y for the next line. Each frame starts at a size that suits the screen (about height / 54).
     * The proxy keeps up to 8 fonts (a face at a size) per frame, for all mods together; ask for more and you get the
     * nearest one it has. */
    float(__cdecl* setTextSize)(const struct DS3Frame* frame, float px);
    /* The line height at the current text size. */
    float(__cdecl* lineHeight)(const struct DS3Frame* frame);

    /* -- Added in DS3 Mod Loader 1.0.0. Check DS3FRAME_HAS(frame, setFont) before using either. -- */

    /* Chooses the font for the text drawn after it, this frame: `face` is the name of a font installed in Windows,
     * e.g. "Consolas" or "Bahnschrift SemiCondensed" (NULL or "" = the proxy's usual face), and `px` the letter
     * height, as in setTextSize. A named face is drawn at normal weight; if Windows doesn't have it, the usual face
     * is used instead. Returns the line height. setTextSize afterwards keeps the face. Each frame starts with the
     * usual face. */
    float(__cdecl* setFont)(const struct DS3Frame* frame, const char* face, float px);
    /* Draws a filled box whose colour runs from `from` to `to`: left to right, or top to bottom if `vertical` is not
     * 0. With alpha 0 at one end, the box fades out towards it. */
    void(__cdecl* gradient)(const struct DS3Frame* frame, float x, float y, float w, float h, unsigned from,
                            unsigned to, int vertical);
} DS3Frame;

/* Your frame callback. `user` is whatever you passed to onFrame. */
typedef void(__cdecl* DS3FrameFn)(const DS3Frame* frame, void* user);

/* ---- Seal offers: what sealOffers' callback fills in (API 3) ---- */

/* The game's own seal-pack types (DS3SealOffer.packType). The game prices, rolls and gives these itself. */
#define DS3_PACK_RESOURCE 2  /* Resource Pack (10 seals) */
#define DS3_PACK_ULTRA 3     /* Ultra Weapon and Resource Pack (30 seals) */
#define DS3_PACK_EPIC 4      /* Epic Weapon and Resource Pack (60 seals) */
#define DS3_PACK_FIRST_MOD 5 /* 5 .. 63: your own types; see sealOffers for what they need */
#define DS3_PACK_LAST_MOD 63

/* The game's seal purchase and seal price functions (addresses as Ghidra shows them). sealOffers lists your own pack
 * types only while your mod holds hooks on both. */
#define DS3_ADDR_BUY_WITH_SEALS 0x0051DD70u
#define DS3_ADDR_SEAL_PRICE 0x00513B00u

/* One offer for the bench store's catalogue. It is sold for Ration Seals only: the proxy fills every money field
 * itself (no currency, price 0, not owned) and refuses any money checkout for it. */
typedef struct DS3SealOffer {
    /* "DS3SEALS:" followed by A-Z, 0-9 or _ (at most 47 characters in all), e.g. "DS3SEALS:MY_PACK". Or exactly
     * "OFB-EAST:109546252" / "OFB-EAST:109546253", the game's own Ultra / Epic seal packs, which the Steam
     * catalogue leaves out. Every other ID is refused, so a mod can never list a real (paid) offer. */
    const char* id;
    const char* name;        /* the list text; copied (up to 63 characters) */
    const char* description; /* the detail text; copied (up to 199; the store shows about 40 per line) */
    int packType;            /* DS3_PACK_RESOURCE / _ULTRA / _EPIC, or one of your own, 5 .. 63 */
} DS3SealOffer;

/* Called each time the bench store opens, on the game's thread. Fill out[0 .. max - 1] and return how many you filled
 * (0 = none this time). The strings only need to stay valid until you return: the proxy copies them. */
typedef int(__cdecl* DS3SealOffersFn)(DS3SealOffer* out, int max, void* user);

/* ---- The host table ---- */

typedef struct DS3ModHost {
    /* ==== API 1 ==== */
    unsigned size;            /* sizeof(DS3ModHost) as the proxy built it; fields are only ever added at the end */
    unsigned apiVersion;      /* DS3MOD_API_VERSION of the proxy */
    const char* proxyVersion; /* e.g. "1.0.0" */
    const char* modId;        /* the mod's [modules] key in ds3_mods.ini */
    const wchar_t* modDir;    /* full path of plugins\<YourMod>, no trailing backslash */
    const wchar_t* iniPath;   /* full path of the mod's settings ini (mod.ini's `ini=`), or L"" if it has none */
    const wchar_t* gameDir;   /* the game folder (where deadspace3.exe and dinput8.dll are) */
    /* Writes one line to ds3_mods.log, prefixed with the mod's id. Any thread. Plain text, no format string. */
    void(__cdecl* log)(const struct DS3ModHost* host, const char* text);
    /* Nonzero while the proxy's Mods panel (or its "mods" pause button) has the keyboard, mouse and pad: the game
     * gets no input then, and it stays so until every key that was used is let go. The proxy can only keep input from
     * the game, not from a mod that reads the keyboard itself (GetAsyncKeyState, raw input, its own hooks), so check
     * this before acting on a hotkey. Any thread. */
    int(__cdecl* inputBlocked)(void);

    /* ==== API 2 ==== */

    /* -- Changing the game's code. Both return 1 if done, 0 if refused; the reason is always in ds3_mods.log. --
     * Addresses are as Ghidra shows them for deadspace3.exe (e.g. 0x006CED10). Both refuse, like patch mods do:
     * anything before the game's code is decrypted; anything outside the exe's .text / .rdata / .data (hook: .text
     * only); the store, entitlement, DLC, login and Origin code; anything within 64 bytes of text about
     * entitlements, DLC ownership or logins; and bytes another mod, patch or proxy hook already holds. Each
     * success is logged with your mod's id and the exact byte range. Call them from DS3ModInit, ideally. */

    /* Hooks the game function at `address`: from then on the game calls `detour` instead, and `*original` is set to
     * a function that runs the game's own code (call it from your detour to keep the game's behaviour). `expect`
     * must be the function's first `expectLen` bytes (5 to 64) in an unchanged game; if they differ (another game
     * version, or someone else changed them) nothing is done. `detour` must have exactly the game function's calling
     * convention and arguments. `name` is only for the log. The proxy uses MinHook and holds the first 16 bytes. */
    int(__cdecl* hook)(const struct DS3ModHost* host, const char* name, unsigned address, const void* expect,
                       unsigned expectLen, void* detour, void** original);
    /* Replaces `len` bytes (1 to 64) at `address`: if the game has `expect` there, `write` is written. To undo, call
     * it again with `expect` and `write` swapped. Calling it when `write` is already there is fine (returns 1). */
    int(__cdecl* patch)(const struct DS3ModHost* host, const char* name, unsigned address, const void* expect,
                        const void* write, unsigned len);

    /* -- Reading the game. Call these from your frame callback (the game's own thread), where the player cannot
     * vanish halfway through a read. Each one checks the game's bytes before its first use and is crash-proof; on a
     * different game version they report "nothing" instead of crashing. -- */

    /* DS3_STATE_NO_PLAYER, DS3_STATE_PLAYING or DS3_STATE_PAUSED. */
    int(__cdecl* gameState)(void);
    /* The local player's object in game memory, or NULL on the main menu and loading screens. Only for your own
     * research: what is known of its layout is in sdk/GAME_NOTES.md. For resources and items, use the calls below. */
    void*(__cdecl* localPlayer)(void);
    /* Reads resource `which` (DS3_TUNGSTEN .. DS3_RATION_SEALS) into *value. Returns 1, or 0 with no player. */
    int(__cdecl* getResource)(int which, unsigned* value);
    /* Sets resource `which` to `value`, the same way the game itself adds and takes resources. Returns 1, or 0 with
     * no player. The game saves it with your next save. Logged (the first few changes per mod). Only the local
     * player; not tried in co-op. */
    int(__cdecl* setResource)(const struct DS3ModHost* host, int which, unsigned value);
    /* "Tungsten", "Semiconductor", "Scrap Metal", "Somatic Gel", "Transducer", "Ration Seals"; "" if out of range. */
    const char*(__cdecl* resourceName)(int which);
    /* How many entries list `list` (DS3_ITEMS_*) has now; 0 with no player. One item can fill several entries. */
    int(__cdecl* itemCount)(int list);
    /* Copies entry `index` (0 .. itemCount - 1) of list `list` into *out, with its name. Returns 1, or 0 if there is
     * no such entry. Read-only: items can't be changed through the SDK. */
    int(__cdecl* getItem)(int list, int index, DS3Item* out);
    /* The game's name for item `id` into buf (at most size - 1 characters). Returns 1, or 0 with buf = "". */
    int(__cdecl* itemName)(const DS3Guid* id, char* buf, unsigned size);

    /* -- Drawing --
     * Registers `fn` to be called once per frame, with `user` passed back to it. One callback per mod: calling it
     * again replaces the first, and fn = NULL stops it. Returns 1. If your callback crashes, the crash is caught,
     * logged, and the callback is switched off for the rest of the game. */
    int(__cdecl* onFrame)(const struct DS3ModHost* host, DS3FrameFn fn, void* user);

    /* ==== API 3 ==== */

    /* -- Selling things in the bench store for Ration Seals --
     * The store's catalogue and checkout code is off limits to hook and patch; this is the one way in, and it can
     * only ever add offers that cost Ration Seals. Before any offer can be shown, the proxy installs its own hook on
     * the game's money checkout, which refuses every checkout that includes a DS3SEALS: offer, either stock seal
     * pack, or any offer a mod listed. If that refusal can't be installed, nothing is installed and this returns 0.
     * The refusal stays in until the game exits, even if your mod is unloaded.
     *
     * Registers `fn`, which the proxy calls each time the store opens to ask which offers to list. One callback per
     * mod: calling again replaces the first, fn = NULL stops it. Returns 1, or 0 if refused (the log says why). Each
     * offer is checked (the ID rule in DS3SealOffer, not already in the catalogue, readable strings); a bad one is
     * skipped with a log line. Your own pack types (5 .. 63) are listed only while your mod holds hooks, made with
     * `hook`, on both DS3_ADDR_BUY_WITH_SEALS and DS3_ADDR_SEAL_PRICE: for a type it doesn't know, the game would
     * take the seals and give nothing, so without your purchase hook the offer is left out. If `fn` crashes, the
     * crash is caught and logged and your offers are switched off. */
    int(__cdecl* sealOffers)(const struct DS3ModHost* host, DS3SealOffersFn fn, void* user);
} DS3ModHost;

typedef int(__cdecl* DS3ModInitFn)(const DS3ModHost* host);
typedef void(__cdecl* DS3ModSettingsChangedFn)(void);
typedef void(__cdecl* DS3ModShutdownFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* DS3MOD_H */
