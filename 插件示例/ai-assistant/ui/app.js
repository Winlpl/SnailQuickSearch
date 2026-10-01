
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

/* ---- 空态示例提示词池 (46 条, 每次随机抽 6 条展示, 「换一批」重新抽取) ----
 * 只收语义/意图类提问 — "文件名条件"类提问搜索框直接打更快, 不收 (2026-09-25 用户口径)。
 * 条目可带 "文案->实际提示词" 映射: 按钮显示 -> 前的短文案, 填入 -> 后的完整提问 (无 -> 则原样);
 * 点击 = 填入输入框不发送, 用户确认/修改后自己发 */
const SUGGS=[
  '现在哪些大文件占用空间最多','找出一周内修改过的文档并列个清单','看看当前的文件分类和重复文件',
  '统计每种扩展名的文件数量,画成饼图','找出最大的 5 个文件,用柱状图对比大小',
  '哪些文件夹占用的空间最大','统计一下各个磁盘分区的空间占用',
  '列出最近三天新建的文件',
  '看看我今天做了什么->请告诉我，我今天具体做了哪些事情？请列出详细清单。','这周我都做了什么->请总结我本周的文件活动：新建、修改、下载了哪些文件，按天列出清单。',
  '我上个月忙了些什么->请总结我上个月的文件活动：修改最多的文件类型与活跃时段，附代表性文件清单。','最近一小时电脑有什么动静->请列出最近 1 小时内新建或修改过的文件，并逐个说明它是什么。','哪些文件今天刚动过->把今天修改过的所有文件按时间先后列成清单，标出我最新编辑的一个。',
  '我今天下载的文件->请列出我今天下载的所有文件，排除非人工主动下载的（软件自动更新、程序自己保存的缓存/日志），只保留我主动下载保存的，按时间先后列成清单。','这周我下载了什么->请列出本周我主动下载的文件（排除软件自动更新包、程序自动保存的缓存），按天分组展示。',
  '昨天下载的文件放哪了->请列出昨天我主动下载的文件，标注每个文件现在所在的完整位置。','下载夹里攒了多少东西->请统计下载文件夹的文件数量和总大小，排除程序自动生成的缓存/日志，按类型归类并列出其中最大的 10 个。',
  '找个文件但忘了名字->我找不到一个文件了，名字记不清了。请一步步问我线索（文件类型、大概什么时间的、名字里可能有的词），再按线索帮我搜出来。','上周写的方案->找出上周修改过的 Word/PPT/PDF 文档，按修改时间排序列出，帮我认出哪份是我在写的方案。',
  '别人发我的文件->列出最近两周在下载/桌面/文档目录新出现的文件，帮我挑出可能是别人传给我的，列成清单。','帮我找张壁纸->找出分辨率较大、横版为主的图片文件，优先最近修改的，挑 10 张列出来让我选。',
  '能发客户的 PDF->找出最近的 PDF 文件，排除帮助文档/软件说明类的，按修改时间列出，让我确认哪份可以外发。','照片都是哪天拍的->把我的照片文件按月份分组统计数量，每组给出一个示例文件名，帮我想起都拍过什么。',
  '哪些文件命名很乱->找出文件名带"新建""副本""未标题""最终版"等字样的文件，列成清单，并给每个建议一个更清晰的名字。','同一文件存了多份->找出文件名相似、疑似同一文件的多个版本（带"副本"/"(1)"/日期后缀的），列出来让我合并清理。',
  '版本太多分不清->找出名字里带 v1/v2/v3 或"旧版/新版"字样的文件，按名字分组列出，标出每组里最新修改的那份。','扫下敏感文件->找出文件名可能含 身份证/账号/密码/合同/简历 等字样的文件，列清单提醒我哪些要注意保管。',
  '我的发票和账单->找出文件名带 发票/收据/订单/账单 字样的文件，按年份月份归类列出。','我的电子书在哪->找出 epub/mobi/pdf 里属于书籍的文件，按所在文件夹归类，列出总数和最大的几本。',
  '装机要用的安装包->列出电脑里的安装程序（exe/msi 安装包），按所在文件夹分组，提醒哪些可能已经用不上了。','今天的电脑足迹->按小时整理今天新建或修改过的文件时间线，让我回顾这一天都处理了什么。','空的没用的->列出所有空文件夹和几乎为零的零散小文件，方便我一次清掉。',

  '列出本月修改过的 Excel 表格','找出一年都没打开过的旧文档',
  '哪些临时文件可以清理掉','找出内容相同的重复大文件','找出文件名重复的文件',
  '统计每种扩展名的文件数量','找出图片文件夹里的旧截图','统计一下音乐文件总共占了多少空间',
  '找出路径层次特别深的文件','统计照片和视频各占多少空间',
  '找出最近浏览过但很久没修改的文件','找出小文件特别多的文件夹','帮我把下载文件夹的文件按类型分个类',
  '帮我整理桌面->看看我的桌面和下载文件夹，给出 3 条具体的整理建议，并说明每条大概能腾出多少空间。','该归档的旧项目->找出超过半年没修改过的项目类文件夹，列出来并建议我归档到哪里。',
  '截图都散在哪->找出散落在各个目录的截图类图片，统计数量和位置，建议集中到一个文件夹。'
];
/* 洗牌副本取前 6 (天然不重复); excl = 上一批, 池够大时优先避开, 保证「换一批」肉眼可见地全换掉 */
function shuffleSuggs(excl){
  const pool=SUGGS.slice();
  for(let i=pool.length-1;i>0;i--){const j=(Math.random()*(i+1))|0;const t=pool[i];pool[i]=pool[j];pool[j]=t;}
  const out=(excl&&excl.length)?pool.filter(q=>excl.indexOf(q)<0):pool;
  return out.slice(0,6);
}
let SUGG_CUR=shuffleSuggs(null);
/* "文案->实际提示词" 映射: 按钮显示短文案, data-q 带完整提问 (无 -> 则两者相同) */
function suggHtml(s){const i=s.indexOf('->');const l=i<0?s:s.slice(0,i),q=i<0?s:s.slice(i+2);
  return '<button class="ai-suggestion" type="button" data-q="'+esc(q)+'">'+esc(l)+'</button>';}
function suggsHtml(){return SUGG_CUR.map(suggHtml).join('');}
function rerollSuggs(){
  SUGG_CUR=shuffleSuggs(SUGG_CUR);
  const box=$('suggBox');if(!box)return;
  box.classList.remove('rolling');void box.offsetWidth;   /* 重触发入场动画 */
  box.innerHTML=suggsHtml();
  box.classList.add('rolling');
}
const EMPTY_HTML='<div class="ai-empty" id="empty">'
  +'<span class="ai-empty-icon glyph" aria-hidden="true">&#xE99A;</span>'
  +'<span class="ai-empty-title">用对话来查找和整理文件</span>'
  +'<span class="ai-empty-desc">我可以读取索引库的全部实时数据（文件名、路径、大小、时间、分类），直接执行搜索并打开文件；每一步工具调用都会以卡片展示。</span>'
  +'<div class="ai-suggestions" id="suggBox">'+suggsHtml()+'</div>'
  +'<button class="ai-sugg-refresh" id="suggRefresh" type="button"><span class="glyph" aria-hidden="true">&#xE72C;</span>换一批</button>'
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
  {k:'ask',    n:'询问', h:'每次写入或删除前先询问，确认后才执行'},
  {k:'allow',  n:'允许', h:'所有文件操作都直接执行，不询问'},
];
/* 命令执行权限三档 (run_command 外部程序; 没有"只读" — 执行类默认询问, 允许一次只放一条)。
 * 档位值 0/2/3 与文件权限共轨, 但本表只列三档 — 必须按值匹配, 不能拿值当数组下标 */
const EPOLICY=[
  {v:0, k:'off',   n:'禁用', h:'不允许 AI 执行任何外部命令'},
  {v:2, k:'ask',   n:'询问', h:'每条命令先展示给你确认，点「允许一次」才执行'},
  {v:3, k:'allow', n:'允许', h:'AI 可直接执行命令，不再询问（高危命令仍会标记提醒）'},
];
function epolicyMeta(v){for(const p of EPOLICY)if(p.v===v)return p;return EPOLICY[1];}

/* ==================== 状态 ==================== */
const S={
  cfg:{url:'',model:'',hasKey:false,reasoning:false,policy:2,epolicy:2,sync:false,name:'',ctx:0,maxOut:0,active:'',profs:[],
       img:false,video:false,audio:false,
       maxTurns:30,maxCtxMsgs:30,searchSample:20,readCapKB:30,cmdTo:120,httpTo:120,
       notify:true,cardsOpen:false,instr:''},
  pal:null, convs:[], msgs:[], cur:0,
  atts:[],          /* 待发送附件 [{u:dataUrl,k:kind,n:文件名}] (发送后清空) */
  sending:false, net:0, phase:0, note:'',   /* note = 过程状态条 (重试/自愈中, status 推送带) */
  usage:{has:false,up:0,uo:0,ut:0,uch:0,lp:0,lc:0,tps:0},
  sideOpen:false, cfgOpen:false, policyOpen:false, epolicyOpen:false, usageOpen:false, ctxOpen:false, modelOpen:false,
  cfgTab:'api',     /* 设置面板当前标签页: 'api' 接口 / 'agent' Agent */
  profDirty:false,  /* 表单里有未保存的编辑: 挡住 C++ 整包下发把正在敲的内容冲掉 */
  testing:false,    /* 接口测试进行中 (防重复点击; 结果回来自动复位) */
  delArmed:false, delTimer:0, modelTimer:0,
  clearArmed:false, clearTimer:0,   /* 清空记录两步确认 (首击待确认, 4s 内再击才清) */
  openSteps:{},     /* 展开的工具卡片样本: convId+':'+msgIdx → true */
  toolGrp:{},       /* 展开的连续工具组: convId+':'+k0 → true (缺省 = 含待确认卡才展开) */
  reasonOpen:{},    /* 手动展开的推理块: msgIdx → true (流式自动展开之外的覆盖) */
  turnLog:{},       /* 过程面板开合: convId+':'+组起点 → true 展开/false 收起
                       (undefined=缺省: 流式中最新回合展开、其余收起; 正文开始自动落 false) */
  chgOpen:{},       /* 本轮文件更改块开合: convId+':'+组起点 → true (缺省收起, 只显头部计数) */
  ic:{},            /* 路径图标缓存: 小写路径 → data URL (pathcheck 回包填; linkify 时第一时间
                       内联画上 — 流式/重渲重建 DOM 不再丢图标闪烁; 超 512 项整表清) */
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
  mermaidRetheme();   /* 已出图的卡片按新调色重出 (mermaid 主题色取自 CSS 变量) */
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
  $('f-img').setAttribute('aria-pressed',p&&p.img?'true':'false');
  $('f-video').setAttribute('aria-pressed',p&&p.video?'true':'false');
  $('f-audio').setAttribute('aria-pressed',p&&p.audio?'true':'false');
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
        S.cfg.ctx=p.ctx||0;S.cfg.maxOut=p.maxOut||0;S.cfg.name=profName(p);
        S.cfg.img=!!p.img;S.cfg.video=!!p.video;S.cfg.audio=!!p.audio;}
  if(S.cfgOpen){renderProfSelect();fillProfForm();}
  renderModelBtn();renderStatus();renderUsage();renderComposer();
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
        reasoning:$('f-reason').getAttribute('aria-pressed')==='true',
        img:$('f-img').getAttribute('aria-pressed')==='true',
        video:$('f-video').getAttribute('aria-pressed')==='true',
        audio:$('f-audio').getAttribute('aria-pressed')==='true'});
  showToast('接口设置已保存','ok');
  cfgToggle(false);   /* 保存成功即收起面板 (校验失败才停留) */
}
/* ==================== 页面内 Toast (宿主 Toast 被浏览器子窗盖住; 页内已知消息直接调,
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
  const wasOpen=S.cfgOpen;
  S.cfgOpen=open;
  if(open&&!wasOpen){
    /* 关闭面板 = 放弃未保存的编辑; 打开时从活动档案重填。
       已开时的重入 (renderAll 被 pal/status 等推送反复调) 不得走这里 —
       重填+清 dirty 会静默抹掉正在敲的编辑 */
    S.profDirty=false;profDisarm();
    S.testing=false;const tb=$('b-test');tb.disabled=false;tb.textContent='测试';   /* 上次测试的残留态复位 */
    renderProfSelect();fillProfForm();
    fillAgentForm();setCfgTab(S.cfgTab||'api');
    setTimeout(()=>{try{$('f-prof').focus();}catch(e){}},0);
  }
  $('cfgPanel').hidden=!open;
  $('b-set').setAttribute('aria-expanded',open?'true':'false');
}

/* ==================== Agent 行为设置 (「Agent」标签页; 整包发 agentCfg, C++ 校验落盘) ==================== */
function setCfgTab(tab){
  S.cfgTab=tab==='agent'?'agent':'api';
  $('cfgPageApi').hidden=S.cfgTab!=='api';
  $('cfgPageAgent').hidden=S.cfgTab!=='agent';
  $('cfgTabApi').setAttribute('aria-selected',S.cfgTab==='api'?'true':'false');
  $('cfgTabAgent').setAttribute('aria-selected',S.cfgTab==='agent'?'true':'false');
}
function numOr(v,def){const n=parseInt(v,10);return isFinite(n)&&n>0?n:0;}
function fillAgentForm(){
  $('a-turns').value=String(S.cfg.maxTurns||30);
  $('a-ctx').value=String(S.cfg.maxCtxMsgs||30);
  $('a-sample').value=String(S.cfg.searchSample||20);
  $('a-readcap').value=String(S.cfg.readCapKB||30);
  $('a-cmdto').value=String(S.cfg.cmdTo||120);
  $('a-httpto').value=String(S.cfg.httpTo||120);
  $('a-instr').value=S.cfg.instr||'';
  $('a-notify').setAttribute('aria-pressed',S.cfg.notify!==false?'true':'false');
  $('a-cardsopen').setAttribute('aria-pressed',S.cfg.cardsOpen?'true':'false');
  $('a-web').setAttribute('aria-pressed',S.cfg.web!==false?'true':'false');
  $('a-acompact').setAttribute('aria-pressed',S.cfg.acompact!==false?'true':'false');
}
function saveAgent(){
  /* 每项越界即整体不保存 (接口页 token 校验同口径, 不静默改一半) */
  const V=[[$('a-turns'),1,100],[$('a-ctx'),4,200],[$('a-sample'),3,50],
           [$('a-readcap'),4,512],[$('a-cmdto'),3,600],[$('a-httpto'),30,600]];
  for(const [el,lo,hi] of V){
    const n=numOr(el.value,0);
    if(n<lo||n>hi){showToast('「'+el.placeholder+'」范围内填写数字','warn');try{el.focus();}catch(e){};return;}
  }
  post({c:'agentCfg',maxTurns:numOr($('a-turns').value,30),maxCtxMsgs:numOr($('a-ctx').value,30),
        searchSample:numOr($('a-sample').value,20),readCapKB:numOr($('a-readcap').value,30),
        cmdTo:numOr($('a-cmdto').value,120),httpTo:numOr($('a-httpto').value,120),
        notify:$('a-notify').getAttribute('aria-pressed')==='true',
        cardsOpen:$('a-cardsopen').getAttribute('aria-pressed')==='true',
        web:$('a-web').getAttribute('aria-pressed')==='true',
        acompact:$('a-acompact').getAttribute('aria-pressed')==='true',
        instr:String($('a-instr').value||'').slice(0,4000)});
  showToast('Agent 设置已保存','ok');
  cfgToggle(false);   /* 保存成功即收起面板 */
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
/* 正文开始 → 本回合过程面板自动收起 (2026-09-25 用户口径 "AI 开始回答正文时自动收缩"):
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
    /* 正在生成: 三点在气泡内; 过程状态条 (重试/自愈中) 淡色小字跟随 */
    inner+='<div class="ai-bubble">'+typingHtml()+(S.note?' <span class="ai-note">'+esc(S.note)+'</span>':'')+'</div>';
  }else{
    inner+=m.html||'<div class="ai-bubble">&nbsp;</div>';
    if(m.r===1&&!(S.sending&&mi===S.msgs.length-1)) inner+=metaHtml(mi);   /* 结尾行只给定稿回答 */
  }
  return inner;
}
/* 本轮导出的文件 (轮级归属 — 块挂在发生导出的回合组末尾, 继续对话不会漂移到最后一条下);
 * 收集 [start,end) 内工具卡片消息的 wrote (C++ 随 steps 推送), 去重首现序 */
function turnExportsHtml(start,end){
  const seen=[];
  for(let k=start;k<end;k++){
    const m=S.msgs[k];
    if(m.r!==2||!m.wrote)continue;
    for(const p of m.wrote) if(seen.indexOf(p)<0) seen.push(p);
  }
  if(!seen.length)return '';
  let h='<div class="sess-exports"><div class="se-head">📁 本轮导出的文件 ('+seen.length+')</div>';
  for(const p of seen) h+='<div class="se-it"><span class="ai-path" data-path="'+esc(p)+'">'+esc(p)+'</span></div>';
  return h+'</div>';
}
/* 本轮文件更改 (file_op 逐项记录; 轮级聚合挂在回合组末尾 — 工具卡整组折叠时更改仍可见);
 * 收集 [start,end) 内卡片消息的 chg ({a:动作,f:源,t:目标}), 按三元组去重首现序 */
const CHG_WORDS=['复制','移动','重命名','删除','新建'];
/* data-rec=1 = 更改记录是历史事实: 改名前旧路径不存在是常态, 禁止进 pathcheck 校验
 * (模糊校正会把旧名回填成磁盘新名 → 记录显示"前后名一样"); 点击仍走活解析不改显示。
 * 同目录的一对 (目录大小写不敏感同径) 只显文件名 — 完整路径恒留在 data-path */
function chgDirOf(p){const i=p.lastIndexOf('\\');return i<0?'':p.slice(0,i+1)}
function chgNameOf(p){const i=p.lastIndexOf('\\');return i<0?p:p.slice(i+1)}
function chgRowHtml(c){
  const bad=c.o===0;   /* 失败项: 红样式 + 原因 (用户要知道哪些文件没动成) */
  let h='<div class="se-it'+(bad?' se-bad':'')+'"><span class="schg-w'+(bad?' bad':'')+'">'+(CHG_WORDS[c.a]||'更改')+'</span>';
  const same=!!(c.f&&c.t)&&chgDirOf(c.f).toLowerCase()===chgDirOf(c.t).toLowerCase();
  if(c.f)h+='<span class="ai-path" data-rec="1" data-path="'+esc(c.f)+'">'+esc(same?chgNameOf(c.f):c.f)+'</span>';
  if(c.f&&c.t)h+='<span class="chg-arr">→</span>';
  if(c.t)h+='<span class="ai-path" data-rec="1" data-path="'+esc(c.t)+'">'+esc(same?chgNameOf(c.t):c.t)+'</span>';
  if(bad&&c.e)h+='<span class="chg-err">✕ '+esc(c.e)+'</span>';
  return h+'</div>';
}
function turnChangesHtml(start,end){
  const seen=[];
  for(let k=start;k<end;k++){
    const m=S.msgs[k];
    if(m.r!==2||!m.chg)continue;
    for(const c of m.chg){
      const key=c.a+'|'+c.f+'|'+c.t+'|'+(c.o===0?'x':'ok');
      if(seen.some(x=>x.key===key))continue;
      seen.push({key,c});
    }
  }
  if(!seen.length)return '';
  const nfail=seen.filter(x=>x.c.o===0).length;
  /* 默认收起只显头部计数 (大量改名 33 行全铺会把回答顶出屏); 点头部展开/收起,
     开合态记 S.chgOpen — 流式期回合组频繁重渲, 不落账一刷新就弹回 */
  const key=S.cur+':'+start;
  const open=!!S.chgOpen[key];
  let h='<div class="sess-chg'+(open?' open':'')+'">'
       +'<button class="sess-chg-head" type="button" data-act="chgtoggle" data-k0="'+start+'"'
       +' aria-expanded="'+(open?'true':'false')+'">'
       +'<span class="se-head">🗂 本轮文件更改 ('+seen.length+(nfail?') <em class="se-fail">✕ '+nfail+' 失败</em>':')')+'</span>'
       +'<span class="sarr glyph" aria-hidden="true">&#xE70D;</span></button>'
       +'<div class="sess-chg-body">';
  for(const x of seen) h+=chgRowHtml(x.c);
  return h+'</div></div>';
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
    const sum=!isLive?'':(S.note||(S.phase===1?'执行中…':'生成中…'));   /* S.note=宿主固定文案, 仍走 esc 保持转义口径统一 */
    main='<div class="ai-turn-log'+(open?' open':'')+'">'
        +'<button class="ai-turn-log-head" type="button" data-act="turnlog" aria-expanded="'+(open?'true':'false')+'">'
        +'<span class="ai-turn-log-ic glyph" aria-hidden="true">&#xE9D9;</span>'
        +'<span class="ai-turn-log-label">思考与工具调用</span>'
        +(sum?'<span class="ai-turn-log-sum">'+esc(sum)+'</span>':'')
        +'<span class="ai-turn-log-arr glyph" aria-hidden="true">&#xE70D;</span>'
        +'</button><div class="ai-turn-log-body">'+log+'</div></div>';
  }
  if(finalIsBubble) main+=turnPartHtml(S.msgs[finalIdx],finalIdx,true);
  return '<div class="ai-msg ai-msg-assistant" data-mi="'+start+'">'
    +'<span class="ai-msg-avatar glyph" aria-hidden="true">&#xE99A;</span>'
    +'<div class="ai-msg-main">'+main+turnExportsHtml(start,end)+turnChangesHtml(start,end)+'</div></div>';
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
  /* 含待确认卡/待应用调整卡的组默认展开 (按钮必须可达); 其余缺省 =
     Agent 设置「工具卡片默认展开」(开 = 组也整组展开, 折叠了卡片就看不见) */
  const wait=(cards.match(/data-act="adjApply"/g)||[]).length;
  const open=S.toolGrp[key]!==undefined?S.toolGrp[key]:(ask>0||wait>0||S.cfg.cardsOpen===true);
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
    +'<div class="ai-msg-main">'+(m.html||'<div class="ai-bubble">&nbsp;</div>')+'</div>'
    /* 悬停出现 (row-reverse 布局下落在气泡左外侧): 删除该提问+其后全部回复, 两步确认见 armDel */
    +'<button class="ai-user-del" type="button" data-act="delturn" data-mi="'+mi+'" title="删除此条问答 (含回复; 需二次确认)">'
    +'<span class="glyph" aria-hidden="true">&#xE74D;</span></button></div>';
}
/* 短气泡收缩: 单段、无块级元素、纯文本 ≤30 字才不跟右缘对齐 */
function applyBubbleShapes(root){
  (root||$('threadInner')).querySelectorAll('.ai-msg-assistant .ai-bubble').forEach(b=>{
    const text=String(b.textContent||'').replace(/\s+/g,' ').trim();
    const block=b.querySelector('br,ul,ol,table,pre,blockquote,hr,h1,h2,h3,h4,h5,h6,div');
    b.classList.toggle('ai-bubble-short',!text||(!block&&b.querySelectorAll('p').length===1&&text.length<=30));
  });
}
/* 样本列表展开态回放 (msgs 全量重推后 JS 自持的展开态不丢; 箭头随开合转向)。
 * 未手动开合过的卡片缺省态 = Agent 设置「工具卡片默认展开」(S.cfg.cardsOpen) */
function applyOpenSteps(){
  document.querySelectorAll('#threadInner .step').forEach(card=>{
    const gi=+card.getAttribute('data-gi');
    const key=S.cur+':'+gi;
    const open=S.openSteps[key]!==undefined?!!S.openSteps[key]:(S.cfg.cardsOpen===true);
    const body=card.querySelector('.ssamples');
    if(body) body.style.display=open?'':'none';
    card.classList.toggle('open',open);
  });
}
function renderThread(keepScroll){
  disarmDel();   /* 整帧重绘 = 确认态复位 (msgs 全量重推后节点已换, 残留 arm 会误跳二次确认) */
  const inner=$('threadInner'), t=threadEl();
  const stick=keepScroll?isNearBottom():true;
  const showEmpty=!S.msgs.length&&!S.sending;
  $('jumpbar').hidden=true;
  if(showEmpty){ inner.innerHTML=EMPTY_HTML; updateJumpbar(); updatePendAsk(); if(stick)scrollToEnd(); return; }   /* updatePendAsk: 空态也要收确认条 — 挂起裁决中切/删会话后残条 = 按钮无收件人的死条 */
  let html='';
  for(let i=0;i<S.msgs.length;){
    if(S.msgs[i].r===0){ html+=userRowHtml(S.msgs[i],i); i++; continue; }
    html+=turnGroupHtml(i);          /* 助手侧连续段 = 一个回合组 (导出块随所属回合) */
    i=turnEnd(i);
  }
  inner.innerHTML=html;
  enhance(inner);
  applyBubbleShapes(); applyOpenSteps();
  if(stick){S.follow=true;scrollToEnd();}
  updateJumpbar();
  updatePendAsk();
}
/* 挂起确认常驻条: 只认 kind 11/13 (run_command/file_op) 的 state==4 — 这两种才真挂起
 * 等裁决 (eallow/edeny 通道); open_file/copy_paths 的策略询问卡也标 state4 但不挂起
 * (pallow/pdeny 通道), 进了常驻条会点不掉。标题行 = 动作摘要 + 允许/拒绝按钮, 下方 =
 * 确认明细正文 (与卡上同源的逐项清单/风险提示) — 过程面板收起、滚上去看历史时裁决
 * 入口不丢。注意: 本条在 threadInner 之外, 点击走自己的监听器 (initPendBar) */
function updatePendAsk(){
  const bar=$('pendbar');if(!bar)return;
  let hit=null;
  S.msgs.forEach(function(m,mi){
    if(hit||m.r!==2||!m.steps)return;
    m.steps.forEach(function(s,si){
      if(hit||+s.state!==4)return;
      if(s.k!==11&&s.k!==13)return;
      hit={mi:mi,si:si,argz:s.argz||'',detail:s.err||''};
    });
  });
  if(!hit){bar.hidden=true;bar.innerHTML='';bar.removeAttribute('data-k');return;}
  const key=hit.mi+':'+hit.si;
  if(!bar.hidden&&bar.getAttribute('data-k')===key)return;
  bar.hidden=false;bar.setAttribute('data-k',key);
  bar.setAttribute('data-mi',hit.mi);bar.setAttribute('data-si',hit.si);   /* 裁决与步骤同 mi/si (宿主现只读裸标志, 属性照口径带上) */
  bar.innerHTML='<div class="pendbar-row"><span class="pendbar-t">⏸ '+esc(hit.argz||'AI 等待确认')+'</span>'
    +'<button class="pendbar-b primary" type="button" data-act="execallow">允许一次</button>'
    +'<button class="pendbar-b" type="button" data-act="execdeny">拒绝</button></div>'
    +(hit.detail?'<div class="pendbar-d">'+esc(hit.detail).replace(/\n/g,'<br>')+'</div>':'');
}
function initPendBar(){
  $('pendbar').addEventListener('click',function(e){
    const b=e.target.closest('.pendbar-b');if(!b)return;
    const bar=$('pendbar');
    post({c:b.getAttribute('data-act')==='execallow'?'eallow':'edeny',
      mi:+bar.getAttribute('data-mi'),si:+bar.getAttribute('data-si')});
  });
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
  updatePendAsk();
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
/* 一轮包含的消息节点: 本轮用户提问起, 到下一轮提问之前 */
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

/* ==================== 可点击交互 (搜索卡片 / 语法高亮 / 路径链接 / 右键菜单) ====================
 * AI 决定点击的类型, 一律标准 Markdown 链接语法 (链接文字 = 给用户看的动作指引):
 * [..](xjs://search?text=..&mode=..) = 搜索卡片 (点击置入搜索框并按模式执行);
 * [..](xjs://open|reveal?id=<FileId>) = 文件动作链接 — 模型只输出引擎 FileId, 路径由
 * C++ 按 ID 解析 (path 参数 = 旧历史消息的路径版链接, 兼容受理); 正文里确有的绝对路径 =
 * 自动文件链接 (单击打开, 右键 打开/定位/复制); 其余照旧 (外链 openurl)。
 * enhance() 挂在每次消息 HTML 落地之后 (innerHTML 重建后节点全新, 幂等无需去重)。 */
const MODE_LABELS={wildcard:'通配符',regex:'正则',sql:'SQL',lua:'Lua过滤','lua-exec':'Lua执行',lua_exec:'Lua执行'};
/* 路径字符边界: 只硬停在 ASCII 空白/引号/尖括号/管道/星号/冒号/问号/正斜杠。
 * 全角标点/汉字段一律照吞, 前端不再猜名字边界 — 2026-09-25 双重实锤:
 * ① 模型写全角 "决战！碧游村4K" (真名原样), 旧"全角标点后跟汉字=正文"规则把候选截在
 *    "F:\分享文件夹\电视剧", 校验反而命中存在的父目录 = 误判;
 * ② "折磨 SM 粗暴 紧缚 2" 的纯汉字段被硬停, 同样截短。
 * 边界歧义 (路径后跟正文/句读) 一律交给 C++ 梯子按存在性裁断: 正文截断/括号失衡/
 * 尾标点剥离都是带存在性检验的变体, 命中哪个显示哪个 (ResolveClickablePath, ai_web.cpp)。 */
const PATH_STOP=/[\s'"`<>|*\/:?]/;
const PATH_RE=new RegExp("(?<![A-Za-z0-9])(?:[A-Za-z]:[\\\\/]|\\\\\\\\)[^\\s'\"`<>|*/:?]+","g");   /* lookbehind 排除 https: 里的 "s:/" */
function pathStopCh(ch){return PATH_STOP.test(ch);}
function extendPath(text,e){
  let k=e;
  for(;;){
    if(text[k]!==' ')break;
    let j=k+1;
    while(j<text.length&&!pathStopCh(text[j]))j++;
    if(j===k+1)break;
    k=j;   /* 全量并入 (含全角标点/纯汉字段): 校验梯子决定真实边界 */
  }
  return k;
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
/* 通用高亮 (python/js/bash/json/c): 关键字+字符串+注释+数字 近似着色, 不做完整 tokenizer */
const PY_KW=new Set('False None True and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield match case'.split(' '));
const JS_KW=new Set('async await break case catch class const continue debugger default delete do else enum export extends false finally for from function if implements import in instanceof interface let new null of private protected public readonly return static super switch this throw true try typeof undefined var void while with yield'.split(' '));
const SH_KW=new Set('if then else elif fi for while until do done case esac function in select break continue return exit local export readonly declare unset shift eval trap echo cd ls pwd grep sed awk cat chmod chown cp mv rm mkdir rmdir touch find tar zip unzip curl wget sudo apt yum dnf systemctl service kill ps top df du head tail sort uniq wc xargs tee which whoami git npm node python pip'.split(' '));
const C_KW=new Set('alignas alignof auto bool break case catch char class const constexpr continue decltype default delete do double dynamic_cast else enum explicit extern false float for friend goto if inline int long mutable namespace new noexcept nullptr operator private protected public register return short signed sizeof static static_cast struct switch template this throw true try typedef typeid typename union unsigned using virtual void volatile while include define ifndef endif pragma'.split(' '));
function hlGeneric(code,src,lang){
  const kw=lang==='python'?PY_KW:lang==='bash'?SH_KW:lang==='c'?C_KW:JS_KW;
  /* 统一 tokenizer: 注释 (# 与 // /*) | 字符串 | 数字 | 标识符 | 空白 | 兜底单字符 */
  const re=/((?:#|\/\/)[^\n]*|\/\*[\s\S]*?(?:\*\/|$))|("(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|`(?:\\.|[^`\\])*`)|(\d+(?:\.\d+)?)|([A-Za-z_@#][\w]*)|(\s+)|([\s\S])/g;
  let html='',m;
  while((m=re.exec(src))){
    if(m[1])html+='<span class="tok-c">'+esc(m[1])+'</span>';
    else if(m[2])html+='<span class="tok-s">'+esc(m[2])+'</span>';
    else if(m[3])html+='<span class="tok-n">'+esc(m[3])+'</span>';
    else if(m[4]){
      const w=m[4];
      if(kw.has(w)||kw.has(w.toLowerCase()))html+='<span class="tok-k">'+esc(w)+'</span>';
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
    /* 通用高亮语言集 (python/js/bash/json/c; 关键字+字符串+注释+数字 近似着色) */
    const GENERIC={python:1,py:1,javascript:1,js:1,typescript:1,ts:1,bash:1,sh:1,shell:1,zsh:1,
                   json:1,c:1,cpp:1,'c++':1,java:1};
    if(lang!=='lua'&&lang!=='luau'&&lang!=='sql'&&!GENERIC[lang])return;
    const code=box.querySelector('pre code');
    /* data-code 是 C++ HtmlEscape 后的属性: 浏览器解析时已解码一次, getAttribute 拿到的
       即原文 — 再过 decodeHtml 会把代码里字面的 &lt; 之类实体样文本二次解码成标签字符 */
    const raw=box.getAttribute('data-code')||'';
    if(!code||!raw)return;
    if(lang==='lua'||lang==='luau')hlApply(code,raw,'lua');
    else if(lang==='sql')hlApply(code,raw,'sql');
    else hlGeneric(code,raw,lang==='py'?'python':(lang==='js'||lang==='ts')?'javascript':
      (lang==='sh'||lang==='shell'||lang==='zsh')?'bash':(lang==='cpp'||lang==='c++'||lang==='java')?'c':lang);
  });
}
/* 气泡正文里的绝对路径 → 可点击 .ai-path 链接 (TreeWalker 只碰文本节点,
   跳过代码块/链接/已有链接/按钮; 句尾标点剥出链接外) */
/* 句尾标点剥离: 配对 closers 只在失衡 (= 正文括号包住了路径, 如 "(见 F:\a\b)") 时剥,
   平衡的名字后缀保留 ("美人鱼 (2016)" 结尾的 ")" 不再被剥掉, 旧规则恒剥 = 点不开);
   名字末尾真带 closer 的由 C++ 侧 ResolveClickablePath 补标点兜底 */
function stripPathTail(p){
  for(;;){
    const ch=p[p.length-1];
    if(ch===undefined)return p;
    const ci=')）]}】」』》〉'.indexOf(ch);
    if(ci>=0){
      const op='(（[{【「『《〈'[ci];
      let no=0,nc=0;
      for(const c of p){if(c===op)no++;else if(c===ch)nc++;}
      if(nc>no){p=p.slice(0,-1);continue;}
      return p;
    }
    if(/[.。,，;；:!?‘’“”…'"]$/.test(ch)){p=p.slice(0,-1);continue;}
    return p;
  }
}
function linkifyPaths(root){
  const scope=root||$('threadInner');
  const walker=document.createTreeWalker(scope,NodeFilter.SHOW_TEXT,{acceptNode:function(n){
    const v=n.nodeValue;
    if(!v||v.length<4)return NodeFilter.FILTER_REJECT;
    const p=n.parentElement;
    /* .ai-chip / .scmd (工具卡头查询串) 里的路径文本不链接化: 点击分发里 chip 分支与
       卡头分支先于 .ai-path 命中, 链接化后路径子串会吞掉这两处自己的点击语义 */
    if(!p||p.closest('.ai-code,a,.ai-path,button,textarea,select,script,style,.ai-chip,.scmd'))return NodeFilter.FILTER_REJECT;
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
      if(end-m.index<4)continue;   /* 截到只剩盘符根 ("C:\（注）…" 的 "C:\") 不成链接 */
      if(end<=last)continue;
      const path=text.slice(m.index,end);
      const strip=stripPathTail(path);
      if(strip.length<3)continue;
      if(!frag)frag=document.createDocumentFragment();
      if(m.index>last)frag.appendChild(document.createTextNode(text.slice(last,m.index)));
      const sp=document.createElement('span');
      sp.className='ai-path';
      sp.setAttribute('data-path',strip);
      const moreRaw=text.substr(end,100);
      if(moreRaw.trim())sp.setAttribute('data-more',moreRaw);   /* 链接后紧随原文 = 校验期"继续解析"的余量 */
      sp.title='点击打开 · 右键更多操作';
      sp.textContent=strip;
      const ic=S.ic[strip.toLowerCase()];   /* 已知图标第一时间内联 (重渲不再闪烁) */
      if(ic)sp.insertAdjacentHTML('afterbegin','<img class="ai-path-ic" src="'+ic+'" alt="" aria-hidden="true">');
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
/* ==================== mermaid 图表 (```mermaid 围栏 → C++ 出 .ai-mermaid 卡) ====================
 * 库 = 插件目录 mermaid.min.js (v11 IIFE), 经 WebView2 虚拟主机映射同源载入 (C++ 控制器创建时
 * SetVirtualHostNameToFolderMapping); 缺文件/旧运行时 = window.mermaid 为空, 图表块恒为源码卡。
 * 流式期间不出图 (每帧重建 DOM, mermaid 渲染太贵), 回合收尾/历史装载的整帧重渲统一出图;
 * 换肤 (pal) 重置已出图卡片按新调色重出。渲染失败 = 错误行 + 保留源码 (内容不丢)。 */
let mmSeq=0;
function mmThemeVars(){
  const cs=getComputedStyle(document.documentElement),v=k=>String(cs.getPropertyValue('--'+k)||'').trim()||'#8888aa';
  return {background:'transparent',fontFamily:'Segoe UI, Microsoft YaHei UI, sans-serif',fontSize:'13px',
    primaryColor:v('accent-violet'),primaryTextColor:v('text-primary'),primaryBorderColor:v('accent-violet'),
    secondaryColor:v('surface-raised'),tertiaryColor:v('surface-raised'),mainBkg:v('surface-raised'),
    nodeBorder:v('accent-violet'),lineColor:v('text-secondary'),textColor:v('text-primary'),
    titleColor:v('text-primary'),edgeLabelBackground:v('surface-raised'),clusterBkg:v('surface-raised'),
    clusterBorder:v('glass-border'),actorBkg:v('surface-raised'),actorBorder:v('accent-violet'),
    actorTextColor:v('text-primary'),signalTextColor:v('text-primary'),labelBoxBkgColor:v('surface-raised'),
    labelBoxBorderColor:v('glass-border'),noteBkgColor:v('accent-amber'),noteTextColor:v('text-primary'),
    noteBorderColor:v('accent-amber'),
    /* pie 系列扇区色: 不接皮肤色时第 2+ 扇区默认近黑, 暗色皮肤下整图糊底 (实测实锤) */
    pie1:v('accent-violet'),pie2:v('accent-cyan'),pie3:v('accent-emerald'),pie4:v('accent-amber'),
    pie5:v('accent-pink'),pie6:v('text-tertiary'),pieOpacity:1,
    pieLegendTextColor:v('text-primary'),pieTitleTextColor:v('text-primary'),pieSectionTextColor:v('text-primary')};
}
function mmInit(){
  if(!window.mermaid)return false;
  try{window.mermaid.initialize({startOnLoad:false,securityLevel:'strict',theme:'base',logLevel:'fatal',
    themeVariables:mmThemeVars()});}catch(e){return false;}
  return true;
}
/* ---- 数学公式: md4c LaTeX 开关输出的 .ai-math 喂 KaTeX (库缺 = 按源码文本显示) ---- */
function renderMath(scope){
  const root=(scope&&scope.querySelectorAll)?scope:document;
  const els=root.querySelectorAll('.ai-math:not([data-kx])');
  if(!els.length||!window.katex)return;
  els.forEach(function(el){
    el.setAttribute('data-kx','1');
    try{
      window.katex.render(el.textContent,el,{throwOnError:false,
        displayMode:el.classList.contains('ai-math-disp')});
    }catch(e){el.removeAttribute('data-kx');}
  });
}
/* ---- emoji 短代码 :white_check_mark: → 真 emoji (代码块/链接/按钮内不动) ---- */
const EMOJI={white_check_mark:'✅',heavy_check_mark:'✔️',check:'✓',x:'❌',negative_squared_cross_mark:'❎',
 warning:'⚠️',rocket:'🚀',sparkles:'✨',fire:'🔥',bulb:'💡',memo:'📝',clipboard:'📋',pushpin:'📌',
 lock:'🔒',unlock:'🔓',key:'🔑',key2:'🗝️',gear:'⚙️',hammer:'🔧',wrench:'🛠️',mag:'🔍',mag_right:'🔎',
 file_folder:'📁',open_file_folder:'📂',card_index:'🗂️',page_facing_up:'📄',date:'📅',hourglass:'⌛',
 alarm_clock:'⏰',bar_chart:'📊',chart:'📊',chart_with_upwards_trend:'📈',chart_with_downwards_trend:'📉',
 clock:'🕐',floppy_disk:'💾',computer:'💻',house:'🏠',office:'🏢',question:'❓',exclamation:'❗',
 star:'⭐',zap:'⚡',wastebasket:'🗑️',package:'📦',inbox_tray:'📥',outbox_tray:'📤',link:'🔗',
 paperclip:'📎',bookmark:'🔖',pencil:'✏️',book:'📖',books:'📚',blue_book:'📘',camera:'📷',
 iphone:'📱',battery:'🔋',point_right:'👉',point_down:'👇',thumbsup:'👍',thumbsdown:'👎',
 ok_hand:'👌',pray:'🙏',eyes:'👀',brain:'🧠',dart:'🎯',trophy:'🏆',medal:'🏅',
 one:'1️⃣',two:'2️⃣',three:'3️⃣',four:'4️⃣',five:'5️⃣',six:'6️⃣',seven:'7️⃣',eight:'8️⃣',nine:'9️⃣'};
const EMOJI_RE=/:([a-z0-9_]{2,24}):/g;
function applyEmoji(scope){
  const root=scope||$('threadInner');
  if(!root)return;
  /* createTreeWalker 挂在 document 上 (Element 没有), root 可以是任意节点 */
  const walker=document.createTreeWalker(root,NodeFilter.SHOW_TEXT,{
    acceptNode:function(n){
      if(!n.nodeValue||n.nodeValue.length<5||n.nodeValue.indexOf(':')<0)return NodeFilter.FILTER_REJECT;
      const p=n.parentElement;
      if(!p||p.closest('.ai-code,a,.ai-path,button,textarea,select,script,style,.ai-chip,.scmd,pre'))return NodeFilter.FILTER_REJECT;
      return NodeFilter.FILTER_ACCEPT;
    }});
  const nodes=[];let n;
  while((n=walker.nextNode()))nodes.push(n);
  nodes.forEach(function(node){
    const v=node.nodeValue;
    EMOJI_RE.lastIndex=0;
    if(!EMOJI_RE.test(v))return;
    let html='',last=0,m;
    EMOJI_RE.lastIndex=0;
    while((m=EMOJI_RE.exec(v))){
      const e=EMOJI[m[1]];
      if(!e)continue;
      html+=esc(v.slice(last,m.index))+e;
      last=m.index+m[0].length;
    }
    if(!last)return;
    html+=esc(v.slice(last));
    const sp=document.createElement('span');
    sp.innerHTML=html;
    node.parentNode.replaceChild(sp,node);
  });
}
function mmSanitize(src){
  /* 模型高频把 引号/圆括号 裸写进 flowchart 节点标签 — 未加引号的标签一遇这些字符
     解析必炸 (Parse error ... Expecting 'SQE' 实锤)。官方转义口径 = 标签整体包双引号,
     内部双引号写 #quot; 实体。只对 flowchart/graph 保守修复: 已引号包裹的标签不动,
     仅在标签内真含危险字符时才包; 裸圆括号形态 (B(文字)) 不碰 — 与边文本里的普通
     括号无法可靠区分, 包错反而毁掉本来合法的图; 其余图型原样过 (失败仍走源码回退)。 */
  if(!/^\s*(flowchart|graph)\b/im.test(src)){
    /* timeline: 时段/事件文字里的冒号是分隔符, 行首裸时间 (07:18 : x) 解析必炸
       (Expecting EOF/SPACE/NEWLINE/title 实锤) — 行首 HH:MM 改写为 07时18分 */
    if(/^\s*timeline\b/im.test(src)){
      src=src.split('\n').map(function(line){
        return line.replace(/^(\s*)(\d{1,2}):(\d{2})(?=\s*:)/,'$1$2时$3分');
      }).join('\n');
    }
    /* xychart: 轴分类列表里未加引号的项自动包双引号 — 词法表 axis_data 态不认裸 '-'
       等特殊字符 (x-axis [C-系统,...] 直接 Lexical error 实锤, 2026-10-01), 引号字符串
       才是词法表收的形态; 已引号/含裸引号的项不动 (保守, 包错更糟)。bar/line 数字数组
       与 y-axis 线性区间 (0 --> N) 无括号列表, 天然不匹配不碰 */
    if(/^\s*xychart(-beta)?\b/im.test(src)){
      src=src.split('\n').map(function(line){
        const m=/^(\s*(?:x|y)-axis(?:\s+"[^"]*")?\s*\[)(.*)(\]\s*)$/.exec(line);
        if(!m)return line;
        const fixed=m[2].split(',').map(function(it){
          const t=it.trim();
          if(!t)return it;
          if(t.length>=2&&t.charAt(0)==='"'&&t.charAt(t.length-1)==='"')return it;
          if(t.indexOf('"')>=0)return it;
          return ' "'+t+'"';
        });
        return m[1]+fixed.join(',')+m[3];
      }).join('\n');
    }
    return src;
  }
  const W=/[A-Za-z0-9_\u00C0-\uFFFF]/;
  return src.split('\n').map(function(line){
    if(/^\s*(%%|flowchart\b|graph\b|subgraph\b|direction\b|end\s*$)/i.test(line))return line;
    let out='',i=0;
    while(i<line.length){
      const ch=line[i];
      let ls=i+1,closeSeq=null;
      if(ch==='[')closeSeq=']';
      else if(ch==='{')closeSeq='}';
      else if(ch==='('&&line[i+1]==='('&&W.test(line[i-1]||' '))closeSeq='))';
      if(!closeSeq||((ch==='('||ch==='{')&&!W.test(line[i-1]||' '))){out+=ch;i++;continue;}
      let ce=-1,cl=1;
      if(ch==='['&&line[i+1]==='('){
        ls=i+2;
        const j=line.indexOf(')]',ls);
        if(j>=0){ce=j;cl=2;}
      }else if(closeSeq===']'||closeSeq==='}'||closeSeq===')'){
        let d=1,j=ls;
        for(;j<line.length;j++){const c2=line[j];
          if(c2===ch)d++;
          else if(c2===closeSeq){d--;if(!d)break;}}
        if(j<line.length){ce=j;cl=1;}
      }else{
        const j=line.indexOf(closeSeq,ls);
        if(j>=0){ce=j;cl=2;}
      }
      if(ce<0){out+=ch;i++;continue;}   /* 未闭合: 原样放过 */
      const inner=line.slice(ls,ce);
      if(inner.length>=2&&inner.charAt(0)==='"'&&inner.charAt(inner.length-1)==='"'){
        out+=line.slice(i,ce+cl);i=ce+cl;continue;   /* 已引号包裹: 原样 */
      }
      const danger=/["]/.test(inner)||
        (ch==='['&&/[(){}]/.test(inner))||
        (ch==='{'&&/[()\[\]]/.test(inner))||
        (ch==='('&&/["()\[\]{}]/.test(inner));
      if(danger){
        out+=line.slice(i,ls)+'"'+inner.replace(/"/g,'#quot;')+'"'+line.slice(ce,ce+cl);
        i=ce+cl;continue;
      }
      out+=line.slice(i,ce+cl);i=ce+cl;
    }
    return out;
  }).join('\n');
}
function renderMermaid(scope){
  const root=(scope&&scope.querySelectorAll)?scope:document;
  const els=root.querySelectorAll('.ai-mermaid:not([data-mm])');
  if(!els.length||!window.mermaid||S.sending)return;   /* 流式中保持源码卡; 库未就绪同 */
  if(!mmInit())return;
  els.forEach(function(card){
    card.setAttribute('data-mm','1');
    const src=card.querySelector('.ai-mermaid-src'),out=card.querySelector('.ai-mermaid-out');
    if(!src||!out)return;
    const id='mmd'+(++mmSeq);
    try{
      window.mermaid.render(id,mmSanitize(src.textContent)).then(function(r){
        out.innerHTML=r.svg;card.setAttribute('data-done','1');
      }).catch(function(e){
        out.innerHTML='<div class="ai-mermaid-err">'+esc('图表渲染失败 (语法有误), 已保留源码: '+
          String((e&&e.message)||e).slice(0,160))+'</div>';
      });
    }catch(e){out.innerHTML='<div class="ai-mermaid-err">图表渲染失败</div>';}
  });
}
function mermaidRetheme(){
  mmLbClose();   /* 灯箱里的克隆图引用卡片内 defs, 重渲后悬空 — 直接收掉最稳 */
  if(!window.mermaid)return;
  const done=document.querySelectorAll('.ai-mermaid[data-done]');
  if(!done.length)return;
  done.forEach(function(card){
    card.removeAttribute('data-done');card.removeAttribute('data-mm');
    const out=card.querySelector('.ai-mermaid-out');
    if(out)out.innerHTML='';
  });
  renderMermaid(document);
}
/* ---- 图表灯箱 (宿主预览灯箱同口径): 点击已出图的图表 → 整面板放大看 — 滚轮缩放
   (5%~800%, 缩放中心跟光标), 按住拖动平移 (指针捕获, 拖出窗不丢), 原地点击/Esc/✕ 收,
   拖动结束不算点击。克隆卡片里已渲好的 SVG (矢量, 任意缩放不糊); 换肤重渲时直接收灯箱
   (克隆图引用卡片内 defs, 悬空会掉箭头)。 ---- */
let mmLbState=null;
function mmLbApply(){
  const st=mmLbState;if(!st)return;
  st.holder.style.transform='translate('+st.tx+'px,'+st.ty+'px) scale('+st.scale+')';
}
function mmLbClose(){
  const lb=document.getElementById('mmLb');
  if(lb)lb.remove();
  mmLbState=null;
}
function mmLbOpen(svg){
  mmLbClose();
  const bb=svg.getBoundingClientRect();
  if(!bb.width||!bb.height)return;
  const lb=document.createElement('div');lb.className='ai-mm-lb';lb.id='mmLb';
  const vp=document.createElement('div');vp.className='mm-vp';
  const holder=document.createElement('div');holder.className='mm-holder';
  const clone=svg.cloneNode(true);
  clone.style.maxWidth='none';clone.style.width='100%';clone.style.height='100%';
  holder.style.width=bb.width+'px';holder.style.height=bb.height+'px';
  holder.appendChild(clone);vp.appendChild(holder);
  const x=document.createElement('button');x.className='mm-x';x.setAttribute('type','button');
  x.textContent='✕';x.title='关闭';
  const hint=document.createElement('div');hint.className='mm-hint';
  hint.textContent='滚轮缩放 · 按住拖动 · 点击空白或 Esc 关闭';
  lb.appendChild(vp);lb.appendChild(x);lb.appendChild(hint);
  document.body.appendChild(lb);
  const vw=innerWidth,vh=innerHeight;
  const fit=Math.min(1,(vw*0.88)/bb.width,(vh*0.88)/bb.height);
  mmLbState={holder:holder,fit:fit,scale:fit,
             tx:(vw-bb.width*fit)/2,ty:(vh-bb.height*fit)/2};
  mmLbApply();
  let drag=null;
  vp.addEventListener('pointerdown',function(e){
    if(e.button!==0||!mmLbState)return;
    drag={x:e.clientX,y:e.clientY,tx:mmLbState.tx,ty:mmLbState.ty,moved:false};
    try{vp.setPointerCapture(e.pointerId);}catch(err){}
    e.preventDefault();
  });
  vp.addEventListener('pointermove',function(e){
    if(!drag||!mmLbState)return;
    const dx=e.clientX-drag.x,dy=e.clientY-drag.y;
    if(Math.abs(dx)>3||Math.abs(dy)>3)drag.moved=true;
    mmLbState.tx=drag.tx+dx;mmLbState.ty=drag.ty+dy;mmLbApply();
  });
  vp.addEventListener('pointerup',function(){
    const wasDrag=drag&&drag.moved;
    drag=null;
    if(!wasDrag)mmLbClose();   /* 原地点击 = 关; 拖完松手不关 */
  });
  vp.addEventListener('pointercancel',function(){drag=null;});
  vp.addEventListener('wheel',function(e){
    e.preventDefault();
    const st=mmLbState;if(!st)return;
    const r=vp.getBoundingClientRect();
    const cx=e.clientX-r.left,cy=e.clientY-r.top;
    const ns=Math.min(8,Math.max(0.05,st.scale*(e.deltaY<0?1.15:1/1.15)));
    st.tx=cx-(cx-st.tx)/st.scale*ns;   /* 缩放中心跟光标: 光标下的内容点保持原位 */
    st.ty=cy-(cy-st.ty)/st.scale*ns;
    st.scale=ns;mmLbApply();
  },{passive:false});
  x.addEventListener('click',function(ev){ev.stopPropagation();mmLbClose();});
}
/* 库 defer 载入可能晚于首帧渲染 (开面板即见历史图表卡): 全部资源就绪后再补一轮 */
window.addEventListener('load',function(){renderMermaid(document);renderMath(document);});
function enhance(root){
  const scope=root||$('threadInner');
  transformActionLinks(scope);
  highlightCode(scope);
  renderMermaid(scope);
  renderMath(scope);
  applyEmoji(scope);
  linkifyPaths(scope);
  schedulePathCheck();
}
/* ---- 路径存在性校验 (C++ pathcheck 批量后端, 2026-09-25 用户口径): 解析出的候选先问
   真实存在性 — 不存在按解析梯子继续试 (more 向后并词/去尾词回退, 与点击解析同一梯子),
   仍不存在 = 退回纯文本, 不画链接不做字符特殊处理。校验异步: enhance 落地后 300ms
   去抖汇总未校验的 .ai-path, 单批 ≤64 条, 余量随回复的 schedulePathCheck 下一轮续检。
   data-rec=1 的链接 (文件更改记录块) 整体排除: 记录是历史事实, 旧路径不存在是常态,
   模糊校正回填会让记录显示成"前后名一样" — 显示恒为落库原样, 点击仍走活解析 */
let pvTimer=0;
function schedulePathCheck(){
  if(pvTimer)return;
  pvTimer=setTimeout(function(){pvTimer=0;sendPathCheck();},300);
}
let icoTimer=0;
function scheduleIcoRetry(){
  if(icoTimer)return;
  icoTimer=setTimeout(function(){icoTimer=0;sendPathCheck();},3000);
}
function sendPathCheck(){
  const ps=[],ids=[],seen={};
  const add=function(sp,isId){
    if(isId){const id=sp.getAttribute('data-id');
      if(!seen['i'+id]){seen['i'+id]=1;ids.push(id);}return;}
    const p=sp.getAttribute('data-path');
    if(p&&!seen['p'+p]){seen['p'+p]=1;ps.push(p);}
  };
  document.querySelectorAll('.ai-path[data-path]:not([data-rec]):not([data-ok]):not([data-bad]),.ai-path[data-id]:not([data-rec]):not([data-ok]):not([data-bad])').forEach(function(sp){
    if(sp.hasAttribute('data-badid'))return;   /* 渲染期已判定的编造 ID: 保持禁用态 */
    add(sp,sp.hasAttribute('data-id'));
  });
  document.querySelectorAll('.ai-path[data-needico]').forEach(function(sp){
    if(sp.querySelector('.ai-path-ic')){sp.removeAttribute('data-needico');return;}
    if(sp.hasAttribute('data-rec'))return;
    const t=+(sp.getAttribute('data-ict')||0);
    if(t>=40){sp.removeAttribute('data-needico');return;}
    sp.setAttribute('data-ict',t+1);
    add(sp,sp.hasAttribute('data-id'));
  });
  if(ps.length||ids.length)post({c:'pathcheck',ps:ps.slice(0,64),ids:ids.slice(0,64)});
}
function applyPathCheck(rs){
  const byKey={};
  (rs||[]).forEach(function(r){byKey[(r.k||'')+'|'+(r.v||'')]=r;});
  let icoRetry=false;
  document.querySelectorAll('.ai-path[data-path]:not([data-rec]):not([data-bad]),.ai-path[data-id]:not([data-rec]):not([data-bad])').forEach(function(sp){
    if(sp.hasAttribute('data-badid'))return;
    const isId=sp.hasAttribute('data-id');
    const key=isId?('i|'+sp.getAttribute('data-id')):('p|'+sp.getAttribute('data-path'));
    const r=byKey[key];
    if(!r)return;   /* 不在本批 (超上限的余量) */
    if(!r.ok){
      if(!sp.hasAttribute('data-ok'))sp.replaceWith(document.createTextNode(sp.textContent));return;}   /* 不存在 = 纯文本; 已 ok 的补投递轮不翻转 */
    if(!sp.hasAttribute('data-ok')){
      sp.setAttribute('data-ok','1');
      if(!isId&&r.q){
        const q=r.q,old=sp.getAttribute('data-path'),shown=sp.textContent;
        if(q!==old){
          sp.setAttribute('data-path',q);   /* 点击/右键直接用校正后的真路径 */
          sp.removeAttribute('data-more');
          if(q.indexOf(shown)===0&&shown.length<q.length){
            /* 继续解析并入了更长路径: 后续文本对上才补齐显示 (对不上只改 data-path, 点击已通) */
            const add=q.slice(shown.length),nx=sp.nextSibling;
            if(nx&&nx.nodeType===3&&nx.nodeValue.indexOf(add)===0){
              nx.nodeValue=nx.nodeValue.slice(add.length);
              sp.textContent=q;
            }
          }else if(shown.indexOf(q)===0){
            /* 粘连的正文词被回退剥掉: 剥下的余量还原为纯文本 */
            sp.textContent=q;
            sp.parentNode.insertBefore(document.createTextNode(shown.slice(q.length)),sp.nextSibling);
          }else sp.textContent=q;   /* 名字模糊校正等: 直接落磁盘权威拼写 */
        }
      }
    }
    if(r.ic&&!sp.querySelector('.ai-path-ic')){   /* 引擎真图标置前 (data URL; 先落文字再插图标);
        同时进 S.ic 缓存 — 原路径与校正后路径双键 (重渲时 linkify 按原文本路径查),
        重渲时 linkify 第一时间内联, 不再闪 */
      if(r.v)S.ic[String(r.v).toLowerCase()]=r.ic;
      if(r.q&&r.q!==r.v)S.ic[String(r.q).toLowerCase()]=r.ic;
      if(Object.keys(S.ic).length>512)S.ic={};
      sp.insertAdjacentHTML('afterbegin','<img class="ai-path-ic" src="'+r.ic+'" alt="" aria-hidden="true">');
      sp.removeAttribute('data-needico');
    }else if(!sp.querySelector('.ai-path-ic')){
      sp.setAttribute('data-needico','1');   /* 校验过了但图标没拿到 (agent 忙/扫描期/配额尽): 挂补投递慢轮重试 */
      icoRetry=true;
    }
  });
  if(icoRetry)scheduleIcoRetry();
  schedulePathCheck();
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
/* ---- 消息 HTML 内的点击 (事件委托) ---- */
function decodeHtml(s){const t=document.createElement('textarea');t.innerHTML=s;return t.value;}
function copyTurn(mi){
  const m=S.msgs[mi];
  if(!m)return;
  let text=String(m.t||'').trim();   /* 原始 Markdown = 事实源, 换行/列表/代码块结构全保留 */
  if(!text&&m.html){const d=document.createElement('div');d.innerHTML=m.html;text=(d.textContent||'').trim();}
  if(!text&&m.reason)text=String(m.reason).trim();
  if(text)post({c:'copy',text});
}
/* ---- 删除问答的两步确认 (用户气泡悬停按钮): 首击进入确认态 (变红"确认删除"),
   4s 内再击才发 delturn; 切换目标/整帧重绘 (renderThread)/超时自动复位 —
   arm 态只活在前端 DOM, msgs 推送重建节点后自然消失 */
function disarmDel(){
  if(S.delArmTimer){clearTimeout(S.delArmTimer);S.delArmTimer=0;}
  if(S.delArmBtn){S.delArmBtn.classList.remove('arm');
    S.delArmBtn.innerHTML='<span class="glyph" aria-hidden="true">&#xE74D;</span>';}
  S.delArm=-1;S.delArmBtn=null;
}
/* 附件图片放大浮层 (点任意处/Esc/点外失焦收) */
function hideLightbox(){const lb=$('lightbox');if(lb)lb.remove();}
function showLightbox(src){
  hideLightbox();
  const lb=document.createElement('div');
  lb.className='ai-lightbox';lb.id='lightbox';
  const im=document.createElement('img');im.src=src;im.alt='';
  lb.appendChild(im);
  lb.addEventListener('mousedown',e=>{e.preventDefault();hideLightbox();});
  document.body.appendChild(lb);
}
function armDel(mi,btn){
  disarmDel();
  S.delArm=mi;S.delArmBtn=btn;
  S.delArmTimer=setTimeout(disarmDel,4000);
  btn.classList.add('arm');btn.textContent='确认删除';
}
function bindThread(){
  const inner=$('threadInner');
  inner.addEventListener('click',e=>{
    const lbi=e.target.closest('.ai-att-img');
    if(lbi){showLightbox(lbi.getAttribute('src'));return;}
    const mmel=e.target.closest('.ai-mermaid-out svg');
    if(mmel){mmLbOpen(mmel);return;}
    const fh=e.target.closest('.ai-fold-head');
    if(fh){fh.closest('.ai-fold').classList.toggle('open');return;}
    const rf=e.target.closest('.ai-sugg-refresh');
    if(rf){rerollSuggs();return;}
    const sug=e.target.closest('.ai-suggestion');
    /* 示例提示词 = 起草不发送: 填入输入框让用户确认/修改后自己发 (2026-09-25 用户口径) */
    if(sug){const ta=$('inputT');
      if(ta&&!S.sending){ta.value=sug.getAttribute('data-q')||sug.textContent;autoSize();ta.focus();refreshSendState();}
      return;}
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
    const del=e.target.closest('.ai-user-del');
    if(del){
      if(S.sending){showToast('回答进行中, 请先停止或等完成后再删除','warn');return;}
      const mi=+del.closest('.ai-msg').getAttribute('data-mi');
      if(S.delArm!==mi){armDel(mi,del);return;}   /* 首击=确认态, 4s 内再击才真正删除 */
      disarmDel();post({c:'delturn',mi});
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
      else post({c:act,path:pth.getAttribute('data-path')||pth.textContent,more:pth.getAttribute('data-more')||''});
      return;}
    const a=e.target.closest('a');
    if(a){e.preventDefault();const href=a.getAttribute('href')||'';
      if(/^https?:/i.test(href))post({c:'openurl',href});return;}
    const cp=e.target.closest('.ai-code-copy');
    if(cp){const box=cp.closest('.ai-code');const code=box?(box.getAttribute('data-code')||''):'';   /* 属性已解码一次, 不过 decodeHtml (同 highlightCode 口径) */
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
      /* execallow/execdeny (允许一次/拒绝) 不在此分发: 按钮只存在于输入框上方常驻确认条
         (#pendbar, 在 threadInner 之外), 点击由它自己的 initPendBar 监听器处理 */
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
    const ch=e.target.closest('.sess-chg-head');
    if(ch){const key=S.cur+':'+ch.getAttribute('data-k0');
      S.chgOpen[key]=!S.chgOpen[key];
      const box=ch.closest('.sess-chg');
      box.classList.toggle('open',S.chgOpen[key]);
      ch.setAttribute('aria-expanded',S.chgOpen[key]?'true':'false');
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
/* 右键: 文件路径 / 搜索卡片 / 工具卡片的操作菜单 (其余区域保留浏览器原生菜单 = 选区复制入口) */
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
      const ref=id?{id:+id}:{path:pth.getAttribute('data-path')||pth.textContent,more:pth.getAttribute('data-more')||''};
      showCtx([
        {t:'打开文件',fn:()=>post(Object.assign({c:'open'},ref))},
        {t:'定位文件',fn:()=>post(Object.assign({c:'reveal'},ref))},
        '-',
        {t:'复制路径',fn:()=>post(Object.assign({c:'copypath'},ref))}
      ],e.clientX,e.clientY);
      return;
    }
    /* 工具卡片 (.step): 右键 = 执行语句 (搜索卡: 重放该语句并把结果同步进窗口列表)
       + 复制查询语句全文 (data-q 为未截断原文, .scmd 只显示前 200 字)。
       落在此分支前, 卡内样本路径已被 .ai-path 分支接走; 其余区域保留原生菜单 = 选区复制入口 */
    const stp=e.target.closest('.step');
    if(stp){
      e.preventDefault();
      const q=stp.getAttribute('data-q')||'';
      const mode=stp.getAttribute('data-mode')||'';
      const req=stp.getAttribute('data-req')||'';
      const items=[];
      if(q&&mode)items.push({t:'执行语句',fn:()=>post({c:'execstmt',gi:+stp.getAttribute('data-gi'),si:+stp.getAttribute('data-si')})});
      if(q)items.push({t:'复制查询语句',fn:()=>post({c:'copy',text:q})});
      if(req)items.push({t:'要求返回的字段',fn:()=>showToast(req,'ok')});
      if(items.length)showCtx(items,e.clientX,e.clientY);
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
  if(!open)hideJumpTip();
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
  const grayed=!S.sending&&!hasText&&!S.atts.length;   /* 有附件 (即使没文字) 也可发送 */
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
  /* 📎 附件入口只对勾了对应能力的模型出现 (图片/视频/音频 任一) */
  $('attB').hidden=!(S.cfg.img||S.cfg.video||S.cfg.audio);
  refreshSendState();
  renderPolicy();
  renderEpolicy();
  renderSync();
  renderUsage();
}
/* ---- 多模态附件 (粘贴 / 📎 选择; 图片压到 ≤1568px, 视频/音频原样 data URL) ---- */
const ATT_KIND_NAME={0:'图片',1:'视频',2:'音频'};
function fmtBytes(n){if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(1)+' KB';return (n/1048576).toFixed(1)+' MB';}
function attCapKind(u){   /* data URL 前缀 → kind (其它类型不收) */
  if(/^data:image\//i.test(u))return 0;
  if(/^data:video\//i.test(u))return 1;
  if(/^data:audio\//i.test(u))return 2;
  return -1;
}
function addAtt(u,kind,name){
  if(S.atts.length>=4){showToast('每条消息最多 4 个附件','warn');return;}
  S.atts.push({u,k:kind,n:name||''});
  renderAtts();refreshSendState();
}
function renderAtts(){
  const row=$('attRow');
  row.hidden=!S.atts.length;
  row.innerHTML=S.atts.map((a,i)=>{
    const meta='<span class="ai-att-meta"><span class="ai-att-name">'+esc(a.n||ATT_KIND_NAME[a.k])+'</span>'
      +'<span class="ai-att-size">'+fmtBytes(Math.round(a.u.length*0.75))+'</span></span>';
    const th=a.k===0
      ?'<img class="ai-att-thumb" src="'+a.u+'" alt="">'
      :'<span class="ai-att-thumb glyph" aria-hidden="true">'+(a.k===1?'&#xE714;':'&#xE8D6;')+'</span>';
    return '<div class="ai-att">'+th+meta
      +'<button class="ai-att-x glyph" data-i="'+i+'" type="button" title="移除" aria-label="移除附件">&#xE74D;</button></div>';
  }).join('');
}
/* 图片读入 → 压缩 (长边 >1568 缩放; PNG ≤2.5MB 保留透明, 否则 JPEG 白底 0.9) → data URL */
function compressImage(dataUrl,cb){
  const img=new Image();
  img.onload=()=>{
    const MAXD=1568;
    const k=Math.max(1,img.naturalWidth/MAXD,img.naturalHeight/MAXD);
    const w=Math.max(1,Math.round(img.naturalWidth/k)),h=Math.max(1,Math.round(img.naturalHeight/k));
    const small=k>1||dataUrl.length>1.5*1024*1024;
    const done=u=>{
      if(u.length>4*1024*1024){showToast('图片过大 (压缩后仍超 4MB)','warn');return;}
      cb(u);
    };
    if(!small){done(dataUrl);return;}
    const cv=document.createElement('canvas');cv.width=w;cv.height=h;
    const cx=cv.getContext('2d');
    cx.drawImage(img,0,0,w,h);
    const png=cv.toDataURL('image/png');
    if(png.length<=2.5*1024*1024){done(png);return;}
    cx.fillStyle='#fff';cx.fillRect(0,0,w,h);cx.drawImage(img,0,0,w,h);   /* JPEG 无透明: 白底垫 */
    done(cv.toDataURL('image/jpeg',0.9));
  };
  img.onerror=()=>showToast('图片读取失败','warn');
  img.src=dataUrl;
}
function addFiles(files){
  for(const f of files){
    if(!f)continue;
    const isImg=/^image\//i.test(f.type),isVid=/^video\//i.test(f.type),isAud=/^audio\//i.test(f.type);
    if(!isImg&&!isVid&&!isAud){showToast('不支持的附件类型：'+(f.name||f.type),'warn');continue;}
    const kind=isImg?0:isVid?1:2;
    const capOn=isImg?S.cfg.img:isVid?S.cfg.video:S.cfg.audio;
    if(!capOn){showToast('当前模型未开启'+ATT_KIND_NAME[kind]+'输入 (接口设置中勾选)','warn');continue;}
    /* 原始字节数粗筛 (data URL≈4/3×原字节; C++ 侧按 data URL 长度硬闸, 留出头部余量) */
    if(isVid&&f.size>17*1048576){showToast('视频过大 (上限约 17MB)：'+f.name,'warn');continue;}
    if(isAud&&f.size>8*1048576){showToast('音频过大 (上限约 8MB)：'+f.name,'warn');continue;}
    const rd=new FileReader();
    rd.onload=()=>{
      const u=String(rd.result||'');
      const k=attCapKind(u);
      if(k<0){showToast('附件读取失败：'+(f.name||''),'warn');return;}
      if(k===1&&u.length>23*1048576){showToast('视频过大 (超 24MB data URL 上限)：'+f.name,'warn');return;}
      if(k===2&&u.length>11*1048576){showToast('音频过大 (超 12MB data URL 上限)：'+f.name,'warn');return;}
      if(k===0)compressImage(u,q=>addAtt(q,0,f.name));
      else addAtt(u,k,f.name);
    };
    rd.onerror=()=>showToast('附件读取失败：'+(f.name||''),'warn');
    rd.readAsDataURL(f);
  }
}
function doSend(){
  const ta=$('inputT');
  const text=ta.value.trim();
  if(S.sending||(!text&&!S.atts.length))return;
  if(!S.cfg.hasKey){  /* 未配置: 打开接口设置面板 + 明确提示 (与参考实现同口径) */
    showToast('尚未配置接口密钥 — 请填写接口地址、API 密钥与模型','warn');
    cfgToggle(true);
    return;
  }
  const atts=S.atts.map(a=>({u:a.u,k:a.k,n:a.n}));
  ta.value='';autoSize();
  S.atts=[];renderAtts();
  refreshSendState();
  post({c:'send',text,atts});
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
    epolicySetOpen(false);   /* 互斥: 命令执行权限下拉 (close 分支不回环) */
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

/* ---- 命令执行权限下拉 (run_command 外部程序; 结构复用文件权限下拉) ---- */
function renderEpolicy(){
  const cur=epolicyMeta(S.cfg.epolicy);
  $('epolicyLabel').textContent=cur.n;
  const btn=$('epolicyBtn');
  btn.dataset.policy=cur.k;
  btn.title='命令执行权限：'+cur.h;
  document.querySelectorAll('#epolicyMenu .ai-cmd-policy-option').forEach(op=>{
    const i=+op.getAttribute('data-epolicy');
    const p=epolicyMeta(i);
    op.querySelector('.ai-cmd-policy-option-label').textContent=p.n;
    op.querySelector('.ai-cmd-policy-option-hint').textContent=p.h;
    op.setAttribute('aria-checked',i===S.cfg.epolicy?'true':'false');
  });
}
function epolicySetOpen(open){
  S.epolicyOpen=open;   /* 开关标志先落账 (点外收起/Escape/按钮切换都读它) */
  const btn=$('epolicyBtn'),menu=$('epolicyMenu');
  btn.setAttribute('aria-expanded',open?'true':'false');
  clearTimeout(S.epolicyTimer);
  if(open){
    usageSetOpen(false);
    policySetOpen(false);
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
  S.epolicyTimer=setTimeout(()=>{
    if(btn.getAttribute('aria-expanded')!=='true'){menu.hidden=true;menu.classList.remove('closing');}
  },90);
}

/* ---- 结果同步勾选 (run_search 结果 → 左侧搜索结果列表; 单击切换, 无下拉) ---- */
function renderSync(){
  const on=!!S.cfg.sync;
  const b=$('syncBtn');
  b.setAttribute('aria-pressed',on?'true':'false');
  b.title=on?'结果同步已开启：AI 每次工具搜索（含 Lua 脚本选中的文件）的结果会同步显示到左侧搜索结果列表，点此关闭'
           :'结果同步已关闭：开启后 AI 工具搜索的结果会同步显示到左侧搜索结果列表';
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
  let top=anchor.top-pr.height-6;
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
      S.sending=!!m.st.sending;S.net=m.st.net;S.phase=m.st.phase||0;S.note=m.st.note||'';
      S.usage=m.usage||S.usage;
      maybeAutoCollapseTurn();
      renderAll();break;
    case 'pal':S.pal=m.pal;applyPal(S.pal);renderAll();break;
    case 'cfg':
      S.cfg=m.cfg;
      /* 面板开着且表单干净 → 从新活动档案重填 (接口+Agent 两页都要, 多窗广播下 Agent 页
         不重填 = 输入框陈旧值整包写回覆盖别窗刚保存的设置); 有未保存编辑就不动 (不冲掉正在敲的内容)。
         面板不再随广播自动收起 (保存后停留展示 + toast 确认, 取消/✕ 才收) */
      if(S.cfgOpen&&!S.profDirty){fillProfForm();fillAgentForm();}
      renderProfSelect();renderModelBtn();
      if(S.modelOpen)renderModelMenu();
      renderStatus();renderComposer();
      break;
    case 'convs':S.convs=m.convs||[];renderSide();break;
    case 'dropPaths':{
      /* 拖放非媒体文件回填 (C++ 按名匹配列表拖出快照; hit=解析成完整路径的个数) */
      const arr=m.paths||[];
      if(!arr.length)break;
      const t=$('inputT'),add=arr.join(' ')+' ';
      if(t.value&&!/\s$/.test(t.value))t.value+=' ';
      t.value+=add;
      autoSize();refreshSendState();
      showToast(m.hit===arr.length?'已把文件完整路径填入输入框':'已把文件填入输入框 (部分仅有文件名)');
      try{$('inputT').focus();}catch(err){}
      break;
    }
    case 'pathcheck':applyPathCheck(m.r);break;
    case 'msgs':
    if(m.cur!==undefined&&S.cur!==m.cur)S.reasonOpen={};   /* 会话切换: 裸消息下标键整体平移失效 */
    else if(S.msgs.length>(m.msgs||[]).length){   /* 消息被删 (delturn/重试截断) 同理: 四张下标键表一并对齐清,
        否则旧键漂移到别的卡片/回合组上 (开合态错位) */
      S.reasonOpen={};S.openSteps={};S.toolGrp={};S.chgOpen={};
    }
      if(m.cur!==undefined)S.cur=m.cur;
      S.msgs=m.msgs||[];
      if(m.st){S.sending=!!m.st.sending;S.net=m.st.net;S.phase=m.st.phase||0;S.note=m.st.note||'';}
      maybeAutoCollapseTurn();
      renderThread(true);renderStatus();renderComposer();renderSide();break;
    case 'last':{
      if(!S.msgs.length)break;
      S.msgs[S.msgs.length-1]=m.m;
      if(m.st){S.sending=!!m.st.sending;S.phase=m.st.phase||0;S.note=m.st.note||'';}
      maybeAutoCollapseTurn();
      applyLast();renderStatus();break;}
    case 'usage':S.usage=m.u||S.usage;renderUsage();break;
    case 'toast':showToast(m.msg,{0:'info',1:'ok',2:'warn',3:'err'}[m.k]||'warn');break;
    case 'testResult':{
      /* 接口测试回包: 复位按钮 + 按结果着色 (ok=绿=接口可用; 其余=失败原因) */
      S.testing=false;const tb=$('b-test');tb.disabled=false;tb.textContent='测试';
      showToast(m.msg||('测试'+(m.ok?'成功':'失败')),m.ok?'ok':'err');break;
    }
    case 'status':
      S.sending=!!m.sending;S.net=m.net;S.phase=m.phase||0;S.note=m.note||'';
      maybeAutoCollapseTurn();
      renderStatus();renderThread(true);renderComposer();renderSide();break;
    case 'blur':
      /* 浏览器焦点离开面板 (点击宿主/切走窗口): 收起瞬态弹层与悬浮侧栏。
         面板外的点击 WebView2 收不到, 页面自己的点外关闭够不着, 由 C++ 补发;
         停靠侧栏 (宽面板) 与设置面板是常驻模式, 不随焦点收 */
      if(S.ctxOpen)hideCtx();
      if($('lightbox'))hideLightbox();
      mmLbClose();   /* 图表灯箱同收 (克隆图随面板失焦, 留着无交互意义) */
      if(S.policyOpen)policySetOpen(false);
      if(S.usageOpen)usageSetOpen(false);
      if(S.modelOpen)modelSetOpen(false);
      if(S.epolicyOpen)epolicySetOpen(false);   /* 命令执行权限下拉与文件权限同构, blur 同收 */
      if(S.sideOpen&&sideFloating())sideToggle(false);
      break;
  }
}
function renderAll(){
  renderProfSelect();renderModelBtn();
  renderStatus();cfgToggle(S.cfgOpen);renderThread(false);renderComposer();renderSide();
}
/* ==================== 事件接线 ==================== */
function bind(){
  const ta=$('inputT');   /* 输入框引用提前: 下方粘贴/附件接线同帧就要用到 (const 有暂时性死区) */
  $('b-set').addEventListener('click',()=>cfgToggle(!S.cfgOpen));
  initPendBar();
  $('b-hist').addEventListener('click',()=>sideToggle(!S.sideOpen));
  /* 侧栏头部 ✕ = 显式关闭入口 (停靠模式下没有点外收起, 必须有可见的关闭钮) */
  $('sideClose').addEventListener('click',()=>sideToggle(false));
  $('b-new').addEventListener('click',()=>post({c:'new'}));
  $('b-close').addEventListener('click',()=>post({c:'close'}));
  $('b-cancel').addEventListener('click',()=>cfgToggle(false));
  $('b-save').addEventListener('click',saveProf);
  $('b-test').addEventListener('click',()=>{   /* 接口测试: 表单当前值直发 (未保存也算数);
                                                  密钥留空由 C++ 用已存密钥, 明文不出宿主 */
    if(S.testing)return;
    if(!$('f-url').value.trim()||!$('f-model').value.trim()){
      showToast('请先填写接口地址与模型名称','warn');return;
    }
    S.testing=true;
    const tb=$('b-test');tb.disabled=true;tb.textContent='测试中…';
    post({c:'profTest',url:$('f-url').value.trim(),key:$('f-key').value.trim(),model:$('f-model').value.trim()});
  });
  /* Agent 标签页: 切页 / 保存 / 取消 / 两勾选 (与接口页同面板, 点外/Esc 收起共用) */
  $('cfgTabApi').addEventListener('click',()=>setCfgTab('api'));
  $('cfgTabAgent').addEventListener('click',()=>setCfgTab('agent'));
  $('b-agent-cancel').addEventListener('click',()=>cfgToggle(false));
  $('b-agent-save').addEventListener('click',saveAgent);
  ['a-notify','a-cardsopen','a-web','a-acompact'].forEach(id=>{
    $(id).addEventListener('click',()=>{
      const b=$(id);
      const on=b.getAttribute('aria-pressed')==='true';
      b.setAttribute('aria-pressed',on?'false':'true');
    });
  });
  /* 模型档案: 下拉切换 / 新建 / 复制 / 删除 (删除是两步确认); 保存成功/点外/Esc 均收起面板 */
  $('f-prof').addEventListener('change',()=>selectProf($('f-prof').value));
  $('f-prof-add').addEventListener('click',()=>{S.profDirty=false;post({c:'profNew',dup:false});});
  $('f-prof-dup').addEventListener('click',()=>{S.profDirty=false;post({c:'profNew',dup:true});});   /* 真布尔: C++ 侧只认 t==1 (曾发数字 1 恒判假 = 复制变新建空档案) */
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
           S.cfg.ctx=np.ctx||0;S.cfg.maxOut=np.maxOut||0;S.cfg.name=profName(np);
           S.cfg.img=!!np.img;S.cfg.video=!!np.video;S.cfg.audio=!!np.audio;}
    else{S.cfg.url='';S.cfg.model='';S.cfg.hasKey=false;S.cfg.ctx=0;S.cfg.maxOut=0;S.cfg.name='未配置接口';
         S.cfg.img=false;S.cfg.video=false;S.cfg.audio=false;}
    if(S.cfgOpen){renderProfSelect();fillProfForm();}
    renderModelBtn();renderStatus();renderUsage();renderComposer();
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
  /* 多模态能力三勾选 (图片/视频/音频): 编辑即标脏 — 挡住宿主整包下发冲掉未保存的勾选 */
  ['f-img','f-video','f-audio'].forEach(id=>{
    $(id).addEventListener('click',()=>{
      const b=$(id);
      const on=b.getAttribute('aria-pressed')==='true';
      b.setAttribute('aria-pressed',on?'false':'true');
      S.profDirty=true;
    });
  });
  /* 📎 附件: 选择文件 / 粘贴图片 (粘贴含媒体文件时接管, 纯文本照常走浏览器默认) */
  $('attB').addEventListener('click',()=>$('attFile').click());
  $('attFile').addEventListener('change',()=>{addFiles(Array.from($('attFile').files||[]));$('attFile').value='';});
  $('attRow').addEventListener('click',e=>{
    const x=e.target.closest('.ai-att-x');
    if(!x)return;
    S.atts.splice(+x.getAttribute('data-i'),1);
    renderAtts();refreshSendState();
  });
  ta.addEventListener('paste',e=>{
    const files=e.clipboardData&&e.clipboardData.files;
    if(!files||!files.length)return;
    const media=Array.from(files).filter(f=>f&&/^(image|video|audio)\//i.test(f.type));
    if(!media.length)return;   /* 剪贴板里只有文本/其它: 浏览器默认粘贴 */
    e.preventDefault();
    addFiles(media);
  });
  /* 面板任意处粘贴媒体 (焦点在对话流/工具栏时也接得住): 输入框自己的 paste 已
     preventDefault, 这里的 closest(textarea,input) 守卫防止二次入列 */
  document.addEventListener('paste',e=>{
    if(e.target&&e.target.closest&&e.target.closest('textarea,input'))return;
    const files=e.clipboardData&&e.clipboardData.files;
    const media=files?Array.from(files).filter(f=>f&&/^(image|video|audio)\//i.test(f.type)):[];
    if(!media.length)return;
    e.preventDefault();
    addFiles(media);
    try{ta.focus();}catch(err){}
  });
  /* 拖放文件 (左侧搜索结果 / 资源管理器拖入皆可): 媒体走 addFiles 附件管线 (同 📎/粘贴);
     非媒体文件浏览器拿不到真实路径 (CF_HDROP 到页面只剩 File 对象), 把文件名填入输入框,
     AI 用搜索工具按名定位。dragover 必须 preventDefault, 否则松手触发 WebView2 默认
     "拖入即导航", 整个面板被文件内容替换 */
  let dragDepth=0;   /* dragenter/leave 在子元素间成对抖动, 计数归零才算真离开 */
  const dragHasFiles=e=>!!(e.dataTransfer&&Array.prototype.indexOf.call(e.dataTransfer.types||[],'Files')>=0);
  document.addEventListener('dragenter',e=>{
    if(!dragHasFiles(e))return;
    e.preventDefault();
    dragDepth++;
    document.body.setAttribute('data-dragover','true');
  });
  document.addEventListener('dragover',e=>{
    if(!dragHasFiles(e))return;
    e.preventDefault();
    e.dataTransfer.dropEffect='copy';
  });
  document.addEventListener('dragleave',e=>{
    if(!dragHasFiles(e))return;
    dragDepth=Math.max(0,dragDepth-1);
    if(!dragDepth)document.body.removeAttribute('data-dragover');
  });
  document.addEventListener('drop',e=>{
    if(!dragHasFiles(e))return;
    e.preventDefault();
    dragDepth=0;document.body.removeAttribute('data-dragover');
    const files=Array.from(e.dataTransfer.files||[]);
    const media=files.filter(f=>f&&/^(image|video|audio)\//i.test(f.type||''));
    const names=files.filter(f=>f&&!/^(image|video|audio)\//i.test(f.type||'')).map(f=>f.name);
    if(media.length)addFiles(media);
    if(names.length){
      /* 非媒体: 页面只有文件名 (沙箱), 完整路径由 C++ 按名匹配最近一次列表拖出快照
         回填 (恒回复 dropPaths; 匹配不上原样给名) — 见 handle() 的 dropPaths 分支 */
      post({c:'dropPaths',names:names});
    }
    try{ta.focus();}catch(err){}
  });
  $('sendB').addEventListener('click',()=>{if(S.sending)post({c:'stop'});else doSend();});
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
  $('epolicyBtn').addEventListener('click',()=>epolicySetOpen(!S.epolicyOpen));
  $('epolicyMenu').addEventListener('click',e=>{
    const op=e.target.closest('.ai-cmd-policy-option');
    if(!op)return;
    epolicySetOpen(false);
    post({c:'epolicy',v:+op.getAttribute('data-epolicy')});
  });
  /* 结果同步: 单击即切换 (宿主落盘+广播, 在跑作业即时跟进) — 本地先落账保持跟手 */
  $('syncBtn').addEventListener('click',()=>{
    S.cfg.sync=!S.cfg.sync;
    renderSync();
    post({c:'sync',v:S.cfg.sync?1:0});
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
    if(S.epolicyOpen&&!e.target.closest('#epolicy')&&!e.target.closest('#epolicyMenu'))epolicySetOpen(false);
    if(S.usageOpen&&!e.target.closest('#usagePanel')&&!e.target.closest('#usageBtn'))usageSetOpen(false);
    if(S.modelOpen&&!e.target.closest('#modelPicker'))modelSetOpen(false);
    /* 悬浮侧栏点外收起 (排除 #b-hist: 按钮自己的 click 负责开关, mousedown 先收会把它再弹开) */
    if(S.sideOpen&&sideFloating()&&!e.target.closest('#side')&&!e.target.closest('#b-hist'))sideToggle(false);
    /* 接口设置面板点外收起 = 取消未保存的编辑 (同样排除 #b-set: 它的 click 负责开关) */
    if(S.cfgOpen&&!e.target.closest('#cfgPanel')&&!e.target.closest('#b-set'))cfgToggle(false);
  });
  document.addEventListener('keydown',e=>{
    if(e.key==='Escape'){
      if(document.getElementById('mmLb')){mmLbClose();e.preventDefault();return;}
      if($('lightbox')){hideLightbox();e.preventDefault();return;}
      if(S.clearArmed){clearDisarm();e.preventDefault();return;}
      if(S.ctxOpen){hideCtx();e.preventDefault();return;}
      if(S.modelOpen){modelSetOpen(false);e.preventDefault();return;}
      if(S.cfgOpen){profDisarm();cfgToggle(false);e.preventDefault();return;}
      if(S.policyOpen){policySetOpen(false);e.preventDefault();return;}
      if(S.epolicyOpen){epolicySetOpen(false);e.preventDefault();return;}
      if(S.usageOpen){usageSetOpen(false);e.preventDefault();return;}
    }
  });
  window.addEventListener('resize',()=>{hideJumpTip();hideCtx();if(S.modelOpen)modelSetOpen(false);});
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
