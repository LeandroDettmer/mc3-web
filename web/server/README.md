# mc3-server

The browser build's server: the start page, the builds and the disc folders
(the URLs tools/web_serve.py serves: `/`, `/discs.json`, `/disc/...`,
`/discs/<name>/...`, `<name>.list`, `<name>.manifest`; COOP/COEP/CORP,
single HTTP Ranges), and the virtual LAN switch for System Link at `/lan`
(`crates/lan`: protocol and limits in its doc comment), with the free-roam
hub at 10.77.0.253:47210 (`crates/roam`: each free-roam player gets a list
of its nearest K, 7 for MC3, twice a second; `--roam off` leaves it out).

    cargo build --release          # Windows, Linux or macOS; Rust 1.85+
    target/release/mc3-server --web ../ --build ../../work-remix/build-web \
        --disc ../../game-remix-usa --port 3000     # from web/server

`--web`, `--build` and `--disc` repeat; `--bind 0.0.0.0` to serve others
(localhost by default). `--allow-origin https://<site>.web.app` (repeats)
lets only pages of those origins join `/lan` (403 otherwise, also without an
Origin header); without it any page may, as before. Tests: `cargo test --workspace`; the two-browser
System Link test is xboxrecomp/tests/xnet_web (check.mjs).

After nfsu2-web's server (https://github.com/th0mk/nfsu2-web, web/server-rs,
MIT License, Copyright (c) 2026 th0mk): `crates/ws` is its WebSocket crate
unchanged (LICENSE-nfsu2-web), `crates/lan` its switch's addresses, frames
and control messages, cut down to System Link; `crates/roam` its free-roam
hub's links (nearest K with look-ahead, view point, hold), without its NFSU2
layer.
