/*
 * ai_file.cpp — 文件动作与内容抽取: read_file / read_image / file_op 三工具的实体
 * ============================================================================
 * 2026-09-26 新增 (文件助手一等公民动作层, 不再绕 run_command 的 shell 兜底):
 *   read_file  读本地文件给模型: BOM/UTF-16/UTF-8/ANSI(GBK) 编码识别统一转 UTF-8
 *              (AiTextToUtf8, ai_core.cpp); docx/pptx/xlsx 直接解包抽文字 (内置
 *              RFC1951 inflate + ZIP 只读解包, 免第三方库); 其它二进制 (含 PDF)
 *              明确报错不硬猜。2026-09-27 起带 offset/limit 行窗口分页 (dsh read
 *              口径, AiWindowLines 纯函数); 被头尾封顶的内容外溢落盘给路径
 *              (AiSpillText, dsh spill 口径 — 模型用 read_file 分页读回中段)。
 *              AiSpillText/AiWindowLines 亦供 fetch_url / run_command 复用。
 *   read_image 把本地图片注入本请求 (input_image data URL): ≤4MB 且格式已知 = 原样
 *              注入; 否则 WIC 解码缩放 (最长边 ≤2000) 重编码 JPEG。视觉模型直接看图;
 *              需当前档案开启图片输入 (g_cfg.img)。
 *   file_op    复制/移动/重命名/删除(回收站)/新建文件夹; 源支持 ids (FileId) 与
 *              paths 混合寻址。权限闸与询问卡在 ai_agent.cpp WorkerMain (与
 *              run_command 同一条 execGrant/execDeny 挂起裁决通道), 这里只做
 *              Prepare (解析+寻址+摘要+确认文案) 与 Execute (逐项 SHFileOperation)。
 * 本文件全部在 agent 工作线程调用; 除 xjs_db_GetPath/GetFileIdByPath 只读查询外
 * 不碰引擎/宿主/UI。工具结果一律遵守"省略数恒给精确值"口径。
 */
#include "ai_assistant.h"
#include <shellapi.h>    /* SHFileOperationW (删除=回收站 FOF_ALLOWUNDO) */
#include <wincodec.h>    /* WIC (read_image 解码/缩放/重编码) */
#include <wrl/client.h>  /* ComPtr (WIC/Shell COM 接口指针管理) */
#include <exdisp.h>      /* IShellWindows/IWebBrowser2 (list_explorer_windows 枚举) */
#include <functional>

/* ==================== 通用小工具 ==================== */

/* 绝对路径且无通配/非法字符 (防相对路径落错目录、防 SHFileOperation 通配展开) */
bool FilePathOk(const std::wstring& p) {
    if (p.size() < 3 || p.size() > 1024) return false;
    bool abs = (p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) || (p.rfind(L"\\\\", 0) == 0);
    if (!abs) return false;
    for (wchar_t c : p)
        if (c < 0x20 || wcschr(L"<>|\"?*", c)) return false;
    return true;
}

static std::wstring NormSlash(std::wstring p) {   /* 正斜杠归一 (UNC 的 \\ 不受影响) + 去尾分隔符 */
    for (auto& c : p) if (c == L'/') c = L'\\';
    /* 尾随 '\' 剥掉 (盘根 "C:\" 与 UNC 根除外): 带尾分隔符时 FileNameOf 返回空串 =
       copy/move 的"目标已存在"误报 + SHFileOperation 按"作用于目录内容"语义平铺,
       rename 的 from 同失真 (三路共用此归一, 一处收口) */
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

static std::wstring FileNameOf(const std::wstring& p) {   /* 末段名 (无分隔符 = 原样) */
    size_t s = p.find_last_of(L'\\');
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

static std::wstring ExtLowerOf(const std::wstring& p) {
    std::wstring name = FileNameOf(p);
    size_t d = name.find_last_of(L'.');
    if (d == std::wstring::npos) return L"";
    std::wstring e = name.substr(d + 1);
    for (auto& c : e) c = towlower(c);
    return e;
}

/* UTF-8 截断回退到字符边界 (头尾裁切用; 与 ai_agent.cpp 的 PruneUtf8Floor 同款) */
static size_t Utf8Floor(const std::string& s, size_t pos) {
    while (pos > 0 && (unsigned char)s[pos] >= 0x80 && (unsigned char)s[pos] < 0xC0) pos--;
    return pos;
}

static bool AiReadWholeFile(const std::wstring& path, size_t cap, std::string* out, std::wstring* err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError() == ERROR_FILE_NOT_FOUND ? L"文件不存在" : L"无法打开文件 (被占用或无权限)";
        return false;
    }
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(h, &sz) != FALSE && sz.QuadPart >= 0 && (unsigned long long)sz.QuadPart <= cap;
    if (ok) {
        out->resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        ok = sz.QuadPart == 0 ||
             (ReadFile(h, &(*out)[0], (DWORD)out->size(), &rd, NULL) && rd == (DWORD)sz.QuadPart);
        if (ok) out->resize(rd);
        if (!ok) *err = L"读取失败 (磁盘错误或文件被截断)";
    } else {
        wchar_t mb[96];
        swprintf(mb, 96, L"文件超过 %.0f MB 读取上限", cap / 1048576.0);
        *err = mb;
    }
    CloseHandle(h);
    return ok;
}

/* 工具参数 ids[] + paths[] + path → 绝对路径清单 (反斜杠归一/去重; id 经 xjs_db_GetPath)。
 * 引擎查询指针为线程本地缓存, 必须立即拷贝 (与 ai_agent.cpp 同口径)。 */
std::wstring FileResolveTargets(const Jv& v, std::vector<std::wstring>* out, int cap) {
    const Jv* ids = v.Get(L"ids");
    const Jv* pths = v.Get(L"paths");
    const Jv* p1 = v.Get(L"path");
    size_t want = (ids && ids->t == 4 ? ids->arr.size() : 0) + (pths && pths->t == 4 ? pths->arr.size() : 0) +
                  (p1 && p1->t == 3 && !p1->str.empty() ? 1 : 0);
    if (want == 0) return L"";   /* 调用方判空给"缺参数" */
    if ((int)want > cap) return L"一次指定的条目过多 (上限 " + std::to_wstring(cap) + L")";
    if (ids && ids->t == 4 && !ids->arr.empty()) {
        xjs_engine* eng = xjs_GetDefaultEngine();
        if (!eng) return L"引擎不可用, 无法按 FileId 解析路径";
        for (auto& e : ids->arr) {
            /* double→int 转换的 UB 防线: NaN/越界/非整数全部拒绝 (曾只挡 <1, 1e18 直转 int) */
            if (e.t != 2 || !(e.num >= 1.0) || e.num > 2147483647.0 ||
                e.num != (double)(long long)e.num)
                return L"ids 必须是 1..2147483647 的整数 FileId 数组";
            const char* p = xjs_db_GetPath(eng, (int)e.num);
            if (!p || !*p) return L"FileId 不在索引中: " + std::to_wstring((long long)e.num);
            out->push_back(NormSlash(W8(p)));
        }
    }
    if (pths && pths->t == 4 && !pths->arr.empty()) {
        for (auto& e : pths->arr) {
            if (e.t != 3 || e.str.empty()) return L"paths 必须是绝对路径字符串数组";
            std::wstring p = NormSlash(e.str);
            if (!FilePathOk(p)) return L"路径非法 (须绝对路径且不含通配符): " + e.str;
            out->push_back(std::move(p));
        }
    }
    if (p1 && p1->t == 3 && !p1->str.empty()) {
        std::wstring p = NormSlash(p1->str);
        if (!FilePathOk(p)) return L"路径非法 (须绝对路径且不含通配符): " + p1->str;
        out->push_back(std::move(p));
    }
    /* 去重 (Windows 路径不区分大小写; 保首现序) */
    std::vector<std::wstring> uniq;
    std::vector<std::wstring> keys;
    for (auto& p : *out) {
        std::wstring k = p;
        for (auto& c : k) c = towlower(c);
        if (std::find(keys.begin(), keys.end(), k) != keys.end()) continue;
        keys.push_back(k);
        uniq.push_back(std::move(p));
    }
    *out = std::move(uniq);
    return L"";
}

/* ==================== inflate (RFC1951, 免第三方) ==================== */

namespace {

/* 位读取器: 按需逐字节喂入 */
struct InfRd {
    const unsigned char* d = NULL;
    size_t n = 0, pos = 0;
    unsigned buf = 0;
    int cnt = 0;
    int bits(int need) {
        if (need == 0) return 0;
        while (cnt < need) {
            if (pos >= n) return -1;
            buf |= (unsigned)d[pos++] << cnt;
            cnt += 8;
        }
        int v = (int)(buf & ((1u << need) - 1));
        buf >>= need;
        cnt -= need;
        return v;
    }
    void alignByte() { buf = 0; cnt = 0; }   /* stored 块前丢弃未满一字节的余位 */
};

struct InfHuf { short count[16]; short sym[288]; };

/* 规范霍夫曼建表; 返回值 <0 = 超订 (坏流), 否则为剩余码空间 (未满表允许, 距离树单码合法) */
static int InfHufBuild(InfHuf& h, const short* len, int n) {
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    if (h.count[0] == n) return 0;   /* 全零 = 空表 */
    int left = 1;
    for (int l = 1; l <= 15; l++) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return -1;
    }
    short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h.count[l];
    for (int i = 0; i < n; i++)
        if (len[i]) h.sym[offs[len[i]]++] = (short)i;
    return left;
}

static int InfDec(InfRd& in, const InfHuf& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = in.bits(1);
        if (b < 0) return -1;
        code |= b;
        int cnt = h.count[len];
        if (code - cnt < first) return h.sym[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const short INF_LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const short INF_LEXT[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const short INF_DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const short INF_DEXT[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

/* 单块解码; lc/dc = NULL 表示固定表 (RFC 1951 §3.2.6 显式码值, 按长度分组连续:
 * len7 0..23 → 256..279; len8 48..191 → 字面量 0..143; len8 192..199 → 长度 280..287;
 * len9 400..511 → 字面量 144..255 — Kraft 恰 = 1, 各长度区间互不为前缀)。
 * 0=块完 -1=坏流/截断 -2=输出超上限 */
static int InfFixedLit(InfRd& in) {
    int code = 0;
    for (int len = 1; len <= 9; len++) {
        int b = in.bits(1);
        if (b < 0) return -1;
        code = (code << 1) | b;   /* 霍夫曼码 MSB 先行 */
        if (len == 7 && code <= 23) return 256 + code;
        if (len == 8) {
            if (code >= 48 && code <= 191) return code - 48;
            if (code >= 192 && code <= 199) return 280 + (code - 192);
        }
        if (len == 9 && code >= 400) return 144 + (code - 400);
    }
    return -1;
}
static int InfFixedDist(InfRd& in) {  /* 固定距离表: 5 位码 0..29 (30/31 非法) */
    int code = 0;
    for (int len = 0; len < 5; len++) {
        int b = in.bits(1);
        if (b < 0) return -1;
        code = (code << 1) | b;
    }
    return code < 30 ? code : -1;
}

static int InfBlock(InfRd& in, const InfHuf* lc, const InfHuf* dc, std::string* out, size_t maxOut) {
    for (;;) {
        int sym = lc ? InfDec(in, *lc) : InfFixedLit(in);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (out->size() >= maxOut) return -2;
            out->push_back((char)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int eb = in.bits(INF_LEXT[sym]);
            if (eb < 0) return -1;
            int len = INF_LBASE[sym] + eb;
            int ds = dc ? InfDec(in, *dc) : InfFixedDist(in);
            if (ds < 0 || ds >= 30) return -1;
            int edb = in.bits(INF_DEXT[ds]);
            if (edb < 0) return -1;
            int dist = INF_DBASE[ds] + edb;
            if ((size_t)dist > out->size()) return -1;
            if (out->size() + (size_t)len > maxOut) return -2;
            size_t from = out->size() - (size_t)dist;
            for (int i = 0; i < len; i++) out->push_back((*out)[from + i]);
        }
    }
}

const short INF_CLEN_ORD[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

/* 裸 deflate 流 (无容器头尾); 返回 0=成功 */
static int inflate_raw(const unsigned char* d, size_t n, std::string* out, size_t maxOut) {
    InfRd in;
    in.d = d;
    in.n = n;
    for (;;) {
        int last = in.bits(1);
        if (last < 0) return -1;
        int type = in.bits(2);
        if (type < 0) return -1;
        if (type == 0) {   /* stored: 对齐字节后 LEN+NLEN+原文 */
            in.alignByte();
            if (in.pos + 4 > in.n) return -1;
            unsigned len = (unsigned)in.d[in.pos] | ((unsigned)in.d[in.pos + 1] << 8);
            in.pos += 4;   /* NLEN 是 LEN 取反, 读取即跳过 */
            if (len > in.n - in.pos) return -1;
            if (out->size() + len > maxOut) return -2;
            out->append((const char*)in.d + in.pos, len);
            in.pos += len;
        } else if (type == 1 || type == 2) {
            if (type == 1) {
                int rc = InfBlock(in, NULL, NULL, out, maxOut);   /* 固定表 (显式码值) */
                if (rc != 0) return rc;
            } else {
                int hlit = in.bits(5);
                int hdist = in.bits(5);
                int hclen = in.bits(4);
                if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
                int nlit = hlit + 257, ndist = hdist + 1, ncode = hclen + 4;
                /* RFC1951: HDIST 5 位 → 距离码数 1~32 (码 30/31 允许存在但长度恒 0, 不许
                 * 据此拒流 — 主流压缩实现会发 ndist=31 的合法流); HLIT 上限 286。 */
                if (nlit > 286 || ndist > 32) return -1;
                short cl[19] = {};
                for (int i = 0; i < ncode; i++) {
                    int v = in.bits(3);
                    if (v < 0) return -1;
                    cl[INF_CLEN_ORD[i]] = (short)v;
                }
                InfHuf clh;
                if (InfHufBuild(clh, cl, 19) < 0) return -1;
                short lens[286 + 32];
                for (int i = 0; i < nlit + ndist; i++) lens[i] = 0;
                int i = 0;
                while (i < nlit + ndist) {
                    int sym = InfDec(in, clh);
                    if (sym < 0) return -1;
                    if (sym < 16) {
                        lens[i++] = (short)sym;
                        continue;
                    }
                    int rep = 0, val = 0;
                    if (sym == 16) {
                        if (i == 0) return -1;
                        val = lens[i - 1];
                        int b = in.bits(2);
                        if (b < 0) return -1;
                        rep = 3 + b;
                    } else if (sym == 17) {
                        int b = in.bits(3);
                        if (b < 0) return -1;
                        rep = 3 + b;
                    } else if (sym == 18) {
                        int b = in.bits(7);
                        if (b < 0) return -1;
                        rep = 11 + b;
                    } else {
                        return -1;
                    }
                    while (rep-- > 0 && i < nlit + ndist) lens[i++] = (short)val;   /* 越界重复容忍 (健壮性优先) */
                }
                if (lens[256] == 0) return -1;   /* 块结束码必须有长度 */
                InfHuf lc, dc;
                if (InfHufBuild(lc, lens, nlit) < 0) return -1;
                if (InfHufBuild(dc, lens + nlit, ndist) < 0) return -1;
                int rc = InfBlock(in, &lc, &dc, out, maxOut);
                if (rc != 0) return rc;
            }
        } else {
            return -1;
        }
        if (last) break;
    }
    return 0;
}

/* ==================== ZIP 只读解包 (EOCD + 中央目录; stored/deflate) ==================== */

struct AiZipEnt {
    std::string name;
    unsigned short method = 0;
    unsigned long long csize = 0, usize = 0;
    unsigned long long lho = 0;   /* local header 偏移 */
};

static unsigned short ZipRd16(const std::string& d, size_t at) {
    return (unsigned short)(unsigned char)d[at] | ((unsigned short)(unsigned char)d[at + 1] << 8);
}
static unsigned long ZipRd32(const std::string& d, size_t at) {
    return (unsigned long)(unsigned char)d[at] | ((unsigned long)(unsigned char)d[at + 1] << 8) |
           ((unsigned long)(unsigned char)d[at + 2] << 16) | ((unsigned long)(unsigned char)d[at + 3] << 24);
}

static bool AiZipOpen(const std::string& d, std::vector<AiZipEnt>* ents, std::wstring* err) {
    if (d.size() < 22) { *err = L"不是有效的 ZIP 包 (文件过小)"; return false; }
    /* EOCD 签名从尾部向前找 (兼容包前置注释; 压缩数据里可能出现同签名, 尾部优先即惯例) */
    size_t lo = d.size() >= 66000 ? d.size() - 66000 : 0;
    size_t eocd = std::string::npos;
    for (size_t i = d.size() - 22 + 1; i-- > lo;) {
        if (d[i] == 'P' && memcmp(d.data() + i, "PK\x05\x06", 4) == 0) { eocd = i; break; }
    }
    if (eocd == std::string::npos) { *err = L"不是有效的 ZIP 包 (找不到目录)"; return false; }
    unsigned short cnt = ZipRd16(d, eocd + 10);
    unsigned long cdoff = ZipRd32(d, eocd + 16);
    if (cnt == 0xFFFF || cdoff == 0xFFFFFFFF) { *err = L"ZIP64 格式暂不支持"; return false; }
    size_t p = cdoff;
    for (unsigned short i = 0; i < cnt; i++) {
        if (p + 46 > d.size() || memcmp(d.data() + p, "PK\x01\x02", 4) != 0) {
            *err = L"ZIP 中央目录损坏";
            return false;
        }
        unsigned long csize = ZipRd32(d, p + 20);
        unsigned long usize = ZipRd32(d, p + 24);
        unsigned long lho = ZipRd32(d, p + 42);
        unsigned short nlen = ZipRd16(d, p + 28), elen = ZipRd16(d, p + 30), clen = ZipRd16(d, p + 32);
        if (csize == 0xFFFFFFFF || usize == 0xFFFFFFFF || lho == 0xFFFFFFFF) {
            *err = L"ZIP64 条目暂不支持";
            return false;
        }
        if (p + 46 + (size_t)nlen > d.size()) { *err = L"ZIP 中央目录损坏"; return false; }
        AiZipEnt e;
        e.name = d.substr(p + 46, nlen);
        e.method = ZipRd16(d, p + 10);
        e.csize = csize;
        e.usize = usize;
        e.lho = lho;
        ents->push_back(std::move(e));
        p += 46 + (size_t)nlen + elen + clen;
    }
    return true;
}

static bool AiZipRead(const std::string& d, const AiZipEnt& e, std::string* out, size_t maxOut, std::wstring* err) {
    if (e.method != 0 && e.method != 8) { *err = L"不支持的 ZIP 压缩方法 (条目 " + W8(e.name.c_str()) + L")"; return false; }
    if (e.lho + 30 > d.size() || memcmp(d.data() + e.lho, "PK\x03\x04", 4) != 0) {
        *err = L"ZIP 条目头损坏";
        return false;
    }
    size_t data = e.lho + 30 + ZipRd16(d, e.lho + 26) + ZipRd16(d, e.lho + 28);
    if (data + e.csize > d.size()) { *err = L"ZIP 条目数据越界"; return false; }
    if (e.usize > maxOut) { *err = L"条目解压后超过大小上限 (条目 " + W8(e.name.c_str()) + L")"; return false; }
    if (e.method == 0) {
        /* stored 不经 inflate 的逐字面封顶: 畸形包可让 usize 撒谎而 csize 高达读取上限,
         * append 按实际 csize 展开会绕过 maxOut — 与 deflate 路径同一封顶口径 */
        if (e.csize > maxOut) { *err = L"条目解压后超过大小上限 (条目 " + W8(e.name.c_str()) + L")"; return false; }
        out->append(d, data, (size_t)e.csize);
        return true;
    }
    int rc = inflate_raw((const unsigned char*)d.data() + data, (size_t)e.csize, out, maxOut);
    if (rc == -2) { *err = L"条目解压后超过大小上限"; return false; }
    if (rc != 0) { *err = L"条目解压失败 (流损坏)"; return false; }
    return true;
}

/* ==================== XML 文本抽取 (OOXML 文字层; 手写扫描, 结构固定) ==================== */

static void XmlEntDec(const char* s, size_t n, std::string* out) {
    size_t i = 0;
    while (i < n) {
        if (s[i] != '&') { *out += s[i++]; continue; }
        size_t sc = i + 1;
        while (sc < n && s[sc] != ';' && s[sc] != '&' && sc - i < 12) sc++;
        if (sc >= n || s[sc] != ';') { *out += s[i++]; continue; }
        std::string ent(s + i + 1, sc - i - 1);
        if (ent == "amp") *out += '&';
        else if (ent == "lt") *out += '<';
        else if (ent == "gt") *out += '>';
        else if (ent == "quot") *out += '"';
        else if (ent == "apos") *out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            int cp = 0;
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                cp = (int)strtoul(ent.c_str() + 2, NULL, 16);
            else
                cp = atoi(ent.c_str() + 1);
            if (cp > 0 && cp < 0x110000) {   /* 编码点 → UTF-8 */
                if (cp < 0x80) *out += (char)cp;
                else if (cp < 0x800) {
                    *out += (char)(0xC0 | (cp >> 6));
                    *out += (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    *out += (char)(0xE0 | (cp >> 12));
                    *out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    *out += (char)(0x80 | (cp & 0x3F));
                } else {
                    *out += (char)(0xF0 | (cp >> 18));
                    *out += (char)(0x80 | ((cp >> 12) & 0x3F));
                    *out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    *out += (char)(0x80 | (cp & 0x3F));
                }
            }
        } else {
            out->append(s + i, sc - i + 1);   /* 未识别实体原样保留 */
        }
        i = sc + 1;
    }
}

/* 找开标签 <tag ...> 与其内容区间; 自闭合返回 true 且内容为空; afterTag = '>' 下一位 */
static bool XmlOpenTag(const std::string& x, size_t from, const char* tag,
                       size_t* contentPos, size_t* contentEnd, size_t* afterTag) {
    size_t tlen = strlen(tag);
    size_t p = from;
    for (;;) {
        p = x.find('<', p);
        if (p == std::string::npos) return false;
        if (x.compare(p + 1, tlen, tag) != 0) { p++; continue; }
        char nx = x[p + 1 + tlen];
        if (nx != '>' && nx != ' ' && nx != '/' && nx != '\t' && nx != '\r' && nx != '\n') { p++; continue; }
        size_t gt = x.find('>', p);
        if (gt == std::string::npos) return false;
        if (x[gt - 1] == '/') {   /* 自闭合 */
            *contentPos = *contentEnd = gt + 1;
            *afterTag = gt + 1;
            return true;
        }
        std::string close = std::string("</") + tag + ">";
        size_t end = x.find(close, gt);
        if (end == std::string::npos) return false;
        *contentPos = gt + 1;
        *contentEnd = end;
        *afterTag = end + close.size();
        return true;
    }
}

/* docx 正文: 段落=换行, w:t=文字, w:tab=制表, w:br=换行 (前缀重叠的标签用"标签名后
 * 一字符"区分: w:t 不吃掉 w:tab, w:p 不吃掉 w:pPr/w:pgSz) */
static void DocxText(const std::string& xml, std::string* out, size_t cap) {
    auto nx = [&](size_t lt, int off) -> char {
        return (lt + off < xml.size()) ? xml[lt + off] : 0;
    };
    size_t i = 0;
    while (i < xml.size()) {
        size_t lt = xml.find('<', i);
        if (lt == std::string::npos) break;
        if (out->size() >= cap) break;
        char c4 = nx(lt, 4), c5 = nx(lt, 5), c6 = nx(lt, 6);
        bool tag = false;
        if (xml.compare(lt, 4, "<w:t") == 0 && (c4 == '>' || c4 == ' ')) {   /* 文字 (XmlOpenTag 内部同样验) */
            size_t cp, ce, af;
            if (!XmlOpenTag(xml, lt, "w:t", &cp, &ce, &af)) break;
            XmlEntDec(xml.data() + cp, ce - cp, out);
            i = af;
            tag = true;
        } else if (xml.compare(lt, 6, "<w:tab") == 0 && (c6 == '>' || c6 == ' ' || c6 == '/')) {
            *out += '\t';
            i = lt + 6;
            tag = true;
        } else if (xml.compare(lt, 5, "<w:br") == 0 && (c5 == '>' || c5 == ' ' || c5 == '/')) {
            *out += '\n';
            i = lt + 5;
            tag = true;
        } else if (xml.compare(lt, 4, "<w:p") == 0 && (c4 == '>' || c4 == ' ' || c4 == '/')) {
            *out += '\n';
            i = lt + 4;
            tag = true;
        }
        if (!tag) i = lt + 1;
    }
}

/* pptx 单页: 所有 a:t 文字直接相连 (段落感由原文空格/标点保留) */
static void PptxSlideText(const std::string& xml, std::string* out, size_t cap) {
    size_t pos = 0;
    for (;;) {
        size_t cp, ce, af;
        if (!XmlOpenTag(xml, pos, "a:t", &cp, &ce, &af)) break;
        XmlEntDec(xml.data() + cp, ce - cp, out);
        if (out->size() >= cap) break;
        pos = af;
    }
}

/* xlsx sharedStrings: 每个 si 的全部 t 文本相连 = 一个共享串 */
static void XlsxSharedStrings(const std::string& xml, std::vector<std::string>* out, size_t cap) {
    size_t pos = 0;
    for (;;) {
        size_t cp, ce, af;
        if (!XmlOpenTag(xml, pos, "si", &cp, &ce, &af)) break;
        std::string item;
        size_t ip = cp;
        for (;;) {
            size_t tp, te, taf;
            if (!XmlOpenTag(xml, ip, "t", &tp, &te, &taf) || taf > ce) break;
            XmlEntDec(xml.data() + tp, te - tp, &item);
            ip = taf;
        }
        out->push_back(std::move(item));
        if (out->size() > 200000) break;   /* 防御: 共享串过多直接停 (正常远达不到) */
        pos = af;
        if (pos > cap) break;
    }
}

/* xlsx 单表: 行=换行, 单元格=制表符分隔; t="s" 走共享串, 其余取 v/内联 is 文本 */
static void XlsxSheetText(const std::string& xml, const std::vector<std::string>& shared, std::string* out, size_t cap) {
    size_t pos = 0;
    for (;;) {
        size_t rp, re, raf;
        if (!XmlOpenTag(xml, pos, "row", &rp, &re, &raf)) break;
        size_t cp = rp;
        bool any = false;
        for (;;) {
            size_t c2p, c2e, c2af;
            if (!XmlOpenTag(xml, cp, "c", &c2p, &c2e, &c2af) || c2p > re) break;
            /* 开标签文本里取 t=".." (共享串判定): 开标签 = 内容区起点向前回溯到 '<' */
            std::string openTag;
            {
                size_t lt = xml.rfind('<', c2p - 1);
                if (lt != std::string::npos) openTag = xml.substr(lt, c2p - lt);
            }
            bool isShared = openTag.find("t=\"s\"") != std::string::npos;
            std::string cell;
            size_t vp = c2p;
            for (;;) {
                size_t vp2, ve2, vaf2;
                if (!XmlOpenTag(xml, vp, "v", &vp2, &ve2, &vaf2) || vp2 > c2e) break;
                XmlEntDec(xml.data() + vp2, ve2 - vp2, &cell);
                vp = vaf2;
            }
            if (cell.empty()) {   /* 内联串 <is><t>..</t></is> */
                size_t ip, ie, iaf;
                if (XmlOpenTag(xml, c2p, "is", &ip, &ie, &iaf) && ip <= c2e) {
                    size_t tp2 = ip;
                    for (;;) {
                        size_t tp3, te3, taf3;
                        if (!XmlOpenTag(xml, tp2, "t", &tp3, &te3, &taf3) || tp3 > ie) break;
                        XmlEntDec(xml.data() + tp3, te3 - tp3, &cell);
                        tp2 = taf3;
                    }
                }
            }
            if (isShared) {   /* 共享串索引 → 原文 */
                int idx = atoi(cell.c_str());
                cell = (idx >= 0 && idx < (int)shared.size()) ? shared[idx] : std::string();
            }
            if (!cell.empty()) {
                if (out->size() + cell.size() >= cap) { any = true; break; }
                if (any) *out += '\t';
                *out += cell;
                any = true;
            }
            cp = c2af;
        }
        if (out->size() >= cap) break;
        *out += '\r';
        *out += '\n';
        pos = raf;
    }
}

/* Office (OOXML) 文本抽取入口: 按扩展名分包; out 封顶 512KB (命中即提前收卷,
 * truncOut 告知调用方 — 之后仍有精确头尾裁剪口径) */
static bool AiOfficeExtract(const std::wstring& path, const std::wstring& ext,
                            std::string* out, std::wstring* kindName, bool* truncOut, std::wstring* err) {
    const size_t cap = 512 * 1024;
    *truncOut = false;
    std::string raw;
    if (!AiReadWholeFile(path, 64ull * 1024 * 1024, &raw, err)) return false;
    std::vector<AiZipEnt> ents;
    if (!AiZipOpen(raw, &ents, err)) {
        if (raw.size() >= 4 && (unsigned char)raw[0] == 0xD0 && (unsigned char)raw[1] == 0xCF &&
            (unsigned char)raw[2] == 0x11 && (unsigned char)raw[3] == 0xE0)
            *err = L"旧版二进制 Office 格式暂不支持 (请另存为对应的新格式后重试)";
        return false;
    }
    /* 名字查条目 (Office 包内路径恒为小写 ASCII) */
    auto readEnt = [&](const char* name, std::string* body) -> bool {
        for (auto& e : ents) {
            if (e.name.size() != strlen(name) || memcmp(e.name.data(), name, e.name.size()) != 0) continue;
            return AiZipRead(raw, e, body, 48ull * 1024 * 1024, err);
        }
        *err = L"包内缺少 " + W8(name) + L" (文件可能损坏)";
        return false;
    };
    auto numSortRead = [&](const char* prefix, const char* suffix,
                           const std::function<void(const std::string&, int)>& fn) -> bool {
        std::vector<std::pair<int, const AiZipEnt*>> list;
        size_t pl = strlen(prefix), sl = strlen(suffix);
        for (auto& e : ents) {
            if (e.name.size() <= pl + sl) continue;
            if (memcmp(e.name.data(), prefix, pl) != 0) continue;
            if (memcmp(e.name.data() + e.name.size() - sl, suffix, sl) != 0) continue;
            std::string mid = e.name.substr(pl, e.name.size() - pl - sl);
            if (mid.empty() || mid.find_first_not_of("0123456789") != std::string::npos) continue;
            list.push_back({ atoi(mid.c_str()), &e });
        }
        std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& it : list) {
            std::string body;
            if (!AiZipRead(raw, *it.second, &body, 48ull * 1024 * 1024, err)) return false;
            fn(body, it.first);
            if (out->size() >= cap) return true;
        }
        return true;
    };
    auto done = [&](const wchar_t* name) {
        *truncOut = out->size() >= cap;
        *kindName = name;
        return true;
    };
    if (ext == L"docx") {
        std::string xml;
        if (!readEnt("word/document.xml", &xml)) return false;
        DocxText(xml, out, cap);
        return done(L"Word 文档 (docx)");
    }
    if (ext == L"pptx") {
        if (!numSortRead("ppt/slides/slide", ".xml", [&](const std::string& body, int no) {
                *out += "\r\n\r\n—— 第 " + std::to_string(no) + " 页 ——\r\n\r\n";
                PptxSlideText(body, out, cap);
            }))
            return false;
        return done(L"演示文稿 (pptx)");
    }
    if (ext == L"xlsx") {
        std::vector<std::string> shared;
        {
            for (auto& e : ents) {
                if (e.name == "xl/sharedStrings.xml") {
                    std::string xml;
                    if (!AiZipRead(raw, e, &xml, 48ull * 1024 * 1024, err)) return false;
                    XlsxSharedStrings(xml, &shared, cap);
                    break;
                }
            }
        }
        int sheetNo = 0;
        if (!numSortRead("xl/worksheets/sheet", ".xml", [&](const std::string& body, int) {
                sheetNo++;
                *out += "\r\n\r\n[工作表 " + std::to_string(sheetNo) + "]\r\n";
                XlsxSheetText(body, shared, out, cap);
            }))
            return false;
        if (sheetNo == 0) { *err = L"包内没有工作表"; return false; }
        return done(L"电子表格 (xlsx; 每行=一行, 单元格间制表符; 日期为序列数)");
    }
    *err = L"不支持的格式";
    return false;
}

} /* namespace (文件局部: inflate/zip/xml 抽取) */

/* ==================== file_op: Prepare / Execute ==================== */

/* 参数 → op (解析+寻址+摘要+确认文案); 返回错误描述 (空=成功) */
std::wstring FileOpPrepare(const Jv& v, AiFileOp* op) {
    std::wstring act = TrimW(v.S(L"action"));
    if (act == L"copy") op->action = 0;
    else if (act == L"move") op->action = 1;
    else if (act == L"rename") op->action = 2;
    else if (act == L"delete") op->action = 3;
    else if (act == L"mkdir") op->action = 4;
    else return L"action 只接受 copy | move | rename | delete | mkdir";
    const Jv* ov = v.Get(L"overwrite");
    op->overwrite = ov && ((ov->t == 1 && ov->b) || (ov->t == 2 && ov->num != 0));
    const Jv* pmv = v.Get(L"permanent");
    op->permanent = pmv && ((pmv->t == 1 && pmv->b) || (pmv->t == 2 && pmv->num != 0));
    auto cutList = [](std::wstring* s, const std::vector<std::pair<std::wstring, std::wstring>>& pairs) {
        for (size_t i = 0; i < pairs.size() && i < 5; i++)
            *s += L"\n· " + FileNameOf(pairs[i].first) + L"  →  " + FileNameOf(pairs[i].second);
        if (pairs.size() > 5) *s += L"\n… 等共 " + std::to_wstring(pairs.size()) + L" 项 (完整清单见卡片样本)";
    };

    if (op->action == 4) {   /* mkdir: target = 要建的目录 */
        op->target = NormSlash(TrimW(v.S(L"target")));
        while (op->target.size() > 3 && op->target.back() == L'\\') op->target.pop_back();
        if (!FilePathOk(op->target)) return L"target 必须是要创建目录的合法绝对路径";
        op->summary = L"新建文件夹 " + op->target;
        op->confirm = L"AI 请求新建文件夹:\n" + op->target;
        return L"";
    }

    if (op->action == 2) {   /* rename: renames = [{from, to}] */
        const Jv* rv = v.Get(L"renames");
        if (!rv || rv->t != 4 || rv->arr.empty()) return L"rename 需要 renames 数组 [{from, to}]";
        if ((int)rv->arr.size() > 128) return L"一次最多 128 项";
        for (auto& r : rv->arr) {
            /* from = 必填的"改名前"完整路径**文本** (模型按 搜索结果旧文件名 + 目录 拼出)。
             * 不再接受用 FileId 反查充当 from: 反查的是索引**当前**名, 文件已被改过名时
             * 拿到的是最新名, 更改记录的"改名前"必然失真 (前后名一样)。id 在 rename 里
             * 不参与寻址与记录。 */
            std::wstring from = NormSlash(TrimW(r.S(L"from")));
            if (!FilePathOk(from))
                return L"renames[].from 缺失或不是合法绝对路径 — from 必须是改名前的完整路径文本"
                       L"(目录 + 搜索结果里的旧文件名); FileId 反查到的是最新名, 不能当改名前记录";
            std::wstring to = TrimW(r.S(L"to"));
            if (to.empty()) return L"renames[].to 缺失 (新文件名或新完整路径)";
            if (to.find(L'\\') == std::wstring::npos && to.find(L'/') == std::wstring::npos) {
                /* 纯文件名: 留在原目录改名 */
                if (to == L"." || to == L"..") return L"renames[].to 非法: " + to;
                for (wchar_t c : to)
                    if (c < 0x20 || wcschr(L"<>|\"?*:", c)) return L"renames[].to 含非法字符: " + to;
                to = from.substr(0, from.find_last_of(L'\\') + 1) + to;
            } else {
                to = NormSlash(to);
                if (!FilePathOk(to)) return L"renames[].to 不是合法绝对路径: " + to;
            }
            op->renames.push_back({ from, to });
        }
        op->summary = L"重命名 " + std::to_wstring(op->renames.size()) + L" 项";
        op->confirm = L"AI 请求重命名 " + std::to_wstring(op->renames.size()) + L" 个文件/文件夹:";
        cutList(&op->confirm, op->renames);
        op->confirm += op->overwrite ? L"\n目标已存在时将被覆盖" : L"\n目标已存在时跳过并报告";
        return L"";
    }

    /* copy/move/delete: 源 = ids/paths */
    std::vector<std::wstring> items;
    std::wstring err = FileResolveTargets(v, &items, 128);
    if (!err.empty()) return err;
    if (items.empty()) return L"缺少 paths / ids 参数 (源清单)";
    op->items = items;

    if (op->action == 0 || op->action == 1) {   /* copy/move: target = 目标目录 */
        op->target = NormSlash(TrimW(v.S(L"target")));
        while (op->target.size() > 3 && op->target.back() == L'\\') op->target.pop_back();
        if (!FilePathOk(op->target)) return L"target 必须是目标目录的合法绝对路径";
        DWORD at = GetFileAttributesW(op->target.c_str());
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY))
            return L"目标目录不存在: " + op->target;
        op->summary = (op->action == 0 ? L"复制 " : L"移动 ") + std::to_wstring(op->items.size()) +
                      L" 项 → " + op->target + (op->overwrite ? L" (覆盖已存在)" : L"");
        op->confirm = L"AI 请求" + (op->action == 0 ? std::wstring(L"复制 ") : std::wstring(L"移动 ")) +
                      std::to_wstring(op->items.size()) + L" 个文件/文件夹到:\n" + op->target;
        size_t show = op->items.size() < 5 ? op->items.size() : 5;
        for (size_t i = 0; i < show; i++) op->confirm += L"\n· " + op->items[i];
        if (op->items.size() > 5) op->confirm += L"\n… 等共 " + std::to_wstring(op->items.size()) + L" 项";
        op->confirm += op->overwrite ? L"\n目标已存在时将被覆盖" : L"\n目标已存在时跳过并报告";
        return L"";
    }

    /* delete */
    bool unc = false;   /* 网络共享路径没有回收站: FOF_ALLOWUNDO 对 UNC 无效 = 实为永久删除,
                           确认卡必须如实说 (曾一律承诺"移入回收站") */
    for (auto& ip : op->items)
        if (ip.size() >= 2 && ip[0] == L'\\' && ip[1] == L'\\') { unc = true; break; }
    op->summary = (op->permanent ? L"彻底删除 " : L"删除 ") + std::to_wstring(op->items.size()) +
                  L" 项" + (op->permanent ? L" ⚠"
                                          : (unc ? L" ⚠ (网络路径无法进回收站, 将直接删除)" : L" (移入回收站)"));
    op->risk = op->permanent ? L"⚠ 彻底删除: 不经过回收站, 无法还原"
                             : (unc ? L"⚠ 网络共享路径没有回收站, 本次删除实际为永久删除, 无法还原" : L"");
    op->confirm = L"AI 请求";
    if (op->permanent) op->confirm += L"彻底删除 (不可还原)";
    else if (unc) op->confirm += L"删除 (网络路径将直接删除, 不经回收站)";
    else op->confirm += L"删除到回收站";
    op->confirm += L" " + std::to_wstring(op->items.size()) + L" 项:";
    size_t show = op->items.size() < 5 ? op->items.size() : 5;
    for (size_t i = 0; i < show; i++) op->confirm += L"\n· " + op->items[i];
    if (op->items.size() > 5) op->confirm += L"\n… 等共 " + std::to_wstring(op->items.size()) + L" 项";
    return L"";
}

/* 单项 SHFileOperation (静默+不弹错误框; 确认已由权限档/询问卡完成) */
static std::wstring ShOpOne(UINT func, const std::wstring& from, const std::wstring& to, bool allowUndo) {
    std::wstring f = from;
    f.push_back(L'\0');
    SHFILEOPSTRUCTW fo;
    ZeroMemory(&fo, sizeof(fo));
    fo.hwnd = NULL;
    fo.wFunc = func;
    fo.pFrom = f.c_str();
    std::wstring t;
    if (!to.empty()) {
        t = to;
        t.push_back(L'\0');
        fo.pTo = t.c_str();
    }
    fo.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
    if (allowUndo) fo.fFlags |= FOF_ALLOWUNDO;
    int rc = SHFileOperationW(&fo);
    if (rc == 0 && !fo.fAnyOperationsAborted) return L"";
    if (fo.fAnyOperationsAborted) return L"操作被系统中止";
    switch (rc) {
        case 2:  return L"路径未找到";
        case 5:  return L"拒绝访问 (被占用或无权限)";
        case 32: return L"文件被其它程序占用";
        case 124:return L"路径不存在或层级过深";
        default:
            wchar_t mb[64];
            swprintf(mb, 64, L"系统拒绝 (代码 %d)", rc);
            return mb;
    }
}

static std::wstring MkdirDeep(const std::wstring& path) {
    /* UNC (\\server\share\...) 不能逐级建: "\\server\" 不是可创建目录, CreateDirectoryW
       报 ERROR_INVALID_NAME — 服务器/共享必须已存在, 直接对完整路径建一次 */
    if (path.rfind(L"\\\\", 0) == 0) {
        if (!CreateDirectoryW(path.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return L"无法创建 " + path + L" (错误码 " + std::to_wstring(GetLastError()) +
                   L"; 网络路径要求 服务器/共享 已存在)";
        return L"";
    }
    std::wstring cur;
    for (size_t i = 0; i < path.size(); i++) {
        wchar_t c = path[i];
        cur += c;
        if (c == L'\\' && cur.size() > 3) {   /* "D:\" 的首个分隔不建目录 */
            if (!CreateDirectoryW(cur.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return L"无法创建 " + cur + L" (错误码 " + std::to_wstring(GetLastError()) + L")";
        }
    }
    if (!CreateDirectoryW(path.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return L"无法创建 " + path + L" (错误码 " + std::to_wstring(GetLastError()) + L")";
    return L"";
}

/* 执行 (worker 线程; j 仅用于逐项间 abort 检查)。部分失败不算工具失败 —
 * 逐项回执进 res8, 模型据实汇报; 卡片样本 (st->top) 放受影响的源路径。
 * 每个成功项与失败项都记 st->chg (失败带原因 — 用户在更改块/回合汇总里直接看到
 * 哪些文件没动成, 不静默丢弃); 结果 JSON: changes=只发成功项, errors=失败清单。 */
std::wstring FileOpExecute(AiJob* j, const AiFileOp& op, AiToolStep* st) {
    picojson::array errs8;
    int done = 0, failed = 0, unchanged = 0;
    auto fail1 = [&](int act, const std::wstring& from, const std::wstring& to, const std::wstring& why) {
        failed++;
        errs8.push_back(JS((to.empty() ? from : from + L" → " + to) + L": " + why));
        st->chg.push_back({ act, from, to, false, why });
    };
    auto ok1 = [&](int act, const std::wstring& from, const std::wstring& to) {
        done++;
        st->chg.push_back({ act, from, to, true, L"" });
    };
    /* 源与目标同一路径 (大小写不敏感; 两侧均已反斜杠归一) = 空跑: 模型在文件已被改过名后
     * 重复提交时, "改名为自己" Windows 也报成功 — 曾据此记出 33 条前后名一样的假更改记录。
     * 无变化不执行、不计成功、不留记录, 单独计 unchanged 让模型如实告知"本就是目标状态"。 */
    auto samePath = [](const std::wstring& a, const std::wstring& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (towlower(a[i]) != towlower(b[i])) return false;
        return true;
    };
    auto abortHit = [&]() {
        return InterlockedCompareExchange(&j->abort, 0, 0) != 0;
    };
    if (op.action == 4) {   /* mkdir */
        if (GetFileAttributesW(op.target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            st->top.push_back(op.target);
            fail1(4, L"", op.target, L"已存在同名文件/目录");
        } else {
            std::wstring e = MkdirDeep(op.target);
            if (e.empty()) ok1(4, L"", op.target);
            else fail1(4, L"", op.target, e);
        }
    } else if (op.action == 2) {   /* rename */
        for (auto& pr : op.renames) {
            if (abortHit()) { fail1(2, pr.first, pr.second, L"已停止 (该调用被用户中止, 未执行)"); continue; }
            if (samePath(pr.first, pr.second)) { unchanged++; continue; }
            if (st->top.size() < 20) st->top.push_back(pr.first);
            if (!op.overwrite && GetFileAttributesW(pr.second.c_str()) != INVALID_FILE_ATTRIBUTES) {
                fail1(2, pr.first, pr.second, L"目标已存在 (overwrite=false)");
                continue;
            }
            std::wstring e = ShOpOne(FO_MOVE, pr.first, pr.second, false);
            if (e.empty()) ok1(2, pr.first, pr.second);
            else fail1(2, pr.first, pr.second, e);
        }
    } else if (op.action == 3) {   /* delete (默认回收站) */
        for (auto& p : op.items) {
            if (abortHit()) { fail1(3, p, L"", L"已停止 (该调用被用户中止, 未执行)"); continue; }
            if (st->top.size() < 20) st->top.push_back(p);
            if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
                fail1(3, p, L"", L"源不存在");
                continue;
            }
            std::wstring e = ShOpOne(FO_DELETE, p, L"", !op.permanent);
            if (e.empty()) ok1(3, p, L"");
            else fail1(3, p, L"", e);
        }
    } else {   /* copy / move */
        for (auto& p : op.items) {
            if (abortHit()) { fail1(op.action, p, L"", L"已停止 (该调用被用户中止, 未执行)"); continue; }
            if (st->top.size() < 20) st->top.push_back(p);
            if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
                fail1(op.action, p, L"", L"源不存在");
                continue;
            }
            std::wstring dst = op.target + L"\\" + FileNameOf(p);
            if (samePath(p, dst)) { unchanged++; continue; }   /* 复制/移动到原位置 = 空跑 */
            if (!op.overwrite && GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
                fail1(op.action, p, dst, L"目标已存在 (overwrite=false)");
                continue;
            }
            std::wstring e = ShOpOne(op.action == 0 ? FO_COPY : FO_MOVE, p, dst, false);
            if (e.empty()) ok1(op.action, p, dst);
            else fail1(op.action, p, dst, e);
        }
    }
    picojson::object o;
    static const wchar_t* ACTN[5] = { L"copy", L"move", L"rename", L"delete", L"mkdir" };
    o["action"] = JS(ACTN[op.action]);
    o["done"] = JN(done);
    o["failed"] = JN(failed);
    if (unchanged) o["unchanged"] = JN(unchanged);   /* 源=目标空跑: 未执行未改动 */
    if (!errs8.empty()) o["errors"] = picojson::value(errs8);
    if (!st->chg.empty()) {   /* 逐项更改回执: 只发成功项 (模型引用改后路径以此为准), 失败已在 errors */
        picojson::array chgs;
        for (auto& c : st->chg) {
            if (!c.ok) continue;
            picojson::object co;
            co["action"] = JS(ACTN[c.act % 5]);
            co["from"] = JS(c.from);
            co["to"] = JS(c.to);
            chgs.push_back(picojson::value(co));
        }
        if (!chgs.empty()) o["changes"] = picojson::value(chgs);
    }
    st->res8 = picojson::value(o).serialize();
    return L"";   /* 部分失败不是工具错误 (回执已带 errors); 全失败同理由模型据实转述 */
}

/* ==================== read_file ==================== */

static const size_t RF_READ_CAP = 8ull * 1024 * 1024;        /* 文本读取上限 */
/* 内容回传头尾: 总量 = Agent 设置 readCapKB (4..512, 缺省 30KB), 头 80% + 尾 20% (同旧 24+6 比例) */
void AiOutHeadTail(size_t* head, size_t* tail) {
    size_t total = (size_t)(g_cfg.readCapKB > 0 ? g_cfg.readCapKB : 30) * 1024;
    *head = total * 4 / 5;
    *tail = total - *head;
}
/* 内容封顶: 超 AiOutHeadTail 上限时头尾保留 + 中段带精确省略量标记 (省略数恒给精确值口径),
 * UTF-8 字符边界落刀; read_file 与 fetch_url (ai_net.cpp) 共用 */
std::string AiCapUtf8HeadTail(const std::string& content8) {
    size_t rfHead, rfTail;
    AiOutHeadTail(&rfHead, &rfTail);
    size_t cap = rfHead + rfTail;
    if (content8.size() <= cap) return content8;
    size_t headEnd = Utf8Floor(content8, rfHead);
    size_t tailBegin = Utf8Floor(content8, content8.size() - rfTail);
    if (tailBegin <= headEnd) tailBegin = headEnd;
    std::string cut = content8.substr(0, headEnd);
    cut += "\n…[中间省略 " + std::to_string(tailBegin - headEnd) + " 字节]…\n";
    cut += content8.substr(tailBegin);
    return cut;
}

/* ==================== 大输出外溢 (dsh spill 口径, 2026-09-27) ====================
 * 被头尾封顶的大输出不再"只丢中段": 完整内容落盘 %TEMP%\SnailQuickSearch-AI\,
 * 路径随工具结果回喂, 模型用 read_file(路径, offset, limit) 行窗口取回中段。
 * 失败/超 8MB = 空串, 调用方维持原头尾行为 (外溢是增益不是依赖)。 */
std::wstring AiSpillText(const std::string& content8, const char* tag8) {
    static const size_t SPILL_CAP = 8u * 1024 * 1024;
    if (!tag8 || content8.empty() || content8.size() > SPILL_CAP) return L"";
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring dir = std::wstring(tmp) + L"SnailQuickSearch-AI";
    CreateDirectoryW(dir.c_str(), NULL);   /* 已存在 = ERROR_ALREADY_EXISTS, 无妨 */
    static LONG swept = 0;                 /* 首次写入顺手清 7 天前的旧外溢 (进程一次) */
    if (InterlockedCompareExchange(&swept, 1, 0) == 0) {
        FILETIME nowFt;
        GetSystemTimeAsFileTime(&nowFt);
        ULONGLONG cutoff = ((ULONGLONG)nowFt.dwHighDateTime << 32 | nowFt.dwLowDateTime) -
                           7ull * 24 * 3600 * 10000000;
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW((dir + L"\\*.txt").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                ULONGLONG wt = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) |
                               fd.ftLastWriteTime.dwLowDateTime;
                if (wt < cutoff) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
    }
    static LONG seq = 0;   /* 同毫秒同名防覆盖 */
    wchar_t name[64];
    swprintf(name, 64, L"溢存-%S-%llu-%ld.txt", tag8,
             (unsigned long long)GetTickCount64(), InterlockedIncrement(&seq));
    std::wstring path = dir + L"\\" + name;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return L"";
    DWORD wr = 0;
    /* 半截写 (磁盘满/配额/杀软拦截) = 没写: 删掉残件维持"写失败=空串"口径,
     * 调用方回退原头尾封顶行为 — 否则模型按"外溢文件"续读到的是静默截断的内容 */
    BOOL okW = WriteFile(f, "\xEF\xBB\xBF", 3, &wr, NULL) && wr == 3;   /* UTF-8 BOM: read_file/notepad 都认 */
    okW = okW && WriteFile(f, content8.data(), (DWORD)content8.size(), &wr, NULL) &&
          wr == (DWORD)content8.size();
    CloseHandle(f);
    if (!okW) {
        DeleteFileW(path.c_str());
        return L"";
    }
    return path;
}

/* 行窗口切取 (read_file offset/limit 分页, dsh read 口径): offset=1-based 起始行,
 * limit=行数上限, 窗口按 budget 字节到行为止 (单行超预算 = 截该行, 与 dsh
 * readMaxLineLength 同粒度; 行内续读不支持, 行级分页已覆盖大文件场景)。
 * note = 分页脚注 (返回行范围/下一页 offset/超尾提示)。纯函数: 预算经参数传入,
 * test\test_ai_file.cpp 直测。 */
std::string AiWindowLines(const std::string& content8, long long offset, long long limit,
                          size_t budget, std::wstring* note) {
    if (note) note->clear();
    if (offset < 1) offset = 1;
    if (limit < 1) limit = 1;
    if (limit > 2000) limit = 2000;
    /* 数总行: '\n' 分隔, 末行无换行也算一行, 末尾换行不产空行 */
    long long total = 0;
    {
        size_t p = 0;
        for (;;) {
            total++;
            size_t nx = content8.find('\n', p);
            if (nx == std::string::npos) break;
            p = nx + 1;
            if (p == content8.size()) break;
        }
        if (content8.empty()) total = 0;
    }
    if (offset > total) {
        if (note)
            *note = L"起始行 " + std::to_wstring(offset) + L" 超出总行数 (共 " +
                    std::to_wstring(total) + L" 行) — 返回空, 不要再加大 offset";
        return "";
    }
    std::string out;
    long long line = 1, endLine = 0;
    bool budgetCut = false;
    size_t p = 0;
    for (; line <= total; line++) {
        size_t nx = content8.find('\n', p);
        size_t e = (nx == std::string::npos) ? content8.size() : nx;
        bool last = (nx == std::string::npos);
        if (line >= offset) {
            size_t len = e - p;
            if (len > 0 && content8[e - 1] == '\r') len--;   /* CRLF 的 '\r' 不入窗 */
            if (out.size() + len + 1 > budget) {
                if (out.empty()) {   /* 单行就超预算: 截该行 (行内续读不支持, dsh 同粒度; 落刀回退 UTF-8 边界,
                                        与 AiCapUtf8HeadTail 同口径) */
                    size_t take = budget > 0 ? budget - 1 : 0;
                    take = Utf8Floor(content8, take);
                    out.append(content8, p, take);
                    out += '\n';
                    endLine = line;
                }
                budgetCut = true;    /* 否则本行放不下: 留给下一窗 */
                break;
            }
            out.append(content8, p, len);
            out += '\n';
            endLine = line;
            if (endLine - offset + 1 >= limit) break;
        }
        if (last) break;
        p = nx + 1;
    }
    if (note) {
        *note = L"第 " + std::to_wstring(offset) + L"–" + std::to_wstring(endLine) +
                L" 行 / 共 " + std::to_wstring(total) + L" 行";
        if (budgetCut || endLine < total)
            *note += L"; 继续读取用 offset=" + std::to_wstring(endLine + 1);
        if (!budgetCut && endLine >= total) *note += L" (已到末尾)";
    }
    return out;
}

/* read_file 实体: 文本 (编码识别) / docx·pptx·xlsx (解包抽文字); 其它二进制明确报错 */
std::wstring ReadFileToolExec(const Jv& v, AiToolStep* st) {
    st->kind = 12;
    std::vector<std::wstring> list;
    std::wstring err = FileResolveTargets(v, &list, 1);
    if (!err.empty()) return err;
    if (list.empty()) return L"缺少 path 或 id 参数 (二者其一)";
    const std::wstring& path = list[0];
    st->argz = FileNameOf(path);
    if (st->argz.size() > 200) { st->argz.resize(200); st->argz += L"…"; }
    DWORD at = GetFileAttributesW(path.c_str());
    if (at == INVALID_FILE_ATTRIBUTES) return L"文件不存在或无法访问: " + path;
    if (at & FILE_ATTRIBUTE_DIRECTORY)
        return L"这是一个目录, 不是文件: " + path + L" (目录清单用 run_search 的 ParentPath 条件)";
    std::wstring ext = ExtLowerOf(path);
    std::string content8;
    std::wstring kindName;
    unsigned long long rawSize = 0;
    bool truncExtract = false;
    if (ext == L"docx" || ext == L"pptx" || ext == L"xlsx") {
        std::wstring oerr;
        if (!AiOfficeExtract(path, ext, &content8, &kindName, &truncExtract, &oerr))
            return oerr;
        /* 抽取文本里不应有 NUL (XML 文本层); 防御性剔除 */
        content8.erase(std::remove(content8.begin(), content8.end(), '\0'), content8.end());
    } else if (ext == L"pdf") {
        return L"PDF 文本抽取暂不支持 (本工具只读文本与 docx/pptx/xlsx) — "
               L"可如实告知用户: 若安装了可用的命令行转换工具可经 run_command 转出文本后再读";
    } else if (ext == L"doc" || ext == L"xls" || ext == L"ppt") {
        return L"旧版二进制 Office 格式暂不支持 — 请用户把文件另存为 docx/xlsx/pptx 后重试";
    } else {
        std::string raw;
        std::wstring rerr;
        if (!AiReadWholeFile(path, RF_READ_CAP, &raw, &rerr)) return rerr;
        rawSize = raw.size();
        /* UTF-16 文本天然含 NUL 高位字节 — 先认 BOM 再做二进制判定, 顺序不能反 */
        bool utf16 = raw.size() >= 2 &&
                     ((raw[0] == (char)0xFF && raw[1] == (char)0xFE) ||
                      (raw[0] == (char)0xFE && raw[1] == (char)0xFF));
        std::wstring enc;
        content8 = AiTextToUtf8(raw, &enc);
        kindName = enc;
        if (!utf16 && content8.find('\0') != std::string::npos)
            return L"内容含二进制数据, read_file 只支持文本文件 (" +
                   (ext.empty() ? L"无扩展名" : L"." + ext) + L")";
        if (utf16 && content8.find('\0') != std::string::npos)
            return L"内容含二进制数据, read_file 只支持文本文件";
    }
    /* 行窗口分页 (dsh read offset/limit 口径, 2026-09-27): 显式传 offset/limit 才启用 —
     * 大文件被头尾封顶截断的中段用行窗口分段读 (docx/pptx/xlsx 抽取文本同样适用);
     * 未传 = 整篇头尾封顶 + 中段外溢落盘给路径 (原行为 + 外溢增益) */
    const Jv* ov = v.Get(L"offset");
    const Jv* lv = v.Get(L"limit");
    bool paged = (ov && ov->t == 2 && ov->num >= 1) || (lv && lv->t == 2 && lv->num >= 1);
    size_t rfHead = 0, rfTail = 0;
    AiOutHeadTail(&rfHead, &rfTail);
    if (paged) {
        long long offset = (ov && ov->t == 2 && ov->num >= 1) ? (long long)ov->num : 1;
        long long limit = (lv && lv->t == 2 && lv->num >= 1) ? (long long)lv->num : 2000;
        std::wstring note;
        std::string win8 = AiWindowLines(content8, offset, limit, rfHead + rfTail, &note);
        picojson::object o;
        o["path"] = JS(path);
        o["格式或编码"] = JS(kindName);
        o["content"] = JS(W8(win8.c_str()));
        o["行范围"] = JS(note);
        if (rawSize) o["总字节"] = JN((long long)rawSize);
        if (truncExtract) o["说明"] = JS(L"文档过长, 文字抽取在中途收卷, 之后内容未包含");
        st->res8 = picojson::value(o).serialize();
        return L"";
    }
    /* 内容封顶: 头+尾+精确省略量 (与 fetch_url 共用收口); 中段外溢落盘给路径 */
    std::string full8 = content8;
    content8 = AiCapUtf8HeadTail(content8);
    picojson::object o;
    o["path"] = JS(path);
    o["格式或编码"] = JS(kindName);
    o["content"] = JS(W8(content8.c_str()));
    if (rawSize) o["总字节"] = JN((long long)rawSize);
    if (truncExtract) o["说明"] = JS(L"文档过长, 文字抽取在中途收卷, 之后内容未包含");
    if (full8.size() > rfHead + rfTail) {
        std::wstring spill = AiSpillText(full8, "file");
        if (!spill.empty()) {
            o["外溢文件"] = JS(spill);
            o["续读"] = JS(L"完整内容已存为外溢文件, 用 read_file(该路径, offset, limit) "
                           L"按行窗口读取头尾之外的中段 (offset=起始行, limit=行数)");
        }
    }
    st->res8 = picojson::value(o).serialize();
    return L"";
}

/* ==================== read_image ==================== */

static const unsigned long long RI_REENCODE_CAP = 24ull * 1024 * 1024;   /* 重编码路径的原图读取上限 */

/* 常见图片格式魔数 → mime (≤AI_ATT_IMG_MAX 的原样直传判定) */
static const wchar_t* ImageMimeOf(const std::string& d) {
    if (d.size() >= 3 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8) return L"image/jpeg";
    if (d.size() >= 8 && memcmp(d.data(), "\x89PNG\r\n\x1a\n", 8) == 0) return L"image/png";
    if (d.size() >= 6 && (memcmp(d.data(), "GIF87a", 6) == 0 || memcmp(d.data(), "GIF89a", 6) == 0))
        return L"image/gif";
    if (d.size() >= 12 && memcmp(d.data(), "RIFF", 4) == 0 && memcmp(d.data() + 8, "WEBP", 4) == 0)
        return L"image/webp";
    if (d.size() >= 2 && d[0] == 'B' && d[1] == 'M') return L"image/bmp";
    return NULL;
}

/* WIC 解码 → 最长边 >2000 时等比缩放 → JPEG(0.9) 重编码 (超大图/未知格式兜底) */
static bool WicShrinkToJpeg(const std::wstring& path, std::string* jpeg) {
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool comHere = SUCCEEDED(co);
    bool ok = false;
    UINT ow = 0, oh = 0, tw = 0, th = 0;
    do {
        Microsoft::WRL::ComPtr<IWICImagingFactory> f;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&f))))
            break;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec;
        if (FAILED(f->CreateDecoderFromFilename(path.c_str(), NULL, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &dec)))
            break;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(dec->GetFrame(0, &frame))) break;
        if (FAILED(frame->GetSize(&ow, &oh)) || !ow || !oh) break;
        Microsoft::WRL::ComPtr<IWICBitmapSource> src = frame;
        UINT lng = ow > oh ? ow : oh;
        tw = ow;
        th = oh;
        if (lng > 2000) {   /* 视觉接口的实用档位: 更大只烧 token 不增识别 */
            tw = (UINT)((unsigned long long)ow * 2000 / lng);
            th = (UINT)((unsigned long long)oh * 2000 / lng);
            if (tw < 1) tw = 1;
            if (th < 1) th = 1;
            Microsoft::WRL::ComPtr<IWICBitmapScaler> sc;
            if (FAILED(f->CreateBitmapScaler(&sc))) break;
            if (FAILED(sc->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeFant))) break;
            src = sc;
        }
        Microsoft::WRL::ComPtr<IStream> stream;
        if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream))) break;   /* 内存流: 编码器写入, 之后读回 JPEG */
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> enc;
        if (FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, NULL, &enc))) break;
        if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) break;
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> fe;
        Microsoft::WRL::ComPtr<IPropertyBag2> pb;
        if (FAILED(enc->CreateNewFrame(&fe, &pb))) break;
        if (pb) {
            PROPBAG2 opt = {};
            opt.pstrName = (LPOLESTR)L"ImageQuality";   /* Write 只读该名 */
            VARIANT vt;
            VariantInit(&vt);
            vt.vt = VT_R4;
            vt.fltVal = 0.9f;
            pb->Write(1, &opt, &vt);
            VariantClear(&vt);
        }
        if (FAILED(fe->Initialize(pb.Get()))) break;
        if (FAILED(fe->SetSize(tw, th))) break;
        if (FAILED(fe->WriteSource(src.Get(), NULL))) break;
        if (FAILED(fe->Commit())) break;
        if (FAILED(enc->Commit())) break;
        STATSTG stg = {};
        if (FAILED(stream->Stat(&stg, STATFLAG_NONAME)) || stg.cbSize.QuadPart == 0) break;
        HGLOBAL hg = NULL;
        if (FAILED(GetHGlobalFromStream(stream.Get(), &hg))) break;
        const void* p = GlobalLock(hg);
        if (!p) break;
        jpeg->assign((const char*)p, (size_t)stg.cbSize.QuadPart);
        GlobalUnlock(hg);
        ok = true;
    } while (0);
    if (comHere) CoUninitialize();
    return ok;
}

/* read_image 实体: 图片 → data URL 注入 j->injImgs (请求体构建时组 input_image) */
std::wstring ReadImageToolExec(AiJob* j, const Jv& v, AiToolStep* st) {
    st->kind = 14;
    if (!g_cfg.img)
        return L"当前模型档案未开启图片输入能力 (接口设置勾选「图片输入」), 无法看图 — 请如实告知用户";
    if (j->injImgs.size() >= AI_ATT_MAX)
        return L"一次任务最多注入 " + std::to_wstring(AI_ATT_MAX) + L" 张图片 (已达上限)";
    std::vector<std::wstring> list;
    std::wstring err = FileResolveTargets(v, &list, 1);
    if (!err.empty()) return err;
    if (list.empty()) return L"缺少 path 或 id 参数 (二者其一)";
    const std::wstring& path = list[0];
    st->argz = FileNameOf(path);
    if (st->argz.size() > 200) { st->argz.resize(200); st->argz += L"…"; }
    DWORD at = GetFileAttributesW(path.c_str());
    if (at == INVALID_FILE_ATTRIBUTES) return L"文件不存在或无法访问: " + path;
    if (at & FILE_ATTRIBUTE_DIRECTORY) return L"这是一个目录, 不是图片文件: " + path;
    std::string raw;
    std::wstring rerr;
    if (!AiReadWholeFile(path, RI_REENCODE_CAP, &raw, &rerr)) return rerr;
    const wchar_t* mime = ImageMimeOf(raw);
    std::string b64;
    std::wstring fmtName;
    if (mime && raw.size() <= AI_ATT_IMG_MAX) {
        b64 = AiB64Enc((const unsigned char*)raw.data(), raw.size());
        static const struct { const wchar_t* m; const wchar_t* n; } NM[] = {
            { L"image/jpeg", L"JPEG" }, { L"image/png", L"PNG" }, { L"image/gif", L"GIF (取首帧)" },
            { L"image/webp", L"WebP" }, { L"image/bmp", L"BMP" },
        };
        for (auto& x : NM)
            if (wcscmp(x.m, mime) == 0) { fmtName = x.n; break; }
        if (fmtName.empty()) fmtName = mime;
    } else {
        /* 超过直传上限或未知/罕见格式: WIC 解码压缩重编码 (顺带兜住 TIFF 等系统认识的样子) */
        std::string jpeg;
        if (!WicShrinkToJpeg(path, &jpeg) || jpeg.empty())
            return L"不是可识别的图片文件, 解码失败: " + path;
        b64 = AiB64Enc((const unsigned char*)jpeg.data(), jpeg.size());
        mime = L"image/jpeg";
        fmtName = L"已压缩转 JPEG (原图过大或非常见格式)";
    }
    std::string dataUrl8 = std::string("data:") + U8(mime) + ";base64," + b64;   /* base64 恒 ASCII */
    j->injImgs.push_back(std::move(dataUrl8));
    picojson::object o;
    o["path"] = JS(path);
    o["bytes"] = JN((long long)raw.size());
    o["格式"] = JS(fmtName);
    o["说明"] = JS(L"图片已附加到本次请求, 直接看图描述/分析; 看不清或图里没有的信息如实说明");
    st->res8 = picojson::value(o).serialize();
    return L"";
}

/* ==================== 资源管理器窗口枚举 (list_explorer_windows, 2.12.0) ====================
 * 用户已打开的文件管理器 (Windows 资源管理器) 窗口/标签页清单 — 以"用户当前正在浏览的
 * 位置"为上下文继续任务 (顺藤摸瓜、报告用户开着哪些位置等)。Shell COM 只读枚举,
 * 不碰文件内容/引擎/宿主/UI, 免权限 (同 get_window_selection 的读环境面口径)。
 */

/* file:// URL → Windows 路径 (纯函数, test\test_ai_file.cpp 直测)。
 * 百分号转义按 UTF-8 字节解码 (Explorer 对非 ASCII 文件夹名发 %E9.. 形), 裸非 ASCII
 * 宽字符照留; UNC 形 file://server/share → \\server\share; ::{CLSID} 虚拟命名空间
 * (此电脑/回收站/控制面板…) = 空串, 调用方标 virtual; 非 file: 协议 = 空串。
 * '+' 不还原成空格 (那是表单编码, 不是 URI 路径); 单遍解码不二次展开 (%25 → %)。 */
std::wstring ExplorerUrlToPath(const std::wstring& url) {
    if (_wcsnicmp(url.c_str(), L"file:", 5) != 0) return L"";
    std::wstring rest = url.substr(5);
    if (rest.rfind(L"//", 0) != 0) return L"";
    rest = rest.substr(2);
    if (rest.rfind(L"::{", 0) == 0 || rest.rfind(L"/::{", 0) == 0) return L"";   /* 虚拟命名空间 */
    std::wstring path;
    if (!rest.empty() && rest[0] == L'/')
        path = rest.substr(1);                    /* file:///D:/... → D:/... */
    else
        path = L"\\\\" + rest;                    /* file://server/share → \\server\share */
    /* 百分号 → 字节流; 段间夹杂的裸宽字符先冲刷字节流 (UTF-8→宽) 再原样接上 */
    std::string u8;
    std::wstring out;
    auto flush = [&]() {
        if (u8.empty()) return;
        int wl = MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), NULL, 0);
        if (wl > 0) {
            std::wstring w((size_t)wl, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), &w[0], wl);
            out += w;
        }
        u8.clear();
    };
    auto hx = [](wchar_t h) { return (int)(h <= L'9' ? h - L'0' : (towlower(h) - L'a' + 10)); };
    for (size_t i = 0; i < path.size(); i++) {
        wchar_t c = path[i];
        if (c == L'%' && i + 2 < path.size() && iswxdigit(path[i + 1]) && iswxdigit(path[i + 2])) {
            u8.push_back((char)((hx(path[i + 1]) << 4) | hx(path[i + 2])));
            i += 2;
        } else if ((unsigned)c >= 0x80) {
            flush();
            out.push_back(c);
        } else {
            u8.push_back((char)c);                /* ASCII 照走字节流 ('/' 统一在最后转) */
        }
    }
    flush();
    for (auto& ch : out)
        if (ch == L'/') ch = L'\\';
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();   /* 去尾分隔 (盘根 D:\ 保留) */
    return out;
}

/* ExplorerWindowEnumerator — 枚举当前打开的资源管理器窗口 (Shell COM IShellWindows):
 * 每个窗口/标签页一条 (Win11 多标签共用顶层 hwnd 各占一条), 过滤 不可见 与 非
 * explorer.exe 宿主 (FullName 尾段比对, 排除残留 IE/自动化宿主; FullName 不可得时保留)。
 * 失败 = false (err=原因)。CoInitializeEx 模式照 WicShrinkToJpeg (SUCCEEDED 含 S_FALSE,
 * 配对 CoUninitialize; RPC_E_CHANGED_MODE = 已按别的模式初始化, COM 照用)。 */
bool ExplorerWindowEnumerator(std::vector<ExplorerWinInfo>* out, std::wstring* err) {
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool comHere = SUCCEEDED(co);
    bool ok = false;
    Microsoft::WRL::ComPtr<IShellWindows> sw;
    do {
        if (FAILED(CoCreateInstance(__uuidof(ShellWindows), NULL, CLSCTX_ALL,
                                    __uuidof(IShellWindows), (void**)sw.GetAddressOf()))) {
            *err = L"无法连接 Shell 窗口枚举服务 (CoCreateInstance 失败)";
            break;
        }
        long n = 0;
        if (FAILED(sw->get_Count(&n))) { *err = L"枚举 Shell 窗口失败"; break; }
        ok = true;
        for (long i = 0; i < n; i++) {
            VARIANT vi;
            VariantInit(&vi);
            vi.vt = VT_I4;
            vi.lVal = i;
            Microsoft::WRL::ComPtr<IDispatch> disp;
            if (FAILED(sw->Item(vi, &disp)) || !disp) continue;
            Microsoft::WRL::ComPtr<IWebBrowser2> wb;
            if (FAILED(disp.As(&wb)) || !wb) continue;
            SHANDLE_PTR hp = 0;
            wb->get_HWND(&hp);
            HWND hwnd = (HWND)(intptr_t)hp;
            if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) continue;
            BSTR full = NULL;
            if (SUCCEEDED(wb->get_FullName(&full)) && full) {
                const wchar_t* exe = wcsrchr(full, L'\\');
                exe = exe ? exe + 1 : full;
                bool keep = _wcsicmp(exe, L"explorer.exe") == 0;
                SysFreeString(full);
                if (!keep) continue;
            }
            ExplorerWinInfo wi;
            wi.hwnd = (long long)(intptr_t)hwnd;
            wi.isForeground = (GetForegroundWindow() == hwnd);   /* 用户正看着的窗口 */
            BSTR name = NULL;
            if (SUCCEEDED(wb->get_LocationName(&name)) && name) {
                wi.title = name;
                SysFreeString(name);
            }
            BSTR url = NULL;
            if (SUCCEEDED(wb->get_LocationURL(&url)) && url) {
                wi.path = ExplorerUrlToPath(url);
                SysFreeString(url);
            }
            if (wi.path.empty()) wi.isVirtual = true;
            /* 去重 (同 hwnd+路径+标题的重复条目 = Shell 偶发重报) */
            bool dup = false;
            for (auto& e : *out)
                if (e.hwnd == wi.hwnd && e.path == wi.path && e.title == wi.title) { dup = true; break; }
            if (!dup) out->push_back(std::move(wi));
        }
    } while (0);
    sw.Reset();
    if (comHere) CoUninitialize();
    return ok;
}

/* list_explorer_windows 实体 (ai_agent.cpp AgentToolExec 分发, kind 17)。
 * 结果 JSON: count + windows=[{hwnd,title,path}] — path 缺失 = 虚拟位置,
 * 同条补 virtual=true (工具 description 已声明该口径)。 */
std::wstring ListExplorerWindowsExec(const Jv& v, AiToolStep* st) {
    (void)v;
    std::vector<ExplorerWinInfo> wins;
    std::wstring err;
    if (!ExplorerWindowEnumerator(&wins, &err)) return err;
    picojson::object out;
    out["count"] = JN((long long)wins.size());
    picojson::array arr;
    for (auto& w : wins) {
        picojson::object o;
        o["hwnd"] = JN(w.hwnd);
        if (!w.title.empty()) o["title"] = JS(w.title);
        if (!w.path.empty())
            o["path"] = JS(w.path);
        else
            o["virtual"] = picojson::value(true);
        if (w.isForeground) o["active"] = picojson::value(true);
        arr.push_back(picojson::value(o));
    }
    out["windows"] = picojson::value(arr);
    st->res8 = picojson::value(out).serialize();
    wchar_t nb[48];
    swprintf(nb, 48, L"%d 个窗口", (int)wins.size());
    st->argz = nb;
    return L"";
}

/* explorer_window_op 实体 (kind 18, ai_agent.cpp 分发): 对 list_explorer_windows 列出的
 * 窗口执行管理动作。全部为瞬时界面动作 (不改任何持久设置), 按"任务类直执行"先例
 * (open_file/set_search) 不走提案卡, 行为约束 (只做用户明确要求的动作) 住工具 description。
 * 目标 hwnd 必须仍在本次现枚举的 Shell 窗口集内 (防陈旧句柄 / 误指其它程序的窗口) —
 * 每次动作前重枚举一次, 顺带拿到窗口标题进卡片 (argz)。 */
std::wstring ExplorerWindowOpExec(const Jv& v, AiToolStep* st) {
    std::wstring act = TrimW(v.S(L"action"));
    static const wchar_t* ACTS[] = { L"activate", L"close", L"minimize", L"maximize", L"restore", L"open" };
    int ai = -1;
    for (int i = 0; i < 6; i++)
        if (act == ACTS[i]) { ai = i; break; }
    if (ai < 0)
        return L"action 只接受 activate | close | minimize | maximize | restore | open";

    /* open: 打开文件夹 (ShellExecute 系统默认行为 — 该文件夹已在某窗口打开时激活那个窗口,
     * 否则开新窗口)。必须是已存在的文件夹; 打开/定位文件走 open_file。 */
    if (ai == 5) {
        std::wstring p = TrimW(v.S(L"path"));
        if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);
        for (auto& c : p) if (c == L'/') c = L'\\';
        if (p.empty() || !FilePathOk(p)) return L"open 需要 path (文件夹绝对路径)";
        DWORD at = GetFileAttributesW(p.c_str());
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY))
            return L"不是可访问的文件夹: " + p + L" (打开/定位文件用 open_file)";
        HINSTANCE r = ShellExecuteW(NULL, L"open", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
        if ((intptr_t)r <= 32) return L"打开失败 (ShellExecute 错误)";
        st->argz = p.size() > 120 ? p.substr(0, 120) + L"…" : p;
        picojson::object o;
        o["ok"] = picojson::value(true);
        o["action"] = JS(L"open");
        o["path"] = JS(p);
        st->res8 = picojson::value(o).serialize();
        return L"";
    }

    /* 其余动作按 hwnd 定向 */
    const Jv* hv = v.Get(L"hwnd");
    long long hwndN = hv && hv->t == 2 ? (long long)hv->num : 0;
    if (!hwndN) return L"hwnd 必填 (list_explorer_windows 获取)";
    HWND hwnd = (HWND)(intptr_t)hwndN;
    if (!IsWindow(hwnd))
        return L"hwnd 已失效 (窗口已关闭? 先 list_explorer_windows 重新获取)";
    std::vector<ExplorerWinInfo> wins;
    std::wstring err;
    if (!ExplorerWindowEnumerator(&wins, &err)) return err;
    std::wstring title;
    bool known = false;
    for (auto& w : wins)
        if (w.hwnd == hwndN) { known = true; title = w.title; break; }
    if (!known)
        return L"hwnd 不是当前打开的资源管理器窗口 (已关闭? 先 list_explorer_windows 重新获取)";

    std::wstring note;
    std::wstring tgt = title.empty() ? std::to_wstring(hwndN) : title;
    picojson::object o;
    o["ok"] = picojson::value(true);
    o["action"] = JS(act);
    o["hwnd"] = JN(hwndN);
    if (!title.empty()) o["title"] = JS(title);

    if (ai == 0) {
        /* activate: 先还原/显示, 抢前台用 AttachThreadInput 对账 (跨进程前台锁的通行解法),
         * 结束后 GetForegroundWindow 复核 — 未落前台在 note 如实说, 不假装成功 */
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        else ShowWindow(hwnd, SW_SHOW);
        bool fg = GetForegroundWindow() == hwnd;
        if (!fg) {
            DWORD myTid = GetCurrentThreadId();
            HWND fgh = GetForegroundWindow();
            DWORD fgTid = fgh ? GetWindowThreadProcessId(fgh, NULL) : 0;
            DWORD tgTid = GetWindowThreadProcessId(hwnd, NULL);
            if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
            if (tgTid && tgTid != myTid) AttachThreadInput(myTid, tgTid, TRUE);
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
            if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
            if (tgTid && tgTid != myTid) AttachThreadInput(myTid, tgTid, FALSE);
            fg = GetForegroundWindow() == hwnd;
        }
        o["foreground"] = picojson::value(fg);
        if (!fg) note = L"窗口已显示但前台焦点未落到它 (可能被系统前台锁策略拦截)";
        st->argz = L"激活 " + tgt;
    } else if (ai == 1) {
        /* close: WM_CLOSE (礼貌关闭, 整个窗口含全部标签页) + 短轮询拿真实 closed 结果 */
        bool sent = PostMessageW(hwnd, WM_CLOSE, 0, 0) != 0;
        bool closed = false;
        for (int i = 0; sent && i < 3 && !closed; i++) {
            Sleep(250);
            closed = !IsWindow(hwnd);
        }
        o["closed"] = picojson::value(closed);
        if (!sent) note = L"关闭消息发送失败";
        else if (!closed) note = L"关闭请求已发送, 窗口仍在 (可能弹出了确认框或被拦截)";
        st->argz = L"关闭 " + tgt;
    } else {
        ShowWindow(hwnd, ai == 2 ? SW_MINIMIZE : ai == 3 ? SW_MAXIMIZE : SW_RESTORE);
        bool done = ai == 2 ? IsIconic(hwnd) != 0
                  : ai == 3 ? IsZoomed(hwnd) != 0
                  : (!IsIconic(hwnd) && !IsZoomed(hwnd));
        o["done"] = picojson::value(done);
        if (!done) note = L"窗口状态未按预期变化";
        st->argz = std::wstring(ai == 2 ? L"最小化 " : ai == 3 ? L"最大化 " : L"还原 ") + tgt;
    }
    if (st->argz.size() > 80) st->argz.resize(80);
    if (!note.empty()) o["note"] = JS(note);
    st->res8 = picojson::value(o).serialize();
    return L"";
}
