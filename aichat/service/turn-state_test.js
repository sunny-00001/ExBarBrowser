// turn-state.js 的离线回归：回合号持久单调、旧历史迁移、同会话串行、异常不死锁。
"use strict";
const fs = require("fs");
const os = require("os");
const path = require("path");
const assert = require("assert");
const { createTurnState } = require("./turn-state");

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "exbar-turns-"));
const st = createTurnState(dir);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}

(async () => {
  console.log("\n[1] 单调回合号（截断 / 清空后不复用）");
  ok(st.nextTurnId("s1") === 1, "首回合 = 1");
  ok(st.nextTurnId("s1") === 2, "第二回合 = 2");
  fs.writeFileSync(path.join(dir, "s1.json"), JSON.stringify([
    { role: "user", content: "只剩这一条", turn: 2, turnId: 2 }
  ]), "utf8");
  ok(st.nextTurnId("s1") === 3, "历史被截断到 1 条后仍 = 3");
  fs.writeFileSync(path.join(dir, "s1.json"), "[]", "utf8");
  ok(st.nextTurnId("s1") === 4, "/api/clear 之后仍 = 4");
  ok(st.currentTurnId("s1") === 4, "currentTurnId 与已发回合一致");

  console.log("\n[2] 无 .turn.json 的旧会话从历史最大 turn 续号");
  fs.writeFileSync(path.join(dir, "s2.json"), JSON.stringify([
    { role: "user", content: "a" },
    { role: "assistant", content: "b", turn: 9 },
    { role: "assistant", content: "c", turnId: 11 }
  ]), "utf8");
  ok(st.nextTurnId("s2") === 12, "取 max(9, 11) + 1 = 12");

  console.log("\n[3] 同会话串行：第二个回合等第一个写回后才读历史");
  const order = [];
  const p1 = st.withTurnRun("s3", async () => {
    order.push("a-start");
    await sleep(25);
    order.push("a-end");
  });
  const p2 = st.withTurnRun("s3", async () => {
    order.push("b-start");
    await sleep(5);
    order.push("b-end");
    return "b";
  });
  ok(st.isBusy("s3") === true, "锁期间 isBusy=true");
  const values = await Promise.all([p1, p2]);
  ok(order.join(",") === "a-start,a-end,b-start,b-end", "执行顺序无重叠：" + order.join(","));
  ok(values[1] === "b", "排队回合的返回值原样透传");
  ok(st.isBusy("s3") === false, "结束后锁释放");

  console.log("\n[4] 异常不泄漏锁");
  await st.withTurnRun("s4", async () => { await sleep(5); throw new Error("boom"); })
    .then(() => ok(false, "异常应向外抛出"))
    .catch((e) => ok(e.message === "boom", "异常向外抛出且锁释放"));
  ok(st.isBusy("s4") === false, "抛错后 isBusy=false");
  const afterError = await st.withTurnRun("s4", async () => "ok");
  ok(afterError === "ok", "后续回合仍能运行");

  console.log("\n[5] 不同会话互不阻塞");
  const cross = [];
  const q1 = st.withTurnRun("s5", async () => { cross.push("a-start"); await sleep(25); cross.push("a-end"); });
  const q2 = st.withTurnRun("s6", async () => { cross.push("b-start"); await sleep(5); cross.push("b-end"); });
  await Promise.all([q1, q2]);
  ok(cross.indexOf("b-end") < cross.indexOf("a-end"), "不同 session 并行：" + cross.join(","));

  assert.strictEqual(fail, 0, `${fail} 项失败`);
  console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
  fs.rmSync(dir, { recursive: true, force: true });
  process.exit(fail ? 1 : 0);
})().catch((e) => {
  console.error(e);
  fs.rmSync(dir, { recursive: true, force: true });
  process.exit(1);
});
