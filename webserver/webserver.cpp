// 蜗牛快搜官网静态服务器 —— httplib.h,把本目录 web\
// 子目录以静态站点形式跑起来,供本地预览与局域网访问。
//
// 用法: webserver.exe [端口]    默认 8080,监听 0.0.0.0(局域网内可打开)
// 构建: build.bat(本目录)     产物 webserver.exe 留在本目录
//
// 说明: 启动时把工作目录切到 exe 所在目录,文档根按 web 子目录相对解析,
//       因此从任何位置启动(双击/计划任务)都能找到站点目录。

#include "httplib.h"

// WIN32_LEAN_AND_MEAN 由 build.bat 命令行传入,源码不重复定义
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

static std::wstring ExeDirPath()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring p(buf);
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring(L".") : p.substr(0, pos);
}

int main(int argc, char** argv)
{
    int port = 8080;
    if (argc >= 2) port = atoi(argv[1]);

    WSAData wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // 日志无缓冲: 重定向到管道/文件时也能实时看到
    setvbuf(stdout, NULL, _IONBF, 0);

    // 工作目录 = exe 所在目录,站点 = 本目录 web 子目录
    SetCurrentDirectoryW(ExeDirPath().c_str());

    DWORD attr = GetFileAttributesW(L"web\\index.html");
    if (attr == INVALID_FILE_ATTRIBUTES) {
        printf("ERROR: web\\index.html not found (keep this exe in webserver\\).\n");
        return 1;
    }

    httplib::Server svr;
    // 基目录必须写相对 CWD 的形式: "/web" 这类以 / 开头的窄字符路径在 Windows
    // 上指向"当前盘符根目录"(D:\web), FileStat 找不到目录 → set_mount_point
    // 静默返回 false → 挂载点没注册, 所有请求全落 404
    if (!svr.set_mount_point("/", "web")) {
        printf("ERROR: cannot mount web\\ folder.\n");
        return 1;
    }

    // 预览口径: 不缓存,改完页面刷新即见最新
    svr.set_default_headers({{"Cache-Control", "no-cache"}});

    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.status == httplib::StatusCode::NotFound_404) {
            res.set_content(
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>404</title></head>"
                "<body style=\"margin:0;height:100vh;display:grid;place-items:center;"
                "background:#0b0f14;color:#e7edf5;font-family:'Segoe UI',sans-serif\">"
                "<div style=\"text-align:center\"><div style=\"font-size:72px;font-weight:800;"
                "letter-spacing:-.03em\">404</div>"
                "<div style=\"color:#8d9bad\">页面不存在</div></div></body></html>",
                "text/html; charset=utf-8");
        }
    });

    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        printf("[%02d:%02d:%02d] %s %s -> %d\n",
               st.wHour, st.wMinute, st.wSecond,
               req.method.c_str(), req.path.c_str(), res.status);
    });

    printf("SnailQuickSearch website server (cpp-httplib %s)\n", CPPHTTPLIB_VERSION);
    printf("Document root: web\n");
    printf("Local   : http://localhost:%d/\n", port);

    // 局域网地址提示: 列出本机全部 IPv4
    char host[256] = {};
    if (gethostname(host, sizeof(host)) == 0) {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &list) == 0) {
            for (addrinfo* p = list; p; p = p->ai_next) {
                auto* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                char ip[64] = {};
                if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip)))
                    printf("Network : http://%s:%d/\n", ip, port);
            }
            freeaddrinfo(list);
        }
    }
    printf("Press Ctrl+C to stop.\n\n");

    if (!svr.listen("0.0.0.0", port)) {
        printf("ERROR: cannot listen on port %d (in use or denied?).\n", port);
        return 1;
    }
    return 0;
}
