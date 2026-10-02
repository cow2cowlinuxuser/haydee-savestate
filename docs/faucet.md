# Faucet: floating texture payloads (D3D11SW_FAUCET)

Rabi-Ribi never holds a pointer to texture pixels. It holds DxLib integer
handles, DxLib holds our D3D objects, and only our objects know where the pixels
are. The faucet uses that: over `D3D11SW_RESIDENT_CAP_MB` it evicts cold textures
to phantoms (description kept, pixels freed, bank copy kept) and paints them back
into any free arena slot on their next draw.

## Where it stands (2026-09-27)

- Phantoms: the game survives a cap swept from 192 MB down to 16 MB.
- Step 1: paint-in at new addresses, no visible difference in play.
- Thrash fix: recency by frame (`D3D11SW_FAUCET_HOT`, default 3 frames), overcommit
  instead of evicting hot textures, 128 KB floor (`PAYLOAD_VA_MIN`), per-window
  stats. In the scene that used to thrash: about 1000 paint-ins per 2 s at 33 fps
  became 0 at 60 fps. Steady hot set about 87 MB, 144 MB peak during loads.
- The bank is an in-process heap copy, so the process holds two copies of every
  managed texture. Address space is about 1650 MB used, with the largest free
  block down to about 130 MB.
- Not yet tried with save and load.

## To fix

1. **Residency never returns to the window.** After a load burst it parks over
   the window for good (188.6 MB against 128 MB), because eviction only runs
   when an allocation needs room. Trim in `faucet_frame`: when over the window,
   evict cold textures only, a few MB per frame, until back under.
2. **Load bursts nearly fill the arena.** Peak 252 MB live, `246 of 256 MB`
   arena. Every new texture counts as hot for 3 frames, so a burst cannot evict
   its own fresh textures. Add a hard ceiling below the arena size (for example
   the arena minus 32 MB). Above it, evict even hot textures, least recently
   drawn first. A brief thrash is better than spilling outside the arena.
3. **Overflow at fixed bases.** When the arena is full, `payload_alloc` falls back
   to `VirtualAlloc(NULL, ...)`, a base the OS picks. Replace that with extra
   extents reserved on demand at fixed candidate bases, in order. The startup
   probe already reports which are free (`0x50000000`, `0x30000000`,
   `0x20000000`). Every payload then stays at an address we chose. Log each
   extent taken.
4. **Paint-in runs on the draw thread,** which in Rabi-Ribi is the game thread.
   Thrash does cost the game frames (33 fps against 60 above). Prefetching into
   the arena from a worker, with the draw thread only swapping the pointer, would
   make thrash free for the game.
5. **Bank to disk** (design step 4): a mapped pack keyed by content hash. This
   removes the second in-process copy and is what a new session would read from.

## Related bug

`lfh_find_regions` (savestate.c) reads recorded region bases in the live process
without checking they are still committed. It faulted once during a save at
`d3d11.dll+0x36420`. Check with `VirtualQuery` before the 64-dword scan: skip
unless committed, readable, not a guard page, and at least 256 bytes left in the
region.
