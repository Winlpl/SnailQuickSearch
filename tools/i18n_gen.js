#!/usr/bin/env node
/* i18n 语言包生成器 — languages\{zh,zh-TW,en,ko,th,ms}.json 六包的唯一生成入口。
 *
 * 日常流程 (= AGENTS.md "新增文案三步"):
 *   ① 源码写  XjsT(L"新.键")            (或 XjsTUtf8(L"新.键"))
 *   ② tools/i18n_keys_map.json 加  "新.键": "中文文案"
 *   ③ tools/i18n_overrides.json 各语言加译文, 然后  node tools/i18n_gen.js
 *
 * 生成规则:
 *   - zh 包 = keys_map 全量 (键序 = keys_map 序)
 *   - 其余包按 keys_map 键序取 overrides[lang][键]; 缺译时保留包内旧值;
 *     两者皆无 → 该键不入包 (运行期回落链 当前语言→en→zh→键名), 记入 missing
 *   - 包内已有但 keys_map 没有的键原样保留 (静态表间接键/待清理), 清单进 missing
 *   - overrides 里指向不存在键的死译文 → tools/i18n_overrides.json.extra
 *   - 源码扫描 XjsT(/XjsTUtf8( 的字面量键, 缺 keys_map 映射 → 告警 (界面会露键名)
 *   - 生成结果与包内容逐字节一致时不写文件 (git 无 diff = 生成器与包一致)
 *
 * 首次 bootstrap (从现有六包导出 keys_map/overrides):
 *   node tools/i18n_gen.js --bootstrap
 * 本脚本纯 Node 内置模块, 无第三方依赖。
 */
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const LANG_DIR = path.join(ROOT, 'languages');
const KEYS_MAP = path.join(__dirname, 'i18n_keys_map.json');
const OVERRIDES = path.join(__dirname, 'i18n_overrides.json');
const MISSING_TXT = path.join(__dirname, 'i18n_missing.txt');
const EXTRA_TXT = path.join(__dirname, 'i18n_overrides.json.extra');
const LANGS = ['zh', 'zh-TW', 'en', 'ko', 'th', 'ms'];

const warn = (msg) => console.log('[告警] ' + msg);

function readJson(file) {
  if (!fs.existsSync(file)) return null;
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}
function writeIfChanged(file, obj) {
  const text = JSON.stringify(obj, null, ' ');   /* 六包既有格式 = 单空格缩进 */
  const old = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : null;
  if (old === text) return false;
  fs.writeFileSync(file, text, 'utf8');
  return true;
}
function writeReport(file, lines) {
  if (lines.length) fs.writeFileSync(file, lines.join('\n') + '\n', 'utf8');
  else if (fs.existsSync(file)) fs.unlinkSync(file);
}

/* ---- bootstrap: 现有六包 → keys_map (zh) + overrides (其余五包) ---- */
function bootstrap() {
  const zh = readJson(path.join(LANG_DIR, 'zh.json'));
  if (!zh) { console.error('缺 languages/zh.json'); process.exit(1); }
  const overrides = {};
  for (const lang of LANGS) {
    if (lang === 'zh') continue;
    const pack = readJson(path.join(LANG_DIR, lang + '.json')) || {};
    overrides[lang] = pack;
  }
  console.log('keys_map 键数: ' + Object.keys(zh).length);
  for (const lang of Object.keys(overrides))
    console.log('overrides.' + lang + ' 键数: ' + Object.keys(overrides[lang]).length);
  const a = writeIfChanged(KEYS_MAP, zh);
  const b = writeIfChanged(OVERRIDES, overrides);
  console.log((a ? '已写入 ' : '无变化 ') + path.relative(ROOT, KEYS_MAP));
  console.log((b ? '已写入 ' : '无变化 ') + path.relative(ROOT, OVERRIDES));
}

/* ---- 生成 ---- */
function generate() {
  const keysMap = readJson(KEYS_MAP);
  const overrides = readJson(OVERRIDES) || {};
  if (!keysMap) {
    console.error('缺 tools/i18n_keys_map.json — 先跑  node tools/i18n_gen.js --bootstrap');
    process.exit(1);
  }

  /* 源码扫描: 仓库根 *.cpp/*.h 的 XjsT(/XjsTUtf8( 字面量键 (UI 模块全在根) */
  const sourceKeys = new Map();   /* 键 → [文件:行, …] */
  for (const name of fs.readdirSync(ROOT)) {
    if (!/\.(cpp|h)$/i.test(name)) continue;
    const lines = fs.readFileSync(path.join(ROOT, name), 'utf8').split('\n');
    lines.forEach((line, i) => {
      const re = /XjsT(?:Utf8)?\s*\(\s*L"([^"]+)"/g;
      let m;
      while ((m = re.exec(line))) {
        const key = m[1];
        if (!sourceKeys.has(key)) sourceKeys.set(key, []);
        sourceKeys.get(key).push(name + ':' + (i + 1));
      }
    });
  }
  console.log('源码 XjsT 键: ' + sourceKeys.size + '  keys_map 键: ' + Object.keys(keysMap).length);

  const missing = [];   /* i18n_missing.txt 汇总 */
  let missingTrans = 0;

  /* 死译文: overrides 指向 keys_map 没有的键 */
  const dead = [];
  for (const lang of Object.keys(overrides))
    for (const k of Object.keys(overrides[lang]))
      if (!(k in keysMap)) dead.push(lang + '\t' + k);
  writeReport(EXTRA_TXT, dead);

  for (const lang of LANGS) {
    const packPath = path.join(LANG_DIR, lang + '.json');
    const existing = readJson(packPath) || {};
    const out = {};
    const inMap = new Set();
    for (const [k, zhVal] of Object.entries(keysMap)) {
      inMap.add(k);
      if (lang === 'zh') { out[k] = zhVal; continue; }
      const ov = overrides[lang] ? overrides[lang][k] : undefined;
      const old = Object.prototype.hasOwnProperty.call(existing, k) ? existing[k] : undefined;
      if (ov !== undefined) out[k] = ov;
      else if (old !== undefined) out[k] = old;   /* 缺译但包内有旧值: 保留 */
      else {
        missingTrans++;
        missing.push('缺译\t' + lang + '\t' + k);
        /* 不入包 → 运行期回落 en→zh→键名 */
      }
    }
    /* 包内键不在 keys_map: 原样保留 (追加尾部), 清单供人工确认 */
    for (const k of Object.keys(existing))
      if (!inMap.has(k)) {
        out[k] = existing[k];
        missing.push('包内键不在keys_map\t' + lang + '\t' + k);
      }
    const changed = writeIfChanged(packPath, out);
    console.log((changed ? '已重生成 ' : '无变化   ') + 'languages/' + lang + '.json  (' + Object.keys(out).length + ' 键)');
  }

  /* 源码键缺映射 (含缺译一并落 missing.txt) */
  for (const [k, locs] of sourceKeys)
    if (!(k in keysMap)) {
      missing.push('源码键缺映射\t' + k + '\t' + locs.slice(0, 3).join(' '));
      warn('源码键缺映射: ' + k + '  (' + locs.slice(0, 3).join(' ') + ') — 界面会露键名, 请补 keys_map 与译文');
    }
  if (missingTrans) warn('缺译 ' + missingTrans + ' 条 (明细见 tools/i18n_missing.txt), 相应键不入包走回落链');
  if (dead.length) warn('死译文 ' + dead.length + ' 条 (明细见 tools/i18n_overrides.json.extra)');
  writeReport(MISSING_TXT, missing);
  console.log(missing.length || dead.length ? '完成 (有告警)' : '完成, 无告警');
}

if (process.argv.includes('--bootstrap')) bootstrap();
else generate();
