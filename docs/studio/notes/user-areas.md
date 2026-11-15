# User areas: the store, saves and Duplicate layout

Persistence for areas the player creates (WORLD-BUILDER-EXPANSION.md). The code is
`src/core/save/UserAreaStore.{h,cpp}` and `AreaLayout.{h,cpp}`. The catalog
(`src/core/content/AreaCatalog`) reads the store through `list()` and `admitted()`. The generator
gets its namespace from `installation_namespace()`.

## Folder

The store lives under `<base dir>/content/areas/`. Nothing in the shipped data folder is written.

```
installation.json            {"namespace": "u-3f9a1c2e"}: made once, kept
index.json                   the index, with three verified versions (.versions/)
areas/area-000017.draft.json the draft being edited (+ .versions/)
areas/area-000017.json       the admitted document games play (+ .versions/)
```

Every file is written through util::AtomicFile. Every document and the index keep their last three
verified versions, with the manifest machinery of save slots (SaveVersions.h, `.json` version
files). A version is working only if two checks pass:

- **integrity:** its bytes on disk are the bytes written, and they parse;
- **document check:**
  - a draft: a location document with a key;
  - an admitted document: `read_location` and `validate_location` pass with no errors, and its
    integrity hash is its own.

A damaged version falls back to the one before.

## index.json

```json
{"format": "plegacy-user-areas", "format_version": 1, "next_id": 18,
 "areas": [{"id": "area-000017", "location_key": "u-3f9a1c2e:north-pole-2",
            "name": "Frost Hollow", "territory": "north-pole", "order": 0,
            "state": "admitted", "revision": 2, "sha256": "...",
            "admitted_version": 5,
            "created_utc": "...", "updated_utc": "..."}]}
```

- `id` is stable: it is never reused or renumbered.
- `territory` is world.placement's region id, or its bonus route.
- `order` is presentation order within the territory, never an identity.
- `admitted_version` is the version of `area-*.json` the area plays. Undoing an index change
  therefore also brings back the document that went with it.

## Operations

- **`save_draft`:** keeps a draft. A draft never enters play, and a key that another area already
  has is refused.
- **`admit`:** validates the draft, writes its integrity hash and makes it the admitted document. A
  changed admission is revision + 1, because a published revision is immutable.
- **`rename`:** changes display.name only. It writes a new draft and admits it again as the next
  revision. The key and the id stay.
- **`reorder`**, **`withdraw`:** index changes only.
- Each of these writes the index once, so `undo_last_change` takes one action back. Undo changes no
  document and deletes nothing.

## Saves

Version 4 saves already embed every non-Classic roster definition with its revision and hash
(ScenarioSave). A game always plays the save's copy. After a load, `area_differences` compares
each `u-*` area with the store. When the store's admitted document is gone or has another hash, the
load notice says "This save uses its own copy of <name>…".

The recovery path is `keep_area_copies`. It puts the copy back as a new admitted area if the store
has no area with that key, and otherwise as a new draft of that area, so the player's own edits are
never overwritten. Classic saves are unchanged.

## Duplicate layout

`duplicate_layout(doc, new_key, new_project_id, new_name)` copies an area's design:

- **new identity:** a new location key, project id and name; revision 1; origin "mixed" if it was
  "generated"; no integrity member;
- **renamed ids:** every entity id (districts, properties, every actor list and the mayor, food
  suppliers, attachments) gets a tag taken from the new key;
- **references followed:** properties[].realtor and assets.thumbnail.attachment;
- **unchanged:** district_cells (by index), supplier numbers and phone numbers.

It is checked with read_location and validate_location before it is handed back as a draft.
Ownership, money and progress are never in a location document.
