#!/usr/bin/env python3
"""双核门禁 —— 通行记录后端（零依赖，只用 Python 标准库）

    python3 tools/backend/server.py --port 8080

设计分工：
    ESP32 只上报"哪个成员(mid) + 结果 + 方式 + 相似度"
    人员信息（姓名/部门/备注）由本服务维护 —— 管理平台本来就该管人，
    而且这样名字能用中文（设备侧触屏打不了中文）。

接口：
    POST /api/record     {"mid":3,"member":"成员 3","result":"OK","reason":"face","sim":0.87,"ts":120}
    POST /api/member     {"mid":3,"name":"张三","dept":"研发部","note":"实习生"}
    GET  /api/records    通行记录（已带姓名）
    GET  /api/members    人员表
    GET  /               记录页（可筛选）
    GET  /members        人员管理页（改名）
"""

from __future__ import annotations

import argparse
import json
import sqlite3
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

DB_PATH = Path(__file__).with_name("records.db")

RESULT_TEXT = {"OK": "通行", "DENIED": "拒绝", "REPLAY": "重放", "LOCKED": "锁定中"}
REASON_TEXT = {"face": "刷脸", "password": "密码", "admin": "管理员"}

STYLE = """
 body{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;margin:0;background:#0e1420;color:#e6edf3}
 .wrap{max-width:960px;margin:0 auto;padding:24px}
 h1{font-size:20px;margin:0 0 4px}
 .sub{color:#7d8ea0;font-size:13px;margin-bottom:16px}
 .chip{display:inline-block;background:#1b2634;border:1px solid #2b3a4a;border-radius:12px;
        padding:4px 10px;margin-right:8px;font-size:13px}
 table{width:100%;border-collapse:collapse;margin-top:16px;font-size:14px}
 th,td{text-align:left;padding:10px 8px;border-bottom:1px solid #1e2a38}
 th{color:#7d8ea0;font-weight:500;font-size:13px}
 .ok{color:#3ddc78} .no{color:#ff8a80}
 form{margin:16px 0}
 input,select,button{background:#16202c;color:#e6edf3;border:1px solid #2b3a4a;
        border-radius:8px;padding:8px 10px;font-size:14px}
 button{background:#185fa5;border:none;cursor:pointer}
 a{color:#7fb2e5;text-decoration:none;margin-right:12px}
"""


# ---------------------------------------------------------------- 数据库
def db_init() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        """CREATE TABLE IF NOT EXISTS records (
               id      INTEGER PRIMARY KEY AUTOINCREMENT,
               ts      INTEGER NOT NULL,
               mid     INTEGER DEFAULT 0,     -- 成员 id（0 = 未识别/管理员）
               member  TEXT,                  -- 设备侧文字（兜底）
               result  TEXT,
               reason  TEXT,
               sim     REAL,
               at      TEXT NOT NULL
           )"""
    )
    conn.execute(
        """CREATE TABLE IF NOT EXISTS members (
               mid     INTEGER PRIMARY KEY,
               name    TEXT,
               dept    TEXT,
               note    TEXT,
               updated TEXT
           )"""
    )
    # 人员表版本号：设备只轮询这个数字，变了才全量拉人员表
    conn.execute("CREATE TABLE IF NOT EXISTS members_meta (k TEXT PRIMARY KEY, v INTEGER)")
    conn.execute("INSERT OR IGNORE INTO members_meta (k, v) VALUES ('members_ver', 1)")
    conn.commit()
    return conn


def members_ver(conn: sqlite3.Connection) -> int:
    row = conn.execute("SELECT v FROM members_meta WHERE k='members_ver'").fetchone()
    return row[0] if row else 0


def members_ver_bump(conn: sqlite3.Connection) -> None:
    conn.execute("UPDATE members_meta SET v = v + 1 WHERE k='members_ver'")


def db_insert_record(rec: dict) -> None:
    conn = db_init()
    mid = int(rec.get("mid", 0) or 0)
    conn.execute(
        "INSERT INTO records (ts, mid, member, result, reason, sim, at) VALUES (?,?,?,?,?,?,?)",
        (
            int(rec.get("ts", 0)),
            mid,
            str(rec.get("member", "")),
            str(rec.get("result", "")),
            str(rec.get("reason", "")),
            float(rec.get("sim", 0)),
            datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        ),
    )
    # 新出现的成员先占个位，方便在人员页补名字
    if mid > 0:
        conn.execute("INSERT OR IGNORE INTO members (mid, name, dept, note, updated) "
                     "VALUES (?,?,?,?,?)", (mid, "", "", "", ""))
    conn.commit()
    conn.close()


def db_upsert_member(mid: int, name: str, dept: str, note: str) -> None:
    conn = db_init()
    conn.execute(
        "INSERT INTO members (mid, name, dept, note, updated) VALUES (?,?,?,?,?) "
        "ON CONFLICT(mid) DO UPDATE SET name=excluded.name, dept=excluded.dept, "
        "note=excluded.note, updated=excluded.updated",
        (mid, name, dept, note, datetime.now().strftime("%Y-%m-%d %H:%M")),
    )
    members_ver_bump(conn)
    conn.commit()
    conn.close()


def db_records(limit=200, member_key="", result=""):
    conn = db_init()
    sql = """
        SELECT r.at, r.mid, COALESCE(NULLIF(m.name,''), NULLIF(r.member,''), '成员 '||r.mid) AS who,
               m.dept, r.result, r.reason, r.sim
        FROM records r LEFT JOIN members m ON m.mid = r.mid
    """
    where, args = [], []
    if member_key:
        where.append("(m.name LIKE ? OR r.member LIKE ? OR CAST(r.mid AS TEXT) = ?)")
        args += [f"%{member_key}%", f"%{member_key}%", member_key]
    if result:
        where.append("r.result = ?")
        args.append(result)
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY r.id DESC LIMIT ?"
    args.append(limit)
    rows = conn.execute(sql, args).fetchall()
    stat = conn.execute("SELECT result, COUNT(*) FROM records GROUP BY result").fetchall()
    conn.close()
    return rows, stat


def db_members():
    conn = db_init()
    rows = conn.execute(
        """SELECT m.mid, m.name, m.dept, m.note, m.updated,
                  (SELECT COUNT(*) FROM records r WHERE r.mid = m.mid) AS visits
           FROM members m ORDER BY m.mid"""
    ).fetchall()
    conn.close()
    return rows


# ---------------------------------------------------------------- 页面
def esc(s) -> str:
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def page(title: str, body: str, refresh: bool = False) -> str:
    # 只有"记录页"才自动刷新；编辑页一旦自动刷新，正在输入的内容会被清掉
    meta = '<meta http-equiv="refresh" content="5">' if refresh else ""
    return f"""<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
{meta}
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title}</title><style>{STYLE}</style></head>
<body><div class="wrap">
<div><a href="/">通行记录</a><a href="/members">人员管理</a></div>
<h1>{title}</h1>
{body}
</div></body></html>"""


def render_records(rows, stat, member_key, result_filter) -> str:
    chips = "".join(
        f'<span class="chip">{esc(RESULT_TEXT.get(r, r))} {n}</span>' for r, n in stat
    ) or '<span class="chip">暂无记录</span>'

    lines = []
    for at, mid, who, dept, result, reason, sim in rows:
        cls = "ok" if result == "OK" else "no"
        lines.append(
            f"<tr><td>{esc(at)}</td><td>{esc(who)}</td><td>{esc(dept or '')}</td>"
            f'<td class="{cls}">{esc(RESULT_TEXT.get(result, result))}</td>'
            f"<td>{esc(REASON_TEXT.get(reason, reason))}</td><td>{sim:.2f}</td></tr>"
        )
    body_rows = "".join(lines) or '<tr><td colspan="6">还没有记录</td></tr>'

    opts = "".join(f'<option value="{k}" {"selected" if result_filter == k else ""}>{v}</option>'
                   for k, v in RESULT_TEXT.items())

    body = f"""
<div class="sub">每 5 秒自动刷新 · 数据来自 ESP32 上报 · 姓名取自「人员管理」</div>
<div>{chips}</div>
<form method="get">
 <input name="member" placeholder="姓名 / 成员编号" value="{esc(member_key)}">
 <select name="result"><option value="">全部结果</option>{opts}</select>
 <button type="submit">筛选</button>
</form>
<table><tr><th>时间</th><th>成员</th><th>部门</th><th>结果</th><th>方式</th><th>相似度</th></tr>
{body_rows}</table>"""
    return page("双核门禁 · 通行记录", body, refresh=True)


def render_members(rows) -> str:
    """注意：<form> 不能直接嵌在 <tr> 里（浏览器会把它移出去，导致提交空数据）。
    这里用 HTML 的 form 属性：表单元素统一放在表格外，输入框用 form="fN" 关联。"""
    forms = []
    lines = []
    for mid, name, dept, note, updated, visits in rows:
        fid = f"f{mid}"
        forms.append(f'<form id="{fid}" method="post" action="/api/member">'
                     f'<input type="hidden" name="mid" value="{mid}"></form>')
        lines.append(f"""<tr>
  <td>{mid}</td>
  <td><input form="{fid}" name="name" value="{esc(name or '')}" placeholder="姓名"></td>
  <td><input form="{fid}" name="dept" value="{esc(dept or '')}" placeholder="部门" size="10"></td>
  <td><input form="{fid}" name="note" value="{esc(note or '')}" placeholder="备注" size="10"></td>
  <td>{visits}</td>
  <td><button form="{fid}" type="submit">保存</button></td>
</tr>""")
    body_rows = "".join(lines) or '<tr><td colspan="6">还没有成员，先录入一张脸</td></tr>'

    body = f"""
<div class="sub">设备只上报成员编号，姓名在这里填（支持中文）· 保存后记录页立刻显示姓名</div>
{''.join(forms)}
<table><tr><th>编号</th><th>姓名</th><th>部门</th><th>备注</th><th>通行次数</th><th></th></tr>
{body_rows}</table>"""
    return page("双核门禁 · 人员管理", body, refresh=False)   # 编辑页不要自动刷新，否则刚输入就被刷掉


# ---------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    def _send(self, code: int, body: bytes, ctype: str = "application/json; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_form_or_json(self) -> dict:
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n) if n else b""
        ctype = self.headers.get("Content-Type", "")
        if "application/json" in ctype:
            try:
                return json.loads(raw.decode("utf-8"))
            except Exception:
                return {}
        q = parse_qs(raw.decode("utf-8"))          # 表单
        return {k: v[0] for k, v in q.items()}

    def do_POST(self):
        path = urlparse(self.path).path
        data = self._read_form_or_json()

        if path == "/api/record":
            db_insert_record(data)
            print(f"[{datetime.now():%H:%M:%S}] 记录 {data}")
            self._send(200, b'{"ok":true}')
            return

        if path == "/api/member":
            try:
                mid = int(data.get("mid", 0))
            except (TypeError, ValueError):
                mid = 0
            if mid <= 0:
                self._send(400, b'{"error":"bad mid"}')
                return
            db_upsert_member(mid, str(data.get("name", "")),
                             str(data.get("dept", "")), str(data.get("note", "")))
            print(f"[{datetime.now():%H:%M:%S}] 人员 {mid} -> {data.get('name','')}")
            # 表单提交跳回人员页；JSON 调用返回 json
            if "application/json" in self.headers.get("Content-Type", ""):
                self._send(200, b'{"ok":true}')
            else:
                self.send_response(302)
                self.send_header("Location", "/members")
                self.end_headers()
            return

        self._send(404, b'{"error":"not found"}')

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)

        if u.path == "/api/records":
            rows, _ = db_records(int(q.get("limit", ["200"])[0]),
                                 q.get("member", [""])[0], q.get("result", [""])[0])
            self._send(200, json.dumps(
                [{"at": r[0], "mid": r[1], "name": r[2], "dept": r[3],
                  "result": r[4], "reason": r[5], "sim": r[6]} for r in rows],
                ensure_ascii=False).encode("utf-8"))
            return

        if u.path == "/api/members/version":
            conn = db_init()
            ver = members_ver(conn)
            conn.close()
            self._send(200, json.dumps({"ver": ver}).encode("utf-8"))
            return

        if u.path == "/api/members":
            rows = db_members()
            self._send(200, json.dumps(
                [{"mid": r[0], "name": r[1], "dept": r[2], "note": r[3], "visits": r[5]}
                 for r in rows], ensure_ascii=False).encode("utf-8"))
            return

        if u.path in ("/", "/index.html"):
            rows, stat = db_records(200, q.get("member", [""])[0], q.get("result", [""])[0])
            html = render_records(rows, stat, q.get("member", [""])[0], q.get("result", [""])[0])
            self._send(200, html.encode("utf-8"), "text/html; charset=utf-8")
            return

        if u.path == "/members":
            self._send(200, render_members(db_members()).encode("utf-8"),
                       "text/html; charset=utf-8")
            return

        self._send(404, b'{"error":"not found"}')

    def log_message(self, fmt, *args):
        pass


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()

    db_init()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"门禁后端已启动：http://127.0.0.1:{args.port}/   (数据库 {DB_PATH})")
    print(f"ESP32 上报地址：http://<本机局域网IP>:{args.port}/api/record")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")


if __name__ == "__main__":
    main()
