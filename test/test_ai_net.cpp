/*
 * test_ai_net.cpp — 联网层纯解析单元验证 (ai_net.cpp)
 * 链 ai_core.obj + ai_file.obj + ai_net.obj (插件示例\ai-assistant\build.bat 产物),
 * 不发网络请求不起引擎不碰 UI (HttpGet/两工具实体不参与)。
 * 覆盖: bing RSS 解析 / bing 结果页 b_algo 解析 (含 ck/a 跳转还原与 href 实体) /
 *       DuckDuckGo Lite 解析 (含 uddg 还原) / WebUnwrapResultUrl / WebHtmlToText /
 *       AiCapUtf8HeadTail (头尾保留 + 精确省略量 + UTF-8 边界落刀)。
 * 构建 (仓库根执行, 先跑过插件 build.bat 让 .obj 在位; /I 解决 include 定位; 库集同插件 build.bat):
 *   cl /nologo /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /Fotest\ /I插件示例\ai-assistant test\test_ai_net.cpp 插件示例\ai-assistant\ai_core.obj 插件示例\ai-assistant\ai_file.obj 插件示例\ai-assistant\ai_net.obj ^
 *      /Fe:test\test_ai_net.exe /link winhttp.lib user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib gdiplus.lib windowscodecs.lib propsys.lib runtimeobject.lib xunjieso.lib
 *   (注意 /Fotest\ 不带引号 — /Fo"test\" 的 \" 会被解析成转义引号吞掉后续源文件, 报 D8003)
 */
#include "ai_assistant.h"
#include <stdio.h>

/* ai_plugin.cpp / ai_agent.cpp / ai_session.cpp / ai_web*.cpp 未参与链接, 补最小外部符号 */
const XjsPluginHost* g_host = NULL;
XjsPluginCtx* g_ctx = NULL;
unsigned g_uiThread = 0;
HWND g_msgwnd = NULL;

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } \
} while (0)

static bool Contains(const std::wstring& hay, const wchar_t* nd) {
    return hay.find(nd) != std::wstring::npos;
}

/* ---------- bing RSS (主路; 结构 = 2026-09-26 实测 cn.bing.com 返回) ---------- */
static void TestBingRss() {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?><rss version=\"2.0\"><channel>"
        "<title>必应：测试查询</title><link>http://www.bing.com:80/search?q=x</link>"
        "<description>搜索结果</description>"
        "<item><title>第一个结果 &amp; 标题</title>"
        "<link>https://example.com/one?x=1&amp;y=2</link>"
        "<description>摘要一 &lt;b&gt;加粗&lt;/b&gt; 文本</description></item>"
        "<item><title><![CDATA[第二个结果 CDATA 标题]]></title>"
        "<link>https://example.org/two</link>"
        "<description><![CDATA[CDATA 摘要正文]]></description></item>"
        "</channel></rss>";
    std::vector<AiWebHit> hits;
    WebParseBingRss(xml, 8, &hits);
    CHECK(hits.size() == 2, "bing_rss 两条结果");
    CHECK(hits.size() >= 1 && hits[0].title == L"第一个结果 & 标题", "bing_rss 实体解码标题");
    CHECK(hits.size() >= 1 && hits[0].url == L"https://example.com/one?x=1&y=2", "bing_rss href 实体还原");
    CHECK(hits.size() >= 1 && Contains(hits[0].snippet, L"<b>") && Contains(hits[0].snippet, L"摘要一"),
          "bing_rss 摘要实体解码");
    CHECK(hits.size() >= 2 && hits[1].title == L"第二个结果 CDATA 标题", "bing_rss CDATA 标题");
    CHECK(hits.size() >= 2 && hits[1].snippet == L"CDATA 摘要正文", "bing_rss CDATA 摘要");
    /* maxN 截断 */
    WebParseBingRss(xml, 1, &hits);
    CHECK(hits.size() == 1, "bing_rss maxN 截断");
    /* 垃圾输入不崩不产条目 */
    WebParseBingRss("this is not xml at all", 8, &hits);
    CHECK(hits.empty(), "bing_rss 非法输入空结果");
}

/* ---------- bing 结果页 HTML (回落; 含 ck/a 跳转包装与嵌套标签) ---------- */
static void TestBingHtml() {
    /* ck/a 载荷 = base64url("https://example.com/doc?id=1") = aHR0cHM6Ly9leGFtcGxlLmNvbS9kb2M_aWQ9MQ */
    std::string html =
        "<html><head><script>var b_algo=1;console.log(\"b_algo noise\");</script></head><body>"
        "<ol id=\"b_results\">"
        "<li class=\"b_algo\"><h2 class=\"\"><a href=\"https://www.bing.com/ck/a?!&amp;p=abc&amp;u=a1aHR0cHM6Ly9leGFtcGxlLmNvbS9kb2M_aWQ9MQ&amp;ntb=1\""
        " h=\"ID=SERP,1\">文档 <em>标题</em></a></h2>"
        "<div class=\"b_caption\"><p class=\"b_lineclamp\">第一段 <b>摘要</b> 内容</p></div></li>"
        "<li class=\"b_algo\"><h2><a href=\"https://example.org/direct?a=1&amp;b=2\">直链结果</a></h2>"
        "<p>直链摘要</p></li>"
        "</ol></body></html>";
    std::vector<AiWebHit> hits;
    WebParseBingHtml(html, 8, &hits);
    CHECK(hits.size() == 2, "bing_html 两条结果");
    CHECK(hits.size() >= 1 && hits[0].url == L"https://example.com/doc?id=1", "bing_html ck/a 跳转还原");
    CHECK(hits.size() >= 1 && hits[0].title == L"文档 标题", "bing_html 嵌套标签标题剥净");
    CHECK(hits.size() >= 1 && hits[0].snippet == L"第一段 摘要 内容", "bing_html 摘要提取");
    CHECK(hits.size() >= 2 && hits[1].url == L"https://example.org/direct?a=1&b=2", "bing_html href 实体还原");
    CHECK(hits.size() >= 2 && hits[1].snippet == L"直链摘要", "bing_html 独立 p 摘要");
    WebParseBingHtml("", 8, &hits);
    CHECK(hits.empty(), "bing_html 空输入空结果");
}

/* ---------- DuckDuckGo Lite (再回落; 含 uddg 包装) ---------- */
static void TestDdg() {
    std::string html =
        "<html><body><table>"
        "<tr><td>1.&nbsp;<a rel=\"nofollow\" href=\"https://example.com/one\" class='result-link'>示例一</a></td></tr>"
        "<tr><td class='result-snippet'>第一条 &amp; 摘要</td></tr>"
        "<tr><td>2.&nbsp;<a rel=\"nofollow\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2Ftwo%3Fq%3D1&amp;rut=abc\" class='result-link'>示例二</a></td></tr>"
        "<tr><td class='result-snippet'>第二条摘要</td></tr>"
        "</table></body></html>";
    std::vector<AiWebHit> hits;
    WebParseDdg(html, 8, &hits);
    CHECK(hits.size() == 2, "ddg 两条结果");
    CHECK(hits.size() >= 1 && hits[0].url == L"https://example.com/one", "ddg 直链");
    CHECK(hits.size() >= 1 && hits[0].snippet == L"第一条 & 摘要", "ddg 摘要实体");
    CHECK(hits.size() >= 2 && hits[1].url == L"https://example.org/two?q=1", "ddg uddg 跳转还原");
    CHECK(hits.size() >= 2 && hits[1].title == L"示例二", "ddg 标题");
}

/* ---------- WebUnwrapResultUrl ---------- */
static void TestUnwrap() {
    /* base64url("https://www.zhihu.com/question/123456") = aHR0cHM6Ly93d3cuemhpaHUuY29tL3F1ZXN0aW9uLzEyMzQ1Ng */
    CHECK(WebUnwrapResultUrl(
        L"https://www.bing.com/ck/a?!&p=abc&u=a1aHR0cHM6Ly93d3cuemhpaHUuY29tL3F1ZXN0aW9uLzEyMzQ1Ng&ntb=1")
        == L"https://www.zhihu.com/question/123456", "unwrap bing/ck base64url");
    CHECK(WebUnwrapResultUrl(L"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2F%E6%B5%8B")
        == L"https://example.org/测", "unwrap ddg 协议相对+百分号编码");
    CHECK(WebUnwrapResultUrl(L"  https://plain.example.com/a  ") == L"https://plain.example.com/a",
          "unwrap 普通链接原样+去空白");
    CHECK(WebUnwrapResultUrl(L"https://x.com/a?u=a1XXXX") != L"", "unwrap 无效载荷不崩原样返回");
}

/* ---------- WebHtmlToText ---------- */
static void TestHtmlToText() {
    std::string html =
        "<html><head><title>页面标题</title><style>p{color:red}</style>"
        "<script>if(a<b){alert(\"x\");}</script></head>"
        "<body><!-- 注释 --><h1>标题一</h1>"
        "<p>第一段&nbsp;文字，含 &lt;标签&gt; 与 &amp; 符号。</p>"
        "<table><tr><td>甲</td><td>乙</td></tr><tr><td>丙</td><td>丁</td></tr></table>"
        "<div>末段<br/>换行</div>"
        "<p>UTF-8 nbsp:\xC2\xA0结尾</p>"
        "</body></html>";
    std::string out;
    WebHtmlToText(html, &out);
    std::wstring w = W8(out.c_str());
    CHECK(Contains(w, L"页面标题"), "html_text title 保留");
    CHECK(!Contains(w, L"alert") && !Contains(w, L"color:red"), "html_text script/style 剔除");
    CHECK(!Contains(w, L"注释"), "html_text 注释剔除");
    CHECK(Contains(w, L"第一段 文字，含 <标签> 与 & 符号。"), "html_text 实体解码+nbsp 折叠");
    CHECK(Contains(w, L"甲\t乙\n丙\t丁"), "html_text 表格结构 (单元格制表/行换行)");
    CHECK(Contains(w, L"末段\n换行"), "html_text br 换行");
    CHECK(Contains(w, L"UTF-8 nbsp: 结尾"), "html_text 原始 nbsp 字节折叠");
    /* 纯脚本页 → 空正文 */
    WebHtmlToText("<html><head><script>var x=1;</script></head><body></body></html>", &out);
    CHECK(out.empty(), "html_text 无正文空输出");
}

/* ---------- AiCapUtf8HeadTail (readCapKB 默认 30 → 头 24576 + 尾 6144) ---------- */
static void TestCap() {
    g_cfg.readCapKB = 30;
    /* 1) 未超上限原样返回 */
    std::string shortTxt = "hello";
    CHECK(AiCapUtf8HeadTail(shortTxt) == shortTxt, "cap 未超上限原样");
    /* 2) ASCII 超上限: 40000 字节 → 头 24576 + 尾 6144, 省略 9280 */
    std::string big(40000, 'a');
    for (size_t i = 0; i < big.size(); i++) big[i] = (char)('a' + i % 26);
    std::string cut = AiCapUtf8HeadTail(big);
    CHECK(cut.size() > 24576 + 6144, "cap 截后长于头尾和");
    CHECK(cut.compare(0, 24576, big, 0, 24576) == 0, "cap 头 24576 字节原样");
    CHECK(cut.compare(cut.size() - 6144, 6144, big, 40000 - 6144, 6144) == 0, "cap 尾 6144 字节原样");
    CHECK(Contains(W8(cut.c_str()), L"中间省略 9280 字节"), "cap 精确省略量标记");
    /* 3) CJK 边界落刀: 24575 个 'a' + 3000 个 '中' (3 字节) — 头 24576 会切进汉字,
     *    Utf8Floor 应回退到 24575; 尾部 27431 起恰在字符边界 (2856/3=952 整除) */
    std::string cjk(24575, 'a');
    for (int i = 0; i < 3000; i++) cjk += "\xE4\xB8\xAD";
    std::string cut2 = AiCapUtf8HeadTail(cjk);
    CHECK(Contains(W8(cut2.c_str()), L"中间省略 2856 字节"), "cap CJK 省略量");
    size_t markerPos = cut2.find("\xE7\x95\xA5\x20");   /* "略 " 的 UTF-8 */
    CHECK(markerPos != std::string::npos && markerPos > 24575, "cap CJK 头部在字符边界落刀");
    std::wstring w2 = W8(cut2.substr(0, 24575).c_str());
    CHECK(w2.size() >= 24570, "cap 头部可无损转宽 (无 U+FFFD 断字)");
}

int main() {
    TestBingRss();
    TestBingHtml();
    TestDdg();
    TestUnwrap();
    TestHtmlToText();
    TestCap();
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails;
}
