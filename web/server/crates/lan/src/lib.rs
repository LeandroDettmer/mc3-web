//! The virtual LAN switch for System Link: every browser that joins gets a virtual IPv4 address and the game's
//! datagrams go from browser to browser through it.  After nfsu2-web's switch (web/server-rs/crates/lan, MIT License,
//! Copyright (c) 2026 th0mk): its addresses, frame layout and control messages, without what only NFSU2 needs there
//! (the Online host, bundles, compact frames, resume tokens, names and bans).  Frames to [`HUB`] go to the free-roam
//! hub (crates/roam, [`Switch::attach_hub`]), which answers through [`Switch::inject`]; a client's {"t":"roam"} gets
//! the hub's players ({"t":"roam","players":[{"name","status"}]}, [`Switch::set_roam_list`]).
//!
//! # Addresses
//!
//! 10.77.0.0/18 ([`MASK`]): 250 hosts in each of 10.77.0-63.x (host bytes 2-251), handed out in turn across the
//! sixty-four /24s ([`vip_at`]: 10.77.0.2, 10.77.1.2, ..., 10.77.63.2, 10.77.0.3, ...), so a title that takes the
//! network for a /24 shows it at once.  10.77.0.1 ([`SERVER`]) and 10.77.0.253 ([`HUB`], a free-roam hub as in
//! nfsu2-web) are never handed out: they stay free for the server's own hosts.
//!
//! # Frames
//!
//! Binary WebSocket messages, big-endian:
//!
//! ```text
//! [0]      u8  kind (opaque here; the runtime's 3 = a LAN UDP datagram, xboxrecomp/src/net/xnet_web.c)
//! [1..3]   reserved (0)
//! [4..7]   u32 source vip       (rewritten here to the sender's own address)
//! [8..11]  u32 destination vip  (0xFFFFFFFF: broadcast)
//! [12..13] u16 source port, [14..15] u16 destination port
//! [16..]   the datagram (at most 1472 bytes: an Ethernet MTU's UDP payload)
//! ```
//!
//! A unicast goes to the peer holding the destination address (nowhere if none); a broadcast to every other peer
//! that has System Link open ("lan":1 in its hello, or {"t":"lan","on":1}).  The sender's own sockets get its
//! broadcasts from its runtime, not from here.
//!
//! # Control messages (text, JSON)
//!
//! From a client: {"t":"hello","name":...,"lan":0|1} (again to rename), {"t":"lan","on":0|1}, {"t":"ping","id":n}.
//! A page that reconnects asks for its old address with /lan?vip=n ([`Switch::join_from`]): given back when free.
//! From the switch: {"t":"welcome","vip":n,"vipStr":"10.77.x.y","mask":n,"mac":"00:50:F2:4D:xx:yy"} at once on
//! joining (the MAC is the one the runtime puts in the player's XNADDR), {"t":"peers","peers":[{"vip","vipStr",
//! "name"},...]} to every client that said hello, each time the list changes (us included), {"t":"pong","id":n}.
//!
//! # Rooms (races)
//!
//! docs/races-protocol.md: {"t":"rooms","sub":0|1}, {"t":"create",...}, {"t":"join","room","pass"?}, {"t":"leave"},
//! {"t":"beat"}, {"t":"signal",...} from a client; {"t":"rooms"}, {"t":"room"}, {"t":"left"}, {"t":"error"},
//! {"t":"signal"} from the switch.  A client is in at most one room; a room is its own LAN: broadcasts and unicasts
//! go only between clients of the same room, and clients in no room share the open LAN as before rooms existed.
//! Free roam's port ([`ROAM_PORT`]) and the hub route whatever the rooms.  The host leaving closes the room.  Only a
//! salted SHA-1 verifier of a room's password is kept.  [`RoomLimits`]: members, rooms, rooms hosted per address.
//!
//! # Limits
//!
//! [`Limits`]: frame size (header + 1472), a token bucket per sender on frames and bytes (over it, frames are
//! dropped, as a congested LAN drops datagrams), and a bounded queue per receiver (full: dropped).

use std::collections::HashMap;
use std::collections::hash_map::RandomState;
use std::hash::BuildHasher;
use std::net::IpAddr;
use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

use bytes::{Bytes, BytesMut};
use serde_json::{Value, json};
use sha1::{Digest, Sha1};
use tokio::sync::mpsc;

pub const NET: u32 = 0x0A4D_0000; // 10.77.0.0
pub const MASK: u32 = 0xFFFF_C000; // /18
pub const BROADCAST: u32 = 0xFFFF_FFFF;
pub const SERVER: u32 = 0x0A4D_0001; // 10.77.0.1, reserved for the server's own hosts
pub const HUB: u32 = 0x0A4D_00FD; // 10.77.0.253, reserved for a free-roam hub (nfsu2-web's NET_ROAM_HUB)
pub const NETS: u32 = 64;
pub const HOSTS: u32 = 250;
pub const ADDRS: u32 = NETS * HOSTS;
pub const HDR: usize = 16;
pub const MAX_DATA: usize = 1472;
pub const MAX_FRAME: usize = HDR + MAX_DATA;
pub const NAME_MAX: usize = 32;
/// free roam's UDP port (crates/roam): its frames route whatever the rooms
pub const ROAM_PORT: u16 = 47210;
/// a room's city / mode text, and its password, in characters
pub const TAG_MAX: usize = 24;
pub const PASS_MAX: usize = 64;
/// a signal message's JSON, in bytes
pub const SIGNAL_MAX: usize = 16 << 10;
const ID_CHARS: &[u8] = b"23456789abcdefghjkmnpqrstuvwxyz";

/// The i-th address handed out (i < ADDRS): across the 64 /24s first, host bytes 2-251.
pub fn vip_at(i: u32) -> u32 {
    NET | (i % NETS) << 8 | (2 + i / NETS)
}

/// One of the addresses [`vip_at`] hands out.
pub fn is_vip(v: u32) -> bool {
    v & MASK == NET && (2..2 + HOSTS).contains(&(v & 0xFF))
}

pub fn vip_str(v: u32) -> String {
    let b = v.to_be_bytes();
    format!("{}.{}.{}.{}", b[0], b[1], b[2], b[3])
}

/// The player's MAC: Microsoft's OUI (an Xbox NIC) and the address's low three bytes, as xnet_hle.c derives it.
pub fn mac_str(v: u32) -> String {
    let b = v.to_be_bytes();
    format!("00:50:F2:{:02X}:{:02X}:{:02X}", b[1], b[2], b[3])
}

/// Text as other players see it: control and format characters out, at most `max` characters, trimmed.
pub fn clean_text(s: &str, max: usize) -> String {
    let s: String = s.chars().filter(|c| !c.is_control() && !matches!(*c, '\u{200B}'..='\u{200F}' | '\u{202A}'..='\u{202E}' | '\u{2060}'..='\u{206F}' | '\u{FEFF}')).take(max).collect();
    s.trim().to_string()
}

/// A display name as other players see it: [`clean_text`], at most NAME_MAX characters, empty -> "Player".
pub fn clean_name(s: &str) -> String {
    let s = clean_text(s, NAME_MAX);
    if s.is_empty() { "Player".to_string() } else { s }
}

/// 64 bits no client can predict: std's per-process random SipHash keys (not a CSPRNG; room ids are not secrets,
/// salts only need to differ).
fn rand64() -> u64 {
    static N: AtomicU64 = AtomicU64::new(0);
    RandomState::new().hash_one(N.fetch_add(1, Ordering::Relaxed))
}

fn room_id() -> String {
    let mut r = rand64();
    let n = ID_CHARS.len() as u64;
    (0..6)
        .map(|_| {
            let c = ID_CHARS[(r % n) as usize] as char;
            r /= n;
            c
        })
        .collect()
}

fn verifier(salt: &[u8; 16], pass: &str) -> [u8; 20] {
    let mut h = Sha1::new();
    h.update(salt);
    h.update(pass.as_bytes());
    h.finalize().into()
}

/// What goes to a client: the server's writer turns each into a WebSocket frame.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Out {
    Bin(Bytes),
    Text(String),
    Pong(Bytes),
    /// close with this code and reason, then stop writing
    Close(u16, &'static str),
}

#[derive(Clone, Debug)]
pub struct Limits {
    /// frames a second a sender may send, and its burst (a 3-player System Link race is ~100 a second)
    pub frames_per_s: f64,
    pub burst_frames: f64,
    /// bytes a second a sender may send, and its burst
    pub bytes_per_s: f64,
    pub burst_bytes: f64,
    /// messages queued for one receiver; past it, frames to it are dropped
    pub queue: usize,
}

impl Default for Limits {
    fn default() -> Self {
        Limits { frames_per_s: 2000.0, burst_frames: 4000.0, bytes_per_s: (1 << 20) as f64, burst_bytes: (2 << 20) as f64, queue: 1024 }
    }
}

#[derive(Clone, Debug)]
pub struct RoomLimits {
    /// members of a room (System Link races hold 8)
    pub members: usize,
    /// rooms on the switch
    pub rooms: usize,
    /// rooms hosted from one IP address at a time
    pub per_ip: usize,
}

impl Default for RoomLimits {
    fn default() -> Self {
        RoomLimits { members: 8, rooms: 256, per_ip: 2 }
    }
}

/// What became of a frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verdict {
    /// routed to this many peers (0: nobody holds the address, or nobody has System Link open)
    Sent(usize),
    TooShort,
    TooLong,
    /// over the sender's budget
    Limited,
    /// the sender is not on the switch
    Unknown,
}

struct Peer {
    tx: mpsc::Sender<Out>,
    name: Option<String>,
    lan: bool,
    frames: f64,
    bytes: f64,
    at: Instant,
    /// its room's key in Inner::rooms, 0: none (the open LAN)
    room: u64,
    /// wants the room list ({"t":"rooms","sub":1})
    sub: bool,
    ip: Option<IpAddr>,
}

struct Room {
    id: String,
    name: String,
    host: u32,
    city: String,
    mode: String,
    max: usize,
    salt: [u8; 16],
    /// SHA-1(salt || password); None: no password
    verifier: Option<[u8; 20]>,
    members: Vec<u32>,
}

struct Inner {
    peers: HashMap<u32, Peer>,
    next: u32,
    /// keyed by creation order (the list's order); key 0 is never used
    rooms: HashMap<u64, Room>,
    seq: u64,
}

impl Inner {
    fn send(&self, vip: u32, v: Value) {
        if let Some(p) = self.peers.get(&vip) {
            let _ = p.tx.try_send(Out::Text(v.to_string()));
        }
    }

    fn error(&self, vip: u32, op: &str, code: &str) {
        self.send(vip, json!({"t": "error", "op": op, "code": code}));
    }

    fn nick(&self, vip: u32) -> &str {
        self.peers.get(&vip).and_then(|p| p.name.as_deref()).unwrap_or("Player")
    }

    fn room_by_id(&self, id: &str) -> Option<u64> {
        self.rooms.iter().find(|(_, r)| r.id == id).map(|(k, _)| *k)
    }

    fn room_json(&self, r: &Room) -> Value {
        json!({"id": r.id, "name": r.name, "host": self.nick(r.host), "hostVip": r.host, "city": r.city, "mode": r.mode,
               "n": r.members.len(), "max": r.max, "locked": r.verifier.is_some()})
    }

    /// The room and its members to each of them.
    fn send_room(&self, k: u64) {
        let Some(r) = self.rooms.get(&k) else { return };
        let members: Vec<Value> =
            r.members.iter().map(|&v| json!({"vip": v, "vipStr": vip_str(v), "name": self.nick(v), "host": v == r.host})).collect();
        let msg = json!({"t": "room", "room": self.room_json(r), "members": members}).to_string();
        for v in &r.members {
            if let Some(p) = self.peers.get(v) {
                let _ = p.tx.try_send(Out::Text(msg.clone()));
            }
        }
    }

    fn rooms_msg(&self) -> String {
        let mut keys: Vec<&u64> = self.rooms.keys().collect();
        keys.sort();
        let rooms: Vec<Value> = keys.into_iter().map(|k| self.room_json(&self.rooms[k])).collect();
        json!({"t": "rooms", "rooms": rooms}).to_string()
    }

    // shortcut: the whole list to every subscriber on each change, as send_list; deltas if lists grow long.
    fn send_rooms(&self) {
        let msg = self.rooms_msg();
        for p in self.peers.values().filter(|p| p.sub) {
            let _ = p.tx.try_send(Out::Text(msg.clone()));
        }
    }

    /// Takes `vip` out of its room (closing it if `vip` hosts it); tells `vip` {"t":"left","why":why} if given.
    /// Whether a room changed (the list is then due).
    fn leave_room(&mut self, vip: u32, why: Option<&str>) -> bool {
        let Some(p) = self.peers.get_mut(&vip) else { return false };
        let k = std::mem::take(&mut p.room);
        let Some(r) = self.rooms.get_mut(&k) else { return false };
        let id = r.id.clone();
        if r.host == vip {
            let r = self.rooms.remove(&k).expect("the room");
            for v in r.members.into_iter().filter(|&v| v != vip) {
                if let Some(q) = self.peers.get_mut(&v) {
                    q.room = 0;
                }
                self.send(v, json!({"t": "left", "room": id, "why": "closed"}));
            }
        } else {
            r.members.retain(|&v| v != vip);
            self.send_room(k);
        }
        if let Some(w) = why {
            self.send(vip, json!({"t": "left", "room": id, "why": w}));
        }
        true
    }
}

/// What the switch hands the free-roam hub (web/server/crates/roam), which stands at [`HUB`]: the datagrams sent to
/// it, and who left.
#[derive(Clone, Debug)]
pub enum ToHub {
    Frame { src: u32, sport: u16, dport: u16, data: Bytes },
    Leave(u32),
}

pub struct Switch {
    pub limits: Limits,
    pub room_limits: RoomLimits,
    inner: Mutex<Inner>,
    hub: Mutex<Option<mpsc::Sender<ToHub>>>,
    /// the hub's players as JSON (`set_roam_list`), the answer to {"t":"roam"}
    roam: Mutex<String>,
    /// free-roam players per room id (set_roam_rooms), for stats_json
    roam_rooms: Mutex<std::collections::BTreeMap<u32, usize>>,
}

impl Switch {
    pub fn new(limits: Limits) -> Self {
        Switch {
            limits,
            room_limits: RoomLimits::default(),
            inner: Mutex::new(Inner { peers: HashMap::new(), next: 0, rooms: HashMap::new(), seq: 0 }),
            hub: Mutex::new(None),
            roam: Mutex::new(r#"{"t":"roam","players":[]}"#.to_string()),
            roam_rooms: Mutex::new(Default::default()),
        }
    }

    pub fn count(&self) -> usize {
        self.inner.lock().unwrap().peers.len()
    }

    pub fn with_rooms(mut self, r: RoomLimits) -> Self {
        self.room_limits = r;
        self
    }

    /// A new client: its address (the welcome is queued on `tx`), or None when every address is held.
    pub fn join(&self, tx: mpsc::Sender<Out>) -> Option<u32> {
        self.join_from(tx, None, None)
    }

    /// [`Switch::join`] from this IP address (counted for [`RoomLimits::per_ip`]); `want`: the address it had before
    /// a reconnect (web/lan.js, /lan?vip=), given back when free, so a System Link session survives the drop.
    pub fn join_from(&self, tx: mpsc::Sender<Out>, ip: Option<IpAddr>, want: Option<u32>) -> Option<u32> {
        let mut g = self.inner.lock().unwrap();
        let vip = match want.filter(|v| is_vip(*v) && !g.peers.contains_key(v)) {
            Some(v) => v,
            None => {
                let start = g.next;
                let v = (0..ADDRS).map(|k| vip_at((start + k) % ADDRS)).find(|v| !g.peers.contains_key(v))?;
                g.next = (start + 1) % ADDRS;
                v
            }
        };
        let welcome = serde_json::json!({"t": "welcome", "vip": vip, "vipStr": vip_str(vip), "mask": MASK, "mac": mac_str(vip)});
        let _ = tx.try_send(Out::Text(welcome.to_string()));
        let (frames, bytes) = (self.limits.burst_frames, self.limits.burst_bytes);
        g.peers.insert(vip, Peer { tx, name: None, lan: false, frames, bytes, at: Instant::now(), room: 0, sub: false, ip });
        Some(vip)
    }

    pub fn leave(&self, vip: u32) {
        let mut g = self.inner.lock().unwrap();
        let rooms = g.leave_room(vip, None);
        if g.peers.remove(&vip).is_some_and(|p| p.name.is_some()) {
            send_list(&g);
        }
        if rooms {
            g.send_rooms();
        }
        drop(g);
        if let Some(h) = self.hub.lock().unwrap().as_ref() {
            let _ = h.try_send(ToHub::Leave(vip));
        }
    }

    /// The free-roam hub at [`HUB`]: the frames sent to it go to `tx` (full: dropped, as a LAN drops).
    pub fn attach_hub(&self, tx: mpsc::Sender<ToHub>) {
        *self.hub.lock().unwrap() = Some(tx);
    }

    /// The hub's players, the answer to a client's {"t":"roam"}: a JSON message {"t":"roam","players":[...]}.
    pub fn set_roam_list(&self, json: String) {
        *self.roam.lock().unwrap() = json;
    }

    pub fn set_roam_rooms(&self, rooms: std::collections::BTreeMap<u32, usize>) {
        *self.roam_rooms.lock().unwrap() = rooms;
    }

    /// GET /lan/stats: counts only (no names, no addresses): sockets, free roam by room id, race rooms.
    pub fn stats_json(&self) -> String {
        let (online, races) = {
            let g = self.inner.lock().unwrap();
            (g.peers.len(), g.rooms.len())
        };
        let rooms = self.roam_rooms.lock().unwrap();
        let total: usize = rooms.values().sum();
        let by: serde_json::Map<String, Value> = rooms.iter().map(|(k, n)| (k.to_string(), json!(n))).collect();
        json!({"online": online, "freeRoam": {"total": total, "rooms": by}, "races": {"rooms": races}}).to_string()
    }

    /// A datagram from one of the server's own hosts (`src`, the hub) to the client at `dst`, as a kind 3 frame;
    /// whether it was queued.
    pub fn inject(&self, src: u32, sport: u16, dst: u32, dport: u16, data: &[u8]) -> bool {
        if data.len() > MAX_DATA {
            return false;
        }
        let mut f = BytesMut::with_capacity(HDR + data.len());
        f.extend_from_slice(&[3, 0, 0, 0]);
        f.extend_from_slice(&src.to_be_bytes());
        f.extend_from_slice(&dst.to_be_bytes());
        f.extend_from_slice(&sport.to_be_bytes());
        f.extend_from_slice(&dport.to_be_bytes());
        f.extend_from_slice(data);
        let g = self.inner.lock().unwrap();
        g.peers.get(&dst).is_some_and(|q| q.tx.try_send(Out::Bin(f.freeze())).is_ok())
    }

    /// A text message from a client (JSON); anything unknown or malformed is ignored.
    pub fn control(&self, vip: u32, text: &str) {
        let Ok(m) = serde_json::from_str::<Value>(text) else { return };
        let on = |k: &str| m.get(k).is_some_and(|v| v.as_u64() == Some(1) || v.as_bool() == Some(true));
        let str_of = |k: &str| m.get(k).and_then(|v| v.as_str()).unwrap_or("");
        let rl = &self.room_limits;
        let mut guard = self.inner.lock().unwrap();
        let g = &mut *guard;
        let Some(p) = g.peers.get_mut(&vip) else { return };
        match m.get("t").and_then(|t| t.as_str()) {
            Some("hello") => {
                p.name = Some(clean_name(str_of("name")));
                p.lan = on("lan");
                let k = p.room;
                send_list(g);
                if k != 0 {
                    g.send_room(k);
                    g.send_rooms();
                }
            }
            Some("lan") => p.lan = on("on"),
            Some("ping") => {
                let pong = serde_json::json!({"t": "pong", "id": m.get("id").cloned().unwrap_or_default()});
                let _ = p.tx.try_send(Out::Text(pong.to_string()));
            }
            Some("roam") => {
                let _ = p.tx.try_send(Out::Text(self.roam.lock().unwrap().clone()));
            }
            Some("rooms") => {
                p.sub = on("sub");
                if p.sub {
                    let msg = g.rooms_msg();
                    let _ = g.peers[&vip].tx.try_send(Out::Text(msg));
                }
            }
            Some("create") => {
                let ip = p.ip;
                let changed = g.leave_room(vip, Some("leave"));
                let hosted = |ip: IpAddr| g.rooms.values().filter(|r| g.peers.get(&r.host).is_some_and(|h| h.ip == Some(ip))).count();
                if g.rooms.len() >= rl.rooms || ip.is_some_and(|ip| hosted(ip) >= rl.per_ip) {
                    g.error(vip, "create", "limit");
                    if changed {
                        g.send_rooms();
                    }
                    return;
                }
                let mut name = clean_text(str_of("name"), NAME_MAX);
                if name.is_empty() {
                    name = clean_text(&format!("{}'s race", g.nick(vip)), NAME_MAX);
                }
                let max = m.get("max").and_then(|v| v.as_u64()).map_or(rl.members, |n| (n as usize).clamp(2, rl.members.max(2)));
                let mut salt = [0u8; 16];
                salt[..8].copy_from_slice(&rand64().to_le_bytes());
                salt[8..].copy_from_slice(&rand64().to_le_bytes());
                let pass: String = str_of("pass").chars().take(PASS_MAX).collect();
                let verifier = (!pass.is_empty()).then(|| verifier(&salt, &pass));
                let id = std::iter::repeat_with(room_id).find(|id| g.room_by_id(id).is_none()).expect("an id");
                let (city, mode) = (clean_text(str_of("city"), TAG_MAX), clean_text(str_of("mode"), TAG_MAX));
                g.seq += 1;
                let k = g.seq;
                g.rooms.insert(k, Room { id, name, host: vip, city, mode, max, salt, verifier, members: vec![vip] });
                g.peers.get_mut(&vip).expect("the peer").room = k;
                g.send_room(k);
                g.send_rooms();
            }
            Some("join") => {
                let mine = p.room;
                let Some(k) = g.room_by_id(str_of("room")) else { return g.error(vip, "join", "gone") };
                if mine == k {
                    return g.send_room(k);
                }
                let r = &g.rooms[&k];
                if r.members.len() >= r.max {
                    return g.error(vip, "join", "full");
                }
                let pass: String = str_of("pass").chars().take(PASS_MAX).collect();
                // constant time: no early exit on the first differing byte
                if r.verifier.is_some_and(|v| verifier(&r.salt, &pass).iter().zip(v).fold(0, |a, (x, y)| a | (x ^ y)) != 0) {
                    return g.error(vip, "join", "password");
                }
                g.leave_room(vip, Some("leave"));
                g.rooms.get_mut(&k).expect("the room").members.push(vip);
                g.peers.get_mut(&vip).expect("the peer").room = k;
                g.send_room(k);
                g.send_rooms();
            }
            Some("leave") => {
                if g.leave_room(vip, Some("leave")) {
                    g.send_rooms();
                }
            }
            Some("signal") => {
                let k = p.room;
                let kind = str_of("kind");
                if !matches!(kind, "offer" | "answer" | "ice") {
                    return;
                }
                if text.len() > SIGNAL_MAX {
                    return g.error(vip, "signal", "big");
                }
                let to = m.get("to").and_then(|v| v.as_u64()).and_then(|v| u32::try_from(v).ok());
                match to.filter(|t| k != 0 && *t != vip && g.peers.get(t).is_some_and(|q| q.room == k)) {
                    Some(t) => g.send(t, json!({"t": "signal", "from": vip, "kind": kind, "data": m.get("data").cloned().unwrap_or_default()})),
                    None => g.error(vip, "signal", "room"),
                }
            }
            _ => {} // "beat" too: any message keeps the connection from idling out (http.rs IDLE)
        }
    }

    /// A binary frame from `vip`: checked, its source rewritten, routed.
    pub fn frame(&self, vip: u32, frame: &[u8], now: Instant) -> Verdict {
        if frame.len() < HDR {
            return Verdict::TooShort;
        }
        if frame.len() > MAX_FRAME {
            return Verdict::TooLong;
        }
        let mut g = self.inner.lock().unwrap();
        let l = &self.limits;
        let Some(p) = g.peers.get_mut(&vip) else { return Verdict::Unknown };
        let dt = now.saturating_duration_since(p.at).as_secs_f64();
        p.at = now;
        p.frames = (p.frames + dt * l.frames_per_s).min(l.burst_frames);
        p.bytes = (p.bytes + dt * l.bytes_per_s).min(l.burst_bytes);
        if p.frames < 1.0 || p.bytes < frame.len() as f64 {
            return Verdict::Limited;
        }
        p.frames -= 1.0;
        p.bytes -= frame.len() as f64;
        let room = p.room;

        let mut b = BytesMut::from(frame);
        b[1..4].fill(0);
        b[4..8].copy_from_slice(&vip.to_be_bytes());
        let b = b.freeze();
        let dst = u32::from_be_bytes([frame[8], frame[9], frame[10], frame[11]]);
        let (sport, dport) = (u16::from_be_bytes([frame[12], frame[13]]), u16::from_be_bytes([frame[14], frame[15]]));
        // a room is its own LAN; free roam's traffic is not a room's business
        let reach = |q: &Peer| q.room == room || sport == ROAM_PORT || dport == ROAM_PORT;
        let mut n = 0;
        if dst == HUB {
            drop(g);
            let data = b.slice(HDR..);
            let sent = self.hub.lock().unwrap().as_ref().is_some_and(|h| h.try_send(ToHub::Frame { src: vip, sport, dport, data }).is_ok());
            return Verdict::Sent(sent as usize);
        }
        if dst == BROADCAST {
            for (&v, q) in g.peers.iter() {
                if v != vip && q.lan && reach(q) && q.tx.try_send(Out::Bin(b.clone())).is_ok() {
                    n += 1;
                }
            }
        } else if dst != vip
            && let Some(q) = g.peers.get(&dst)
            && reach(q)
            && q.tx.try_send(Out::Bin(b)).is_ok()
        {
            n = 1;
        }
        Verdict::Sent(n)
    }
}

/// The whole peer list to everyone who said hello.
// shortcut: the whole list on every change is O(players^2) messages; fine for a System Link lobby, switch to
// nfsu2-web's deltas (members.rs) before many players share one switch (free roam).
fn send_list(g: &Inner) {
    let mut list: Vec<(&u32, &Peer)> = g.peers.iter().filter(|(_, p)| p.name.is_some()).collect();
    list.sort_by_key(|(v, _)| **v);
    let peers: Vec<_> = list
        .iter()
        .map(|(v, p)| serde_json::json!({"vip": **v, "vipStr": vip_str(**v), "name": p.name.as_deref().unwrap_or("")}))
        .collect();
    let msg = serde_json::json!({"t": "peers", "peers": peers}).to_string();
    for (_, p) in list {
        let _ = p.tx.try_send(Out::Text(msg.clone()));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    fn peer(sw: &Switch) -> (u32, mpsc::Receiver<Out>) {
        let (tx, mut rx) = mpsc::channel(64);
        let vip = sw.join(tx).expect("an address");
        match rx.try_recv() {
            Ok(Out::Text(t)) => assert!(t.contains("\"welcome\"") && t.contains(&vip_str(vip)), "{t}"),
            o => panic!("no welcome: {o:?}"),
        }
        (vip, rx)
    }

    fn drain(rx: &mut mpsc::Receiver<Out>) -> Vec<Out> {
        std::iter::from_fn(|| rx.try_recv().ok()).collect()
    }

    fn bins(rx: &mut mpsc::Receiver<Out>) -> Vec<Bytes> {
        drain(rx).into_iter().filter_map(|o| if let Out::Bin(b) = o { Some(b) } else { None }).collect()
    }

    fn udp(src: u32, dst: u32, data: &[u8]) -> Vec<u8> {
        let mut f = vec![3, 9, 9, 9];
        f.extend_from_slice(&src.to_be_bytes());
        f.extend_from_slice(&dst.to_be_bytes());
        f.extend_from_slice(&1000u16.to_be_bytes());
        f.extend_from_slice(&1000u16.to_be_bytes());
        f.extend_from_slice(data);
        f
    }

    /// A reconnecting page asks for its old address: given back when free, else a new one; never a reserved one.
    #[test]
    fn resume() {
        let sw = Switch::new(Limits::default());
        let (a, _ra) = peer(&sw);
        let (tx, _rb) = mpsc::channel(64);
        assert_ne!(sw.join_from(tx, None, Some(a)), Some(a), "held: another address");
        sw.leave(a);
        let (tx, _rc) = mpsc::channel(64);
        assert_eq!(sw.join_from(tx, None, Some(a)), Some(a), "free again: the same one");
        for bad in [HUB, SERVER, BROADCAST, 0x0A4D_0001, 0x0A4E_0002] {
            let (tx, _r) = mpsc::channel(64);
            assert_ne!(sw.join_from(tx, None, Some(bad)), Some(bad), "{bad:08x}");
        }
    }

    #[test]
    fn addresses() {
        assert_eq!(vip_str(vip_at(0)), "10.77.0.2");
        assert_eq!(vip_str(vip_at(1)), "10.77.1.2");
        assert_eq!(vip_str(vip_at(64)), "10.77.0.3");
        assert_eq!(vip_str(vip_at(ADDRS - 1)), "10.77.63.251");
        let all: std::collections::HashSet<u32> = (0..ADDRS).map(vip_at).collect();
        assert_eq!(all.len(), ADDRS as usize);
        for v in &all {
            assert_eq!(v & MASK, NET);
            assert!(*v != SERVER && *v != HUB && v & 0xFF != 0 && v & 0xFF != 255);
        }
        assert_eq!(mac_str(0x0A4D_0102), "00:50:F2:4D:01:02");
    }

    #[test]
    fn routing() {
        let sw = Switch::new(Limits::default());
        let (a, mut ra) = peer(&sw);
        let (b, mut rb) = peer(&sw);
        let (c, mut rc) = peer(&sw);
        assert!(a != b && b != c && a != c);
        sw.control(a, r#"{"t":"hello","name":"Ann","lan":1}"#);
        sw.control(b, r#"{"t":"hello","name":"Bob\u0007","lan":1}"#);
        sw.control(c, r#"{"t":"hello","name":"Cy"}"#); // System Link shut
        let last = drain(&mut rc).into_iter().last().unwrap();
        match last {
            Out::Text(t) => assert!(t.contains("\"Ann\"") && t.contains("\"Bob\"") && t.contains("\"Cy\""), "{t}"),
            o => panic!("{o:?}"),
        }
        drain(&mut ra);
        drain(&mut rb);

        // broadcast: to b (System Link open), not to c, not back to a; source rewritten, reserved bytes cleared
        assert_eq!(sw.frame(a, &udp(0x01020304, BROADCAST, b"hi"), Instant::now()), Verdict::Sent(1));
        let got = bins(&mut rb);
        assert_eq!(got.len(), 1);
        let mut want = udp(a, BROADCAST, b"hi");
        want[1..4].fill(0);
        assert_eq!(&got[0][..], &want[..]);
        assert!(bins(&mut rc).is_empty() && bins(&mut ra).is_empty());

        // c opens System Link: broadcasts reach it too
        sw.control(c, r#"{"t":"lan","on":1}"#);
        assert_eq!(sw.frame(a, &udp(a, BROADCAST, b"x"), Instant::now()), Verdict::Sent(2));
        assert_eq!(bins(&mut rc).len(), 1);
        drain(&mut rb);

        // unicast: only the holder; to nobody or to oneself: nowhere
        assert_eq!(sw.frame(b, &udp(b, c, b"u"), Instant::now()), Verdict::Sent(1));
        let got = bins(&mut rc);
        assert_eq!(&got[0][4..8], &b.to_be_bytes());
        assert_eq!(&got[0][16..], b"u");
        assert!(bins(&mut ra).is_empty());
        assert_eq!(sw.frame(b, &udp(b, 0x0A4D_3F3F, b"u"), Instant::now()), Verdict::Sent(0));
        assert_eq!(sw.frame(b, &udp(b, b, b"u"), Instant::now()), Verdict::Sent(0));

        // sizes
        assert_eq!(sw.frame(a, &[3; HDR - 1], Instant::now()), Verdict::TooShort);
        assert_eq!(sw.frame(a, &udp(a, b, &[0; MAX_DATA]), Instant::now()), Verdict::Sent(1));
        assert_eq!(sw.frame(a, &udp(a, b, &[0; MAX_DATA + 1]), Instant::now()), Verdict::TooLong);

        // c leaves: the others hear the list without it, frames to it go nowhere
        sw.leave(c);
        assert_eq!(sw.count(), 2);
        assert!(drain(&mut ra).iter().any(|o| matches!(o, Out::Text(t) if t.contains("peers") && !t.contains("Cy"))));
        assert_eq!(sw.frame(a, &udp(a, c, b"u"), Instant::now()), Verdict::Sent(0));
        assert_eq!(sw.frame(c, &udp(c, a, b"u"), Instant::now()), Verdict::Unknown);
    }

    #[test]
    fn ping() {
        let sw = Switch::new(Limits::default());
        let (a, mut ra) = peer(&sw);
        sw.control(a, r#"{"t":"ping","id":7}"#);
        sw.control(a, "not json");
        assert_eq!(drain(&mut ra), vec![Out::Text(r#"{"id":7,"t":"pong"}"#.to_string())]);
    }

    #[test]
    fn rate_limit() {
        let sw = Switch::new(Limits { frames_per_s: 10.0, burst_frames: 5.0, ..Limits::default() });
        let (a, _ra) = peer(&sw);
        let (b, mut rb) = peer(&sw);
        let t0 = Instant::now();
        let v: Vec<Verdict> = (0..7).map(|_| sw.frame(a, &udp(a, b, b"z"), t0)).collect();
        assert_eq!(v.iter().filter(|v| **v == Verdict::Sent(1)).count(), 5);
        assert_eq!(v[5], Verdict::Limited);
        assert_eq!(bins(&mut rb).len(), 5);
        // 0.2 s later: two more frames' worth
        let t1 = t0 + Duration::from_millis(200);
        assert_eq!(sw.frame(a, &udp(a, b, b"z"), t1), Verdict::Sent(1));
        assert_eq!(sw.frame(a, &udp(a, b, b"z"), t1), Verdict::Sent(1));
        assert_eq!(sw.frame(a, &udp(a, b, b"z"), t1), Verdict::Limited);
    }

    #[test]
    fn full_queue_drops() {
        let sw = Switch::new(Limits::default());
        let (a, _ra) = peer(&sw);
        let (tx, _rb) = mpsc::channel(2);
        let b = sw.join(tx).unwrap(); // the welcome takes one place
        assert_eq!(sw.frame(a, &udp(a, b, b"1"), Instant::now()), Verdict::Sent(1));
        assert_eq!(sw.frame(a, &udp(a, b, b"2"), Instant::now()), Verdict::Sent(0));
    }

    #[test]
    fn hub() {
        let sw = Switch::new(Limits::default());
        let (a, mut ra) = peer(&sw);
        let (htx, mut hrx) = mpsc::channel(8);
        sw.attach_hub(htx);
        let mut f = udp(a, HUB, b"MC3R");
        f[12..14].copy_from_slice(&47210u16.to_be_bytes());
        assert_eq!(sw.frame(a, &f, Instant::now()), Verdict::Sent(1));
        match hrx.try_recv() {
            Ok(ToHub::Frame { src, sport, data, .. }) => assert!(src == a && sport == 47210 && &data[..] == b"MC3R"),
            o => panic!("{o:?}"),
        }
        assert!(sw.inject(HUB, 47210, a, 47210, b"list"));
        let got = bins(&mut ra);
        assert_eq!(&got[0][4..8], &HUB.to_be_bytes());
        assert_eq!(&got[0][16..], b"list");
        sw.set_roam_list(r#"{"t":"roam","players":[{"name":"Ann"}]}"#.into());
        sw.control(a, r#"{"t":"roam"}"#);
        assert!(drain(&mut ra).iter().any(|o| matches!(o, Out::Text(t) if t.contains("Ann"))));
        sw.leave(a);
        assert!(matches!(hrx.try_recv(), Ok(ToHub::Leave(v)) if v == a));
    }

    fn texts(rx: &mut mpsc::Receiver<Out>) -> Vec<serde_json::Value> {
        drain(rx).into_iter().filter_map(|o| if let Out::Text(t) = o { serde_json::from_str(&t).ok() } else { None }).collect()
    }

    fn named(sw: &Switch, name: &str, ip: Option<IpAddr>) -> (u32, mpsc::Receiver<Out>) {
        let (tx, mut rx) = mpsc::channel(256);
        let vip = sw.join_from(tx, ip, None).unwrap();
        sw.control(vip, &format!(r#"{{"t":"hello","name":"{name}","lan":1}}"#));
        drain(&mut rx);
        (vip, rx)
    }

    fn last<'a>(v: &'a [serde_json::Value], t: &str) -> &'a serde_json::Value {
        v.iter().rev().find(|m| m["t"] == t).unwrap_or_else(|| panic!("no {t} in {v:?}"))
    }

    #[test]
    fn rooms() {
        let sw = Switch::new(Limits::default());
        let (a, mut ra) = named(&sw, "Ann", None);
        let (b, mut rb) = named(&sw, "Bob", None);
        let (c, mut rc) = named(&sw, "Cy", None);
        let (d, mut rd) = named(&sw, "Di", None);
        let (_e, mut re) = named(&sw, "Ed", None);
        sw.control(b, r#"{"t":"rooms","sub":1}"#);
        assert_eq!(last(&texts(&mut rb), "rooms")["rooms"], serde_json::json!([]));

        // a hosts a locked race; the list shows it locked, never the password
        sw.control(a, r#"{"t":"create","name":"Night\u0007 run","pass":"s3cret","city":"Atlanta","max":4}"#);
        let m = texts(&mut ra);
        let room = &last(&m, "room")["room"];
        assert!(room["name"] == "Night run" && room["host"] == "Ann" && room["locked"] == true && room["n"] == 1 && room["max"] == 4);
        let id = room["id"].as_str().unwrap().to_string();
        assert!(id.len() == 6 && id.bytes().all(|c| ID_CHARS.contains(&c)), "{id}");
        let list = texts(&mut rb);
        assert_eq!(last(&list, "rooms")["rooms"][0]["id"], id.as_str());
        assert!(!format!("{list:?}{m:?}").contains("s3cret"));

        // the password guards the door
        sw.control(b, &format!(r#"{{"t":"join","room":"{id}"}}"#));
        assert_eq!(last(&texts(&mut rb), "error")["code"], "password");
        sw.control(b, &format!(r#"{{"t":"join","room":"{id}","pass":"S3cret"}}"#));
        assert_eq!(last(&texts(&mut rb), "error")["code"], "password");
        sw.control(b, r#"{"t":"join","room":"zzzzzz"}"#);
        assert_eq!(last(&texts(&mut rb), "error")["code"], "gone");
        sw.control(b, &format!(r#"{{"t":"join","room":"{id}","pass":"s3cret"}}"#));
        let m = texts(&mut rb);
        assert_eq!(last(&m, "room")["members"].as_array().unwrap().len(), 2);
        assert_eq!(last(&m, "rooms")["rooms"][0]["n"], 2);
        assert_eq!(last(&texts(&mut ra), "room")["members"][1]["name"], "Bob");

        // c hosts an open race of its own; d and e stay on the open LAN
        sw.control(c, r#"{"t":"create","name":""}"#);
        assert_eq!(last(&texts(&mut rc), "room")["room"]["name"], "Cy's race");

        // broadcasts stay in their room, or on the open LAN
        assert_eq!(sw.frame(a, &udp(a, BROADCAST, b"a"), Instant::now()), Verdict::Sent(1));
        assert_eq!(bins(&mut rb).len(), 1);
        assert_eq!(sw.frame(c, &udp(c, BROADCAST, b"c"), Instant::now()), Verdict::Sent(0));
        assert_eq!(sw.frame(d, &udp(d, BROADCAST, b"d"), Instant::now()), Verdict::Sent(1));
        assert_eq!(bins(&mut re).len(), 1);
        assert!(bins(&mut ra).is_empty() && bins(&mut rc).is_empty());
        // unicasts too; free roam's port goes through
        assert_eq!(sw.frame(b, &udp(b, a, b"u"), Instant::now()), Verdict::Sent(1));
        assert_eq!(sw.frame(a, &udp(a, c, b"u"), Instant::now()), Verdict::Sent(0));
        assert_eq!(sw.frame(d, &udp(d, a, b"u"), Instant::now()), Verdict::Sent(0));
        let mut roam = udp(a, c, b"MC3R");
        roam[14..16].copy_from_slice(&ROAM_PORT.to_be_bytes());
        assert_eq!(sw.frame(a, &roam, Instant::now()), Verdict::Sent(1));
        drain(&mut ra);
        drain(&mut rc);

        // signalling only between members of one room
        sw.control(a, &format!(r#"{{"t":"signal","to":{b},"kind":"offer","data":{{"sdp":"x"}}}}"#));
        let s = last(&texts(&mut rb), "signal").clone();
        assert!(s["from"] == a && s["kind"] == "offer" && s["data"]["sdp"] == "x");
        sw.control(a, &format!(r#"{{"t":"signal","to":{c},"kind":"ice","data":null}}"#));
        assert_eq!(last(&texts(&mut ra), "error")["code"], "room");
        assert!(texts(&mut rc).is_empty());

        // a member leaves: the room goes on; it is back on the open LAN
        sw.control(b, r#"{"t":"leave"}"#);
        assert_eq!(last(&texts(&mut rb), "left")["why"], "leave");
        assert_eq!(last(&texts(&mut ra), "room")["members"].as_array().unwrap().len(), 1);
        assert_eq!(sw.frame(d, &udp(d, BROADCAST, b"d"), Instant::now()), Verdict::Sent(2));
        drain(&mut rb);
        drain(&mut re);

        // the host goes: the room closes, its members are told and land on the open LAN
        sw.control(b, &format!(r#"{{"t":"join","room":"{id}","pass":"s3cret"}}"#));
        drain(&mut rb);
        sw.leave(a);
        let m = texts(&mut rb);
        assert!(last(&m, "left")["why"] == "closed" && last(&m, "left")["room"] == id.as_str());
        assert_eq!(last(&m, "rooms")["rooms"].as_array().unwrap().len(), 1);
        assert_eq!(sw.frame(b, &udp(b, BROADCAST, b"b"), Instant::now()), Verdict::Sent(2));
        sw.control(d, &format!(r#"{{"t":"join","room":"{id}"}}"#));
        assert_eq!(last(&texts(&mut rd), "error")["code"], "gone");
    }

    #[test]
    fn room_limits() {
        let ip: IpAddr = "192.0.2.7".parse().unwrap();
        let sw = Switch::new(Limits::default()).with_rooms(RoomLimits { members: 2, rooms: 3, per_ip: 2 });
        let (a, mut ra) = named(&sw, "A", Some(ip));
        let (b, mut rb) = named(&sw, "B", Some(ip));
        let (c, mut rc) = named(&sw, "C", Some(ip));
        let (d, mut rd) = named(&sw, "D", None);
        let (e, mut re) = named(&sw, "E", None);
        sw.control(a, r#"{"t":"create","name":"x","max":8}"#);
        let room = last(&texts(&mut ra), "room")["room"].clone();
        assert_eq!(room["max"], 2);
        sw.control(b, r#"{"t":"create","name":&"y"}"#); // malformed: ignored
        sw.control(b, r#"{"t":"create","name":"y"}"#);
        sw.control(c, r#"{"t":"create","name":"z"}"#);
        assert_eq!(last(&texts(&mut rc), "error")["code"], "limit"); // two hosted from this address
        assert!(texts(&mut rb).iter().any(|m| m["t"] == "room"));
        sw.control(d, &format!(r#"{{"t":"join","room":{}}}"#, room["id"]));
        sw.control(e, &format!(r#"{{"t":"join","room":{}}}"#, room["id"]));
        assert_eq!(last(&texts(&mut re), "error")["code"], "full");
        sw.control(e, r#"{"t":"create","name":"w"}"#);
        sw.control(d, r#"{"t":"create","name":"v"}"#); // d leaves a's room first; 3 rooms already
        assert_eq!(last(&texts(&mut rd), "error")["code"], "limit");
        assert!(texts(&mut ra).iter().any(|m| m["t"] == "room" && m["members"].as_array().unwrap().len() == 1));
        assert_eq!(clean_text(&"r".repeat(50), NAME_MAX).len(), NAME_MAX);
    }

    #[test]
    fn stats() {
        let sw = Switch::new(Limits::default());
        let (tx, _rx) = mpsc::channel(8);
        sw.join(tx).unwrap();
        sw.set_roam_rooms([(0, 1), (2, 2)].into_iter().collect());
        let v: Value = serde_json::from_str(&sw.stats_json()).unwrap();
        assert_eq!(v, json!({"online": 1, "freeRoam": {"total": 3, "rooms": {"0": 1, "2": 2}}, "races": {"rooms": 0}}));
    }

    #[test]
    fn names() {
        assert_eq!(clean_name("  \u{202E}Ann\n "), "Ann");
        assert_eq!(clean_name(""), "Player");
        assert_eq!(clean_name(&"x".repeat(40)).len(), NAME_MAX);
    }
}
