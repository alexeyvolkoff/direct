#!/usr/bin/env python3
"""
Build and export the `direct`-tagged knowledge base from `docs/` in the repo tree.

Usage (run from `ai/` or anywhere):
  # 1. Index `../docs/` into active `memory_index/unified_index` with tag `direct`:
  python3 scripts/build_direct_kb.py --index-docs

  # 2. Export ONLY `direct`-tagged records into a standalone tar.gz for installers:
  python3 scripts/build_direct_kb.py --export-tar ../chroma_direct.tar.gz

  # 3. Build a clean standalone `chroma_direct.tar.gz` from `../docs/` in a temp dir
  #    without touching the running node's `memory_index/unified_index`:
  python3 scripts/build_direct_kb.py --standalone-tar ../chroma_direct.tar.gz
"""

import argparse
import os
import re
import shutil
import sys
import tarfile
import tempfile
from pathlib import Path

AI_DIR = Path(__file__).resolve().parent.parent
REPO_ROOT = AI_DIR.parent
DOCS_DIR = REPO_ROOT / "docs"

if str(AI_DIR) not in sys.path:
    sys.path.insert(0, str(AI_DIR))


def _strip_frontmatter(text: str) -> str:
    if text.startswith("---\n"):
        end = text.find("\n---\n", 4)
        if end != -1:
            return text[end + 5:].lstrip()
    return text


def _extract_title(text: str, fallback: str) -> str:
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("# "):
            return s[2:].strip()
        if s.lower().startswith("title:"):
            return s.split(":", 1)[1].strip().strip('"').strip("'")
    return fallback


def _collect_doc_files(docs_root: Path) -> list[Path]:
    files: list[Path] = []
    for p in sorted(docs_root.rglob("*")):
        if not p.is_file():
            continue
        if p.suffix.lower() not in (".md", ".markdown"):
            continue
        if any(part.startswith("_") for part in p.relative_to(docs_root).parts):
            continue
        files.append(p)
    return files


def index_docs_into_collection(unified_memory, docs_root: Path, tags: list[str] | None = None) -> int:
    tags = tags or ["direct", "docs"]
    files = _collect_doc_files(docs_root)
    total_chunks = 0
    for fpath in files:
        rel = fpath.relative_to(REPO_ROOT).as_posix()
        doc_id = "/" + rel
        raw = fpath.read_text(encoding="utf-8", errors="ignore")
        title = _extract_title(raw, fpath.stem.replace("_", " ").title())
        body = _strip_frontmatter(raw).strip()
        if not body:
            continue
        # Normalize markdown image links / excessive blank lines
        body = re.sub(r"\n{3,}", "\n\n", body)
        n_chunks = unified_memory.chunk_and_index_document(
            text=body,
            document_id=doc_id,
            owner="system",
            tags=tags,
            title=title,
            chunk_size=450,
            overlap=60,
        )
        if not n_chunks:
            continue
        total_chunks += n_chunks
        print(f"  indexed {doc_id}: {n_chunks} chunks ({title})")
    return total_chunks


def export_tag_snapshot(unified_memory, out_dir: Path, tag: str = "direct") -> int:
    import chromadb
    from chromadb.config import Settings

    src_coll = unified_memory.get_collection()
    res = src_coll.get(
        where={f"t_{tag.lower()}": {"$eq": 1}},
        include=["documents", "metadatas", "embeddings"],
    )
    ids = res.get("ids") or []
    docs = res.get("documents") or []
    metas = res.get("metadatas") or []
    raw_embs = res.get("embeddings")
    embs = raw_embs.tolist() if hasattr(raw_embs, "tolist") else (list(raw_embs) if raw_embs is not None else [])

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    dst_client = chromadb.PersistentClient(
        path=str(out_dir),
        settings=Settings(anonymized_telemetry=False),
    )
    dst_coll = dst_client.get_or_create_collection(
        name="omd_unified",
        metadata={"hnsw:space": "cosine"},
    )
    if ids:
        batch = 250
        for i in range(0, len(ids), batch):
            dst_coll.upsert(
                ids=ids[i:i + batch],
                documents=docs[i:i + batch],
                metadatas=metas[i:i + batch],
                embeddings=embs[i:i + batch],
            )
    return len(ids)


def _pack_dir_to_tar(src_dir: Path, tar_path: Path) -> None:
    tar_path.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tar_path, "w:gz") as tf:
        for item in sorted(src_dir.iterdir()):
            tf.add(item, arcname=item.name)


def main():
    parser = argparse.ArgumentParser(description="Build & export `direct` knowledge base from docs/")
    parser.add_argument("--index-docs", action="store_true", help="Index repo docs/ into active memory_index/unified_index with tag 'direct'")
    parser.add_argument("--export-tar", type=str, default="", help="Export only 'direct'-tagged records from active unified_index to tar.gz")
    parser.add_argument("--standalone-tar", type=str, default="", help="Build clean standalone chroma_direct.tar.gz from docs/ in a temp dir")
    args = parser.parse_args()

    if not (args.index_docs or args.export_tar or args.standalone_tar):
        parser.print_help()
        sys.exit(1)

    if args.standalone_tar:
        with tempfile.TemporaryDirectory(prefix="direct_kb_") as tmp:
            os.chdir(tmp)
            import unified_memory
            count = index_docs_into_collection(unified_memory, DOCS_DIR)
            snap_dir = Path(tmp) / "snapshot"
            exported = export_tag_snapshot(unified_memory, snap_dir, tag="direct")
            out_tar = Path(args.standalone_tar).resolve()
            _pack_dir_to_tar(snap_dir, out_tar)
            print(f"[build_direct_kb] Standalone snapshot: {count} chunks, {exported} exported records -> {out_tar}")
        return

    os.chdir(AI_DIR)
    import unified_memory
    if args.index_docs:
        count = index_docs_into_collection(unified_memory, DOCS_DIR)
        print(f"[build_direct_kb] Indexed {count} chunks from {DOCS_DIR} with tag 'direct'")

    if args.export_tar:
        with tempfile.TemporaryDirectory(prefix="direct_export_") as tmp:
            snap_dir = Path(tmp) / "snapshot"
            exported = export_tag_snapshot(unified_memory, snap_dir, tag="direct")
            out_tar = Path(args.export_tar).resolve()
            _pack_dir_to_tar(snap_dir, out_tar)
            print(f"[build_direct_kb] Exported {exported} 'direct' records -> {out_tar}")


if __name__ == "__main__":
    main()
