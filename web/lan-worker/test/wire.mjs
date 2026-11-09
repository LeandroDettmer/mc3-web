// Test helpers: roam datagrams as xboxrecomp/src/net/roam/roam_core.c writes them (crates/roam tests' dgram/hello/snap).
import { crc32, ROAM_PORT, MSG_HELLO, MSG_SNAP, HF_CREW, ST_ROAM } from '../src/roam.js';

const le16 = n => [n & 255, n >>> 8 & 255], le32 = n => [n & 255, n >>> 8 & 255, n >>> 16 & 255, n >>> 24 & 255];
const f32 = x => [...new Uint8Array(new Float32Array([x]).buffer)];

export function dgram(kind, sender, flags, body) {
  const len = 16 + body.length + 4;
  const b = [0x4D, 0x43, 0x33, 0x52, 1, kind, ...le16(len), ...le32(sender), 0, 0, ...le16(flags), ...body];
  return Uint8Array.from([...b, ...le32(crc32(Uint8Array.from(b)))]);
}
export function hello(sender, name, links, room = 0) {
  const b = [16, 0, 4, ST_ROAM, ...le16(ROAM_PORT), 0, 0, ...le32(room), name.length, ...Buffer.from(name), 0];
  if (links > 0) b.push(0, links);
  return dgram(MSG_HELLO, sender, links > 0 ? HF_CREW | 0x0040 | 0x0020 : 0, b);
}
export function snap(sender, p, v) {
  const b = [ST_ROAM, 0, 0, 0, 1, 0, ...le32(1234), ...[...p, ...v].flatMap(f32), ...new Array(52 - 28).fill(0)];
  return dgram(MSG_SNAP, sender, 0, b);
}
