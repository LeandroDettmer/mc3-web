//! mc3-web's server: the start page, the browser build and the disc folders (what tools/web_serve.py serves, at the
//! same URLs), and the virtual LAN switch for System Link at /lan (crates/lan).
//!
//!     mc3-server --web web --build work-remix/build-web --disc game-remix-usa [--port 3000] [--bind 127.0.0.1]
//!
//! --web, --build and --disc repeat.  A file is looked up in each --web folder, then each --build folder (the start
//! page from web/, mcdub3.js / mcdub3remix.js from the builds).  The first --disc is at /disc/, every one at
//! /discs/<folder name>/; /discs.json lists them for the start page, /disc.list and /discs/<name>.list list a disc's
//! files (WASMFS has no directory listing over HTTP), /disc.manifest and /discs/<name>.manifest with size and mtime
//! (web/disc_cache.js).  Every answer carries COOP/COEP/CORP (threads need a
//! cross-origin isolated page) and takes single HTTP Ranges (the game reads the disc in 1 MB ranges).  It binds to
//! localhost unless --bind says otherwise: the disc is yours, not the network's.  --allow-origin (repeats) lets only
//! pages of those origins (scheme://host[:port]) join /lan; without it any page may.  --roam on|off (default on): the free-roam hub
//! at 10.77.0.253 (crates/roam), which links each free-roam player with its nearest.

mod http;

use std::path::PathBuf;
use std::process::exit;
use std::sync::Arc;

use mc3_lan::{Limits, Switch};

fn usage() -> ! {
    eprintln!(
        "usage: mc3-server --web <dir>... [--build <dir>...] [--disc <dir>...] [--port 3000] [--bind 127.0.0.1] [--allow-origin <origin>...] [--roam on|off] [--roam-k 32]"
    );
    exit(2)
}

#[tokio::main]
async fn main() {
    let mut site = http::Site::default();
    let (mut port, mut bind) = (3000u16, "127.0.0.1".to_string());
    let mut builds = Vec::new();
    let mut roam = true;
    let mut roam_opts = mc3_roam::Options::default();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut val = || args.next().unwrap_or_else(|| usage());
        match a.as_str() {
            "--web" => site.roots.push(PathBuf::from(val())),
            "--build" => builds.push(PathBuf::from(val())),
            "--disc" => {
                let d = PathBuf::from(val());
                if !d.join("default.xbe").is_file() {
                    eprintln!("error: no default.xbe in {}", d.display());
                    exit(1);
                }
                let name = d.canonicalize().unwrap_or(d.clone()).file_name().map(|n| n.to_string_lossy().into_owned());
                site.discs.push((name.unwrap_or_else(|| "disc".into()), d));
            }
            "--port" => port = val().parse().unwrap_or_else(|_| usage()),
            "--bind" => bind = val(),
            // repeats, or a comma list (Cloud Run's --args cannot repeat a flag)
            "--allow-origin" => site.allow_origins.extend(
                val().split(',').map(|o| o.trim().trim_end_matches('/').to_string()).filter(|o| !o.is_empty())),
            "--roam" => roam = match val().as_str() { "on" | "1" => true, "off" | "0" => false, _ => usage() },
            // links for a player whose HELLO asks none (MC3 asks its own: MC3_ROAM_CARS / MC3_ROAM_K)
            "--roam-k" => roam_opts.default_k = val().parse().ok().filter(|&k| k > 0).unwrap_or_else(|| usage()),
            _ => usage(),
        }
    }
    site.roots.extend(builds);
    if !site.roots.iter().any(|r| r.join("index.html").is_file()) {
        eprintln!("error: no index.html in any --web/--build folder");
        exit(1);
    }
    if site.discs.is_empty() {
        eprintln!("note: no --disc: the page will load, the game will not find its disc");
    }

    let listener = tokio::net::TcpListener::bind((bind.as_str(), port)).await.unwrap_or_else(|e| {
        eprintln!("error: cannot listen on {bind}:{port}: {e}");
        exit(1)
    });
    let host = if bind == "127.0.0.1" { "localhost" } else { &bind };
    println!(
        "serving {} at http://{host}:{port}/, {} disc(s), System Link switch at ws://{host}:{port}/lan",
        site.roots.iter().map(|r| r.display().to_string()).collect::<Vec<_>>().join(" + "),
        site.discs.len()
    );
    let site = Arc::new(site);
    let switch = Arc::new(Switch::new(Limits::default()));
    if roam {
        mc3_roam::spawn(switch.clone(), roam_opts);
        println!("free-roam hub at 10.77.0.253:{}", mc3_roam::ROAM_PORT);
    }
    loop {
        let Ok((stream, _)) = listener.accept().await else {
            tokio::time::sleep(std::time::Duration::from_millis(50)).await; // out of sockets: let some close
            continue;
        };
        let _ = stream.set_nodelay(true);
        tokio::spawn(http::serve(stream, site.clone(), switch.clone()));
    }
}
