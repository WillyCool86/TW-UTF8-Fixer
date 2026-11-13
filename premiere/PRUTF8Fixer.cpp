#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectVers.h"
#include "AE_EffectSuites.h"
#include "AE_GeneralPlug.h"
#include "AE_CacheOnLoadSuite.h"
#include "MinHook.h"

#include <windows.h>
#include <tlhelp32.h>
#ifndef EXCEPTION_EXECUTE_FHANDLER
#define EXCEPTION_EXECUTE_FHANDLER 1
#endif
#include <string>
#include <string_view>
#include <cstring>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <span>
#include <cstdio>
#include <cstdarg>
#include <share.h>

// ==========================================
// 0. Premiere 版新增：自身模块句柄 + 诊断日志
//    日志位置：%TEMP%\PR_UTF8_Fixer.log
// ==========================================

static HMODULE g_SelfModule = nullptr;
static std::mutex g_LogMutex;
static std::atomic<int> g_LogLines{ 0 };
static constexpr int kMaxLogLines = 600;

static bool GetLogPath(wchar_t* out, size_t cap) noexcept {
    DWORD n = GetTempPathW(static_cast<DWORD>(cap), out);
    if (n == 0 || n + 32 >= cap) return false;
    return wcscat_s(out, cap, L"PR_UTF8_Fixer.log") == 0;
}

static void LogReset() noexcept {
    wchar_t path[MAX_PATH];
    if (!GetLogPath(path, MAX_PATH)) return;
    std::lock_guard<std::mutex> lock(g_LogMutex);
    FILE* f = _wfsopen(path, L"wb", _SH_DENYNO);
    if (f) {
        fputs("\xEF\xBB\xBF", f); // UTF-8 BOM，方便记事本打开
        fclose(f);
    }
}

static void LogLine(const char* fmt, ...) noexcept {
    if (g_LogLines.fetch_add(1, std::memory_order_relaxed) >= kMaxLogLines) return;
    wchar_t path[MAX_PATH];
    if (!GetLogPath(path, MAX_PATH)) return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard<std::mutex> lock(g_LogMutex);
    FILE* f = _wfsopen(path, L"ab", _SH_DENYNO);
    if (f) {
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fclose(f);
    }
}

static void LogWidePath(const char* prefix, const wchar_t* wpath) noexcept {
    char u8[1024] = {};
    if (wpath) WideCharToMultiByte(CP_UTF8, 0, wpath, -1, u8, sizeof(u8) - 1, nullptr, nullptr);
    LogLine("%s %s", prefix, u8);
}

// 跟踪已 hook 的插件数量
#define WRAPPER_POOL_SIZE 2048
std::atomic<void*> g_OriginalEntryPoints[WRAPPER_POOL_SIZE] = {};
std::atomic<int> g_WrapperCount{ 0 };
std::mutex g_WrapperMutex;

// ==========================================
// 1. UTF-8 验证与处理
// ==========================================

inline bool IsValidUTF8(std::string_view sv) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(sv.data());
    const auto* end = bytes + sv.size();

    bool has_high_byte = false;      
    bool has_3byte_character = false; 

    while (bytes < end) {
        if (bytes[0] <= 0x7F) {
            bytes += 1;
        }
        else if ((bytes[0] & 0xE0) == 0xC0) {
            if (bytes + 1 >= end || (bytes[1] & 0xC0) != 0x80) return false;
            has_high_byte = true;
            bytes += 2;
        }
        else if ((bytes[0] & 0xF0) == 0xE0) {
            if (bytes + 2 >= end || (bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80) return false;
            has_high_byte = true;
            has_3byte_character = true;
            bytes += 3;
        }
        else if ((bytes[0] & 0xF8) == 0xF0) {
            if (bytes + 3 >= end || (bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80 || (bytes[3] & 0xC0) != 0x80) return false;
            has_high_byte = true;
            bytes += 4;
        }
        else {
            return false;
        }
    }

    if (has_high_byte && !has_3byte_character) {
        return false; 
    }

    return true;
}

void TrimInvalidTailUTF8(std::span<char> s) noexcept {
    if (s.empty()) return;
    size_t i = s.size();
    while (i > 0) {
        auto c = static_cast<unsigned char>(s[i - 1]);
        if ((c & 0x80) == 0x00) {
            break;
        }
        else if ((c & 0xC0) == 0xC0) {
            size_t need_bytes = 0;
            if ((c & 0xE0) == 0xC0) need_bytes = 2;
            else if ((c & 0xF0) == 0xE0) need_bytes = 3;
            else if ((c & 0xF8) == 0xF0) need_bytes = 4;

            if (s.size() - (i - 1) < need_bytes) {
                s[i - 1] = '\0';
            }
            break;
        }
        else if ((c & 0xC0) == 0x80) {
            i--;
        }
        else {
            s[i - 1] = '\0';
            i--;
        }
    }
}

// ==========================================
// 2. GBK 转换器
//    目标编码依系统而定：
//    - 繁体中文系统 (ACP 950)：Premiere 用 Big5 解读参数名，
//      所以先简转繁，再输出 Big5
//    - 其他系统：与 AE 原版相同，输出 UTF-8
// ==========================================

static UINT g_TargetCP = CP_UTF8;
static bool g_ToTraditional = false;

static bool ConvertFromGBK(const char* gbk, size_t maxBytes, std::string& out, std::string& logU8) {
    if (!gbk || gbk[0] == '\0') return false;

    constexpr UINT kGBK = 936;
    int wlen = MultiByteToWideChar(kGBK, 0, gbk, -1, nullptr, 0);
    if (wlen <= 1) return false;

    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(kGBK, 0, gbk, -1, w.data(), wlen);
    w.resize(static_cast<size_t>(wlen) - 1);

    if (g_ToTraditional) {
        std::wstring t(w.size(), L'\0');
        int r = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_TRADITIONAL_CHINESE,
                              w.c_str(), static_cast<int>(w.size()),
                              t.data(), static_cast<int>(t.size()),
                              nullptr, nullptr, 0);
        if (r == static_cast<int>(w.size())) w.swap(t);
    }

    // 以「字」为单位截断，保证不会切出半个字
    size_t n = w.size();
    for (;;) {
        if (n == 0) return false;
        int blen = WideCharToMultiByte(g_TargetCP, 0, w.data(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        if (blen <= 0) return false;
        if (maxBytes == 0 || static_cast<size_t>(blen) <= maxBytes) {
            out.assign(static_cast<size_t>(blen), '\0');
            WideCharToMultiByte(g_TargetCP, 0, w.data(), static_cast<int>(n), out.data(), blen, nullptr, nullptr);
            break;
        }
        --n;
    }

    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    if (ulen > 0) {
        logU8.assign(static_cast<size_t>(ulen), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(n), logU8.data(), ulen, nullptr, nullptr);
    }
    return !out.empty();
}

// ==========================================
// 3. 全局字符串缓存
//    同时用来记住「我们自己输出过的字符串」，避免二次转换
//    （Big5 输出不是合法 UTF-8，不记住的话会被当成 GBK 再转一次）
// ==========================================

static constexpr size_t kCacheShards = 4;

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

struct StringEqual {
    using is_transparent = void;
    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

struct CacheContainer {
    mutable std::shared_mutex mutex;
    std::unordered_set<std::string, StringHash, StringEqual> cache;
};

inline CacheContainer& GetShardedCache(std::string_view sv) noexcept {
    static CacheContainer shards[kCacheShards];
    size_t hash = std::hash<std::string_view>{}(sv);
    return shards[hash % kCacheShards];
}

static bool IsKnownOutput(std::string_view sv) {
    auto& c = GetShardedCache(sv);
    std::shared_lock read_lock(c.mutex);
    return c.cache.find(sv) != c.cache.end();
}

static const char* InternOutput(std::string&& str) {
    std::string_view sv = str;
    auto& c = GetShardedCache(sv);
    {
        std::shared_lock read_lock(c.mutex);
        if (auto it = c.cache.find(sv); it != c.cache.end()) {
            return it->c_str();
        }
    }
    std::unique_lock write_lock(c.mutex);
    auto [it, inserted] = c.cache.insert(std::move(str));
    return it->c_str();
}

// 判断一个字符串是不是本来就是 Big5（繁体）编码。
// 依据：GB2312 的第二字节一定 >= 0xA1，而 Big5 常用字大量使用 0x40-0x7E；
//       Big5 常用字首字节 0xA4-0xAF 在 GB2312 里不是汉字；
//       GB2312 首字节 0xC7 以上在 Big5 里是罕用字。
static bool LooksLikeBig5(const char* str) noexcept {
    auto* b = reinterpret_cast<const unsigned char*>(str);
    int big5 = 0, gbk = 0;
    for (size_t i = 0; b[i] != 0; ) {
        unsigned char c = b[i];
        unsigned char t = b[i + 1];
        if (c < 0x81 || t == 0) { i++; continue; }
        if ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xA0)) big5++;
        else if (c >= 0xA4 && c <= 0xAF) big5++;
        else if (c >= 0xC7 && c <= 0xF7) gbk++;
        i += 2;
    }
    return big5 > gbk;
}

// 只有在「被我们 hook 的第三方插件」执行期间才 > 0。
// Premiere 内建效果的参数名本来就是系统编码，绝对不能碰。
static thread_local int t_PluginDepth = 0;

struct PluginScope {
    PluginScope() noexcept { ++t_PluginDepth; }
    ~PluginScope() noexcept { --t_PluginDepth; }
};

static bool NeedsFix(const char* s) {
    if (!s || s[0] == '\0') return false;
    if (IsValidUTF8(s)) return false;     // 纯英文或已是 UTF-8
    if (IsKnownOutput(s)) return false;   // 我们已经转过的
    if (g_TargetCP == 950 && LooksLikeBig5(s)) return false; // 本来就是繁体编码
    return true;
}

// ==========================================
// 4. 参数修复与 Hook 逻辑
// ==========================================

inline bool IsBufferWritable(const void* p, size_t len) noexcept {
    if (!p || len == 0) return false;

    auto* base = static_cast<const unsigned char*>(p);
    auto* end = base + len;

    MEMORY_BASIC_INFORMATION mbi{};
    auto* cursor = base;
    while (cursor < end) {
        SIZE_T queried = VirtualQuery(cursor, &mbi, sizeof(mbi));
        if (queried == 0) return false;

        constexpr DWORD writable_mask = PAGE_READWRITE | PAGE_WRITECOPY
                                      | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & writable_mask) == 0) return false;

        auto* regionEnd = static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor) return false;

        cursor = regionEnd;
    }
    return true;
}

static bool FixStringPointer(const char*& ptr) {
    if (!NeedsFix(ptr)) return false;

    std::string out, logU8;
    if (!ConvertFromGBK(ptr, 0, out, logU8)) return false;

    ptr = InternOutput(std::move(out));
    LogLine("fix(ptr): %s", logU8.c_str());
    return true;
}

template <size_t N>
static bool FixStringArray(char(&arr)[N]) {
    // 先确认数组内有结尾符，避免越界读取
    if (memchr(arr, '\0', N) == nullptr) return false;
    if (!NeedsFix(arr)) return false;
    if (!IsBufferWritable(arr, N)) return false;

    std::string out, logU8;
    if (!ConvertFromGBK(arr, N - 1, out, logU8)) return false;

    memcpy(arr, out.data(), out.size());
    arr[out.size()] = '\0';

    InternOutput(std::move(out));
    LogLine("fix(arr): %s", logU8.c_str());
    return true;
}

static bool ProcessAndFixParamImpl(PF_ParamDef* def) {
    bool changed = FixStringArray(def->PF_DEF_NAME);

    switch (def->param_type) {
    case PF_Param_POPUP:
        changed |= FixStringPointer(def->u.pd.u.PF_DEF_NAMESPTR);
        break;

    case PF_Param_CHECKBOX:
        changed |= FixStringPointer(def->u.bd.u.PF_DEF_NAMEPTR);
        break;

    case PF_Param_BUTTON:
        changed |= FixStringPointer(def->u.button_d.u.PF_DEF_NAMESPTR);
        break;

    case PF_Param_SLIDER:
        changed |= FixStringArray(def->u.sd.value_desc);
        break;

    case PF_Param_FIX_SLIDER:
        changed |= FixStringArray(def->u.fd.value_str);
        changed |= FixStringArray(def->u.fd.value_desc);
        break;

    case PF_Param_FLOAT_SLIDER:
        changed |= FixStringArray(def->u.fs_d.value_desc);
        break;

    default:
        break;
    }
    return changed;
}

bool ProcessAndFixParam(PF_ParamDef* def) noexcept {
    if (!def) return false;
    try {
        return ProcessAndFixParamImpl(def);
    }
    catch (...) {
        return false; // 任何异常都不能传回宿主
    }
}

// ==========================================
// 5. 后处理扫描，确保插件修改后参数名称仍然有效
// ==========================================

static int FixAllParamNamesAndCheck(PF_InData* in_data, PF_ParamDef* params[]) noexcept {
    if (!in_data || !params || in_data->num_params <= 0) return 0;

    PF_ParamIndex scan_limit = in_data->num_params;
    if (scan_limit > 10000) scan_limit = 10000;

    int fixed_count = 0;
    for (PF_ParamIndex i = 1; i < scan_limit; i++) {
        if (params[i] && ProcessAndFixParam(params[i])) {
            fixed_count++;
        }
    }
    return fixed_count;
}

inline bool TryCreateAndEnableHook(void* pTarget, LPVOID pDetour, LPVOID* ppOriginal) noexcept {
    if (MH_CreateHook(pTarget, pDetour, ppOriginal) != MH_OK) {
        return false;
    }
    if (MH_EnableHook(pTarget) != MH_OK) {
        MH_DisableHook(pTarget);
        MH_RemoveHook(pTarget);
        return false;
    }
    return true;
}

static std::unordered_set<void*> g_HookedAddParamAddrs;
static PF_Err(*g_OriginalHostAddParam)(PF_ProgPtr, PF_ParamIndex, PF_ParamDef*) = nullptr;
static std::mutex g_AddParamHookMutex;

PF_Err Hooked_HostAddParam(PF_ProgPtr effect_ref, PF_ParamIndex index, PF_ParamDef* def) {
    if (def && t_PluginDepth > 0) ProcessAndFixParam(def);
    return g_OriginalHostAddParam(effect_ref, index, def);
}

void EnsureHostAddParamHooked(void* pTarget) noexcept {
    if (!pTarget) return;

    std::lock_guard<std::mutex> lock(g_AddParamHookMutex);

    if (g_HookedAddParamAddrs.count(pTarget) > 0) {
        return;
    }

    if (TryCreateAndEnableHook(pTarget,
                               reinterpret_cast<LPVOID>(&Hooked_HostAddParam),
                               reinterpret_cast<LPVOID*>(&g_OriginalHostAddParam))) {
        g_HookedAddParamAddrs.insert(pTarget);
        LogLine("host add_param hooked @ %p", pTarget);
    } else {
        LogLine("host add_param hook FAILED @ %p", pTarget);
    }
}

static PF_Err(*g_Original_PF_UpdateParamUI)(PF_ProgPtr, PF_ParamIndex, const PF_ParamDef*) = nullptr;

PF_Err Hooked_PF_UpdateParamUI(PF_ProgPtr effect_ref, PF_ParamIndex param_index, const PF_ParamDef* defP) {
    if (!defP || t_PluginDepth <= 0) return g_Original_PF_UpdateParamUI(effect_ref, param_index, defP);
    
    PF_ParamDef fixed_def = *defP;
    ProcessAndFixParam(&fixed_def);
    return g_Original_PF_UpdateParamUI(effect_ref, param_index, &fixed_def);
}

static bool TryInstallPFUpdateParamUIHook(SPBasicSuite* basic) {
    if (!basic) return false;
    
    __try {
        PF_ParamUtilsSuite3* paramSuite = nullptr;
        A_Err err = basic->AcquireSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, (const void**)&paramSuite);
        if (err != A_Err_NONE || !paramSuite) {
            return false;
        }
        
        if (!paramSuite->PF_UpdateParamUI) {
            basic->ReleaseSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
            return false;
        }
        
        void* target = reinterpret_cast<void*>(paramSuite->PF_UpdateParamUI);
        TryCreateAndEnableHook(target,
                               reinterpret_cast<LPVOID>(&Hooked_PF_UpdateParamUI),
                               reinterpret_cast<LPVOID*>(&g_Original_PF_UpdateParamUI));
        
        basic->ReleaseSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
        return true;
    }
    __except(EXCEPTION_EXECUTE_FHANDLER) {
        return false;
    }
}

template<int ID>
PF_Err Trigger_EffectEntryPoint(
    ULONG_PTR arg0,
    ULONG_PTR arg1,
    ULONG_PTR arg2,
    ULONG_PTR arg3,
    ULONG_PTR arg4,
    ULONG_PTR arg5)
{
    void* original = g_OriginalEntryPoints[ID].load(std::memory_order_acquire);
    if (!original) [[unlikely]] return PF_Err_NONE;

    if (arg0 > 0xFFFF) {
        typedef PF_Err(*AEGP_EntryPoint_t)(struct SPBasicSuite*, A_long, A_long, AEGP_PluginID, AEGP_GlobalRefcon*);
        auto aegp_original = reinterpret_cast<AEGP_EntryPoint_t>(original);
        return aegp_original(
            reinterpret_cast<struct SPBasicSuite*>(arg0),
            static_cast<A_long>(arg1),
            static_cast<A_long>(arg2),
            static_cast<AEGP_PluginID>(arg3),
            reinterpret_cast<AEGP_GlobalRefcon*>(arg4)
        );
    }

    PF_Cmd cmd = static_cast<PF_Cmd>(arg0);
    auto* in_data = reinterpret_cast<PF_InData*>(arg1);
    auto* out_data = reinterpret_cast<PF_OutData*>(arg2);
    auto** params = reinterpret_cast<PF_ParamDef**>(arg3);
    auto* output = reinterpret_cast<PF_LayerDef*>(arg4);
    void* extra = reinterpret_cast<void*>(arg5);

    typedef PF_Err(*EffectEntryPoint_t)(PF_Cmd, PF_InData*, PF_OutData*, PF_ParamDef* [], PF_LayerDef*, void*);
    auto effect_original = reinterpret_cast<EffectEntryPoint_t>(original);

    if (cmd == PF_Cmd_PARAMS_SETUP && in_data) {
        if (in_data->inter.add_param) {
            EnsureHostAddParamHooked(reinterpret_cast<void*>(in_data->inter.add_param));
        }
        if (in_data->pica_basicP && !g_Original_PF_UpdateParamUI) {
            TryInstallPFUpdateParamUIHook(in_data->pica_basicP);
        }
    }

    PluginScope scope; // 标记：现在正在第三方插件里

    PF_Err result = effect_original(cmd, in_data, out_data, params, output, extra);

    if (cmd == PF_Cmd_UPDATE_PARAMS_UI || cmd == PF_Cmd_USER_CHANGED_PARAM) {
        int fixed_count = FixAllParamNamesAndCheck(in_data, params);
        if (fixed_count > 0 && out_data) {
            out_data->out_flags |= PF_OutFlag_REFRESH_UI;
        }
    }

    return result;
}

#define P_ENTRY(n) &Trigger_EffectEntryPoint<n>
typedef PF_Err(*EntryPointFunc_t)(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);

#define P_ENTRY_8(n)   P_ENTRY(n), P_ENTRY(n+1), P_ENTRY(n+2), P_ENTRY(n+3), P_ENTRY(n+4), P_ENTRY(n+5), P_ENTRY(n+6), P_ENTRY(n+7)
#define P_ENTRY_32(n)  P_ENTRY_8(n), P_ENTRY_8(n+8), P_ENTRY_8(n+16), P_ENTRY_8(n+24)
#define P_ENTRY_128(n) P_ENTRY_32(n), P_ENTRY_32(n+32), P_ENTRY_32(n+64), P_ENTRY_32(n+96)
#define P_ENTRY_512(n) P_ENTRY_128(n), P_ENTRY_128(n+128), P_ENTRY_128(n+256), P_ENTRY_128(n+384)
#define P_ENTRY_2048(n) P_ENTRY_512(n), P_ENTRY_512(n+512), P_ENTRY_512(n+1024), P_ENTRY_512(n+1536)

EntryPointFunc_t g_WrapperPool[WRAPPER_POOL_SIZE] = {
    P_ENTRY_2048(0)
};

// 从 PiPL 资源里读出效果插件真正的入口函数名
// （不同厂商的入口名称不一样，不能只认 EffectMain / EntryPointFunc）
struct PiPLScan {
    char names[8][64];
    int count;
};

static BOOL CALLBACK EnumPiPLProc(HMODULE hMod, LPCWSTR type, LPWSTR name, LONG_PTR lParam) {
    auto* scan = reinterpret_cast<PiPLScan*>(lParam);

    HRSRC res = FindResourceW(hMod, name, type);
    if (!res) return TRUE;
    DWORD size = SizeofResource(hMod, res);
    HGLOBAL glob = LoadResource(hMod, res);
    if (!glob) return TRUE;
    auto* p = static_cast<const unsigned char*>(LockResource(glob));
    if (!p || size < 16) return TRUE;

    // Kind 必须是 eFKT（效果）；Windows 上四字码是倒着存的
    bool isEffect = false;
    for (DWORD i = 0; i + 16 <= size; i++) {
        if (memcmp(p + i, "dnik", 4) == 0 && memcmp(p + i + 12, "TKFe", 4) == 0) {
            isEffect = true;
            break;
        }
    }
    if (!isEffect) return TRUE;

    // '8664' = CodeWin64X86：key(4) + id(4) + length(4) + 入口名
    for (DWORD i = 0; i + 13 <= size; i++) {
        if (memcmp(p + i, "4668", 4) != 0) continue;

        DWORD len = 0;
        memcpy(&len, p + i + 8, 4);
        DWORD avail = size - (i + 12);
        if (len == 0 || len > avail) continue;

        const char* str = reinterpret_cast<const char*>(p + i + 12);
        size_t slen = strnlen(str, len);
        if (slen == 0 || slen >= 64) continue;
        if (scan->count >= 8) break;

        memcpy(scan->names[scan->count], str, slen);
        scan->names[scan->count][slen] = '\0';
        scan->count++;
    }
    return TRUE;
}

static std::unordered_set<void*> g_HookedTargets;
static std::wstring g_HostDirLower; // Premiere 主程序所在目录（小写，含结尾反斜线）

// 调用前须持有 g_WrapperMutex
static bool HookOneEntry(void* pTarget) {
    if (!pTarget) return false;
    if (g_HookedTargets.count(pTarget) > 0) return false;

    int currentID = g_WrapperCount.load(std::memory_order_relaxed);
    if (currentID >= WRAPPER_POOL_SIZE) return false;

    void* trampolineBuf = nullptr;
    if (!TryCreateAndEnableHook(pTarget,
                                reinterpret_cast<LPVOID>(g_WrapperPool[currentID]),
                                reinterpret_cast<LPVOID*>(&trampolineBuf))) {
        return false;
    }

    g_OriginalEntryPoints[currentID].store(trampolineBuf, std::memory_order_release);
    g_WrapperCount.fetch_add(1, std::memory_order_release);
    g_HookedTargets.insert(pTarget);

    // 钉住被 hook 的插件：Premiere 之后若卸载再重载它，hook 才不会失效或悬空
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(pTarget), &pinned);
    return true;
}

void HookModuleEntryPoints(HMODULE hMod, const std::wstring& modPath) {
    if (!hMod) return;
    if (hMod == g_SelfModule) return;                       // 不要 hook 自己
    if ((reinterpret_cast<ULONG_PTR>(hMod) & 3) != 0) return; // 以「数据文件」方式加载的，不是可执行模块

    // 以系统回报的完整路径为准（传进来的可能是相对路径）
    std::wstring lowerPath = modPath;
    {
        wchar_t full[MAX_PATH * 2] = {};
        DWORD n = GetModuleFileNameW(hMod, full, MAX_PATH * 2);
        if (n > 0 && n < MAX_PATH * 2) lowerPath.assign(full, n);
    }
    if (lowerPath.empty()) return;

    CharLowerBuffW(lowerPath.data(), static_cast<DWORD>(lowerPath.size()));

    const bool isAex = lowerPath.size() >= 4 &&
                       lowerPath.compare(lowerPath.size() - 4, 4, L".aex") == 0;
    if (!isAex && lowerPath.find(L"\\plug-ins\\") == std::wstring::npos) {
        return;
    }
    // Premiere 自己安装目录里的内建效果：不 hook
    if (!g_HostDirLower.empty() && lowerPath.compare(0, g_HostDirLower.size(), g_HostDirLower) == 0) {
        return;
    }
    // AE 原版 fixer 若被误放进来，不去碰它
    if (lowerPath.find(L"utf8_fixer") != std::wstring::npos) return;

    PiPLScan scan{};
    if (isAex) {
        EnumResourceNamesW(hMod, L"PiPL", EnumPiPLProc, reinterpret_cast<LONG_PTR>(&scan));
    }

    std::lock_guard<std::mutex> lock(g_WrapperMutex);

    int hooked = 0;
    int found = 0;
    for (int i = 0; i < scan.count; i++) {
        void* pTarget = reinterpret_cast<void*>(GetProcAddress(hMod, scan.names[i]));
        if (!pTarget) continue;
        found++;
        if (HookOneEntry(pTarget)) hooked++;
    }

    // 后备：常见的两个入口名
    static const char* const kFallbackNames[] = { "EffectMain", "EntryPointFunc" };
    for (const char* fb : kFallbackNames) {
        void* pTarget = reinterpret_cast<void*>(GetProcAddress(hMod, fb));
        if (!pTarget) continue;
        found++;
        if (HookOneEntry(pTarget)) hooked++;
    }

    if (hooked > 0) {
        LogWidePath("hooked effect:", modPath.c_str());
    } else if (isAex && found == 0) {
        LogWidePath("NO ENTRY FOUND:", modPath.c_str());
    }
}

void EnumerateAndHookLoadedModules() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me;
        me.dwSize = sizeof(me);
        if (Module32FirstW(hSnapshot, &me)) {
            do {
                HookModuleEntryPoints(me.hModule, me.szExePath);
            } while (Module32NextW(hSnapshot, &me));
        }
        CloseHandle(hSnapshot);
    }
}

typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
LoadLibraryW_t Original_LoadLibraryW = nullptr;
LoadLibraryExW_t Original_LoadLibraryExW = nullptr;

HMODULE WINAPI Hooked_LoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE hMod = Original_LoadLibraryW(lpLibFileName);
    if (hMod && lpLibFileName) {
        HookModuleEntryPoints(hMod, lpLibFileName);
    }
    return hMod;
}

HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hMod = Original_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    constexpr DWORD kNotExecutable = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE
                                   | LOAD_LIBRARY_AS_IMAGE_RESOURCE | DONT_RESOLVE_DLL_REFERENCES;
    if (hMod && lpLibFileName && (dwFlags & kNotExecutable) == 0) {
        HookModuleEntryPoints(hMod, lpLibFileName);
    }
    return hMod;
}

#if defined(_MSC_VER)
#define FIXER_EXPORT __declspec(dllexport)
#else
#define FIXER_EXPORT __attribute__((visibility("default")))
#endif

// ==========================================
// 6. Premiere Pro 入口
//    Premiere 不运行 AEGP，所以这里伪装成一个「效果插件」：
//    - PF_Cmd_GLOBAL_SETUP 时安装所有 hook
//    - 通过 PF Cache On Load Suite 要求 Premiere 每次启动都加载本插件
//    - 标记为 OBSOLETE，不会出现在效果面板里
// ==========================================

#define FIXER_VERSION PF_VERSION(1, 0, 0, PF_Stage_DEVELOP, 1)   // = 524289，需与 PiPL 一致

static std::once_flag g_InstallOnce;

static void InstallAllHooks() noexcept {
    // 钉住自身：即使 Premiere 之后卸载本插件，DLL 也留在内存里，hook 不会悬空
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&InstallAllHooks), &pinned);

    if (GetACP() == 950) {
        // 繁体中文 Windows：输出 Big5，并把简体字转成繁体字
        g_TargetCP = 950;
        g_ToTraditional = true;
    }

    {
        wchar_t exePath[MAX_PATH] = {};
        DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring dir(exePath, n);
            size_t slash = dir.find_last_of(L'\\');
            if (slash != std::wstring::npos) {
                dir.resize(slash + 1);
                CharLowerBuffW(dir.data(), static_cast<DWORD>(dir.size()));
                g_HostDirLower = std::move(dir);
            }
        }
    }

    LogReset();
    LogLine("PR UTF8 Fixer 1.2 loaded, ACP=%u, output codepage=%u, to-traditional=%d",
            GetACP(), g_TargetCP, g_ToTraditional ? 1 : 0);
    LogWidePath("host dir:", g_HostDirLower.c_str());

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LogLine("MH_Initialize failed: %d", static_cast<int>(status));
        return;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32) {
        auto* pLLW = reinterpret_cast<void*>(GetProcAddress(hKernel32, "LoadLibraryW"));
        if (pLLW) {
            bool ok = TryCreateAndEnableHook(pLLW,
                                             reinterpret_cast<LPVOID>(&Hooked_LoadLibraryW),
                                             reinterpret_cast<LPVOID*>(&Original_LoadLibraryW));
            LogLine("LoadLibraryW hook: %s", ok ? "ok" : "FAILED");
        }

        auto* pLLEW = reinterpret_cast<void*>(GetProcAddress(hKernel32, "LoadLibraryExW"));
        if (pLLEW) {
            bool ok = TryCreateAndEnableHook(pLLEW,
                                             reinterpret_cast<LPVOID>(&Hooked_LoadLibraryExW),
                                             reinterpret_cast<LPVOID*>(&Original_LoadLibraryExW));
            LogLine("LoadLibraryExW hook: %s", ok ? "ok" : "FAILED");
        }
    }

    EnumerateAndHookLoadedModules();
    LogLine("init done, effects hooked so far: %d", g_WrapperCount.load());
}

static int RequestNoCacheOnLoad(PF_InData* in_data) noexcept {
    __try {
        if (!in_data || !in_data->pica_basicP) return -1;
        SPBasicSuite* basic = in_data->pica_basicP;
        const PF_CacheOnLoadSuite1* suite = nullptr;
        if (basic->AcquireSuite(kPFCacheOnLoadSuite, kPFCacheOnLoadSuiteVersion1,
                                reinterpret_cast<const void**>(&suite)) != 0 || !suite) {
            return -2;
        }
        int result = -3;
        if (suite->PF_SetNoCacheOnLoad) {
            result = static_cast<int>(suite->PF_SetNoCacheOnLoad(in_data->effect_ref, 1));
        }
        basic->ReleaseSuite(kPFCacheOnLoadSuite, kPFCacheOnLoadSuiteVersion1);
        return result;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -9;
    }
}

extern "C" FIXER_EXPORT PF_Err EffectMain(
    PF_Cmd cmd,
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    void* extra)
{
    PF_Err err = PF_Err_NONE;

    switch (cmd) {
    case PF_Cmd_GLOBAL_SETUP:
        if (out_data) {
            out_data->my_version = FIXER_VERSION;
            out_data->out_flags = PF_OutFlag_I_AM_OBSOLETE;
            out_data->out_flags2 = 0;
        }
        // 只在 Premiere Pro（'PrMr'）里工作；AE 请用原版 AE-UTF8-Fixer
        if (in_data && in_data->appl_id == 0x50724D72 /* 'PrMr' */) {
            std::call_once(g_InstallOnce, InstallAllHooks);
            int r = RequestNoCacheOnLoad(in_data);
            LogLine("GLOBAL_SETUP, SetNoCacheOnLoad result: %d (0 = ok)", r);
        }
        break;

    case PF_Cmd_PARAMS_SETUP:
        if (out_data) out_data->num_params = 1; // 只有输入图层
        break;

    case PF_Cmd_RENDER:
        // 万一被套用到素材上：原样输出
        if (in_data && params && params[0] && output) {
            err = PF_COPY(&params[0]->u.ld, output, nullptr, nullptr);
        }
        break;

    default:
        break;
    }

    return err;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_SelfModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        // lpvReserved != nullptr 表示进程正在退出，此时不要再碰 hook
        if (lpvReserved == nullptr) {
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
        }
    }
    return TRUE;
}
