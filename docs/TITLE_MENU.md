# Title menu study (Quit entry)

Question studied: how hard is it to add a "Quit" entry to the game's own title menu? Answer: hard and fragile in
the game, easy as a host overlay. Nothing was implemented; this records what was found so the work can resume.

All addresses are eboot virtual addresses (image base 0), read from the user's own dump. No game bytes or
extracted text are stored here.

## How it was found

1. `DCP_TRACE_MSG=1` (`src/hle/msg_trace.cpp`) logs, once per (id, caller), every message-table lookup and
   SetText call with the return address. Run the game to the title, move over the entries, close, read `^msg:`
   in stderr. It needs a recompile that includes the `site` lines for `0x5915d0` and `0x591730` in
   `hooks/eboot_patches.txt`.
2. `dcp_scan <eboot> --show-fn ADDR` (any address inside the function), `--callers ADDR`, `--refs LO HI`
   (RIP-relative `lea` into a range, used to find constructors through their vtable) and `--find` / `--xref`.
   Vtable slots are not visible as raw bytes (RELATIVE relocations), so read the relocation addends instead.
3. Message ids in the trace are the index in `SystemMsg_US.fms` minus 4. Examples: id 382 is "Start Game",
   385 "Options", 386 "Gallery"; ids 389-393 are the one-line descriptions shown for the highlighted entry.

## Map

| Address | What |
|---|---|
| `0x4af4b0` | Scene factory: `id-1 <= 0x42`, jump table at `0x7570c0`, allocates (`0x56f0c0`) and constructs each scene |
| `0x53bd30` | Title scene constructor (object about 0x165650 bytes, many embedded widgets); called only from the factory. Uses text id `0x17d` ("Press button") |
| `0x53c980` | Thunk, vtable slot at `0xbbbc90`, jumps to `0x53ce80` |
| `0x53ce80` | Title method that sets the description of the highlighted entry; calls `0x53d3d0` and `0x53da70` |
| `0x53d3d0` | Title per-phase handler: `switch` on a mode (0..5) from `0x58dca0`, jump table at `0x75baa0`; sets texts such as `0x17e` with fixed positions |
| `0x4d5770` | Gallery scene constructor, vtable `0xbb3638` |
| `0x4d6b40` | Gallery phase dispatcher (`sil` 0..4); case 1 is `0x4d5c00`, which writes the Gallery text (id 386, or `0x41f` when `[obj+0x40e0] != 0`) |
| `0x481260` | Small SetText wrapper used by many screens (not the menu builder) |

Not verified: that `0x53bd30` is the title scene and not the boot screen (the evidence is the text ids it uses and
that its methods set the entry descriptions seen in the trace); which scene number selects each case of the
factory table.

## Why a native entry is a bad idea

- There is no entry list to extend. Each phase of the title has its own code, text ids, positions and float
  constants written inline.
- A seventh entry means changing the cursor range, the layout, the descriptions and the transitions between
  phases, in several functions. A mistake shows as a broken cursor or graphics, not as a clean failure.

## Options if it is picked up again

- **Host overlay** (recommended): draw a "Quit" item ourselves and act on a key or button. The game code stays
  untouched. Quitting already exists as touchpad click + Options.
- **Native entry**: start from `0x53d3d0` / `0x53ce80`, find where the entry count and cursor wrap live, and
  add the widget with the same layout constants. Expect a long session with the render capture open.
