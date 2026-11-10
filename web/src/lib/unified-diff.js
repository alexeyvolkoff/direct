// Minimal Myers diff producing unified-format text (git style, no color).
// Used by the Workspace review panel to diff in-browser editor changes
// against the file content originally loaded from the device.
// Output format matches `git diff` hunks, so both terminal and editor
// reviews render identically. Trailing-newline state is modelled with a
// sentinel so git's "\ No newline at end of file" markers are reproduced.

const NO_NL = "\0n";

function splitLines(text) {
  const s = String(text);
  if (s === "") return [];
  const parts = s.split("\n");
  if (parts[parts.length - 1] === "") {
    parts.pop();
  } else {
    parts[parts.length - 1] += NO_NL;
  }
  return parts;
}

function backtrack(trace, a, b, max) {
  const ops = [];
  let x = a.length;
  let y = b.length;
  for (let d = trace.length - 1; d >= 0; d--) {
    const v = trace[d];
    const k = x - y;
    const idx = k + max;
    let prevK;
    if (k === -d || (k !== d && v[idx - 1] < v[idx + 1])) {
      prevK = k + 1;
    } else {
      prevK = k - 1;
    }
    const prevX = v[prevK + max];
    const prevY = prevX - prevK;
    while (x > prevX && y > prevY) {
      ops.push({ t: '=', line: a[x - 1] });
      x--;
      y--;
    }
    if (d > 0) {
      if (x === prevX) {
        ops.push({ t: '+', line: b[y - 1] });
        y--;
      } else {
        ops.push({ t: '-', line: a[x - 1] });
        x--;
      }
    }
  }
  return ops.reverse();
}

function myersOps(a, b) {
  const N = a.length;
  const M = b.length;
  const max = N + M;
  const v = new Int32Array(2 * max + 1);
  const trace = [];
  // Bound trace memory: ~15M ints (~60MB) worst case.
  const maxD = Math.max(1, Math.min(2000, Math.floor(15000000 / (2 * max + 1))));
  for (let d = 0; d <= Math.min(max, maxD); d++) {
    trace.push(v.slice());
    for (let k = -d; k <= d; k += 2) {
      const idx = k + max;
      let x;
      if (k === -d || (k !== d && v[idx - 1] < v[idx + 1])) {
        x = v[idx + 1];
      } else {
        x = v[idx - 1] + 1;
      }
      let y = x - k;
      while (x < N && y < M && a[x] === b[y]) {
        x++;
        y++;
      }
      v[idx] = x;
      if (x >= N && y >= M) {
        return backtrack(trace, a, b, max);
      }
    }
  }
  // Too different: treat everything as changed.
  const ops = a.map(line => ({ t: '-', line }));
  for (const line of b) ops.push({ t: '+', line });
  return ops;
}

function buildHunks(ops, context) {
  const changeIdx = [];
  for (let i = 0; i < ops.length; i++) {
    if (ops[i].t !== '=') changeIdx.push(i);
  }
  if (!changeIdx.length) return [];
  const groups = [[changeIdx[0]]];
  for (let k = 1; k < changeIdx.length; k++) {
    if (changeIdx[k] - changeIdx[k - 1] - 1 <= 2 * context) {
      groups[groups.length - 1].push(changeIdx[k]);
    } else {
      groups.push([changeIdx[k]]);
    }
  }
  const hunks = [];
  for (const g of groups) {
    const start = Math.max(0, g[0] - context);
    const end = Math.min(ops.length - 1, g[g.length - 1] + context);
    let aBefore = 0;
    let bBefore = 0;
    for (let i = 0; i < start; i++) {
      if (ops[i].t !== '+') aBefore++;
      if (ops[i].t !== '-') bBefore++;
    }
    let aCount = 0;
    let bCount = 0;
    for (let i = start; i <= end; i++) {
      if (ops[i].t !== '+') aCount++;
      if (ops[i].t !== '-') bCount++;
    }
    hunks.push({
      start,
      end,
      aStart: aCount === 0 ? aBefore : aBefore + 1,
      bStart: bCount === 0 ? bBefore : bBefore + 1,
      aCount,
      bCount
    });
  }
  return hunks;
}

function fmtCount(start, count) {
  return count === 1 ? String(start) : `${start},${count}`;
}

/**
 * Produce a unified diff between two texts.
 * @param {string} aText original content
 * @param {string} bText modified content
 * @param {string} [name] file name used in the a/ b/ headers
 * @param {number} [context] context lines per hunk (git default 3)
 * @returns {string} unified diff text, or '' when there are no changes
 */
export function unifiedDiff(aText, bText, name = 'file', context = 3) {
  const a = splitLines(aText);
  const b = splitLines(bText);
  const ops = myersOps(a, b);
  const hunks = buildHunks(ops, context);
  if (!hunks.length) return '';

  let lastAIdx = -1;
  let lastBIdx = -1;
  for (let i = ops.length - 1; i >= 0; i--) {
    if (lastAIdx === -1 && ops[i].t !== '+') lastAIdx = i;
    if (lastBIdx === -1 && ops[i].t !== '-') lastBIdx = i;
    if (lastAIdx !== -1 && lastBIdx !== -1) break;
  }

  const out = [`--- a/${name}`, `+++ b/${name}`];
  for (const h of hunks) {
    out.push(`@@ -${fmtCount(h.aStart, h.aCount)} +${fmtCount(h.bStart, h.bCount)} @@`);
    for (let i = h.start; i <= h.end; i++) {
      const op = ops[i];
      const nl = op.line.endsWith(NO_NL);
      const clean = nl ? op.line.slice(0, -NO_NL.length) : op.line;
      out.push((op.t === '=' ? ' ' : op.t) + clean);
      const aMark = op.t !== '+' && i === lastAIdx && nl;
      const bMark = op.t !== '-' && i === lastBIdx && nl;
      if (aMark || bMark) {
        out.push("\\ No newline at end of file");
      }
    }
  }
  return out.join('\n') + '\n';
}
