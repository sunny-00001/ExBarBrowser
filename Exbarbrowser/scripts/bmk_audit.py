# -*- coding: utf-8 -*-
# 书签栏全操作面结构化审查（第二轮：修正第一轮脚本 bug）
#   · 所有变更经 Client().call（= ebbctl.py call 同一入口），本地只读回断言
#   · 第一轮教训：
#     (a) 批量 undo 收尾会穿透回滚审查前的历史操作 —— 本轮每项用逆操作精确恢复
#     (b) danger 命令需 approve=True（aiAllowDanger 只是硬开关）
#     (c) add.path 对已收藏路径会拒绝，测试用未收藏路径
#     (d) newfolder 不返回 id（真实缺陷），用名字回查 id
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

def find_id(name):
    for b in entries():
        if b['name'] == name:
            return b['id']
    return None

def rm(id_):
    if id_:
        c.call('bmk.remove', {'id': id_}, approve=True)

BASE = entries()
BASE_IDS = [b['id'] for b in BASE]
print(f'基线 {len(BASE)} 条')
print('=' * 62)

# ---------- 1. 可见性 ----------
c.call('bmk.hide', {})
check('hide 后可见=false', c.state().get('bookmarkBarVisible') is False)
c.call('bmk.show', {})
check('show 后可见=true', c.state().get('bookmarkBarVisible') is True)

# ---------- 2. newfolder：创建可用，但返回值缺 id（缺陷） ----------
c.call('bmk.newfolder', {'name': '审查A'})
fid = find_id('审查A')
check('newfolder 落盘且 isFolder', bool(fid), '按名字找不到分类')
nested_ok = False
fid2 = None
if fid:
    c.call('bmk.newfolder', {'name': '审查B', 'parentId': fid})
    fid2 = find_id('审查B')
    if fid2:
        sub = entries(fid)
        nested_ok = any(b['id'] == fid2 for b in sub)
check('newfolder 嵌套可用', nested_ok, '子分类未出现在父分类下')

# ---------- 3. add.path：未收藏路径 / 已收藏路径 / 非法路径 ----------
r = c.call('bmk.add.path', {'path': r'C:\Windows', 'name': '审查-临时'})
pid_ = (r.get('structuredContent') or {}).get('id') or ''
check('add.path 未收藏路径成功', r.get('ok') is True, json.dumps({k: r.get(k) for k in ('ok','error')}))
if not pid_:
    # 已知缺陷 #2（见 .workbuddy/memory/MEMORY.md）：成功不返回新条目 id。
    # 作为 xfail 记录，不计失败；按名字回查 id 以便后续步骤可用。
    pid_ = find_id('审查-临时') or ''
    print('  -> 已知缺陷#2: add.path 成功但 structuredContent=null（不返回 id），xfail 标记，按名字回查')

r = c.call('bmk.add.path', {'path': r'E:\ExBarBrowser', 'name': '审查-重复路径'})
check('add.path 已收藏路径被拒（可观测性）',
      r.get('ok') is not True,
      '居然成功')
print(f"  -> 已收藏路径拒绝时的报错: {r.get('error','')!r}（'命令执行失败'不透明=改进点）")

r = c.call('bmk.add.path', {'path': '::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C', 'name': '审查-缺括号'})
bad_id = (r.get('structuredContent') or {}).get('id') or ''
if r.get('ok') is True and bad_id:
    print(f'  -> 缺 }} CLSID 被收下 id={bad_id}（缺陷：无输入校验）')
    rm(bad_id)
else:
    print(f'  -> 缺 }} CLSID 被拒绝: {r.get("error","")!r}')

# ---------- 4. add.selection：无选中应被前置拒绝 ----------
r = c.call('bmk.add.selection', {})
check('无选中 add.selection 被拒', r.get('ok') is not True, json.dumps({k: r.get(k) for k in ('ok','error')}))
print(f"  -> 拒绝原因: {r.get('error','')}")

# ---------- 5. edit：改名 / 改纯分类（用真实临时条目） ----------
if pid_:
    r = c.call('bmk.edit', {'id': pid_, 'name': '审查-改名'})
    it = [b for b in entries() if b['id'] == pid_]
    check('edit 改名读回', r.get('ok') is True and it and it[0]['name'] == '审查-改名', json.dumps({k: r.get(k) for k in ('ok','error')}))
    r = c.call('bmk.edit', {'id': pid_, 'path': ''})
    it = [b for b in entries() if b['id'] == pid_]
    check('edit 改纯分类（path=空）读回', r.get('ok') is True and it and it[0].get('path') == '', json.dumps({k: r.get(k) for k in ('ok','error')}))

# ---------- 6. move：toIndex / toId / 越界 ----------
if pid_:
    r = c.call('bmk.move', {'id': pid_, 'toIndex': 0})
    ids_now = [b['id'] for b in entries()]
    check('move toIndex=0 置顶', r.get('ok') is True and ids_now and ids_now[0] == pid_)

    if fid:
        r = c.call('bmk.move', {'id': pid_, 'toId': fid, 'toIndex': 0})
        sub = entries(fid)
        check('move toId 移入分类', r.get('ok') is True and any(b['id'] == pid_ for b in sub),
              json.dumps({k: r.get(k) for k in ('ok','error')}))
        r = c.call('bmk.move', {'id': pid_, 'toId': '', 'toIndex': len(entries()) - 1})
        check('move 移回顶层', r.get('ok') is True, json.dumps({k: r.get(k) for k in ('ok','error')}))

    r = c.call('bmk.move', {'id': pid_, 'toIndex': 9999})
    check('move 越界不崩溃', r.get('ok') is not None)
    print(f"  -> 越界行为: ok={r.get('ok')} err={r.get('error','')!r}")

# ---------- 7. dedupe（danger，需 approve） ----------
# add.path 会拒绝已收藏路径（防重语义），制造重复只能走 bmk.copy ——
# 顺带验证 copy 的返回值契约（新命令必须返回新 id）。
c.call('bmk.add.path', {'path': r'C:\Windows\System32', 'name': '审查-重复1'})
src_id = find_id('审查-重复1')
r = c.call('bmk.copy', {'id': src_id})
copy_id = (r.get('structuredContent') or {}).get('id') or ''
check('bmk.copy 返回新 id（返回值契约）', r.get('ok') is True and bool(copy_id),
      json.dumps({k: r.get(k) for k in ('ok','error')}))
n0 = len(entries())
r = c.call('bmk.dedupe', {}, approve=True)
n1 = len(entries())
check('dedupe 去重 -1 条（副本与原件同路径）', r.get('ok') is True and n1 == n0 - 1,
      f'{n0}->{n1} err={r.get("error","")}')
dups = [b['id'] for b in entries() if b['name'].startswith('审查-重复')]
for x in dups:
    rm(x)

# ---------- 8. sort（逆操作恢复） ----------
before = [b['id'] for b in entries()]
c.call('bmk.sort', {'ascending': False})
mid = [b['id'] for b in entries()]
check('sort 降序确实生效', mid != before)
c.call('bmk.sort', {'ascending': True})
after = [b['id'] for b in entries()]
print(f'  -> 升序恢复后与排序前不同属正常（sort 按名称重排，非原顺序）')

# ---------- 9. 清理全部审查产物 ----------
for nm in ('审查A', '审查B', '审查-临时', '审查-改名', '审查-缺括号', '审查-重复1', '审查-重复1 副本'):
    rm(find_id(nm))
leftover = [b for b in entries() if b['name'].startswith('审查')]
check('审查产物清理干净', not leftover, str(leftover))

# ---------- 10. 基线对比（顺序可能有 sort 重排影响） ----------
final_ids = sorted(b['id'] for b in entries())
base_ids_sorted = sorted(BASE_IDS)
check('条目集合回到基线（id 集一致）', final_ids == base_ids_sorted,
      f'多: {set(final_ids)-set(base_ids_sorted)} 少: {set(base_ids_sorted)-set(final_ids)}')
order_now = [b['id'] for b in entries()]
if order_now != BASE_IDS:
    print(f'  -> 顺序与基线不同（sort 重排所致），结构化调 move 恢复原顺序')
    # 按 BASE_IDS 顺序逐个 toIndex 归位
    for i, bid in enumerate(BASE_IDS):
        c.call('bmk.move', {'id': bid, 'toIndex': i})
    final = [b['id'] for b in entries()]
    check('顺序恢复基线', final == BASE_IDS, f'{final}')

c.call('config.set', {'key': 'aiAllowDanger', 'value': False})
print('=' * 62)
print(f'审查结束: {len(fails)} 项失败')
for f in fails:
    print('  FAIL:', f)
sys.exit(1 if fails else 0)
