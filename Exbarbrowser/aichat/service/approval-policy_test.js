// approval-policy.js 的离线回归：三种执行模式的审批口径不可逆。
"use strict";
const assert = require("assert");
const { isMutating, shouldAskApproval } = require("./approval-policy");

let pass = 0, fail = 0;
function ok(cond, what) {
  if (cond) { pass++; console.log("  OK", what); }
  else { fail++; console.log("  FAIL", what); }
}
const meta = (o) => Object.assign({ known: true, needed: false, readOnly: false }, o);

console.log("\n[1] 危险命令：任何模式都要问");
ok(shouldAskApproval(meta({ needed: true }), false) === true, "confirm 模式问危险命令");
ok(shouldAskApproval(meta({ needed: true }), true) === true, "auto 模式仍问危险命令");

console.log("\n[2] 普通写：confirm 问，auto 不问");
ok(shouldAskApproval(meta({ readOnly: false }), false) === true, "confirm 模式问普通写");
ok(shouldAskApproval(meta({ readOnly: false }), true) === false, "auto 模式免普通写");

console.log("\n[3] 只读：任何模式都不问");
ok(shouldAskApproval(meta({ readOnly: true }), false) === false, "confirm 模式不问只读");
ok(shouldAskApproval(meta({ readOnly: true }), true) === false, "auto 模式不问只读");

console.log("\n[4] 未知工具 fail closed");
ok(shouldAskApproval({ known: false, needed: false, readOnly: false }, true) === true,
  "auto 模式遇到未知工具仍问");
ok(isMutating({ known: false, needed: false, readOnly: false }) === true,
  "readonly 模式把未知工具当写命令拦下");

console.log("\n[5] readonly 模式拦写不拦读");
ok(isMutating(meta({ readOnly: false, needed: false })) === true, "普通写是 mutation");
ok(isMutating(meta({ readOnly: true, needed: false })) === false, "只读不是 mutation");
ok(isMutating(null) === true, "元数据缺失按 mutation 处理");

assert.strictEqual(fail, 0, `${fail} 项失败`);
console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
process.exit(fail ? 1 : 0);
