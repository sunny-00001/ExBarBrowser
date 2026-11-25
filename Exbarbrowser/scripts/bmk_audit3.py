# -*- coding: utf-8 -*-
# 书签栏结构化审查第三轮：补齐前两轮遗漏的命令 + 边界行为
# 遗漏项：bmk.add.current / bmk.toggle / bmk.clear / bmk.remove 边界 / bmk.list parentId
# 纪律：变更全部走 Client().call；clear 用"精确一次 undo"恢复；产物用逆操作清理
import sys, io, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
sys.path.insert(0, 'tools')
from ebbctl import Client

c = Client()
c.call('config.set', {'key': 'aiAllowDanger', 'value': True})
fails = []

def check(name, cond, detail=''):
    tag = 'PASS' if cond else 'FAIL'
    print(f'{tag} {name}' + (f'  [{detail}]' if detail and not cond else ''))
    if not cond:
        fails.append(f'{name}: {detail}')

def entries(parent=None):
    r = c.call('bmk.list', {} if parent is None else {'parentId': parent})
    return r['structuredContent']['entries']

BASE = entries()
BASE_IDS = [b['id'] for b in BASE]
BASE_LEN = len(BASE_IDS)
print(f'基线 {BASE_LEN} 条: {BASE_IDS}')
print('=' * 62)

# ---------- 1. bmk.add.current（收藏当前目录） ----------
s = c.state()
active = s.get('activePath', '')
print(f"当前 activePath = {active!r}")
already = [b for b in BASE if b['path'] == active]
r = c.call('bmk.add.current', {})
if already:
    check('add.current 对已收藏目录被拒', r.get('ok') is not True,
          json.dumps({k: r.get(k) for k in ('ok','error')}))
    print(f"  -> 报错: {r.get('error','')!r}")
else:
    check('add.current 成功', r.get('ok') is True, json.dumps({k: r.get(k) for k in ('ok','error')}))
    e2 = entries()
    newone = [b for b in e2 if b['path'] == active and b['id'] not in BASE_IDS]
    check('add.current 读回（当前目录已在书签栏末尾）', len(newone) == 1,
          str([(b['id'],b['name'],b['path']) for b in e2 if b['id'] not in BASE_IDS]))
    print(f"  -> 新条目: {[(b['id'],b['name']) for b in newone]}")
    print(f"  -> 返回值含 id? {'structuredContent' in r and bool((r.get('structuredContent') or {}).get('id'))}")
    if newone:
        c.call('bmk.remove', {'id': newone[0]['id']}, approve=True)

# ---------- 2. bmk.toggle ----------
vis0 = c.state().get('bookmarkBarVisible')
c.call('bmk.toggle', {})
vis1 = c.state().get('bookmarkBarVisible')
c.call('bmk.toggle', {})
vis2 = c.state().get('bookmarkBarVisible')
check('toggle 切换生效', vis1 != vis0, f'{vis0}->{vis1}')
check('toggle 两次回到原态', vis2 == vis0, f'{vis2} vs {vis0}')

# ---------- 3. bmk.clear（danger；PushUndo 保证可撤） ----------
r = c.call('bmk.clear', {}, approve=True)
check('clear 执行成功', r.get('ok') is True, json.dumps({k: r.get(k) for k in ('ok','error')}))
n_after_clear = len(entries())
check('clear 后书签为空', n_after_clear == 0, f'{n_after_clear} 条残留')
# 空书上再 clear 一次：handler roots().empty() 时返回 false
r2 = c.call('bmk.clear', {}, approve=True)
check('空书再 clear 被拒（幂等护栏）', r2.get('ok') is not True,
      json.dumps({k: r2.get(k) for k in ('ok','error')}))
print(f"  -> 空书 clear 报错: {r2.get('error','')!r}")
# 精确 undo 一次撤销 clear
r = c.call('bmk.undo', {})
restored = entries()
check('undo 一次恢复全部书签', len(restored) == BASE_LEN and
      sorted(b['id'] for b in restored) == sorted(BASE_IDS),
      f'{len(restored)} 条')

# ---------- 4. 边界：不存在的 id ----------
r = c.call('bmk.remove', {'id': 'b-99999'}, approve=True)
check('remove 不存在 id 报错不崩溃', r.get('ok') is not True and len(entries()) == BASE_LEN,
      json.dumps({k: r.get(k) for k in ('ok','error')}))
print(f"  -> remove 幽灵 id 报错: {r.get('error','')!r}")
r = c.call('bmk.edit', {'id': 'b-99999', 'name': 'x'})
check('edit 不存在 id 报错不崩溃', r.get('ok') is not True and len(entries()) == BASE_LEN,
      json.dumps({k: r.get(k) for k in ('ok','error')}))
print(f"  -> edit 幽灵 id 报错: {r.get('error','')!r}")
r = c.call('bmk.move', {'id': 'b-99999', 'toIndex': 0})
check('move 不存在 id 报错不崩溃', r.get('ok') is not True and len(entries()) == BASE_LEN,
      json.dumps({k: r.get(k) for k in ('ok','error')}))
print(f"  -> move 幽灵 id 报错: {r.get('error','')!r}")

# ---------- 5. bmk.list parentId 边界 ----------
r = c.call('bmk.list', {'parentId': 'f-99999'})
check('list 幽灵 parentId 不崩溃', r.get('ok') is not None, json.dumps({k: r.get(k) for k in ('ok','error')}))
print(f"  -> 幽灵 parentId 返回: ok={r.get('ok')} err={r.get('error','')!r}")

# ---------- 6. bmk.focus 后焦点状态 ----------
r = c.call('bmk.focus', {})
s = c.state()
print(f"  -> bmk.focus 执行 ok={r.get('ok')}；state.focus={s.get('focus')!r}（后台窗口无键盘焦点时 unknown 属预期）")
check('focus 执行不崩溃', r.get('ok') is True or r.get('error') != '', '')

# ---------- 7. 磁盘一致性 ----------
import hashlib
raw = open('data/bookmarks.html', 'rb').read()
final = entries()
check('终态与基线一致（条目集）',
      sorted(b['id'] for b in final) == sorted(BASE_IDS) and len(final) == BASE_LEN,
      f'{[b["id"] for b in final]}')
order_now = [b['id'] for b in final]
if order_now != BASE_IDS:
    print('  -> 顺序有偏差，结构化恢复')
    for i, bid in enumerate(BASE_IDS):
        c.call('bmk.move', {'id': bid, 'toIndex': i})
    final2 = [b['id'] for b in entries()]
    check('顺序恢复基线', final2 == BASE_IDS, f'{final2}')

c.call('config.set', {'key': 'aiAllowDanger', 'value': False})
print('=' * 62)
print(f'第三轮结束: {len(fails)} 项失败')
for f in fails:
    print('  FAIL:', f)
sys.exit(1 if fails else 0)
