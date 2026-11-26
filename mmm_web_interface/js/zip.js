// A small ZIP reader and writer for the all-presets file. Patchling only runs in Chrome and other Chromium browsers, which
// have the deflate pieces built in (CompressionStream and DecompressionStream), so no library is needed.
// Writes UTF-8 names and deflates a file when that makes it smaller. Reads stored and deflated files, so a zip made by another
// tool (Finder, Explorer, zip) opens too. No encryption, no zip64.

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

async function through(bytes, transform) {
  const stream = new Blob([bytes]).stream().pipeThrough(transform);
  return new Uint8Array(await new Response(stream).arrayBuffer());
}
const deflateRaw = (bytes) => through(bytes, new CompressionStream('deflate-raw'));
const inflateRaw = (bytes) => through(bytes, new DecompressionStream('deflate-raw'));

function dosTime(d) { return (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1); }
function dosDate(d) { return ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(); }

/** files: [{ name, data: Uint8Array }]. Returns the zip as a Uint8Array. */
export async function makeZip(files, when = new Date()) {
  const enc = new TextEncoder();
  const chunks = [];
  const central = [];
  let offset = 0;
  const time = dosTime(when), date = dosDate(when);
  for (const f of files) {
    const name = enc.encode(f.name);
    const crc = crc32(f.data);
    const packed = await deflateRaw(f.data);
    const method = packed.length < f.data.length ? 8 : 0;
    const body = method === 8 ? packed : f.data;

    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, 0x04034B50, true);
    local.setUint16(4, 20, true);
    local.setUint16(6, 0x0800, true); // names are UTF-8
    local.setUint16(8, method, true);
    local.setUint16(10, time, true);
    local.setUint16(12, date, true);
    local.setUint32(14, crc, true);
    local.setUint32(18, body.length, true);
    local.setUint32(22, f.data.length, true);
    local.setUint16(26, name.length, true);
    local.setUint16(28, 0, true);
    chunks.push(new Uint8Array(local.buffer), name, body);

    const entry = new DataView(new ArrayBuffer(46));
    entry.setUint32(0, 0x02014B50, true);
    entry.setUint16(4, 20, true);
    entry.setUint16(6, 20, true);
    entry.setUint16(8, 0x0800, true);
    entry.setUint16(10, method, true);
    entry.setUint16(12, time, true);
    entry.setUint16(14, date, true);
    entry.setUint32(16, crc, true);
    entry.setUint32(20, body.length, true);
    entry.setUint32(24, f.data.length, true);
    entry.setUint16(28, name.length, true);
    entry.setUint32(42, offset, true);
    central.push(new Uint8Array(entry.buffer), name);
    offset += 30 + name.length + body.length;
  }
  const centralSize = central.reduce((n, c) => n + c.length, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054B50, true);
  end.setUint16(8, files.length, true);
  end.setUint16(10, files.length, true);
  end.setUint32(12, centralSize, true);
  end.setUint32(16, offset, true);
  const all = [...chunks, ...central, new Uint8Array(end.buffer)];
  const out = new Uint8Array(all.reduce((n, c) => n + c.length, 0));
  let at = 0;
  for (const c of all) { out.set(c, at); at += c.length; }
  return out;
}

/** The files in a zip: [{ name, data: Uint8Array }], folders left out. Throws on anything that is not a readable zip. */
export async function readZip(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let end = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 22 - 65535); i--) {
    if (view.getUint32(i, true) === 0x06054B50) { end = i; break; }
  }
  if (end < 0) throw new Error('not-a-zip');
  const count = view.getUint16(end + 10, true);
  let at = view.getUint32(end + 16, true);
  const dec = new TextDecoder();
  const files = [];
  for (let n = 0; n < count; n++) {
    if (view.getUint32(at, true) !== 0x02014B50) throw new Error('not-a-zip');
    const flags = view.getUint16(at + 8, true);
    const method = view.getUint16(at + 10, true);
    const crc = view.getUint32(at + 16, true);
    const packedSize = view.getUint32(at + 20, true);
    const size = view.getUint32(at + 24, true);
    const nameLen = view.getUint16(at + 28, true);
    const extraLen = view.getUint16(at + 30, true);
    const commentLen = view.getUint16(at + 32, true);
    const localAt = view.getUint32(at + 42, true);
    const name = dec.decode(bytes.subarray(at + 46, at + 46 + nameLen));
    at += 46 + nameLen + extraLen + commentLen;
    if (name.endsWith('/')) continue;
    if (flags & 1) throw new Error('zip-encrypted');
    if (view.getUint32(localAt, true) !== 0x04034B50) throw new Error('not-a-zip');
    const dataAt = localAt + 30 + view.getUint16(localAt + 26, true) + view.getUint16(localAt + 28, true);
    const body = bytes.subarray(dataAt, dataAt + packedSize);
    let data;
    if (method === 0) data = body.slice();
    else if (method === 8) data = await inflateRaw(body);
    else throw new Error('zip-method-' + method);
    if (data.length !== size || crc32(data) !== crc) throw new Error('zip-damaged');
    files.push({ name, data });
  }
  return files;
}
