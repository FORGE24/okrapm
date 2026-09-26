#!/usr/bin/env python3
"""OkraPM / Lunar HTTP software source.

Serves index.yaml and .oaa artifacts so a guest can run:

    lunar repo add okra http://10.0.2.2:8765
    lunar sync okra
    lunar install GNU.gcc
"""

from __future__ import annotations

import argparse
import tarfile
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Iterable


def extract_meta(archive: Path) -> str:
    with tarfile.open(archive, "r:*") as tf:
        for name in ("meta.yaml", "./meta.yaml"):
            try:
                member = tf.getmember(name)
            except KeyError:
                continue
            data = tf.extractfile(member)
            if data is None:
                continue
            return data.read().decode("utf-8", errors="replace")
        for member in tf.getmembers():
            if member.name.endswith("meta.yaml") and member.isfile():
                data = tf.extractfile(member)
                if data is None:
                    continue
                return data.read().decode("utf-8", errors="replace")
    raise FileNotFoundError(f"no meta.yaml in {archive}")


def iter_artifacts(repo_root: Path) -> Iterable[Path]:
    artifacts = repo_root / "artifacts"
    if not artifacts.is_dir():
        return
    for path in sorted(artifacts.iterdir()):
        if path.suffix in {".oaa", ".okra"} and path.is_file():
            yield path


def build_index(repo_root: Path) -> str:
    chunks: list[str] = []
    for archive in iter_artifacts(repo_root):
        try:
            meta = extract_meta(archive).strip()
        except Exception as exc:
            chunks.append(f"# skipped {archive.name}: {exc}")
            continue
        if not meta:
            continue
        chunks.append(meta)
    return "\n---\n".join(chunks) + ("\n" if chunks else "")


def write_index(repo_root: Path) -> Path:
    index = repo_root / "index.yaml"
    index.write_text(build_index(repo_root), encoding="utf-8")
    return index


class RepoHandler(SimpleHTTPRequestHandler):
    repo_root: Path

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(self.repo_root), **kwargs)

    def log_message(self, fmt: str, *args) -> None:
        print("%s - %s" % (self.address_string(), fmt % args), flush=True)

    def do_GET(self) -> None:
        if self.path in ("/", "/index.html"):
            write_index(self.repo_root)
            body = self.listing_page().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.split("?", 1)[0] in ("/index.yaml", "/packages.idx"):
            data = write_index(self.repo_root).read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        super().do_GET()

    def listing_page(self) -> str:
        rows = []
        for archive in iter_artifacts(self.repo_root):
            size = archive.stat().st_size
            rows.append(
                f"<tr><td><code>{archive.name}</code></td>"
                f"<td>{size}</td>"
                f"<td><a href='/artifacts/{archive.name}'>download</a></td></tr>"
            )
        table = "\n".join(rows) or "<tr><td colspan='3'>no artifacts</td></tr>"
        return f"""<!doctype html>
<html><head><meta charset="utf-8"><title>OkraPM repo</title></head>
<body>
<h1>OkraPM software source</h1>
<p>Add this repo, then install GCC:</p>
<pre>lunar repo add okra {self.server_url()}
lunar sync okra
lunar install GNU.gcc</pre>
<p>Index: <a href="/index.yaml">/index.yaml</a></p>
<table border="1" cellpadding="6">
<tr><th>artifact</th><th>bytes</th><th></th></tr>
{table}
</table>
</body></html>
"""

    def server_url(self) -> str:
        host, port = self.server.server_address[:2]
        if host in ("0.0.0.0", ""):
            host = "10.0.2.2"
        return f"http://{host}:{port}"


def main() -> None:
    parser = argparse.ArgumentParser(description="OkraPM / Lunar software source")
    parser.add_argument(
        "--root",
        default=str(Path(__file__).resolve().parent.parent / "repo"),
        help="repository directory containing artifacts/",
    )
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    repo_root = Path(args.root).resolve()
    (repo_root / "artifacts").mkdir(parents=True, exist_ok=True)
    write_index(repo_root)
    RepoHandler.repo_root = repo_root

    httpd = ThreadingHTTPServer((args.bind, args.port), RepoHandler)
    print(f"OkraPM repo at http://{args.bind}:{args.port}/")
    print(f"root: {repo_root}")
    print("lunar repo add okra http://10.0.2.2:%s" % args.port)
    print("lunar install GNU.gcc")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
