// web/cloud.js -- cloud saves under a keyphrase (docs/cloud-saves-plan.md): this browser's saves kept on the server
// too, sealed with a key only the keyphrase gives, so they can be loaded in any browser the keyphrase is typed into.
//
// After nfsu2-web's vault (th0mk, MIT: web/src/app/vault.ts, vault-words.ts; NOTICE.md): the keyphrase, the PBKDF2 key
// and token, the AES-GCM box and the sync rules (VaultSync: push, pull, conflicts, a lost answer, a keyphrase gone) are
// ported to plain JS and trimmed for mc3. The word list is EFF's short wordlist 1 (1296 words, by Joseph Bonneau and
// the EFF, https://www.eff.org/dice, CC BY 3.0 US), as nfsu2-web's vault-words.ts has it.
//
// - keyphrase: "mc3-" + 8 words (about 2^83), read back in any case, with any separators, "mc3" optional;
// - PBKDF2-SHA-256 (100,000 rounds, fixed salt) of it gives 64 bytes: an AES-256-GCM key and a token (base64url). The
//   server keeps the box under SHA-256(token) and never sees the keyphrase, the key or a save;
// - box: version byte 1, 12-byte IV, ciphertext + tag of the deflated (deflate-raw) plain: u32 LE header length, a
//   JSON header { v: 1, guarded: 1, files: [[path, size], ...] }, then the files' bytes in that order;
// - what goes in: UDATA/54540079/<folder>/<file> (career profiles + SaveMeta.xbx) and online/profile.bin only. Never
//   backup/, cache/, TDATA/, save/, .tmp files or any setting (owner, 2026-11-16: device things stay on the device);
// - a file that fails its checks (web_fs.c career_good against its .bak1; profile.bin magic, version, size, CRC) is
//   "torn": no automatic send while more are torn than at the last sync, and a version holding one is not taken by
//   itself;
// - every write of a downloaded version goes through deps.apply (index.html): a dated snapshot of the saves here
//   first, then each file overlaid (the old one kept as a rotating copy); nothing local is ever deleted by a sync;
// - polite to the server (owner): automatic sends at most once every 5 minutes, after 5 s without a new save signal,
//   never one whose files are unchanged since the last sync; failures back off 1 min doubling to 1 h, a 429/503's
//   Retry-After is honoured for every try; no retry loop. A pull at page open and before Play only.
//
// Plain script for the page (window.MC3Cloud) and node (globalThis.MC3Cloud: tools/test_cloud_saves.mjs). No DOM here.
(function (G) {
  'use strict';
  const WORDS = `
acid acorn acre acts afar affix aged agent agile aging agony ahead aide aids aim ajar alarm alias alibi alien alike
alive aloe aloft aloha alone amend amino ample amuse angel anger angle ankle apple april apron aqua area arena argue
arise armed armor army aroma array arson art ashen ashes atlas atom attic audio avert avoid awake award awoke axis
bacon badge bagel baggy baked baker balmy banjo barge barn bash basil bask batch bath baton bats blade blank blast
blaze bleak blend bless blimp blink bloat blob blog blot blunt blurt blush boast boat body boil bok bolt boned boney
bonus bony book booth boots boss botch both boxer breed bribe brick bride brim bring brink brisk broad broil broke
brook broom brush buck bud buggy bulge bulk bully bunch bunny bunt bush bust busy buzz cable cache cadet cage cake
calm cameo canal candy cane canon cape card cargo carol carry carve case cash cause cedar chain chair chant chaos
charm chase cheek cheer chef chess chest chew chief chili chill chip chomp chop chow chuck chump chunk churn chute
cider cinch city civic civil clad claim clamp clap clash clasp class claw clay clean clear cleat cleft clerk click
cling clink clip cloak clock clone cloth cloud clump coach coast coat cod coil coke cola cold colt coma come comic
comma cone cope copy coral cork cost cot couch cough cover cozy craft cramp crane crank crate crave crawl crazy creme
crepe crept crib cried crisp crook crop cross crowd crown crumb crush crust cub cult cupid cure curl curry curse curve
curvy cushy cut cycle dab dad daily dairy daisy dance dandy darn dart dash data date dawn deaf deal dean debit debt
debug decaf decal decay deck decor decoy deed delay denim dense dent depth derby desk dial diary dice dig dill dime
dimly diner dingy disco dish disk ditch ditzy dizzy dock dodge doing doll dome donor donut dose dot dove down dowry
doze drab drama drank draw dress dried drift drill drive drone droop drove drown drum dry duck duct dude dug duke duo
dusk dust duty dwarf dwell eagle early earth easel east eaten eats ebay ebony ebook echo edge eel eject elbow elder
elf elk elm elope elude elves email emit empty emu enter entry envoy equal erase error erupt essay etch evade even
evict evil evoke exact exit fable faced fact fade fall false fancy fang fax feast feed femur fence fend ferry fetal
fetch fever fiber fifth fifty film filth final finch fit five flag flaky flame flap flask fled flick fling flint flip
flirt float flock flop floss flyer foam foe fog foil folic folk food fool found fox foyer frail frame fray fresh fried
frill frisk from front frost froth frown froze fruit gag gains gala game gap gas gave gear gecko geek gem genre gift
gig gills given giver glad glass glide gloss glove glow glue goal going golf gong good gooey goofy gore gown grab
grain grant grape graph grasp grass grave gravy gray green greet grew grid grief grill grip grit groom grope growl
grub grunt guide gulf gulp gummy guru gush gut guy habit half halo halt happy harm hash hasty hatch hate haven hazel
hazy heap heat heave hedge hefty help herbs hers hub hug hula hull human humid hump hung hunk hunt hurry hurt hush hut
ice icing icon icy igloo image ion iron islam issue item ivory ivy jab jam jaws jazz jeep jelly jet jiffy job jog
jolly jolt jot joy judge juice juicy july jumbo jump junky juror jury keep keg kept kick kilt king kite kitty kiwi
knee knelt koala kung ladle lady lair lake lance land lapel large lash lasso last latch late lazy left legal lemon
lend lens lent level lever lid life lift lilac lily limb limes line lint lion lip list lived liver lunar lunch lung
lurch lure lurk lying lyric mace maker malt mama mango manor many map march mardi marry mash match mate math moan
mocha moist mold mom moody mop morse most motor motto mount mouse mousy mouth move movie mower mud mug mulch mule mull
mumbo mummy mural muse music musky mute nacho nag nail name nanny nap navy near neat neon nerd nest net next niece
ninth nutty oak oasis oat ocean oil old olive omen onion only ooze opal open opera opt otter ouch ounce outer oval
oven owl ozone pace pagan pager palm panda panic pants panty paper park party pasta patch path patio payer pecan penny
pep perch perky perm pest petal petri petty photo plank plant plaza plead plot plow pluck plug plus poach pod poem
poet pogo point poise poker polar polio polka polo pond pony poppy pork poser pouch pound pout power prank press print
prior prism prize probe prong proof props prude prune pry pug pull pulp pulse puma punch punk pupil puppy purr purse
push putt quack quake query quiet quill quilt quit quota quote rabid race rack radar radio raft rage raid rail rake
rally ramp ranch range rank rant rash raven reach react ream rebel recap relax relay relic remix repay repel reply
rerun reset rhyme rice rich ride rigid rigor rinse riot ripen rise risk ritzy rival river roast robe robin rock rogue
roman romp rope rover royal ruby rug ruin rule runny rush rust rut sadly sage said saint salad salon salsa salt same
sandy santa satin sauna saved savor sax say scale scam scan scare scarf scary scoff scold scoop scoot scope score
scorn scout scowl scrap scrub scuba scuff sect sedan self send sepia serve set seven shack shade shady shaft shaky
sham shape share sharp shed sheep sheet shelf shell shine shiny ship shirt shock shop shore shout shove shown showy
shred shrug shun shush shut shy sift silk silly silo sip siren sixth size skate skew skid skier skies skip skirt skit
sky slab slack slain slam slang slash slate slaw sled sleek sleep sleet slept slice slick slimy sling slip slit slob
slot slug slum slurp slush small smash smell smile smirk smog snack snap snare snarl sneak sneer sniff snore snort
snout snowy snub snuff speak speed spend spent spew spied spill spiny spoil spoke spoof spool spoon sport spot spout
spray spree spur squad squat squid stack staff stage stain stall stamp stand stank stark start stash state stays steam
steep stem step stew stick sting stir stock stole stomp stony stood stool stoop stop storm stout stove straw stray
strut stuck stud stuff stump stung stunt suds sugar sulk surf sushi swab swan swarm sway swear sweat sweep swell swept
swim swing swipe swirl swoop swore syrup tacky taco tag take tall talon tamer tank taper taps tarot tart task taste
tasty taunt thank thaw theft theme thigh thing think thong thorn those throb thud thumb thump thus tiara tidal tidy
tiger tile tilt tint tiny trace track trade train trait trap trash tray treat tree trek trend trial tribe trick trio
trout truce truck trump trunk try tug tulip tummy turf tusk tutor tutu tux tweak tweet twice twine twins twirl twist
uncle uncut undo unify union unit untie upon upper urban used user usher utter value vapor vegan venue verse vest veto
vice video view viral virus visa visor vixen vocal voice void volt voter vowel wad wafer wager wages wagon wake walk
wand wasp watch water wavy wheat whiff whole whoop wick widen widow width wife wifi wilt wimp wind wing wink wipe
wired wiry wise wish wispy wok wolf womb wool woozy word work worry wound woven wrath wreck wrist xerox yahoo yam yard
year yeast yelp yield yolk yodel yoga yoyo yummy zebra zero zesty zippy zone zoom`.trim().split(/\s+/);
  const WORD_SET = new Set(WORDS);
  const PREFIX = 'mc3', PHRASE_WORDS = 8, KDF_ROUNDS = 100000;
  const SALT = 'mc3-web keyphrase saves v1', AAD = SALT;
  const MAX_BOX = 512 << 10;                   // what the server takes (plan 1.4)
  const MAX_PLAIN = 16 << 20;                  // the most a box inflates to
  const KEEPALIVE_MAX = 60 << 10;              // browsers cap a keepalive request's body at 64 KB
  const PUSH_GAP = 5 * 60e3;                   // automatic sends (and their tries): at most one every 5 min
  const QUIET = 5e3;                           // after the last save signal (the game saves in bursts)
  const PULL_GAP = 60e3;                       // an automatic pull within a minute of the last asks nothing
  const BACKOFF_MIN = 60e3, BACKOFF_MAX = 60 * 60e3;
  const SENT_MAX = 8;
  const OPROF = 'online/profile.bin';
  // keep in step with index.html's OPROF_BODIES (src/games/remix/recomp_manual.c, "The Online profile")
  const OPROF_MAGIC = 0x4F33434D, OPROF_HDR = 20, OPROF_BODIES = [0, 4 + 0x18340, 8 + 0x18340, 12 + 0x18340, 16 + 0x18340];
  const KEEP = /^UDATA\/54540079\/[0-9A-Za-z]{1,64}\/[^/\\.][^/\\]*$/;
  const enc = new TextEncoder(), dec = new TextDecoder();

  /** A path a keyphrase keeps (a box's other paths are never written). */
  const keeps = p => p === OPROF || (KEEP.test(p) && !p.endsWith('.tmp'));

  class CloudError extends Error {
    /** code: a fixed name; status: the HTTP status; retry: the server's Retry-After, ms */
    constructor(msg, code = 'cloud', status = 0, retry = 0) { super(msg); this.code = code; this.status = status; this.retry = retry; }
  }

  // ------------------------------------------------------------------------------------------------ keyphrase --
  function newPhrase() {
    const words = [], limit = Math.floor(65536 / WORDS.length) * WORDS.length;   // past it a value favours the first words
    while (words.length < PHRASE_WORDS)
      for (const v of crypto.getRandomValues(new Uint16Array(PHRASE_WORDS)))
        if (v < limit && words.length < PHRASE_WORDS) words.push(WORDS[v % WORDS.length]);
    return [PREFIX, ...words].join('-');
  }
  /** A keyphrase as typed or pasted: { phrase } or { bad: why }. */
  function readPhrase(text) {
    let w = String(text || '').toLowerCase().split(/[^a-z0-9]+/).filter(Boolean);
    if (w[0] === PREFIX) w = w.slice(1);
    if (!w.length) return { bad: 'Type or paste your keyphrase.' };
    const unknown = w.find(x => !WORD_SET.has(x));
    if (unknown) return { bad: `"${unknown}" is not a keyphrase word: check the spelling.` };
    if (w.length !== PHRASE_WORDS) return { bad: `A keyphrase has ${PHRASE_WORDS} words after "${PREFIX}"; this has ${w.length}.` };
    return { phrase: [PREFIX, ...w].join('-') };
  }

  const b64url = b => btoa(String.fromCharCode(...b)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  const hex = b => Array.from(b, x => x.toString(16).padStart(2, '0')).join('');

  /** { key: AES-GCM CryptoKey, token: 43 base64url } of a keyphrase */
  async function keysOf(phrase) {
    const base = await crypto.subtle.importKey('raw', enc.encode(phrase), 'PBKDF2', false, ['deriveBits']);
    const bits = new Uint8Array(await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: enc.encode(SALT), iterations: KDF_ROUNDS }, base, 512));
    const key = await crypto.subtle.importKey('raw', bits.slice(0, 32), 'AES-GCM', false, ['encrypt', 'decrypt']);
    return { key, token: b64url(bits.subarray(32)) };
  }
  async function seal(key, plain) {
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv, additionalData: enc.encode(AAD) }, key, plain));
    const out = new Uint8Array(13 + ct.length);
    out[0] = 1; out.set(iv, 1); out.set(ct, 13);
    return out;
  }
  async function unseal(key, box) {
    if (box.length >= 1 && box[0] > 1) throw new CloudError('these saves come from a newer version of the page: reload the page', 'newer-box');
    if (box.length < 13 + 16 || box[0] !== 1) throw new CloudError('what the server holds for this keyphrase is not saves', 'not-saves');
    try { return new Uint8Array(await crypto.subtle.decrypt({ name: 'AES-GCM', iv: box.slice(1, 13), additionalData: enc.encode(AAD) }, key, box.slice(13))); }
    catch { throw new CloudError('these saves do not open with this keyphrase (damaged)', 'damaged'); }
  }

  // ---------------------------------------------------------------------------------------------------- box --
  async function bytesOf(stream, max = Infinity) {
    const rd = stream.getReader(), parts = [];
    let n = 0;
    try {
      for (;;) {
        const { done, value } = await rd.read();
        if (done) break;
        if ((n += value.length) > max) throw new CloudError('these saves are larger than a keyphrase keeps', 'too-large');
        parts.push(value);
      }
    } catch (e) { rd.cancel().catch(() => {}); throw e; }
    const out = new Uint8Array(n);
    let p = 0;
    for (const x of parts) { out.set(x, p); p += x.length; }
    return out;
  }
  /** files [{ path, data }] -> deflated plain */
  async function pack(files) {
    const head = enc.encode(JSON.stringify({ v: 1, guarded: 1, files: files.map(f => [f.path, f.data.length]) }));
    const n = new Uint8Array(4);
    new DataView(n.buffer).setUint32(0, head.length, true);
    return bytesOf(new Blob([n, head, ...files.map(f => f.data)]).stream().pipeThrough(new CompressionStream('deflate-raw')));
  }
  /** pack()'s files again, only the paths a keyphrase keeps */
  async function unpack(packed) {
    let plain;
    try { plain = await bytesOf(new Blob([packed]).stream().pipeThrough(new DecompressionStream('deflate-raw')), MAX_PLAIN); }
    catch (e) { throw e instanceof CloudError ? e : new CloudError(`these saves are damaged (${e.message || e})`, 'damaged'); }
    let head;
    try {
      const hl = new DataView(plain.buffer, plain.byteOffset, plain.length).getUint32(0, true);
      head = JSON.parse(dec.decode(plain.subarray(4, 4 + hl)));
      head.at = 4 + hl;
    } catch { throw new CloudError('these saves are damaged (layout)', 'damaged'); }
    if (head.v !== 1) throw new CloudError('these saves come from a newer version of the page: reload the page', 'newer-box');
    const files = [];
    let at = head.at;
    for (const [path, len] of head.files) {
      if (!(len >= 0) || at + len > plain.length) throw new CloudError('these saves are damaged (cut short)', 'damaged');
      if (keeps(path)) files.push({ path, data: plain.slice(at, at + len) });
      at += len;
    }
    return files;
  }
  /** What the files hold, whatever their times: SHA-256 over paths and bytes, in path order. */
  async function contentHash(files) {
    const sorted = [...files].sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
    const parts = sorted.flatMap(f => {
      const name = enc.encode(f.path), h = new Uint8Array(8), v = new DataView(h.buffer);
      v.setUint32(0, name.length); v.setUint32(4, f.data.length);
      return [h, name, f.data];
    });
    return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', await new Blob(parts).arrayBuffer())));
  }

  // ------------------------------------------------------------------------------------------------- checks --
  let zeroKey = null;
  async function signed(d) {            // web_fs.c sig_ok: last 20 bytes = HMAC-SHA1 of the rest under 16 zero bytes
    if (d.length <= 20) return false;
    zeroKey = zeroKey || await crypto.subtle.importKey('raw', new Uint8Array(16), { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
    const h = new Uint8Array(await crypto.subtle.sign('HMAC', zeroKey, d.subarray(0, d.length - 20)));
    return h.every((x, i) => x === d[d.length - 20 + i]);
  }
  /** web_fs.c career_good: not empty; against its newest copy r: not shorter, the same 4-byte stamp, signed when r is */
  async function careerOk(d, r) {
    if (!d || !d.length) return false;
    if (!r || !r.length) return true;
    if (d.length < r.length) return false;
    if (d.length >= 4 && r.length >= 4 && (d[0] !== r[0] || d[1] !== r[1] || d[2] !== r[2] || d[3] !== r[3])) return false;
    return !(await signed(r)) || await signed(d);
  }
  /** 'ok', 'newer' (a version above this page's) or 'bad' (index.html oprofCheck) */
  function oprofState(d) {
    if (d.length < OPROF_HDR) return 'bad';
    const v = new DataView(d.buffer, d.byteOffset, d.length), ver = v.getUint32(4, true);
    if (v.getUint32(0, true) !== OPROF_MAGIC) return 'bad';
    if (ver >= OPROF_BODIES.length) return 'newer';
    if (!OPROF_BODIES[ver] || v.getUint32(8, true) !== OPROF_BODIES[ver] || d.length !== OPROF_HDR + OPROF_BODIES[ver]) return 'bad';
    return v.getUint32(12, true) === crc32(d.subarray(OPROF_HDR)) ? 'ok' : 'bad';
  }
  const crcTable = [...Array(256)].map((_, n) => { for (let k = 0; k < 8; k++) n = n & 1 ? 0xEDB88320 ^ (n >>> 1) : n >>> 1; return n >>> 0; });
  const crc32 = b => { let c = ~0; for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return ~c >>> 0; };

  /**
   * The files by unit (a profile folder, or 'online'): profiles = units with their main file (.mc3Remix/.mc3,
   * profile.bin), torn = those with a file failing its check, ok = the paths that pass, newer = a profile.bin of a
   * later version. refs: path -> its newest rotating copy (.bak1, else .bak2) in this browser.
   */
  async function survey(files, refs = {}) {
    const units = new Map(), ok = new Set();
    let newer = false;
    for (const f of files) {
      const u = f.path === OPROF ? 'online' : f.path.slice(0, f.path.lastIndexOf('/'));
      const e = units.get(u) || { main: false, bad: false };
      units.set(u, e);
      if (f.path === OPROF || /\.mc3(remix)?$/i.test(f.path)) e.main = true;
      const c = f.path === OPROF ? oprofState(f.data) : await careerOk(f.data, refs[f.path]) ? 'ok' : 'bad';
      if (c === 'ok') ok.add(f.path);
      else { e.bad = true; if (c === 'newer') newer = true; }
    }
    let profiles = 0, torn = 0;
    for (const e of units.values()) if (e.main) { profiles++; if (e.bad) torn++; }
    return { units, ok, newer, profiles, torn, whole: profiles - torn };
  }
  /** one side of a conflict: profile names (SaveMeta Name=), the online profile's money and cars, when saved */
  function sideOf(files, saved) {
    const profiles = [];
    let online = null;
    for (const f of files) {
      if (/\/SaveMeta\.xbx$/.test(f.path)) {
        const m = /Name=([^\r\n\0]*)/.exec(new TextDecoder('utf-16le').decode(f.data));
        profiles.push(m ? m[1].replace(/^Remix : /, '') : f.path.split('/')[2]);
      } else if (f.path === OPROF && f.data.length >= OPROF_HDR + 4) {
        const v = new DataView(f.data.buffer, f.data.byteOffset, f.data.length);
        online = { money: v.getInt32(OPROF_HDR, true), cars: v.getUint32(16, true) };
      }
    }
    const times = files.map(f => f.mtime).filter(Boolean);
    return { saved: saved ?? (times.length ? Math.max(...times) : null), profiles: profiles.sort(), online };
  }

  // ------------------------------------------------------------------------------------------------- server --
  function remoteOf(r) {
    const rev = r.headers.get('etag'), saved = Number(r.headers.get('x-vault-saved')), e = r.headers.get('x-vault-earlier');
    return rev && saved ? { rev, saved, earlier: e ? Number(e) : null } : null;
  }
  function failure(r) {
    const ra = Number(r.headers.get('retry-after'));
    const msg = r.status === 429 ? 'the server asks to wait (too many requests)'
      : r.status === 507 ? 'the server has no room for more saves now'
      : r.status === 503 ? 'the server takes no new cloud saves for now: try again later'
      : r.status === 413 ? 'these saves are larger than the server takes'
      : `the server answered ${r.status}`;
    return new CloudError(msg, 'http', r.status, Number.isFinite(ra) && ra > 0 ? ra * 1000 : 0);
  }
  const incomplete = () => new CloudError('the server\'s answer is incomplete', 'incomplete');

  /** savesUrl + '/v' for one token (plan 1.3) */
  class Api {
    constructor(url, token, f, timeoutMs = 30000) { this.url = url; this.token = token; this.f = f; this.timeoutMs = timeoutMs; }
    req(method, headers = {}, body, query = '', keepalive = false) {
      return this.f(this.url + query, {
        method, headers: { authorization: `Bearer ${this.token}`, ...headers }, body, cache: 'no-store', keepalive,
        credentials: 'include',              // the disc Worker's gate (Cloudflare Access cookie), as disc_cache.js
        ...keepalive ? {} : { signal: AbortSignal.timeout(this.timeoutMs) },
      });
    }
    /** null: none; { same: remote }: still rev; { box, remote } */
    async get(rev, earlier = false) {
      const r = await this.req('GET', rev && !earlier ? { 'if-none-match': rev } : {}, undefined, earlier ? '?earlier' : '');
      if (r.status === 404) return null;
      const remote = remoteOf(r);
      if (r.status === 304) { if (!remote) throw incomplete(); return { same: remote }; }
      if (!r.ok) throw failure(r);
      if (!remote) throw incomplete();
      return { box: new Uint8Array(await r.arrayBuffer()), remote };
    }
    /** over rev (null: a new keyphrase): { saved } or { conflict: remote | null } */
    async put(box, rev) {
      const r = await this.req('PUT', rev ? { 'if-match': rev } : { 'if-none-match': '*' }, box);
      if (r.status === 412) return { conflict: remoteOf(r) };
      if (!r.ok) throw failure(r);
      const remote = remoteOf(r);
      if (!remote) throw incomplete();
      return { saved: remote };
    }
    async remove() {
      const r = await this.req('DELETE');
      if (r.status === 404) return false;
      if (!r.ok) throw failure(r);
      return true;
    }
  }
  /** this server keeps cloud saves: savesUrl/v without a token answers 401 */
  async function available(url, f = (u, i) => fetch(u, i)) {
    try { return (await f(url, { cache: 'no-store', credentials: 'include' })).status === 401; } catch { return false; }
  }
  function failureText(e) {
    return e instanceof CloudError ? e.message
      : e && (e.name === 'TimeoutError' || e.name === 'AbortError') ? 'the server did not answer in time'
      : e instanceof TypeError ? 'the server does not answer'
      : String(e && e.message || e);
  }

  // -------------------------------------------------------------------------------------------------- sync --
  /**
   * deps: url (savesUrl/v), read() -> { files: [{ path, data, mtime }], refs: { path: bytes } } (the saves here and their
   * newest rotating copies), apply(files) (snapshot first, then overlay; throws when nothing could be written),
   * load()/save(link) (localStorage mc3.cloud), fetch, log(line), playing() (a game runs), now(), setTimer(fn, ms),
   * clearTimer(id).
   *
   * link: { phrase, rev, hash, saved, earlier, profiles, whole, torn, dropped, sent[], gone }
   * state.s: off | synced | working (what) | error (msg) | conflict (why, theirs, here, cloud) | gone | other
   * conflict why: both, load, older, bare, thin, newer, bad (pull); empty, fewer (push)
   */
  class Sync {
    constructor(d) {
      this.d = d;
      this.now = d.now || Date.now;
      this.f = d.fetch || ((u, i) => fetch(u, i));
      this.link = d.load() || null;
      this.other = false;                 // another tab holds the lock (lock())
      this.state = this.rest();
      this.onChange = () => {};
      this.keys = null;
      this.queue = Promise.resolve();
      this.backoff = 0; this.waitUntil = 0;   // automatic tries wait until then
      this.serverUntil = 0;                   // the server asked to wait (Retry-After): every try waits
      this.lastTry = 0; this.lastPull = 0;
      this.dirty = false; this.sig = 0; this.lastSig = 0; this.timer = null; this.ready = null;
    }
    log(s) { this.d.log?.(s); }
    rest() { return this.other ? { s: 'other' } : !this.link ? { s: 'off' } : this.link.gone ? { s: 'gone' } : { s: 'synced' }; }
    set(s) { this.state = s; this.onChange(); }
    setLink(l) { this.link = l; this.d.save(l); }
    playing() { return !!this.d.playing?.(); }

    /** One syncing tab per browser (plan 4.5): false = another tab syncs. */
    lock() {
      const locks = G.navigator && G.navigator.locks;
      if (!locks) return Promise.resolve(true);
      return new Promise(res => locks.request('mc3-cloud', { ifAvailable: true }, l => {
        this.other = !l;
        res(!!l);
        if (!this.other) return new Promise(() => {});   // held for the page's life
        this.set(this.rest());
      })).catch(() => true);
    }

    async keysFor(phrase) {
      if (this.keys?.phrase !== phrase) this.keys = { phrase, k: await keysOf(phrase) };
      return this.keys.k;
    }
    api(k, timeoutMs) { return new Api(this.d.url, k.token, this.f, timeoutMs); }

    // anti-spam: what waits after a failure
    failed(e) {
      const now = this.now(), st = e && e.status;
      if (st === 429 || st === 503 || (e && e.retry)) this.serverUntil = now + Math.max(e.retry || 0, BACKOFF_MIN);
      this.backoff = st === 413 || st === 507 || st === 401 || st === 403 ? BACKOFF_MAX : Math.min(Math.max(this.backoff * 2, BACKOFF_MIN), BACKOFF_MAX);
      this.waitUntil = Math.max(now + this.backoff, this.serverUntil);
    }
    answered() { this.backoff = 0; this.waitUntil = this.serverUntil; }
    /** auto: an automatic try (page open, Play, a save signal) */
    blocked(auto) { const now = this.now(); return now < this.serverUntil || (auto && now < this.waitUntil); }
    waitText() { return `the server asked to wait: try again in ${Math.max(1, Math.ceil((this.serverUntil - this.now()) / 60e3))} min`; }

    /** one job at a time; a failure becomes the error state (never thrown to the caller) */
    run(what, job) {
      const p = this.queue.then(async () => {
        this.set({ s: 'working', what });
        try { return await job(); }
        catch (e) { this.failed(e); this.log(`[cloud] ${what.toLowerCase()} failed: ${failureText(e)}`); this.set({ s: 'error', msg: failureText(e) }); }
        finally { if (this.state.s === 'working') this.set(this.rest()); }
      });
      this.queue = p.catch(() => {});
      return p;
    }
    refuse(auto) {           // a try while the server asks to wait (manual) says so
      if (!auto) this.set({ s: 'error', msg: this.waitText() });
      return Promise.resolve();
    }

    async local() {
      const r = await this.d.read(), files = r.files.filter(f => keeps(f.path));
      return { files, refs: r.refs || {}, s: await survey(files, r.refs), hash: await contentHash(files) };
    }
    async boxOf(k, files) {
      const box = await seal(k.key, await pack(files));
      if (box.length > MAX_BOX) throw new CloudError(`these saves pack into ${Math.ceil(box.length / 1024)} KB, more than the server takes (${MAX_BOX >> 10} KB)`, 'too-large');
      return box;
    }
    /** the version now: { remote, files, hash } or null */
    async current(k, api) {
      const r = await api.get();
      if (!r) return null;
      if (!r.box) throw incomplete();
      const files = await unpack(await unseal(k.key, r.box));
      return { remote: r.remote, files, hash: await contentHash(files) };
    }
    linked(phrase, r, hash, s) {
      this.setLink({ phrase, rev: r.rev, hash, saved: r.saved, earlier: r.earlier, profiles: s.profiles, whole: s.whole, torn: s.torn });
    }
    sending(hash) {
      const l = this.link;
      this.setLink({ ...l, sent: [...(l.sent || []).filter(h => h !== hash), hash].slice(-SENT_MAX) });
      return this.link;
    }
    async goneNow() {
      if (!await available(this.d.url, this.f)) throw new CloudError('the server keeps no cloud saves now', 'off');
      if (this.link) this.setLink({ ...this.link, gone: true, earlier: null });
      this.log('[cloud] the keyphrase is not on the server any more: nothing is sent by itself');
      this.set({ s: 'gone' });
    }
    clash(why, theirs, here, cloud) {
      this.log(`[cloud] conflict: ${why}`);
      this.set({ s: 'conflict', why, theirs, here: sideOf(here), cloud: cloud ? sideOf(cloud, theirs?.saved) : theirs ? { saved: theirs.saved, profiles: null, online: null } : null });
    }
    /** a question about the saves here, the cloud's version beside them */
    async ask(why, files) {
      let c = null;
      try { const k = await this.keysFor(this.link.phrase); c = await this.current(k, this.api(k)); } catch { /* not known */ }
      this.clash(why, c?.remote || null, files, c?.files);
    }
    /** a version written here (snapshot first, overlay; the files passing their checks only), the link to it */
    async take(phrase, theirs, ts, remote) {
      if (this.playing()) throw new CloudError('saves are only replaced before Play: reload the page, then choose', 'playing');
      const good = theirs.filter(f => ts.ok.has(f.path));
      if (good.length < theirs.length) this.log(`[cloud] ${theirs.length - good.length} file(s) of the cloud's fail their checks: left out`);
      await this.d.apply(good);
      this.linked(phrase, remote, await contentHash(good), await survey(good));
      this.ready = null;
      this.log(`[cloud] pulled: ${good.length} file(s) written (a snapshot of the saves here was taken first)`);
    }

    /** Another browser's newer version, taken by itself only when it is safe (plan 4.2). */
    pull(o = {}) {
      if (!this.link || this.state.s === 'conflict' || this.other) return Promise.resolve();
      if (this.blocked(o.auto)) return this.refuse(o.auto);
      if (o.auto && this.now() - this.lastPull < PULL_GAP) return Promise.resolve();
      this.lastPull = this.now();
      return this.run('Checking', async () => {
        let l = this.link;
        const k = await this.keysFor(l.phrase), r = await this.api(k, o.timeoutMs).get(l.rev);
        this.answered();
        if (!r) return this.goneNow();
        if (l.gone) this.setLink(l = { ...l, gone: false });
        if (r.same) { if (r.same.earlier !== l.earlier) this.setLink({ ...l, earlier: r.same.earlier }); return; }
        const theirs = await unpack(await unseal(k.key, r.box)), here = await this.local();
        const theirHash = await contentHash(theirs), ts = await survey(theirs, here.refs);
        if (theirHash === here.hash) return this.linked(l.phrase, r.remote, here.hash, here.s);
        // this browser's own, sent before without an answer: the link follows it; the next push sends what changed
        if (l.sent?.includes(theirHash)) return this.linked(l.phrase, r.remote, theirHash, ts);
        if (this.playing()) return;        // never under a running game
        const hs = here.s, lacks = [...hs.units].some(([u, e]) => e.main && !ts.units.get(u)?.main);
        const why = here.hash !== l.hash ? 'both' : r.remote.saved < l.saved ? 'older' : !ts.profiles && hs.profiles ? 'bare'
          : lacks ? 'thin' : ts.newer ? 'newer' : ts.torn ? 'bad' : null;
        if (why) return this.clash(why, r.remote, here.files, theirs);
        await this.take(l.phrase, theirs, ts, r.remote);
      });
    }

    /** The saves here sent when they changed since the last sync (plan 4.1). playing: a game runs. */
    push(o = {}) {
      if (!this.link || this.link.gone || this.state.s === 'conflict' || this.other) return Promise.resolve();
      if (this.blocked(o.auto)) return this.refuse(o.auto);
      if (o.auto) this.lastTry = this.now();
      return this.run('Syncing', async () => {
        this.dirty = false;
        const p = await this.prepare(o);
        if (p) await this.send(p, this.link.rev);
      });
    }
    /** what a push would send, or null (unchanged, waits, or asked); dry: no question asked (makeReady) */
    async prepare(o, dry = false) {
      let h;
      try { h = await this.local(); }
      catch (e) {
        if (!o.playing) throw e;
        this.dirty = true;                 // a file the game has open: the next try reads it
        this.log(`[cloud] saves busy (${e.name || e}): sent at the next try`);
        return null;
      }
      const l = this.link, s = h.s;
      if (o.playing && s.torn > (l.torn || 0)) { this.dirty = true; return null; }   // a profile being written
      if (h.hash === l.hash && !l.sent?.length) return null;
      if (!s.profiles && l.profiles > 0 && (l.dropped || 0) < (l.whole ?? l.profiles)) { if (!dry) await this.ask('empty', h.files); return null; }
      if (s.torn > (l.torn || 0) || s.whole < (l.whole ?? l.profiles) - (l.dropped || 0)) {
        if (!dry) {
          this.log(`[cloud] ${s.whole} whole profile(s) here and ${s.torn} not, ${l.whole ?? l.profiles} and ${l.torn || 0} at the last sync: nothing sent, asked`);
          await this.ask('fewer', h.files);
        }
        return null;
      }
      return h;
    }
    /** over rev; the server's other version: the same files linked, our own unanswered one sent over once, else a conflict */
    async send(p, rev) {
      const k = await this.keysFor(this.link.phrase), api = this.api(k), box = await this.boxOf(k, p.files);
      for (let again = false; ; again = true) {
        let l = this.sending(p.hash);
        const r = await api.put(box, rev);
        this.answered();
        if (r.saved) {
          this.linked(l.phrase, r.saved, p.hash, p.s);
          this.ready = null;
          this.lastTry = this.now();        // the server refuses a write within 60 s of the last (429): the next automatic one waits 5 min
          this.log(`[cloud] pushed ${Math.ceil(box.length / 1024)} KB (${p.s.profiles} profile(s))`);
          return;
        }
        const c = r.conflict && await this.current(k, api);
        if (!c) return this.goneNow();
        if (c.hash === p.hash) return this.linked(l.phrase, c.remote, p.hash, p.s);
        if (again || !l.sent?.includes(c.hash)) {
          if (l.gone) this.setLink({ ...l, gone: false });
          return this.clash('both', c.remote, p.files, c.files);
        }
        this.linked(l.phrase, c.remote, c.hash, await survey(c.files, p.refs));
        rev = c.remote.rev;
      }
    }

    // ---- triggers (plan 5): a save signal, its debounce, pagehide
    /** The game wrote a save (index.html polls mc3_saves_box); also a page write (import, restore, delete). */
    saved() {
      this.sig++; this.dirty = true; this.lastSig = this.now();
      if (this.link) this.schedule();
    }
    autoOk() { return this.link && !this.link.gone && this.state.s !== 'conflict' && !this.other; }
    schedule() {
      if (this.timer) this.d.clearTimer(this.timer);
      this.timer = null;
      if (!this.dirty || !this.autoOk()) return;
      const now = this.now(), quietAt = this.lastSig + QUIET;
      const sendAt = Math.max(quietAt, this.lastTry + PUSH_GAP, this.waitUntil, this.serverUntil);
      const at = !this.ready && quietAt < sendAt ? quietAt : sendAt;
      this.timer = this.d.setTimer(() => { this.timer = null; return this.tick(); }, Math.max(0, at - now));
    }
    async tick() {
      if (!this.dirty || !this.autoOk()) return;
      const now = this.now();
      if (now >= this.lastSig + QUIET && now >= Math.max(this.lastTry + PUSH_GAP, this.waitUntil, this.serverUntil)) await this.push({ playing: this.playing(), auto: true });
      else if (!this.ready && now >= this.lastSig + QUIET) await this.makeReady();
      this.schedule();
    }
    /** the box the next send would be, sealed now, for pagehide (flush) while the 5 minutes run */
    async makeReady() {
      const sig = this.sig;
      try {
        const p = await this.prepare({ playing: this.playing() }, true);
        if (!p || sig !== this.sig || !this.link) return;
        const k = await this.keysFor(this.link.phrase);
        this.ready = { box: await this.boxOf(k, p.files), hash: p.hash, sig, rev: this.link.rev };
      } catch { /* the send itself says why */ }
    }
    /** pagehide: the ready box sent with keepalive (64 KB at most), its answer never read: Link.sent covers it */
    flush() {
      const r = this.ready, l = this.link;
      if (!this.dirty || !r || r.sig !== this.sig || !l || r.rev !== l.rev || !this.autoOk() || this.state.s === 'working'
          || r.box.length > KEEPALIVE_MAX || this.keys?.phrase !== l.phrase || this.blocked(true)) return false;
      this.sending(r.hash);
      try { new Api(this.d.url, this.keys.k.token, this.f).req('PUT', l.rev ? { 'if-match': l.rev } : { 'if-none-match': '*' }, r.box, '', true).catch(() => {}); }
      catch { return false; }
      this.ready = null;
      this.log('[cloud] page closing: the waiting saves sent');
      return true;
    }

    // ---- the player's actions
    /** A new keyphrase for the saves here, their first version sent: the keyphrase, or null. */
    create() {
      if (this.blocked(false)) return this.refuse(false).then(() => null);
      return this.run('Making keyphrase', async () => {
        const h = await this.local();
        for (let i = 0; i < 3; i++) {
          const phrase = newPhrase(), k = await this.keysFor(phrase), r = await this.api(k).put(await this.boxOf(k, h.files), null);
          this.answered();
          if (r.conflict) continue;          // one of 2^83 taken already
          this.linked(phrase, r.saved, h.hash, h.s);
          this.lastTry = this.now();
          this.log('[cloud] keyphrase made, the saves here sent');
          return phrase;
        }
        throw new CloudError('no free keyphrase (try again)', 'taken');
      });
    }
    /** Link with a keyphrase typed in: { bad } or {} (linked; a conflict when this browser has other saves). */
    load(text) {
      const p = readPhrase(text);
      if (p.bad) return Promise.resolve({ bad: p.bad });
      if (this.blocked(false)) return this.refuse(false).then(() => ({ bad: this.waitText() }));
      return this.run('Loading', async () => {
        const k = await this.keysFor(p.phrase), r = await this.api(k).get();
        this.answered();
        if (!r) return { bad: 'No saves under this keyphrase.' };
        const theirs = await unpack(await unseal(k.key, r.box)), here = await this.local();
        const theirHash = await contentHash(theirs), ts = await survey(theirs, here.refs);
        if (theirHash === here.hash) { this.linked(p.phrase, r.remote, here.hash, here.s); return {}; }
        if (!here.files.length && !ts.newer) { await this.take(p.phrase, theirs, ts, r.remote); return {}; }
        // rev null, hash '': any sync before the player picks finds the conflict again (never sends by itself)
        this.setLink({ phrase: p.phrase, rev: null, hash: '', saved: 0, earlier: r.remote.earlier, profiles: here.s.profiles, whole: here.s.whole, torn: here.s.torn });
        this.clash(ts.newer ? 'newer' : 'load', r.remote, here.files, theirs);
        return {};
      }).then(x => x || { bad: this.state.msg || 'not loaded' });
    }
    /** A conflict settled for the saves here: sent over the cloud's version. */
    keepMine() {
      const st = this.state;
      if (!this.link || st.s !== 'conflict' || st.why === 'newer') return Promise.resolve();
      if (this.blocked(false)) return this.refuse(false);
      return this.run('Syncing', async () => {
        const l = this.link, h = await this.local(), k = await this.keysFor(l.phrase), api = this.api(k), box = await this.boxOf(k, h.files);
        this.sending(h.hash);
        let r = await api.put(box, st.theirs?.rev ?? l.rev);
        if (r.conflict?.rev) r = await api.put(box, r.conflict.rev);
        this.answered();
        if (r.saved) { this.linked(l.phrase, r.saved, h.hash, h.s); this.dirty = false; this.lastTry = this.now(); this.log('[cloud] conflict settled: this browser\'s saves sent'); return; }
        if (!r.conflict) return this.goneNow();
        this.clash('both', r.conflict, h.files);
      });
    }
    /** A conflict settled for the cloud's version: it overlays the saves here (snapshot first). */
    takeTheirs() {
      const st = this.state;
      if (!this.link || st.s !== 'conflict' || st.why === 'newer') return Promise.resolve();
      if (this.blocked(false)) return this.refuse(false);
      return this.run('Loading', async () => {
        const l = this.link, k = await this.keysFor(l.phrase), c = await this.current(k, this.api(k));
        this.answered();
        if (!c) return this.goneNow();
        const here = await this.local();
        await this.take(l.phrase, c.files, await survey(c.files, here.refs), c.remote);
      });
    }
    /** The keyphrase gone from the server: the saves here sent under it again. */
    resend() {
      if (!this.link?.gone) return Promise.resolve();
      if (this.blocked(false)) return this.refuse(false);
      return this.run('Syncing', async () => {
        this.setLink({ ...this.link, gone: false });
        await this.send(await this.local(), null);
      });
    }
    /** The copy of an earlier day put back here (snapshot first), then sent as the current version. */
    async goBack() {
      if (!this.link?.earlier || this.blocked(false)) return this.refuse(false);
      let done = false;
      await this.run('Loading', async () => {
        if (this.playing()) throw new CloudError('saves are only replaced before Play: reload the page first', 'playing');
        const l = this.link, k = await this.keysFor(l.phrase), r = await this.api(k).get(null, true);
        this.answered();
        if (!r || !r.box) throw new CloudError('no copy of an earlier day on the server', 'none');
        const here = await this.local(), theirs = await unpack(await unseal(k.key, r.box)), ts = await survey(theirs, here.refs);
        if (ts.newer) throw new CloudError('that copy comes from a newer version of the game: reload the page', 'newer');
        await this.d.apply(theirs.filter(f => ts.ok.has(f.path)));
        this.intended();
        done = true;
      });
      if (done) return this.push({});
    }
    /** The player changed the saves here on purpose (import, restore, delete): fewer profiles then is no loss. */
    intended() { const l = this.link; if (l) this.setLink({ ...l, dropped: Math.max(l.dropped || 0, l.whole ?? l.profiles) }); }
    removed(n = 1) { const l = this.link; if (l) this.setLink({ ...l, dropped: (l.dropped || 0) + n }); }
    /** "Decide later" keeps the conflict (asked again at Play); a conflict's sides may be stale: re-judged at the next try */
    reopen() { if (this.state.s === 'conflict') this.set(this.rest()); }
    /** This browser stops syncing; the saves stay here and on the server. */
    forget() { return this.run('Stopping', async () => { this.setLink(null); this.dirty = false; this.ready = null; }); }
    /** The keyphrase and its versions deleted from the server, this browser unlinked. */
    remove() {
      if (!this.link) return Promise.resolve();
      if (this.blocked(false)) return this.refuse(false);
      return this.run('Deleting', async () => {
        await this.api(await this.keysFor(this.link.phrase)).remove();
        this.answered();
        this.setLink(null); this.dirty = false; this.ready = null;
        this.log('[cloud] keyphrase deleted from the server');
      });
    }
  }

  G.MC3Cloud = {
    WORDS, PREFIX, PHRASE_WORDS, KDF_ROUNDS, MAX_BOX, KEEPALIVE_MAX, PUSH_GAP, QUIET, PULL_GAP, BACKOFF_MIN, BACKOFF_MAX, OPROF,
    keeps, newPhrase, readPhrase, keysOf, seal, unseal, pack, unpack, contentHash, careerOk, oprofState, survey, sideOf,
    available, failureText, CloudError, Api, Sync,
  };
})(typeof window != 'undefined' ? window : globalThis);
