/*
 * ai_web_ui.cpp — 嵌入式前端: 整套 AI 对话 UI 的单文件 HTML+CSS+JS (NavigateToString 装载)。
 * 内容 = 一个完整 HTML 文档, 按多段相邻宽原始字面量拼接 (本机 cl 宽字面量实测上限约 8KB,
 * 每段控制在 6KB 内; 段界只是拼接缝, 改内容直接在字面量里编辑, 段超限就再切一刀)。
 * 数值 = 参考实现 CSS 值 (1× 基准; DPI 由 WebView2 按父窗自动缩放, 页面缩放经 ZoomFactor),
 * 颜色 = CSS 变量 (运行时由 C++ 推送的皮肤调色注入, 派生规则同旧 AiPal)。
 * 交互: C++→JS 推送 (boot/pal/cfg/convs/msgs/last/usage/status), JS→C++ 命令 (send/stop/
 * close/settings/policy/new/load/del/clearHist/copy/openurl/pallow/pdeny/notify/ready)。
 * 安全面: CSP 关 fetch/XHR/表单/外域; 模型输出永不产生活 HTML (C++ md4c 层转义裁剪);
 * <a> 点击拦截转 openurl 命令; 选区/复制/右键/输入法 = 浏览器原生能力。
 */
#include "ai_assistant.h"

const wchar_t* AiWebUiHtml() {
    return 
           LR"AIWEBUI(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy"
      content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:; connect-src 'none'; form-action 'none'; base-uri 'none'">
<title>AI 助手</title>
<style>
:root{
  --bg:#14171f; --panel:#1b2030; --text:#e8eaf0; --dim:#9aa3b5; --accent:#4f7cff;
  --t3:#5c6577; --hover:#e8eaf01f; --divider:#e8eaf026; --border:#e8eaf02e; --borderStrong:#e8eaf05e;
  --cyan:#0ea5e9; --emerald:#10b981; --amber:#f59e0b; --red:#ef4444; --ok:#22c55e; --userAcc:#f97316;
  --userBubble:#2f2823; --userBubbleBorder:#f9731657; --userText:#f1e6df;
  --hColor:#d3d9f2; --inlineCode:#d3d9f2; --marker:#7f8bd9; --thColor:#dcdff5; --cardBad:#f2a1a1;
  --reasonText:#8a90a2; --reasonStrong:#b3b9c9;
}
*{box-sizing:border-box}
html,body{height:100%;margin:0}
body{background:var(--bg);color:var(--text);overflow:hidden;user-select:none;cursor:default;
     font-family:"Microsoft YaHei UI","Microsoft YaHei","Segoe UI",sans-serif;font-size:12.5px}
.ic{font-family:"Segoe Fluent Icons","Segoe MDL2 Assets",sans-serif;font-style:normal;line-height:1}
#app{display:flex;flex-direction:column;height:100%}

/* ==================== 工具栏 ==================== */
#head{flex:0 0 62px;display:flex;align-items:center;padding:0 20px;border-bottom:1px solid var(--border);position:relative;z-index:5}
#htitle{font-size:20px;font-weight:bold;margin-right:12px;white-space:nowrap}
#hdot{width:7px;height:7px;border-radius:50%;flex:0 0 7px;margin-right:6px}
#hdot.glow{box-shadow:0 0 6px 1px}
#hdot.pulse{animation:dotp 1.2s ease-in-out infinite}
@keyframes dotp{0%,100%{opacity:1}50%{opacity:.35}}
#hstate{font-size:11px;color:var(--dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#hbtns{margin-left:auto;display:flex;gap:8px}
.hbtn{height:30px;border:1px solid var(--border);border-radius:4px;background:var(--panel);
      color:var(--dim);font-size:12px;padding:0 12px;display:flex;align-items:center;gap:6px;white-space:nowrap}
.hbtn .ic{font-size:11px}
.hbtn:hover{background:var(--hover);color:var(--text);border-color:var(--borderStrong)}
.hbtn.icononly{width:35px;justify-content:center;padding:0}

/* ==================== 内嵌接口设置面板 ==================== */
#cfgpanel{flex:0 0 auto;background:color-mix(in srgb,var(--panel) 70%,var(--bg));border-bottom:1px solid var(--border);
          padding:14px 20px 16px;display:none;position:relative;z-index:4}
#cfgpanel.open{display:block}
.cfgrow{display:flex;align-items:center;margin-bottom:10px}
.cfglab{flex:0 0 72px;font-size:12px;color:var(--dim)}
.cfgin{flex:1;height:28px;background:var(--panel);border:1px solid var(--border);border-radius:4px;color:var(--text);
       font-size:12.5px;font-family:inherit;padding:0 8px;outline:none;user-select:text}
.cfgin:hover{border-color:var(--borderStrong)}
.cfgin:focus{border-color:color-mix(in srgb,var(--accent) 55%,var(--border))}
.cfghint{font-size:11px;color:var(--t3);line-height:17px;margin:2px 0 0 84px}
.cfgbtns{display:flex;justify-content:flex-end;gap:8px;margin-top:12px}
#cfgchk{display:flex;align-items:center;gap:8px;height:20px;font-size:12px;color:var(--dim);margin:0 0 10px 84px;cursor:default}
#cfgchk:hover{color:var(--text)}
#cfgchk .box{width:13px;height:13px;border-radius:3px;border:1.2px solid var(--t38f);background:#ffffff26;position:relative;flex:0 0 13px}
#cfgchk.on .box{background:#4f7cff3d;border-color:#4f7cffc2}
#cfgchk.on .box::after{content:"";position:absolute;left:3px;top:1px;width:4px;height:7px;
       border:solid var(--accent);border-width:0 1.6px 1.6px 0;transform:rotate(45deg)}

/* ==================== 主体三列 (跳转条 / 消息流 / 侧栏) ==================== */
#mid{flex:1;display:flex;min-height:0;position:relative}
#threadwrap{flex:1;min-width:0;display:flex}
#jump{flex:0 0 28px;position:relative;display:none}
#jump.has{display:block}
#jump .tick{position:absolute;left:50%;transform:translateX(-50%);width:7px;height:2px;border-radius:1px;background:#ffffff2b}
#jump .tick.act{background:#4f7cffc7;width:11px}
#jump .tick.hot{background:var(--text);width:11px}
#jumppv{position:absolute;width:280px;background:var(--panel);border:1px solid var(--border);border-radius:6px;
        padding:8px 10px;display:none;z-index:30;pointer-events:none}
#jumppv .n{font-size:11px;color:var(--t3);margin-bottom:3px}
#jumppv .q{font-size:12px;line-height:18px;color:var(--text);word-break:break-all;
           display:-webkit-box;-webkit-line-clamp:3;-webkit-box-orient:vertical;overflow:hidden}
#thread{flex:1;min-width:0;overflow-y:auto;overflow-x:hidden}
#thread::-webkit-scrollbar{width:8px}
#thread::-webkit-scrollbar-thumb{background:#9aa3b550;border-radius:4px}
#thread::-webkit-scrollbar-thumb:hover{background:#9aa3b5aa}
#thread-inner{max-width:1600px;margin:0 auto;padding:16px 20px 16px 12px;display:flex;flex-direction:column;gap:16px}

/* ---- 消息行 ---- */
.msg{display:flex;gap:10px;align-items:flex-start}
.msg.user{flex-direction:row-reverse}
.avatar{flex:0 0 26px;width:26px;height:26px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:13px}
.msg.ai .avatar{background:#4f7cff33;color:var(--accent)}
.msg.user .avatar{background:#ffffff1f;color:var(--dim)}
.mmain{min-width:0;max-width:calc(100% - 0px);flex:1;display:flex;flex-direction:column;align-items:flex-start}
.msg.user .mmain{align-items:flex-end}
.mmain.wide{flex:1}

/* ---- 打字三点 ---- */
.typing{padding:14px 14px;border-radius:8px;border:1px solid var(--border);background:var(--panel);
        display:flex;gap:4px;align-items:center}
.typing i{width:5px;height:5px;border-radius:50%;background:var(--t3);animation:tb 0.9s ease-in-out infinite}
.typing i:nth-child(2){animation-delay:.15s}
.typing i:nth-child(3){animation-delay:.3s}
@keyframes tb{0%,100%{transform:translateY(0);opacity:.35}30%,60%{transform:translateY(-3px);opacity:1}}

/* ---- 气泡与正文 (md) ---- */
.bubble{padding:9px 12px;border-radius:8px;border:1px solid var(--border);background:var(--panel);
)AIWEBUI"
           LR"AIWEBUI(        user-select:text;cursor:auto;max-width:100%;min-width:44px}
.b-user{background:var(--userBubble);border-color:var(--userBubbleBorder);color:var(--userText)}
.b-err{border-color:#ef44446b;background:color-mix(in srgb,var(--red) 12%,var(--panel))}
.bubble p{margin:0 0 6px 0}
.bubble p:last-child{margin-bottom:0}
.bubble ul,.bubble ol{margin:4px 0;padding-left:18px}
.bubble ul{list-style-type:disc}.bubble ul ul{list-style-type:circle}.bubble ul ul ul{list-style-type:square}
.bubble ol{list-style-type:decimal}
.bubble li{margin:2px 0}
.bubble li::marker{color:var(--marker)}
.bubble strong{font-weight:bold;color:var(--text)}
.bubble em{color:var(--text)}
.bubble s{color:var(--t3);text-decoration:line-through}
.bubble a{color:var(--cyan);text-decoration:none;border-bottom:1px solid #0ea5e966;cursor:pointer}
.bubble code{padding:1px 4px;border-radius:3px;background:#4f7cff21;color:var(--inlineCode);
             font-family:Consolas,"Courier New",monospace;font-size:11.5px}
.bubble h1,.bubble h2,.bubble h3,.bubble h4{margin:10px 0 6px 0;font-weight:bold;line-height:1.4}
.bubble h1,.bubble h2,.bubble h3{color:var(--hColor)}
.bubble h4{color:var(--text)}
.bubble h1{font-size:17px;border-bottom:1px solid var(--divider);padding-bottom:4px}
.bubble h2{font-size:15.5px}
.bubble h3{font-size:14px}
.bubble h4{font-size:13px}
.bubble h1:first-child,.bubble h2:first-child,.bubble h3:first-child,.bubble h4:first-child{margin-top:0}
.bubble p.sub{margin:9px 0 5px 0;color:var(--hColor);font-weight:bold}
.bubble blockquote{margin:6px 0;padding:6px 10px 6px 10px;border-left:3px solid #4f7cff8c;
                   border-radius:0 4px 4px 0;background:#4f7cff12;color:var(--dim)}
.bubble blockquote p{margin:0}
.bubble hr{display:block;height:1px;margin:10px 0;border:none;background:var(--divider)}
.bubble .tblwrap{margin:6px 0;border:1px solid var(--border);border-radius:5px;overflow:hidden}
.bubble table{border-collapse:collapse;width:100%;font-size:12px;line-height:1.5}
.bubble th,.bubble td{padding:5px 9px;border-bottom:1px solid var(--divider);color:var(--text);text-align:left}
.bubble th{background:#4f7cff1f;color:var(--thColor);font-weight:bold;white-space:nowrap}
.bubble .ai-code{margin:6px 0;border:1px solid var(--border);border-radius:6px;background:var(--bg)}
.bubble .codehead{padding:5px 8px;border-bottom:1px solid var(--divider);background:var(--hover);overflow:hidden}
.bubble .codelang{font-family:Consolas,"Courier New",monospace;font-size:10.5px;color:var(--t3)}
.bubble .codecopy{float:right;padding:3px 8px;border:1px solid var(--border);border-radius:4px;
                  color:var(--dim);font-size:11px;cursor:pointer}
.bubble .codecopy:hover{color:var(--text);border-color:var(--borderStrong)}
.bubble .codecopy.copied{color:var(--emerald);border-color:#10b98173}
.bubble .ai-code pre{margin:0;padding:8px 10px;white-space:pre;overflow-x:auto;
                     font-family:Consolas,"Courier New",monospace;font-size:11.5px;line-height:1.6;
                     background:transparent;color:var(--text)}
.bubble .ai-code pre code{padding:0;background:transparent;color:var(--text);font-size:11.5px}
.bubble .task{display:block;list-style:none}
.bubble .tbox{display:inline-block;width:12px;height:12px;margin:0 6px 0 0;border:1px solid var(--t3b2);
              border-radius:3px;vertical-align:middle;position:relative}
.bubble .tbox.done{background:#10b9813d;border-color:#10b981c2}
.bubble .tbox.done::after{content:"";position:absolute;left:3px;top:0.5px;width:3.5px;height:6.5px;
       border:solid var(--emerald);border-width:0 1.5px 1.5px 0;transform:rotate(45deg)}
.bubble .tdone{color:var(--t3);text-decoration:line-through}

/* ---- 推理块 ---- */
.reason{margin:0 0 8px 0;border:1px solid #4f7cff52;border-radius:6px;background:#4f7cff0d;width:100%}
.rhead{padding:6px 10px;color:var(--dim);font-size:11.5px;cursor:pointer}
.rhead:hover{color:var(--reasonStrong)}
.rhead .mark{margin-right:8px}
.rhead .arr{margin-left:8px;font-size:10px}
.rbody{padding:2px 10px 8px 10px;border-top:1px solid #4f7cff33;color:var(--reasonText);
       font-size:12px;line-height:20px;white-space:pre-wrap;user-select:text;cursor:auto;word-break:break-word}
.rbody code{background:#4f7cff1a;color:#9aa3d9;font-size:11px;padding:1px 4px;border-radius:3px}

/* ---- 工具卡片 ---- */
.step{margin:0 0 6px 0;border:1px solid var(--border);border-radius:5px;background:#ffffff0d;width:100%}
.step:last-child{margin-bottom:0}
.step.failed{border-color:#ef444461}
.shead{padding:5px 8px;font-size:11px;overflow:hidden;cursor:pointer}
.sbadge{margin-right:6px;padding:0 5px 1px 5px;border-radius:3px;background:#4f7cff29;
        color:var(--hColor);font-size:10px}
.scmd{color:var(--dim);font-family:Consolas,"Courier New",monospace;font-size:11px;
      word-break:break-all}
.sst{float:right;color:var(--t3);margin-left:8px}
.sst.bad{color:var(--cardBad)}
.sout{border-top:1px solid var(--border);background:#ffffff0a;padding:6px 8px;color:var(--dim);
      font-size:11px;font-family:Consolas,"Courier New",monospace;white-space:pre-wrap;word-break:break-all;
      user-select:text;cursor:auto}
.sask{padding:6px 8px 8px 8px;border-top:1px solid var(--border);color:var(--dim);font-size:11.5px;line-height:18px}
.abtn{display:inline-block;margin:8px 8px 0 0;padding:4px 10px;border:1px solid var(--border);border-radius:4px;
      color:var(--dim);font-size:11px;cursor:pointer}
.abtn:hover{color:var(--text);border-color:var(--borderStrong)}
.abtn.primary{background:var(--accent);border-color:var(--accent);color:#ffffff}
.abtn.primary:hover{background:color-mix(in srgb,var(--accent) 84%,#ffffff)}

/* ---- 跳转高亮 (1.5s 渐隐描边) ---- */
@keyframes flashb{0%{outline-color:#4f7cff8c}100%{outline-color:transparent}}
.msg.flash .mmain>*{outline:2px solid transparent;outline-offset:2px;border-radius:9px;animation:flashb 1.5s forwards}

/* ---- 空态 ---- */
#empty{flex:1;display:none;flex-direction:column;align-items:center;justify-content:center;text-align:center;padding:20px}
)AIWEBUI"
           LR"AIWEBUI(#empty.has{display:flex}
#empty .eic{font-size:32px;color:#4f7cff9e;margin-bottom:26px}
#empty .et{font-size:14px;margin-bottom:14px}
#empty .ed{font-size:12px;color:var(--dim);line-height:21px;max-width:460px;margin-bottom:16px}
#empty .esuggs{display:flex;flex-wrap:wrap;gap:8px;justify-content:center}
.esugg{height:30px;padding:0 11px;border-radius:15px;border:1px solid var(--border);background:var(--panel);
       color:var(--dim);font-size:12px;display:flex;align-items:center;cursor:pointer}
.esugg:hover{background:var(--hover);color:var(--text);border-color:color-mix(in srgb,var(--accent) 45%,var(--border))}

/* ==================== 输入区 ==================== */
#composer{flex:0 0 auto;padding:12px 20px 14px}
#cbox{max-width:1600px;margin:0 auto}
#crow{position:relative;background:var(--panel);border:1px solid var(--border);border-radius:8px}
#crow.focus{border-color:color-mix(in srgb,var(--accent) 55%,var(--border))}
#inputT{display:block;width:100%;min-height:38px;max-height:120px;padding:8px 46px 8px 12px;
        background:transparent;border:none;outline:none;resize:none;color:var(--text);
        font-family:inherit;font-size:12.5px;line-height:20px;overflow-y:auto;user-select:text}
#inputT::placeholder{color:var(--t3)}
#inputT::-webkit-scrollbar{width:5px}
#inputT::-webkit-scrollbar-thumb{background:#9aa3b55a;border-radius:2.5px}
#sendB{position:absolute;right:8px;bottom:8px;width:28px;height:28px;border-radius:50%;border:none;
       display:flex;align-items:center;justify-content:center;font-size:13px;color:#fff;background:var(--accent);cursor:pointer}
#sendB.empty{background:var(--hover);color:var(--t3);cursor:default}
#sendB.stop{background:var(--red)}
#sendB:not(.empty):hover{background:color-mix(in srgb,var(--accent) 84%,#ffffff)}
#hintrow{display:flex;align-items:center;margin-top:6px;max-width:1600px;margin-left:auto;margin-right:auto}
#hintkey{font-size:11px;color:#5c6577cc;margin-right:auto}
#usagebtn{display:flex;align-items:center;gap:7px;height:22px;padding:0 6px;border-radius:5px;cursor:pointer}
#usagebtn:hover,#usagebtn.open{background:var(--hover)}
#ubar{width:34px;height:3px;border-radius:2px;background:#5c657742;position:relative;overflow:hidden}
#ubarf{position:absolute;left:0;top:0;bottom:0;background:#4f7cffb3;border-radius:2px}
#ubarf.warn{background:var(--amber)}
#ubrieftxt{font-size:11px;color:var(--dim)}
#ubrieftxt.warn{color:color-mix(in srgb,var(--amber) 85%,var(--dim))}
#uchevr{font-size:8px;color:var(--t3)}
#policy{display:flex;align-items:center;border:1px solid var(--border);border-radius:5px;height:22px;margin:0 12px 0 0}
#policy .pic{font-size:11px;color:var(--t3);padding:0 5px 0 5px}
#policy .pseg{font-size:11px;color:var(--t3);padding:2px 7px;cursor:pointer;display:flex;align-items:center;height:20px}
#policy .pseg+.pseg{border-left:1px solid var(--border)}
#policy .pseg:hover{color:var(--text);background:var(--hover)}
#policy .pseg.act{color:var(--hColor);background:#4f7cff38}
#policy .pseg.act.dis{color:var(--dim);background:var(--hover)}
#policy .pseg.act.allow{color:color-mix(in srgb,var(--amber) 82%,var(--text));background:#f59e0b38}

/* ---- 用量详情浮层 ---- */
#usagepop{position:absolute;right:20px;width:226px;background:var(--panel);border:1px solid var(--border);
          border-radius:6px;padding:8px 0 8px 0;z-index:40;display:none;box-shadow:0 6px 24px #00000059}
#usagepop.open{display:block}
#usagepop .urow{display:flex;align-items:center;height:20px;padding:0 10px;font-size:11.5px}
#usagepop .urow+.urow.data{border-top:1px solid var(--divider)}
#usagepop .urow .k{color:var(--t3)}
#usagepop .urow .v{margin-left:auto;color:var(--text)}
#usagepop .urow .v.good{color:color-mix(in srgb,var(--emerald) 80%,var(--text))}
#usagepop .urow .v.warn{color:var(--amber)}
#usagepop .urow .v.empty{color:var(--t3)}
#usagepop .ugrp{font-size:10.5px;font-weight:bold;color:var(--dim);padding-top:3px}
#usagepop .unote{font-size:10.5px;color:var(--t3);padding:2px 10px 0 10px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}

/* ==================== 历史侧栏 ==================== */
#side{flex:0 0 232px;background:color-mix(in srgb,var(--panel) 55%,var(--bg));border-left:1px solid var(--border);
      display:none;flex-direction:column;min-height:0}
#side.has{display:flex}
#sidehead{flex:0 0 44px;display:flex;align-items:center;padding:0 16px}
#sidehead .st{font-size:12px;color:var(--dim);margin-right:auto}
#clearb{height:24px;padding:0 8px;border-radius:4px;border:1px solid var(--border);background:var(--panel);
        color:var(--dim);font-size:11px;cursor:pointer}
#clearb:hover{background:var(--hover);color:var(--text)}
#clearb.arm{background:#e81123;border-color:#e81123;color:#fff}
#sidelist{flex:1;overflow-y:auto;padding:4px}
#sidelist::-webkit-scrollbar{width:8px}
#sidelist::-webkit-scrollbar-thumb{background:#9aa3b550;border-radius:4px}
.srow{position:relative;height:44px;border-radius:6px;padding:0 34px 0 10px;margin-bottom:4px;cursor:pointer}
.srow:hover{background:#4f7cff1a}
.srow.act{background:#4f7cff12;border:1px solid #4f7cff59}
.srow .tt{font-size:12px;color:var(--text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:6px}
.srow .tm{font-size:11px;color:var(--t3);margin-top:2px;display:flex;align-items:center;gap:6px}
.srow .pdot{width:5px;height:5px;border-radius:50%;background:var(--accent);animation:dotp 1s ease-in-out infinite}
.srow .del{position:absolute;right:6px;top:11px;width:22px;height:22px;border-radius:4px;border:none;background:transparent;
           color:var(--t3);font-size:12px;display:flex;align-items:center;justify-content:center;cursor:pointer}
.srow .del:hover{background:#ef444424;color:var(--red)}
.srow .del.arm{background:#e81123;color:#fff}
#sideempty{text-align:center;color:var(--t3);font-size:12px;margin-top:18px}
#sidescrim{position:absolute;inset:0 232px 0 0;background:#00000028;z-index:20;display:none}
#sidescrim.has{display:block}
#side.float{position:absolute;right:0;top:0;bottom:0;width:262px;z-index:25;box-shadow:-8px 0 24px #00000045}
)AIWEBUI"
           LR"AIWEBUI(</style>
</head>
<body>
<div id="app">
  <div id="head">
    <div id="htitle">AI 助手</div>
    <div id="hdot"></div>
    <div id="hstate"></div>
    <div id="hbtns">
      <button class="hbtn" id="b-set"><span class="ic">&#xE713;</span>接口设置</button>
      <button class="hbtn" id="b-hist"><span class="ic">&#xE81C;</span>历史对话</button>
      <button class="hbtn icononly" id="b-new" title="新对话"><span class="ic">&#xE72C;</span></button>
      <button class="hbtn icononly" id="b-close" title="关闭"><span class="ic">&#xE8BB;</span></button>
    </div>
  </div>
  <div id="cfgpanel">
    <div class="cfgrow"><div class="cfglab">接口地址</div><input class="cfgin" id="f-url" placeholder="https://api.deepseek.com"></div>
    <div class="cfgrow"><div class="cfglab">API 密钥</div><input class="cfgin" id="f-key" type="password" placeholder="sk-..."></div>
    <div class="cfgrow"><div class="cfglab">模型</div><input class="cfgin" id="f-model" placeholder="deepseek-flash"></div>
    <div id="cfgchk"><span class="box"></span>深度思考 (reasoning.effort=high)</div>
    <div class="cfghint">密钥经混淆后只存在本机配置文件里，不会上传到别处；接口需兼容 OpenAI Responses 协议 (/responses)。</div>
    <div class="cfgbtns">
      <button class="hbtn" id="b-cancel">取消</button>
      <button class="hbtn" id="b-save" style="background:var(--accent);border-color:var(--accent);color:#fff">保存</button>
    </div>
  </div>
  <div id="mid">
    <div id="threadwrap">
      <div id="jump"><div id="jumppv"><div class="n"></div><div class="q"></div></div></div>
      <div id="thread"><div id="thread-inner"></div></div>
      <div id="empty">
        <div class="eic ic">&#xE99A;</div>
        <div class="et">用对话来查找和整理文件</div>
        <div class="ed">我可以读取索引库的全部实时数据（文件名、路径、大小、时间、分类），直接执行搜索并打开文件；每一步工具调用都会以卡片展示。</div>
        <div class="esuggs" id="esuggs"></div>
      </div>
    </div>
    <div id="sidescrim"></div>
    <div id="side">
      <div id="sidehead"><div class="st">历史对话</div><button id="clearb">清空记录</button></div>
      <div id="sidelist"></div>
    </div>
  </div>
  <div id="composer">
    <div id="cbox">
      <div id="crow">
        <textarea id="inputT" rows="1" placeholder="描述你的目标，例如：找出一周内修改过的文档"></textarea>
        <button id="sendB"><span class="ic" id="sendIc">&#xE74A;</span></button>
      </div>
      <div id="hintrow">
        <div id="hintkey">Enter 发送，Shift + Enter 换行</div>
        <div id="policy"><span class="pic ic">&#xE756;</span></div>
        <div id="usagebtn" title="用量详情">
          <div id="ubar"><div id="ubarf"></div></div>
          <div id="ubrieftxt">剩余 100% · --</div>
          <div id="uchevr" class="ic">&#xE70D;</div>
        </div>
      </div>
    </div>
    <div id="usagepop"></div>
  </div>
</div>
<script>
'use strict';
/* ==================== 工具 ==================== */
const $ = id => document.getElementById(id);
/* JS→C++ 命令唯一通道。必须发对象本体 (不能 JSON.stringify): C++ 端取
   WebMessageAsJson 后按 t==5 判对象, 发字符串会被当成 JSON 串拒收 */
function post(o){try{chrome.webview.postMessage(o);}catch(e){}}
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function hex2rgba(h,a){const n=parseInt(h.slice(1,7),16);const r=(n>>16)&255,g=(n>>8)&255,b=n&255;return 'rgba('+r+','+g+','+b+','+a+')';}
function mix(h1,h2,t){ /* 线性混色 (C++ MixCol 同式), t=0 → h1 */
  const p=h=>[parseInt(h.slice(1,3),16),parseInt(h.slice(3,5),16),parseInt(h.slice(5,7),16)];
  const a=p(h1),b=p(h2);
  const c=a.map((v,i)=>Math.round(v*(1-t)+b[i]*t));
  return '#'+c.map(v=>('0'+v.toString(16)).slice(-2)).join('');
}
function withA(h,a){ /* 8 位 hex (#rrggbbaa) */
  const al=('0'+Math.round(a*255).toString(16)).slice(-2);
  return h+al;
}
function fmtTokens(n){n=Math.max(0,n||0);if(n<1000)return ''+n;if(n<10000)return (n/1000).toFixed(2)+'K';
  if(n<1000000)return (n/1000).toFixed(1)+'K';return (n/1000000).toFixed(2)+'M';}
function ctxWindowFor(model){model=(model||'').toLowerCase();
  const T=[['deepseek',1000000],['gemini',1000000],['gpt-4.1',1047576],['gpt-4o',128000],['gpt-4-turbo',128000],
    ['gpt-3.5',16385],['claude',200000],['qwen',131072],['glm',131072],['moonshot',131072],['kimi',131072],
    ['llama',131072],['mistral',131072]];
  for(const [k,w] of T) if(model.indexOf(k)>=0) return w; return 128000;}
function histTime(t){if(!t)return '';const d=new Date(t*1000),now=new Date();
  const p=n=>('0'+n).slice(-2);
  if(d.getFullYear()===now.getFullYear()&&d.getMonth()===now.getMonth()&&d.getDate()===now.getDate())
    return p(d.getHours())+':'+p(d.getMinutes());
  return p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes());}
const SUGGS=['现在哪些大文件占用空间最多','找出一周内修改过的文档并列个清单','看看当前的文件分类和重复文件'];

/* ==================== 状态 ==================== */
const S = {
  cfg:{url:'',model:'',hasKey:false,reasoning:false,policy:2},
  pal:null, convs:[], msgs:[], cur:0,
  sending:false, net:0, phase:0,
  usage:{has:false,up:0,uo:0,ut:0,uch:0,lp:0,lc:0,tps:0},
  sideOpen:false, usageOpen:false, cfgOpen:false,
  dUrl:'',dKey:'',dModel:'',dReason:false,
  armRow:-1, armTimer:0, clearArm:false, clearTimer:0,
  openSteps:{},     /* 展开的工具卡片样本: convId+':'+groupIdx → true */
  reasonOpen:{},    /* 手动展开的推理块: msgIdx → true (流式自动展开之外的覆盖) */
  flashMi:-1, flashTimer:0,
  copiedTimer:0,    /* 代码复制按钮 "已复制" 复位 */
  jumpHover:-1,
};

/* ==================== 调色注入 ==================== */
function applyPal(p){
  if(!p) return;
  const r=document.documentElement.style;
  for(const k in p) r.setProperty('--'+k, p[k]);
  r.setProperty('--userBubble', mix(p.userAcc,p.panel,0.82));
  r.setProperty('--userBubbleBorder', withA(p.userAcc,0.34));
  r.setProperty('--userText', mix(p.userAcc,p.text,0.94));
  r.setProperty('--hColor', mix(p.accent,p.text,0.2));
  r.setProperty('--inlineCode', mix(p.accent,p.text,0.2));
  r.setProperty('--marker', mix(p.accent,p.t3,0.38));
  r.setProperty('--thColor', mix(p.accent,p.text,0.28));
  r.setProperty('--cardBad', mix(p.red,p.text,0.22));
  r.setProperty('--reasonText', mix(p.dim,p.t3,0.38));
  r.setProperty('--reasonStrong', mix(p.dim,p.text,0.42));
}

/* ==================== 工具栏 ==================== */
function renderHead(){
  const complete = S.cfg.url && S.cfg.model && S.cfg.hasKey;
)AIWEBUI"
           LR"AIWEBUI(  const dot=$('hdot'), st=$('hstate');
  let color, text, glow=false, pulse=false;
  if(S.sending){color=S.pal?S.pal.accent:'#4f7cff'; text='正在与 '+S.cfg.model+' 对话'; pulse=true;}
  else if(!complete){color=S.pal?S.pal.amber:'#f59e0b'; text='未配置接口';}
  else if(S.net===2){color=S.pal?S.pal.red:'#ef4444'; text='上次请求失败';}
  else {color=S.pal?S.pal.ok:'#22c55e'; text='已连接 '+S.cfg.model; glow=true;}
  dot.style.background=color; dot.className=''; if(glow){dot.classList.add('glow');dot.style.boxShadow='0 0 6px 1px '+withA(color,0.5);}
  if(pulse)dot.classList.add('pulse');
  st.textContent=text;
}

/* ==================== 接口设置面板 ==================== */
function renderCfg(){
  $('cfgpanel').classList.toggle('open', S.cfgOpen);
  if(S.cfgOpen){
    $('f-url').value=S.dUrl; $('f-key').value=S.dKey; $('f-model').value=S.dModel;
    $('cfgchk').classList.toggle('on', S.dReason);
  }
}
function cfgToggle(open){
  S.cfgOpen=open;
  if(open){S.dUrl=S.cfg.url;S.dKey='';S.dModel=S.cfg.model;S.dReason=S.cfg.reasoning;
    setTimeout(()=>{try{$('f-url').focus();}catch(e){}},0);}
  renderCfg();
}

/* ==================== 消息流 ==================== */
function threadEl(){return $('thread');}
function atBottom(){const t=threadEl();return t.scrollHeight-t.scrollTop-t.clientHeight<24;}
function scrollBottom(){const t=threadEl();t.scrollTop=t.scrollHeight;}

function lastIsEmptyAssistant(){
  if(!S.msgs.length) return false;
  const m=S.msgs[S.msgs.length-1];
  return S.sending && S.phase===0 && m.r===1 && m.empty===true;
}
function msgInnerHtml(m,mi){
  let inner='';
  if(m.reason){
    const autoOpen = S.sending && mi===S.msgs.length-1 && S.phase===0;
    const open = S.reasonOpen[mi]!==undefined ? S.reasonOpen[mi] : autoOpen;
    inner+='<div class="reason"><div class="rhead" data-act="reason" data-mi="'+mi+'">'
      +'<span class="mark">'+(autoOpen?'◐':'●')+'</span><span class="rlabel">'
      +(autoOpen?'正在深度思考…':'已深度思考（推理过程）')+'</span><span class="arr">'+(open?'▼':'▶')+'</span></div>';
    if(open) inner+='<div class="rbody">'+esc(m.reason)+'</div>';
    inner+='</div>';
  }
  inner+=m.html||'<div class="bubble">&nbsp;</div>';
  return inner;
}
function msgRowHtml(m,mi){
  const user=m.r===0;
  let cls='msg '+(user?'user':'ai');
  if(S.flashMi===mi) cls+=' flash';
  return '<div class="'+cls+'" data-mi="'+mi+'">'
    +'<div class="avatar"><span class="ic">'+(user?'&#xE77B;':'&#xE99A;')+'</span></div>'
    +'<div class="mmain wide">'+(lastIsEmptyAssistant()&&mi===S.msgs.length-1
        ? '<div class="typing"><i></i><i></i><i></i></div>'
        : msgInnerHtml(m,mi))
    +'</div></div>';
}
function renderThread(keepScroll){
  const t=threadEl();
  const stick=keepScroll?atBottom():true;
  const inner=$('thread-inner');
  const empty=$('empty');
  const showEmpty = !S.msgs.length && !S.sending;
  empty.classList.toggle('has', showEmpty);
  $('jump').classList.toggle('has', roundCount()>=2 && !showEmpty);
  if(showEmpty){ inner.innerHTML=''; renderJump(); return; }
  let html='';
  for(let i=0;i<S.msgs.length;i++) html+=msgRowHtml(S.msgs[i],i);
  inner.innerHTML=html;
  applyOpenSteps();
  if(stick) scrollBottom();
  renderJump();
}
/* 样本列表展开态回放 (msgs 全量重推后 JS 自持的展开态不丢) */
function applyOpenSteps(){
  document.querySelectorAll('#thread-inner .step').forEach(card=>{
    const gi=+card.getAttribute('data-gi');
    const body=card.querySelector('.ssamples');
    if(body) body.style.display=S.openSteps[S.cur+':'+gi]?'':'none';
  });
}
function applyLast(){
  /* 流式增量: 只换最后一行 (吸底跟随) */
  if(!S.msgs.length) return;
  const t=threadEl();
  const stick=atBottom();
  const inner=$('thread-inner');
  const mi=S.msgs.length-1;
  const old=inner.querySelector('.msg[data-mi="'+mi+'"]');
  const frag=document.createElement('div');
  frag.innerHTML=msgRowHtml(S.msgs[mi],mi);
  const row=frag.firstChild;
  if(old) inner.replaceChild(row,old); else inner.appendChild(row);
  if(S.flashMi===mi){row.classList.add('flash');}
  if(stick) scrollBottom();
}
function roundCount(){let n=0;for(const m of S.msgs) if(m.r===0) n++; return n;}

/* ---- 轮次跳转条 ---- */
function viewTopOf(el){
  const t=threadEl().getBoundingClientRect();
  return el.getBoundingClientRect().top-t.top;
}
function renderJump(){
  const j=$('jump'), pv=$('jumppv');
  const rounds=roundCount();
  const t=threadEl();
  const viewH=t.clientHeight;
  let ticks='';
  if(rounds>=2 && viewH>=80){
    const spacing=12, total=rounds*spacing;
    const cy0=viewH/2-total/2+spacing/2;
    /* 活动轮 = 视口中线之前最近的用户消息 (视觉行几何) */
    let active=0, r=-1;
    for(let i=0;i<S.msgs.length;i++){
      const el=innerAt(i); if(!el) continue;
      if(viewTopOf(el)<=viewH/2 && S.msgs[i].r===0){ r++; active=Math.max(0,r); }
    }
    for(let k=0;k<rounds;k++){
      const y=cy0+k*spacing;
      const hot=S.jumpHover===k, act=k===active;
      ticks+='<div class="tick'+(act?' act':'')+(hot?' hot':'')+'" data-jump="'+k+'" style="top:'+(y-1)+'px"></div>';
    }
  }
  /* 保留 pv 节点: 重建 ticks 前摘出再插回 */
  if(pv.parentNode===j) j.removeChild(pv);
  j.innerHTML=ticks;
  j.appendChild(pv);
  if(S.jumpHover>=0) renderJumpPv();
}
function innerAt(i){const el=$('thread-inner'); return el?el.children[i]:null;}
function msgRoundIdx(mi){let r=-1;for(let i=0;i<=mi&&i<S.msgs.length;i++) if(S.msgs[i].r===0) r++; return r;}
function renderJumpPv(){
  const j=$('jump'), pv=$('jumppv');
  if(S.jumpHover<0 || S.jumpHover>=roundCount()){pv.style.display='none';return;}
  let seen=-1, mi=-1;
  for(let i=0;i<S.msgs.length;i++){ if(S.msgs[i].r===0) seen++; if(seen===S.jumpHover){mi=i;break;} }
  if(mi<0){pv.style.display='none';return;}
  const q=(S.msgs[mi].q||'').slice(0,120);
  pv.querySelector('.n').textContent='第 '+(S.jumpHover+1)+' 轮';
  pv.querySelector('.q').textContent=q;
  const el=innerAt(mi);
  const viewH=threadEl().clientHeight;
  const yInThread = el ? viewTopOf(el) : viewH/2;
  let py=yInThread-pv.offsetHeight/2;
  py=Math.max(4,Math.min(py,viewH-pv.offsetHeight-4));
  pv.style.top=py+'px';
  pv.style.display='block';
)AIWEBUI"
           LR"AIWEBUI(}

/* ---- 消息 HTML 内的点击 (事件委托) ---- */
function bindThread(){
  const inner=$('thread-inner');
  inner.addEventListener('click',e=>{
    const a=e.target.closest('a');
    if(a){e.preventDefault();const href=a.getAttribute('href')||'';
      if(/^https?:/i.test(href)) post({c:'openurl',href}); return;}
    const cp=e.target.closest('.codecopy');
    if(cp){const box=cp.closest('.ai-code'); const code=box?decodeHtml(box.getAttribute('data-code')||''):'';
      post({c:'copy',text:code});
      cp.classList.add('copied');cp.textContent='已复制';
      clearTimeout(S.copiedTimer);S.copiedTimer=setTimeout(()=>{ // 2s 复位该按钮
        document.querySelectorAll('.codecopy.copied').forEach(b=>{b.classList.remove('copied');b.textContent='复制';});
      },2000); return;}
    const ab=e.target.closest('.abtn');
    if(ab){const act=ab.getAttribute('data-act');
      if(act==='authallow')post({c:'pallow'});
      else if(act==='authdeny')post({c:'pdeny'});
      return;}
    const st=e.target.closest('.step .shead');
    if(st){const card=st.closest('.step');const gi=+card.getAttribute('data-gi');const key=S.cur+':'+gi;
      S.openSteps[key]=!S.openSteps[key];
      const body=card.querySelector('.ssamples');
      if(body)body.style.display=S.openSteps[key]?'':'none';
      return;}
    const rh=e.target.closest('.rhead');
    if(rh){const mi=+rh.getAttribute('data-mi');
      const cur=S.reasonOpen[mi]!==undefined?S.reasonOpen[mi]
        :(S.sending&&mi===S.msgs.length-1&&S.phase===0);
      S.reasonOpen[mi]=!cur;
      const m=S.msgs[mi];
      const main=rh.closest('.mmain');
      if(main){const f=document.createElement('div');f.innerHTML=msgInnerHtml(m,mi);main.innerHTML=f.innerHTML;}
      return;}
  });
  /* 悬停跳转刻度 */
  const j=$('jump');
  j.addEventListener('mousemove',e=>{
    const tk=e.target.closest('.tick');
    const hover=tk?+tk.getAttribute('data-jump'):-1;
    if(hover!==S.jumpHover){S.jumpHover=hover;renderJump();}
  });
  j.addEventListener('mouseleave',()=>{if(S.jumpHover!==-1){S.jumpHover=-1;renderJump();}});
  j.addEventListener('click',e=>{
    const tk=e.target.closest('.tick'); if(!tk)return;
    const k=+tk.getAttribute('data-jump');
    let seen=-1,mi=-1;
    for(let i=0;i<S.msgs.length;i++){if(S.msgs[i].r===0)seen++;if(seen===k){mi=i;break;}}
    if(mi<0)return;
    const el=innerAt(mi);
    if(el){threadEl().scrollTop=Math.max(0,viewTopOf(el)+threadEl().scrollTop-12);}
    S.flashMi=mi; clearTimeout(S.flashTimer);
    S.flashTimer=setTimeout(()=>{S.flashMi=-1;
      document.querySelectorAll('.msg.flash').forEach(x=>x.classList.remove('flash'));},1500);
    document.querySelectorAll('.msg[data-mi="'+mi+'"]').forEach(x=>x.classList.add('flash'));
    S.jumpHover=-1; renderJump();
  });
}
function decodeHtml(s){const t=document.createElement('textarea');t.innerHTML=s;return t.value;}

/* ==================== 历史侧栏 ==================== */
function dockedSide(){return document.getElementById('mid').clientWidth>=760;}
function renderSide(){
  const side=$('side'), scrim=$('sidescrim');
  side.classList.toggle('has', S.sideOpen);
  const dock=dockedSide();
  side.classList.toggle('float', S.sideOpen&&!dock);
  scrim.classList.toggle('has', S.sideOpen&&!dock);
  if(!S.sideOpen) return;
  $('clearb').textContent=S.clearArm?'确认清空?':'清空记录';
  $('clearb').classList.toggle('arm', S.clearArm);
  let html='';
  if(!S.convs.length) html='<div id="sideempty">暂无历史对话</div>';
  for(const c of S.convs){
    const act=c.id===S.cur;
    const arm=S.armRow===c.id;
    html+='<div class="srow'+(act?' act':'')+'" data-id="'+c.id+'">'
      +'<div class="tt">'+(c.title?esc(c.title):'未命名对话')+'</div>'
      +'<div class="tm">'+((act&&S.sending)?'<span class="pdot"></span>':'')+esc(histTime(c.t))+'</div>'
      +'<button class="del'+(arm?' arm':'')+'" data-del="'+c.id+'" title="删除"><span class="ic">&#xE74D;</span></button>'
      +'</div>';
  }
  $('sidelist').innerHTML=html;
}
function sideToggle(open){S.sideOpen=open; if(!open){S.clearArm=false;clearTimeout(S.clearTimer);S.armRow=-1;} renderSide();}

/* ==================== 输入区 ==================== */
const POLICY=['禁用','只读','询问','允许'];
function renderComposer(){
  const ta=$('inputT');
  const empty=!ta.value && !S.sending;
  $('sendB').className=S.sending?'stop':(empty?'empty':'');
  $('sendB').title=S.sending?'停止生成':'发送';
  $('sendIc').innerHTML=S.sending?'&#xE71A;':'&#xE74A;';
  let segs='<span class="pic ic">&#xE756;</span>';
  for(let i=0;i<4;i++){
    const act=S.cfg.policy===i;
    let extra=act?(i===3?' allow':i===0?' dis':''):'';
    segs+='<span class="pseg'+(act?' act'+extra:'')+'" data-policy="'+i+'">'+POLICY[i]+'</span>';
  }
  $('policy').innerHTML=segs;
  renderUsage();
}
function autoSize(){
  const ta=$('inputT');
  ta.style.height='auto';
  ta.style.height=Math.min(120,Math.max(38,ta.scrollHeight))+'px';
  ta.style.overflowY=ta.scrollHeight>120?'auto':'hidden';
}
function renderUsage(){
  const u=S.usage;
  const win=ctxWindowFor(S.cfg.model);
  const used=u.has?(u.lp+u.lc):0;
  const ratio=win>0?Math.min(1,Math.max(0,used/win)):0;
  const warn=(1-ratio)<=0.1;
  const f=$('ubarf');
  f.style.width=Math.max(ratio*100, used>0?8:0)+'%';
  f.className=warn?'warn':'';
  const brief=$('ubrieftxt');
  brief.textContent='剩余 '+Math.round((1-ratio)*100)+'% · '+(u.has&&used>0?fmtTokens(used):'--');
  brief.className=warn?'warn':'';
  renderUsagePop();
}
function renderUsagePop(){
  const pop=$('usagepop');
  pop.classList.toggle('open',S.usageOpen);
  if(!S.usageOpen)return;
  const u=S.usage;
  const win=ctxWindowFor(S.cfg.model);
  const used=u.has?(u.lp+u.lc):0;
  const free=Math.max(0,win-used), freeRatio=win>0?free/win:1;
  const has=u.has;
  const cachePct=(has&&u.up)?Math.round(u.uch/u.up*100)+'%':'--';
  const speed=u.tps>0?Math.round(u.tps*10)/10+' tok/s':'--';
  const row=(k,v,cls)=>'<div class="urow data"><span class="k">'+k+'</span><span class="v '+(cls||'')+'">'+v+'</span></div>';
  const grp=k=>'<div class="urow ugrp">'+k+'</div>';
)AIWEBUI"
           LR"AIWEBUI(  let html=grp('本次对话累计')
    +row('输入',has?fmtTokens(u.up):'--',has&&u.up?'':'empty')
    +row('输出',has?fmtTokens(u.uo):'--',has&&u.uo?'':'empty')
    +row('合计',has?fmtTokens(u.ut):'--',has&&u.ut?'':'empty')
    +row('缓存命中',cachePct,(has&&u.up&&u.uch*2>=u.up)?'good':(has&&u.up?'':'empty'))
    +grp('当前上下文')
    +row('上下文已用',has?fmtTokens(used)+' / '+fmtTokens(win):'--',used?'':'empty')
    +row('上下文剩余',Math.round(freeRatio*100)+'%',freeRatio<=0.1?'warn':'')
    +row('速度',speed,speed==='--'?'empty':'');
  html+='<div class="unote" title="'+esc('上下文上限按模型「'+S.cfg.model+'」推断为 '+fmtTokens(win))+'">'
    +'上下文上限按模型「'+esc(S.cfg.model)+'」推断为 '+fmtTokens(win)+'</div>';
  pop.innerHTML=html;
  /* 贴在用量按钮上方 (放不下翻到下方) */
  const btn=$('usagebtn'), comp=$('composer');
  const bh=pop.offsetHeight||240;
  const btnTop=btn.getBoundingClientRect().top-comp.getBoundingClientRect().top;
  pop.style.top=(btnTop-bh-6)+'px';
}

/* ==================== 发送 ==================== */
function doSend(){
  const ta=$('inputT');
  const text=ta.value.trim();
  if(!text||S.sending)return;
  if(!S.cfg.hasKey){toastLocal();return;}
  ta.value=''; autoSize();
  post({c:'send',text});
}
function toastLocal(){ /* 未配置提示与旧口径一致 (宿主 Toast) */
  post({c:'notify',msg:'尚未配置接口密钥 — 请点右上角 接口设置 填写'});
}

/* ==================== C++ → JS ==================== */
function handle(m){
  switch(m.t){
    case 'boot':
      S.cfg=m.cfg; S.pal=m.pal; applyPal(S.pal);
      S.convs=m.convs||[]; S.cur=m.cur||0; S.msgs=m.msgs||[];
      S.sending=!!m.st.sending; S.net=m.st.net; S.phase=m.st.phase||0;
      S.usage=m.usage||S.usage;
      renderAll(); break;
    case 'pal': S.pal=m.pal; applyPal(S.pal); renderAll(); break;
    case 'cfg':
      S.cfg=m.cfg;
      if(S.cfgOpen){S.dUrl=m.cfg.url;S.dModel=m.cfg.model;S.dReason=m.cfg.reasoning;S.cfgOpen=false;}
      renderHead(); renderCfg(); renderComposer(); break;
    case 'convs': S.convs=m.convs||[]; renderSide(); break;
    case 'msgs':
      if(m.cur!==undefined)S.cur=m.cur;
      S.msgs=m.msgs||[];
      if(m.st){S.sending=!!m.st.sending;S.net=m.st.net;S.phase=m.st.phase||0;}
      renderThread(true); renderHead(); renderComposer(); renderSide(); break;
    case 'last':{
      if(!S.msgs.length)break;
      S.msgs[S.msgs.length-1]=m.m;
      if(m.st){S.sending=!!m.st.sending;S.phase=m.st.phase||0;}
      applyLast(); renderHead(); break;}
    case 'usage': S.usage=m.u||S.usage; renderUsage(); break;
    case 'status':
      S.sending=!!m.sending; S.net=m.net; S.phase=m.phase||0;
      renderHead(); renderThread(true); renderComposer(); renderSide(); break;
  }
}
function renderAll(){
  renderHead(); renderCfg(); renderThread(false); renderComposer(); renderSide();
}

/* ==================== 事件接线 ==================== */
function bind(){
  $('b-set').addEventListener('click',()=>cfgToggle(!S.cfgOpen));
  $('b-hist').addEventListener('click',()=>sideToggle(!S.sideOpen));
  $('b-new').addEventListener('click',()=>post({c:'new'}));
  $('b-close').addEventListener('click',()=>post({c:'close'}));
  $('b-cancel').addEventListener('click',()=>cfgToggle(false));
  $('b-save').addEventListener('click',()=>{
    S.cfgOpen=false; renderCfg();
    post({c:'settings',url:$('f-url').value.trim(),key:$('f-key').value.trim(),
          model:$('f-model').value.trim(),reasoning:$('cfgchk').classList.contains('on')});
  });
  $('cfgchk').addEventListener('click',()=>{S.dReason=!S.dReason;
    $('cfgchk').classList.toggle('on',S.dReason);});
  $('sendB').addEventListener('click',()=>{ if(S.sending)post({c:'stop'}); else doSend(); });
  const ta=$('inputT');
  ta.addEventListener('input',autoSize);
  ta.addEventListener('focus',()=>{$('crow').classList.add('focus');});
  ta.addEventListener('blur',()=>{$('crow').classList.remove('focus');});
  ta.addEventListener('keydown',e=>{
    if(e.key==='Enter'&&!e.shiftKey&&!e.isComposing){e.preventDefault();doSend();}
  });
  $('policy').addEventListener('click',e=>{
    const seg=e.target.closest('.pseg'); if(!seg)return;
    post({c:'policy',v:+seg.getAttribute('data-policy')});
  });
  $('usagebtn').addEventListener('click',()=>{S.usageOpen=!S.usageOpen;renderUsagePop();});
  $('clearb').addEventListener('click',()=>{
    if(S.clearArm){S.clearArm=false;clearTimeout(S.clearTimer);post({c:'clearHist'});sideToggle(false);}
    else {S.clearArm=true;renderSide();clearTimeout(S.clearTimer);
      S.clearTimer=setTimeout(()=>{S.clearArm=false;renderSide();},4000);}
  });
  $('sidelist').addEventListener('click',e=>{
    const del=e.target.closest('.del');
    if(del){e.stopPropagation();
      const id=+del.getAttribute('data-del');
      if(S.armRow===id){S.armRow=-1;post({c:'del',id});}
      else {S.armRow=id;renderSide();clearTimeout(S.armTimer);
        S.armTimer=setTimeout(()=>{S.armRow=-1;renderSide();},4000);}
      return;}
    const row=e.target.closest('.srow');
    if(row){S.sideOpen=false;renderSide();post({c:'load',id:+row.getAttribute('data-id')});}
  });
  $('sidescrim').addEventListener('mousedown',()=>sideToggle(false));
  $('empty').addEventListener('click',e=>{
    const b=e.target.closest('.esugg'); if(!b)return;
    if(S.sending)return;
    post({c:'send',text:b.getAttribute('data-q')});
  });
  document.addEventListener('mousedown',e=>{
    if(S.usageOpen&&!e.target.closest('#usagepop')&&!e.target.closest('#usagebtn')){
      S.usageOpen=false;renderUsagePop();}
  });
  document.addEventListener('keydown',e=>{
    if(e.key==='Escape'){
      if(S.cfgOpen){cfgToggle(false);e.preventDefault();return;}
      if(S.sideOpen){sideToggle(false);e.preventDefault();return;}
    }
  });
  window.addEventListener('resize',()=>{if(S.sideOpen)renderSide();renderJump();});
  /* 设置面板/侧栏开合改变消息流高度 (窗口尺寸不变) → 跳转条重排 */
  if(window.ResizeObserver){ try{ new ResizeObserver(()=>renderJump()).observe(threadEl()); }catch(e){} }
  bindThread();
  /* 建议按钮 */
  $('esuggs').innerHTML=SUGGS.map(q=>'<div class="esugg" data-q="'+esc(q)+'">'+esc(q)+'</div>').join('');
  /* JS ready → C++ 推 boot */
  post({c:'ready'});
}
try{
)AIWEBUI"
           LR"AIWEBUI(  chrome.webview.addEventListener('message',e=>{
    try{handle(typeof e.data==='string'?JSON.parse(e.data):e.data);}catch(err){}
  });
}catch(e){}
document.addEventListener('DOMContentLoaded',bind);
</script>
</body>
</html>
)AIWEBUI";
}
