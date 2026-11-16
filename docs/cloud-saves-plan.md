# Cloud saves (keyphrase): implementation plan

Status: plan only (2026-11-16). Nothing here is built or deployed.

Goal: a player's saves follow them to any browser by a keyphrase only they
have. Adapted from nfsu2-web's "Passphrase saves" (th0mk, MIT;
`E:\Development\Dev\nfsu2-web\web\src\app\vault.ts`, `vault-ui.ts`,
`vault-words.ts`, `web/server-rs/crates/vault/src/lib.rs`,
`web/server-rs/src/app/vault.rs`). Every file that reuses its code or rules
carries "after nfsu2-web's vault (th0mk, MIT)" in its header, and NOTICE.md
gets a line (CLAUDE.md, Browser rules). The word list is EFF's short
wordlist 1 (CC BY 3.0 US), as nfsu2-web credits it.

The CLAUDE.md "Rules" bind every step below:

- saves are never damaged or lost;
- formats are versioned;
- writes go through `.tmp` + rename;
- the rotating copies and the dated snapshots stay as they are.

The cloud is one more copy. It never replaces a local file without one of two
things: a snapshot taken first, or the local file unchanged since the last
sync.

## 1. Architecture

### 1.1 Decision: one Worker + R2, one object per keyphrase, conditional PUT

The new Worker is `mc3-saves` (`web/saves-worker/`, laid out like
`web/races-worker/`). It has its own `wrangler.toml` and its own route, for
example `saves.mcdub3.xyz`. It binds an R2 bucket `mc3-saves` (not the disc
bucket `mc3`), plus one small Durable Object `Ledger` for the global caps.

Separation from the lan Worker: a lan deploy drops every online player. The
saves Worker shares no code, config or deploy step with `web/lan-worker`,
`.deploy/worker.js` or the races Worker. Its deploy never redeploys those.

Why R2 conditional PUT, not a Durable Object per keyphrase:

- **Atomicity.** nfsu2 needs compare-and-swap on the keyphrase's current
  version (If-Match). R2's binding gives it: `bucket.put(key, body, { onlyIf:
  { etagMatches: old.etag } })` returns `null` when the object changed since
  `old` was read. R2 is strongly consistent, and the condition is checked by
  the store, not by the Worker. A new keyphrase uses `onlyIf: {
  etagDoesNotMatch: '*' }` (If-None-Match: *). Phase 1 proves both on a
  staging bucket before anything else is built. If either is not atomic, the
  fallback is a DO per keyphrase id (below).
- **Cost.** A DO per keyphrase bills a DO request and a storage write for
  every save, plus the Worker request. R2 bills one Class A (put) and one
  Class B (get or head) per save (section 8).
- **Simplicity.** The data is one blob per player. Nothing needs pushing to a
  client, so there is no socket and no alarm. A DO buys nothing that the
  conditional put does not already give.
- **The earlier-day copy without a second key.** As in nfsu2's file format
  (crates/vault lib.rs:9-31), the object holds a header, the current version,
  then the copy of an earlier day. One object means one atomic put, so the
  current version and the earlier copy can never disagree.

Fallback if conditional put is not atomic: a DO `Box` keyed by
`idFromName(hex(id))` that serialises gets and puts for one keyphrase and
keeps writing the same R2 object. The client API stays the same.

### 1.2 Object layout (`k/<64 hex>`, little-endian, after nfsu2's NFV1)

The key is the SHA-256 of the token (lib.rs:125-130). The Worker never sees
a keyphrase, a key or a save.

| at | size | field |
|---|---|---|
| 0 | 4 | "MCV1" |
| 4 | u32 | flags (1 = KEPT: never expired, set by the operator only) |
| 8 | u64 | current rev (random, nonzero; the ETag the client sees, not R2's etag) |
| 16 | i64 | current saved (ms, Worker clock) |
| 24 | u32 | current length |
| 28 | u32 | earlier length |
| 32 | u64 | earlier rev |
| 40 | i64 | earlier saved (0 = none) |
| 48 | i64 | created |
| 56 | 8 | 0 |

The current body follows, then the earlier body. The rule is the one in
nfsu2's lib.rs:443-451: the first PUT on a new UTC day moves the current
version into the earlier slot. A future format bumps the magic; the Worker
keeps reading every older magic.

### 1.3 HTTP API (nfsu2 vault.rs:7-21, same semantics)

The token travels in `Authorization: Bearer <43 base64url>`, never in the
URL. CORS is allowed only for `ALLOW_ORIGINS`, with an exact origin. The page
is COEP require-corp, and a cors-mode fetch needs no CORP header.
`Access-Control-Max-Age: 7200` keeps the preflights down.

- `GET /v`: returns 200 with the current body, `ETag "<16 hex>"`,
  `x-vault-saved` and `x-vault-earlier`. With a matching `If-None-Match` it
  answers 304 from `bucket.head()`, which reads no body. It answers 404 when
  the keyphrase does not exist. `?earlier` returns the earlier copy. With no
  token it answers 401 (how the page probes availability).
- `PUT /v`: needs `If-None-Match: *` (create) or `If-Match: "<rev>"`.
  - 201 or 200 on success, with the new ETag and `x-vault-saved`.
  - 412 on a mismatch or a lost race, with the current ETag.
  - 428 with neither header.
  - 413 over `MAX_BOX`.
  - 507 when the bucket is full.
  - 503 when the day's new-keyphrase budget is spent.
- `DELETE /v`: deletes the object; 404 if it does not exist.
- Touch: a GET of an object last written more than 30 days ago rewrites it
  unchanged, in a background `waitUntil`. With the bucket lifecycle rule
  below, a keyphrase therefore expires 2 years after its last use, not its
  last save. That costs at most one Class A per keyphrase per month.

### 1.4 Limits and abuse (from vault.rs:18-21 and lib.rs:33-52, scaled down)

- **Per version:** `MAX_BOX` = 512 KB sealed. Phase 0 measures real folders;
  the expected size is well under it (section 3).
- **Per IP** (CF-Connecting-IP), through the Workers Rate Limiting binding
  (`[[ratelimits]]`):
  - reads: 60 a minute;
  - writes: 30 a minute;
  - misses (GET 404): 30 an hour, checked before the read, so guessing
    keyphrases teaches nothing;
  - new keyphrases: 20 a day, which goes through Ledger (the binding's
    longest period is 60 s).
  - Over a limit, the answer is 429.
- **Global, in the `Ledger` DO.** It is consulted only on create and delete:
  - keyphrase count;
  - new keyphrases today: at most 2000 a day, else 503 + Retry-After;
  - a hard cap of 40,000 keyphrases (about 20 GB at 512 KB worst case), else
    507.
  - A daily cron (`scheduled`) lists the bucket and resets the count and the
    stored bytes. A list is Class A, 1000 keys a call: about 10 calls at 10k
    keyphrases.
- **Growing writes:** a PUT that grows an object past 64 KB counts its growth
  against a per-day byte bucket in Ledger. This is nfsu2's `per_day` idea
  (lib.rs:39-47), so a flood takes weeks, not hours. A PUT that keeps the
  size, or stays within 64 KB, never asks Ledger.
- **Expiry:** an R2 lifecycle rule deletes objects 730 days after their last
  upload. Together with the touch above, that equals nfsu2's "two years
  unused" (lib.rs:51). Objects flagged KEPT live under `kept/`, a prefix the
  rule skips.
- **Logs:** the Worker never logs a token or an id. `console.log` counts only.

## 2. Client crypto and keyphrase (vault.ts:1-103, ported to plain JS)

The page is plain JS with no bundler, so this goes in a new pre-js file,
`web/cloud.js`, plus `web/cloud-words.js`. Both load from index.html like
`lan.js`.

- **Keyphrase:** `mc3-` followed by 8 words from EFF short list 1 (1296
  words; nfsu2's `vault-words.ts` copied with credit), about 2^83. The words
  come from `crypto.getRandomValues` with rejection sampling (vault.ts:56-63).
  The phrase is read back in any case and with any separators, and the
  prefix is optional (vault.ts:66-75).
- **Key and token:** PBKDF2-SHA-256 with 100,000 rounds and the fixed salt
  `"mc3-web keyphrase saves v1"` gives 64 bytes. Bytes 0-31 are the AES-256-GCM
  key; bytes 32-63 are the token (base64url, 43 characters) (vault.ts:83-88).
  The AAD is the same string.
- **Box:** a version byte (1), a 12-byte IV, then the ciphertext and tag
  (vault.ts:90-103). The plaintext is a stored zip, deflated through
  `CompressionStream('deflate-raw')`. The zip writer reuses index.html's
  `zipStore` (index.html:2193). The page needs an unzip of its own: a small
  stored-only reader.
- The zip comment carries the GUARDED marker (vault.ts:155-161), so a future
  page can tell boxes sent by pages that honour the "fewer profiles" rule.

## 3. What goes in the box

**In:**

- every file under OPFS `UDATA/54540079/<12 hex>/`: `<name>.mc3Remix` (or
  `.mc3`) and `SaveMeta.xbx`;
- OPFS `online/profile.bin` (v1-v4, recomp_manual.c:3695-3732; 99,172 bytes
  at v4: header 20, money 4, garage 0x18340, car 4, wins 4, drift 4).

These are the same paths `snapCurrent()` collects (index.html:2226-2231), so
the box is a snapshot's file list.

**Out:**

- `backup/` (`.bak1/.bak2/.bad`, snapshots: this browser's own safety copies);
- `cache/` (disc cache, pipelines);
- `TDATA/` (title-wide options: open decision O3);
- `save/` (MC3_SAVE_DIR T:/U:/Z: scratch);
- `.tmp` files;
- every localStorage setting (keymap, padmap, flag, nick, drift setups,
  device settings).

**Size:**

- A career file is at least 99,162 bytes. The Remix writer streams more
  (CLAUDE.md, Saves), and its exact size is unproven. `profile.bin` is 99,172
  bytes.
- Raw, one career profile + the online profile is about 200 KB.
- Deflated, the garage blob is mostly empty CarCfg slots, so the estimate is
  20-60 KB per file. Phase 0 measures it.
- A player with 3 careers + online is estimated at 60-200 KB sealed, under
  the 512 KB `MAX_BOX`.
- If phase 0 shows otherwise, raise `MAX_BOX` (Worker constant + page
  constant) before shipping.

**Checks before a file is packed.** A file that fails its check means no push
this time; it is never sent:

- career: not empty, the same 4-byte stamp as its `.bak1`, signed when the
  `.bak1` is (the rule of web_fs.c `career_good`, :198-208, done in JS with
  WebCrypto HMAC-SHA1 under a zero key, as `sig_ok` :188-196 does);
- `profile.bin`: the magic, a known version, the body size for that version,
  and the CRC-32 of the body (index.html:2340-2342 already has the
  constants).

A failing file counts as "torn" (nfsu2's tornCount, vault.ts:194-198).

## 4. Sync rules (nfsu2 VaultSync, vault.ts:383-748, trimmed)

Link, kept in localStorage `mc3.cloud`:

```
{ phrase, rev, hash, saved, earlier, profiles, whole, torn, dropped, sent[], gone }
```

`hash` is SHA-256 over the kept files' (path, bytes), sorted by path; file
times are ignored (vault.ts:175-184).

### 4.1 Push (send)

A push runs only when all of these hold:

- linked, not gone, no conflict waiting;
- hash ≠ link.hash, or an unanswered send is pending (vault.ts:672-674);
- no file torn now. During a game it simply waits for the next save
  (vault.ts:669-671). Outside a game it asks: conflict 'fewer'.
- not 'empty': no profile here while the link had some, unless they were
  deleted on purpose (vault.ts:675-676);
- not 'fewer': whole profiles < link.whole - dropped (vault.ts:684-691).

It sends `PUT If-Match: link.rev`.

- 200: link to the new rev.
- 412: fetch the current version.
  - Same hash: link to it.
  - Its hash is in `link.sent` (our own lost answer, vault.ts:540-556): relink
    and send over it once.
  - Otherwise: conflict 'both'.
- 404 on 412 (the keyphrase was deleted elsewhere or expired): state 'gone';
  nothing is sent by itself (vault.ts:505-511).

### 4.2 Pull (take)

`GET If-None-Match: link.rev`.

- 304: nothing to do.
- 404: 'gone'.
- 200: a version from another browser. It is taken by itself only when all of
  these hold:
  1. the game is not running (`playing` false, index.html:2753): a version
     never comes in under a running game;
  2. the files here are unchanged since the link (hash = link.hash);
  3. theirs is not older (remote.saved ≥ link.saved: a server that went back
     never wins by itself);
  4. theirs is not bare: profiles there, or none here;
  5. theirs holds every profile folder here (else 'thin');
  6. every file of theirs passes the checks of section 3;
  7. no `profile.bin` in it has a version above what this page knows
     (`OPROF_BODIES.length - 1`): a newer build's file waits, and the player
     is asked to reload for the new page.

  Anything else is a conflict: 'both', 'older', 'bare' or 'thin'.

### 4.3 Applying any downloaded version (take, Use the cloud's, Load, earlier day)

The steps run in this order:

1. Snapshot the current saves into `backup/snap/<today>[-N]/`. This reuses
   `snapCurrent` and `writeTree` (index.html:2226-2240, the same code
   `restoreSnap` runs first, :2245-2251). If the snapshot fails, nothing is
   written.
2. Keep a copy of each replaced file through `keepCopy` (index.html:2170),
   which feeds the rotating `.bak1/.bak2` the C side also uses.
3. Write each file with `writeBytes` (createWritable, replaced at close:
   index.html:2162).
4. Overlay only. A local file absent from the version stays; nothing local is
   ever deleted by a sync. Cost: a profile deleted in one browser can come
   back from another. This is documented in the UI and accepted ("lost" is
   worse than "came back").
5. Relink at the version's hash: the hash of what was written, not of what is
   now on disk (vault.ts:588-595).

### 4.4 Conflicts

The player picks. Nothing syncs while a conflict waits.

The dialog shows both sides:

- for each side: profile names (SaveMeta Name=), file time, and the online
  profile's money and cars (header +16, body +0);
- the cloud side's time (server clock);
- the newer side marked.

The choices:

- **Keep this browser's.** PUT over the cloud's rev. The cloud's previous
  version stays reachable as the earlier-day copy if it is from an earlier
  day.
- **Use the cloud's.** The steps of 4.3.
- **Decide later.** Play is still possible, but the player is asked again at
  Play (nfsu2 askAtPlay).

### 4.5 One writer per browser

`navigator.locks.request('mc3-cloud', { ifAvailable: true })`: only the tab
holding the lock syncs. Other tabs show "synced by another tab". The game
already runs only in one tab, since its OPFS sync handles are exclusive.

## 5. Sync triggers in mc3

### C side (browser only, `__EMSCRIPTEN__`, no relift)

Bodies of existing overrides only, so `make web` is enough.

- In `src/web_fs.c`, add `static volatile uint32_t s_saves_seq;` and
  `EMSCRIPTEN_KEEPALIVE uintptr_t mc3_saves_box(void)`, plus `void
  mc3_web_saves_changed(void)` (++seq; one line: `[SAVE] saves changed
  (seq N)`). This is shared code with no game address in it.
- Call it in `src/games/remix/recomp_manual.c`:
  - after a career write, at :4523-4530 (`sub_00022040`): after
    `sub_00022040_gen()` and `mc3_web_saves_check()`, only when al = 1;
  - after a good Online write, at :3952 (`online_profile_save`, `if (ok)`).
    That function covers every `online_garage_left` caller: the garage exit
    (:2638, :4039), race prizes (online_progress.c:173), drift payouts
    (drift_score.c:98), tune (online_tune.c:139), police
    (police.c:288, police_players.c:194/232) and the manual save
    (`manual_save_pass` :4463 -> :4441).

### Page side (index.html + web/cloud.js)

- **Poll.** Every 2 s while playing, inside the existing interval near
  `whereNow` (index.html:3095), the page reads `Module.HEAPU32` at
  `mc3_saves_box`. Per the HEAP rule it reads only after
  `runtimeInitialized`, inside try.
- **After a save.** When the seq changed, the page waits 5 s with no further
  change (the game saves in bursts: garage exit + prize), then runs
  `push({ playing: true })`. Reading a file the game has open can throw
  (`NoModificationAllowedError` under an open sync access handle). That is
  caught and retried at the next tick, never counted as torn.
- **Page open.** Run `pull()`, after the OPFS check, before the Saves list is
  drawn.
- **Before Play.** In `play()` (index.html:2821), run `pull({ timeoutMs:
  4000 })` before `playing = true`. On timeout or offline, Play goes on with
  the local saves. A waiting conflict opens the dialog first.
- **pagehide** (index.html:2730 has one): flush a pending push with
  `fetch(..., { keepalive: true })`, only if the box is ≤ 60 KB. The browser
  caps keepalive bodies at 64 KB. A larger box relies on the 5 s debounce
  having run already.
- **Page writes.** Import, online import, snapshot restore and the cloud take
  itself call `cloud.intended()` and then `push()`. A restore the player did
  on purpose is no 'fewer' (vault.ts:424-428).

## 6. UI: the Saves tab (index.html:669-684)

A new block, "Cloud saves", goes between the Profiles list (:679) and
Snapshots (:682). It is shown only when `config.json` has `savesUrl` and
`GET savesUrl/v` without a token answers 401.

| State | What the block shows |
|---|---|
| Off | "Keep your saves under a keyphrase" with **Create keyphrase**, and "Load saves from a keyphrase" with an input plus **Load** |
| Linked | The keyphrase box (first 3 characters + dots, an eye to show it for 30 s, Copy, Download as `.txt`), the status (Synced HH:MM / Syncing / Error: why / Not on the server any more), **Sync now**, **Go back a day** (when `earlier` exists; uses 4.3), **Stop syncing here** (unlink only), **Delete from the server** (confirm, red) |
| Conflict | Both sides as in 4.4, the 3 buttons |

- Load on a browser that already has saves goes through the conflict dialog,
  never a silent overwrite.
- After Create, the page tells the player to write the keyphrase down: the
  page cannot recover it.
- Copy and download use the existing `saveFile` (index.html:2076).
- The hint text and `docs/web/PLAYING.md` (Saves) get a paragraph.
- CHANGELOG gets one player line: "Cloud saves: keep your saves under a
  keyphrase and load them in another browser".

## 7. Migration and compatibility

- **Opt-in.** Nothing changes for a player who never makes a keyphrase.
  Existing saves, `.bak1/.bak2`, `.bad` and snapshots are untouched. No C
  save format changes.
- **The first Create** uploads the current folder as is.
- **The first Load** on a browser with saves is a conflict. Whatever the
  player picks, the local saves go into a snapshot first.
- **Snapshots made by a take** are named like the page's restore snapshots,
  and the C side's 7-day pruning applies (web_fs.c :299-312). A take's
  snapshot therefore lives 7 days, and the cloud's earlier-day copy covers
  the day before. Open decision O4.
- **Imports placed in SHA-1 folders before 2026-11-13** (CLAUDE.md, Saves)
  sync as they are: unreadable then, unreadable now, never deleted.
- **Box format:** version byte 1. A future box version is read by new pages
  only. An old page that meets a newer box version shows "update the page",
  never treats it as damaged, and never sends over it (nfsu2's VAULT-NOT-SAVES
  becomes an ask).
- **Native build:** web_fs.c is web-only, and the recomp_manual.c calls sit
  under `__EMSCRIPTEN__`. Native stays byte for byte the same.
- **Commit note:** the C commit carries a migration note ("no save format
  change; signal only"), and CLAUDE.md Rules gets one line on the cloud copy.

## 8. Costs (R2 + Workers), 150-300 players a day

Assumed per player-day:

- 2 pulls (open, Play), about 2 B (head on 304);
- about 10 pushes (career saves + online garage, prizes, drift), each 1 get +
  1 put = 1 B + 1 A;
- about 12 Worker requests + preflights (cached 2 h).

At 300 players a day:

| Item | Per day | Per month | Free tier | Cost |
|---|---|---|---|---|
| Class A (put, list, touch) | about 3,000 | about 90k | 1 M | 0 |
| Class B (get, head) | about 3,600 | about 110k | 10 M, but already exceeded by disc reads (about 3.4 M/day, CLAUDE.md) | 110k x $0.36/M ≈ $0.04 |
| Storage | | 5k keyphrases x about 300 KB (current + earlier) ≈ 1.5 GB | 10 GB shared with the disc bucket (about 5-6 GB) | at most $0.02/GB-mo ≈ $0.03 |
| Egress | | | free on R2 | 0 |
| Worker requests | about 4-8k | | 100k/day free, or 10 M/mo on Paid | 0 (the DO volume suggests Paid already: open O1) |
| Ledger DO | creates and deletes only: tens a day | | | 0 |

Total: well under $1 a month. Worst case under abuse: the global caps hold
storage under 20 GB (about $0.30/mo) and the per-IP limits hold write bursts.

## 9. Privacy

- The server stores ciphertext under SHA-256(token). It never sees a
  keyphrase, the key, profile names or the IP in storage. The IP is used only
  in the rate limiter, in memory.
- No profile names leave the browser unencrypted.
- No cookies: the token lives in localStorage and the Authorization header,
  so another site cannot ride it.
- Anyone who has the keyphrase owns the saves. The UI says so: "anyone with
  this keyphrase can load and replace these saves".
- Delete from the server removes both versions at once.
- The privacy note in PLAYING.md lists what is stored, for how long (2 years
  unused), and the earlier-day copy.

## 10. Failure modes

| Failure | Behaviour |
|---|---|
| Offline / timeout | State 'error' with the reason. The next save, Play or Sync now tries again. Play never waits more than 4 s. |
| Lost PUT answer (tab closed) | The hash goes into `link.sent`. The next push or pull sees its own version and relinks (vault.ts:540-564). |
| Two browsers save at once | One PUT wins; the other gets 412, then conflict 'both'. Neither is lost: one is in the cloud, the other is local plus its snapshot. |
| Storage full (OPFS quota) on a take | The snapshot fails, so nothing is written, and the error is shown. A partial overlay is impossible: each file is written whole, and the snapshot came first. |
| Torn file mid-game | No push; the next save retries. |
| Server rolled back / object restored | 'older' conflict, never applied by itself. |
| Keyphrase expired or deleted elsewhere | 'gone'. The link stays, and **Send again** re-creates it with the local saves (vault.ts:713-720). |
| R2 conditional put not atomic (found in phase 1) | Switch to the per-keyphrase DO fallback (1.1) before phase 2. |
| Worker or bucket down | Same as offline. Local saves are unaffected. |
| A page from an older build (cached) | A newer box version is refused, and so is a newer `profile.bin` version (4.2 rule 7). |
| Wrong keyphrase typed | 404, which uses a miss: "no saves under this keyphrase". After 30 misses an hour, 429. |
| GCM tag fails (corrupt object) | "These saves are damaged". Offer **Go back a day**; nothing local is touched. |

## 11. Test plan

No browser and no game launch by agents (CLAUDE.md browser rules). Node only.

- **`web/saves-worker/test/worker.test.mjs`** (node --test, like
  `web/races-worker/test/lobby.test.mjs`). It uses an in-memory R2 mock with
  real etag compare-and-swap and a Ledger stub. Cases:
  - create (If-None-Match *), then a duplicate create gets 412;
  - If-Match ok, stale and missing (428);
  - a 304 from head;
  - the earlier copy moved on a new UTC day and kept within a day;
  - `?earlier`;
  - 413 over MAX_BOX;
  - 401 with no token or a bad token;
  - rate limits (429), the 503 day cap, the 507 hard cap;
  - DELETE;
  - touch after 30 days;
  - CORS only for allowed origins;
  - a race of two PUTs on one rev: exactly one 200.
- **`tools/test_cloud_saves.mjs`**, after nfsu2's `test/vault.test.mjs` (its
  21 tests are the checklist). It loads `web/cloud.js` in node (webcrypto,
  CompressionStream) against the Worker module + the mock. Two "browsers" with
  an in-memory OPFS. Cases:
  - phrase read-back (case, separators, prefix);
  - PBKDF2 vectors against node:crypto;
  - seal and unseal with the wrong phrase;
  - pack size of a real-sized folder;
  - the content hash ignores times;
  - excluded paths never packed;
  - create in A, load in B (conflict when B has saves);
  - each takes the other's;
  - both change: conflict;
  - the same change on both: no conflict;
  - B empties: 'empty' asked;
  - torn mid-game: no push; torn outside a game: 'fewer';
  - bare or older versions are never taken;
  - thin: a profile here missing there;
  - a newer `profile.bin` version is not taken;
  - lost answer: relinked;
  - gone and send again;
  - every take leaves a snapshot with the replaced files, and no local file
    is deleted.
- **C:** a compile check `make web GAME=remix` into an own BUILD_DIR. Native
  `make build` unchanged.
- **Owner run on :3000.** The owner uses two Chrome profiles: create, play,
  save in the garage, load in the other, change both, settle the conflict,
  then go back a day. The log lines `[SAVE] saves changed`,
  `[cloud] pushed|pulled|conflict ...` are checked.

## 12. Phases

| # | What | Files | Size |
|---|---|---|---|
| 0 | Measure: deflate the owner's exported `mc3-saves-*.zip` (careers + `profile.bin`) and fix `MAX_BOX`; prove R2 `onlyIf` CAS (create and match) on a staging bucket with two concurrent puts | scratch script, notes added here | S (½ day) |
| 1 | Worker + Ledger + tests + `wrangler.example.toml` (route, R2 binding, ratelimits, cron, lifecycle steps in STEPS) | `web/saves-worker/src/index.js` (~300 lines), `src/ledger.js` (~80), `test/worker.test.mjs` (~300) | M (1-1.5 days) |
| 2 | Client core: words, phrase, keys, seal, zip/unzip, checks, VaultSync port (push/pull/conflict/take/sent/gone) | `web/cloud.js` (~600), `web/cloud-words.js` (copied), `tools/test_cloud_saves.mjs` (~500) | L (2-3 days) |
| 3 | C signal: saves box + 2 calls | `src/web_fs.c` (+15), `src/games/remix/recomp_manual.c` (+3) | S (1 hour + `make web`) |
| 4 | Saves tab UI, triggers (poll, Play, pagehide, page writes), the conflict dialog, NOTICE.md, PLAYING.md, CHANGELOG, CLAUDE.md note | `web/index.html` (+250), docs | M (1-1.5 days) |
| 5 | Owner deploys the saves Worker (never the lan one) and sets `savesUrl` in config.json via stage.sh; the owner tests on :3000, then release | `.deploy` (owner-only) | S |

Every phase is its own commit (or commits), run in order. Phases 1 and 3 can
go in parallel.

## 13. Open decisions for the owner

- **O1. Plan tier.** Is the account on Workers Paid? It changes nothing in
  the design, only the request budget line.
- **O2. Domain.** `saves.mcdub3.xyz`, or a path on an existing host? A path on
  the disc Worker would tie deploys together, so a separate host is
  recommended.
- **O3. Game options.** Should `TDATA` (the game's own options, title-wide)
  and the player's identity (nick, flag) travel too? The recommendation for
  v1 is no: they are device-ish, and nfsu2 learned to keep device settings
  local (vault.ts:15-18).
- **O4. How long a take's snapshot lives.** The current 7-day pruning, or a
  `cloud` mark that keeps it 30 days like nfsu2's replacedDir? A 30-day mark
  needs a C change in `mc3_web_saves_snapshot`'s pruning.
- **O5. Deleted profiles.** The overlay-only take accepts that a profile
  deleted in one browser can come back from another. Alternatively, port
  nfsu2's `dropped`/'thin' rules fully and let a take delete. That is more
  code and adds a loss path.
- **O6. Caps.** `MAX_BOX` 512 KB, 40k keyphrases, 2000 new a day, 2 years
  unused: confirm after phase 0.
- **O7. Should Online career money travel?** It already does, inside
  `profile.bin`. There is no server-side check, so a player can edit their
  own box exactly as they can edit OPFS today. Confirm that this is fine,
  since there are no leaderboards.
