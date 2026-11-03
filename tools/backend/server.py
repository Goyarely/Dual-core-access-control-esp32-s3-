#!/usr/bin/env python3
"""双核门禁 —— 通行记录后端（零依赖，只用 Python 标准库）

    python3 tools/backend/server.py --port 8080

ESP32 每次认证完成后 POST 一条 JSON 到 /api/record，
浏览器打开 http://<本机IP>:8080/ 就能看到记录、按成员/结果筛选。

接口：
    POST /api/record   {"ts":1729,"member":"Person_1","result":"OK","reason":"face","sim":0.87}
    GET  /api/records?limit=100&member=..&result=..
    GET  /             网页（自动刷新）
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

DB_PATH = Path(__file__).with_name("records.db")

RESULT_TEXT = {"OK": "通行", "DENIED": "拒绝", "REPLAY": "重放", "LOCKED": "锁定中"}
REASON_TEXT = {"face": "刷脸", "password": "密码", "admin": "管理员"}


# ---------------------------------------------------------------- 数据库
def db_init() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        """CREATE TABLE IF NOT EXISTS records (
               id      INTEGER PRIMARY KEY AUTOINCREMENT,
               ts      INTEGER NOT NULL,      -- 设备侧时间戳（秒）
               member  TEXT,
               result  TEXT,
               reason  TEXT,
               sim     REAL,
               at      TEXT NOT NULL          -- 服务器收到的时间
           )"""
    )
    conn.commit()
    return conn


def db_insert(rec: dict) -> None:
    conn = db_init()
    conn.execute(
        "INSERT INTO records (ts, member, result, reason, sim, at) VALUES (?,?,?,?,?,?)",
        (
            int(rec.get("ts", 0)),
            str(rec.get("member", "")),
            str(rec.get("result", "")),
            str(rec.get("reason", "")),
            float(rec.get("sim", 0)),
            datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        ),
    )
    conn.commit()
    conn.close()


def db_query(limit: int = 100, member: str | None = None, result: str | None = None):
    conn = db_init()
    sql = "SELECT ts, member, result, reason, sim, at FROM records"
    where, args = [], []
    if member:
        where.append("member = ?")
        args.append(member)
    if result:
        where.append("result = ?")
        args.append(result)
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY id DESC LIMIT ?"
    args.append(limit)
    rows = conn.execute(sql, args).fetchall()
    stat = conn.execute(
        "SELECT result, COUNT(*) FROM records GROUP BY result"
    ).fetchall()
    conn.close()
    return rows, stat


# ---------------------------------------------------------------- 页面
def render_page(rows, stat, member_filter, result_filter) -> str:
    def esc(s):
        return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

    stats = "".join(
        f'<span class="chip">{esc(RESULT_TEXT.get(r, r))} {n}</span>' for r, n in stat
    ) or '<span class="chip">暂无记录</span>'

    lines = []
    for ts, member, result, reason, sim, at in rows:
        cls = "ok" if result == "OK" else "no"
        lines.append(
            f"<tr><td>{esc(at)}</td><td>{esc(member or '-')}</td>"
            f'<td class="{cls}">{esc(RESULT_TEXT.get(result, result))}</td>'
            f"<td>{esc(REASON_TEXT.get(reason, reason))}</td>"
            f"<td>{sim:.2f}</td></tr>"
        )
    body = "".join(lines) or '<tr><td colspan="5">还没有记录</td></tr>'

    return f"""<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8">
<meta http-equiv="refresh" content="5">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>双核门禁 · 通行记录</title>
<style>
 body{{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;margin:0;background:#0e1420;color:#e6edf3}}
 .wrap{{max-width:900px;margin:0 auto;padding:24px}}
 h1{{font-size:20px;margin:0 0 4px}}
 .sub{{color:#7d8ea0;font-size:13px;margin-bottom:16px}}
 .chip{{display:inline-block;background:#1b2634;border:1px solid #2b3a4a;border-radius:12px;
        padding:4px 10px;margin-right:8px;font-size:13px}}
 table{{width:100%;border-collapse:collapse;margin-top:16px;font-size:14px}}
 th,td{{text-align:left;padding:10px 8px;border-bottom:1px solid #1e2a38}}
 th{{color:#7d8ea0;font-weight:500;font-size:13px}}
 .ok{{color:#3ddc78}} .no{{color:#ff8a80}}
 form{{margin:16px 0}}
 input,select,button{{background:#16202c;color:#e6edf3;border:1px solid #2b3a4a;
        border-radius:8px;padding:8px 10px;font-size:14px}}
 button{{background:#185fa5;border:none;cursor:pointer}}
</style></head><body><div class="wrap">
<h1>双核门禁 · 通行记录</h1>
<div class="sub">每 5 秒自动刷新 · 数据来源 ESP32 上报</div>
<div>{stats}</div>
<form method="get">
 <input name="member" placeholder="成员（如 Person_1）" value="{esc(member_filter or '')}">
 <select name="result">
  <option value="">全部结果</option>
  {''.join(f'<option value="{k}" {"selected" if result_filter==k else ""}>{v}</option>'
           for k, v in RESULT_TEXT.items())}
 </select>
 <button type="submit">筛选</button>
</form>
<table>
<tr><th>时间</th><th>成员</th><th>结果</th><th>方式</th><th>相似度</th></tr>
{body}
</table>
</div></body></html>"""


# ---------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    def _send(self, code: int, body: bytes, ctype: str = "application/json; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if urlparse(self.path).path != "/api/record":
            self._send(404, b'{"error":"not found"}')
            return

        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n) if n else b"{}"
        try:
            rec = json.loads(raw.decode("utf-8"))
        except Exception:
            self._send(400, b'{"error":"bad json"}')
            return

        db_insert(rec)
        print(f"[{datetime.now():%H:%M:%S}] 收到 {rec}")
        self._send(200, b'{"ok":true}')

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)

        if u.path == "/api/records":
            limit = int(q.get("limit", ["100"])[0])
            rows, stat = db_query(limit,
                                  q.get("member", [None])[0],
                                  q.get("result", [None])[0])
            self._send(200, json.dumps(
                [{"member": r[1], "result": r[2], "reason": r[3],
                  "sim": r[4], "at": r[5]} for r in rows],
                ensure_ascii=False).encode("utf-8"))
            return

        if u.path in ("/", "/index.html"):
            rows, stat = db_query(100,
                                  q.get("member", [None])[0],
                                  q.get("result", [None])[0])
            html = render_page(rows, stat, q.get("member", [None])[0],
                               q.get("result", [None])[0])
            self._send(200, html.encode("utf-8"), "text/html; charset=utf-8")
            return

        self._send(404, b'{"error":"not found"}')

    def log_message(self, fmt, *args):
        pass          # 自己打印，别刷屏


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()

    db_init()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"门禁后端已启动：http://127.0.0.1:{args.port}/   (数据库 {DB_PATH})")
    print("ESP32 上报地址：http://<本机局域网IP>:%d/api/record" % args.port)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")


if __name__ == "__main__":
    main()
