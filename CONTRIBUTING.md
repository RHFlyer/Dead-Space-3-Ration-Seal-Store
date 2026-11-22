# Contributing to the Ration Seal Store

Thanks for wanting to help! You don't need to be an expert, and you don't need to ask before you start.

## Report a bug

[Open an issue](https://github.com/RHFlyer/Dead-Space-3-Ration-Seal-Store/issues) and say what you did, what you expected and what happened instead. Add the
Ration Seal Store's version and the DS3 Mod Loader's: the Mods panel shows the store's version under its switch;
in `ds3_mods.log`, the last line that contains "DS3 Mod Loader v" shows the loader's. Attach
`plugins\SealStore\seal_store.log` and `ds3_mods.log` from the game folder. Read a log before you post it.

A bug in the loader itself (the Mods panel, loading mods) belongs on the
[DS3 Mod Loader's issues](https://github.com/RHFlyer/Dead-Space-3-Mod-Loader/issues).

## Send a fix

1. Fork the repo and make a branch for your change.
2. Build it (see "Building from source" in [`README.md`](README.md)) and test it in the game.
3. Open a pull request and say what you changed and how you tested it.

Leave `ds3mod.h` as it is: it is pinned to a loader release, and the build check refuses a changed copy. Using AI to
help you is fine. Just say so in the pull request, and test what you send.

## The rules

The DS3 Mod Loader's rules apply here too, and pull requests that break them can't be accepted:

- **Seals only.** Nothing in this mod may be sold for real money, and it never changes the game's real offers.
- **Never touch paid DLC, entitlements, ownership, logins or the game's copy protection (DRM).**
- **No game files** and **no personal data** in the repo.

By sending a pull request you agree that your change is shared under the project's MIT licence
([`LICENSE.txt`](LICENSE.txt)).

Thanks!
RHFlyer
