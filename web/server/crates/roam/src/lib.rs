//! The free-roam hub, after nfsu2-web's (web/server-rs/crates/roam, MIT License, Copyright (c) 2026 th0mk), without
//! its NFSU2 layer (crews, lobbies, maps, watch, claims, blocks, one-way links, the busy cap).
//!
//! The hub is the server's host on the switch at 10.77.0.253 ([`mc3_lan::HUB`]; xboxrecomp/src/net/roam/roam_api.h
//! ROAM_HUB_VIP), UDP port 47210.  It tells each free-roam player which players are near it; the snapshots still go
//! straight from player to player through the switch (xboxrecomp/src/net/roam/roam_core.c, hub mode).
//!
//! - player -> hub: a HELLO every second (its id, status, name, and the links it wants: its K; MC3 asks 12-32, MC3_ROAM_CARS), a SNAP
//!   twice a second (where its car is and how fast it goes), a BYE when it stops.
//! - hub -> player: every `tick_ms` (500) a PEERS list of the players linked to it, { virtual address, port, id }; an
//!   empty one says the hub is there.  Every `far_ms` (1 s) a FAR list of the other players of its room, those it is
//!   not linked with first, then the linked ones, each nearest first (at most FAR_MAX): { id, status, place,
//!   orientation, name } each, for map markers (a linked player whose snapshots do not reach it still gets one; the
//!   core drops those it draws as cars).  A core that does not know FAR (type 9) drops it as an unknown type.  The
//!   list goes in parts of FAR_PART, one datagram each, header flags = part | parts << 8, one seq, the nearest part
//!   (0) sent last: a core that knows no parts keeps the last FAR it got, so it still shows the nearest FAR_PART.
//!
//! The links: the pairs of players with a position younger than `stale_ms`, nearest first, each player at most its K
//! (at most `degree`).  "Nearest" looks ahead: a pair counts at the least of its distance once both cars went on
//! `soon_ms` (1 s) at their last velocities, once both went on `look_ms` (4 s), and from either one's view point
//! (`view_m`, 150 m, ahead of where it drives, from its place soon_ms on) to the other one soon_ms on.  A player not in
//! free roam counts `away_m` (100 m) up.  A link stays at least `hold_ms` (5 s) and after that counts at `keep` (half)
//! its distance: a new link costs a handshake and the car's record.  Both ends list each other.
//!
//! Positions are 3D and any axis may be up (the view point follows the velocity, not the car's orientation), so the
//! hub is game-agnostic.  What it keeps, in memory only: per endpoint its address, port, id, status, name, links wanted
//! and last position; gone `gone_ms` after its last message, at its BYE, or when its browser leaves the switch.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, Instant};

use mc3_lan::{HUB, Switch, ToHub};
use tokio::sync::mpsc;

pub const ROAM_PORT: u16 = 47210;
const MAGIC: &[u8; 4] = b"MC3R";
const HDR: usize = 16;
const CRC: usize = 4;
const MSG_HELLO: u8 = 1;
const MSG_PEERS: u8 = 2;
const MSG_SNAP: u8 = 3;
const MSG_BYE: u8 = 8;
const MSG_FAR: u8 = 9;
const HF_CREW: u16 = 0x0200;
const ST_ROAM: u8 = 2;
const LIST_MAX: usize = 96;
/// FAR entries per datagram at most: 50 bytes each at worst, one datagram under 1200 bytes
pub const FAR_PART: usize = 20;
/// FAR entries per list (a city) at most, over ceil(n / FAR_PART) datagrams
pub const FAR_MAX: usize = 256;

#[derive(Clone, Debug)]
pub struct Options {
    /// links a player gets when its HELLO asks for none, and the most it may ask for
    pub default_k: usize,
    pub degree: usize,
    pub tick_ms: u64,
    pub stale_ms: u64,
    pub gone_ms: u64,
    pub soon_ms: u64,
    pub look_ms: u64,
    pub view_m: f64,
    pub away_m: f64,
    pub hold_ms: u64,
    pub keep: f64,
    pub max_players: usize,
    pub far_ms: u64,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            default_k: 32,
            degree: 64,
            tick_ms: 500,
            stale_ms: 60_000,
            gone_ms: 10_000,
            soon_ms: 1000,
            look_ms: 4000,
            view_m: 150.0,
            away_m: 100.0,
            hold_ms: 5000,
            keep: 0.5,
            max_players: mc3_lan::ADDRS as usize,
            far_ms: 1000,
        }
    }
}

// ---- wire (xboxrecomp/src/net/roam/roam_core.h) ----

pub fn crc32(d: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in d {
        c ^= b as u32;
        for _ in 0..8 {
            c = (c >> 1) ^ (0xEDB8_8320 & 0u32.wrapping_sub(c & 1));
        }
    }
    !c
}

#[derive(Clone, Debug, PartialEq)]
pub enum Msg {
    Hello { sender: u32, status: u8, name: String, links: u8, room: u32 },
    Snap { sender: u32, status: u8, p: [f32; 3], v: [f32; 3], q: [i16; 4] },
    Bye { sender: u32 },
    Other,
}

struct Rd<'a> {
    d: &'a [u8],
    n: usize,
}
impl Rd<'_> {
    fn take(&mut self, k: usize) -> Option<&[u8]> {
        let s = self.d.get(self.n..self.n + k)?;
        self.n += k;
        Some(s)
    }
    fn u8(&mut self) -> Option<u8> {
        self.take(1).map(|s| s[0])
    }
    fn u16(&mut self) -> Option<u16> {
        self.take(2).map(|s| u16::from_le_bytes([s[0], s[1]]))
    }
    fn u32(&mut self) -> Option<u32> {
        self.take(4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }
    fn f32(&mut self) -> Option<f32> {
        self.u32().map(f32::from_bits)
    }
}

/// A datagram to the hub: header, CRC and the fields the hub reads (None: not one of ours, or malformed).
pub fn decode(d: &[u8]) -> Option<Msg> {
    if d.len() < HDR + CRC || &d[..4] != MAGIC || d[4] != 1 || u16::from_le_bytes([d[6], d[7]]) as usize != d.len() {
        return None;
    }
    let body = &d[..d.len() - CRC];
    if crc32(body) != u32::from_le_bytes(d[d.len() - 4..].try_into().ok()?) {
        return None;
    }
    let mut r = Rd { d: body, n: 8 };
    let (sender, _seq, flags) = (r.u32()?, r.u16()?, r.u16()?);
    if sender == 0 {
        return None;
    }
    Some(match d[5] {
        MSG_HELLO => {
            let (_core, _plat, status, _port, _cfg, room) = (r.u16()?, r.u8()?, r.u8()?, r.u16()?, r.u16()?, r.u32()?);
            let nl = r.u8()? as usize;
            let name = r.take(nl)?;
            if nl > 24 || name.iter().any(|&c| !(0x20..0x7F).contains(&c)) {
                return None;
            }
            let name: String = name.iter().map(|&c| c as char).collect();
            let seen = r.u8()? as usize;
            r.take(4 * seen)?;
            let mut links = 0;
            if flags & HF_CREW != 0 && r.n < body.len() {
                let crews = r.u8()? as usize;
                r.take(4 * crews)?;
                links = r.u8().unwrap_or(0);
            }
            Msg::Hello { sender, status, name, links, room }
        }
        MSG_SNAP => {
            let (status, _epoch, _cfg, n, _clock) = (r.u8()?, r.u8()?, r.u16()?, r.u8()?, r.u8()?);
            if n == 0 {
                return None;
            }
            let _t = r.u32()?;
            let p = [r.f32()?, r.f32()?, r.f32()?];
            let v = [r.f32()?, r.f32()?, r.f32()?];
            if p.iter().chain(&v).any(|x| !x.is_finite() || x.abs() > 20000.0) {
                return None;
            }
            let q = [r.u16()? as i16, r.u16()? as i16, r.u16()? as i16, r.u16()? as i16];
            Msg::Snap { sender, status, p, v, q }
        }
        MSG_BYE => Msg::Bye { sender },
        _ => Msg::Other,
    })
}

/// A PEERS datagram: (virtual address, port, id) each, the address big-endian.
pub fn encode_peers(sender: u32, seq: u16, list: &[(u32, u16, u32)]) -> Vec<u8> {
    let n = list.len().min(LIST_MAX);
    let len = HDR + 1 + 10 * n + CRC;
    let mut b = Vec::with_capacity(len);
    b.extend_from_slice(MAGIC);
    b.extend_from_slice(&[1, MSG_PEERS]);
    b.extend_from_slice(&(len as u16).to_le_bytes());
    b.extend_from_slice(&sender.to_le_bytes());
    b.extend_from_slice(&seq.to_le_bytes());
    b.extend_from_slice(&[0, 0]);
    b.push(n as u8);
    for &(ip, port, id) in &list[..n] {
        b.extend_from_slice(&ip.to_be_bytes());
        b.extend_from_slice(&port.to_le_bytes());
        b.extend_from_slice(&id.to_le_bytes());
    }
    let c = crc32(&b);
    b.extend_from_slice(&c.to_le_bytes());
    b
}

/// A FAR datagram, part `part` of `parts`: (id, status, place, orientation as the SNAP carried it, name) each, at most
/// FAR_PART.
pub fn encode_far(sender: u32, seq: u16, part: u8, parts: u8, list: &[(u32, u8, [f32; 3], [i16; 4], &str)]) -> Vec<u8> {
    let n = list.len().min(FAR_PART);
    let mut b = Vec::with_capacity(HDR + 1 + 50 * n + CRC);
    b.extend_from_slice(MAGIC);
    b.extend_from_slice(&[1, MSG_FAR, 0, 0]);
    b.extend_from_slice(&sender.to_le_bytes());
    b.extend_from_slice(&seq.to_le_bytes());
    b.extend_from_slice(&[part, parts]);
    b.push(n as u8);
    for &(id, status, p, q, name) in &list[..n] {
        b.extend_from_slice(&id.to_le_bytes());
        b.push(status);
        p.iter().for_each(|x| b.extend_from_slice(&x.to_le_bytes()));
        q.iter().for_each(|x| b.extend_from_slice(&x.to_le_bytes()));
        let nm = &name.as_bytes()[..name.len().min(24)];
        b.push(nm.len() as u8);
        b.extend_from_slice(nm);
    }
    let len = (b.len() + CRC) as u16;
    b[6..8].copy_from_slice(&len.to_le_bytes());
    let c = crc32(&b);
    b.extend_from_slice(&c.to_le_bytes());
    b
}

// ---- the hub ----

type Key = (u32, u16);

struct Player {
    id: u32,
    name: String,
    room: u32,
    status: u8,
    k: usize,
    pos: Option<([f64; 3], [f64; 3], u64)>, // place, velocity, when (ms)
    q: [i16; 4],                            // orientation, as the last SNAP carried it
    last: u64,
    links: HashMap<Key, u64>, // since (ms)
}

pub struct Hub {
    pub opts: Options,
    players: HashMap<Key, Player>,
    id: u32,
    seq: u16,
    far_at: u64,
}

type Place = ([f64; 3], [f64; 3], Option<[f64; 3]>, f64);

fn dist(a: &[f64; 3], b: &[f64; 3]) -> f64 {
    ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2) + (a[2] - b[2]).powi(2)).sqrt()
}

impl Hub {
    pub fn new(opts: Options) -> Hub {
        Hub { opts, players: HashMap::new(), id: 0x4855_4231, seq: 0, far_at: 0 }
    }

    pub fn count(&self) -> usize {
        self.players.len()
    }

    /// A datagram to the hub from `src`:`sport`, at `now` ms.
    pub fn frame(&mut self, src: u32, sport: u16, data: &[u8], now: u64) {
        let key = (src, sport);
        match decode(data) {
            Some(Msg::Hello { sender, status, name, links, room }) => {
                if !self.players.contains_key(&key) {
                    // the same player at another address (the page reconnected, web/lan.js): its old endpoint goes
                    self.players.retain(|_, p| p.id != sender);
                }
                if !self.players.contains_key(&key) && self.players.len() >= self.opts.max_players {
                    return;
                }
                let k = if links == 0 { self.opts.default_k } else { links as usize }.min(self.opts.degree);
                let p = self.players.entry(key).or_insert_with(|| Player {
                    id: sender,
                    name: String::new(),
                    room,
                    status,
                    k,
                    pos: None,
                    q: [0; 4],
                    last: now,
                    links: HashMap::new(),
                });
                if p.id != sender {
                    // a restart at the same endpoint: a new player
                    p.id = sender;
                    p.pos = None;
                    p.links.clear();
                }
                p.name = name;
                p.room = room;
                p.status = status;
                p.k = k;
                p.last = now;
            }
            Some(Msg::Snap { sender, status, p: at, v, q }) => {
                if let Some(p) = self.players.get_mut(&key).filter(|p| p.id == sender) {
                    p.status = status;
                    p.pos = Some((at.map(f64::from), v.map(f64::from), now));
                    p.q = q;
                    p.last = now;
                }
            }
            Some(Msg::Bye { sender }) => {
                if self.players.get(&key).is_some_and(|p| p.id == sender) {
                    self.players.remove(&key);
                }
            }
            _ => {}
        }
    }

    /// Every endpoint of a browser that left the switch.
    pub fn leave(&mut self, vip: u32) {
        self.players.retain(|k, _| k.0 != vip);
    }

    /// The links again, and each player's list: (address, port, PEERS datagram); every `far_ms` also each placed
    /// player's FAR list.
    // shortcut: every pair of players is weighed (O(n^2)): fine to a couple of thousand players; past that, nfsu2-web's
    // grid walk (links.rs Grid/Near) takes each player's nearest only.
    pub fn tick(&mut self, now: u64) -> Vec<(u32, u16, Vec<u8>)> {
        let o = self.opts.clone();
        self.players.retain(|_, p| now.saturating_sub(p.last) < o.gone_ms);
        let keys: Vec<Key> = self.players.keys().copied().collect();
        // where each one will be: soon, later, and its view point
        let places: Vec<Option<Place>> = keys
            .iter()
            .map(|k| {
                let p = &self.players[k];
                let (at, v, t) = p.pos?;
                if now.saturating_sub(t) >= o.stale_ms {
                    return None;
                }
                let roam = p.status == ST_ROAM;
                let age = now.saturating_sub(t) as f64 / 1000.0;
                let go = |s: f64| if roam { [at[0] + v[0] * s, at[1] + v[1] * s, at[2] + v[2] * s] } else { at };
                let soon = go(age + o.soon_ms as f64 / 1000.0);
                let later = go(age + o.look_ms as f64 / 1000.0);
                let speed = dist(&v, &[0.0; 3]);
                let view = (roam && speed > 1.0).then(|| {
                    let k = o.view_m / speed;
                    [soon[0] + v[0] * k, soon[1] + v[1] * k, soon[2] + v[2] * k]
                });
                Some((soon, later, view, if roam { 0.0 } else { o.away_m }))
            })
            .collect();
        let mut pairs: Vec<(f64, usize, usize)> = Vec::new();
        for i in 0..keys.len() {
            let Some((si, li, vi, ai)) = places[i] else { continue };
            for j in i + 1..keys.len() {
                let Some((sj, lj, vj, aj)) = places[j] else { continue };
                if keys[i].0 == keys[j].0 {
                    continue; // two endpoints of one browser
                }
                if self.players[&keys[i]].room != self.players[&keys[j]].room {
                    continue; // another room (MC3: another city)
                }
                let mut d = dist(&si, &sj).min(dist(&li, &lj));
                if let Some(v) = vi {
                    d = d.min(dist(&v, &sj));
                }
                if let Some(v) = vj {
                    d = d.min(dist(&v, &si));
                }
                d += ai + aj;
                if let Some(&since) = self.players[&keys[i]].links.get(&keys[j]) {
                    d = if now.saturating_sub(since) < o.hold_ms { -1.0 } else { d * o.keep };
                }
                pairs.push((d, i, j));
            }
        }
        pairs.sort_by(|a, b| a.0.total_cmp(&b.0).then(a.1.cmp(&b.1)).then(a.2.cmp(&b.2)));
        let mut deg = vec![0usize; keys.len()];
        let mut linked: Vec<HashMap<Key, u64>> = vec![HashMap::new(); keys.len()];
        for (_, i, j) in pairs {
            let (ki, kj) = (self.players[&keys[i]].k, self.players[&keys[j]].k);
            if deg[i] >= ki || deg[j] >= kj {
                continue;
            }
            deg[i] += 1;
            deg[j] += 1;
            let since = self.players[&keys[i]].links.get(&keys[j]).copied().unwrap_or(now);
            linked[i].insert(keys[j], since);
            linked[j].insert(keys[i], since);
        }
        self.seq = self.seq.wrapping_add(1);
        let mut out = Vec::with_capacity(keys.len());
        for (i, k) in keys.iter().enumerate() {
            let links = std::mem::take(&mut linked[i]);
            let list: Vec<(u32, u16, u32)> = links.keys().map(|l| (l.0, l.1, self.players[l].id)).collect();
            out.push((k.0, k.1, encode_peers(self.id, self.seq, &list)));
            self.players.get_mut(k).unwrap().links = links;
        }
        if now.saturating_sub(self.far_at) >= o.far_ms {
            self.far_at = now;
            self.far(&keys, &places, &mut out);
        }
        out
    }

    /// Each FAR list of a player with a known place: the others of its room with a known place (not its own browser),
    /// stale ones too, at their last place (every player of a city shows on the maps), those it is not linked with
    /// first, then the linked ones, each nearest first.
    // shortcut: a sort per player every far_ms (O(n^2 log n)), as tick's O(n^2): the grid walk would take this too.
    fn far(&self, keys: &[Key], places: &[Option<Place>], out: &mut Vec<(u32, u16, Vec<u8>)>) {
        let spot = |j: usize| places[j].map(|(a, ..)| a).or_else(|| self.players[&keys[j]].pos.map(|(a, ..)| a));
        for (i, ki) in keys.iter().enumerate() {
            let Some(ai) = spot(i) else { continue };
            let me = &self.players[ki];
            let mut v: Vec<(bool, f64, usize)> = (0..keys.len())
                .filter(|&j| j != i && keys[j].0 != ki.0 && self.players[&keys[j]].room == me.room)
                .filter_map(|j| spot(j).map(|aj| (me.links.contains_key(&keys[j]), dist(&ai, &aj), j)))
                .collect();
            v.sort_by(|a, b| a.0.cmp(&b.0).then(a.1.total_cmp(&b.1)).then(a.2.cmp(&b.2)));
            let list: Vec<_> = v
                .iter()
                .take(FAR_MAX)
                .map(|&(_, _, j)| {
                    let q = &self.players[&keys[j]];
                    let at = q.pos.map(|(a, ..)| a.map(|x| x as f32)).unwrap_or_default();
                    (q.id, q.status, at, q.q, q.name.as_str())
                })
                .collect();
            let parts = list.len().div_ceil(FAR_PART).max(1);
            for k in (0..parts).rev() {
                // the nearest part last (see the top)
                let l = &list[(k * FAR_PART).min(list.len())..((k + 1) * FAR_PART).min(list.len())];
                out.push((ki.0, ki.1, encode_far(self.id, self.seq, k as u8, parts as u8, l)));
            }
        }
    }

    /// The players as the page's Online tab shows them: {"t":"roam","players":[{"name","status"}]}, by name.
    pub fn list_json(&self) -> String {
        let mut v: Vec<&Player> = self.players.values().collect();
        v.sort_by(|a, b| a.name.cmp(&b.name).then(a.id.cmp(&b.id)));
        let players: Vec<_> = v.iter().map(|p| serde_json::json!({"name": p.name, "status": p.status})).collect();
        serde_json::json!({"t": "roam", "players": players}).to_string()
    }

    /// Players per room id (the stats endpoint): counts only.
    pub fn room_counts(&self) -> std::collections::BTreeMap<u32, usize> {
        let mut m = std::collections::BTreeMap::new();
        for p in self.players.values() {
            *m.entry(p.room).or_insert(0) += 1;
        }
        m
    }

    /// The players linked with `vip`:`port` (tests, logs).
    pub fn links_of(&self, vip: u32, port: u16) -> Vec<(u32, u16)> {
        let mut v: Vec<Key> = self.players.get(&(vip, port)).map(|p| p.links.keys().copied().collect()).unwrap_or_default();
        v.sort();
        v
    }
}

/// The hub on a task of its own, attached to the switch: frames in order, a tick every `tick_ms`, its lists out
/// through the switch.
pub fn spawn(switch: Arc<Switch>, opts: Options) {
    let (tx, mut rx) = mpsc::channel::<ToHub>(8192);
    switch.attach_hub(tx);
    let tick = Duration::from_millis(opts.tick_ms.max(50));
    let mut hub = Hub::new(opts);
    tokio::spawn(async move {
        let t0 = Instant::now();
        let ms = || t0.elapsed().as_millis() as u64;
        let mut iv = tokio::time::interval(tick);
        loop {
            tokio::select! {
                m = rx.recv() => match m {
                    Some(ToHub::Frame { src, sport, dport, data }) if dport == ROAM_PORT => hub.frame(src, sport, &data, ms()),
                    Some(ToHub::Leave(vip)) => hub.leave(vip),
                    Some(_) => {}
                    None => return,
                },
                _ = iv.tick() => {
                    for (vip, port, d) in hub.tick(ms()) {
                        switch.inject(HUB, ROAM_PORT, vip, port, &d);
                    }
                    switch.set_roam_list(hub.list_json());
                    switch.set_roam_rooms(hub.room_counts());
                }
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dgram(kind: u8, sender: u32, flags: u16, body: &[u8]) -> Vec<u8> {
        let len = HDR + body.len() + CRC;
        let mut b = Vec::new();
        b.extend_from_slice(MAGIC);
        b.extend_from_slice(&[1, kind]);
        b.extend_from_slice(&(len as u16).to_le_bytes());
        b.extend_from_slice(&sender.to_le_bytes());
        b.extend_from_slice(&[0, 0]);
        b.extend_from_slice(&flags.to_le_bytes());
        b.extend_from_slice(body);
        let c = crc32(&b);
        b.extend_from_slice(&c.to_le_bytes());
        b
    }
    fn hello(sender: u32, name: &str, links: u8) -> Vec<u8> {
        hello_in(sender, name, links, 0)
    }
    fn hello_in(sender: u32, name: &str, links: u8, room: u32) -> Vec<u8> {
        let mut b = vec![16, 0, 4, ST_ROAM];
        b.extend_from_slice(&ROAM_PORT.to_le_bytes());
        b.extend_from_slice(&[0, 0]);
        b.extend_from_slice(&room.to_le_bytes());
        b.push(name.len() as u8);
        b.extend_from_slice(name.as_bytes());
        b.push(0);
        if links > 0 {
            b.extend_from_slice(&[0, links]);
        }
        dgram(MSG_HELLO, sender, if links > 0 { HF_CREW | 0x0040 | 0x0020 } else { 0 }, &b)
    }
    fn snap(sender: u32, p: [f32; 3], v: [f32; 3]) -> Vec<u8> {
        let mut b = vec![ST_ROAM, 0, 0, 0, 1, 0];
        b.extend_from_slice(&1234u32.to_le_bytes());
        for x in p.iter().chain(&v) {
            b.extend_from_slice(&x.to_le_bytes());
        }
        b.extend_from_slice(&[0; 52 - 28]);
        dgram(MSG_SNAP, sender, 0, &b)
    }
    fn vip(i: u32) -> u32 {
        mc3_lan::vip_at(i)
    }

    #[test]
    fn rooms_counted() {
        let mut h = Hub::new(Options::default());
        for (i, room) in [0u32, 2, 2].into_iter().enumerate() {
            h.frame(vip(i as u32), ROAM_PORT, &hello_in(100 + i as u32, "x", 0, room), 0);
        }
        assert_eq!(h.room_counts().into_iter().collect::<Vec<_>>(), vec![(0, 1), (2, 2)]);
    }

    /// Players of one room (MC3: city) are linked and far-listed only with each other, side by side; no room = 0.
    #[test]
    fn rooms_apart() {
        let mut h = Hub::new(Options::default());
        for (i, room) in [0u32, 1, 1, 0].into_iter().enumerate() {
            let i = i as u32;
            h.frame(vip(i), ROAM_PORT, &hello_in(100 + i, "x", 7, room), 0);
            h.frame(vip(i), ROAM_PORT, &snap(100 + i, [i as f32, 0.0, 0.0], [0.0; 3]), 0);
        }
        let out = h.tick(2000);
        assert_eq!(h.links_of(vip(0), ROAM_PORT), vec![(vip(3), ROAM_PORT)]);
        assert_eq!(h.links_of(vip(1), ROAM_PORT), vec![(vip(2), ROAM_PORT)]);
        // each FAR list holds its one room-mate (linked), never a player of the other room
        assert!(out.iter().filter(|o| o.2[5] == MSG_FAR).all(|o| o.2[16] == 1), "only its own room");
        // player 3 drives to Atlanta: linked with 1 and 2 at the next tick
        h.frame(vip(3), ROAM_PORT, &hello_in(103, "x", 7, 1), 2100);
        h.tick(2500);
        assert!(h.links_of(vip(0), ROAM_PORT).is_empty());
        assert_eq!(h.links_of(vip(3), ROAM_PORT), vec![(vip(1), ROAM_PORT), (vip(2), ROAM_PORT)]);
    }

    #[test]
    fn wire() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(decode(&hello(5, "Ann", 7)), Some(Msg::Hello { sender: 5, status: ST_ROAM, name: "Ann".into(), links: 7, room: 0 }));
        assert_eq!(decode(&hello(5, "Ann", 0)), Some(Msg::Hello { sender: 5, status: ST_ROAM, name: "Ann".into(), links: 0, room: 0 }));
        let mut s = snap(6, [1.0, 2.0, 3.0], [4.0, 0.0, 0.0]);
        assert_eq!(decode(&s), Some(Msg::Snap { sender: 6, status: ST_ROAM, p: [1.0, 2.0, 3.0], v: [4.0, 0.0, 0.0], q: [0; 4] }));
        s[20] ^= 1;
        assert_eq!(decode(&s), None);
        assert_eq!(decode(&dgram(MSG_BYE, 6, 0, &[2])), Some(Msg::Bye { sender: 6 }));
        let p = encode_peers(9, 1, &[(0x0A4D_0102, ROAM_PORT, 77)]);
        assert_eq!(&p[..4], b"MC3R");
        assert_eq!(p.len(), HDR + 1 + 10 + CRC);
        assert_eq!(&p[17..21], &[10, 77, 1, 2]);
        assert_eq!(u32::from_le_bytes(p[p.len() - 4..].try_into().unwrap()), crc32(&p[..p.len() - 4]));
    }

    /// 20 players on a line 10 m apart, K = 3: each linked with near ones only, both ways, at most K.
    #[test]
    fn nearest_k() {
        let mut h = Hub::new(Options::default());
        for i in 0..20u32 {
            h.frame(vip(i), ROAM_PORT, &hello(100 + i, &format!("P{i}"), 3), 0);
            h.frame(vip(i), ROAM_PORT, &snap(100 + i, [10.0 * i as f32, 0.0, 0.0], [0.0; 3]), 0);
        }
        let out = h.tick(10);
        assert_eq!(out.len(), 20);
        for i in 0..20u32 {
            let l = h.links_of(vip(i), ROAM_PORT);
            assert!(!l.is_empty() && l.len() <= 3, "{i}: {l:?}");
            for (v, port) in &l {
                assert!(h.links_of(*v, *port).contains(&(vip(i), ROAM_PORT)), "both ways");
            }
            // the nearest ones first (a player left with room links farther ones too)
            for j in [i.wrapping_sub(1), i + 1].into_iter().filter(|&j| j < 20) {
                assert!(l.contains(&(vip(j), ROAM_PORT)), "{i} not linked with its neighbour {j}: {l:?}");
            }
        }
        // each list as the C core decodes it: its entries are the links
        let (_, _, d) = out.iter().find(|o| o.0 == vip(10)).unwrap();
        assert_eq!(d[16] as usize, h.links_of(vip(10), ROAM_PORT).len());
        assert!(h.list_json().contains("\"P10\""));
    }

    /// A car driving towards another is linked before a nearer one it drives away from (look-ahead); a link holds.
    /// A page that reconnects comes back at another address with the same player id: one player, the new endpoint.
    #[test]
    fn reconnect_moves_the_player() {
        let mut h = Hub::new(Options::default());
        for i in 0..2 {
            h.frame(vip(i), ROAM_PORT, &hello(100 + i, "x", 1), 0);
            h.frame(vip(i), ROAM_PORT, &snap(100 + i, [i as f32, 0.0, 0.0], [0.0; 3]), 0);
        }
        h.frame(vip(9), ROAM_PORT, &hello(101, "x", 1), 100);
        h.frame(vip(9), ROAM_PORT, &snap(101, [1.0, 0.0, 0.0], [0.0; 3]), 100);
        h.tick(100);
        assert_eq!(h.links_of(vip(0), ROAM_PORT), vec![(vip(9), ROAM_PORT)]);
        assert!(h.links_of(vip(1), ROAM_PORT).is_empty());
    }

    #[test]
    fn look_ahead_and_hold() {
        let mut h = Hub::new(Options::default());
        let k1 = |h: &mut Hub, i: u32, p: [f32; 3], v: [f32; 3]| {
            h.frame(vip(i), ROAM_PORT, &hello(100 + i, "x", 1), 0);
            h.frame(vip(i), ROAM_PORT, &snap(100 + i, p, v), 0);
        };
        k1(&mut h, 0, [0.0, 0.0, 0.0], [30.0, 0.0, 0.0]); // drives along +x at 30 m/s
        k1(&mut h, 1, [-60.0, 0.0, 0.0], [0.0; 3]); // 60 m behind it
        k1(&mut h, 2, [200.0, 0.0, 0.0], [0.0; 3]); // 200 m ahead: in 4 s it is 80 m off, its view point on it
        h.tick(0);
        assert_eq!(h.links_of(vip(0), ROAM_PORT), vec![(vip(2), ROAM_PORT)]);
        // player 1 comes right next to it: the link to 2 holds for hold_ms
        h.frame(vip(1), ROAM_PORT, &snap(101, [1.0, 0.0, 0.0], [30.0, 0.0, 0.0]), 1000);
        h.frame(vip(0), ROAM_PORT, &snap(100, [0.0, 0.0, 0.0], [30.0, 0.0, 0.0]), 1000);
        h.tick(1000);
        assert_eq!(h.links_of(vip(0), ROAM_PORT), vec![(vip(2), ROAM_PORT)]);
        for t in [6000u64, 6500] {
            for i in 0..3 {
                h.frame(vip(i), ROAM_PORT, &hello(100 + i, "x", 1), t);
            }
        }
        h.frame(vip(1), ROAM_PORT, &snap(101, [1.0, 0.0, 0.0], [30.0, 0.0, 0.0]), 6500);
        h.frame(vip(0), ROAM_PORT, &snap(100, [0.0, 0.0, 0.0], [30.0, 0.0, 0.0]), 6500);
        h.frame(vip(2), ROAM_PORT, &snap(102, [2000.0, 0.0, 0.0], [0.0; 3]), 6500);
        h.tick(6500);
        assert_eq!(h.links_of(vip(0), ROAM_PORT), vec![(vip(1), ROAM_PORT)]);
    }

    /// Every far_ms each placed player hears the others of its room, unlinked first, nearest first, with name and place.
    #[test]
    fn far_list() {
        let mut h = Hub::new(Options::default());
        for i in 0..4u32 {
            h.frame(vip(i), ROAM_PORT, &hello(100 + i, &format!("P{i}"), 1), 0);
            h.frame(vip(i), ROAM_PORT, &snap(100 + i, [100.0 * i as f32, 0.0, 5.0], [0.0; 3]), 0);
        }
        h.frame(vip(9), ROAM_PORT, &hello(109, "nowhere", 1), 0); // no place yet: in nobody's list, gets none
        assert!(h.tick(500).iter().all(|o| o.2[5] == MSG_PEERS), "not due yet");
        let out = h.tick(2000);
        let far: Vec<_> = out.iter().filter(|o| o.2[5] == MSG_FAR).collect();
        assert_eq!(far.len(), 4);
        let (_, _, d) = far.iter().find(|o| o.0 == vip(0)).unwrap();
        assert_eq!(&d[..4], b"MC3R");
        assert_eq!(u16::from_le_bytes([d[6], d[7]]) as usize, d.len());
        assert_eq!(u32::from_le_bytes(d[d.len() - 4..].try_into().unwrap()), crc32(&d[..d.len() - 4]));
        // 0 is linked with 1 (K = 1): 2 and 3 first, nearest first, then 1
        assert_eq!(h.links_of(vip(0), ROAM_PORT), vec![(vip(1), ROAM_PORT)]);
        assert_eq!(d[16], 3);
        let e = &d[17..];
        assert_eq!(u32::from_le_bytes(e[..4].try_into().unwrap()), 102);
        assert_eq!(e[4], ST_ROAM);
        assert_eq!(f32::from_le_bytes(e[5..9].try_into().unwrap()), 200.0);
        assert_eq!(f32::from_le_bytes(e[13..17].try_into().unwrap()), 5.0);
        assert_eq!(&e[25..28], &[2, b'P', b'2']);
        assert_eq!(u32::from_le_bytes(e[28..32].try_into().unwrap()), 103);
        assert_eq!(u32::from_le_bytes(e[56..60].try_into().unwrap()), 101, "the linked one last");
        assert_eq!(decode(d), Some(Msg::Other), "a hub-bound decoder skips it");
        assert!(h.tick(2500).iter().all(|o| o.2[5] == MSG_PEERS), "next one at 3000");
    }

    /// A city of 60 players: every receiver hears all 59 others over 3 FAR datagrams, the nearest part last.
    #[test]
    fn far_list_parts() {
        let mut h = Hub::new(Options::default());
        for i in 0..60u32 {
            h.frame(vip(i), ROAM_PORT, &hello(100 + i, &format!("Player number {i}"), 3), 0);
            h.frame(vip(i), ROAM_PORT, &snap(100 + i, [37.0 * i as f32, 0.0, -11.0 * i as f32], [0.0; 3]), 0);
        }
        let out = h.tick(1000);
        let far: Vec<_> = out.iter().filter(|o| o.2[5] == MSG_FAR).collect();
        assert_eq!(far.len(), 60 * 3);
        for i in 0..60u32 {
            let mine: Vec<&Vec<u8>> = far.iter().filter(|o| o.0 == vip(i)).map(|o| &o.2).collect();
            let flags: Vec<u16> = mine.iter().map(|d| u16::from_le_bytes([d[14], d[15]])).collect();
            assert_eq!(flags, vec![2 | 3 << 8, 1 | 3 << 8, 3 << 8], "parts 2, 1, 0: the nearest one last");
            let mut ids = vec![];
            for d in &mine {
                assert!(d.len() < 1200 && u16::from_le_bytes([d[6], d[7]]) as usize == d.len());
                assert_eq!(u32::from_le_bytes(d[d.len() - 4..].try_into().unwrap()), crc32(&d[..d.len() - 4]));
                let mut o = 17;
                for _ in 0..d[16] {
                    ids.push(u32::from_le_bytes(d[o..o + 4].try_into().unwrap()));
                    o += 26 + d[o + 25] as usize;
                }
            }
            ids.sort();
            assert_eq!(ids, (0..60).filter(|&j| j != i).map(|j| 100 + j).collect::<Vec<_>>(), "{i} hears all 59");
        }
    }

    #[test]
    fn gone() {
        let mut h = Hub::new(Options::default());
        h.frame(vip(0), ROAM_PORT, &hello(1, "a", 0), 0);
        h.frame(vip(1), ROAM_PORT, &hello(2, "b", 0), 0);
        h.frame(vip(1), ROAM_PORT, &dgram(MSG_BYE, 2, 0, &[2]), 0);
        assert_eq!(h.count(), 1);
        h.leave(vip(0));
        assert_eq!(h.count(), 0);
        h.frame(vip(2), ROAM_PORT, &hello(3, "c", 0), 0);
        h.tick(20_000);
        assert_eq!(h.count(), 0, "silent players go after gone_ms");
    }

    /// Through the switch: a player's HELLO and SNAP reach the hub task, its list comes back as a kind 3 frame.
    #[tokio::test]
    async fn through_the_switch() {
        let sw = Arc::new(Switch::new(mc3_lan::Limits::default()));
        spawn(sw.clone(), Options { tick_ms: 50, ..Options::default() });
        let mut rxs = Vec::new();
        let mut vips = Vec::new();
        for i in 0..2u32 {
            let (tx, rx) = mpsc::channel(256);
            let v = sw.join(tx).unwrap();
            vips.push(v);
            rxs.push(rx);
            for d in [hello(10 + i, "p", 7), snap(10 + i, [i as f32, 0.0, 0.0], [0.0; 3])] {
                let mut f = vec![3, 0, 0, 0];
                f.extend_from_slice(&v.to_be_bytes());
                f.extend_from_slice(&HUB.to_be_bytes());
                f.extend_from_slice(&ROAM_PORT.to_be_bytes());
                f.extend_from_slice(&ROAM_PORT.to_be_bytes());
                f.extend_from_slice(&d);
                assert_eq!(sw.frame(v, &f, Instant::now()), mc3_lan::Verdict::Sent(1));
            }
        }
        let got = tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                if let Some(mc3_lan::Out::Bin(b)) = rxs[0].recv().await
                    && b.len() > HDR + 16
                    && b[16 + 5] == MSG_PEERS
                    && b[16 + 16] == 1
                {
                    return b;
                }
            }
        })
        .await
        .expect("a PEERS list with one entry");
        assert_eq!(&got[4..8], &HUB.to_be_bytes());
        assert_eq!(&got[16 + 17..16 + 21], &vips[1].to_be_bytes());
    }
}
