# Security

## Reporting

Open a private security advisory on the GitHub repository, or an issue
without exploit details asking for a contact. For problems in the
unchanged Pizza Legacy engine, also tell upstream:
https://codeberg.org/cowomaly/pizzalegacy/issues

## What the program touches

| Surface | Risk | Mitigation |
|---|---|---|
| Original game files (VGA, BOB, FNT, VOC, CMF, data) | Malformed files from an untrusted copy | Read-only; bounds checks in the loaders; the exporter checks headers before decoding and skips bad files |
| HD pack PNGs | Untrusted images (packs shared by others) | Decoded by SDL3's PNG loader; size/aspect validated before use; a bad file is ignored with a warning, never fatal |
| Saves and settings | Local files in the user's data folder | Unchanged from Pizza Legacy |
| `hdpack.py comfy` | HTTP to a ComfyUI server | Only `http`/`https` URLs accepted; default is `127.0.0.1`; nothing else on the network is contacted; no credentials are sent |
| `hdpack.py` workflows | Workflow JSON is sent to ComfyUI as-is | Only use workflows you trust; ComfyUI custom nodes can run code on the GPU machine |

The game executes no downloaded code, loads no plugins and opens no network
connections. Pack art is data only.

## Threat model in short

Treat HD packs from strangers like any image download: they can't run code
here, but a decoder bug in a dependency could crash the game. Dependencies are
built from pinned release tags by `scripts/fetch-deps.*`; bump the tags there
to pick up fixes.
