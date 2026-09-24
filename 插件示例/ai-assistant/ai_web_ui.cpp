/*
 * ai_web_ui.cpp — 嵌入式前端: 整套 AI 对话 UI 的单文件 HTML+CSS+JS (NavigateToString 装载)。
 * 内容 = 一个完整 HTML 文档, 按多段相邻宽原始字面量拼接 (段界只是拼接缝)。
 * 结构/样式/交互与参考实现的 AI 助手页同源 (类名同源: ai-msg/ai-bubble/ai-reasoning/
 * ai-cmd-policy/ai-usage/ai-jumpbar/ai-history-*), 颜色 = CSS 变量 (运行时由 C++
 * 推送的皮肤调色注入, 派生色一律 color-mix 从变量现算)。
 * 交互: C++→JS 推送 (boot/pal/cfg/convs/msgs/last/usage/status/toast), JS→C++ 命令 (send/stop/
 * close/settings/policy/new/load/del/clearHist/copy/openurl/pallow/pdeny/retry/ready/
 * search/searchfill/open/reveal/copypath)。toast = 页面内提示浮层: 面板被浏览器子窗盖住,
 * 宿主 Toast 画不进来, 面板打开期间的提示一律走这条 (C++ WebToast 推送 / 页内 showToast)。
 * 可点击交互 (AI 决定点击的类型, 一律标准 Markdown 链接语法): 模型输出 [指引](xjs://search?text=..&mode=..)
 * 渲染成搜索卡片 (单击=置入搜索框并按模式执行, 右键=只填入/复制); 文件动作 [文件名](xjs://open|reveal?id=<FileId>)
 * 只带引擎 FileId — 路径由程序按 ID 解析, 前端不接触路径 (2026-09-25 用户口径); path 参数 =
 * 旧历史消息的路径版链接, 继续受理; 正文里确有绝对路径 (旧消息/ai.row 的路径字段) 仍自动识别为文件链接
 * (单击=打开, 右键=打开/定位/复制); lua/luau/sql 代码块做词法级语法高亮 (.tok-*)。
 * 安全面: CSP 关 fetch/XHR/表单/外域; 模型输出永不产生活 HTML (C++ md4c 层转义裁剪);
 * <a> 点击拦截转 openurl 命令; 选区/复制/右键/输入法 = 浏览器原生能力 (右键菜单只在
 * 卡片/路径上接管为自绘菜单, 其余区域保留原生菜单 = 选区复制入口)。
 * 维护口径: 改内容直接在下方字面量里编辑, 段超限就再切一刀 (段界只是拼接缝)。
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
/* ============================================================
   设计语言对齐参考实现 (AI 助手页):
   中性 · 克制 · 专业 — 单一蓝主色 + 中性灰表面, 语义色仅用于状态指示。
   所有颜色经 CSS 变量注入 (运行时由 C++ 推送的皮肤调色写入 :root),
   其余派生色全部用 color-mix 从变量现算, 深浅皮肤两用。
   ============================================================ */
:root{
  --bg:#14171f; --surface-raised:#1b2030;
  --text-primary:#e8eaf0; --text-secondary:#9aa3b5; --text-tertiary:#5c6577;
  --accent-violet:#4f7cff; --accent-cyan:#0ea5e9; --accent-emerald:#10b981;
  --accent-amber:#f59e0b; --accent-pink:#ef4444;
  --glass-border:#e8eaf02e; --overlay-border:#e8eaf05e;
  --divider:#e8eaf026; --btn-secondary-hover:#e8eaf01f;
  --ai-user-accent:#f97316;
  --scrollbar-thumb:rgba(128,128,128,.38);
  --scrollbar-thumb-hover:rgba(112,112,112,.58);
  --scrollbar-thumb-pressed:rgba(96,96,96,.78);
  /* AI 对话区三个面板框 (思考过程 / 输出内容 / 授权卡) 共用的圆角与边框浓度 */
  --ai-surface-radius:8px;
  --ai-surface-border:color-mix(in srgb,var(--text-primary) 12%,transparent);
  /* 用户气泡 (暖橙, 与助手侧的中性底互补; 明度接近才是和谐的关键) */
  --ai-user-bubble-bg:color-mix(in srgb,var(--ai-user-accent) 18%,var(--surface-raised));
  --ai-user-bubble-border:color-mix(in srgb,var(--ai-user-accent) 34%,transparent);
  --ai-user-bubble-text:color-mix(in srgb,var(--ai-user-accent) 6%,var(--text-primary));
  /* 消息内容列左右各让出的宽度 = 头像 26 + 行内间距 10 (气泡与授权卡同源) */
  --ai-msg-gutter:36px;
  --ai-composer-radius:12px;
}
*{margin:0;padding:0;box-sizing:border-box}
[hidden]{display:none!important}
html,body{height:100%}
body{background:var(--bg);color:var(--text-primary);overflow:hidden;position:relative;cursor:default;
     user-select:none;-webkit-user-select:none;
     font-family:'Microsoft YaHei','微软雅黑','PingFang SC','Segoe UI',sans-serif;font-size:12px}
textarea,input{user-select:text;-webkit-user-select:text}
.glyph{font-family:'Segoe Fluent Icons','Segoe MDL2 Assets',sans-serif;font-style:normal;line-height:1}
.ellipsis-text{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}

/* ---- 全局滚动条 (对话流内 16px 大号覆盖, 见 .ai-thread) ---- */
*::-webkit-scrollbar{width:12px;height:12px}
*::-webkit-scrollbar-track{background:transparent}
*::-webkit-scrollbar-thumb{background:var(--scrollbar-thumb);border-radius:999px;border:3px solid transparent;background-clip:padding-box}
*::-webkit-scrollbar-thumb:hover{background:var(--scrollbar-thumb-hover);border:3px solid transparent;background-clip:padding-box}
*::-webkit-scrollbar-thumb:active{background:var(--scrollbar-thumb-pressed);border:3px solid transparent;background-clip:padding-box}
*::-webkit-scrollbar-corner{background:transparent}

#app{display:flex;height:100%;min-height:0;position:relative}
.ai-main{display:flex;flex:1 1 auto;flex-direction:column;min-width:0;min-height:0}

/* ==================== 工具栏 (标题 + 连接状态 + 按钮) ==================== */
.ai-toolbar{display:flex;align-items:center;gap:12px;flex:0 0 auto;padding:16px 20px;border-bottom:1px solid var(--glass-border)}
.ai-toolbar-title{color:var(--text-primary);font-size:20px;font-weight:600;white-space:nowrap;
        flex:0 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis}
.ai-toolbar-status{display:inline-flex;align-items:center;gap:6px;min-width:0;font-size:11px;color:var(--text-secondary)}
.ai-status-dot{flex:0 0 auto;width:7px;height:7px;border-radius:50%;background:var(--text-tertiary)}
.ai-status-dot[data-state="ready"]{background:#22c55e;box-shadow:0 0 6px rgba(34,197,94,.5)}
.ai-status-dot[data-state="missing"]{background:var(--accent-amber)}
.ai-status-dot[data-state="busy"]{background:var(--accent-violet)}
.ai-status-dot[data-state="error"]{background:var(--accent-pink)}
.ai-toolbar-actions{display:flex;align-items:center;gap:8px;margin-left:auto}
.ai-btn{display:inline-flex;align-items:center;gap:6px;height:30px;padding:0 12px;border:1px solid var(--glass-border);
        border-radius:4px;background:var(--surface-raised);color:var(--text-secondary);font:inherit;font-size:12px;
        cursor:default;transition:background-color 120ms ease,color 120ms ease,border-color 120ms ease}
.ai-btn:hover{background:var(--btn-secondary-hover);color:var(--text-primary)}
.ai-btn:disabled{opacity:.45;pointer-events:none}
.ai-btn.icononly{padding:0 9px}
.ai-btn .glyph{font-size:11px}
.ai-btn-primary{border-color:transparent;background:var(--accent-violet);color:#fff}
)AIWEBUI"
           LR"AIWEBUI(.ai-btn-primary:hover{background:color-mix(in srgb,var(--accent-violet) 84%,#fff);color:#fff}

/* ---- 接口设置 (折叠面板) ---- */
.ai-config-panel{display:grid;gap:10px;flex:0 0 auto;padding:14px 20px 16px;border-bottom:1px solid var(--glass-border);
                 background:color-mix(in srgb,var(--surface-raised) 70%,var(--bg))}
.ai-config-row{display:grid;grid-template-columns:72px minmax(0,1fr);align-items:center;gap:12px}
.ai-config-label{font-size:12px;color:var(--text-secondary)}
.ai-config-input{height:28px;min-width:0;padding:0 8px;border:1px solid var(--glass-border);border-radius:4px;
                 background:var(--surface-raised);color:var(--text-primary);font:inherit;font-size:12px;
                 transition:border-color 120ms ease}
.ai-config-input:hover{border-color:var(--overlay-border)}
.ai-config-input:focus{outline:none;border-color:var(--accent-violet)}
.ai-config-hint{font-size:11px;line-height:1.6;color:var(--text-tertiary)}
.ai-config-actions{display:flex;justify-content:flex-end;gap:8px}
/* 深度思考开关 (参考实现无此行; 本插件的 reasoning.effort 功能保留, 样式随面板) */
.ai-reason-toggle{display:inline-flex;align-items:center;gap:7px;border:0;background:transparent;padding:0;
                  color:var(--text-secondary);font:inherit;font-size:12px;cursor:default}
.ai-reason-toggle:hover{color:var(--text-primary)}
.ai-reason-box{position:relative;flex:0 0 auto;width:13px;height:13px;border:1px solid color-mix(in srgb,var(--text-tertiary) 70%,transparent);
               border-radius:3px;background:transparent}
.ai-reason-toggle[aria-pressed="true"] .ai-reason-box{border-color:color-mix(in srgb,var(--accent-violet) 60%,transparent);
               background:color-mix(in srgb,var(--accent-violet) 20%,transparent)}
.ai-reason-toggle[aria-pressed="true"] .ai-reason-box::after{content:'';position:absolute;left:3px;top:.5px;width:3.5px;height:6.5px;
               border-right:1.6px solid var(--accent-violet);border-bottom:1.6px solid var(--accent-violet);transform:rotate(42deg)}
)AIWEBUI"
           LR"AIWEBUI(/* 档案下拉与"新建/复制/删除"并排: 下拉吞掉剩余宽度, 按钮各自保持内容宽 */
.ai-config-profile-row{display:grid;grid-template-columns:minmax(0,1fr) auto auto auto;align-items:center;gap:8px}
.ai-config-profile-row .ai-btn{height:28px;padding:0 10px}
.ai-config-profile-row select{cursor:default}
/* 待确认删除: 两步确认语言 (删除模型档案 / 清空历史记录共用) */
.ai-config-profile-row .ai-btn[data-armed="true"],.ai-history-clear[data-armed="true"]{border-color:color-mix(in srgb,var(--accent-pink) 55%,transparent);
               color:var(--accent-pink)}
/* 工具栏模型切换下拉 (管理动作都在接口设置面板里, 末项固定是"管理模型…") */
.ai-model-picker{position:relative;display:flex;flex:0 1 auto;min-width:0}
.ai-model-picker-button{max-width:190px}
.ai-model-caret{flex:0 0 auto;font-size:9px;line-height:1;transition:transform 120ms ease}
.ai-model-picker-button[aria-expanded="true"] .ai-model-caret{transform:rotate(180deg)}
.ai-model-menu{position:absolute;right:0;top:calc(100% + 6px);z-index:17;display:grid;gap:2px;width:max-content;
          min-width:190px;max-width:300px;max-height:320px;overflow-y:auto;padding:4px;border-radius:7px;
          background:var(--surface-raised);box-shadow:inset 0 0 0 1px var(--overlay-border),0 8px 24px rgba(0,0,0,.28);
          cursor:default;opacity:1;transform:translateY(0) scale(1);transform-origin:right top;
          transition:opacity 120ms ease,transform 140ms cubic-bezier(.2,0,0,1)}
.ai-model-menu[hidden]{display:none}
.ai-model-menu.opening,.ai-model-menu.closing{opacity:0;transform:translateY(-5px) scale(.98);pointer-events:none}
.ai-model-menu.closing{transition-duration:90ms}
.ai-model-option{display:grid;grid-template-columns:16px minmax(0,1fr);align-items:start;gap:6px;width:100%;
          padding:5px 8px;border:0;border-radius:4px;background:transparent;color:var(--text-secondary);
          font:inherit;font-size:12px;line-height:1.5;text-align:left;cursor:default;outline:none}
.ai-model-option:hover{background:var(--btn-secondary-hover);color:var(--text-primary)}
.ai-model-option[aria-checked="true"]{background:color-mix(in srgb,var(--accent-violet) 14%,transparent);color:var(--text-primary)}
.ai-model-option-check{color:transparent;font-size:11px;line-height:1.6}
.ai-model-option[aria-checked="true"] .ai-model-option-check{color:var(--accent-violet)}
/* 档案名与其模型名: 两条档案显示名撞车时, 靠第二行才分得清 */
.ai-model-option-text{display:flex;flex-direction:column;min-width:0}
.ai-model-option-name{overflow:hidden;white-space:nowrap;text-overflow:ellipsis}
.ai-model-option-hint{overflow:hidden;color:var(--text-tertiary);font-size:10.5px;line-height:1.4;white-space:nowrap;text-overflow:ellipsis}
.ai-model-option[aria-checked="true"] .ai-model-option-hint{color:var(--text-secondary)}
/* 管理入口不是"一个可选模型", 用一条分隔线划开 */
.ai-model-option-manage{display:block;margin-top:4px;padding-top:8px;border-top:1px solid var(--divider);
               border-radius:0 0 4px 4px;color:var(--text-tertiary)}

/* ==================== 对话流 ==================== */
.ai-thread-wrap{position:relative;display:flex;flex:1 1 auto;min-width:0;min-height:0}
/* 左内边距 40px 给左缘轮次跳转条留位 */
.ai-thread{position:relative;flex:1 1 auto;min-height:0;overflow-y:auto;overflow-x:hidden;padding:18px 20px 14px 40px}
.ai-thread::-webkit-scrollbar,.ai-thread *::-webkit-scrollbar{width:16px;height:16px}
.ai-thread::-webkit-scrollbar-thumb,.ai-thread *::-webkit-scrollbar-thumb{border:4px solid transparent;background-clip:padding-box}
.ai-thread::-webkit-scrollbar-thumb:hover,.ai-thread *::-webkit-scrollbar-thumb:hover{border:4px solid transparent;background-clip:padding-box}
.ai-thread::-webkit-scrollbar-thumb:active,.ai-thread *::-webkit-scrollbar-thumb:active{border:4px solid transparent;background-clip:padding-box}
.ai-thread-inner{display:flex;flex-direction:column;gap:16px;width:100%;max-width:min(1600px,100%);margin:0 auto;
                 user-select:text;-webkit-user-select:text}
/* 对话区里不参与文本选择的只有交互控件与图标 */
.ai-thread-inner button,.ai-msg-avatar,.ai-reasoning-ic,.ai-reasoning-chevron,.ai-meta-btn{user-select:none;-webkit-user-select:none}

.ai-msg{display:flex;gap:10px;min-width:0}
.ai-msg-user{flex-direction:row-reverse}
.ai-msg-avatar{display:grid;flex:0 0 auto;width:26px;height:26px;place-items:center;border-radius:50%;font-size:13px}
.ai-msg-assistant .ai-msg-avatar{background:color-mix(in srgb,var(--accent-violet) 20%,transparent);color:var(--accent-violet)}
.ai-msg-user .ai-msg-avatar{background:var(--btn-secondary-hover);color:var(--text-secondary)}
.ai-msg-main{display:flex;flex-direction:column;gap:8px;min-width:0;flex:1 1 auto;max-width:calc(100% - var(--ai-msg-gutter)*2)}
.ai-msg-user .ai-msg-main{align-items:flex-end}

.ai-bubble{padding:9px 12px;border-radius:var(--ai-surface-radius);font-size:12.5px;line-height:1.72;
           color:var(--text-primary);overflow-wrap:anywhere;min-width:44px;max-width:100%}
.ai-msg-assistant .ai-bubble{border:1px solid var(--ai-surface-border);background:var(--surface-raised)}
.ai-msg-assistant .ai-bubble.ai-bubble-short{align-self:flex-start;width:fit-content}
.ai-msg-assistant .ai-bubble:empty{display:none}
.ai-msg-user .ai-bubble{border:1px solid var(--ai-user-bubble-border);background:var(--ai-user-bubble-bg);color:var(--ai-user-bubble-text)}
.ai-msg-error .ai-bubble,.ai-bubble-error{border:1px solid color-mix(in srgb,var(--accent-pink) 42%,transparent)!important;
           background:color-mix(in srgb,var(--accent-pink) 12%,var(--surface-raised))!important}

/* ---- 过程区 (一个回合内, 回答气泡之前的全部中间产物) ----
 * 思考块 + 工具组 + 中间叙述合成**一块**面板: 各自只占一行, 行间细线分隔,
 * 不再每块自带边框各自成卡散开 (用户反馈"有思考的时候不放在一起") ——
 * 面板之下才是本轮回答气泡。--ai-reasoning-text 的取值与推理区同源: 过程是草稿, 比正文淡一档 */
.ai-turn-log{display:flex;flex-direction:column;gap:0;width:100%;overflow:hidden;
             border:1px solid var(--glass-border);border-radius:8px;
             background:color-mix(in srgb,var(--text-primary) 3%,transparent);
             --ai-reasoning-text:color-mix(in srgb,var(--text-secondary) 62%,var(--text-tertiary))}
/* 面板整体可收起: 头部一行常驻 (图标+标签+状态+箭头), 体 = 各过程行 (2026-09-25 用户口径
 * "AI 开始回答正文时自动收缩" — 收起落账见 maybeAutoCollapseTurn; 点头部开合后手动覆写自动) */
.ai-turn-log-head{display:flex;align-items:center;gap:7px;width:100%;padding:6px 10px;border:0;
             background:transparent;color:var(--ai-reasoning-text);font:inherit;font-size:11px;
             line-height:1.5;text-align:left;cursor:default}
.ai-turn-log-head:hover{background:color-mix(in srgb,var(--accent-violet) 8%,transparent);color:var(--text-secondary)}
.ai-turn-log-ic,.ai-turn-log-arr{flex:0 0 auto;font-size:11px}
.ai-turn-log-arr{font-size:9px;transition:transform 120ms ease}
.ai-turn-log.open .ai-turn-log-arr{transform:rotate(180deg)}
.ai-turn-log-label{flex:1 1 auto;min-width:0;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}
.ai-turn-log-sum{flex:0 0 auto;color:color-mix(in srgb,var(--accent-amber) 82%,var(--text-primary))}
.ai-turn-log-body{display:none;flex-direction:column;gap:0;border-top:1px solid var(--divider)}
.ai-turn-log.open>.ai-turn-log-body{display:flex}
/* 相邻行细分隔线; 行内元素在面板里脱掉自己的卡框 (圆角/边框/底色归面板所有) */
.ai-turn-log-body>*+*{border-top:1px solid var(--divider)}
.ai-turn-log .ai-reasoning,.ai-turn-log .ai-toolgrp{border:0;border-radius:0;background:transparent}
.ai-turn-log-body>.ai-steps>.step{border:0;border-radius:0;background:transparent}
/* 面板内各行的水平内边距统一 10px (推理头原样, 工具组头/单卡片行原本 8px 会错位) */
.ai-turn-log-body>.ai-toolgrp>.ai-toolgrp-head,
.ai-turn-log-body>.ai-steps>.step>.shead{padding-left:10px;padding-right:10px}
.ai-turn-note{font-size:12px;line-height:1.68;color:var(--ai-reasoning-text);overflow-wrap:anywhere;padding:7px 10px}
/* 中间叙述复用 C++ 的 ai-bubble 输出, 在过程区内脱掉气泡外框 (无边框无底色, 淡色小字) */
.ai-turn-note>.ai-bubble{border:0;background:transparent;padding:0;min-width:0;
             color:inherit;font-size:12px;line-height:1.68}
.ai-turn-note>.ai-bubble p{margin:0 0 5px}
.ai-turn-note>.ai-bubble p:last-child{margin-bottom:0}
.ai-turn-note>.ai-bubble strong{color:var(--ai-reasoning-text)}

/* ---- 每条回答结尾的信息行 (重试 / 复制) ---- */
.ai-msg-meta{display:flex;align-items:center;gap:2px;align-self:flex-start;width:fit-content;min-width:0;
             margin-top:-3px;color:var(--text-tertiary);font-size:11px;line-height:1.4}
)AIWEBUI"
           LR"AIWEBUI(.ai-meta-btn{display:inline-flex;align-items:center;justify-content:center;width:20px;height:20px;padding:0;border:0;
             border-radius:4px;background:transparent;color:var(--text-tertiary);font-size:11px;cursor:default;
             transition:background-color 120ms ease,color 120ms ease}
.ai-meta-btn:hover{background:var(--btn-secondary-hover);color:var(--text-primary)}

/* ---- 气泡正文 (md) ---- */
.ai-bubble p{margin:0 0 6px}
.ai-bubble p:last-child{margin-bottom:0}
.ai-bubble ul,.ai-bubble ol{margin:4px 0;padding-left:18px}
.ai-bubble li{margin:2px 0}
.ai-bubble li::marker{color:color-mix(in srgb,var(--accent-violet) 62%,var(--text-tertiary))}
.ai-bubble strong{font-weight:600;color:var(--text-primary)}
.ai-bubble em{color:var(--text-primary)}
.ai-bubble s,.ai-bubble del{color:var(--text-tertiary);text-decoration:line-through}
.ai-bubble a{color:var(--accent-cyan);text-decoration:none;border-bottom:1px solid color-mix(in srgb,var(--accent-cyan) 40%,transparent);cursor:pointer}
.ai-bubble a:hover{color:color-mix(in srgb,var(--accent-cyan) 80%,#fff);border-bottom-color:var(--accent-cyan)}
.ai-bubble code{padding:1px 4px;border-radius:3px;background:color-mix(in srgb,var(--accent-violet) 13%,transparent);
                color:color-mix(in srgb,var(--accent-violet) 80%,var(--text-primary));
                font-family:Consolas,'Cascadia Mono',monospace;font-size:11.5px}
.ai-bubble h1,.ai-bubble h2,.ai-bubble h3,.ai-bubble h4{margin:10px 0 6px;font-weight:600;line-height:1.4}
.ai-bubble h1,.ai-bubble h2,.ai-bubble h3{color:color-mix(in srgb,var(--accent-violet) 80%,var(--text-primary))}
.ai-bubble h4{color:var(--text-primary)}
.ai-bubble h1{font-size:17px;border-bottom:1px solid var(--divider);padding-bottom:4px}
.ai-bubble h2{font-size:15.5px}
.ai-bubble h3{font-size:14px}
.ai-bubble h4{font-size:13px}
.ai-bubble h5{margin:8px 0 5px;color:var(--text-secondary);font-weight:600;font-size:12.5px;line-height:1.4}
.ai-bubble h6{margin:8px 0 5px;color:var(--text-tertiary);font-weight:600;font-size:12px;line-height:1.4}
.ai-bubble h1:first-child,.ai-bubble h2:first-child,.ai-bubble h3:first-child,.ai-bubble h4:first-child,
.ai-bubble h5:first-child,.ai-bubble h6:first-child{margin-top:0}
/* 加粗短句小标题 (C++ md 层改写为 p.ai-md-sub) */
.ai-bubble p.ai-md-sub{margin:9px 0 5px;color:color-mix(in srgb,var(--accent-violet) 78%,var(--text-primary));font-weight:600}
.ai-bubble p.ai-md-sub:first-child{margin-top:0}
.ai-bubble blockquote{margin:6px 0;padding:6px 10px;border-left:3px solid color-mix(in srgb,var(--accent-violet) 55%,transparent);
                      border-radius:0 4px 4px 0;background:color-mix(in srgb,var(--accent-violet) 7%,transparent);color:var(--text-secondary)}
.ai-bubble blockquote blockquote{margin:4px 0;background:transparent}
.ai-bubble blockquote p{margin:0}
.ai-bubble hr{margin:10px 0;border:0;height:1px;background:var(--divider)}
/* 表格 */
.ai-bubble .ai-table-wrap{margin:6px 0;overflow-x:auto;border:1px solid var(--glass-border);border-radius:5px}
.ai-bubble table{width:100%;border-collapse:collapse;font-size:12px;line-height:1.5}
.ai-bubble th,.ai-bubble td{padding:5px 9px;border-bottom:1px solid var(--divider);color:var(--text-primary);text-align:left;
                           overflow-wrap:break-word;word-break:normal}   /* break-word: "10" 这类短词不被 anywhere 拆成 "1 0" */
.ai-bubble th{background:color-mix(in srgb,var(--accent-violet) 12%,transparent);
              color:color-mix(in srgb,var(--accent-violet) 72%,var(--text-primary));font-weight:600;white-space:nowrap}
.ai-bubble tr:last-child td{border-bottom:0}
.ai-bubble tbody tr:hover td{background:var(--btn-secondary-hover)}
/* 嵌套列表层级符号 */
.ai-bubble ul ul{list-style:circle}
.ai-bubble ul ul ul{list-style:square}
/* 任务列表 */
.ai-bubble li.ai-task{display:flex;align-items:flex-start;gap:6px;margin:3px 0;list-style:none}
.ai-task-box{position:relative;flex:0 0 auto;box-sizing:border-box;width:12px;height:12px;margin-top:3px;
             border:1.5px solid color-mix(in srgb,var(--text-tertiary) 70%,transparent);border-radius:3px}
.ai-task-box.done{border-color:color-mix(in srgb,var(--accent-emerald) 76%,transparent);
                  background:color-mix(in srgb,var(--accent-emerald) 24%,transparent)}
.ai-task-box.done::after{content:'';position:absolute;top:.5px;left:3px;width:3px;height:5.5px;
             border-right:1.6px solid var(--accent-emerald);border-bottom:1.6px solid var(--accent-emerald);transform:rotate(42deg)}
.ai-bubble li.ai-task .ai-task-text{flex:1 1 auto;min-width:0}
.ai-bubble li.ai-task .ai-task-box.done+.ai-task-text{color:var(--text-tertiary);text-decoration:line-through}
/* 代码块 (带语言标注 + 复制按钮) */
.ai-bubble .ai-code{margin:6px 0;overflow:hidden;border:1px solid var(--glass-border);border-radius:6px;background:var(--bg)}
.ai-code-head{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:5px 8px 5px 10px;
              border-bottom:1px solid var(--divider);background:var(--btn-secondary-hover)}
)AIWEBUI"
           LR"AIWEBUI(.ai-code-lang{font-family:Consolas,'Cascadia Mono',monospace;font-size:10.5px;letter-spacing:.4px;
              text-transform:uppercase;color:var(--text-tertiary)}
.ai-code-copy{padding:3px 8px;border:1px solid var(--glass-border);border-radius:4px;background:transparent;
              color:var(--text-secondary);font-size:11px;font-family:inherit;cursor:default;
              transition:background 120ms ease,color 120ms ease,border-color 120ms ease}
.ai-code-copy:hover{background:var(--btn-secondary-hover);color:var(--text-primary);border-color:var(--overlay-border)}
.ai-code-copy.ai-code-copied{color:var(--accent-emerald);border-color:color-mix(in srgb,var(--accent-emerald) 45%,transparent)}
.ai-bubble .ai-code pre{margin:0;padding:8px 10px;overflow-x:auto}
.ai-bubble .ai-code pre code{padding:0;background:transparent;color:var(--text-primary);font-size:11.5px;line-height:1.6;white-space:pre}
)AIWEBUI"
           LR"AIWEBUI(/* ---- 可点击交互: 搜索卡片 (xjs-search 围栏渲染) ---- */
.ai-chip{display:flex;align-items:center;gap:8px;width:fit-content;max-width:100%;margin:7px 0;
         padding:7px 11px;border:1px solid color-mix(in srgb,var(--accent-violet) 40%,var(--glass-border));
         border-radius:8px;background:color-mix(in srgb,var(--accent-violet) 8%,var(--surface-raised));
         color:var(--text-primary);font-size:12px;line-height:1.5;cursor:pointer;
         user-select:none;-webkit-user-select:none;
         transition:background 120ms ease,border-color 120ms ease,box-shadow 120ms ease}
.ai-chip:hover{background:color-mix(in srgb,var(--accent-violet) 15%,var(--surface-raised));
               border-color:color-mix(in srgb,var(--accent-violet) 62%,transparent);
               box-shadow:0 2px 10px rgba(0,0,0,.16)}
.ai-chip:active{transform:translateY(1px)}
.ai-chip-ic{flex:0 0 auto;font-size:13px;color:var(--accent-violet)}
.ai-chip-text{flex:0 1 auto;min-width:0;max-width:520px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;
              font-family:Consolas,'Cascadia Mono',monospace;font-size:11.5px}
.ai-chip-mode{flex:0 0 auto;padding:1px 7px;border-radius:999px;
              background:color-mix(in srgb,var(--accent-violet) 20%,transparent);
              color:color-mix(in srgb,var(--accent-violet) 82%,var(--text-primary));
              font-size:10px;letter-spacing:.4px;text-transform:uppercase}
.ai-chip-go{flex:0 0 auto;font-size:10px;color:var(--text-tertiary);transition:color 120ms ease}
.ai-chip:hover .ai-chip-go{color:var(--accent-violet)}
/* 点击已发命令的瞬时确认 (600ms): 区分"命令没发出"与"宿主侧没执行" */
.ai-chip.sent{border-color:var(--accent-violet);box-shadow:0 0 0 2px color-mix(in srgb,var(--accent-violet) 28%,transparent)}
.ai-chip.sent .ai-chip-go{color:var(--accent-violet)}
/* ---- 文件路径链接 (气泡正文自动识别 + 工具卡片样本): 单击打开, 右键菜单 ---- */
.ai-path{color:var(--accent-cyan);border-bottom:1px dashed color-mix(in srgb,var(--accent-cyan) 48%,transparent);
         cursor:pointer;transition:color 120ms ease,border-bottom-color 120ms ease}
.ai-path:hover{color:color-mix(in srgb,var(--accent-cyan) 80%,#fff);border-bottom-style:solid}
/* ---- 右键菜单 (文件路径 / 搜索卡片的操作项) ---- */
.ai-ctx{position:fixed;left:0;top:0;z-index:13000;display:flex;flex-direction:column;min-width:150px;padding:4px;
        border-radius:7px;background:var(--surface-raised);
        box-shadow:inset 0 0 0 1px var(--overlay-border),0 8px 24px rgba(0,0,0,.3);cursor:default}
.ai-ctx-item{display:block;width:100%;padding:6px 10px;border:0;border-radius:4px;background:transparent;
             color:var(--text-primary);font:inherit;font-size:12px;text-align:left;cursor:pointer;white-space:nowrap}
.ai-ctx-item:hover{background:var(--btn-secondary-hover)}
.ai-ctx-sep{height:1px;margin:3px 6px;background:var(--divider)}
/* ---- 代码语法高亮 (Lua / SQL; 颜色取皮肤语义色, 深浅皮肤两用) ---- */
.tok-k{color:color-mix(in srgb,var(--accent-violet) 72%,var(--text-primary));font-weight:600}
.tok-s{color:var(--accent-emerald)}
.tok-c{color:var(--text-tertiary);font-style:italic}
.tok-n{color:var(--accent-amber)}
.tok-f{color:var(--accent-cyan)}
)AIWEBUI"
           LR"AIWEBUI(/* ---- 推理过程 (可折叠; 挂在 .ai-msg-main 里, 气泡之前) ---- */
.ai-reasoning{margin:0;overflow:hidden;width:100%;border:1px solid var(--ai-surface-border);border-radius:var(--ai-surface-radius);
              background:color-mix(in srgb,var(--accent-violet) 5%,transparent);
              --ai-reasoning-text:color-mix(in srgb,var(--text-secondary) 62%,var(--text-tertiary));
              --ai-reasoning-strong:color-mix(in srgb,var(--text-secondary) 58%,var(--text-primary))}
.ai-reasoning-head{display:flex;align-items:center;gap:6px;width:100%;padding:6px 10px;border:0;background:transparent;
                   color:var(--text-secondary);font-size:11.5px;font-family:inherit;text-align:left;cursor:default;
                   transition:color 120ms ease,background 120ms ease}
.ai-reasoning-head:hover{background:color-mix(in srgb,var(--accent-violet) 8%,transparent);color:var(--text-primary)}
.ai-reasoning-ic{flex:0 0 auto;font-size:12px;color:var(--accent-violet)}
.ai-reasoning-label{flex:1 1 auto}
.ai-reasoning-chevron{flex:0 0 auto;font-size:10px;color:var(--text-tertiary);transition:transform 160ms ease}
.ai-reasoning[data-open="true"] .ai-reasoning-chevron{transform:rotate(90deg)}
.ai-reasoning-body{display:grid;grid-template-rows:0fr;transition:grid-template-rows 200ms cubic-bezier(.2,0,0,1)}
.ai-reasoning-inner{overflow:hidden;min-height:0;opacity:0;font-size:12px;line-height:1.68;overflow-wrap:anywhere;
                    color:var(--ai-reasoning-text);white-space:pre-wrap;
                    transition:padding 200ms ease,opacity 150ms ease}
.ai-reasoning[data-open="true"] .ai-reasoning-body{grid-template-rows:1fr}
.ai-reasoning[data-open="true"] .ai-reasoning-inner{padding:2px 10px 8px;opacity:1;
                    border-top:1px solid color-mix(in srgb,var(--accent-violet) 20%,transparent)}
/* 思考中: 图标与标题缓慢呼吸 */
.ai-reasoning[data-thinking="true"] .ai-reasoning-ic{animation:ai-reasoning-think 1.5s ease-in-out infinite}
.ai-reasoning[data-thinking="true"] .ai-reasoning-label{animation:ai-reasoning-title-think 1.5s ease-in-out infinite}
@keyframes ai-reasoning-think{0%,100%{opacity:.4;transform:scale(.9)}50%{opacity:1;transform:scale(1.08)}}
@keyframes ai-reasoning-title-think{0%,100%{opacity:.7}50%{opacity:1}}

/* ---- 工具执行卡片 (role==2 消息; 过程记录外观与参考实现的命令记录同源) ----
 * 折叠口径: 头部恒一行 (徽标 + 单行省略的查询摘要 + 状态 + 箭头),
 * 点击展开才看完整查询与样本列表 —— 长查询默认全展示会把过程区撑满整屏 (用户反馈) */
.ai-steps{display:flex;flex-direction:column;gap:6px;width:100%}
/* ---- 连续工具组 (≥2 张卡片聚一组, 整组折叠; 对齐参考实现的推理区折叠语言) ---- */
.ai-toolgrp{border:1px solid var(--glass-border);border-radius:5px;
            background:color-mix(in srgb,var(--text-primary) 3%,transparent);overflow:hidden}
.ai-toolgrp-head{display:flex;align-items:center;gap:8px;width:100%;padding:6px 8px;border:0;background:transparent;
            color:var(--text-secondary);font-size:11px;font-family:inherit;text-align:left;cursor:pointer;
            transition:color 120ms ease,background 120ms ease}
.ai-toolgrp-head:hover{background:color-mix(in srgb,var(--accent-violet) 8%,transparent);color:var(--text-primary)}
.ai-toolgrp-label{flex:0 0 auto;font-weight:600}
.ai-toolgrp-sum{flex:1 1 auto;min-width:0;text-align:right;color:var(--text-tertiary);font-size:10px;
            font-variant-numeric:tabular-nums;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.ai-toolgrp-sum.bad{color:color-mix(in srgb,var(--accent-pink) 78%,var(--text-primary))}
.ai-toolgrp-sum.wait{color:color-mix(in srgb,var(--accent-amber) 82%,var(--text-primary))}
.ai-toolgrp-arr{flex:0 0 auto;font-size:9px;color:var(--text-tertiary);transition:transform 120ms ease}
.ai-toolgrp.open .ai-toolgrp-arr{transform:rotate(180deg)}
.ai-toolgrp-body{display:none;flex-direction:column;gap:6px;padding:6px;border-top:1px solid var(--glass-border)}
.ai-toolgrp.open .ai-toolgrp-body{display:flex}
.step{border:1px solid var(--glass-border);border-radius:5px;background:color-mix(in srgb,var(--text-primary) 5%,transparent);overflow:hidden}
.step.failed{border-color:color-mix(in srgb,var(--accent-pink) 38%,transparent)}
.shead{display:flex;align-items:baseline;gap:6px;padding:5px 8px;font-size:11px;cursor:pointer;overflow:hidden}
.sbadge{flex:0 0 auto;padding:0 5px;border-radius:3px;background:color-mix(in srgb,var(--accent-violet) 16%,transparent);
        color:color-mix(in srgb,var(--accent-violet) 80%,var(--text-primary));font-size:10px;line-height:1.6}
.scmd{flex:1 1 auto;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;
      color:var(--text-secondary);font-family:Consolas,'Cascadia Mono',monospace;font-size:11px}
.sst{flex:0 0 auto;color:var(--text-tertiary);font-size:10px;font-variant-numeric:tabular-nums}
.sst.bad{color:color-mix(in srgb,var(--accent-pink) 78%,var(--text-primary))}
.sarr{flex:0 0 auto;font-size:9px;color:var(--text-tertiary);transition:transform 120ms ease}
.step.open .sarr{transform:rotate(180deg)}
.sout,.ssamples{margin:0;padding:6px 8px;max-height:200px;overflow:auto;border-top:1px solid var(--glass-border);
        background:color-mix(in srgb,var(--text-primary) 4%,transparent);font-family:Consolas,'Cascadia Mono',monospace;
        font-size:11px;line-height:1.5;color:var(--text-secondary);white-space:pre-wrap;overflow-wrap:anywhere}
/* 策略询问 (卡上确认): 上分隔线 + 说明文字 + 允许/拒绝按钮 */
.sask{padding:7px 8px 8px;border-top:1px solid var(--divider);color:var(--text-tertiary);font-size:11px;line-height:1.5}
/* 待应用的调整 (AI 提案, 用户逐项 应用/忽略; 源样式对齐 .sask 一族) */
.sadj{padding:7px 8px 8px;border-top:1px solid var(--divider);font-size:11px;line-height:1.6}
.sadj-head{color:var(--text-tertiary);margin-bottom:3px}
.sadj-it{display:flex;align-items:center;gap:6px;padding:2px 0;min-width:0}
.sadj-k{flex:0 0 auto;color:var(--text-secondary)}
.sadj-v{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--text-primary);font-weight:600}
.sadj-st{flex:0 0 auto;margin-left:auto;color:var(--text-tertiary);white-space:nowrap}
.sadj-it.done .sadj-st{color:var(--accent-emerald)}
.sadj-it.bad .sadj-st{color:var(--accent-pink)}
.sadj-btns{flex:0 0 auto;display:flex;gap:4px;margin-left:8px;white-space:nowrap}
.sadj-foot{display:flex;align-items:center;gap:6px;margin-top:4px;color:var(--text-tertiary)}
.sadj-sum{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.abtn{display:inline-flex;align-items:center;height:22px;margin:6px 6px 0 0;padding:0 9px;border:1px solid var(--glass-border);
)AIWEBUI"
           LR"AIWEBUI(      border-radius:4px;background:var(--surface-raised);color:var(--text-secondary);font-size:11px;cursor:default;
      transition:background-color 120ms ease,color 120ms ease}
.abtn:hover{background:var(--btn-secondary-hover);color:var(--text-primary)}
.abtn.primary{border-color:transparent;background:var(--accent-violet);color:#fff}
.abtn.primary:hover{background:color-mix(in srgb,var(--accent-violet) 84%,#fff);color:#fff}

/* ---- 生成指示 (三点跳动, 在气泡内) ---- */
.ai-typing{display:inline-flex;align-items:center;gap:4px;height:20px}
.ai-typing i{width:5px;height:5px;border-radius:50%;background:var(--text-tertiary);animation:ai-typing-bounce 900ms ease-in-out infinite}
.ai-typing i:nth-child(2){animation-delay:150ms}
.ai-typing i:nth-child(3){animation-delay:300ms}
@keyframes ai-typing-bounce{0%,60%,100%{opacity:.35;transform:translateY(0)}30%{opacity:1;transform:translateY(-3px)}}

/* ---- 会话内轮次跳转条 (左缘紧凑刻度组) ----
   默认刻意压到很淡, 鼠标进入整组才提亮 —— 辅助导航, 不跟对话内容抢注意力 */
.ai-jumpbar{--ai-jump-dot:color-mix(in srgb,var(--text-tertiary) 42%,transparent);
            position:absolute;left:14px;top:50%;z-index:3;display:flex;width:13px;flex-direction:column;align-items:center;
            transform:translateY(-50%);max-height:min(64%,520px)}
.ai-jumpbar:hover{--ai-jump-dot:color-mix(in srgb,var(--text-tertiary) 78%,transparent)}
.ai-jump-track{display:flex;flex:0 1 auto;min-height:0;overflow:hidden;flex-direction:column;align-items:center;
               justify-content:space-between;width:100%}
.ai-jump-item{display:grid;flex:0 1 12px;width:13px;min-height:4px;padding:0;place-items:center;border:none;
              background:transparent;cursor:default}
.ai-jump-item::before{content:'';width:7px;height:2px;border-radius:1px;background:var(--ai-jump-dot);
              transition:width 120ms ease,background-color 120ms ease}
.ai-jump-item:hover::before{width:11px;background:var(--text-primary)}
.ai-jump-item.active::before{width:11px;background:color-mix(in srgb,var(--accent-violet) 78%,transparent)}
/* 悬停预览浮层: 挂 body + fixed, 不被对话流裁剪 */
.ai-jump-tip{position:fixed;left:0;top:0;z-index:12000;max-width:min(280px,calc(100vw - 24px));padding:8px 10px;
             border:1px solid var(--glass-border);border-radius:6px;background:var(--surface-raised);
             box-shadow:0 8px 24px rgba(0,0,0,.24);opacity:0;pointer-events:none;transition:opacity 120ms ease}
.ai-jump-tip.visible{opacity:1}
.ai-jump-tip-round{font-size:11px;color:var(--text-tertiary)}
.ai-jump-tip-text{display:-webkit-box;margin-top:3px;overflow:hidden;-webkit-box-orient:vertical;-webkit-line-clamp:3;line-clamp:3;
                  font-size:12px;line-height:1.5;color:var(--text-primary);overflow-wrap:anywhere}
/* 跳转后目标轮短暂高亮 */
.ai-msg.ai-jump-flash .ai-bubble{animation:ai-jump-flash 1500ms ease-out}
@keyframes ai-jump-flash{0%,12%{box-shadow:0 0 0 2px color-mix(in srgb,var(--accent-violet) 55%,transparent)}100%{box-shadow:0 0 0 2px transparent}}

/* ---- 空态 ---- */
.ai-empty{display:flex;flex-direction:column;align-items:center;gap:10px;padding:52px 20px 40px;text-align:center}
.ai-empty-icon{font-size:32px;color:var(--accent-violet);opacity:.62}
.ai-empty-title{font-size:14px;color:var(--text-primary)}
.ai-empty-desc{max-width:460px;font-size:12px;line-height:1.75;color:var(--text-secondary)}
.ai-suggestions{display:flex;flex-wrap:wrap;justify-content:center;gap:8px;margin-top:6px}
.ai-suggestion{padding:6px 11px;border:1px solid var(--glass-border);border-radius:999px;background:var(--surface-raised);
               color:var(--text-secondary);font:inherit;font-size:12px;cursor:default;
               transition:background-color 120ms ease,color 120ms ease,border-color 120ms ease}
.ai-suggestion:hover{border-color:color-mix(in srgb,var(--accent-violet) 45%,var(--glass-border));
                     background:var(--btn-secondary-hover);color:var(--text-primary)}
.ai-donate-row{display:flex;align-items:center;justify-content:center;gap:10px;margin-top:10px}
.ai-donate{padding:0 2px;background:none;border:none;color:var(--text-tertiary);
           font:inherit;font-size:11px;cursor:default;text-decoration:underline dotted;
           transition:color 120ms ease}
.ai-donate:hover{color:var(--accent-violet)}
.ai-donate-sep{color:var(--text-tertiary);font-size:11px}

/* ---- 捐赠二维码 (AI 经 get_donate_qr 工具拿到引用语法, 回答里的 ![..](xjs://donate?kind=..)
   由 C++ md 渲染层换成缓存真图; 这里只管样式: 块级竖排居中 (屏幕窄不并列), 白底保扫得动) ---- */
.ai-donate-qr{display:block;margin:10px auto;width:min(220px,62%);object-fit:contain;
              background:#fff;border-radius:8px;padding:6px;cursor:default}

/* ==================== 输入区 ====================
   整个区域文本指针: 留白也是"点一下就能继续打字"的地方 */
.ai-composer{flex:0 0 auto;padding:12px 20px 14px;cursor:text}
.ai-composer-inner{width:100%;max-width:min(1600px,100%);margin:0 auto}
.ai-composer-box{--r:var(--ai-composer-radius);position:relative;display:flex;flex-direction:column;gap:6px;
                 padding:9px 9px 7px 12px;border:1px solid var(--glass-border);border-radius:var(--r);
                 background:var(--surface-raised);transition:border-color 120ms ease}
.ai-composer-box:focus-within{border-color:color-mix(in srgb,var(--accent-violet) 55%,var(--glass-border))}
/* 生成中: 边框流光 (聚焦染色让位 — 底色整圈变紫会把转动的亮弧抹平) */
@property --ai-composer-flow-angle{syntax:"<angle>";inherits:false;initial-value:0deg}
)AIWEBUI"
           LR"AIWEBUI(.ai-composer-box[data-streaming="true"]:focus-within{border-color:var(--glass-border)}
.ai-composer-box[data-streaming="true"]::before{content:'';position:absolute;inset:-1px;border-radius:calc(var(--r) + 1px);padding:1px;
  background:conic-gradient(from var(--ai-composer-flow-angle),transparent 0turn,transparent .58turn,
    color-mix(in srgb,var(--accent-violet) 40%,transparent) .74turn,
    color-mix(in srgb,var(--accent-violet) 85%,#fff) .86turn,
    color-mix(in srgb,var(--accent-violet) 40%,transparent) .98turn,transparent 1turn);
  -webkit-mask-image:linear-gradient(#000 0 0),linear-gradient(#000 0 0);
  -webkit-mask-clip:content-box,border-box;-webkit-mask-composite:xor;
  mask-image:linear-gradient(#000 0 0),linear-gradient(#000 0 0);
  mask-clip:content-box,border-box;mask-composite:exclude;
  pointer-events:none;animation:ai-composer-flow 1700ms linear infinite}
@keyframes ai-composer-flow{to{--ai-composer-flow-angle:360deg}}
.ai-input{flex:0 0 auto;min-width:0;min-height:22px;max-height:132px;resize:none;border:0;background:transparent;
          color:var(--text-primary);font:inherit;font-size:12.5px;line-height:1.6;outline:none;overflow-y:hidden}
.ai-input::placeholder{color:var(--text-tertiary)}
/* ---- 工具条 (输入框容器内部底行): 左=对话级设置, 右=本轮状态与动作 ---- */
.ai-composer-bar{display:flex;align-items:center;gap:6px;min-width:0}
/* 发送/停止: 圆形图标按钮, 图标由 CSS 按 data-mode 绘制 */
.ai-send{display:flex;flex:0 0 auto;align-items:center;justify-content:center;width:28px;height:28px;padding:0;border:0;
         border-radius:50%;background:var(--accent-violet);color:#fff;font:inherit;cursor:default;
         transition:background-color 120ms ease,color 120ms ease}
.ai-send::before{content:'\E74A';font-family:'Segoe Fluent Icons','Segoe MDL2 Assets',sans-serif;font-size:13px;line-height:1}
.ai-send:hover{background:color-mix(in srgb,var(--accent-violet) 84%,#fff)}
.ai-send[data-mode="stop"]{background:var(--accent-pink)}
.ai-send[data-mode="stop"]::before{content:'\E71A';font-size:12px}
.ai-send[data-empty="true"]{background:var(--btn-secondary-hover);color:var(--text-tertiary)}
.ai-send[data-empty="true"]:hover{background:var(--btn-secondary-hover)}
.ai-send:disabled{pointer-events:none}
/* ---- 文件操作权限下拉 (四档; 收起只留当前档位按钮, 向上弹出) ---- */
.ai-cmd-policy{position:relative;display:flex;flex:0 0 auto}
.ai-cmd-policy-button{display:inline-flex;align-items:center;gap:5px;padding:2px 7px;border:1px solid var(--glass-border);
          border-radius:6px;background:transparent;color:var(--text-tertiary);font-family:inherit;font-size:11px;
          line-height:1.5;white-space:nowrap;cursor:default;outline:none;
          transition:background 120ms ease,color 120ms ease,border-color 120ms ease}
.ai-cmd-policy-button:hover,.ai-cmd-policy-button[aria-expanded="true"]{background:var(--btn-secondary-hover);color:var(--text-primary)}
.ai-cmd-policy-icon{flex:0 0 auto;font-size:11px}
.ai-cmd-policy-chevron{flex:0 0 auto;font-size:9px;transition:transform 120ms ease}
.ai-cmd-policy-button[aria-expanded="true"] .ai-cmd-policy-chevron{transform:rotate(180deg)}
/* 风险提示染在收起状态的按钮上: 允许=警告色, 禁用=中性灰 */
.ai-cmd-policy-button[data-policy="allow"]{border-color:color-mix(in srgb,var(--accent-amber) 45%,transparent);
          background:color-mix(in srgb,var(--accent-amber) 16%,transparent);
          color:color-mix(in srgb,var(--accent-amber) 82%,var(--text-primary))}
.ai-cmd-policy-button[data-policy="off"]{color:var(--text-secondary)}
.ai-cmd-policy-menu{position:absolute;left:0;bottom:calc(100% + 6px);z-index:16;display:grid;gap:4px;width:max-content;
          min-width:148px;max-width:280px;padding:4px;border-radius:7px;background:var(--surface-raised);
          box-shadow:inset 0 0 0 1px var(--overlay-border),0 8px 24px rgba(0,0,0,.28);cursor:default;
          opacity:1;transform:translateY(0) scale(1);transform-origin:left bottom;
          transition:opacity 120ms ease,transform 140ms cubic-bezier(.2,0,0,1)}
.ai-cmd-policy-menu.opening,.ai-cmd-policy-menu.closing{opacity:0;transform:translateY(5px) scale(.98);pointer-events:none}
.ai-cmd-policy-menu.closing{transition-duration:90ms}
.ai-cmd-policy-option{display:grid;grid-template-columns:16px minmax(0,1fr);align-items:start;gap:6px;width:100%;
          padding:5px 8px;border:0;border-radius:4px;background:transparent;color:var(--text-secondary);
          font:inherit;font-size:12px;line-height:1.5;text-align:left;cursor:default;outline:none}
.ai-cmd-policy-option:hover{background:var(--btn-secondary-hover);color:var(--text-primary)}
.ai-cmd-policy-option[aria-checked="true"]{background:color-mix(in srgb,var(--accent-violet) 14%,transparent);color:var(--text-primary)}
.ai-cmd-policy-check{color:transparent;font-size:11px;line-height:1.6}
)AIWEBUI"
           LR"AIWEBUI(.ai-cmd-policy-option[aria-checked="true"] .ai-cmd-policy-check{color:var(--accent-violet)}
.ai-cmd-policy-option-text{display:flex;flex-direction:column;min-width:0}
.ai-cmd-policy-option-label{white-space:nowrap}
.ai-cmd-policy-option-hint{color:var(--text-tertiary);font-size:10.5px;line-height:1.4}
.ai-cmd-policy-option[aria-checked="true"] .ai-cmd-policy-option-hint{color:var(--text-secondary)}
/* ---- 用量简况 (上下文占用条 + 剩余比例; 点开看详情) ---- */
.ai-usage{display:inline-flex;flex:0 0 auto;align-items:center;gap:7px;min-width:0;margin-left:auto;padding:2px 6px;
          border:1px solid transparent;border-radius:5px;background:transparent;color:var(--text-tertiary);
          font-family:inherit;font-size:11px;font-variant-numeric:tabular-nums;white-space:nowrap;cursor:default;
          transition:background 120ms ease,border-color 120ms ease,color 120ms ease}
.ai-usage:hover,.ai-usage[aria-expanded="true"]{border-color:var(--glass-border);background:var(--btn-secondary-hover);
          color:var(--text-secondary)}
.ai-usage-bar{position:relative;display:inline-block;flex:0 0 auto;overflow:hidden;width:34px;height:3px;border-radius:2px;
          background:color-mix(in srgb,var(--text-tertiary) 26%,transparent)}
.ai-usage-bar-fill{position:absolute;top:0;bottom:0;left:0;width:0;border-radius:2px;
          background:color-mix(in srgb,var(--accent-violet) 70%,transparent);transition:width 240ms ease,background-color 240ms ease}
.ai-usage[data-warn="true"] .ai-usage-bar-fill{background:var(--accent-amber)}
.ai-usage[data-warn="true"] .ai-usage-brief{color:color-mix(in srgb,var(--accent-amber) 85%,var(--text-secondary))}
.ai-usage-brief{overflow:hidden;color:var(--text-secondary);white-space:nowrap;text-overflow:ellipsis}
.ai-usage-caret{flex:0 0 auto;font-size:8px;color:var(--text-tertiary);transition:transform 160ms ease}
.ai-usage[aria-expanded="true"] .ai-usage-caret{transform:rotate(180deg)}
/* ---- 用量详情浮层 (fixed 贴按钮上方, 放不下翻下方) ---- */
.ai-usage-panel{position:fixed;left:0;top:0;z-index:12000;min-width:200px;padding:8px 10px;border:1px solid var(--glass-border);
          border-radius:6px;background:var(--surface-raised);box-shadow:0 8px 24px rgba(0,0,0,.24);font-size:11.5px;
          font-variant-numeric:tabular-nums;cursor:default;opacity:0;pointer-events:none;transition:opacity 120ms ease}
.ai-usage-panel.visible{opacity:1;pointer-events:auto}
.ai-usage-row{display:flex;align-items:baseline;justify-content:space-between;gap:16px;padding:2px 0}
.ai-usage-row + .ai-usage-row{border-top:1px solid color-mix(in srgb,var(--divider) 60%,transparent)}
.ai-usage-group{margin:6px 0 2px;color:var(--text-secondary);font-size:10.5px;font-weight:600;letter-spacing:.3px}
.ai-usage-group:first-child{margin-top:0}
.ai-usage-group + .ai-usage-row{border-top:0}
.ai-usage-row-label{color:var(--text-tertiary)}
.ai-usage-row-value{color:var(--text-primary);font-weight:600}
.ai-usage-row[data-empty="true"] .ai-usage-row-value{color:var(--text-tertiary);font-weight:400}
.ai-usage-row-value[data-good="true"]{color:color-mix(in srgb,var(--accent-emerald) 80%,var(--text-primary))}
.ai-usage-row-value[data-warn="true"]{color:var(--accent-amber)}
.ai-usage-note{margin-top:5px;color:var(--text-tertiary);font-size:10.5px;line-height:1.45}

/* ==================== 历史对话 (右侧边栏, 可折叠) ==================== */
.ai-history-sidebar{display:flex;flex:0 0 232px;flex-direction:column;gap:8px;width:232px;min-height:0;
          padding:14px 16px 14px 12px;border-left:1px solid var(--glass-border);
          background:color-mix(in srgb,var(--surface-raised) 55%,var(--bg))}
.ai-history-head{display:flex;align-items:center;justify-content:space-between;gap:8px;flex:0 0 auto}
.ai-history-title{font-size:12px;color:var(--text-secondary)}
.ai-history-clear{height:24px;padding:0 8px;font-size:11px}
.ai-history-list{display:flex;flex:1 1 auto;flex-direction:column;gap:4px;min-height:0;overflow-y:auto}
.ai-history-item{display:flex;align-items:center;gap:8px;padding:7px 8px 7px 10px;border:1px solid transparent;
          border-radius:6px;cursor:default;transition:background 120ms ease}
.ai-history-item:hover{background:color-mix(in srgb,var(--accent-violet) 10%,transparent)}
.ai-history-item-current{border-color:color-mix(in srgb,var(--accent-violet) 35%,transparent);
          background:color-mix(in srgb,var(--accent-violet) 7%,transparent)}
/* 正在生成的对话: 标题前一个脉冲点 */
.ai-history-item-streaming .ai-history-item-title::before{content:'';display:inline-block;width:5px;height:5px;
          margin-right:6px;border-radius:50%;background:var(--accent-violet);vertical-align:middle;
          animation:ai-history-stream-pulse 1.2s ease-in-out infinite}
@keyframes ai-history-stream-pulse{0%,100%{opacity:.35;transform:scale(.75)}50%{opacity:1;transform:scale(1)}}
.ai-history-item-text{flex:1 1 auto;min-width:0}
)AIWEBUI"
           LR"AIWEBUI(.ai-history-item-title{overflow:hidden;font-size:12px;color:var(--text-primary);text-overflow:ellipsis;white-space:nowrap}
.ai-history-item-time{margin-top:2px;font-size:11px;color:var(--text-tertiary)}
.ai-history-item-delete{display:grid;flex:0 0 auto;width:22px;height:22px;place-items:center;border:none;border-radius:4px;
          background:transparent;color:var(--text-tertiary);cursor:default;font-size:12px;
          transition:background 120ms ease,color 120ms ease}
.ai-history-item-delete:hover{background:color-mix(in srgb,#e81123 14%,transparent);color:#e81123}
.ai-history-empty{flex:0 0 auto;padding:18px 4px;font-size:12px;color:var(--text-tertiary);text-align:center}
/* 窄面板分级收缩: ≤640 先隐状态文字 (保留连接色点) → ≤520 工具条按钮只留图标
   (与 .icononly 同款内边距, 动作语义由 title 悬停提示保留); 标题可截断兜底,
   任何宽度下右侧按钮都不得挤出可视区 */
@media (max-width:640px){
  #stText{display:none}
}
@media (max-width:520px){
  .ai-toolbar{padding:12px;gap:10px}
  .ai-btn{padding:0 9px}
  .ai-btn .ellipsis-text{display:none}
}
/* 窄窗口: 侧边栏悬浮在对话区之上, 不再挤压主列 */
@media (max-width:760px){
  .ai-history-sidebar{position:absolute;z-index:40;width:260px;flex-basis:260px;top:0;bottom:0;right:0;
          box-shadow:0 8px 28px rgba(0,0,0,.28)}
}
/* ==================== 页面内 Toast (宿主 Toast 被面板上的浏览器子窗盖住, 提示一律画在页面里;
   顶部居中浮层: 设置面板/对话区两态都可见, 也不与 composer 向上弹的浮层相撞) ==================== */
.ai-toast{position:fixed;left:50%;top:72px;transform:translateX(-50%) translateY(-8px);z-index:14000;
          max-width:min(420px,calc(100vw - 32px));padding:8px 14px;border:1px solid var(--overlay-border);
          border-radius:8px;background:color-mix(in srgb,var(--surface-raised) 94%,var(--bg));
          box-shadow:0 8px 24px rgba(0,0,0,.24);font-size:12px;line-height:1.5;color:var(--text-primary);
          opacity:0;pointer-events:none;transition:opacity 160ms ease,transform 160ms ease}
.ai-toast.visible{opacity:1;transform:translateX(-50%) translateY(0)}
.ai-toast::before{content:'';display:inline-block;width:7px;height:7px;margin-right:8px;border-radius:50%;
          background:var(--accent-violet);vertical-align:1px}
.ai-toast[data-kind="ok"]::before{background:var(--accent-emerald)}
.ai-toast[data-kind="warn"]::before{background:var(--accent-amber)}
.ai-toast[data-kind="err"]::before{background:var(--accent-pink)}
</style>
</head>
<body>
<div id="app">
  <div class="ai-main">
    <div class="ai-toolbar">
      <h1 class="ai-toolbar-title">AI 助手</h1>
      <span class="ai-toolbar-status">
        <span class="ai-status-dot" id="stDot" data-state="missing" aria-hidden="true"></span>
        <span class="ellipsis-text" id="stText">未配置接口</span>
      </span>
      <div class="ai-toolbar-actions">
        <!-- 模型切换: 点开列出全部档案, 选中即生效; 管理动作在接口设置面板里 -->
        <div class="ai-model-picker" id="modelPicker">
          <button class="ai-btn ai-model-picker-button" id="modelBtn" type="button"
            aria-haspopup="listbox" aria-expanded="false" title="模型">
            <span class="glyph" aria-hidden="true">&#xE99A;</span><span class="ellipsis-text" id="modelBtnLabel"></span><span class="ai-model-caret glyph" aria-hidden="true">&#xE70D;</span>
          </button>
          <div class="ai-model-menu" id="modelMenu" role="listbox" hidden></div>
        </div>
        <button class="ai-btn" id="b-set" type="button" title="接口设置"><span class="glyph" aria-hidden="true">&#xE713;</span><span class="ellipsis-text">接口设置</span></button>
        <button class="ai-btn" id="b-hist" type="button" title="历史对话"><span class="glyph" aria-hidden="true">&#xE81C;</span><span class="ellipsis-text">历史对话</span></button>
        <button class="ai-btn" id="b-new" type="button" title="新对话"><span class="glyph" aria-hidden="true">&#xE72C;</span><span class="ellipsis-text">新对话</span></button>
        <button class="ai-btn icononly" id="b-close" type="button" title="关闭"><span class="glyph" aria-hidden="true">&#xE8BB;</span></button>
      </div>
    </div>

    <!-- 接口设置: 管理模型档案 (每条档案是一套完整接口配置, 跨服务商时地址与密钥随档案走) -->
    <div class="ai-config-panel" id="cfgPanel" hidden>
      <div class="ai-config-row"><label class="ai-config-label" for="f-prof">模型</label>
        <div class="ai-config-profile-row">
          <select class="ai-config-input" id="f-prof"></select>
          <button class="ai-btn" id="f-prof-add" type="button">新建</button>
          <button class="ai-btn" id="f-prof-dup" type="button">复制</button>
          <button class="ai-btn" id="f-prof-del" type="button" data-armed="false">删除</button>
        </div></div>
      <div class="ai-config-row"><label class="ai-config-label" for="f-name">名称</label>
        <input class="ai-config-input" id="f-name" type="text" spellcheck="false" autocomplete="off" placeholder="例如：DeepSeek 官方"></div>
      <div class="ai-config-row"><label class="ai-config-label" for="f-url">接口地址</label>
        <input class="ai-config-input" id="f-url" type="text" spellcheck="false" autocomplete="off" placeholder="https://api.deepseek.com"></div>
      <div class="ai-config-row"><label class="ai-config-label" for="f-key">API 密钥</label>
        <input class="ai-config-input" id="f-key" type="password" spellcheck="false" autocomplete="off" placeholder="sk-..."></div>
      <div class="ai-config-row"><label class="ai-config-label" for="f-model">模型</label>
        <input class="ai-config-input" id="f-model" type="text" spellcheck="false" autocomplete="off" placeholder="deepseek-flash"></div>
      <div class="ai-config-row"><label class="ai-config-label" for="f-ctx">上下文长度</label>
        <input class="ai-config-input" id="f-ctx" type="text" inputmode="numeric" spellcheck="false" autocomplete="off" placeholder="留空则自动推断"></div>
      <div class="ai-config-row"><label class="ai-config-label" for="f-max">最大输出</label>
        <input class="ai-config-input" id="f-max" type="text" inputmode="numeric" spellcheck="false" autocomplete="off" placeholder="留空则用服务端默认"></div>
      <div class="ai-config-row"><span class="ai-config-label" aria-hidden="true"></span>
        <button class="ai-reason-toggle" id="f-reason" type="button" aria-pressed="false"><span class="ai-reason-box" aria-hidden="true"></span>深度思考 (reasoning.effort=high)</button></div>
      <div class="ai-config-row"><span class="ai-config-label" aria-hidden="true"></span>
        <span class="ai-config-hint">两个长度都填 token 数，支持 128K、1M 这类写法。上下文长度只影响用量的“剩余”显示（留空则按模型名推断）；最大输出留空时不发送该参数。</span></div>
      <div class="ai-config-row"><span class="ai-config-label" aria-hidden="true"></span>
        <span class="ai-config-hint">密钥以本机 GUID 为密码加密后只存在本机配置文件里，换机或分享配置均无法解出；留空保存 = 保留已存密钥。接口需兼容 OpenAI Responses 协议 (/responses)。</span></div>
      <div class="ai-config-actions">
        <button class="ai-btn" id="b-cancel" type="button">取消</button>
        <button class="ai-btn ai-btn-primary" id="b-save" type="button">保存</button>
      </div>
    </div>

    <div class="ai-thread-wrap">
      <div class="ai-thread" id="thread">
        <div class="ai-thread-inner" id="threadInner"></div>
      </div>
      <div class="ai-jumpbar" id="jumpbar" hidden><div class="ai-jump-track" id="jumpTrack"></div></div>
    </div>

    <div class="ai-composer">
      <div class="ai-composer-inner">
        <div class="ai-composer-box" id="cbox">
          <textarea class="ai-input" id="inputT" rows="1" aria-label="给 AI 助手的消息"
            placeholder="描述你的目标，例如：找出一周内修改过的文档"></textarea>
          <div class="ai-composer-bar">
            <div class="ai-cmd-policy" id="policy">
              <button class="ai-cmd-policy-button" id="policyBtn" type="button" aria-haspopup="listbox" aria-expanded="false">
                <span class="ai-cmd-policy-icon glyph" aria-hidden="true">&#xE756;</span>
                <span id="policyLabel"></span>
                <span class="ai-cmd-policy-chevron glyph" aria-hidden="true">&#xE70D;</span>
              </button>
              <div class="ai-cmd-policy-menu" id="policyMenu" role="listbox" aria-label="文件操作权限" hidden>
                <button class="ai-cmd-policy-option" type="button" data-policy="0" role="option" aria-checked="false">
                  <span class="ai-cmd-policy-check glyph" aria-hidden="true">&#xE73E;</span>
)AIWEBUI"
           LR"AIWEBUI(                  <span class="ai-cmd-policy-option-text"><span class="ai-cmd-policy-option-label"></span><span class="ai-cmd-policy-option-hint"></span></span></button>
                <button class="ai-cmd-policy-option" type="button" data-policy="1" role="option" aria-checked="false">
                  <span class="ai-cmd-policy-check glyph" aria-hidden="true">&#xE73E;</span>
                  <span class="ai-cmd-policy-option-text"><span class="ai-cmd-policy-option-label"></span><span class="ai-cmd-policy-option-hint"></span></span></button>
                <button class="ai-cmd-policy-option" type="button" data-policy="2" role="option" aria-checked="true">
                  <span class="ai-cmd-policy-check glyph" aria-hidden="true">&#xE73E;</span>
                  <span class="ai-cmd-policy-option-text"><span class="ai-cmd-policy-option-label"></span><span class="ai-cmd-policy-option-hint"></span></span></button>
                <button class="ai-cmd-policy-option" type="button" data-policy="3" role="option" aria-checked="false">
                  <span class="ai-cmd-policy-check glyph" aria-hidden="true">&#xE73E;</span>
                  <span class="ai-cmd-policy-option-text"><span class="ai-cmd-policy-option-label"></span><span class="ai-cmd-policy-option-hint"></span></span></button>
              </div>
            </div>
            <button class="ai-usage" id="usageBtn" type="button" aria-expanded="false">
              <span class="ai-usage-bar" aria-hidden="true"><i class="ai-usage-bar-fill" id="ubarf"></i></span>
              <span class="ai-usage-brief" id="ubrief"></span>
              <span class="ai-usage-caret glyph" aria-hidden="true">&#xE70D;</span>
            </button>
            <button class="ai-send" id="sendB" type="button" data-mode="send" aria-label="发送"></button>
          </div>
          <div class="ai-usage-panel" id="usagePanel" role="dialog" aria-label="用量详情"></div>
        </div>
      </div>
    </div>
  </div>

  <aside class="ai-history-sidebar" id="side" aria-label="历史对话" hidden>
    <div class="ai-history-head">
      <span class="ai-history-title">历史对话</span>
      <button class="ai-btn ai-history-clear" id="clearb" type="button" title="清空全部对话记录" hidden>清空记录</button>
    </div>
    <div class="ai-history-list" id="sideList"></div>
    <div class="ai-history-empty" id="sideEmpty" hidden>暂无历史对话</div>
  </aside>

  <!-- 页面内 Toast (宿主 Toast 被浏览器子窗盖住; 恒在 DOM, 显隐走 .visible, pointer-events:none 不挡点击) -->
  <div class="ai-toast" id="toast" role="status" aria-live="polite"></div>
</div>
<script>
'use strict';
/* ==================== 工具 ==================== */
const $=id=>document.getElementById(id);
/* JS→C++ 命令唯一通道。必须发对象本体 (不能 JSON.stringify): C++ 端取
   WebMessageAsJson 后按 t==5 判对象, 发字符串会被当成 JSON 串拒收 */
function post(o){try{chrome.webview.postMessage(o);}catch(e){}}
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
/* 紧凑数字: 1234 -> 1.23K, 1234567 -> 1.23M (千位凑成 1000.0K 时进位成 M) */
function fmtTokens(n){n=Math.max(0,Math.round(n||0));if(n<1000)return String(n);
  const k=(n/1000).toFixed(n<10000?2:1);if(Number(k)<1000)return k+'K';return (n/1000000).toFixed(2)+'M';}
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
const EMPTY_HTML='<div class="ai-empty" id="empty">'
  +'<span class="ai-empty-icon glyph" aria-hidden="true">&#xE99A;</span>'
  +'<span class="ai-empty-title">用对话来查找和整理文件</span>'
  +'<span class="ai-empty-desc">我可以读取索引库的全部实时数据（文件名、路径、大小、时间、分类），直接执行搜索并打开文件；每一步工具调用都会以卡片展示。</span>'
  +'<div class="ai-suggestions">'+SUGGS.map(q=>'<button class="ai-suggestion" type="button">'+esc(q)+'</button>').join('')+'</div>'
  +'<span class="ai-donate-row">'
  +'<button class="ai-donate" type="button" data-q="介绍一下作者和这个软件">👤 关于作者</button>'
  +'<span class="ai-donate-sep">·</span>'
  +'<button class="ai-donate" type="button" data-q="我是土豪，我要捐赠">💛 我是土豪，我要捐赠</button>'
  +'</span>'
  +'</div>';
/* 文件操作权限四档 (对话级; 档位名 + 各档含义 — 权限必须能看懂每档会做什么) */
const POLICY=[
  {k:'off',    n:'禁用', h:'不允许 AI 执行任何文件操作'},
  {k:'readonly',n:'只读', h:'只自动执行读取类操作，其余拒绝'},
)AIWEBUI"
           LR"AIWEBUI(  {k:'ask',    n:'询问', h:'每次写入或删除前先询问，确认后才执行'},
  {k:'allow',  n:'允许', h:'所有文件操作都直接执行，不询问'},
];

/* ==================== 状态 ==================== */
const S={
  cfg:{url:'',model:'',hasKey:false,reasoning:false,policy:2,name:'',ctx:0,maxOut:0,active:'',profs:[]},
  pal:null, convs:[], msgs:[], cur:0,
  sending:false, net:0, phase:0,
  usage:{has:false,up:0,uo:0,ut:0,uch:0,lp:0,lc:0,tps:0},
  sideOpen:false, cfgOpen:false, policyOpen:false, usageOpen:false, ctxOpen:false, modelOpen:false,
  profDirty:false,  /* 表单里有未保存的编辑: 挡住 C++ 整包下发把正在敲的内容冲掉 */
  delArmed:false, delTimer:0, modelTimer:0,
  clearArmed:false, clearTimer:0,   /* 清空记录两步确认 (首击待确认, 4s 内再击才清) */
  openSteps:{},     /* 展开的工具卡片样本: convId+':'+msgIdx → true */
  toolGrp:{},       /* 展开的连续工具组: convId+':'+k0 → true (缺省 = 含待确认卡才展开) */
  reasonOpen:{},    /* 手动展开的推理块: msgIdx → true (流式自动展开之外的覆盖) */
  turnLog:{},       /* 过程面板开合: convId+':'+组起点 → true 展开/false 收起
                       (undefined=缺省: 流式中最新回合展开、其余收起; 正文开始自动落 false) */
  follow:true,      /* 生成期间跟随滚动 (用户手动上滚即停) */
  policyTimer:0, copiedTimer:0, toastTimer:0,
  jumpRounds:[], jumpFlashTimer:0, jumpFrame:0, jumpTip:null,
};

/* ==================== 调色注入 (键 = 皮肤派生五色 + 语义色; 其余派生色由 CSS color-mix 现算) ==================== */
function applyPal(p){
  if(!p) return;
  const r=document.documentElement.style;
  const set=(k,v)=>r.setProperty('--'+k,v);
  set('bg',p.bg); set('surface-raised',p.panel);
  set('text-primary',p.text); set('text-secondary',p.dim); set('text-tertiary',p.t3);
  set('accent-violet',p.accent); set('accent-cyan',p.cyan); set('accent-emerald',p.emerald);
  set('accent-amber',p.amber); set('accent-pink',p.red);
  set('glass-border',p.border); set('overlay-border',p.borderStrong);
  set('divider',p.divider); set('btn-secondary-hover',p.hover);
  set('ai-user-accent',p.userAcc);
}

/* ==================== 状态点 ==================== */
function renderStatus(){
  const complete=S.cfg.url&&S.cfg.model&&S.cfg.hasKey;
  /* 状态里用档案显示名而不是裸模型名: 与工具栏下拉同一套取名, 切模型后才能对上 */
  const nm=S.cfg.name||S.cfg.model||'未配置接口';
  let st,txt;
  if(S.sending){st='busy';txt='正在与 '+nm+' 对话';}
  else if(!complete){st='missing';txt='未配置接口';}
  else if(S.net===2){st='error';txt='上次请求失败';}
  else {st='ready';txt='已连接 '+nm;}
  $('stDot').setAttribute('data-state',st);
  $('stText').textContent=txt;
}

/* ==================== 模型档案 (多模型切换; 源样式 aiProfiles 同构) ====================
 * 宿主 (C++) 是权威副本: 页面的增删改都发命令回传, cfg 广播整包覆盖本地。
 * 密钥不在其中 (只见 hasKey): 复制/切换档案时密钥在 C++ 侧搬运。 */
function profById(id){return S.cfg.profs.find(p=>p.id===id)||null;}
function profActive(){return profById(S.cfg.active)||(S.cfg.profs[0]||null);}
/* 显示名: 用户没起名时用模型名兜底, 两者都没有才说"未命名模型" (与 C++ CfgDisplayName 同款) */
function profName(p){return p?(p.name||p.model||'未命名模型'):'未配置接口';}
/* token 长度: 空=0(未指定), 支持 128K/1M 简写; 非法=null (调用方提示, 不静默改值) */
function parseTok(t){
  const raw=String(t==null?'':t).trim().replace(/[,，\s]/g,'');
  if(!raw)return 0;
  const m=/^(\d+(?:\.\d+)?)([kKmM])?$/.exec(raw);
  if(!m)return null;
  const u=(m[2]||'').toLowerCase(),v=Number(m[1])*(u==='m'?1000000:u==='k'?1000:1);
  if(!isFinite(v)||v<=0)return null;
  return Math.round(Math.max(1,Math.min(100000000,v)));
}
function fmtTokInput(v){v=+v||0;return v>0?String(v):'';}
/* 活动档案的上下文窗口: 档案指定值优先, 否则按模型名推断 (宁可保守不要乐观) */
function ctxWin(){return S.cfg.ctx>0?S.cfg.ctx:ctxWindowFor(S.cfg.model);}
function fillProfForm(){
  const p=profActive();
  $('f-name').value=p?(p.name||''):'';
  $('f-url').value=p?(p.url||''):'';
  $('f-key').value='';   /* 密钥不回显 (宿主不给明文): 恒空, 留空保存 = 保留已存 */
  $('f-model').value=p?(p.model||''):'';
  $('f-ctx').value=fmtTokInput(p?p.ctx:0);
  $('f-max').value=fmtTokInput(p?p.maxOut:0);
  $('f-reason').setAttribute('aria-pressed',S.cfg.reasoning?'true':'false');
}
function renderProfSelect(){
  const sel=$('f-prof');sel.textContent='';
  const act=profActive();
  if(!S.cfg.profs.length){
    const o=document.createElement('option');o.value='';o.textContent='未配置接口';sel.appendChild(o);
  }else{
    S.cfg.profs.forEach(p=>{
      const o=document.createElement('option');o.value=p.id;o.textContent=profName(p);sel.appendChild(o);
    });
    sel.value=act?act.id:'';
  }
  /* 没有档案时管理按钮没有作用对象 */
  const has=S.cfg.profs.length>0;
  $('f-prof-dup').disabled=!has;
  $('f-prof-del').disabled=!has;
  $('f-prof-add').disabled=S.cfg.profs.length>=50;
}
/* 删除两步确认复位 (重开面板/收起/超时都归零) */
function profDisarm(){
  S.delArmed=false;clearTimeout(S.delTimer);
  const b=$('f-prof-del');b.textContent='删除';b.setAttribute('data-armed','false');
}
/* 切换当前档案: 本地即时生效 (广播随后确认), 切换是离散动作立即持久化 */
function selectProf(id){
  if(!profById(id))return;
  S.profDirty=false;
  S.cfg.active=id;
  const p=profActive();
  if(p){S.cfg.url=p.url||'';S.cfg.model=p.model||'';S.cfg.hasKey=!!p.hasKey;
        S.cfg.ctx=p.ctx||0;S.cfg.maxOut=p.maxOut||0;S.cfg.name=profName(p);}
  if(S.cfgOpen){renderProfSelect();fillProfForm();}
  renderModelBtn();renderStatus();renderUsage();
  post({c:'profActive',id});
}
function saveProf(){
  const ctx=parseTok($('f-ctx').value),max=parseTok($('f-max').value);
  if(ctx===null||max===null){   /* 任一写法非法就整体不保存, 不静默把合法的一半写进去 */
    showToast('长度需填写正整数 token 数（可写 128K、1M）','warn');
    return;
  }
  S.profDirty=false;
  post({c:'profSave',name:$('f-name').value.trim(),url:$('f-url').value.trim(),
        key:$('f-key').value.trim(),model:$('f-model').value.trim(),ctx,max,
        reasoning:$('f-reason').getAttribute('aria-pressed')==='true'});
  showToast('接口设置已保存','ok');
  cfgToggle(false);   /* 保存成功即收起面板 (校验失败才停留) */
}
)AIWEBUI"
           LR"AIWEBUI(/* ==================== 页面内 Toast (宿主 Toast 被浏览器子窗盖住; 页内已知消息直接调,
   C++ 已知消息经 t:"toast" 推送进来, 同一浮层) ==================== */
function showToast(msg,kind){
  const t=$('toast');
  t.textContent=msg;
  t.setAttribute('data-kind',kind||'info');
  t.classList.add('visible');
  clearTimeout(S.toastTimer);
  S.toastTimer=setTimeout(()=>t.classList.remove('visible'),2600);
}
/* ==================== 工具栏模型下拉 (切换之外的管理动作都在接口设置面板里) ==================== */
function renderModelBtn(){
  const label=profName(profActive());
  $('modelBtnLabel').textContent=label;
  $('modelBtn').title='模型：'+label;
}
function renderModelMenu(){
  const menu=$('modelMenu');menu.textContent='';
  const act=profActive();
  S.cfg.profs.forEach(p=>{
    const o=document.createElement('button');
    o.className='ai-model-option';o.type='button';
    o.setAttribute('data-pid',p.id);o.setAttribute('role','option');
    o.setAttribute('aria-checked',act&&p.id===act.id?'true':'false');
    /* 第二行放模型名: 两条档案显示名撞车时, 靠它才分得清 */
    o.innerHTML='<span class="ai-model-option-check glyph" aria-hidden="true">&#xE73E;</span>'
      +'<span class="ai-model-option-text"><span class="ai-model-option-name">'+esc(profName(p))+'</span>'
      +'<span class="ai-model-option-hint">'+esc(p.model||'未填写模型名')+'</span></span>';
    menu.appendChild(o);
  });
  const mg=document.createElement('button');
  mg.className='ai-model-option ai-model-option-manage';mg.type='button';
  mg.textContent='管理模型…';
  menu.appendChild(mg);
}
function modelSetOpen(open){
  S.modelOpen=open;
  const btn=$('modelBtn'),menu=$('modelMenu');
  btn.setAttribute('aria-expanded',open?'true':'false');
  clearTimeout(S.modelTimer);
  if(open){
    renderModelMenu();
    usageSetOpen(false);policySetOpen(false);   /* 互斥: 另外两个浮层也从工具区展开 */
    menu.classList.remove('closing');menu.classList.add('opening');menu.hidden=false;
    requestAnimationFrame(()=>requestAnimationFrame(()=>{
      if(btn.getAttribute('aria-expanded')==='true')menu.classList.remove('opening');
    }));
    return;
  }
  menu.classList.remove('opening');
  if(menu.hidden)return;
  menu.classList.add('closing');
  S.modelTimer=setTimeout(()=>{
    if(btn.getAttribute('aria-expanded')!=='true'){menu.hidden=true;menu.classList.remove('closing');}
  },90);
}

/* ==================== 接口设置面板 ==================== */
function cfgToggle(open){
  S.cfgOpen=open;
  if(open){
    /* 关闭面板 = 放弃未保存的编辑; 打开时从活动档案重填 */
    S.profDirty=false;profDisarm();
    renderProfSelect();fillProfForm();
    setTimeout(()=>{try{$('f-prof').focus();}catch(e){}},0);
  }
  $('cfgPanel').hidden=!open;
  $('b-set').setAttribute('aria-expanded',open?'true':'false');
}

/* ==================== 对话流 ==================== */
function threadEl(){return $('thread');}
function isNearBottom(){const t=threadEl();return t.scrollHeight-t.scrollTop-t.clientHeight<120;}
function scrollToEnd(){const t=threadEl();t.scrollTop=t.scrollHeight;}

function typingHtml(){return '<span class="ai-typing"><i></i><i></i><i></i></span>';}
function metaHtml(mi){
  return '<div class="ai-msg-meta" data-mi="'+mi+'">'
    +'<button class="ai-meta-btn glyph" data-act="retry" title="重试" aria-label="重试">&#xE72C;</button>'
    +'<button class="ai-meta-btn glyph" data-act="copy" title="复制" aria-label="复制">&#xE8C8;</button>'
    +'</div>';
}
function reasoningHtml(m,mi){
  const autoOpen=S.sending&&mi===S.msgs.length-1&&S.phase===0;
  const open=S.reasonOpen[mi]!==undefined?S.reasonOpen[mi]:autoOpen;
  return '<div class="ai-reasoning" data-open="'+(open?'true':'false')+'"'+(autoOpen?' data-thinking="true"':'')+'>'
    +'<button class="ai-reasoning-head" type="button" data-act="reason" data-mi="'+mi+'" aria-expanded="'+(open?'true':'false')+'">'
    +'<span class="ai-reasoning-ic glyph" aria-hidden="true">&#xE9D9;</span>'
    +'<span class="ai-reasoning-label">'+(autoOpen?'正在深度思考…':'已深度思考（推理过程）')+'</span>'
    +'<span class="ai-reasoning-chevron glyph" aria-hidden="true">&#xE76C;</span>'
    +'</button><div class="ai-reasoning-body"><div class="ai-reasoning-inner">'+esc(m.reason)+'</div></div></div>';
}
/* ---- 回合分组渲染 ----
 * 一轮提问之后的全部助手侧消息 (中间叙述文本 + 工具卡片 + 最终回答) 归为**一个** ai-msg 组:
 * 只有末尾一条是回答气泡, 前面的全部收进上方"过程区" (虚线分隔 + 淡色小字),
 * 不再每段叙述各自成泡 = 看起来像连答多次 (用户反馈)。
 * 过程区面板整体可收起 (头部一行), 正文开始自动收起 — 见 maybeAutoCollapseTurn。 */
function turnEnd(start){   /* 助手侧连续段 [start, end) */
  let e=start;
  while(e<S.msgs.length&&S.msgs[e].r!==0) e++;
  return e;
}
)AIWEBUI"
           LR"AIWEBUI(/* 正文开始 → 本回合过程面板自动收起 (2026-09-25 用户口径 "AI 开始回答正文时自动收缩"):
 * 末条消息首次出现非空正文 (打字点气泡有了文字) 时落账 false。只在尚无落账 (undefined) 时写 —
 * 用户手动开合过的面板不被自动行为覆盖。回合里只有这一条 (无过程面板) 不动。 */
function maybeAutoCollapseTurn(){
  if(!S.sending||!S.msgs.length)return;
  const m=S.msgs[S.msgs.length-1];
  if(m.r===2||m.r===0||m.empty||!m.html)return;
  let start=S.msgs.length-1;
  while(start>0&&S.msgs[start-1].r!==0)start--;
  if(start===S.msgs.length-1)return;
  const key=S.cur+':'+start;
  if(S.turnLog[key]===undefined)S.turnLog[key]=false;
}
function turnPartHtml(m,mi,isFinal){
  if(m.r===2) return m.html||'';   /* 工具卡片组 (C++ 生成的 ai-steps; 折叠态见 .step) */
  if(!isFinal){
    /* 中间叙述: 淡色小字过程区, 无气泡外框; 无正文的消息只画推理行
       (empty=C++ 判定没有正文 — 不看它, &nbsp; 占位气泡会渲染成空行, 2026-09-25 实锤) */
    if(m.empty||!m.html) return m.reason?reasoningHtml(m,mi):'';
    return (m.reason?reasoningHtml(m,mi):'')+'<div class="ai-turn-note">'+m.html+'</div>';
  }
  let inner='';
  if(S.sending&&mi===S.msgs.length-1&&S.phase===0&&m.empty){
    inner+='<div class="ai-bubble">'+typingHtml()+'</div>';   /* 正在生成: 三点在气泡内 */
  }else{
    inner+=m.html||'<div class="ai-bubble">&nbsp;</div>';
    if(m.r===1&&!(S.sending&&mi===S.msgs.length-1)) inner+=metaHtml(mi);   /* 结尾行只给定稿回答 */
  }
  return inner;
}
function turnGroupHtml(start){
  const end=turnEnd(start);
  const finalIdx=end-1;
  const finalIsBubble=S.msgs[finalIdx].r!==2;   /* 回合内最后一条非工具 = 回答气泡;
                                                   工具执行期 (末条是卡片) 本回合暂无气泡 */
  let log='';
  for(let k=start;k<end;k++){
    if(finalIsBubble&&k===finalIdx){
      /* 最终回答的推理也归过程面板 (与中间轮同一行样式, 不再自成独立卡散在面板外) */
      if(S.msgs[k].reason) log+=reasoningHtml(S.msgs[k],k);
      continue;
    }
    const m=S.msgs[k];
    if(m.r!==2){ log+=turnPartHtml(m,k,false); continue; }
    /* 连续工具段: ≥2 次聚成一组整组折叠 (默认一行摘要, 点开展开全部卡片);
       单次仍是一张卡片。state 恒连续 append, k0 即组键 */
    let e2=k;
    while(e2<end&&S.msgs[e2].r===2) e2++;
    if(e2-k>=2) log+=toolGroupHtml(k,e2);
    else log+=turnPartHtml(m,k,false);
    k=e2-1;
  }
  let main='';
  if(log){
    /* 面板整体可收起 (2026-09-25 用户口径): 缺省 = 流式中的最新回合展开 (看得到进行中的过程)、
       其余 (已完成回合/历史载入) 收起; 正文开始自动收起 (maybeAutoCollapseTurn 落账),
       手动点头部开合落账后自动行为不再覆盖。收起只留头部一行 (标签+状态+箭头) */
    const isLive=S.sending&&end===S.msgs.length;
    const key=S.cur+':'+start;
    const open=S.turnLog[key]!==undefined?S.turnLog[key]:isLive;
    const sum=!isLive?'':(S.phase===1?'执行中…':'生成中…');
    main='<div class="ai-turn-log'+(open?' open':'')+'">'
        +'<button class="ai-turn-log-head" type="button" data-act="turnlog" aria-expanded="'+(open?'true':'false')+'">'
        +'<span class="ai-turn-log-ic glyph" aria-hidden="true">&#xE9D9;</span>'
        +'<span class="ai-turn-log-label">思考与工具调用</span>'
        +(sum?'<span class="ai-turn-log-sum">'+sum+'</span>':'')
        +'<span class="ai-turn-log-arr glyph" aria-hidden="true">&#xE70D;</span>'
        +'</button><div class="ai-turn-log-body">'+log+'</div></div>';
  }
  if(finalIsBubble) main+=turnPartHtml(S.msgs[finalIdx],finalIdx,true);
  return '<div class="ai-msg ai-msg-assistant" data-mi="'+start+'">'
    +'<span class="ai-msg-avatar glyph" aria-hidden="true">&#xE99A;</span>'
    +'<div class="ai-msg-main">'+main+'</div></div>';
}
/* 连续工具组: 头部 = 次数 + 聚合状态; 体 = 各卡片 (卡自身仍可单独展开看查询)。
 * 含待确认卡的组默认展开 (确认按钮必须可达), 其余默认折叠 */
function toolGroupHtml(k0,e2){
  const key=S.cur+':'+k0;
  let running=0,failed=0,ask=0,done=0;
  for(let k=k0;k<e2;k++){
    const st=S.msgs[k].steps&&S.msgs[k].steps[0];
    if(!st)continue;
    if(st.state===0||st.state===1)running++;
    else if(st.state===3)failed++;
    else if(st.state===4)ask++;
    else done++;
  }
  let cards='';
  for(let k=k0;k<e2;k++) cards+=S.msgs[k].html||'';
  /* 含待确认卡/待应用调整卡的组默认展开 (按钮必须可达); 其余默认折叠 */
  const wait=(cards.match(/data-act="adjApply"/g)||[]).length;
  const open=S.toolGrp[key]!==undefined?S.toolGrp[key]:(ask>0||wait>0);
  let sum=wait?('待应用 × '+wait):ask?('等待确认 × '+ask):(running?('执行中… × '+running)
    :(failed?(failed+' 次失败'+(done?' · 完成 '+done:'')):('完成 '+done+' 次')));
  return '<div class="ai-toolgrp'+(open?' open':'')+'" data-k0="'+k0+'">'
    +'<button class="ai-toolgrp-head" type="button" data-act="toolgrp" aria-expanded="'+(open?'true':'false')+'">'
    +'<span class="ai-toolgrp-label">工具调用 · '+(e2-k0)+' 次</span>'
    +'<span class="ai-toolgrp-sum'+(failed?' bad':(ask||wait?' wait':''))+'">'+sum+'</span>'
    +'<span class="ai-toolgrp-arr glyph" aria-hidden="true">&#xE70D;</span>'
    +'</button><div class="ai-toolgrp-body">'+cards+'</div></div>';
}
function userRowHtml(m,mi){
  return '<div class="ai-msg ai-msg-user" data-mi="'+mi+'">'
    +'<span class="ai-msg-avatar glyph" aria-hidden="true">&#xE77B;</span>'
    +'<div class="ai-msg-main">'+(m.html||'<div class="ai-bubble">&nbsp;</div>')+'</div></div>';
}
/* 短气泡收缩: 单段、无块级元素、纯文本 ≤30 字才不跟右缘对齐 */
)AIWEBUI"
           LR"AIWEBUI(function applyBubbleShapes(root){
  (root||$('threadInner')).querySelectorAll('.ai-msg-assistant .ai-bubble').forEach(b=>{
    const text=String(b.textContent||'').replace(/\s+/g,' ').trim();
    const block=b.querySelector('br,ul,ol,table,pre,blockquote,hr,h1,h2,h3,h4,h5,h6,div');
    b.classList.toggle('ai-bubble-short',!text||(!block&&b.querySelectorAll('p').length===1&&text.length<=30));
  });
}
/* 样本列表展开态回放 (msgs 全量重推后 JS 自持的展开态不丢; 箭头随开合转向) */
function applyOpenSteps(){
  document.querySelectorAll('#threadInner .step').forEach(card=>{
    const gi=+card.getAttribute('data-gi');
    const open=!!S.openSteps[S.cur+':'+gi];
    const body=card.querySelector('.ssamples');
    if(body) body.style.display=open?'':'none';
    card.classList.toggle('open',open);
  });
}
function renderThread(keepScroll){
  const inner=$('threadInner'), t=threadEl();
  const stick=keepScroll?isNearBottom():true;
  const showEmpty=!S.msgs.length&&!S.sending;
  $('jumpbar').hidden=true;
  if(showEmpty){ inner.innerHTML=EMPTY_HTML; updateJumpbar(); if(stick)scrollToEnd(); return; }
  let html='';
  for(let i=0;i<S.msgs.length;){
    if(S.msgs[i].r===0){ html+=userRowHtml(S.msgs[i],i); i++; continue; }
    html+=turnGroupHtml(i);          /* 助手侧连续段 = 一个回合组 */
    i=turnEnd(i);
  }
  inner.innerHTML=html;
  enhance(inner);
  applyBubbleShapes(); applyOpenSteps();
  if(stick){S.follow=true;scrollToEnd();}
  updateJumpbar();
}
/* 流式增量: 只换最后一个回合组 (回答/过程都长在组内, 组粒度替换与整帧重绘同观感) */
function applyLast(){
  if(!S.msgs.length)return;
  const inner=$('threadInner');
  const stick=isNearBottom();
  let start=S.msgs.length-1;
  while(start>0&&S.msgs[start-1].r!==0) start--;
  const old=inner.querySelector('.ai-msg[data-mi="'+start+'"]');
  const frag=document.createElement('div');
  frag.innerHTML=turnGroupHtml(start);
  const row=frag.firstChild;
  if(old)inner.replaceChild(row,old);else inner.appendChild(row);
  enhance(row);
  applyBubbleShapes(row); applyOpenSteps();
  if(stick){S.follow=true;scrollToEnd();}
  updateJumpbar();
}

/* ---- 会话内轮次跳转条: 刻度等距排列 (CSS flex 压缩), 悬停预览, 点击跳转 ---- */
function collectRounds(){
  const rounds=[];
  $('threadInner').querySelectorAll(':scope > .ai-msg-user').forEach(el=>{
    const b=el.querySelector('.ai-bubble');
    rounds.push({element:el,text:b?b.textContent.replace(/\s+/g,' ').trim():''});
  });
  return rounds;
}
function buildJumpTip(){
  if(S.jumpTip)return S.jumpTip;
  const tip=document.createElement('div');
  tip.className='ai-jump-tip';
  document.body.appendChild(tip);
  S.jumpTip=tip;
  return tip;
}
function showJumpTip(round,index){
  if(!round||!round.item)return;
  const tip=buildJumpTip();
  tip.innerHTML='<div class="ai-jump-tip-round">第 '+(index+1)+' 轮</div>';
  if(round.text){
    const tx=document.createElement('div');
    tx.className='ai-jump-tip-text';
    tx.textContent=round.text;
    tip.appendChild(tx);
  }
  /* 顺序: 填内容 → 测量 → 写坐标 → 加 visible (首帧就在正确位置) */
  const rect=round.item.getBoundingClientRect();
  const tr=tip.getBoundingClientRect();
  let left=rect.right+10;
  if(left+tr.width>window.innerWidth-8) left=rect.left-tr.width-10;
  if(left<8) left=8;
  let top=rect.top+rect.height/2-tr.height/2;
  top=Math.max(8,Math.min(top,window.innerHeight-tr.height-8));
  tip.style.left=Math.round(left)+'px';
  tip.style.top=Math.round(top)+'px';
  tip.classList.add('visible');
}
function hideJumpTip(){if(S.jumpTip)S.jumpTip.classList.remove('visible');}
function syncJumpActive(){
  const rounds=S.jumpRounds;
  if(!rounds.length)return;
  const t=threadEl();
  let active=0;
  const maxScroll=t.scrollHeight-t.clientHeight;
  if(maxScroll>0&&maxScroll-t.scrollTop<=2) active=rounds.length-1;   /* 已到底 = 末轮 */
  else{
    const marker=t.scrollTop+t.clientHeight*0.28;
    rounds.forEach((r,i)=>{if(r.element.offsetTop<=marker)active=i;});
  }
  rounds.forEach((r,i)=>r.item.classList.toggle('active',i===active));
}
function updateJumpbar(){
  hideJumpTip();
  const bar=$('jumpbar'),track=$('jumpTrack');
  const rounds=collectRounds();
  if(rounds.length<2){track.textContent='';S.jumpRounds=[];bar.hidden=true;return;}
  /* 轮次集合没变就复用现有刻度 (不重建 → 悬停预览/动画不被打断) */
  const keep=rounds.length===S.jumpRounds.length&&
    rounds.every((r,i)=>r.element===S.jumpRounds[i].element);
  if(keep){
    rounds.forEach((r,i)=>r.item=S.jumpRounds[i].item);
    S.jumpRounds=rounds;bar.hidden=false;syncJumpActive();return;
  }
  track.textContent='';
  S.jumpRounds=rounds;
  rounds.forEach((r,i)=>{
    const item=document.createElement('button');
    item.type='button';
    item.className='ai-jump-item';
    item.setAttribute('aria-label','跳转到第 '+(i+1)+' 轮');
    item.addEventListener('click',()=>jumpTo(r));
    item.addEventListener('mouseenter',()=>showJumpTip(r,i));
    item.addEventListener('mouseleave',hideJumpTip);
    track.appendChild(item);
    r.item=item;
  });
  bar.hidden=false;
  syncJumpActive();
}
)AIWEBUI"
           LR"AIWEBUI(/* 一轮包含的消息节点: 本轮用户提问起, 到下一轮提问之前 */
function roundMessages(roundElement){
  const nodes=[];
  let node=roundElement;
  while(node){
    if(node!==roundElement&&node.classList.contains('ai-msg-user'))break;
    if(node.classList.contains('ai-msg'))nodes.push(node);
    node=node.nextElementSibling;
  }
  return nodes;
}
function jumpTo(round){
  if(!round||!round.element)return;
  hideJumpTip();
  const t=threadEl();
  const top=round.element.getBoundingClientRect().top-t.getBoundingClientRect().top+t.scrollTop-12;
  t.scrollTo({top:Math.max(0,top),behavior:'smooth'});
  /* 目标轮短暂高亮 (用户消息 + 其后的助手/工具消息) */
  clearTimeout(S.jumpFlashTimer);
  const clear=()=>document.querySelectorAll('.ai-jump-flash').forEach(n=>n.classList.remove('ai-jump-flash'));
  clear();
  roundMessages(round.element).forEach(n=>n.classList.add('ai-jump-flash'));
  S.jumpFlashTimer=setTimeout(()=>{S.jumpFlashTimer=0;clear();},1600);
}

)AIWEBUI"
           LR"AIWEBUI(/* ==================== 可点击交互 (搜索卡片 / 语法高亮 / 路径链接 / 右键菜单) ====================
 * AI 决定点击的类型, 一律标准 Markdown 链接语法 (链接文字 = 给用户看的动作指引):
 * [..](xjs://search?text=..&mode=..) = 搜索卡片 (点击置入搜索框并按模式执行);
 * [..](xjs://open|reveal?id=<FileId>) = 文件动作链接 — 模型只输出引擎 FileId, 路径由
 * C++ 按 ID 解析 (path 参数 = 旧历史消息的路径版链接, 兼容受理); 正文里确有的绝对路径 =
 * 自动文件链接 (单击打开, 右键 打开/定位/复制); 其余照旧 (外链 openurl)。
 * enhance() 挂在每次消息 HTML 落地之后 (innerHTML 重建后节点全新, 幂等无需去重)。 */
const MODE_LABELS={wildcard:'通配符',regex:'正则',sql:'SQL',lua:'Lua过滤','lua-exec':'Lua执行',lua_exec:'Lua执行'};
/* 路径字符边界: 停在 空格/引号/尖括号/管道/星号/冒号/问号/正斜杠 + 全角标点与中文句读,
   其余 (含括号、逗号、点、&、汉字) 都是合法文件名字符, 种子照吞 —
   "美人鱼 (2016)\美人鱼 (2016).mkv"、"a, b & c.mkv" 才切得完整 */
const PATH_STOP=/[\s'"`<>|*\/:?\u3000-\u303F\uFF01-\uFF5E\u2010-\u2027]/;
const PATH_RE=new RegExp("(?<![A-Za-z0-9])(?:[A-Za-z]:[\\\\/]|\\\\\\\\)[^\\s'\"`<>|*/:?\\u3000-\\u303F\\uFF01-\\uFF5E\\u2010-\\u2027]+","g");   /* lookbehind 排除 https: 里的 "s:/" */
/* 路径含空格 ("C:\Program Files\App\x.exe"、"美人鱼 (2016)\美人鱼 (2016).mkv"、整句英文长文件名):
 * 种子切到空格后按词延伸 — 词内含 \ (路径续段) 或以"点+字母扩展名"收尾 (文件名) 才落锚记为路径;
 * 纯汉字词 = 正文开始, 硬停 (锚点之前的部分就是路径); 其余 ASCII 词先吃进继续看。
 * "共 12.5 GB" 的 12.5 不会被当扩展名 (限字母开头), "C:\a\b 是 x.txt 备份" 的"是"先断, 不会错并。 */
const PATH_CJKWORD=/^[\u3400-\u4DBF\u4E00-\u9FFF\uF900-\uFAFF]+$/;
function pathStopCh(ch){return PATH_STOP.test(ch);}
function extendPath(text,e){
  let best=e,k=e;
  for(;;){
    if(text[k]!==' ')break;
    let j=k+1;
    while(j<text.length&&!pathStopCh(text[j]))j++;
    if(j===k+1)break;
    const seg=text.slice(k+1,j);
    if(PATH_CJKWORD.test(seg))break;
    k=j;
    if(seg.indexOf('\\')>=0||/\.[A-Za-z][A-Za-z0-9]{0,4}$/.test(seg))best=k;
  }
  return best;
}
const LUA_KW=new Set('and break do else elseif end false for function goto if in local nil not or repeat return then true until while self'.split(' '));
const SQL_KW=new Set('select from where group by order having limit offset as and or not null is like ilike in between case when then else end distinct join left right inner outer cross on asc desc union all exists insert into values update set delete create table view index with cast interval now current_date current_timestamp'.split(' '));
function modeLabel(m){m=(m||'').toLowerCase();return MODE_LABELS[m]||m;}
/* xjs:// 动作链接解析: "xjs://search?text=..&mode=.." → {kind, p}; 值兼容 %-编码与裸中文
   (decodeURIComponent 失败原样用), 裸 & 会截断参数 — 提示词已要求 URL 编码 */
function xjsUrl(href){
  const m=/^xjs:\/\/([a-z]+)(?:\?(.*))?$/i.exec(String(href||'').trim());
  if(!m)return null;
  const p={};
  (m[2]||'').split('&').forEach(function(kv){
    const i=kv.indexOf('=');
    if(i<1)return;
    let v=kv.slice(i+1);
    try{v=decodeURIComponent(v.replace(/\+/g,'%20'));}catch(e){}
    p[kv.slice(0,i).toLowerCase()]=v;
  });
  return {kind:m[1].toLowerCase(),p:p};
}
/* Markdown 链接 → 可点击控件:
   xjs://search → 搜索卡片 (链接文字 = 卡片上的动作指引, 搜索词藏在 data-text);
   xjs://open|reveal → 文件动作链接 (.ai-path, 链接文字照 AI 写的指引显示, data-open 区分默认动作) */
function transformActionLinks(root){
  (root||$('threadInner')).querySelectorAll('a[href^="xjs://"]').forEach(a=>{
    const u=xjsUrl(a.getAttribute('href'));
    if(!u)return;
    if(u.kind==='search'){
      const q=(u.p.text||'').trim()||a.textContent.trim();
      if(!q)return;
      let mode=(u.p.mode||'').toLowerCase();
      if(mode==='lua_exec')mode='lua-exec';   /* run_search 枚举变体归一: lua=过滤脚本, lua_exec=执行模式 (链接名 lua-exec) */
      const chip=document.createElement('div');
      chip.className='ai-chip';chip.setAttribute('role','button');
      chip.setAttribute('data-text',q);
      if(mode)chip.setAttribute('data-mode',mode);
      chip.title='点击执行搜索: '+q+' · 右键更多操作';
      chip.innerHTML='<span class="ai-chip-ic glyph" aria-hidden="true">&#xE721;</span>'
        +'<span class="ai-chip-text">'+esc(a.textContent.trim()||q)+'</span>'
        +(mode?'<span class="ai-chip-mode">'+esc(modeLabel(mode))+'</span>':'')
        +'<span class="ai-chip-go glyph" aria-hidden="true">&#xE768;</span>';
      a.replaceWith(chip);
    }else if(u.kind==='open'||u.kind==='reveal'){
      const id=String(u.p.id||'').trim();
      const path=(u.p.path||'').trim();   /* path = 旧历史消息的路径版链接, 继续受理 */
      if(!id&&!path)return;
      const sp=document.createElement('span');
      sp.className='ai-path';
      if(id&&!/^\d+$/.test(id)){
        /* 模型写出的 ID 不是纯数字 (编造/近似, 如"…附近"): 渲染成禁用态, 点击给明确提示 */
        sp.setAttribute('data-badid','1');
        sp.title='链接无效: AI 未能给出准确的文件 ID, 无法打开';
      }else if(id){
        sp.setAttribute('data-id',id);
        if(u.kind==='reveal')sp.setAttribute('data-open','reveal');
        sp.title=(u.kind==='reveal'?'定位文件':'打开文件')+' · 右键更多操作';
      }else{
        sp.setAttribute('data-path',path);
        if(u.kind==='reveal')sp.setAttribute('data-open','reveal');
        sp.title=(u.kind==='reveal'?'定位文件':'打开文件')+' · 右键更多操作';
      }
      sp.textContent=a.textContent.trim()||path||('#'+id);
      a.replaceWith(sp);
    }
  });
}
/* Lua / SQL 轻量词法高亮 (注释/字符串/数字/关键字/函数调用; 颜色见 .tok-*):
   高亮只重涂 pre code 的内文, 复制按钮取的 data-code 原文不受影响 */
function hlApply(code,src,lang){
  const re=lang==='lua'
    ? /(--\[(?:=*)\[[\s\S]*?(?:\](?:=*)\]|$)|--[^\n]*)|(\[(?:=*)\[[\s\S]*?(?:\](?:=*)\]|$)|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')|(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|([A-Za-z_]\w*)|(\s+)|([\s\S])/g
    : /(--[^\n]*|\/\*[\s\S]*?(?:\*\/|$))|('(?:''|[^'\n])*'|"(?:""|[^"\n])*")|(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|([A-Za-z_]\w*)|(\s+)|([\s\S])/g;
  const kw=lang==='lua'?LUA_KW:SQL_KW;
  let html='',m;
  while((m=re.exec(src))){
    if(m[1])html+='<span class="tok-c">'+esc(m[1])+'</span>';
    else if(m[2])html+='<span class="tok-s">'+esc(m[2])+'</span>';
    else if(m[3])html+='<span class="tok-n">'+esc(m[3])+'</span>';
    else if(m[4]){
      const w=m[4],lo=w.toLowerCase();
      if(kw.has(lo))html+='<span class="tok-k">'+esc(w)+'</span>';
      else{let k=re.lastIndex;while(k<src.length&&(src[k]===' '||src[k]==='\t'))k++;
        html+=(src[k]==='(')?'<span class="tok-f">'+esc(w)+'</span>':esc(w);}
    }
    else html+=esc(m[5]||m[6]||'');
  }
  code.innerHTML=html;
}
function highlightCode(root){
  (root||$('threadInner')).querySelectorAll('.ai-code').forEach(box=>{
    const langEl=box.querySelector('.ai-code-lang');
    if(!langEl)return;
    const lang=langEl.textContent.trim().toLowerCase();
    if(lang!=='lua'&&lang!=='luau'&&lang!=='sql')return;
    const code=box.querySelector('pre code');
    const raw=decodeHtml(box.getAttribute('data-code')||'');
    if(code&&raw)hlApply(code,raw,lang==='sql'?'sql':'lua');
  });
}
)AIWEBUI"
           LR"AIWEBUI(/* 气泡正文里的绝对路径 → 可点击 .ai-path 链接 (TreeWalker 只碰文本节点,
   跳过代码块/链接/已有链接/按钮; 句尾标点剥出链接外) */
function linkifyPaths(root){
  const scope=root||$('threadInner');
  const walker=document.createTreeWalker(scope,NodeFilter.SHOW_TEXT,{acceptNode:function(n){
    const v=n.nodeValue;
    if(!v||v.length<4)return NodeFilter.FILTER_REJECT;
    const p=n.parentElement;
    if(!p||p.closest('.ai-code,a,.ai-path,button,textarea,select,script,style'))return NodeFilter.FILTER_REJECT;
    return v.match(PATH_RE)?NodeFilter.FILTER_ACCEPT:NodeFilter.FILTER_REJECT;
  }});
  const nodes=[];let n;
  while((n=walker.nextNode()))nodes.push(n);
  nodes.forEach(function(node){
    const text=node.nodeValue;
    let m,frag=null,last=0;
    PATH_RE.lastIndex=0;
    while((m=PATH_RE.exec(text))){
      if(m.index+m[0].length<=last)continue;   /* 已被上一个延伸段覆盖 */
      const end=extendPath(text,m.index+m[0].length);
      if(end<=last)continue;
      const path=text.slice(m.index,end);
      const strip=path.replace(/[.。,，;；:!?)】」》>'"\u2019\u201d]+$/,'');
      if(strip.length<3)continue;
      if(!frag)frag=document.createDocumentFragment();
      if(m.index>last)frag.appendChild(document.createTextNode(text.slice(last,m.index)));
      const sp=document.createElement('span');
      sp.className='ai-path';
      sp.setAttribute('data-path',strip);
      sp.title='点击打开 · 右键更多操作';
      sp.textContent=strip;
      frag.appendChild(sp);
      if(strip.length<path.length)frag.appendChild(document.createTextNode(path.slice(strip.length)));
      last=end;
    }
    if(frag){
      if(last<text.length)frag.appendChild(document.createTextNode(text.slice(last)));
      node.parentNode.replaceChild(frag,node);
    }
  });
}
function enhance(root){
  const scope=root||$('threadInner');
  transformActionLinks(scope);
  highlightCode(scope);
  linkifyPaths(scope);
}
/* ---- 右键菜单 (文件路径 / 搜索卡片的操作项; 点外/Esc/滚动/缩放关闭) ---- */
function hideCtx(){
  const m=$('ctxMenu');
  if(m)m.remove();
  S.ctxOpen=false;
}
function showCtx(items,x,y){
  hideCtx();
  const menu=document.createElement('div');
  menu.className='ai-ctx';
  menu.id='ctxMenu';
  items.forEach(function(it){
    if(it==='-'){const sep=document.createElement('div');sep.className='ai-ctx-sep';menu.appendChild(sep);return;}
    const b=document.createElement('button');
    b.type='button';
    b.className='ai-ctx-item';
    b.textContent=it.t;
    b.addEventListener('click',function(ev){ev.stopPropagation();hideCtx();it.fn();});
    menu.appendChild(b);
  });
  document.body.appendChild(menu);
  const r=menu.getBoundingClientRect();
  let L=x,T=y;
  if(L+r.width>window.innerWidth-8)L=window.innerWidth-r.width-8;
  if(T+r.height>window.innerHeight-8)T=Math.max(8,y-r.height-4);
  menu.style.left=Math.round(Math.max(8,L))+'px';
  menu.style.top=Math.round(T)+'px';
  S.ctxOpen=true;
}
)AIWEBUI"
           LR"AIWEBUI(/* ---- 消息 HTML 内的点击 (事件委托) ---- */
function decodeHtml(s){const t=document.createElement('textarea');t.innerHTML=s;return t.value;}
function copyTurn(mi){
  const m=S.msgs[mi];
  if(!m)return;
  let text=String(m.t||'').trim();   /* 原始 Markdown = 事实源, 换行/列表/代码块结构全保留 */
  if(!text&&m.html){const d=document.createElement('div');d.innerHTML=m.html;text=(d.textContent||'').trim();}
  if(!text&&m.reason)text=String(m.reason).trim();
  if(text)post({c:'copy',text});
}
function bindThread(){
  const inner=$('threadInner');
  inner.addEventListener('click',e=>{
    const sug=e.target.closest('.ai-suggestion');
    if(sug){if(!S.sending)post({c:'send',text:sug.textContent});return;}
    const dnt=e.target.closest('.ai-donate');
    if(dnt){if(!S.sending)post({c:'send',text:dnt.getAttribute('data-q')||'关于作者'});return;}
    /* 有非折叠选区 = 用户在拖选复制, 不当作点击 (路径误开防线)。
       卡片不走此判定: .ai-chip 是 user-select:none, 点它不折叠既有选区 —
       挂着选区(拖选/双击过正文后)的正常点击会被这里吞掉 = "点了没反应";
       且卡片本身拖不出选区, mousedown/mouseup 异目标的拖选也不会派发 click 到卡片。 */
    const chip=e.target.closest('.ai-chip');
    if(chip){post({c:'search',text:chip.getAttribute('data-text')||'',mode:chip.getAttribute('data-mode')||''});
      chip.classList.add('sent');setTimeout(function(){chip.classList.remove('sent');},600);
      return;}
    const hasSel=window.getSelection&&!window.getSelection().isCollapsed;
    const pth=e.target.closest('.ai-path');
    if(pth&&!hasSel){
      /* 模型编造/近似的 ID (渲染期已标禁用): 点击如实告知, 不发命令 */
      if(pth.getAttribute('data-badid')){showToast('这个链接的文件 ID 无效 (AI 未能给出准确 ID), 无法打开','warn');return;}
      const act=pth.getAttribute('data-open')==='reveal'?'reveal':'open';
      const id=pth.getAttribute('data-id');
      /* 新链接只带引擎 FileId (程序按 ID 取路径); 旧历史消息是 data-path */
      if(id)post({c:act,id:+id});
      else post({c:act,path:pth.getAttribute('data-path')||pth.textContent});
      return;}
    const a=e.target.closest('a');
    if(a){e.preventDefault();const href=a.getAttribute('href')||'';
      if(/^https?:/i.test(href))post({c:'openurl',href});return;}
    const cp=e.target.closest('.ai-code-copy');
    if(cp){const box=cp.closest('.ai-code');const code=box?decodeHtml(box.getAttribute('data-code')||''):'';
      post({c:'copy',text:code});
      cp.classList.add('ai-code-copied');cp.textContent='已复制';
      clearTimeout(S.copiedTimer);
      S.copiedTimer=setTimeout(()=>{  /* 2s 复位该按钮 */
        document.querySelectorAll('.ai-code-copy.ai-code-copied').forEach(b=>{b.classList.remove('ai-code-copied');b.textContent='复制';});
      },2000);return;}
    const mb=e.target.closest('.ai-meta-btn');
    if(mb){const mi=+mb.closest('.ai-msg-meta').getAttribute('data-mi');
      if(mb.getAttribute('data-act')==='copy')copyTurn(mi);
      else if(mb.getAttribute('data-act')==='retry'&&!S.sending){
        /* 重跑同一回合组起点不变: 清掉本会话过程面板开合落账 (上轮自动收起的 false 会让
           重跑过程一直藏着), 回到缺省口径 = 流式中的最新回合展开, 其余仍按缺省收起 */
        const pfx=S.cur+':';
        Object.keys(S.turnLog).forEach(k=>{if(k.startsWith(pfx))delete S.turnLog[k];});
        post({c:'retry'});}
      return;}
    const ab=e.target.closest('.abtn');
    if(ab){const act=ab.getAttribute('data-act');
      if(act==='authallow')post({c:'pallow'});
      else if(act==='authdeny')post({c:'pdeny'});
      else if(act==='adjApply'||act==='adjIgnore'||act==='adjApplyAll'||act==='adjIgnoreAll'){
        const box=ab.closest('.sadj'),it=ab.closest('.sadj-it');
        if(box)post({c:'adj',act:act.slice(3).toLowerCase(),
          mi:+box.getAttribute('data-mi'),si:+box.getAttribute('data-si'),
          ii:it?(+it.getAttribute('data-ii')):-1});}
      return;}
    const st=e.target.closest('.step .shead');
    if(st){const card=st.closest('.step');const gi=+card.getAttribute('data-gi');const key=S.cur+':'+gi;
      S.openSteps[key]=!S.openSteps[key];
      const body=card.querySelector('.ssamples');
      if(body)body.style.display=S.openSteps[key]?'':'none';
      card.classList.toggle('open',!!S.openSteps[key]);
      return;}
    const tlh=e.target.closest('.ai-turn-log-head');
    if(tlh){const msg=tlh.closest('.ai-msg');const key=S.cur+':'+msg.getAttribute('data-mi');
      const panel=tlh.closest('.ai-turn-log');
      const cur=S.turnLog[key]!==undefined?S.turnLog[key]:panel.classList.contains('open');
      S.turnLog[key]=!cur;
      panel.classList.toggle('open',S.turnLog[key]);
      tlh.setAttribute('aria-expanded',S.turnLog[key]?'true':'false');
      return;}
    const tg=e.target.closest('.ai-toolgrp-head');
    if(tg){const grp=tg.closest('.ai-toolgrp');const key=S.cur+':'+grp.getAttribute('data-k0');
      S.toolGrp[key]=!(S.toolGrp[key]!==undefined?S.toolGrp[key]:grp.classList.contains('open'));
      grp.classList.toggle('open',S.toolGrp[key]);
      tg.setAttribute('aria-expanded',S.toolGrp[key]?'true':'false');
      return;}
    const rh=e.target.closest('.ai-reasoning-head');
    if(rh){const mi=+rh.getAttribute('data-mi');
      const cur=S.reasonOpen[mi]!==undefined?S.reasonOpen[mi]
        :(S.sending&&mi===S.msgs.length-1&&S.phase===0);
      S.reasonOpen[mi]=!cur;
      const turn=rh.closest('.ai-msg');   /* 回合组: 整组重渲染 (过程区+气泡同源) */
      if(turn){const start=+turn.getAttribute('data-mi');
        const f=document.createElement('div');f.innerHTML=turnGroupHtml(start);
        const nw=f.firstChild;enhance(nw);
        turn.replaceWith(nw);applyOpenSteps();}
      return;}
  });
  /* 右键: 文件路径 / 搜索卡片的操作菜单 (其余区域保留浏览器原生菜单 = 选区复制入口) */
  inner.addEventListener('contextmenu',e=>{
    const chip=e.target.closest('.ai-chip');
    if(chip){
      e.preventDefault();
      const text=chip.getAttribute('data-text')||'',mode=chip.getAttribute('data-mode')||'';
      showCtx([
        {t:'执行搜索',fn:()=>post({c:'search',text,mode})},
        {t:'只填入, 不执行',fn:()=>post({c:'searchfill',text,mode})},
        '-',
        {t:'复制搜索词',fn:()=>post({c:'copy',text})}
      ],e.clientX,e.clientY);
      return;
    }
    const pth=e.target.closest('.ai-path');
    if(pth){
      e.preventDefault();
      /* 禁用链接 (编造的 ID): 只留"复制名称", 不发打开/定位命令 */
      if(pth.getAttribute('data-badid')){
        showCtx([{t:'复制名称',fn:()=>post({c:'copy',text:pth.textContent})}],e.clientX,e.clientY);
        return;
      }
      const id=pth.getAttribute('data-id');
      const ref=id?{id:+id}:{path:pth.getAttribute('data-path')||pth.textContent};
      showCtx([
        {t:'打开文件',fn:()=>post(Object.assign({c:'open'},ref))},
        {t:'定位文件',fn:()=>post(Object.assign({c:'reveal'},ref))},
        '-',
        {t:'复制路径',fn:()=>post(Object.assign({c:'copypath'},ref))}
      ],e.clientX,e.clientY);
      return;
    }
  });
  /* 滚动: 跟随标记 + 当前轮次同步 (rAF 合并同帧多次触发) */
  const t=threadEl();
  t.addEventListener('scroll',()=>{
    S.follow=isNearBottom();
    hideJumpTip();
    hideCtx();
    if(!S.jumpRounds.length||S.jumpFrame)return;
    S.jumpFrame=requestAnimationFrame(()=>{S.jumpFrame=0;syncJumpActive();});
  });
}

/* ==================== 历史侧栏 ==================== */
function renderSide(){
  $('side').hidden=!S.sideOpen;
  $('b-hist').setAttribute('aria-expanded',S.sideOpen?'true':'false');
  if(!S.sideOpen)return;
  const has=S.convs.length>0;
  $('clearb').hidden=!has;
  $('sideEmpty').hidden=has;
  let html='';
  for(const c of S.convs){
    const act=c.id===S.cur;
    const streaming=act&&S.sending;
    html+='<div class="ai-history-item'+(act?' ai-history-item-current':'')+(streaming?' ai-history-item-streaming':'')+'" data-id="'+c.id+'">'
      +'<div class="ai-history-item-text">'
      +'<div class="ai-history-item-title">'+(c.title?esc(c.title):'未命名对话')+'</div>'
      +'<div class="ai-history-item-time">'+esc(histTime(c.t))+'</div>'
      +'</div>'
      +'<button class="ai-history-item-delete glyph" data-del="'+c.id+'" title="删除这条对话" aria-label="删除这条对话">&#xE74D;</button>'
      +'</div>';
  }
  $('sideList').innerHTML=html;
}
/* 清空记录两步确认复位 (开合侧栏/超时/Esc 都归零) */
function clearDisarm(){
  S.clearArmed=false;clearTimeout(S.clearTimer);
  const b=$('clearb');b.textContent='清空记录';b.setAttribute('data-armed','false');
}
function sideToggle(open){
  S.sideOpen=open;clearDisarm();
)AIWEBUI"
           LR"AIWEBUI(  if(!open)hideJumpTip();
  renderSide();
}
/* 侧栏悬浮模式 = 窄面板 (阈值与样式表 @media 同一口径 760px): 悬浮盖住对话区,
   点外/失焦要收; 宽面板时停靠成布局列, 属常驻模式不随点外/失焦收 */
function sideFloating(){return !!(window.matchMedia&&window.matchMedia('(max-width: 760px)').matches);}

/* ==================== 输入区 ==================== */
function autoSize(){
  const ta=$('inputT');
  ta.style.height='auto';
  ta.style.height=Math.min(132,Math.max(22,ta.scrollHeight))+'px';
  ta.style.overflowY=ta.scrollHeight>132?'auto':'hidden';
}
function refreshSendState(){
  const hasText=!!$('inputT').value.trim();
  const grayed=!S.sending&&!hasText;
  const b=$('sendB');
  b.disabled=grayed;
  if(grayed)b.setAttribute('data-empty','true');else b.removeAttribute('data-empty');
}
function renderComposer(){
  const b=$('sendB');
  b.dataset.mode=S.sending?'stop':'send';
  const label=S.sending?'停止':'发送';
  b.setAttribute('aria-label',label);
  b.setAttribute('title',S.sending?label:label+' · Enter 发送，Shift + Enter 换行');
  $('cbox').setAttribute('data-streaming',S.sending?'true':'false');
  refreshSendState();
  renderPolicy();
  renderUsage();
}
function doSend(){
  const ta=$('inputT');
  const text=ta.value.trim();
  if(!text||S.sending)return;
  if(!S.cfg.hasKey){  /* 未配置: 打开接口设置面板 + 明确提示 (与参考实现同口径) */
    showToast('尚未配置接口密钥 — 请填写接口地址、API 密钥与模型','warn');
    cfgToggle(true);
    return;
  }
  ta.value='';autoSize();refreshSendState();
  post({c:'send',text});
}

/* ---- 文件操作权限下拉 ---- */
function renderPolicy(){
  const cur=POLICY[S.cfg.policy]||POLICY[2];
  $('policyLabel').textContent=cur.n;
  const btn=$('policyBtn');
  btn.dataset.policy=cur.k;
  btn.title='文件操作权限：'+cur.h;
  document.querySelectorAll('#policyMenu .ai-cmd-policy-option').forEach(op=>{
    const i=+op.getAttribute('data-policy');
    const p=POLICY[i];
    op.querySelector('.ai-cmd-policy-option-label').textContent=p.n;
    op.querySelector('.ai-cmd-policy-option-hint').textContent=p.h;
    op.setAttribute('aria-checked',i===S.cfg.policy?'true':'false');
  });
}
function policySetOpen(open){
  S.policyOpen=open;   /* 状态标志必须先落账: 点外收起/Escape/按钮切换全读它 (曾漏赋值=开关视觉正常但标志恒 false, 弹层永远关不掉) */
  const btn=$('policyBtn'),menu=$('policyMenu');
  btn.setAttribute('aria-expanded',open?'true':'false');
  clearTimeout(S.policyTimer);
  if(open){
    usageSetOpen(false);   /* 互斥: 用量浮层同从工具条向上展开 */
    menu.classList.remove('closing');
    menu.classList.add('opening');
    menu.hidden=false;
    requestAnimationFrame(()=>requestAnimationFrame(()=>{
      if(btn.getAttribute('aria-expanded')==='true')menu.classList.remove('opening');
    }));
    return;
  }
  menu.classList.remove('opening');
  if(menu.hidden)return;
  menu.classList.add('closing');
  S.policyTimer=setTimeout(()=>{
    if(btn.getAttribute('aria-expanded')!=='true'){menu.hidden=true;menu.classList.remove('closing');}
  },90);
}

/* ---- 用量简况 + 详情浮层 ---- */
function renderUsage(){
  const u=S.usage;
  const win=ctxWin();
  const used=u.has?(u.lp+u.lc):0;   /* 占用条按最近一次请求 (累计值会把上下文重复计入) */
  const ratio=win>0?Math.min(1,Math.max(0,used/win)):0;
  const free=1-ratio;
  $('ubarf').style.width=(ratio*100).toFixed(1)+'%';
  if(free<=0.1)$('usageBtn').setAttribute('data-warn','true');
  else $('usageBtn').removeAttribute('data-warn');
  $('ubrief').textContent='剩余 '+Math.round(free*100)+'% · '+((u.has&&used>0)?fmtTokens(used):'--');
  if(S.usageOpen){renderUsagePanel();positionUsagePanel();}
}
function renderUsagePanel(){
  const u=S.usage;
  const win=ctxWin();
  const used=u.has?(u.lp+u.lc):0;
  const free=Math.max(0,win-used),freeRatio=win>0?free/win:1;
  const has=u.has;
  const cachePct=(has&&u.up)?Math.round(u.uch/u.up*100)+'%':'--';
  const speed=u.tps>0?(Math.round(u.tps*10)/10)+' tok/s':'--';
  const row=(k,v,attr,empty)=>'<div class="ai-usage-row"'+(empty?' data-empty="true"':'')+'>'
    +'<span class="ai-usage-row-label">'+k+'</span>'
    +'<span class="ai-usage-row-value"'+(attr?' '+attr:'')+'>'+v+'</span></div>';
  const grp=k=>'<div class="ai-usage-group">'+k+'</div>';
  let html=grp('本次对话累计')
    +row('输入',has?fmtTokens(u.up):'--','',!(has&&u.up))
    +row('输出',has?fmtTokens(u.uo):'--','',!(has&&u.uo))
    +row('合计',has?fmtTokens(u.ut):'--','',!(has&&u.ut))
    +row('缓存命中',cachePct,(has&&u.up&&u.uch*2>=u.up)?'data-good="true"':'',!(has&&u.up))
    +grp('当前上下文')
    +row('上下文已用',has?fmtTokens(used)+' / '+fmtTokens(win):'--','',!used)
    +row('上下文剩余',Math.round(freeRatio*100)+'%',freeRatio<=0.1?'data-warn="true"':'')
    +row('速度',speed,'',speed==='--');
  /* 上限来源照实标注: 档案指定了就写"由档案指定", 没写才是按模型名推断 */
  const note=S.cfg.ctx>0
    ?'上下文上限由档案指定为 '+fmtTokens(win)
    :'上下文上限按模型「'+esc(S.cfg.model)+'」推断为 '+fmtTokens(win);
  html+='<div class="ai-usage-note">'+note+'</div>';
  $('usagePanel').innerHTML=html;
}
function positionUsagePanel(){
  const anchor=$('usageBtn').getBoundingClientRect();
  const panel=$('usagePanel');
  const pr=panel.getBoundingClientRect();
  let left=anchor.right-pr.width;
  const maxLeft=window.innerWidth-pr.width-8;
  left=Math.max(8,Math.min(left,maxLeft));
)AIWEBUI"
           LR"AIWEBUI(  let top=anchor.top-pr.height-6;
  if(top<8)top=anchor.bottom+6;   /* 上方空间不够: 翻到下方 */
  panel.style.left=Math.round(left)+'px';
  panel.style.top=Math.round(top)+'px';
}
function usageSetOpen(open){
  S.usageOpen=open;   /* 同 policySetOpen: 状态标志先落账 */
  if(open){renderUsagePanel();positionUsagePanel();
    $('usagePanel').classList.add('visible');
    policySetOpen(false);   /* 互斥 */
  }else $('usagePanel').classList.remove('visible');
  $('usageBtn').setAttribute('aria-expanded',open?'true':'false');
}

/* ==================== C++ → JS ==================== */
function handle(m){
  switch(m.t){
    case 'boot':
      S.cfg=m.cfg;S.pal=m.pal;applyPal(S.pal);
      S.convs=m.convs||[];S.cur=m.cur||0;S.msgs=m.msgs||[];
      S.sending=!!m.st.sending;S.net=m.st.net;S.phase=m.st.phase||0;
      S.usage=m.usage||S.usage;
      maybeAutoCollapseTurn();
      renderAll();break;
    case 'pal':S.pal=m.pal;applyPal(S.pal);renderAll();break;
    case 'cfg':
      S.cfg=m.cfg;
      /* 面板开着且表单干净 → 从新活动档案重填; 有未保存编辑就不动 (不冲掉正在敲的内容)。
         面板不再随广播自动收起 (保存后停留展示 + toast 确认, 取消/✕ 才收) */
      if(S.cfgOpen&&!S.profDirty)fillProfForm();
      renderProfSelect();renderModelBtn();
      if(S.modelOpen)renderModelMenu();
      renderStatus();renderComposer();
      break;
    case 'convs':S.convs=m.convs||[];renderSide();break;
    case 'msgs':
      if(m.cur!==undefined)S.cur=m.cur;
      S.msgs=m.msgs||[];
      if(m.st){S.sending=!!m.st.sending;S.net=m.st.net;S.phase=m.st.phase||0;}
      maybeAutoCollapseTurn();
      renderThread(true);renderStatus();renderComposer();renderSide();break;
    case 'last':{
      if(!S.msgs.length)break;
      S.msgs[S.msgs.length-1]=m.m;
      if(m.st){S.sending=!!m.st.sending;S.phase=m.st.phase||0;}
      maybeAutoCollapseTurn();
      applyLast();renderStatus();break;}
    case 'usage':S.usage=m.u||S.usage;renderUsage();break;
    case 'toast':showToast(m.msg,{0:'info',1:'ok',2:'warn',3:'err'}[m.k]||'warn');break;
    case 'status':
      S.sending=!!m.sending;S.net=m.net;S.phase=m.phase||0;
      maybeAutoCollapseTurn();
      renderStatus();renderThread(true);renderComposer();renderSide();break;
    case 'blur':
      /* 浏览器焦点离开面板 (点击宿主/切走窗口): 收起瞬态弹层与悬浮侧栏。
         面板外的点击 WebView2 收不到, 页面自己的点外关闭够不着, 由 C++ 补发;
         停靠侧栏 (宽面板) 与设置面板是常驻模式, 不随焦点收 */
      if(S.ctxOpen)hideCtx();
      if(S.policyOpen)policySetOpen(false);
      if(S.usageOpen)usageSetOpen(false);
      if(S.modelOpen)modelSetOpen(false);
      if(S.sideOpen&&sideFloating())sideToggle(false);
      break;
  }
}
function renderAll(){
  renderProfSelect();renderModelBtn();
  renderStatus();cfgToggle(S.cfgOpen);renderThread(false);renderComposer();renderSide();
}
)AIWEBUI"
           LR"AIWEBUI(/* ==================== 事件接线 ==================== */
function bind(){
  $('b-set').addEventListener('click',()=>cfgToggle(!S.cfgOpen));
  $('b-hist').addEventListener('click',()=>sideToggle(!S.sideOpen));
  $('b-new').addEventListener('click',()=>post({c:'new'}));
  $('b-close').addEventListener('click',()=>post({c:'close'}));
  $('b-cancel').addEventListener('click',()=>cfgToggle(false));
  $('b-save').addEventListener('click',saveProf);
  /* 模型档案: 下拉切换 / 新建 / 复制 / 删除 (删除是两步确认); 保存成功/点外/Esc 均收起面板 */
  $('f-prof').addEventListener('change',()=>selectProf($('f-prof').value));
  $('f-prof-add').addEventListener('click',()=>{S.profDirty=false;post({c:'profNew',dup:0});});
  $('f-prof-dup').addEventListener('click',()=>{S.profDirty=false;post({c:'profNew',dup:1});});
  $('f-prof-del').addEventListener('click',()=>{
    if(!S.cfg.profs.length)return;
    if(!S.delArmed){   /* 首击进入待确认, 4s 内再击才删 (超时自动复位) */
      S.delArmed=true;
      const b=$('f-prof-del');b.textContent='确认删除';b.setAttribute('data-armed','true');
      clearTimeout(S.delTimer);S.delTimer=setTimeout(profDisarm,4000);
      return;
    }
    profDisarm();
    const p=profActive();
    const del=p?p.id:'';
    /* 本地先切到剩下的第一条; 宿主回推 (cfg 广播) 会给出同样的结果 */
    S.profDirty=false;
    S.cfg.profs=S.cfg.profs.filter(q=>q.id!==del);
    S.cfg.active=S.cfg.profs.length?S.cfg.profs[0].id:'';
    const np=profActive();
    if(np){S.cfg.url=np.url||'';S.cfg.model=np.model||'';S.cfg.hasKey=!!np.hasKey;
           S.cfg.ctx=np.ctx||0;S.cfg.maxOut=np.maxOut||0;S.cfg.name=profName(np);}
    else{S.cfg.url='';S.cfg.model='';S.cfg.hasKey=false;S.cfg.ctx=0;S.cfg.maxOut=0;S.cfg.name='未配置接口';}
    if(S.cfgOpen){renderProfSelect();fillProfForm();}
    renderModelBtn();renderStatus();renderUsage();
    post({c:'profDel',id:del});
  });
  /* 表单里任何编辑都标脏 —— 挡住宿主整包下发把用户正在敲的内容冲掉 */
  ['f-name','f-url','f-key','f-model','f-ctx','f-max'].forEach(id=>{
    $(id).addEventListener('input',()=>{S.profDirty=true;});
  });
  /* 工具栏模型下拉: 触发器开合 / 选中即切换 / 管理入口 / 点外部与 Esc 收起 */
  $('modelBtn').addEventListener('click',()=>modelSetOpen(!S.modelOpen));
  $('modelMenu').addEventListener('click',e=>{
    const mg=e.target.closest('.ai-model-option-manage');
    if(mg){modelSetOpen(false);if(!S.cfgOpen)cfgToggle(true);return;}
    const o=e.target.closest('.ai-model-option');
    if(o&&o.getAttribute('data-pid')){selectProf(o.getAttribute('data-pid'));modelSetOpen(false);}
  });
  $('f-reason').addEventListener('click',()=>{
    const on=$('f-reason').getAttribute('aria-pressed')==='true';
    $('f-reason').setAttribute('aria-pressed',on?'false':'true');
  });
  $('sendB').addEventListener('click',()=>{if(S.sending)post({c:'stop'});else doSend();});
  const ta=$('inputT');
  ta.addEventListener('input',()=>{autoSize();refreshSendState();});
  ta.addEventListener('keydown',e=>{
    if(e.key==='Enter'&&!e.shiftKey&&!e.isComposing){e.preventDefault();doSend();}
  });
  /* 输入区留白点击 = 聚焦输入框 (整个区域文本指针, 点空白继续打字) */
  document.querySelector('.ai-composer').addEventListener('mousedown',e=>{
    if(e.target.closest('button')||e.target.closest('.ai-usage-panel')||e.target===ta)return;
    e.preventDefault();ta.focus();
  });
  $('policyBtn').addEventListener('click',()=>policySetOpen(!S.policyOpen));
  $('policyMenu').addEventListener('click',e=>{
    const op=e.target.closest('.ai-cmd-policy-option');
    if(!op)return;
    policySetOpen(false);
    post({c:'policy',v:+op.getAttribute('data-policy')});
  });
  $('usageBtn').addEventListener('click',()=>usageSetOpen(!S.usageOpen));
  /* 清空记录 = 两步确认 (与删除模型档案同一套语言): 首击进入待确认, 4s 内再击才清空 */
  $('clearb').addEventListener('click',()=>{
    if(!S.clearArmed){
      S.clearArmed=true;
      const b=$('clearb');b.textContent='确认清空';b.setAttribute('data-armed','true');
      clearTimeout(S.clearTimer);S.clearTimer=setTimeout(clearDisarm,4000);
      return;
    }
    clearDisarm();
    post({c:'clearHist'});
  });
  $('sideList').addEventListener('click',e=>{
    const del=e.target.closest('.ai-history-item-delete');
    if(del){e.stopPropagation();post({c:'del',id:+del.getAttribute('data-del')});return;}
    const row=e.target.closest('.ai-history-item');
    if(row){
      /* 窄窗口 (悬浮模式) 选中后收起侧栏, 露出对话区 */
      if(sideFloating())sideToggle(false);
      post({c:'load',id:+row.getAttribute('data-id')});
      try{ta.focus();}catch(err){}
    }
  });
  document.addEventListener('mousedown',e=>{
    if(S.ctxOpen&&!e.target.closest('#ctxMenu'))hideCtx();
    if(S.policyOpen&&!e.target.closest('#policy')&&!e.target.closest('#policyMenu'))policySetOpen(false);
    if(S.usageOpen&&!e.target.closest('#usagePanel')&&!e.target.closest('#usageBtn'))usageSetOpen(false);
    if(S.modelOpen&&!e.target.closest('#modelPicker'))modelSetOpen(false);
    /* 悬浮侧栏点外收起 (排除 #b-hist: 按钮自己的 click 负责开关, mousedown 先收会把它再弹开) */
    if(S.sideOpen&&sideFloating()&&!e.target.closest('#side')&&!e.target.closest('#b-hist'))sideToggle(false);
    /* 接口设置面板点外收起 = 取消未保存的编辑 (同样排除 #b-set: 它的 click 负责开关) */
    if(S.cfgOpen&&!e.target.closest('#cfgPanel')&&!e.target.closest('#b-set'))cfgToggle(false);
  });
  document.addEventListener('keydown',e=>{
    if(e.key==='Escape'){
      if(S.clearArmed){clearDisarm();e.preventDefault();return;}
      if(S.ctxOpen){hideCtx();e.preventDefault();return;}
      if(S.modelOpen){modelSetOpen(false);e.preventDefault();return;}
      if(S.cfgOpen){profDisarm();cfgToggle(false);e.preventDefault();return;}
      if(S.policyOpen){policySetOpen(false);e.preventDefault();return;}
      if(S.usageOpen){usageSetOpen(false);e.preventDefault();return;}
    }
  });
)AIWEBUI"
           LR"AIWEBUI(  window.addEventListener('resize',()=>{hideJumpTip();hideCtx();if(S.modelOpen)modelSetOpen(false);});
  /* 内容高度变化时自动跟随到底 (用户手动上滚后 S.follow=false 不再拉回) */
  try{
    new ResizeObserver(()=>{
      if(!S.follow)return;
      requestAnimationFrame(()=>{if(S.follow)scrollToEnd();});
    }).observe($('threadInner'));
  }catch(e){}
  bindThread();
  $('threadInner').innerHTML=EMPTY_HTML;
  /* JS ready → C++ 推 boot */
  post({c:'ready'});
}
try{
  chrome.webview.addEventListener('message',e=>{
    try{handle(typeof e.data==='string'?JSON.parse(e.data):e.data);}catch(err){}
  });
}catch(e){}
document.addEventListener('DOMContentLoaded',bind);
</script>
</body>
</html>

)AIWEBUI"
           ;
}
