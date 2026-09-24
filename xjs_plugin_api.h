#ifndef XJS_PLUGIN_API_H
#define XJS_PLUGIN_API_H
/* ============================================================================
 * xjs_plugin_api.h — 插件扩展 API 内部接缝 (宿主内部头, 插件作者不可见)
 * ============================================================================
 * 职责: "EXE 设置 / 界面操作 / 搜索模式管理" 这组扩展宿主 API 的宿主侧声明。
 *   插件侧入口 = 宿主表尾的 host->QueryApi(name) 名称解析 (xjs_plugin_sdk.h);
 *   实现全部收在本模块配套的 xjs_plugin_api.cpp, 不堆进 xjs_plugin.cpp (一文件一职责)。
 *
 * 三方分工:
 *   xjs_plugin.cpp    注册表/闸门/窗口令牌 (唯一事实源, 本模块经下面的薄包装取用)
 *   xjs_plugin_api.cpp QueryApi 解析器 + 12 个扩展 API + 运行时搜索模式存储
 *   xjs_plugin_sdk.h  插件作者可见面 (XJS_API_* 名称常量 + XjsApi* 函数指针类型)
 * ============================================================================ */

#include "xjs_app.h"

/* 与 xjs_plugin_sdk.h 同款调用约定宏 (两个宿主文件都可见; x64 下为空) */
#ifndef XJS_PLUGIN_CALL
#define XJS_PLUGIN_CALL
#endif

struct XjsPluginCtx;   /* 完整定义在 xjs_plugin_sdk.h (只有两个插件 cpp include 它) */

/* ---- 名称解析入口 (xjs_plugin_api.cpp 实现; xjs_plugin.cpp 挂宿主表尾) ----
   ctx 非法/未知名 = NULL; 返回的函数指针终身有效 (static 函数地址)。
   权限/线程闸不在这里做 — 各 API 入口照常过闸, 解析器只管"名字 → 函数"。 */
void* XJS_PLUGIN_CALL XjsPluginApiQuery(XjsPluginCtx* ctx, const char* name);

/* ---- 闸门/身份 (xjs_plugin.cpp 实现: 注册表细节不外泄, 只给窄口) ---- */
/* 统一闸门 = PluginApiCheck(ctx, perm, uiOnly=true) 薄包装 (全部扩展 API 仅 UI 线程);
   perm = XPP_* 位 (0 = 免权限, 只查启用闸); cap = 需同时声明的能力位 (0 = 不查)。
   返回 XJS_PLUGIN_OK 或错误码 (禁用/未授权 = ERR_PERM, 错线程 = ERR_THREAD)。 */
int  XjsPluginApiGate(XjsPluginCtx* ctx, unsigned perm, unsigned cap = 0);
/* 窗口令牌 → 窗口 (0 = 主窗缺省; 失效 = NULL, *err = ERR_NOTFOUND) */
XjsSearchWindow* XjsPluginApiWindow(XjsPluginCtx* ctx, unsigned long long token, int* err);
/* 调用插件身份 id (目录名; 归属标记用, 运行时模式的 owner) */
void XjsPluginApiPluginId(XjsPluginCtx* ctx, std::wstring* out);
/* 窗口 → 令牌 (windows.enum / window.create 回传用) */
unsigned long long XjsPluginApiTokenOf(XjsSearchWindow* w);

/* ---- 插件互操作桥梁的消息派发 (注册表侧落点, xjs_plugin.cpp 实现) ----
   按目标 id 找条目 → 启用/加载/收信口校验 → 同步调目标 OnPluginMessage (恒 UI 线程;
   深度闸防 A↔B 互发递归爆栈)。广播 = 快照收信人后逐个派发, 不收集回复, 不送达自己。 */
int XjsPluginApiMsgSend(XjsPluginCtx* sender, const char* targetIdUtf8,
                        const char* jsonUtf8, char* buf, int cap);
int XjsPluginApiMsgBroadcast(XjsPluginCtx* sender, const char* jsonUtf8);

/* ---- 输出/转义小工具 (唯一实现 xjs_plugin.cpp, 两个插件 cpp 共用) ---- */
int  PluginBufOut(char* buf, int cap, const std::string& s);
void PluginJsonEscape(const std::string& s, std::string* out);
std::string PluginJsonStr(const std::string& s);

/* ---- 关键词模式名↔值 (xjs_plugin.cpp 接线 SearchSetText 的 mode 参数用) ----
   名串与 settings.set "搜索模式"/配置文件同源 ("wildcard|regex|sql|lua|lua-exec",
   与 XMODE_* 序一致); 非法名 = -1。
   应用口径在 xjs_plugin.cpp 侧与设置页同款: 写 w->mode + XjsSaveConfig, 重搜时机按 execute。 */
int XjsPluginApiKeyModeIndexFromUtf8(const std::wstring& s);

/* ---- 运行时搜索模式存储 (xjs_plugin_api.cpp 持有; 会话级, 不落盘) ----
 * 插件经 modes.add 运行时添加的模板型模式: 与清单 "搜索模式" 同一合并视图 (药丸菜单/
 * 托管标签链/prune 全走既有路径, 引擎零改动)。srcId 仍 = "p:<插件id>:<序>", 但序号从
 * XJS_PLUGIN_RT_MODE_BASE 起 — 清单模式序永远到不了这里, 段内自增不复用 (删除不漂移)。
 * xjs_plugin.cpp 的 XjsPluginModeCount/At/DefAt 在清单模式之后接上这三口。 */
static const int XJS_PLUGIN_RT_MODE_BASE = 10000;
int  XjsPluginApiRtModeCount(int pluginIdx);                              /* 该插件运行时模式数 */
const XjsPluginModeDef* XjsPluginApiRtModeDefAt(int pluginIdx, int srcIdx); /* 越界/失效 = NULL */
void XjsPluginApiPruneOwners();   /* 重扫后调用: owner 已消失的运行时模式整条剪掉 */

#endif /* XJS_PLUGIN_API_H */
