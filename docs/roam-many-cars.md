# Free roam: many remote cars (plan, 2026-11-09)

Goal: show 16-32 remote players' cars in the Remix's Online cruise instead
of the 7 player slots used today (src/games/remix/roam_game.c). Model:
nfsu2-web (th0mk, MIT; origin/main runtime/roam_game.c): cars made by the
game's own CreateCar outside the AI index, appended only to the world's
render list, driver 0, physics inert, pose written each frame after the
physics step, collision only within 20 m, reuse a parked car with the same
look / free slot / re-skin / evict the longest unseen, car count by heap and
frame rate (32 default, 48-64 with a 2 GB heap window).

Addresses: Remix lift (work-remix/gen, disasm work-remix/analysis/disasm).

## What MC3 has

**Building a car without a player.** The player builder does not read the
player object: builder ctor sub_002CB270(name, 0, slot, CarCfg*, flag, pad)
(vtable 0x476940), build sub_002CBC40, dtor sub_002CB2C0. Steps:
- model instance new 0x3138 sub_000F4430 (-> veh+0x68), look applied by
  sub_000F32A0(CarCfg, 1, slot) @0x2CBCBA (every bit of the CarCfg's dirty
  mask +8 but 0x80, mask & 0xFFFFC080 after). Two parts of the look come
  later (2026-11-09, the bare rims and wrong paint of standalone cars):
  bit 0x80 is the wheel step of the vehicle update (0x00100657: tyre size
  from rim + tyre profile sub_00121970, wheel physics sub_00105450), so a
  CarCfg memcpy'd from a record (+8 = the sender's, 0 after its build) never
  got tyres -- operator= sub_00271E20 sets +8 = 0x3FFF, roam_cars.c too now;
  a preset paint (+0x40 != 0xFF) reaches the model's paint (+0x1228) only in
  the model update sub_000F2C00 while +0x1270 is set, and the draw
  (0x000FE09C) applies +0x1228 and clears +0x1270: a car built after the
  physics step was drawn first and kept its own colour. roam_car_build calls
  sub_000F2C00 on the model after the build. Still a bare Jetta after that
  (2026-11-10): the step (0x00100654) sizes the tyres, sub_00121970 on
  [veh+0x50] with (CarCfg, holder [[veh+0xC]+0x2F0]), from the rim geometry
  [[holder+0x1800]+0x18]+0x3818/+0x381C (sub_000F11E0, -0.3 m when missing).
  Re-arming 0x80 each frame did not help (2026-11-11, every peer's car still
  bare): the update never reaches the step for a network player's car
  (0x00100645 -> return at 0x001007E8; or the 0x001005E7 path). roam_tyres
  (roam_game.c) now calls sub_00121970 itself, once per vehicle (slots and
  standalone), as soon as the rim geometry is loaded; '[ROAM] tyres:
  slot|car N sized after S s (radius ...)' once per build, or '... no rim
  geometry after 10 s';
- vt+0x20 sub_002CB2D0: car cache acquire sub_00105B80 -> builder+0x4C
  (cache [0x50A08C], 16 slots, refcounted per car index: same model shared);
- vt+4 sub_002CBDE0: vehicle new 0x6C (vtable 0x454F40, ctor sub_000FF160),
  veh+0x5D = slot, veh+0x5E = 0 (player; AI = 1); physics instance 0x44
  -> veh+0x1C;
- vt+0x10 sub_002CB5C0: car sim 0x49C -> veh+0xC; **draw add** in
  sub_000FBA90 @0x0FBC72 (sub_00093190, ecx = shared model [carSim+0x18]);
- vt+0xC sub_002CB390: car 0x1AC -> veh+8, car vt+0x34 sub_00121DE0;
  **physics add** @0x121F78: [0x5580F0]->vt+0x70 sub_002ABD00(collider
  car+0x68, 0) -- silently skipped when the level's pool is full;
- vt+8 sub_002CB320: controller 0x84 (none for pad -1 outside a network race;
  roam_build_step already forces it);
- **registry add** @0x2CBDBD: [0x5638E4]->vt+4 sub_000FEEA0(veh).
- Removal (inverse) is sub_000FFB40: physics vt+0x14 sub_002AD320
  @0x0FFB53, draw sub_00092EF0(.., 0) @0x0FFB5E, registry vt+8 sub_000FEF10.

Slot caveat: sub_002CB710 (vt+0x1C) reads the race-setup entry of the slot
([0x53F2E0]+0x114+slot*0x2C) and the slot is compared with the local player
(sub_002694B0/sub_002694D0) and copied into sub-objects (sub_00128C40,
sub_00111040, sub_00101960; readers not checked). Standalone cars must use
slot numbers 1..7 (not 0, not >= 8) whose entry has controller +0x18 = -1;
several cars share a slot number. Untested.

Not usable: the AI racer builder sub_001FAE10 (vtable 0x46C730, from AI
ctor sub_001ED2F0) builds a 0x148 AI driver bound to an AI object;
traffic/ambients (Pool_Size in sub_0007FDA0, tune/traffic/%s, models from
$/resources/city/%s_traffic) never use the 0x454F40 vehicle nor a CarCfg.

**Who updates a standalone car.** Vehicles are stepped through the
registry: the player manager update sub_002BDEC0 walks it (vt+0x10
sub_000FEF80 over +0x80/+0x88/+0x90, vt+0x14 sub_000FF0A0 over +0x94).
sub_002BDEC0 is already overridden (recomp_manual.c, cam_call): the
"after the physics step" hook is there, no relift. A standalone car has no
player, so sub_002C1520 (and sub_002C0540 pose correction, which reads the
player's net block) never runs for it: we blend and write the pose
ourselves (SetMatrix sub_002A35A0 on [[veh+8]+0x68], SetVel sub_003432B0 on
[that+0x10], as roam_set_pose), inputs into its controller with
sub_002C9570 (thiscall on the controller, no player needed).

**Vehicle registry** [0x5638E4] (0x98 B, made once at boot: sub_0001DC80
push 0xA @0x1DD4B -> sub_000FF0D0 -> ctor sub_000FEDC0, base sub_0035D200):
capacity 10 at +0x7C, 18 lists {count, array} (arrays +8..+0x78, +0x84,
+0x8C, +0x94; sized cap x4, +0x38 x8, +0x40 x16, +0x48 x32, +0x50 x8). Add
(sub_000FEEA0 / base sub_0035D500) never checks the capacity; all readers
(only via getter sub_003308C0: sub_000FFB40, sub_000FFBA0, sub_0013CA70,
sub_002CBAE0, sub_001FB040, sub_002CBC40, sub_002BDEC0) loop by count. 8
player cars use 8 of 10: any extra car needs it enlarged. **Done**
(roam_cars.c, MC3_ROAM_CARS=N: capacity 8 + N + 2, arrays repointed into
the contiguous window, contents copied).

**Minimap blips.** The walker sub_000BF850 -> sub_000BE140 (gated on
[[veh+0xC]+0x12D]) draws each blip with sub_000BB310 (ecx = minimap, ret
0x10; args icon desc*, Matrix34* ([veh+0x1C]+0x10: heading +0x18/+0x20,
position +0x24), scale, 1), default desc [0x507508]+0x40, scale
[[0x507508]+0x1C], after minimap vt+0x7C. Must run inside sub_000BF850's
pass (esi = minimap): needs an override of sub_000BF850 (new override =
relift).

**Names over cars.** No name field in the player/vehicle. Options: the
page draws them (DOM overlay) from positions projected with the camera the
projection override already sees (recomp_manual.c ~:1625), or the game's
Flash HUD text (not found). Overlay is the cheaper route.

## The heap (why 'Heap overrun', and 128 MB)

- 'Heap overrun (%d bytes requested from %s)' (0x466FA0) and 'Heap %s
  overrun' (0x466FCC) are both in the game's own allocator sub_001B4740
  (@0x1B4986 bump top + size > end; @0x1B4933 after the free list), not the
  toolkit. operator new sub_001B3B90 uses it on [0x52B488].
- The main heap is a **fixed 47 MB**: sub_00012440 @0x12503 `push 0x2F` ->
  sub_002698C0 -> sub_003670F0 (`shl ecx, 0x14` @0x367189) -> sub_00367310
  -> sub_00018EC6 = MmAllocateContiguousMemoryEx(size, 0, -1, 0x1000, 4);
  'cannot allocate game heap' (0x4A5378). No tune file, no
  MmQueryStatistics / MmQuerySystemSize / GlobalMemoryStatus import.
- Sub-heaps come out of it: LoadHeap 800 KB (@0x1BC12), VehHeap 3 MB
  (@0x150FE6, 0x150FF0, 0x152495: the garage), a network heap.
- In the port it lives in the contiguous window: XBOX_CONTIG_BASE
  0x80000000, XBOX_CONTIG_SIZE 64 MB (kernel.h:398-399), arena
  xbox_ContiguousAlloc (xbox_memory_layout.c:3616), shared with the image
  and D3D's surfaces.
- So **128 MB of RAM alone changes nothing** (xbox_SetTotalRam /
  XBOX_DEVKIT_RAM exist, xbox_memory_layout.h:48-49, unused): the game
  never asks. A devkit-like 128 MB needs (a) XBOX_CONTIG_SIZE 128 MB with
  XBOX_BUS_RAM_TAG moved off 0x04000000 (kernel.h:409; users apu_shim.h:108,
  nv2a_pb_exec.c:107, ohci.c:489, kernel_bridge.c:2831, xbox_devbus.c:44),
  PB_RAM_BYTES (nv2a_pb_scan.c:173), online_ram's 0x84000000 bound; and (b)
  the heap size: an override of sub_002698C0 (or sub_003670F0) passing
  MC3_HEAP_MB instead of 47 (imm8 max 127). (b) adds an override: relift.
- **The cars heap** (2026-11-10, untested in game; roam_cheap in
  roam_game.c): the game's heaps are a list ([0x52B478], next at +4; the
  ctor sub_001B4600 prepends) and operator delete (sub_001B3C00, []
  sub_001B3CD0) finds a block's heap by address (sub_001B3D80: base +8,
  size +0xC), not by the current heap. So a second heap of the game's own
  type ("RoamCars": ctor + init sub_001B3DF0(base, size, 1, 0) as
  sub_00367310 does the main one; the object in its block's first page)
  made current ([0x52B488]) around every remote car's build -- the player
  manager at load (sub_002BE2C0_gen, our car too), slot swaps and
  standalone cars (roam_car_build) -- keeps the cars out of the main heap,
  and their frees need nothing. Its memory comes from the contiguous window
  (xbox_ContiguousAlloc), not the extended VMA: a car's textures and vertex
  data sit in its heap blocks and the GPU reads them at bus addresses, which
  only window memory has (dma_resolve, nv2a_pb_exec.c). Window 128 MB: main
  heap 63 MB (was 95) + cars heap 48 MB (MC3_ROAM_HEAP_MB; 0 = none, main 95
  as before), the same ~5 MB left over as retail's 64 MB layout (main heap
  at 0x80BCC000: 11.8 MB of the window before it). 64 MB window: none, cars
  in the main heap with the old caps. The cap on cars is then the cars
  heap's room (2 MB kept after one more car): ~55 cars, above the default
  MC3_ROAM_CARS 32. Logs '[ROAM] cars heap: N KB at ...', '[ROAM] heap
  'RoamCars' before/after the cruise's cars', '[ROAM] main heap: N KB taken
  meanwhile'. Made at the first cruise with remote cars, kept for the
  session (a model the car cache read during a build stays in it).

## Per-car cost (estimates; measure)

- Own allocations per car (player builder): model instance 0x3138, vehicle
  0x6C (+0xE0, +0xC4), car sim 0x49C (+0x434, 0x88, 0x100, 0x2C), car
  0x1AC, collider 0x78, 0x1A4, ~15 small objects, 0x54 per part: ~20-25 KB
  of structures. Unknown: what sub_000F4430 / sub_000F32A0 build per car
  (per-car paint/vinyl textures likely; the overrun at 7 cars + a garage
  visit says the real cost is well above 25 KB). online-heap-fix's '[ROAM]
  the cruise's N cars took X KB, about Y KB each' line gives the number.
- Shared per distinct car (cache slot): the model file data, refcounted;
  16 slots, so at most ~14 distinct remote models besides ours and the
  garage's.
- Physics: one collider in the level per car; pool size unknown (init
  sub_002AC730); full = car silently not added.
- Draws: cruise ~500 draws/frame; a car is roughly 20-40 (body, glass,
  4 wheels, lights, shadow), only when in view (the game culls).

**Estimated max N.** 64 MB guest / 47 MB heap: the margin after a load is
what 7 player cars plus a garage round trip just overran; with reuse of
same-look cars and no garage-time rebuilds, about **8-12** standalone cars,
16 only if a car costs under ~150 KB (measure first). 128 MB-like (heap
~100 MB, contiguous window 128 MB): +50 MB, **32** cars even at ~1 MB each;
the 16-slot model cache (distinct looks) and the physics pool become the
limits.

## Steps

| # | Step | Where | Effort | Risk |
|---|---|---|---|---|
| 1 | Registry capacity 8+N+2 (MC3_ROAM_CARS) | roam_cars.c, done | S | low: count-driven readers, arrays copied |
| 2 | Spare race-setup entries 1..7 as templates (+0x18 = -1), N standalone cars built at cruise load with the player builder on a stack builder, after sub_002BE2C0_gen (same calls as roam_rebuild) | roam_cars.c | M | med: slot copies in sub-objects; heap |
| 3 | Per frame after sub_002BDEC0_gen: blend pose (tau 100 ms, snap 8 m), SetMatrix/SetVel, inputs via sub_002C9570 if a controller exists; park + drawn flag 0 when unused | roam_cars.c + 1 line in the sub_002BDEC0 override | M | med: pose written after the step is drawn as is; wheels from controller |
| 4 | Collision within 20 m: physics level vt+0x70 add / vt+0x14 remove by distance (keep the pool small) | roam_cars.c | S | med: pool size unknown |
| 5 | Assignment: nfsu2-web's (same look parked > free > re-skin via roam_rebuild path > evict longest unseen), hub K raised (MC3_ROAM_K) | roam_cars.c, hub | M | low |
| 6 | Blips: override sub_000BF850, sub_000BB310 per standalone car | recomp_manual.c, relift | S | low |
| 7 | Names: page overlay from projected positions | web + projection override | M | low |
| 8 | Bigger heap: MC3_HEAP_MB override of sub_002698C0 + toolkit contiguous window 128 MB (bus tag move). **Built, untested in game (2026-11-09)**: RECOMP_RAM_MB=128 (toolkit, default 64) makes the window 128 MB, the bus tag = window size (bit 27), tiled alias 128 MB, MmQueryStatistics 128 MB; low RAM stays 64 MB. sub_002698C0 override: MC3_HEAP_MB, default 47, 95 at 128 MB, at most window - 17; '[HEAP] main heap N KB' | toolkit + relift | M-L | med-high: every bus-address user |
| 9 | Auto count: start 8, +4 per 10 s while heap free > 4 MB and < 5 % late frames; down on overrun risk | roam_cars.c | S | low |

Steps 2-5 and 9 **built, untested in game (2026-11-09)**, src/games/remix/roam_cars.c:
MC3_ROAM_CARS = remote cars at most, slots included (auto 32 / 12 by the
window); built at load for the peers known + 2, then on demand one per
0.25 s; step 9 is by heap only (max(6 MB, heap/8) kept free after one more
car, its cost measured on the last build), no frame-time rule. Cruise end:
the player manager's delete sub_002BD7F0 (from teardown sub_00029690) has
no override, so cars are dropped whenever a state request is queued
(sub_00020EC0 pass) and rebuilt if the cruise goes on.

Order: 1-3 with MC3_ROAM_CARS=4 (measure heap per car), then 4-5, then 6-9.
