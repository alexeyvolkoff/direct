import { marked } from 'marked';
import { Document, Paragraph, TextRun, HeadingLevel, Packer } from 'docx';

/**
 * Supported office/document extensions that can be opened in Univer Docs.
 */
export const DOC_VIEWER_EXTS = ['docx', 'odt', 'rtf', 'epub', 'fb2', 'doc'];
export const SHEET_VIEWER_EXTS = ['xlsx'];

/**
 * Extensions edited by WordCraft (WASM, real round-trip of the original file).
 * epub/fb2 have no WordCraft support yet and stay on the Univer path;
 * xlsx stays on UniverSheetsViewer.
 */
export const WC_DOC_EXTS = ['docx', 'docm', 'dotx', 'dotm', 'doc', 'dot', 'odt', 'rtf'];

/**
 * Extensions edited by GridCraft (WASM spreadsheet, built-in ?host=parent bridge,
 * real XLSX round-trip). .xls/.ods import values only — they stay on Univer.
 */
export const GC_SHEET_EXTS = ['xlsx', 'xlsm', 'csv', 'tsv'];

/**
 * Extensions edited by DeckCraft (WASM presentations, the fork's deckcraft:*
 * parent bridge, real PPTX round trip).
 */
export const DC_PRES_EXTS = ['pptx', 'potx', 'ppsx'];

export function isOfficeDocument(filename = '', mime = '') {
  const ext = (filename.includes('.') ? filename.split('.').pop() : '').toLowerCase();
  const m = (mime || '').toLowerCase();
  if (DOC_VIEWER_EXTS.includes(ext) || SHEET_VIEWER_EXTS.includes(ext)) return true;
  if (m === 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet') return true;
  if (m === 'application/vnd.openxmlformats-officedocument.wordprocessingml.document') return true;
  if (m === 'application/vnd.oasis.opendocument.text') return true;
  if (m === 'application/rtf' || m === 'application/epub+zip') return true;
  return false;
}

/**
 * Removes control characters that the Univer docs parser treats as
 * structure tokens (tables, sections, etc.). Corruption of the dataStream
 * (e.g. binary content read as text) used to crash rendering with
 * "Cannot set properties of undefined (setting 'isFinished')".
 */
export function sanitizeDataStreamText(text = '') {
  return String(text).replace(/[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F]/g, '');
}

/**
 * Converts Markdown text into a valid Univer Docs IDocumentData snapshot.
 */
export function markdownToUniverDoc(md = '', filename = 'document.docx', docId = 'doc-1') {
  md = sanitizeDataStreamText(md);
  let dataStream = '';
  const paragraphs = [];
  const textRuns = [];

  const tokens = marked.lexer(md || '');

  function appendInline(inlineTokens, defaultStyle = {}) {
    if (!inlineTokens || !inlineTokens.length) return;
    for (const t of inlineTokens) {
      if (t.type === 'text') {
        const st = dataStream.length;
        dataStream += t.text;
        const ed = dataStream.length;
        if (Object.keys(defaultStyle).length > 0) {
          textRuns.push({ st, ed, ts: { ...defaultStyle } });
        }
      } else if (t.type === 'strong') {
        appendInline(t.tokens || [{ type: 'text', text: t.text }], { ...defaultStyle, bl: 1 });
      } else if (t.type === 'em') {
        appendInline(t.tokens || [{ type: 'text', text: t.text }], { ...defaultStyle, it: 1 });
      } else if (t.type === 'del') {
        appendInline(t.tokens || [{ type: 'text', text: t.text }], { ...defaultStyle, st: { s: 1 } });
      } else if (t.type === 'codespan') {
        const st = dataStream.length;
        dataStream += t.text;
        const ed = dataStream.length;
        textRuns.push({ st, ed, ts: { ...defaultStyle, ff: 'monospace', bg: { rgb: '#f0f0f0' } } });
      } else if (t.type === 'link') {
        appendInline(t.tokens || [{ type: 'text', text: t.text }], {
          ...defaultStyle,
          cl: { rgb: '#2c53f1' },
          ul: { s: 1 }
        });
      } else if (t.tokens) {
        appendInline(t.tokens, defaultStyle);
      } else if (t.text) {
        const st = dataStream.length;
        dataStream += t.text;
        const ed = dataStream.length;
        if (Object.keys(defaultStyle).length > 0) {
          textRuns.push({ st, ed, ts: { ...defaultStyle } });
        }
      }
    }
  }

  function addBlock(blockTokens, paraStyle = {}) {
    appendInline(blockTokens);
    dataStream += '\r';
    // Univer convention: paragraphs[].startIndex points at the terminating \r,
    // not at the paragraph start.
    paragraphs.push({
      startIndex: dataStream.length - 1,
      paragraphStyle: paraStyle
    });
  }

  for (const token of tokens) {
    if (token.type === 'heading') {
      const headingId = 'Heading ' + token.depth;
      const fsMap = { 1: 24, 2: 20, 3: 16, 4: 14, 5: 13, 6: 12 };
      addBlock(token.tokens || [{ type: 'text', text: token.text }], {
        headingId,
        textStyle: { bl: 1, fs: fsMap[token.depth] || 16 }
      });
    } else if (token.type === 'paragraph') {
      addBlock(token.tokens || [{ type: 'text', text: token.text }], {});
    } else if (token.type === 'list') {
      for (const item of token.items) {
        addBlock(item.tokens || [{ type: 'text', text: item.text }], {
          bullet: { listId: token.ordered ? 'ordered' : 'bullet', nestingLevel: 0 }
        });
      }
    } else if (token.type === 'code') {
      const lines = (token.text || '').split('\n');
      for (const line of lines) {
        dataStream += line + '\r';
        paragraphs.push({
          startIndex: dataStream.length - 1,
          paragraphStyle: { textStyle: { ff: 'monospace', fs: 11 } }
        });
      }
    } else if (token.type === 'blockquote') {
      for (const t of token.tokens || []) {
        if (t.tokens) {
          addBlock(t.tokens, { textStyle: { it: 1, cl: { rgb: '#666666' } } });
        } else if (t.text) {
          addBlock([{ type: 'text', text: t.text }], { textStyle: { it: 1, cl: { rgb: '#666666' } } });
        }
      }
    } else if (token.type === 'table') {
      if (token.header && token.header.length) {
        addBlock(token.header.map(h => ({ type: 'strong', text: h.text + '   ' })), { textStyle: { bl: 1 } });
      }
      for (const row of token.rows || []) {
        addBlock(row.map(c => ({ type: 'text', text: c.text + '   ' })), {});
      }
    }
  }

  if (!dataStream.length) {
    dataStream = '\r';
    paragraphs.push({ startIndex: 0 });
  }

  dataStream += '\n';
  const sectionBreaks = [{ startIndex: dataStream.length - 1 }];

  return {
    id: docId,
    title: filename,
    body: {
      dataStream,
      paragraphs,
      sectionBreaks,
      textRuns
    },
    documentStyle: {
      pageSize: { width: 595.28, height: 841.89 },
      marginTop: 72,
      marginBottom: 72,
      marginLeft: 72,
      marginRight: 72
    }
  };
}

/**
 * Converts Markdown into plain text blocks ready for the facade
 * insertion API (insertParagraph). This bypasses handcrafted
 * IDocumentData bodies, which render nothing on some Univer builds.
 */
export function markdownToUniverBlocks(md = '') {
  md = sanitizeDataStreamText(md);
  const tokens = marked.lexer(md || '');
  const blocks = [];

  function inlineTextOf(inlineTokens) {
    let out = '';
    for (const t of inlineTokens || []) {
      if (t.type === 'text') out += t.text;
      else if (t.text && !t.tokens) out += t.text;
      else if (t.tokens) out += inlineTextOf(t.tokens);
      else if (t.text) out += t.text;
    }
    return out;
  }

  function push(text, paragraphStyle = {}) {
    const clean = String(text || '').replace(/\r?\n/g, ' ');
    if (clean.length || paragraphStyle.bullet || paragraphStyle.headingId) {
      blocks.push({ text: clean, paragraphStyle });
    }
  }

  for (const token of tokens) {
    if (token.type === 'heading') {
      push(inlineTextOf(token.tokens), {
        headingId: 'Heading ' + token.depth,
        textStyle: { bl: 1, fs: { 1: 24, 2: 20, 3: 16, 4: 14, 5: 13, 6: 12 }[token.depth] || 16 }
      });
    } else if (token.type === 'paragraph') {
      push(inlineTextOf(token.tokens), {});
    } else if (token.type === 'list') {
      for (const item of token.items || []) {
        push((token.ordered ? '1. ' : '• ') + inlineTextOf(item.tokens), {
          bullet: { listId: token.ordered ? 'ordered' : 'bullet', nestingLevel: 0 }
        });
      }
    } else if (token.type === 'code') {
      for (const line of (token.text || '').split('\n')) {
        push(line, { textStyle: { ff: 'monospace', fs: 11 } });
      }
    } else if (token.type === 'blockquote') {
      for (const t of token.tokens || []) {
        push('> ' + inlineTextOf(t.tokens && t.tokens.length ? t.tokens : [{ type: 'text', text: t.text || '' }]), {});
      }
    } else if (token.type === 'table') {
      if (token.header?.length) push(token.header.map(h => h.text).join(' | '), { textStyle: { bl: 1 } });
      for (const row of token.rows || []) push(row.map(c => c.text).join(' | '), {});
    }
  }

  if (!blocks.length) push('');
  return blocks;
}

/**
 * Converts a Univer Docs snapshot to clean HTML5 markup.
 */
/**
 * True when the snapshot carries native tables or embedded-image markers,
 * i.e. features the docs canvas cannot reliably render in this build.
 */
/**
 * Pagination (visual page break) for the HTML preview: measures block heights
 * after paint and wraps blocks into A4 "sheet" divs, same look as the native
 * canvas gave. Needs snapshot.documentStyle for pt sizes.
 */
/** Render header/footer marker images (data URLs from snapshot notes). */
export function renderStripStrip(text, snapshot) {
  const images = (snapshot && snapshot.notes && snapshot.notes.omdImages) || [];
  const map = new Map();
  for (const img of images) map.set(img.name, img);
  let t = String(text || '');
  t = t.replace(/\[\[omd-img:([^|\]]+)(?:\|w=([0-9.]+))?(?:\|h=([0-9.]+))?\]\]/g, (m, name, wPt, hPt) => {
    const img = map.get(name);
    if (!img) return '';
    const PX = 96 / 72;
    const w = wPt && Number(wPt) > 0 ? 'width:' + Math.round(Number(wPt) * PX) + 'px;' : 'max-width:100%;';
    const h = hPt && Number(hPt) > 0 ? 'height:' + Math.round(Number(hPt) * PX) + 'px;' : 'height:auto;';
    return '<img src="data:' + img.mime + ';base64,' + img.data + '" style="' + w + h + '" alt="' + name + '"/>';
  });
  return t;
}

export function paginatePreviewSheets(container, snapshot) {
  if (!container || container.dataset.paginated === '1') return;
  container.dataset.paginated = '1';
  const ds = (snapshot && (snapshot.documentStyle || {})) || {};
  const pageW = (ds.pageSize && ds.pageSize.width) || 595.28;
  const pageH = (ds.pageSize && ds.pageSize.height) || 841.89;
  const PX = 96 / 72;
  const sheetW = Math.round(pageW * PX);
  const sheetH = Math.round(pageH * PX);
  const mT = Math.round((ds.marginTop ?? 72) * PX);
  const mB = Math.round((ds.marginBottom ?? 72) * PX);
  const mL = Math.round((ds.marginLeft ?? 72) * PX);
  const mR = Math.round((ds.marginRight ?? 72) * PX);
  const usable = Math.max(sheetH - mT - mB, 120);
  const hdrText = snapshot && snapshot.settings ? snapshot.settings.omdHeader || '' : '';
  const ftrText = snapshot && snapshot.settings ? snapshot.settings.omdFooter || '' : '';

  container.classList.add('omd-sheet-viewport');
  const nodes = Array.from(container.childNodes).filter(n => n.nodeType === 1 || (n.nodeType === 3 && n.textContent.trim()));
  if (!nodes.length) return;

  const first = nodes[0];
  let sheet = document.createElement('div');
  sheet.className = 'omd-sheet';
  container.insertBefore(sheet, nodes[0]);

  if (hdrText) { const hd = document.createElement('div'); hd.className = 'omd-doc-header'; hd.innerHTML = hdrText; sheet.appendChild(hd); }

  if (hdrText) { const hd = document.createElement('div'); hd.className = 'omd-doc-header'; hd.innerHTML = hdrText; sheet.appendChild(hd); }

  let acc = 0;
  for (const node of nodes) {
    const h = node.getBoundingClientRect && node.getBoundingClientRect().height || 0;
    if (acc > 0 && acc + h > usable) {
      const gap = document.createElement('div');
      gap.className = 'omd-sheet-gap';
      container.insertBefore(gap, node);
      const next = document.createElement('div');
      next.className = 'omd-sheet';
      container.insertBefore(next, gap.nextSibling);
      sheet = next;
      if (hdrText) { const hd = document.createElement('div'); hd.className = 'omd-doc-header'; hd.innerHTML = hdrText; sheet.appendChild(hd); }
      acc = 0;
    }
    // running header on the first sheet too

    sheet.appendChild(node);
    acc += h;
  }
  if (ftrText) { const ft = document.createElement('div'); ft.className = 'omd-doc-footer'; ft.innerHTML = ftrText; sheet.appendChild(ft); }
}

/**
 * Pipes the converter-provided omdHeader/omdFooter into the native Univer
 * docs headers/footers model so canvas documents show them too (HTML preview
 * path already draws the strips). Image markers are stripped for the canvas;
 * plain text only.
 */
export function applyDocumentHeadersFooters(snapshot) {
  if (!snapshot) return snapshot;
  const hdr = snapshot.settings && snapshot.settings.omdHeader;
  const ftr = snapshot.settings && snapshot.settings.omdFooter;
  const strip = (t) => String(t || '').replace(/\[\[omd-img:[^\]]+\]\]/g, '').replace(/\s+/g, ' ').trim();
  snapshot.documentStyle = snapshot.documentStyle || {};
  snapshot.body = snapshot.body || {};
  if (hdr) {
    const ht = strip(hdr);
    if (ht) {
      const st = ht + '\r\n';
      const id = 'omd-hdr-1';
      snapshot.body.headers = snapshot.body.headers || {};
      snapshot.body.headers[id] = {
        headerId: id,
        body: {
          dataStream: st,
          textRuns: [],
          paragraphs: [{ startIndex: st.length - 2, paragraphStyle: {} }],
          sectionBreaks: [{ startIndex: st.length - 1 }],
          tables: [],
          customRanges: []
        }
      };
      snapshot.documentStyle.defaultHeaderId = id;
      snapshot.documentStyle.marginHeader = snapshot.documentStyle.marginHeader || 30;
    }
  }
  if (ftr) {
    const ft = strip(ftr);
    if (ft) {
      const st = ft + '\r\n';
      const id = 'omd-ftr-1';
      snapshot.body.footers = snapshot.body.footers || {};
      snapshot.body.footers[id] = {
        footerId: id,
        body: {
          dataStream: st,
          textRuns: [],
          paragraphs: [{ startIndex: Math.max(st.length - 2, 0), paragraphStyle: {} }],
          sectionBreaks: [{ startIndex: st.length - 1 }],
          tables: [],
          customRanges: []
        }
      };
      snapshot.documentStyle.defaultFooterId = id;
      snapshot.documentStyle.marginFooter = snapshot.documentStyle.marginFooter || 30;
    }
  }
  return snapshot;
}

export function docNeedsHtmlPreview(snapshot) {
  const body = snapshot && snapshot.body;
  if (!body) return false;
  if (body.tables && Object.keys(body.tables).length) return true;
  if (snapshot.notes && snapshot.notes.omdImages && snapshot.notes.omdImages.length) return true;
  const stream = body.dataStream || '';
  if (stream.includes('[[omd-img:')) return true;
  return /[]/.test(stream);
}

export function univerDocToHtml(snapshot) {
  if (!snapshot || !snapshot.body) return '<p></p>';
  const body = snapshot.body;
  const { dataStream = '', paragraphs = [], textRuns = [] } = body;
  if (!dataStream) return '<p></p>';

  // Embedded images (converter writes [[omd-img:<name>]] markers alongside a
  // notes.omdImages registry); unknown markers render as nothing.
  const imgMap = new Map();
  const omdImages = (snapshot.notes && snapshot.notes.omdImages) || [];
  for (const img of omdImages) imgMap.set(img.name, img);
  const imageMarker = (name, wPt, hPt, anchor, wrap, pos) => {
    const img = imgMap.get(name);
    if (!img) return '';
    const PX = 96 / 72; // pt -> css px
    let style = 'max-width:100%;height:auto;';
    if (wPt && Number(wPt) > 0) {
      const h = hPt && Number(hPt) > 0 ? Math.round(Number(hPt) * PX) + 'px' : 'auto';
      style = 'width:' + Math.round(Number(wPt) * PX) + 'px;max-width:100%;height:' + h + ';';
    }
    // Layout: as-char/char stay inline; paragraph/page anchors float or center
    // per the odt graphic wrap/horizontal-pos (what the canvas can't do).
    if (anchor && anchor !== 'char' && anchor !== 'as-char') {
      style = style + 'display:block;';
      if (pos === 'center') style += 'margin:10px auto;';
      else if (wrap === 'left') { style += 'float:right;margin:8px 0 10px 16px;'; }
      else { style += 'float:left;margin:8px 16px 10px 0;'; }
    }
    return '<img src="data:' + img.mime + ';base64,' + img.data + '" alt="' + escapeHtml(name) + '" style="' + style + '"/>';
  };

  // Style-aware inline renderer for stream slice [st, ed). textRuns hold
  // absolute stream offsets, so slicing a paragraph or a table cell keeps
  // bold/italic/color styling wired to the right characters.
  const runs = textRuns || [];
  const renderSlice = (st, ed) => {
    let html = '';
    let i = st;
    while (i < ed) {
      const active = runs.find(r => i >= r.st && i < r.ed) || null;
      if (!active) {
        const next = runs.filter(r => r.st > i && r.st < ed).sort((a, b) => a.st - b.st);
        const upto = next.length ? Math.min(next[0].st, ed) : ed;
        html += escapeHtml(dataStream.slice(i, upto));
        i = upto;
      } else {
        const upto = Math.min(active.ed, ed);
        let seg = escapeHtml(dataStream.slice(i, upto));
        const ts = active.ts || {};
        if (ts.bl) seg = '<strong>' + seg + '</strong>';
        if (ts.it) seg = '<em>' + seg + '</em>';
        if (ts.st && ts.st.s) seg = '<del>' + seg + '</del>';
        if (ts.ul && ts.ul.s) seg = '<u>' + seg + '</u>';
        if (ts.cl && ts.cl.rgb) seg = '<span style="color:' + ts.cl.rgb + '">' + seg + '</span>';
        html += seg;
        i = upto;
      }
    }
    return html.replace(/\[\[omd-img:([^\]]+)\]\]/g, (m, body) => {
        const [name, ...opts] = body.split('|');
        const opt = {};
        for (const p of opts) { const eq = p.indexOf('='); if (eq > 0) opt[p.slice(0, eq)] = p.slice(eq + 1); }
        return imageMarker(name, opt.w, opt.h, opt.anchor, opt.wrap, opt.pos);
      });
  };

  const paraByIndex = new Map();
  for (const p of paragraphs || []) paraByIndex.set(p.startIndex, p);

  const ST = '\u001A', RS = '\u001B', CS = '\u001C', CE = '\u001D', RE = '\u000E', TE = '\u000F';
  const hasMarkers = /[\u001A\u001B\u001C\u001D\u000E\u000F]/.test(dataStream);

  // Structure pass: split the stream into paragraph blocks and (marker-span)
  // table blocks; absolute stream offsets are preserved so run styles map.
  const blocks = [];
  {
    let textStart = 0;
    let k = 0;
    while (k < dataStream.length) {
      const ch = dataStream[k];
      if (ch === ST) {
        const end = dataStream.indexOf(TE, k + 1);
        const maxEnd = end === -1 ? dataStream.length : end + 1;
        const rawTable = dataStream.slice(k, maxEnd);
        if (textStart < k) blocks.push({ kind: 'para', start: textStart, crIdx: k - 1 });
        const rows = [];
        let curRow = null;
        let curCell = null;
        for (let j = 0; j < rawTable.length; j++) {
          const c = rawTable[j];
          if (c === RS) { curRow = []; rows.push(curRow); }
          else if (c === CS) { curCell = { start: k + j + 1 }; }
          else if (c === CE) {
            if (curRow && curCell) { curCell.end = k + j; curRow.push(curCell); }
            curCell = null;
          }
          // 0x0E row end implicit; 0x0F table end handled by slice bounds
        }
        blocks.push({ kind: 'table', rows });
        k = maxEnd;
        textStart = k;
        continue;
      }
      if (ch === '\r') {
        blocks.push({ kind: 'para', start: textStart, crIdx: k });
        textStart = k + 1;
      }
      k++;
    }
  }

  const out = [];
  for (const b of blocks) {
    if (b.kind === 'table') {
      const rowsHtml = (b.rows || []).map(cells => {
        const tds = (cells || []).map(c => {
          let txt = renderSlice(c.start, c.end);
          txt = txt.replace(/\s+/g, ' ').trim();
          return '<td>' + (txt || '&nbsp;') + '</td>';
        }).join('');
        return '<tr>' + tds + '</tr>';
      }).join('');
      out.push('<table class="omd-doc-table"><tbody>' + rowsHtml + '</tbody></table>');
      continue;
    }
    const s0 = b.start, cr = b.crIdx;
    if (s0 >= cr && s0 !== 0) { /* empty paragraph after a table */ }
    let content = cr > s0 ? renderSlice(s0, cr) : '';
    if (imgMap.size && content.includes('[[')) {
      content = content.replace(/\[\[omd-img:([^\]]+)\]\]/g, (m, body) => {
        const [name, ...opts] = body.split('|');
        const opt = {};
        for (const p of opts) { const eq = p.indexOf('='); if (eq > 0) opt[p.slice(0, eq)] = p.slice(eq + 1); }
        return imageMarker(name, opt.w, opt.h, opt.anchor, opt.wrap, opt.pos);
      });
    }
    const meta = paraByIndex.get(cr) || {};
    const ps = meta.paragraphStyle || {};
    let tag = 'p';
    const alignCls = ps.horizontalAlign === 2 ? ' omd-align-center' : ps.horizontalAlign === 3 ? ' omd-align-right' : ps.horizontalAlign === 4 ? ' omd-align-justify' : '';
    const hid = ps.headingId || '';
    if (hid.startsWith('Heading ')) {
      const level = parseInt(hid.replace('Heading ', ''), 10) || 1;
      tag = 'h' + Math.min(Math.max(level, 1), 6);
    }
    if (!content) content = '<br/>';
    out.push('<' + tag + ' class="' + (alignCls ? alignCls.trim() : '') + '">' + content + '</' + tag + '>');
  }
  return out.filter(Boolean).join('\n') || '<p></p>';
}


const HEADING_SPACING_PT = {
  1: { above: 18, below: 8 },
  2: { above: 16, below: 6 },
  3: { above: 14, below: 6 },
  4: { above: 10, below: 4 },
  5: { above: 10, below: 4 },
  6: { above: 10, below: 4 }
};

export function applyDefaultParagraphSpacing(snapshot) {
  const paras = snapshot?.body?.paragraphs;
  if (Array.isArray(paras)) {
    for (const p of paras) {
      const ps = p.paragraphStyle || (p.paragraphStyle = {});
      if (ps.spaceAbove != null || ps.spaceBelow != null) continue;
      const lvl = parseInt(String(ps.headingId || ''), 10) ||
        (ps.headingId ? parseInt(String(ps.headingId).replace('Heading ', ''), 10) || 0 : 0);
      const h = HEADING_SPACING_PT[lvl];
      if (h) {
        ps.spaceAbove = { v: h.above };
        ps.spaceBelow = { v: h.below };
      } else if (ps.bullet && ps.bullet.listId) {
        ps.spaceAbove = { v: 0 };
        ps.spaceBelow = { v: 4 };
      } else {
        ps.spaceAbove = { v: 0 };
        ps.spaceBelow = { v: 8 };
      }
    }
  }
  return snapshot;
}

export function univerDocToMarkdown(snapshot) {
  if (!snapshot || !snapshot.body) return '';
  const body = snapshot.body;
  const { dataStream = '', paragraphs = [], textRuns = [] } = body;

  const cleanStream = dataStream.replace(/\n$/, '');
  const rawParas = cleanStream.split('\r');

  let currentOffset = 0;
  const mdParts = [];

  for (let i = 0; i < rawParas.length; i++) {
    const pText = rawParas[i];
    const pStart = currentOffset;
    const pEnd = currentOffset + pText.length;
    currentOffset = pEnd + 1;

    if (!pText && i === rawParas.length - 1) continue;

    const paraMeta = (paragraphs || []).find(p => p.startIndex === pEnd) || {};
    const paraStyle = paraMeta.paragraphStyle || {};
    const headingId = paraStyle.headingId || '';

    let prefix = '';
    if (headingId.startsWith('Heading ')) {
      const level = parseInt(headingId.replace('Heading ', ''), 10) || 1;
      prefix = '#'.repeat(level) + ' ';
    } else if (paraStyle.bullet?.listId) {
      prefix = paraStyle.bullet.listId === 'ordered' ? '1. ' : '- ';
    }

    const pRuns = textRuns.filter(r => r.st < pEnd && r.ed > pStart);
    let pContent = '';

    if (pRuns.length === 0) {
      pContent = pText;
    } else {
      let charIdx = 0;
      while (charIdx < pText.length) {
        const globalIdx = pStart + charIdx;
        const activeRun = pRuns.find(r => globalIdx >= r.st && globalIdx < r.ed);

        if (!activeRun) {
          const nextRuns = pRuns.filter(r => r.st > globalIdx).sort((a, b) => a.st - b.st);
          const nextStart = nextRuns.length > 0 ? Math.min(nextRuns[0].st - pStart, pText.length) : pText.length;
          pContent += pText.slice(charIdx, nextStart);
          charIdx = nextStart;
        } else {
          const runEndInP = Math.min(activeRun.ed - pStart, pText.length);
          let seg = pText.slice(charIdx, runEndInP);
          const ts = activeRun.ts || {};
          if (ts.bl) seg = `**${seg}**`;
          if (ts.it) seg = `*${seg}*`;
          if (ts.st?.s) seg = `~~${seg}~~`;
          pContent += seg;
          charIdx = runEndInP;
        }
      }
    }

    mdParts.push(prefix + pContent.trim());
  }

  // ODT content is tab-indented: `marked` renders 4-space/tab indented lines as
  // code blocks and cannot parse `**\tBold**` runs; nbsp keeps the visual gap.
  return mdParts
    .join('\n\n')
    .replace(/\t/g, '\u00a0\u00a0')
    .replace(/\*\*([^*]+)\*\*/g, (_, c) => '**' + c.replace(/\u00a0/g, ' ').trim() + '**');
}

/**
 * Converts a Univer Docs snapshot directly to a .docx Blob using the browser-compatible 'docx' library.
 */
export async function univerDocToDocxBlob(snapshot, filename = 'document.docx') {
  const body = snapshot?.body;
  const { dataStream = '', paragraphs = [], textRuns = [] } = body || {};

  const cleanStream = (dataStream || '').replace(/\n$/, '');
  const rawParas = cleanStream.split('\r');

  let currentOffset = 0;
  const docxParas = [];

  for (let i = 0; i < rawParas.length; i++) {
    const pText = rawParas[i];
    const pStart = currentOffset;
    const pEnd = currentOffset + pText.length;
    currentOffset = pEnd + 1;

    if (!pText && i === rawParas.length - 1) continue;

    const paraMeta = (paragraphs || []).find(p => p.startIndex === pEnd) || {};
    const paraStyle = paraMeta.paragraphStyle || {};
    const headingId = paraStyle.headingId || '';

    let heading = undefined;
    if (headingId.startsWith('Heading ')) {
      const level = parseInt(headingId.replace('Heading ', ''), 10);
      if (level === 1) heading = HeadingLevel.HEADING_1;
      else if (level === 2) heading = HeadingLevel.HEADING_2;
      else if (level === 3) heading = HeadingLevel.HEADING_3;
      else if (level === 4) heading = HeadingLevel.HEADING_4;
      else if (level === 5) heading = HeadingLevel.HEADING_5;
      else if (level === 6) heading = HeadingLevel.HEADING_6;
    }

    const pRuns = (textRuns || []).filter(r => r.st < pEnd && r.ed > pStart);
    const docxRuns = [];

    if (pRuns.length === 0) {
      docxRuns.push(new TextRun(pText || ' '));
    } else {
      let charIdx = 0;
      while (charIdx < pText.length) {
        const globalIdx = pStart + charIdx;
        const activeRun = pRuns.find(r => globalIdx >= r.st && globalIdx < r.ed);

        if (!activeRun) {
          const nextRuns = pRuns.filter(r => r.st > globalIdx).sort((a, b) => a.st - b.st);
          const nextStart = nextRuns.length > 0 ? Math.min(nextRuns[0].st - pStart, pText.length) : pText.length;
          docxRuns.push(new TextRun(pText.slice(charIdx, nextStart)));
          charIdx = nextStart;
        } else {
          const runEndInP = Math.min(activeRun.ed - pStart, pText.length);
          const segText = pText.slice(charIdx, runEndInP);
          const ts = activeRun.ts || {};

          docxRuns.push(new TextRun({
            text: segText,
            bold: !!ts.bl,
            italics: !!ts.it,
            strike: !!ts.st?.s,
            underline: ts.ul?.s ? {} : undefined,
            color: ts.cl?.rgb ? ts.cl.rgb.replace('#', '') : undefined
          }));
          charIdx = runEndInP;
        }
      }
    }

    docxParas.push(new Paragraph({
      heading,
      children: docxRuns.length ? docxRuns : [new TextRun(' ')]
    }));
  }

  const doc = new Document({
    title: filename,
    sections: [{
      properties: {},
      children: docxParas.length ? docxParas : [new Paragraph({ children: [new TextRun('')] })]
    }]
  });

  return await Packer.toBlob(doc);
}

function escapeHtml(str = '') {
  return str
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;');
}

/**
 * Extracts text (markdown-ish) from an office document blob entirely in
 * the browser. Used when the server-side pandoc ?totext conversion is
 * unavailable (e.g. files served over the P2P bridge, where Open/Read
 * RPC ignores the query). Works for zip-based formats: docx, odt, epub.
 */
export async function extractDocTextFromBlob(blob, filename = '') {
  const { default: JSZip } = await import('jszip');
  const zip = await JSZip.loadAsync(await blob.arrayBuffer());

  const docXml = await zip.file('word/document.xml')?.async('string');
  if (docXml) return docxXmlToMarkdown(docXml);

  const odtXml = await zip.file('content.xml')?.async('string');
  if (odtXml) return odtXmlToMarkdown(odtXml);

  return null;
}

function docxXmlToMarkdown(xml) {
  const parser = new DOMParser();
  const doc = parser.parseFromString(xml, 'application/xml');
  const body = doc.getElementsByTagName('w:body')[0];
  if (!body) return null;

  const parts = [];
  for (const p of body.childNodes) {
    if (p.nodeName !== 'w:p' && p.nodeName !== 'w:tbl') continue;

    if (p.nodeName === 'w:tbl') {
      parts.push(tblToMarkdown(p));
      continue;
    }

    let prefix = '';
    const styleEl = p.querySelector('w\\:pStyle, pStyle');
    const style = styleEl ? (styleEl.getAttribute('w:val') || '') : '';
    const headingMatch = style.match(/^Heading(\d)/i);
    const isList = p.querySelector('w\\:numPr, numPr');
    if (headingMatch) prefix = '#'.repeat(parseInt(headingMatch[1], 10)) + ' ';
    else if ((style || '').toLowerCase() === 'title') prefix = '# ';
    else if (isList) prefix = '- ';

    const text = Array.from(p.getElementsByTagName('w:t'))
      .map(t => t.textContent)
      .join('');
    if (!text.trim() && !prefix) {
      parts.push('');
      continue;
    }
    parts.push(prefix + text);
  }
  return sanitizeDataStreamText(parts.join('\r\n\r\n'));
}

function odtXmlToMarkdown(xml) {
  const parser = new DOMParser();
  const doc = parser.parseFromString(xml, 'text/xml');
  const body = doc.getElementsByTagName('office:body')[0];
  if (!body) return null;

  const parts = [];
  const walk = (node, blockPrefix) => {
    for (const child of childElements(node)) {
      const n = child.nodeName;
      if (n === 'text:h') {
        const level = parseInt(child.getAttribute('text:outline-level') || '1', 10) || 1;
        const inline = inlineText(child);
        parts.push(blockPrefix + '#'.repeat(Math.min(level, 6)) + ' ' + inline);
      } else if (n === 'text:p') {
        const inline = inlineText(child);
        parts.push(blockPrefix + inline);
      } else if (n === 'text:list' || n === 'text:list-item') {
        walk(child, blockPrefix);
        continue;
      } else {
        walk(child, blockPrefix);
        continue;
      }
    }
  };
  walk(body, '');
  return sanitizeDataStreamText(parts.join('\r\n\r\n'));
}

function childElements(node) {
  return Array.from(node.childNodes).filter(n => n.nodeType === 1);
}

function inlineText(node) {
  let text = '';
  for (const n of node.childNodes) {
    if (n.nodeType === 1) {
      const name = n.nodeName;
      if (name === 'text:s') {
        text += ' '.repeat(parseInt(n.getAttribute('text:c') || '1', 10) || 1);
      } else if (name === 'text:tab') {
        text += '\t';
      } else if (name === 'text:line-break') {
        text += '\r';
      } else {
        text += inlineText(n);
      }
    } else if (n.nodeType === 3) {
      text += n.nodeValue || '';
    }
  }
  return text;
}

function tblToMarkdown(tblEl) {
  const rows = [];
  for (const tr of tblEl.getElementsByTagName('w:tr')) {
    const cells = Array.from(tr.getElementsByTagName('w:tc'))
      .map(tc => Array.from(tc.getElementsByTagName('w:t')).map(t => t.textContent).join(' ').trim());
    rows.push('| ' + cells.join(' | ') + ' |');
  }
  return rows.join('\r\n');
}

/**
 * Determines whether a fetch result of a ?totext request returned the raw
 * binary document instead of converted text (e.g. over P2P where Open/Read
 * ignores the query) — such body must not be rendered as plain text.
 */
export function isBinaryResponseType(contentType) {
  const t = (contentType || '').toLowerCase();
  if (!t) return false;
  return /zip|octet-stream|vnd\.openxml|vnd\.oasis|epub|msword/.test(t);
}
