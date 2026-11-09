//! HTTP/1.1 for the page, the build and the discs (GET and HEAD, keep-alive, one Range), and the upgrade to the
//! WebSocket switch at /lan.  Just what the start page and the game ask for; tools/web_serve.py's rules.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use bytes::{Bytes, BytesMut};
use mc3_lan::{Out, Switch, Verdict};
use mc3_ws::{Message, ReadError, WsReader, frame, handshake_response};
use tokio::io::{AsyncReadExt, AsyncSeekExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::mpsc;

const HEAD_MAX: usize = 16 << 10;
const WS_MAX_PAYLOAD: usize = 64 << 10;
const IDLE: Duration = Duration::from_secs(120);

#[derive(Default)]
pub struct Site {
    /// --web folders, then --build folders: a file is taken from the first that has it
    pub roots: Vec<PathBuf>,
    /// (folder name, path); the first is also /disc
    pub discs: Vec<(String, PathBuf)>,
    /// --allow-origin: the pages (scheme://host[:port]) that may join /lan; empty = any
    pub allow_origins: Vec<String>,
}

/// May a page of this Origin join /lan?  No list: any (and none).  A list: only its origins; a client without an
/// Origin header (not a browser) is refused.
fn origin_ok(allow: &[String], origin: Option<&str>) -> bool {
    allow.is_empty() || origin.is_some_and(|o| allow.iter().any(|a| a.eq_ignore_ascii_case(o)))
}

enum Body {
    File(PathBuf),
    Bytes(Vec<u8>, &'static str),
    NotFound,
}

struct Req {
    method: String,
    path: String,
    keep_alive: bool,
    range: Option<String>,
    ws_key: Option<String>,
    origin: Option<String>,
    has_body: bool,
    vip: Option<u32>, // /lan?vip=N: the address a reconnecting page had (web/lan.js)
}

pub async fn serve(mut s: TcpStream, site: Arc<Site>, switch: Arc<Switch>) {
    let mut buf = BytesMut::with_capacity(4096);
    loop {
        // a request's head
        let end = loop {
            if let Some(i) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                break i + 4;
            }
            if buf.len() > HEAD_MAX {
                let _ = reply(&mut s, "431 Request Header Fields Too Large", &[], b"", false).await;
                return;
            }
            match tokio::time::timeout(IDLE, s.read_buf(&mut buf)).await {
                Ok(Ok(n)) if n > 0 => {}
                _ => return,
            }
        };
        let head = buf.split_to(end);
        let Some(req) = parse(&head) else {
            let _ = reply(&mut s, "400 Bad Request", &[], b"", false).await;
            return;
        };
        if req.path == "/lan/stats" && req.method == "GET" {
            // counts only; no Origin = same-origin page, allowed; a cross-origin page must be on the list
            if req.origin.is_some() && !origin_ok(&site.allow_origins, req.origin.as_deref()) {
                let _ = reply(&mut s, "403 Forbidden", &[], b"", false).await;
                return;
            }
            let acao = match (&req.origin, site.allow_origins.is_empty()) {
                (Some(o), false) => o.as_str(),
                _ => "*",
            };
            let body = switch.stats_json();
            let h = [("Content-Type", "application/json"), ("Access-Control-Allow-Origin", acao), ("Vary", "Origin")];
            if reply(&mut s, "200 OK", &h, body.as_bytes(), true).await.is_err() || !req.keep_alive {
                return;
            }
            continue;
        }
        if req.path == "/lan" && req.method == "GET" {
            if !origin_ok(&site.allow_origins, req.origin.as_deref()) {
                let _ = reply(&mut s, "403 Forbidden", &[], b"", false).await;
                return;
            }
            if let Some(key) = &req.ws_key {
                lan(s, buf.freeze(), key, switch, req.vip).await;
            } else {
                let _ = reply(&mut s, "426 Upgrade Required", &[], b"", false).await;
            }
            return;
        }
        if req.has_body || (req.method != "GET" && req.method != "HEAD") {
            let _ = reply(&mut s, "405 Method Not Allowed", &[("Allow", "GET, HEAD")], b"", false).await;
            return;
        }
        if answer(&mut s, &site, &req).await.is_err() || !req.keep_alive {
            return;
        }
    }
}

fn parse(head: &[u8]) -> Option<Req> {
    let text = std::str::from_utf8(head).ok()?;
    let mut lines = text.split("\r\n");
    let mut first = lines.next()?.split(' ');
    let (method, target, version) = (first.next()?, first.next()?, first.next()?);
    if first.next().is_some() || !version.starts_with("HTTP/1.") {
        return None;
    }
    let path = decode(target.split(['?', '#']).next()?)?;
    let query = target.split_once('?').map_or("", |q| q.1).split('#').next().unwrap_or("");
    let vip = query.split('&').find_map(|kv| kv.strip_prefix("vip=")?.parse().ok());
    let mut req = Req { method: method.into(), path, keep_alive: version == "HTTP/1.1", range: None, ws_key: None, origin: None, has_body: false, vip };
    let mut upgrade = false;
    for l in lines.filter(|l| !l.is_empty()) {
        let (k, v) = l.split_once(':')?;
        let v = v.trim();
        match k.trim().to_ascii_lowercase().as_str() {
            "connection" => {
                let v = v.to_ascii_lowercase();
                if v.contains("close") {
                    req.keep_alive = false;
                } else if v.contains("keep-alive") {
                    req.keep_alive = true;
                }
            }
            "range" => req.range = Some(v.into()),
            "upgrade" => upgrade = v.eq_ignore_ascii_case("websocket"),
            "sec-websocket-key" => req.ws_key = Some(v.into()),
            "origin" => req.origin = Some(v.into()),
            "content-length" => req.has_body |= v != "0",
            "transfer-encoding" => req.has_body = true,
            _ => {}
        }
    }
    if !upgrade {
        req.ws_key = None;
    }
    Some(req)
}

/// %XX decoding of a path; None if it is not UTF-8 afterwards.
fn decode(p: &str) -> Option<String> {
    let b = p.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        let hex = |c: u8| (c as char).to_digit(16);
        if b[i] == b'%' && i + 2 < b.len() {
            if let (Some(h), Some(l)) = (hex(b[i + 1]), hex(b[i + 2])) {
                out.push((h * 16 + l) as u8);
                i += 3;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8(out).ok()
}

/// A URL path's parts under a base: '', '.' and '..' dropped (WASMFS asks for /disc//file), and none that a
/// Windows path would read as a separator or drive.
fn under(base: &Path, rest: &str) -> Option<PathBuf> {
    let mut p = base.to_path_buf();
    for part in rest.split('/').filter(|x| !matches!(*x, "" | "." | "..")) {
        if part.contains(['\\', ':', '\0']) {
            return None;
        }
        p.push(part);
    }
    Some(p)
}

fn disc_list(dir: &Path) -> Vec<u8> {
    fn walk(dir: &Path, rel: &str, out: &mut Vec<String>) {
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        let mut ents: Vec<_> = rd.flatten().map(|e| (e.file_name().to_string_lossy().into_owned(), e)).collect();
        ents.sort_by(|a, b| a.0.cmp(&b.0));
        let (dirs, files): (Vec<_>, Vec<_>) = ents.into_iter().partition(|(_, e)| e.file_type().is_ok_and(|t| t.is_dir()));
        for (n, _) in files {
            out.push(format!("{rel}{n}"));
        }
        for (n, e) in dirs {
            walk(&e.path(), &format!("{rel}{n}/"), out);
        }
    }
    let mut out = Vec::new();
    walk(dir, "", &mut out);
    let mut s = out.join("\n");
    s.push('\n');
    s.into_bytes()
}

/// tools/web_serve.py disc_manifest: the listed files with size and mtime (web/disc_cache.js keys its cache on them).
fn disc_manifest(dir: &Path) -> Vec<u8> {
    let files: Vec<_> = String::from_utf8_lossy(&disc_list(dir))
        .lines()
        .filter(|l| !l.is_empty())
        .filter_map(|rel| {
            let m = std::fs::metadata(rel.split('/').fold(dir.to_path_buf(), |p, x| p.join(x))).ok()?;
            let t = m.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?.as_secs();
            Some(serde_json::json!({"p": rel, "s": m.len(), "m": t}))
        })
        .collect();
    serde_json::to_vec(&serde_json::json!({"files": files})).unwrap_or_default()
}

/// tools/web_serve.py disc_info: game-remix* is the Remix, the rest the DUB Edition; the edition follows the prefix.
fn discs_json(site: &Site) -> Vec<u8> {
    let list: Vec<_> = site
        .discs
        .iter()
        .map(|(name, _)| {
            let (game, prefix) = if name.starts_with("game-remix") { ("remix", "game-remix") } else { ("dub", "game") };
            let edition = name.strip_prefix(prefix).unwrap_or("").trim_start_matches('-');
            serde_json::json!({"name": name, "game": game, "edition": edition})
        })
        .collect();
    serde_json::to_vec(&list).unwrap_or_default()
}

fn resolve(site: &Site, path: &str) -> Body {
    const LIST: &str = "text/plain; charset=utf-8";
    const JSON: &str = "application/json";
    let first = site.discs.first().map(|d| &d.1);
    if path == "/discs.json" {
        return Body::Bytes(discs_json(site), JSON);
    }
    if path == "/disc.list" {
        return first.map_or(Body::NotFound, |d| Body::Bytes(disc_list(d), LIST));
    }
    if path == "/disc.manifest" {
        return first.map_or(Body::NotFound, |d| Body::Bytes(disc_manifest(d), JSON));
    }
    let (bases, rest): (Vec<&PathBuf>, &str) = if let (Some(d), Some(rest)) = (first, path.strip_prefix("/disc").filter(|r| r.is_empty() || r.starts_with('/'))) {
        (vec![d], rest)
    } else if let Some(r) = path.strip_prefix("/discs/") {
        let (name, rest) = r.split_once('/').unwrap_or((r, ""));
        if let Some(n) = name.strip_suffix(".list").filter(|_| rest.is_empty()) {
            return site.discs.iter().find(|d| d.0 == n).map_or(Body::NotFound, |d| Body::Bytes(disc_list(&d.1), LIST));
        }
        if let Some(n) = name.strip_suffix(".manifest").filter(|_| rest.is_empty()) {
            return site.discs.iter().find(|d| d.0 == n).map_or(Body::NotFound, |d| Body::Bytes(disc_manifest(&d.1), JSON));
        }
        match site.discs.iter().find(|d| d.0 == name) {
            Some(d) => (vec![&d.1], rest),
            None => (site.roots.iter().collect(), path),
        }
    } else {
        (site.roots.iter().collect(), path)
    };
    for b in bases {
        let Some(mut p) = under(b, rest) else { return Body::NotFound };
        if p.is_dir() {
            p.push("index.html");
        }
        if p.is_file() {
            return Body::File(p);
        }
    }
    Body::NotFound
}

fn mime(p: &Path) -> &'static str {
    match p.extension().and_then(|e| e.to_str()).map(|e| e.to_ascii_lowercase()).as_deref() {
        Some("html" | "htm") => "text/html; charset=utf-8",
        Some("js" | "mjs") => "text/javascript",
        Some("wasm") => "application/wasm",
        Some("json") => "application/json",
        Some("css") => "text/css",
        Some("png") => "image/png",
        Some("svg") => "image/svg+xml",
        Some("ico") => "image/x-icon",
        Some("txt" | "list" | "csv" | "ini") => "text/plain; charset=utf-8",
        _ => "application/octet-stream",
    }
}

/// tools/web_serve.py's reading of a Range: one `bytes=a-b`, `bytes=a-` or `bytes=-n`; anything else is the whole
/// file.  Err(()) = unsatisfiable (416).
fn range(h: &str, size: u64) -> Option<Result<(u64, u64), ()>> {
    let (a, b) = h.trim().strip_prefix("bytes=")?.split_once('-')?;
    let num = |s: &str| if s.is_empty() { Some(None) } else if s.bytes().all(|c| c.is_ascii_digit()) { s.parse().ok().map(Some) } else { None };
    let (a, b): (Option<u64>, Option<u64>) = (num(a)?, num(b)?);
    let (start, end) = match (a, b) {
        (None, None) => return None,
        (Some(s), e) => (s, e.map_or(size.saturating_sub(1), |e| e.min(size.saturating_sub(1)))),
        (None, Some(n)) => (size.saturating_sub(n), size.saturating_sub(1)),
    };
    Some(if start >= size || start > end { Err(()) } else { Ok((start, end)) })
}

async fn reply(s: &mut TcpStream, status: &str, extra: &[(&str, &str)], body: &[u8], with_body: bool) -> std::io::Result<()> {
    let mut h = format!("HTTP/1.1 {status}\r\n");
    for (k, v) in extra {
        h.push_str(&format!("{k}: {v}\r\n"));
    }
    h.push_str("Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\n");
    h.push_str("Cross-Origin-Resource-Policy: same-origin\r\nAccept-Ranges: bytes\r\nCache-Control: no-cache\r\n");
    h.push_str(&format!("Content-Length: {}\r\n\r\n", body.len()));
    s.write_all(h.as_bytes()).await?;
    if with_body {
        s.write_all(body).await?;
    }
    Ok(())
}

async fn answer(s: &mut TcpStream, site: &Site, req: &Req) -> std::io::Result<()> {
    let get = req.method == "GET";
    let path = match resolve(site, &req.path) {
        Body::Bytes(b, ct) => return reply(s, "200 OK", &[("Content-Type", ct)], &b, get).await,
        Body::NotFound => return reply(s, "404 Not Found", &[("Content-Type", "text/plain")], b"not found\n", get).await,
        Body::File(p) => p,
    };
    let Ok(mut f) = tokio::fs::File::open(&path).await else {
        return reply(s, "404 Not Found", &[("Content-Type", "text/plain")], b"not found\n", get).await;
    };
    let size = f.metadata().await?.len();
    let (status, start, len, cr) = match req.range.as_deref().and_then(|r| range(r, size)) {
        Some(Err(())) => return reply(s, "416 Range Not Satisfiable", &[("Content-Range", &format!("bytes */{size}"))], b"", get).await,
        Some(Ok((a, b))) => ("206 Partial Content", a, b - a + 1, Some(format!("bytes {a}-{b}/{size}"))),
        None => ("200 OK", 0, size, None),
    };
    let mut h = format!("HTTP/1.1 {status}\r\nContent-Type: {}\r\nContent-Length: {len}\r\n", mime(&path));
    if let Some(cr) = cr {
        h.push_str(&format!("Content-Range: {cr}\r\n"));
    }
    h.push_str("Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\n");
    h.push_str("Cross-Origin-Resource-Policy: same-origin\r\nAccept-Ranges: bytes\r\nCache-Control: no-cache\r\n\r\n");
    s.write_all(h.as_bytes()).await?;
    if get {
        f.seek(std::io::SeekFrom::Start(start)).await?;
        let n = tokio::io::copy(&mut f.take(len), s).await?;
        if n != len {
            return Err(std::io::ErrorKind::UnexpectedEof.into()); // the file shrank: the client must not reuse us
        }
    }
    Ok(())
}

/// A /lan connection: the handshake, then the switch's frames both ways until either side closes.
async fn lan(mut s: TcpStream, pre: Bytes, key: &str, switch: Arc<Switch>, want: Option<u32>) {
    if s.write_all(&handshake_response(key, None)).await.is_err() {
        return;
    }
    // shortcut: the TCP peer's address; behind a reverse proxy every client shares its address (rooms per IP)
    let ip = s.peer_addr().ok().map(|a| a.ip());
    let (rd, mut wr) = s.into_split();
    let (tx, mut rx) = mpsc::channel::<Out>(switch.limits.queue);
    let Some(vip) = switch.join_from(tx.clone(), ip, want) else {
        let mut b = Vec::new();
        frame::encode(frame::CLOSE, &frame::close_payload(1013, "LAN full"), &mut b);
        let _ = wr.write_all(&b).await;
        return;
    };
    let writer = tokio::spawn(async move {
        let mut b = Vec::new();
        while let Some(o) = rx.recv().await {
            // what is queued goes out in one write
            let mut next = Some(o);
            let mut close = false;
            while let Some(o) = next.take() {
                match o {
                    Out::Bin(d) => frame::encode(frame::BINARY, &d, &mut b),
                    Out::Text(t) => frame::encode(frame::TEXT, t.as_bytes(), &mut b),
                    Out::Pong(p) => frame::encode(frame::PONG, &p, &mut b),
                    Out::Close(code, why) => {
                        frame::encode(frame::CLOSE, &frame::close_payload(code, why), &mut b);
                        close = true;
                        break;
                    }
                }
                if b.len() < 64 << 10 {
                    next = rx.try_recv().ok();
                }
            }
            if wr.write_all(&b).await.is_err() || close {
                return;
            }
            b.clear();
        }
    });
    let mut r = WsReader::new(rd, pre, WS_MAX_PAYLOAD);
    loop {
        let m = match tokio::time::timeout(IDLE, r.next()).await {
            Ok(m) => m,
            Err(_) => {
                let _ = tx.send(Out::Close(1001, "idle")).await;
                break;
            }
        };
        match m {
            Ok(Message::Binary(b)) => {
                if let v @ (Verdict::TooShort | Verdict::TooLong) = switch.frame(vip, &b, Instant::now()) {
                    let _ = tx.send(Out::Close(1009, if v == Verdict::TooLong { "frame too long" } else { "frame too short" })).await;
                    break;
                }
            }
            Ok(Message::Text(t)) => switch.control(vip, std::str::from_utf8(&t).unwrap_or("")),
            Ok(Message::Ping(p)) => {
                let _ = tx.try_send(Out::Pong(p));
            }
            Ok(Message::Pong(_)) => {}
            Ok(Message::Close(code, _)) => {
                let _ = tx.send(Out::Close(if code == 1005 { 1000 } else { code }, "")).await;
                break;
            }
            Err(ReadError::Protocol { code, reason }) => {
                let _ = tx.send(Out::Close(code, reason)).await;
                break;
            }
            Err(ReadError::Closed) => break,
        }
    }
    switch.leave(vip);
    drop(tx);
    let _ = tokio::time::timeout(Duration::from_secs(5), writer).await;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ranges() {
        assert_eq!(range("bytes=0-9", 100), Some(Ok((0, 9))));
        assert_eq!(range("bytes=90-", 100), Some(Ok((90, 99))));
        assert_eq!(range("bytes=95-200", 100), Some(Ok((95, 99))));
        assert_eq!(range("bytes=-10", 100), Some(Ok((90, 99))));
        assert_eq!(range("bytes=100-", 100), Some(Err(())));
        assert_eq!(range("bytes=5-1", 100), Some(Err(())));
        assert_eq!(range("bytes=0-1,4-5", 100), None);
        assert_eq!(range("bytes=-", 100), None);
    }

    #[test]
    fn paths() {
        assert_eq!(decode("/discs/game%20x/a%2Fb").as_deref(), Some("/discs/game x/a/b"));
        assert_eq!(decode("/a%").as_deref(), Some("/a%"));
        let b = Path::new("base");
        assert_eq!(under(b, "/../x//./y"), Some(b.join("x").join("y")));
        assert_eq!(under(b, "/C:/Windows"), None);
        assert_eq!(under(b, "/a\\..\\b"), None);
    }

    #[test]
    fn origins() {
        let list = vec!["https://mc3.web.app".to_string()];
        assert!(origin_ok(&[], None));
        assert!(origin_ok(&[], Some("http://x")));
        assert!(origin_ok(&list, Some("https://MC3.web.app")));
        assert!(!origin_ok(&list, Some("https://evil.example")));
        assert!(!origin_ok(&list, None));
    }
}
