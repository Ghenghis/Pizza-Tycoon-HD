# Save versions: the per-slot manifest

Persistence lane, delivery step 2 of COMMANDS-AND-RECOVERY.md. Code: `src/core/save/SaveVersions.{h,cpp}`
(files, manifest, retention) and `SaveGameManager::save_to_slot`, `load`, `load_over_current`,
`checkpoint`, `restore_version`, `versions` and `delete_save`.

## Layout

A slot is a save file in the saves folder, such as `MYGAME.plsave`. Its versions live beside it:

```
saves/
  MYGAME.plsave                  the slot's file: a copy of its active version
  MYGAME.plsave.versions/
    manifest.json                which version is which
    v000012.plsave               one file per version, named by its sequence number
    v000013.plsave
    v000014.plsave
```

- A version file is a complete save in the normal format. It is never rewritten: a new save gets the
  next sequence number. Names are `v` plus at least six digits, without extra leading zeros, plus
  `.plsave`.
- The slot's file stays where the load menu, `read_save_metadata`, older builds and players look.
  The manifest decides what loading gives, not the slot's file.
- The load and save menus list regular `.plsave` files only, so the folder never shows up in them.
- The startup temp-file cleanup also looks in every `*.plsave.versions` folder.
- `save()` still writes one plain file and keeps no versions. It is for tools and tests. The game
  saves with `save_to_slot`, and the autosave uses it too.

## manifest.json (format 1)

```json
{
  "format": "plegacy-save-versions",
  "format_version": 1,
  "slot": "MYGAME.plsave",
  "next_sequence": 15,
  "active": 14,
  "versions": [
    {
      "sequence": 14,
      "status": "working",
      "level": "load-verified",
      "reason": "",
      "made_for": "save",
      "written_at": "2026-11-14T09:30:12Z",
      "size": 18234,
      "checksum": "crc32:0a1b2c3d",
      "fingerprint": "84c8e5a930faab92",
      "game_date": "Sat Mar  4 1995 09:00"
    }
  ],
  "retired": [11]
}
```

| Field | Meaning |
|---|---|
| `format`, `format_version` | Always these. A build refuses a manifest with a newer `format_version` and leaves the folder alone. It then saves the slot's file only, the way builds before versions did. |
| `slot` | The slot's file name. For people reading the file only; it is not checked. |
| `next_sequence` | One more than the newest sequence the slot has used. A save also skips past any version file already on disk, listed or not. |
| `active` | The working version that the slot's file holds and that loading tries first, or `null`. Always a working version. |
| `previous_active` | The version that was active before the last `restore_version`, for `undo_restore`. Optional: older manifests don't have it, and one that names no working version is ignored. |
| `versions` | Newest first. At most 64 entries are read. |
| `retired` | Versions dropped from the list whose files may still exist. Each save removes those files first, so pruning picks up where it stopped. |

Each version:

| Field | Meaning |
|---|---|
| `sequence` | At least 1 and below `next_sequence`; unique. The file is `v` + the sequence. |
| `status` | `working`: it passed every check and may be used. `candidate`: it did not pass, and `reason` says why. |
| `level` | The furthest check passed: `written`, `integrity-checked`, `load-verified`. A working version is always `load-verified`. (`gameplay-verified` is for delivery step 3.) |
| `reason` | Why a candidate is not working, e.g. "it is 512 bytes on disk, not the 18234 written". It is "being written" while a save is under way. Empty for a working version. |
| `made_for` | `save`, `autosave`, `checkpoint before a new game`, or `checkpoint before loading a save`. |
| `written_at` | UTC time, to the second. |
| `size`, `checksum` | The file's byte count and CRC-32 (`crc32:` plus 8 lowercase hex digits). |
| `fingerprint` | `SaveGameManager::fingerprint` of the game that was saved: FNV-1a over its state, minus the save time, as 16 hex digits. |
| `game_date` | The game's date, as the load menu shows it. |

Anything else in the file, a missing field, a wrong type or a broken rule makes the whole manifest
unreadable. An unreadable manifest is logged and no file in the folder is deleted. Loading then
tries the slot's file and every version file, newest first. The next save starts a new manifest
and leaves the version files it does not list where they are.

## Retention

After each save, `apply_retention` keeps:

- the newest three working versions;
- the active version, however old;
- the newest candidate, but only while no working version is newer.

Everything else moves to `retired`. A save that fails its checks can only drop an older candidate,
never a working version.

## Saving: `save_to_slot`

1. List the new version in the manifest as a candidate, "being written", and write the manifest.
   After this, a save cut short anywhere leaves nothing the manifest does not account for.
2. Write the version file through AtomicFile, under its own new name. No working version is
   touched.
3. **Integrity check.** Read the file back. Its size and CRC-32 must match the bytes written, and
   its header and every size in it must hold up.
4. **Load check.** Decode, migrate and validate the file. Build it into a scratch `GameState`, the
   way `load` does. Its fingerprint must equal the saved game's. The shared random generator is put
   back afterwards.
5. Set the status, level and reason. A working version becomes `active`. Apply retention, then
   write the manifest. This write is the commit.
6. A working version is also written over the slot's file. If that fails, loading still takes the
   new version from the manifest.
7. Remove the retired files.

A failure in steps 1 to 4 returns false with a message for the player, and the slot loads what it
loaded before.

## Loading: `load`

It tries copies in this order, and loads the first one that passes every check:

1. The slot's file, if its bytes match no listed version. That means it was written outside the
   versions: by `save()`, by an older build, or by hand. Or it is damaged.
2. Each working version, the active one first and then newest first. Each comes from its own file,
   and then from the slot's file if that holds the same bytes. A copy whose size or checksum
   differs from the manifest is refused before it is decoded.
3. Version files the manifest does not list.

A slot without a versions folder loads exactly as before. If the copy loaded is not the first
copy, and not the same version from its other file, the caller gets a notice ("…so the game loaded
the verified version of it saved 2026-11-14 09:30 UTC (…)."). A warning also goes to the status
line, and the log names both files.

## Checkpoints: `checkpoint`, `load_over_current`

The hook the game calls before something replaces the game in progress. It saves that game, through
`save_to_slot`, as a new version of `CHECKPNT.plsave`.

- It returns `NoGame` when there is no game in progress: a player slot is empty, or player 1 is not
  a person, as in the menus' placeholder state.
- It returns `Unchanged`, and writes nothing, when the newest working checkpoint has the same
  fingerprint as the game.

`load_over_current` runs the checkpoint once the save to load has passed every check, and just
before it replaces the game. The save is already read by then, so loading `CHECKPNT` itself still
gives what it held when the player chose it. A failed checkpoint does not stop the load; the notice
says so.

## Restoring: `restore_version`

Makes a working version the active one:

1. Check the version again: the integrity check against its manifest entry, then the load check
   against its fingerprint. A version that fails becomes a candidate, with the reason, and nothing
   else changes.
2. Write the manifest with the new `active`.
3. Write the slot's file.

If the restore is cut short between steps 2 and 3, loading already takes the restored version.
Every other version stays, so restoring the previous one undoes a restore. The running game is not
touched.

## Timed checkpoints: `TimedCheckpoints`, `start_timed_checkpoint`, `promote_checkpoint`

The game calls `TimedCheckpoints::frame` every frame, telling it whether the game itself ran.
`CheckpointTimer` asks for a checkpoint after five minutes of play, and when a game day or a game
week begins. A week counts over a day, and a boundary over the interval. They go to the slot
`AUTOCHK.plsave`, which nothing else writes. One job runs at a time.

- **On the game thread, at a consistent tick:** `SaveGameManager::capture`, which is
  `state_to_json` and nothing more.
- **On the job's worker:**
  - bring the slot's file up to the active version;
  - fingerprint the captured game. If the slot's newest version, working or awaiting promotion,
    has the same fingerprint, stop: the job reports `unchanged()`;
  - otherwise encode, list, write and integrity-check the version (steps 1 to 3 above);
  - build it apart from the game, with the game's options and an empty context, and check its
    fingerprint;
  - commit it as a candidate at `load-verified`, with the reason `awaiting promotion`, and write
    the slot's file.

  A failure leaves it a candidate with its reason. Until promotion, loading still takes the active
  version, since this one is listed only as a candidate.
- **On the game thread, once the job is done:** `promote(job)` makes it working and active. That is
  one small manifest write.

### Autosave

The autosave takes the same path. The turn pipeline's Autosave step calls
`autosave_in_background`, which captures the game and starts the worker. It writes to the next
`AUTOSAVn` slot and never coalesces.

`TimedCheckpoints::frame` calls `settle_background_saves(state)`, which promotes finished autosaves.
For one that failed, it posts "Autosave failed! Save by hand." as before. `finish()` waits for
whatever is still running and promotes it as the game closes.

Anything on the game thread that touches a slot first waits for a background autosave to that slot
and promotes it: `save_to_slot`, `load`, `restore_version`, `restore_as_branch` and `delete_save`.
So there is never a second writer.

The synchronous `autosave()` is kept. A manual Save stays synchronous behind the busy cursor.

What makes the build safe on a worker:

- `engine::random`'s simulation and presentation streams are `thread_local`. A worker starts at 1,
  as the program does, and its `set_state` and draws never touch the game's sequence.
- The factories' tables are filled once and then only read:
  - ingredients, suppliers, furniture, trainings and floor patterns are filled whole under
    `FillOnce` (`std::call_once`, behind an atomic flag);
  - each character is made once in its own slot, published through an atomic pointer.

  Reads take no lock.
- The build gets its own `GameContext`, so nothing in it can reach the game's managers.

The hidden `[bench][checkpoint]` case measures what each step costs.

## Recovery Center model

- `recovery_slots(folder)` lists every slot, sorted by name: its file if it has one, its versions
  with their status and level, and why a manifest could not be read. Checkpoint slots
  (`CHECKPNT`, `AUTOCHK`) are flagged.
- `restore_as_branch(slot, n, new_slot)` checks version `n` again apart from the game. It then
  writes a new slot: its version 1, its manifest, its file. The original slot is never touched. A
  name that is already a save is refused. A failure partway removes the new slot.
- `undo_restore(slot)` restores `previous_active`. Undoing twice goes back again.

## Fault tests

`tests/SaveVersionTests.cpp` uses `util::testing::ScopedAtomicFault`, a test-only seam in
`AtomicFile`. It works on real files and makes chosen writes do one of four things:

- fail partway with "disk full";
- succeed but lose the end of the file;
- fail the rename;
- stop before the rename, as a crash would. This throws `AtomicFileInterrupted` and leaves the
  temporary file behind.

The game never installs a fault.
