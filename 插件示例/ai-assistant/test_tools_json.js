/* test_tools_json.js — 校验 AI_TOOLS_JSON 两段拼接后的完整性 (对 422 tools[9]=null 的回归防线):
 * 从 ai_agent.cpp 抽出所有 R"json(...)json" 段, 按出现顺序拼回一个字符串, 断言
 * 合法 JSON / 是数组 / 每个元素是带 name 的对象 / 无 null。退出码 = 失败数。 */
'use strict';
const fs = require('fs'), path = require('path');

const src = fs.readFileSync(path.join(__dirname, 'ai_agent.cpp'), 'utf8');
let segs = [], m, re = /R"json\(([\s\S]*?)\)json"/g;
while ((m = re.exec(src)) !== null) segs.push(m[1]);
// 只取工具定义那段 (以 "[" 开头的第一段) 与它的续段: 从含 "name":"run_search" 的段开始连续拼接
let start = segs.findIndex(s => s.includes('"name":"run_search"'));
if (start < 0) { console.error('FAIL: 找不到工具定义段'); process.exit(1); }
let joined = '';
for (let i = start; i < segs.length; i++) {
  joined += segs[i];
  if (segs[i].includes('])json') || segs[i].trimEnd().endsWith(']')) break;   // 到数组收尾段为止
}
let fails = 0;
let arr = null;
try {
  arr = JSON.parse(joined);
} catch (e) {
  console.error('FAIL: 拼接后不是合法 JSON —', e.message);
  process.exit(1);
}
const ok = Array.isArray(arr) && arr.length > 0 && arr.every(t => t && typeof t === 'object' && typeof t.name === 'string');
if (!ok) { console.error('FAIL: 数组含 null/非对象元素或结构异常, 共', arr.length, '项'); fails++; }
const expect = 28;
if (arr.length !== expect) { console.error(`FAIL: 工具数 ${arr.length} != 期望 ${expect} (新增/删除工具后请更新本断言)`); fails++; }
console.log(fails === 0 ? `PASS: ${arr.length} 个工具定义完整无缺` : `${fails} failed`);
process.exit(fails);
