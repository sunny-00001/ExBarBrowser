# -*- coding: utf-8 -*-
"""书签审查第四轮：bmk.add.tabs / bmk.copy 专项（新命令回归）。
全部变更经 Client().call 结构化命令；产物清理后核对基线。
"""
import io, json, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
sys.path.insert(0, 'tools')
from ebbctl import Client

c = Client()
fails = []
def check(name, cond, detail=''):
    print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else f'  [{detail}]'))
    if not cond: fails.append(f'{name}: {detail}')

def entries(parent=None):
    r = c.call('bmk.list', {} if parent is None else {'parentId': parent})
    return r['structuredContent']['entries']

def find_id(name):
    for b in entries():
        if b['name'] == name: return b['id']
    return None

def rm(id_):
    if id_: c.call('bmk.remove', {'id': id_}, approve=True)

BASE = entries()
BASE_IDS = [b['id'] for b in BASE]
BASE_PATHS = {b['path'] for b in BASE}
print(f'基线 {len(BASE)} 条'); print('=' * 62)

c.call('config.set', {'key': 'aiAllowDanger', 'value': True})

# ---------- 1. bmk.add.tabs ----------
st = c.state()
tabs = st.get('tabs') or []
expect_add = sum(1 for t in tabs if t and t not in BASE_PATHS)
print(f'  -> 当前 {len(tabs)} 个标签: {tabs}；其中未收藏 {expect_add} 个')

r = c.call('bmk.add.tabs', {})
sc = r.get('structuredContent') or {}
check('add.tabs ok 且返回 added/skipped/ids',
      r.get('ok') is True and isinstance(sc.get('added'), int) and 'ids' in sc,
      json.dumps({k: r.get(k) for k in ('ok','error')}))
check('add.tabs added 与未收藏标签数一致', sc.get('added') == expect_add,
      f"added={sc.get('added')} 期望={expect_add}")
check('add.tabs skipped 与已收藏/空标签数一致',
      sc.get('skipped') == len(tabs) - expect_add,
      f"skipped={sc.get('skipped')} 期望={len(tabs)-expect_add}")
added_ids = sc.get('ids') or []
check('add.tabs ids 数量与 added 一致', len(added_ids) == sc.get('added'),
      f'ids={added_ids}')
after = entries()
check('add.tabs ids 真实存在于书签栏',
      all(any(b['id'] == i for b in after) for i in added_ids), str(added_ids))

r2 = c.call('bmk.add.tabs', {})
sc2 = r2.get('structuredContent') or {}
check('add.tabs 二次调用幂等（added=0）', sc2.get('added') == 0,
      f'added={sc2.get("added")}')
check('add.tabs 二次调用全部 skipped', sc2.get('skipped') == len(tabs),
      f'skipped={sc2.get("skipped")}')

# 返回值契约兑现：用返回的 ids 精确清理
for i in added_ids: rm(i)
check('add.tabs 产物按返回 ids 清理干净',
      not [b for b in entries() if b['path'] in tabs and b['id'] in added_ids])

# ---------- 2. bmk.copy 边界与子树 ----------
r = c.call('bmk.copy', {'id': 'no-such-id'})
check('copy 幽灵 id 报错不崩溃', r.get('ok') is not True and r.get('ok') is not None,
      f'ok={r.get("ok")}')

r = c.call('bmk.newfolder', {'name': '审查4-分类'})
fid = find_id('审查4-分类')
check('newfolder 可用（返回 id 为已知缺陷#2，按名字回查）', bool(fid))
c.call('bmk.add.path', {'path': r'C:\Windows\System32', 'name': '审查4-子项', 'parentId': fid})
orig_child = c.call('bmk.list', {'parentId': fid})['structuredContent']['entries']

r = c.call('bmk.copy', {'id': fid})
copy_fid = (r.get('structuredContent') or {}).get('id') or ''
check('copy 分类返回新 id', r.get('ok') is True and bool(copy_fid),
      json.dumps({k: r.get(k) for k in ('ok','error')}))
copy_name = next((b['name'] for b in entries() if b['id'] == copy_fid), '')
check('copy 分类名带 副本 后缀', copy_name == '审查4-分类 副本', repr(copy_name))
copy_children = c.call('bmk.list', {'parentId': copy_fid})['structuredContent']['entries']
check('copy 子树内容一致', [b['name'] for b in copy_children] == [b['name'] for b in orig_child],
      f'{[b["name"] for b in copy_children]}')
check('copy 子树 id 全部重分配（不与原件撞 id）',
      bool(copy_children) and all(cc['id'] != oc['id'] for cc, oc in zip(copy_children, orig_child)),
      f'副本ids={[b["id"] for b in copy_children]} 原ids={[b["id"] for b in orig_child]}')
# 编辑副本不影响原件（id 重分配的行为学验证）
if copy_children:
    c.call('bmk.edit', {'id': copy_children[0]['id'], 'name': '审查4-子项-已改'})
    orig_after = c.call('bmk.list', {'parentId': fid})['structuredContent']['entries']
    check('改副本子项不影响原件', orig_after[0]['name'] == '审查4-子项',
          f'原件子项变成 {orig_after[0]["name"]!r}')

rm(fid); rm(copy_fid)
check('copy 产物清理干净', not find_id('审查4-分类') and not find_id('审查4-分类 副本'))

# ---------- 3. 基线恢复 ----------
final = entries()
check('条目集合回基线', sorted(b['id'] for b in final) == sorted(BASE_IDS))
if [b['id'] for b in final] != BASE_IDS:
    for i, bid in enumerate(BASE_IDS):
        c.call('bmk.move', {'id': bid, 'toIndex': i})
    check('顺序回基线', [b['id'] for b in entries()] == BASE_IDS)

c.call('config.set', {'key': 'aiAllowDanger', 'value': False})
print('=' * 62)
print(f'第四轮结束: {len(fails)} 项失败')
for f in fails: print('  FAIL:', f)
sys.exit(1 if fails else 0)
