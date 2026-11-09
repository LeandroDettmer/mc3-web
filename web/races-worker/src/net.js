// The virtual LAN's addresses and the rooms' limits (crates/lan), shared by the races Lobby and the /lan switch
// (web/lan-worker). Not in index.js: workerd takes every named export of a Worker's entry module for an entrypoint
// and refuses plain values there.

export const NET = 0x0A4D0000, MASK = 0xFFFFC000, NETS = 64, ADDRS = 64 * 250;   // crates/lan: 10.77.0.0/18, vip_at()
export const LIMITS = { members: 8, rooms: 256, roomsPerIp: 2, name: 32, text: 24, pass: 64, signal: 16384, msg: 20480 };

export const vipAt = i => (NET | (i % NETS) << 8 | (2 + Math.floor(i / NETS))) >>> 0;
export const vipStr = v => [v >>> 24, v >>> 16 & 255, v >>> 8 & 255, v & 255].join('.');
const hex2 = n => n.toString(16).toUpperCase().padStart(2, '0');
export const macStr = v => `00:50:F2:${hex2(v >>> 16 & 255)}:${hex2(v >>> 8 & 255)}:${hex2(v & 255)}`;
