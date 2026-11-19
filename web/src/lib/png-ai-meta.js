/**
 * Injects standard IPTC/EXIF-compatible PNG tEXt chunks for EU AI Act Art. 50
 * synthetic media disclosure (`DigitalSourceType = trainedAlgorithmicMedia`).
 */

const PNG_SIG = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]);

let crcTable = null;
function getCrcTable() {
  if (crcTable) return crcTable;
  crcTable = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    crcTable[n] = c >>> 0;
  }
  return crcTable;
}

function crc32(bytes) {
  const table = getCrcTable();
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    c = table[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function makeTextChunk(keyword, value) {
  const enc = new TextEncoder();
  const keyBytes = enc.encode(keyword);
  const valBytes = enc.encode(value);
  const dataLen = keyBytes.length + 1 + valBytes.length;
  const chunk = new Uint8Array(12 + dataLen);
  const view = new DataView(chunk.buffer);

  view.setUint32(0, dataLen, false);
  chunk.set([0x74, 0x45, 0x58, 0x74], 4); // "tEXt"
  chunk.set(keyBytes, 8);
  chunk[8 + keyBytes.length] = 0; // null separator
  chunk.set(valBytes, 9 + keyBytes.length);

  const crc = crc32(chunk.subarray(4, 8 + dataLen));
  view.setUint32(8 + dataLen, crc, false);
  return chunk;
}

function isPng(bytes) {
  if (bytes.length < 24) return false;
  for (let i = 0; i < PNG_SIG.length; i++) {
    if (bytes[i] !== PNG_SIG[i]) return false;
  }
  return true;
}

export async function injectPngAiMetadata(blob) {
  if (!blob || blob.size < 24) return blob;
  const buf = await blob.arrayBuffer();
  const bytes = new Uint8Array(buf);
  if (!isPng(bytes)) return blob;

  // Insert right after IHDR (signature 8 bytes + IHDR chunk length 4 + type 4 + 13 + crc 4 = 33 bytes)
  const insertAt = 33;
  if (bytes.length <= insertAt) return blob;

  const sourceChunk = makeTextChunk(
    "DigitalSourceType",
    "http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia"
  );
  const softwareChunk = makeTextChunk("Software", "Direct AI Studio");

  const out = new Uint8Array(bytes.length + sourceChunk.length + softwareChunk.length);
  out.set(bytes.subarray(0, insertAt), 0);
  out.set(sourceChunk, insertAt);
  out.set(softwareChunk, insertAt + sourceChunk.length);
  out.set(bytes.subarray(insertAt), insertAt + sourceChunk.length + softwareChunk.length);

  return new Blob([out], { type: "image/png" });
}
