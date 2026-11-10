// =============================================================================
//  ReShade Pre-UI add-on for FSR 3  (AMD FidelityFX Super Resolution 3)
//
//  把 ReShade 的 .fx 效果绘制到「UI 合成之前」的 3D 画面上：
//  UI 不受滤镜影响，效果由游戏自己 Present。
//
//  与 DLSS 版的区别：
//    * 注入点从 NVSDK_NGX_D3D12_EvaluateFeature 换成 FSR 3 的 ffxDispatch。
//    * output 资源在 FfxDispatchDescription 内部，结构体布局随 SDK 版本变化，
//      所以这里不引入 AMD 头文件、也不硬编码偏移，改为运行时自标定：
//      在 desc 里扫描 [width, height, 1, 1] 四元组（即 FfxResourceDescription 的
//      width/height/depth/mipCount），命中后往前推 16 字节即 resource 指针；
//      再用 ReShade 查到的真实 D3D12 描述挑出 scene color。
//
//  实测（《王者荣耀世界》+ amd_fidelityfx_dx12.dll / FFX API 1.1）：
//      desc 开头 16 字节是 { uint64 type=0x10001; void* pNext; }，commandList 在 +16；
//      FfxResource 每个 48 字节，一共 7 个：
//        +24  color           3840x2160 R16G16B16A16_FLOAT
//        +72  depth           3840x2160 R32G32_FLOAT
//        +120 motionVectors   3840x2160 R16G16B16A16_FLOAT
//        +168 transparencyAndComposition  1x1 dummy（未使用）
//        +216 reactive mask   3840x2160 R8_UNORM
//        +264 shadingChange   3840x2160 R8_UNORM
//        +312 output          3840x2160 R16G16B16A16_FLOAT   <-- 目标
//      即 width 字段分别在 40 / 88 / 136 / 232 / 280 / 328 字节。
//      FSR 3.1.4 / FSR 4 之前只有 5 个资源，output 在 +216 —— 所以扫描范围必须
//      覆盖 512 字节，否则会把 R8 的 mask 当成 output（v0.2 的 bug）。
//
//  版本号唯一来源是 CMakeLists.txt 的 project(... VERSION x.y.z)，编译期宏
//  PREUI_VERSION 注入（见下方 #ifndef 兜底）。
//
//  PreUI_FSR.ini（与 .addon64 同目录，可选）：
//      [PreUI]
//      Enabled=1
//      DelayFrames=120
//      Trace=1
//      DumpDesc=1
//      OutputOffset=0
// =============================================================================

#ifndef PREUI_VERSION
#define PREUI_VERSION "0.0.0-dev"
#endif

#include <Windows.h>
#include <TlHelp32.h>
#include <d3d12.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <reshade.hpp>

#include "MinHook.h"

namespace {

using reshade::api::command_list;
using reshade::api::effect_runtime;
using reshade::api::format;
using reshade::api::resource;
using reshade::api::resource_usage;
using reshade::api::resource_view;
using reshade::api::resource_view_desc;

// -----------------------------------------------------------------------------
// 配置与日志
// -----------------------------------------------------------------------------
wchar_t g_base_dir[MAX_PATH] = {};
FILE *g_log_file = nullptr;

bool g_enabled = true;
bool g_trace = true;
bool g_dump_desc = true;
int g_delay_frames = 120;
int g_forced_offset = -1;   // >0 = 跳过自标定，直接用这个偏移（人工校准用）

void init_paths(HMODULE self)
{
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0)
        return;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash != nullptr)
    {
        *slash = L'\0';
        wcscpy_s(g_base_dir, path);
    }
}

// 日志只在异常/节流点写；这里在启动阶段（还没打开日志文件之前）做一次截断，
// 避免历史版本的刷屏日志无限膨胀。
void rotate_log_if_large()
{
    if (g_base_dir[0] == L'\0')
        return;

    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\PreUI-FSR.log", g_base_dir);

    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
        return;

    const ULONGLONG size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size <= 4ull * 1024ull * 1024ull)
        return;

    FILE *file = nullptr;
    if (_wfopen_s(&file, path, L"w") == 0 && file != nullptr)
    {
        fputs("[PreUI-FSR] log rotated (previous log exceeded 4 MiB)\n", file);
        fclose(file);
    }
}

void file_log(const char *fmt, ...)
{
    if (g_log_file == nullptr)
    {
        wchar_t path[MAX_PATH] = {};
        swprintf_s(path, L"%s\\PreUI-FSR.log", g_base_dir);
        _wfopen_s(&g_log_file, path, L"a");
        if (g_log_file == nullptr)
        {
            wchar_t temp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, temp);
            swprintf_s(path, L"%sPreUI-FSR.log", temp);
            _wfopen_s(&g_log_file, path, L"a");
        }
        if (g_log_file == nullptr)
            return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);
    fputc('\n', g_log_file);
    fflush(g_log_file);
}

void log_line(reshade::log::level level, const char *fmt, ...)
{
    char buffer[512] = {};
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    reshade::log::message(level, buffer);
}

void load_config()
{
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\PreUI_FSR.ini", g_base_dir);

    const DWORD attr = GetFileAttributesW(path);
    file_log("[PreUI-FSR] ini: %ls exists=%d", path, (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0);

    g_enabled = GetPrivateProfileIntW(L"PreUI", L"Enabled", 1, path) != 0;
    g_trace = GetPrivateProfileIntW(L"PreUI", L"Trace", 1, path) != 0;
    g_dump_desc = GetPrivateProfileIntW(L"PreUI", L"DumpDesc", 1, path) != 0;
    g_delay_frames = static_cast<int>(GetPrivateProfileIntW(L"PreUI", L"DelayFrames", 120, path));
    if (g_delay_frames < 1)
        g_delay_frames = 1;

    // 人工校准用：-1 / 0 / 缺省 = 自动标定
    const int forced = static_cast<int>(GetPrivateProfileIntW(L"PreUI", L"OutputOffset", 0, path));
    g_forced_offset = (forced > 0 && forced < 4096) ? forced : -1;
}

// -----------------------------------------------------------------------------
// 全局状态
// -----------------------------------------------------------------------------
std::mutex g_runtime_mutex;
effect_runtime *g_runtime = nullptr;
reshade::api::device *g_device = nullptr;   // 只在 g_temp_mutex 下写

std::mutex g_cmd_mutex;
std::unordered_map<uint64_t, command_list *> g_cmd_lists;   // key = api::command_list*

// native(COM) -> api 指针偏移；-1 表示尚未确定
ptrdiff_t g_proxy_offset = -1;

// FfxDispatchDescription 内 width 字段的字节偏移；-1 表示尚未标定。
// 仅渲染线程访问，不需要锁。
int g_output_offset = -1;

// 标定成功时那个 resource 指针。每帧只要 desc[g_output_offset-16] 还是它，
// 就直接用，不做任何额外校验（省掉一次 VirtualQuery）。
const void *g_output_resource = nullptr;

// desc 内 commandList 的字节偏移；-1 表示尚未探测。
// 老 API（FSR 3.1.0~3.1.2）在第 0 字节；
// 新 FFX API（FSR 3.1.3+/FSR4，amd_fidelityfx_dx12.dll）前面有 16 字节头
// { uint64 type; void* pNext; }，commandList 在第 16 字节。
int g_cmd_desc_offset = -1;

struct temp_target
{
    resource texture{};
    resource_view rtv{};
    resource_view rtv_srgb{};
    uint32_t width = 0;
    uint32_t height = 0;
    format fmt = format::unknown;
    bool ready = false;
};

temp_target g_temp;
std::mutex g_temp_mutex;

// FfxErrorCode ffxDispatch(void *context, const void *description)
using ffx_dispatch_fn = int32_t (*)(void *, const void *);

constexpr int32_t kFfxOk = 0;
constexpr int kMaxHooks = 4;

ffx_dispatch_fn g_originals[kMaxHooks] = {};
int g_slot_count = 0;

std::mutex g_target_mutex;
std::unordered_set<uint64_t> g_hooked_targets;

volatile LONG g_present_count = 0;
volatile LONG g_inject_ok = 0;
volatile LONG g_frame_injected = 0;
volatile LONG g_hook_calls = 0;
volatile LONG g_trace_count = 0;
volatile LONG g_reset_count = 0;
volatile LONG g_hooks_installed = 0;    // 成功 MH_EnableHook 的次数
volatile LONG g_last_hook_frame = 0;    // 最近一次挂上钩子时的 present 帧号
volatile LONG g_disabled_count = 0;     // 效果关闭而跳过注入的帧数（只打一次日志）
volatile LONG g_probe_fail = 0;         // 标定/探针事件计数（只用来限制日志条数）

thread_local int t_call_depth = 0;

void trace(const char *msg)
{
    if (!g_trace)
        return;
    if (InterlockedIncrement(&g_trace_count) <= 60)
        file_log("[PreUI-FSR] trace: %s", msg);
}

// -----------------------------------------------------------------------------
// 内存探针
//   我们只读「调用方传进来的 desc 结构体」和其中已经判定为可读的指针，
//   绝不假设 AMD 结构体的版本。任何越界读都由 __try 兜住。
// -----------------------------------------------------------------------------
bool is_readable(const void *p)
{
    if (p == nullptr)
        return false;
    if ((reinterpret_cast<uintptr_t>(p) & 7u) != 0)
        return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if ((mbi.Protect & PAGE_NOACCESS) != 0 || (mbi.Protect & PAGE_GUARD) != 0)
        return false;
    return true;
}

uint32_t read_u32(const uint8_t *p)
{
    uint32_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t read_u64(const uint8_t *p)
{
    uint64_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

// 比 is_readable 更严格：还要求该地址的头 8 字节（vtable 指针）也可读。
// 用来把「看着像指针的随机值」挡在 get_resource_desc 之前。
bool is_probable_com_object(const void *p)
{
    if (!is_readable(p))
        return false;

    __try
    {
        const void *vtable = nullptr;
        memcpy(&vtable, p, sizeof(vtable));
        return vtable != nullptr && is_readable(vtable);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct desc_candidate
{
    int offset = -1;            // width 字段的字节偏移
    const void *resource = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

// FfxResource 的布局（各版本一致）：
//   void *resource;                 // +0
//   FfxResourceDescription desc;    // +8
//     uint32 type;                  // +8
//     uint32 format;                // +12
//     uint32 width;                 // +16   <-- 我们定位这个
//     uint32 height;                // +20
//     uint32 depth;                 // +24   (== 1)
//     uint32 mipCount;              // +28   (== 1)
//     uint32 flags;                 // +32
// 所以 width 在 i 处时，resource 在 i-16。
//
// 这里只负责「把所有长得像 resource 的都收集起来」，
// 具体哪个是 output 交给调用方用真实的 D3D12 资源描述来判断。
void collect_candidates_impl(const uint8_t *base, size_t size,
                             desc_candidate *out, int max, int *count)
{
    for (size_t i = 16; i + 24 <= size; i += 4)
    {
        const uint32_t w = read_u32(base + i);
        const uint32_t h = read_u32(base + i + 4);
        const uint32_t d = read_u32(base + i + 8);
        const uint32_t m = read_u32(base + i + 12);

        // depth 对 2D 纹理是 1（个别版本给 0），mipCount 恒为 1
        if (d > 1 || m != 1)
            continue;
        if (w < 320 || h < 200 || w > 16384 || h > 16384)
            continue;
        if ((w & 1u) != 0)
            continue;

        const void *candidate = nullptr;
        memcpy(&candidate, base + i - 16, sizeof(candidate));
        if (!is_probable_com_object(candidate))
            continue;

        bool duplicate = false;
        for (int k = 0; k < *count; ++k)
        {
            if (out[k].offset == static_cast<int>(i))
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;

        if (*count >= max)
            return;

        out[*count].offset = static_cast<int>(i);
        out[*count].resource = candidate;
        out[*count].width = w;
        out[*count].height = h;
        ++(*count);
    }
}

bool collect_candidates(const void *desc, size_t size,
                        desc_candidate *out, int max, int *count)
{
    __try
    {
        collect_candidates_impl(reinterpret_cast<const uint8_t *>(desc), size, out, max, count);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return *count > 0;
    }
}

void dump_description_impl(const void *desc, size_t size)
{
    const uint8_t *base = reinterpret_cast<const uint8_t *>(desc);
    file_log("[PreUI-FSR] ---- FfxDispatchDescription dump (%zu bytes) ----", size);

    for (size_t off = 0; off < size; off += 32)
    {
        char line[256] = {};
        int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "+%03zu:", off);
        if (len < 0)
            continue;

        for (size_t i = 0; i < 32 && off + i < size; i += 8)
        {
            uint64_t qword = 0;
            memcpy(&qword, base + off + i, sizeof(qword));
            const int n = _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %016llX",
                                      static_cast<unsigned long long>(qword));
            if (n < 0)
                break;
            len += n;
        }
        file_log("[PreUI-FSR] %s", line);
    }

    for (size_t off = 0; off < size; off += 32)
    {
        char line[256] = {};
        int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "b+%03zu:", off);
        if (len < 0)
            continue;

        for (size_t i = 0; i < 32 && off + i < size; ++i)
        {
            const int n = _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %02X", base[off + i]);
            if (n < 0)
                break;
            len += n;
        }
        file_log("[PreUI-FSR] %s", line);
    }
}

// desc 的实际大小取决于游戏用的 SDK 版本，读超界也不能把游戏带崩。
void dump_description(const void *desc, size_t size)
{
    __try
    {
        dump_description_impl(desc, size);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        file_log("[PreUI-FSR] desc dump aborted (structure smaller than %zu bytes)", size);
    }
}

// -----------------------------------------------------------------------------
// 命令列表记录（按 api 指针）
// -----------------------------------------------------------------------------
void remember_command_list(command_list *cmd_list)
{
    if (cmd_list == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_cmd_mutex);
    g_cmd_lists[reinterpret_cast<uint64_t>(cmd_list)] = cmd_list;
}

void on_init_command_list(command_list *cmd_list) { remember_command_list(cmd_list); }

void on_reset_command_list(command_list *cmd_list)
{
    const LONG n = InterlockedIncrement(&g_reset_count);
    if (n == 1)
        file_log("[PreUI-FSR] first reset_command_list api=%p", cmd_list);
    remember_command_list(cmd_list);
}

void on_close_command_list(command_list *cmd_list) { remember_command_list(cmd_list); }
void on_execute_command_list(reshade::api::command_queue *, command_list *cmd_list) { remember_command_list(cmd_list); }

command_list *resolve_command_list(ID3D12GraphicsCommandList *native, ptrdiff_t *out_offset)
{
    if (native == nullptr)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_cmd_mutex);

    if (g_proxy_offset >= 0)
    {
        auto *candidate = reinterpret_cast<command_list *>(
            reinterpret_cast<char *>(native) + g_proxy_offset);
        const auto it = g_cmd_lists.find(reinterpret_cast<uint64_t>(candidate));
        if (it != g_cmd_lists.end())
        {
            if (out_offset != nullptr)
                *out_offset = g_proxy_offset;
            return it->second;
        }
    }

    for (int offset = 0; offset <= 256; offset += 8)
    {
        auto *candidate = reinterpret_cast<command_list *>(
            reinterpret_cast<char *>(native) + offset);
        const auto it = g_cmd_lists.find(reinterpret_cast<uint64_t>(candidate));
        if (it == g_cmd_lists.end())
            continue;

        if (g_proxy_offset != offset)
        {
            g_proxy_offset = offset;
            file_log("[PreUI-FSR] proxy offset resolved: %d (native=%p)", offset, native);
        }
        if (out_offset != nullptr)
            *out_offset = g_proxy_offset;
        return it->second;
    }

    if (out_offset != nullptr)
        *out_offset = g_proxy_offset;
    return nullptr;
}

// 从 desc 里找出 commandList，并解析成 ReShade 的 api::command_list。
// 两种布局都试（第 0 / 16 字节），靠「能不能在命令列表表里查到」来判断，
// 这样不依赖 AMD 的版本。
command_list *resolve_command_list_from_desc(const void *desc, ptrdiff_t *out_proxy)
{
    const uint8_t *base = reinterpret_cast<const uint8_t *>(desc);

    if (g_cmd_desc_offset >= 0)
    {
        void *candidate = nullptr;
        memcpy(&candidate, base + g_cmd_desc_offset, sizeof(candidate));
        if (is_probable_com_object(candidate))
        {
            command_list *resolved = resolve_command_list(
                static_cast<ID3D12GraphicsCommandList *>(candidate), out_proxy);
            if (resolved != nullptr)
                return resolved;
        }
    }

    for (int offset = 0; offset <= 24; offset += 8)
    {
        void *candidate = nullptr;
        memcpy(&candidate, base + offset, sizeof(candidate));
        if (!is_probable_com_object(candidate))
            continue;

        command_list *resolved = resolve_command_list(
            static_cast<ID3D12GraphicsCommandList *>(candidate), out_proxy);
        if (resolved != nullptr)
        {
            if (g_cmd_desc_offset != offset)
            {
                g_cmd_desc_offset = offset;
                file_log("[PreUI-FSR] commandList offset resolved: %d", offset);
            }
            return resolved;
        }
    }

    return nullptr;
}

// -----------------------------------------------------------------------------
// 事件：运行时
// -----------------------------------------------------------------------------
void on_init_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (g_runtime == nullptr)
        g_runtime = runtime;
    file_log("[PreUI-FSR] effect runtime created");
    log_line(reshade::log::level::info, "[PreUI-FSR] effect runtime created");
}

void destroy_temp(reshade::api::device *device)
{
    if (device == nullptr || !g_temp.ready)
        return;

    if (g_temp.rtv_srgb.handle != 0 && g_temp.rtv_srgb.handle != g_temp.rtv.handle)
        device->destroy_resource_view(g_temp.rtv_srgb);
    if (g_temp.rtv.handle != 0)
        device->destroy_resource_view(g_temp.rtv);
    if (g_temp.texture.handle != 0)
        device->destroy_resource(g_temp.texture);

    g_temp = {};
}

void on_destroy_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (g_runtime != runtime)
        return;
    g_runtime = nullptr;

    std::lock_guard<std::mutex> temp_lock(g_temp_mutex);
    destroy_temp(g_device);
}

// -----------------------------------------------------------------------------
// 临时纹理
// -----------------------------------------------------------------------------
bool ensure_temp(reshade::api::device *device, uint32_t width, uint32_t height, format fmt)
{
    if (width == 0 || height == 0)
        return false;

    if (g_temp.ready && g_temp.width == width && g_temp.height == height && g_temp.fmt == fmt)
        return true;

    destroy_temp(device);

    const reshade::api::resource_desc desc(
        width, height, 1, 1, fmt, 1,
        reshade::api::memory_heap::default_,
        resource_usage::render_target | resource_usage::copy_source | resource_usage::copy_dest);

    resource texture{};
    if (!device->create_resource(desc, nullptr, resource_usage::general, &texture))
    {
        file_log("[PreUI-FSR] create_resource(%ux%u) failed", width, height);
        return false;
    }

    resource_view rtv{};
    if (!device->create_resource_view(texture, resource_usage::render_target, resource_view_desc(fmt), &rtv))
    {
        device->destroy_resource(texture);
        file_log("[PreUI-FSR] create RTV failed");
        return false;
    }

    resource_view rtv_srgb = rtv;
    const format srgb_fmt = reshade::api::format_to_default_typed(fmt, 1);
    if (srgb_fmt != fmt)
        device->create_resource_view(texture, resource_usage::render_target, resource_view_desc(srgb_fmt), &rtv_srgb);

    g_temp.texture = texture;
    g_temp.rtv = rtv;
    g_temp.rtv_srgb = rtv_srgb;
    g_temp.width = width;
    g_temp.height = height;
    g_temp.fmt = fmt;
    g_temp.ready = true;
    file_log("[PreUI-FSR] temp target created %ux%u fmt=%d", width, height, static_cast<int>(fmt));
    return true;
}

// -----------------------------------------------------------------------------
// 解析 desc 里的 output 资源
//
//   光看 desc 里的 uint32 猜不出哪个是 output：
//   FSR 3.1.4/FSR4 的 dispatch desc 里依次是
//     color / depth / motionVectors / transparencyAndComposition /
//     reactive / shadingChange / output
//   一共 7 个 FfxResource，尺寸可能全是显示分辨率，其中 reactive / shadingChange
//   就是 R8 的单通道 mask。v0.2 只扫 256 字节、把第 5 个（mask）当成了 output，
//   所以注入毫无效果。
//
//   现在把候选全收集起来，用 ReShade 查每个候选真实的 D3D12 描述，再按
//   「颜色目标的特征」挑：
//     * 必须是 texture2d、非 MSAA
//     * 每像素 >= 32 bit（排除 R8/R16 mask）
//     * 排除 depth_stencil
//     * 尺寸等于交换链分辨率加分
//     * 位深越大越优先
//     * 同分取 desc 里最靠后的 —— output 是最后一个 FfxResource
// -----------------------------------------------------------------------------
bool resolve_output(const void *desc, effect_runtime *runtime, reshade::api::device *device,
                    const void **out_resource)
{
    const uint8_t *base = reinterpret_cast<const uint8_t *>(desc);

    // 人工指定了偏移：不做扫描，直接读，读不到就这帧跳过（避免每帧重复扫描）
    if (g_forced_offset >= 16)
    {
        const void *forced = nullptr;
        memcpy(&forced, base + g_forced_offset - 16, sizeof(forced));
        if (is_probable_com_object(forced))
        {
            *out_resource = forced;
            return true;
        }
        if (InterlockedIncrement(&g_probe_fail) <= 3)
            file_log("[PreUI-FSR] forced OutputOffset=%d yields unreadable pointer, check the ini",
                     g_forced_offset);
        return false;
    }

    // 已标定：绝大多数帧走到这里，只做一次比较，不做 VirtualQuery。
    if (g_output_offset >= 16)
    {
        const void *cached = nullptr;
        memcpy(&cached, base + g_output_offset - 16, sizeof(cached));

        if (cached != nullptr && cached == g_output_resource)
        {
            *out_resource = cached;
            return true;
        }

        // 同一位置换了纹理（最常见的原因是改分辨率）。只校验一次，不重扫。
        if (is_probable_com_object(cached))
        {
            file_log("[PreUI-FSR] output resource changed at offset %d (new ptr=%p)",
                     g_output_offset, cached);
            g_output_resource = cached;
            *out_resource = cached;
            return true;
        }

        file_log("[PreUI-FSR] cached output offset %d went stale, rescanning", g_output_offset);
        g_output_offset = -1;
        g_output_resource = nullptr;
    }

    uint32_t want_w = 0;
    uint32_t want_h = 0;
    runtime->get_screenshot_width_and_height(&want_w, &want_h);

    const bool verbose = InterlockedIncrement(&g_probe_fail) <= 3;

    // 注意：FSR 3.1.4 / FSR 4 在 output 之前又插了 reactive / shadingChange 两张 mask，
    // 结构体比 256 字节长得多（7 个 FfxResource ≈ 24 + 7*48 = 360 字节）。
    // 只扫 256 会错把 R8 的 mask 当成 output —— v0.2 就是这么翻车的。
    desc_candidate candidates[12] = {};
    int count = 0;
    collect_candidates(desc, 512, candidates, 12, &count);

    if (verbose)
    {
        file_log("[PreUI-FSR] desc type=0x%llX swapchain=%ux%u candidates=%d",
                 static_cast<unsigned long long>(read_u64(reinterpret_cast<const uint8_t *>(desc))),
                 want_w, want_h, count);
        if (g_dump_desc)
            dump_description(desc, 512);
    }

    int best = -1;
    int best_score = -1;
    int last_any = -1;      // 万一所有候选都不合格，退回「最后一个 2D 纹理」
    reshade::api::resource_desc best_info = {};

    for (int k = 0; k < count; ++k)
    {
        const resource candidate{reinterpret_cast<uint64_t>(candidates[k].resource)};
        const reshade::api::resource_desc info = device->get_resource_desc(candidate);

        uint32_t bpp = 0;
        if (info.type == reshade::api::resource_type::texture_2d)
            bpp = reshade::api::format_row_pitch(info.texture.format, 1) * 8;

        if (verbose)
        {
            file_log("[PreUI-FSR] cand[%d] off=%d desc=%ux%u -> %ux%u fmt=%d bpp=%u samples=%u",
                     k, candidates[k].offset, candidates[k].width, candidates[k].height,
                     info.texture.width, info.texture.height,
                     static_cast<int>(info.texture.format), bpp, info.texture.samples);
        }

        if (info.type != reshade::api::resource_type::texture_2d || info.texture.samples > 1)
            continue;
        // 深度/模板缓冲不是「UI 之前的场景色」
        if ((static_cast<uint32_t>(info.usage) & static_cast<uint32_t>(resource_usage::depth_stencil)) != 0)
            continue;

        last_any = k;   // 兜底用：最后一个非深度的 2D 纹理（不要求位深）

        if (bpp < 32)
            continue;

        int score = static_cast<int>(bpp / 8);      // 64bpp HDR = 8 分
        if ((static_cast<uint32_t>(info.usage) & static_cast<uint32_t>(resource_usage::render_target)) != 0)
            score += 100;                           // 可作渲染目标
        if ((static_cast<uint32_t>(info.usage) & static_cast<uint32_t>(resource_usage::unordered_access)) != 0)
            score += 100;                           // FSR 用 UAV 写输出
        if (info.texture.width == want_w && info.texture.height == want_h)
            score += 1000;
        // desc 里写的尺寸和真实资源对不上 → 这个指针多半不是这块资源，重罚
        if (info.texture.width != candidates[k].width || info.texture.height != candidates[k].height)
            score -= 500;
        score += candidates[k].offset / 8;          // output 是 desc 里最后一个 FfxResource

        if (score > best_score)
        {
            best_score = score;
            best = k;
            best_info = info;
        }
    }

    if (best < 0)
    {
        // 一个都不合格（可能是新的 mask 布局 / 8bit 输出）：退回最后一个合格的 2D 纹理，
        // 至少保证有画面输出，并把情况写进日志。
        if (last_any >= 0)
        {
            best = last_any;
            best_info = device->get_resource_desc(
                resource{reinterpret_cast<uint64_t>(candidates[best].resource)});
            file_log("[PreUI-FSR] no ideal color target, falling back to last 2D candidate (off=%d)",
                     candidates[best].offset);
        }
        else
        {
            if (verbose)
                file_log("[PreUI-FSR] no usable color target among %d candidates", count);
            return false;
        }
    }

    g_output_offset = candidates[best].offset;
    g_output_resource = candidates[best].resource;
    file_log("[PreUI-FSR] output resolved: off=%d ptr=%p %ux%u fmt=%d",
             g_output_offset, candidates[best].resource,
             best_info.texture.width, best_info.texture.height,
             static_cast<int>(best_info.texture.format));
    *out_resource = candidates[best].resource;
    return true;
}

// -----------------------------------------------------------------------------
// 核心注入
//   风险操作（指针探测、get_resource_desc）全部放在加锁之前，
//   避免 SEH 捕获到异常时锁对象没被析构导致死锁。
// -----------------------------------------------------------------------------
void inject_pre_ui(const void *desc)
{
    effect_runtime *runtime = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        runtime = g_runtime;
    }
    if (runtime == nullptr)
    {
        trace("no runtime");
        return;
    }
    if (!runtime->get_effects_state())
    {
        if (InterlockedIncrement(&g_disabled_count) == 1)
            file_log("[PreUI-FSR] effects are disabled, skipping injection until re-enabled");
        return;
    }

    // 新 FFX API 的 dispatch desc 以 { uint64 type; void* pNext; } 开头，
    // 本作实测 type=0x10001 就是升频。按「高 16 位」判断类别：
    //   0x0001 = upscale、0x0002 = framegeneration、0x0003 = denoise
    // 只有升频那次的 output 才是「UI 之前的场景色」，其它两次注入会重复叠加。
    // 老布局下第 0 字节是 commandList 指针（堆地址高 16 位远大于 3），所以两种布局都安全。
    const uint64_t desc_type = read_u64(reinterpret_cast<const uint8_t *>(desc));
    const uint64_t desc_family = desc_type >> 16;
    if (desc_family == 0x0002ull || desc_family == 0x0003ull)
    {
        if (InterlockedIncrement(&g_probe_fail) <= 3)
            file_log("[PreUI-FSR] skip non-upscale dispatch (desc type=0x%llX)",
                     static_cast<unsigned long long>(desc_type));
        return;
    }

    reshade::api::device *device = runtime->get_command_queue()->get_device();
    if (device == nullptr)
    {
        trace("no device");
        return;
    }

    const void *output_raw = nullptr;
    if (!resolve_output(desc, runtime, device, &output_raw))
    {
        trace("output not found in description");
        return;
    }

    ptrdiff_t proxy_offset = -1;
    command_list *cmd_list = resolve_command_list_from_desc(desc, &proxy_offset);
    if (cmd_list == nullptr)
    {
        char msg[200];
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(g_cmd_mutex);
            count = g_cmd_lists.size();
        }
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "no cmd list in desc (cmd offset=%d proxy=%td registered=%zu reset=%ld)",
                    g_cmd_desc_offset, proxy_offset, count, g_reset_count);
        trace(msg);
        return;
    }

    const resource target{reinterpret_cast<uint64_t>(output_raw)};
    const reshade::api::resource_desc target_desc = device->get_resource_desc(target);
    if (target_desc.type != reshade::api::resource_type::texture_2d || target_desc.texture.samples > 1)
    {
        if (InterlockedIncrement(&g_probe_fail) <= 3)
        {
            char msg[160];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE, "bad output desc type=%d samples=%u",
                        static_cast<int>(target_desc.type), target_desc.texture.samples);
            file_log("[PreUI-FSR] %s (ptr=%p)", msg, output_raw);
        }
        g_output_offset = -1;   // 可能标定错了，下一帧重扫
        g_output_resource = nullptr;
        return;
    }

    const format linear_fmt = reshade::api::format_to_default_typed(target_desc.texture.format, 0);
    if (linear_fmt == format::unknown)
    {
        trace("unknown format");
        return;
    }

    std::lock_guard<std::mutex> temp_lock(g_temp_mutex);
    g_device = device;
    if (!ensure_temp(device, target_desc.texture.width, target_desc.texture.height, linear_fmt))
    {
        trace("ensure_temp failed");
        return;
    }

    if (InterlockedCompareExchange(&g_frame_injected, 1, 0) != 0)
    {
        trace("already injected this frame");
        return;
    }

    const resource_usage state_general = resource_usage::general;
    const resource_usage state_copy_src = resource_usage::copy_source;
    const resource_usage state_copy_dst = resource_usage::copy_dest;
    const resource_usage state_rt = resource_usage::render_target;

    cmd_list->barrier(1, &target, &state_general, &state_copy_src);
    cmd_list->barrier(1, &g_temp.texture, &state_general, &state_copy_dst);
    cmd_list->copy_texture_region(target, 0, nullptr, g_temp.texture, 0, nullptr);

    cmd_list->barrier(1, &g_temp.texture, &state_copy_dst, &state_rt);
    runtime->render_effects(cmd_list, g_temp.rtv, g_temp.rtv_srgb);

    cmd_list->barrier(1, &g_temp.texture, &state_rt, &state_copy_src);
    cmd_list->barrier(1, &target, &state_copy_src, &state_copy_dst);
    cmd_list->copy_texture_region(g_temp.texture, 0, nullptr, target, 0, nullptr);

    cmd_list->barrier(1, &g_temp.texture, &state_copy_src, &state_general);
    cmd_list->barrier(1, &target, &state_copy_dst, &state_general);

    const LONG count = InterlockedIncrement(&g_inject_ok);
    if (count == 1 || (count % 600) == 0)
    {
        file_log("[PreUI-FSR] injected %ld frames (%ux%u fmt=%d)",
                 count, target_desc.texture.width, target_desc.texture.height,
                 static_cast<int>(linear_fmt));
        log_line(reshade::log::level::info, "[PreUI-FSR] injected %ld frames", count);
    }
}

// -----------------------------------------------------------------------------
// 多层 hook：只在最外层（depth==1）注入
// -----------------------------------------------------------------------------
template <int I>
int32_t hook_thunk(void *context, const void *desc)
{
    ++t_call_depth;

    const LONG call = InterlockedIncrement(&g_hook_calls);
    if (call <= 10)
        file_log("[PreUI-FSR] slot[%d] entered (call=%ld depth=%d)", I, call, t_call_depth);

    const int32_t result = g_originals[I](context, desc);

    if (result == kFfxOk && t_call_depth == 1 && desc != nullptr && g_enabled)
    {
        __try
        {
            inject_pre_ui(desc);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            file_log("[PreUI-FSR] exception during injection (slot %d)", I);
        }
    }

    --t_call_depth;
    return result;
}

LPVOID thunk_for(int index)
{
    static const LPVOID table[kMaxHooks] = {
        reinterpret_cast<LPVOID>(&hook_thunk<0>),
        reinterpret_cast<LPVOID>(&hook_thunk<1>),
        reinterpret_cast<LPVOID>(&hook_thunk<2>),
        reinterpret_cast<LPVOID>(&hook_thunk<3>),
    };
    return (index >= 0 && index < kMaxHooks) ? table[index] : nullptr;
}

// 大小写不敏感的子串查找
bool wcontains_ci(const wchar_t *hay, const wchar_t *needle)
{
    const size_t n = wcslen(needle);
    if (n == 0)
        return true;
    for (const wchar_t *p = hay; *p != L'\0'; ++p)
    {
        if (_wcsnicmp(p, needle, n) == 0)
            return true;
    }
    return false;
}

// 帧生成 / 降噪专用的 DLL 直接跳过：它们的 dispatch 输出是插值帧或降噪结果，
// 不是「UI 之前的场景色」，勾上去会让滤镜叠两次。
// 注意：FSR 3.1.3+/FSR4 把三个功能合并进了同一个 amd_fidelityfx_dx12.dll，
// 这时靠 dispatch desc 里的 type 字段区分（见 inject_pre_ui）。
bool is_framegen_module(const wchar_t *name)
{
    return wcontains_ci(name, L"framegeneration") || wcontains_ci(name, L"denoiser");
}

// 诊断用：模块名里含这些关键字就记一笔（第一次扫描时输出）
bool looks_relevant(const wchar_t *name)
{
    static const wchar_t *needles[] = {L"fsr", L"ffx", L"fidelityfx", L"upscal", L"amd", L"nvngx", L"dlss"};
    for (const wchar_t *needle : needles)
    {
        if (wcontains_ci(name, needle))
            return true;
    }
    return false;
}

void hook_target(void *target, const wchar_t *module_name, const wchar_t *module_path)
{
    if (target == nullptr)
        return;

    {
        std::lock_guard<std::mutex> lock(g_target_mutex);
        if (g_hooked_targets.count(reinterpret_cast<uint64_t>(target)) != 0)
            return;
        if (g_slot_count >= kMaxHooks)
        {
            file_log("[PreUI-FSR] no free hook slot");
            return;
        }
    }

    int slot = 0;
    {
        std::lock_guard<std::mutex> lock(g_target_mutex);
        slot = g_slot_count++;
    }

    file_log("[PreUI-FSR] target %ls!ffxDispatch -> %p (%ls)", module_name, target, module_path);

    MH_STATUS status = MH_CreateHook(target, thunk_for(slot),
                                     reinterpret_cast<LPVOID *>(&g_originals[slot]));
    if (status != MH_OK)
    {
        file_log("[PreUI-FSR] MH_CreateHook failed: %d", static_cast<int>(status));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_target_mutex);
        g_hooked_targets.insert(reinterpret_cast<uint64_t>(target));
    }

    status = MH_EnableHook(target);
    file_log("[PreUI-FSR] MH_EnableHook -> %d", static_cast<int>(status));
    if (status == MH_OK)
    {
        InterlockedIncrement(&g_hooks_installed);
        InterlockedExchange(&g_last_hook_frame, g_present_count);
    }
}

void scan_and_hook(bool dump_census)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    // 游戏 exe 里也可能导出一份 ffxDispatch（静态链接 FFX 时）。
    // 优先用 DLL 那份：patch 游戏本体风险更高，而且实测游戏调用的是 DLL 的。
    // 所以先把 exe 记下来，整个列表扫完、一个都没挂上时才退回去试它。
    HMODULE exe_module = nullptr;
    wchar_t exe_name[MAX_MODULE_NAME32 + 1] = {};
    wchar_t exe_path[MAX_PATH] = {};

    for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
    {
        const bool interesting = looks_relevant(entry.szModule);
        if (dump_census && interesting)
            file_log("[PreUI-FSR] module: %ls", entry.szExePath);

        if (is_framegen_module(entry.szModule))
            continue;

        if (wcontains_ci(entry.szModule, L".exe"))
        {
            exe_module = entry.hModule;
            wcscpy_s(exe_name, entry.szModule);
            wcscpy_s(exe_path, entry.szExePath);
            if (dump_census && interesting)
                file_log("[PreUI-FSR]   (exe, will only be used as fallback)");
            continue;
        }

        // 按导出找，不按文件名找：FSR 3.1.3+/FSR4 用的是统一库
        // amd_fidelityfx_dx12.dll，名字里没有 "upscaler"。
        void *target = reinterpret_cast<void *>(GetProcAddress(entry.hModule, "ffxDispatch"));
        if (target == nullptr)
        {
            if (dump_census && interesting)
                file_log("[PreUI-FSR]   no ffxDispatch export");
            continue;
        }

        if (dump_census && interesting)
            file_log("[PreUI-FSR]   exports ffxDispatch");

        hook_target(target, entry.szModule, entry.szExePath);
    }

    CloseHandle(snapshot);

    if (exe_module != nullptr && InterlockedCompareExchange(&g_hooks_installed, 0, 0) == 0)
    {
        void *target = reinterpret_cast<void *>(GetProcAddress(exe_module, "ffxDispatch"));
        if (target != nullptr)
        {
            file_log("[PreUI-FSR] no DLL provided ffxDispatch, falling back to the game exe");
            hook_target(target, exe_name, exe_path);
        }
    }
}

void on_reshade_present(effect_runtime *runtime)
{
    (void)runtime;

    InterlockedExchange(&g_frame_injected, 0);

    if (!g_enabled)
        return;

    const LONG frame = InterlockedIncrement(&g_present_count);
    if (frame < g_delay_frames)
    {
        if (frame == 1)
            file_log("[PreUI-FSR] delaying hook for %d frames", g_delay_frames);
        return;
    }

    if (frame != g_delay_frames && (frame % 60) != 0)
        return;

    // 钩子挂上之后降频到每 3600 帧（约 1 分钟）扫一次；
    // 一直没挂上就维持每 60 帧，直到挂上为止。
    if (InterlockedCompareExchange(&g_hooks_installed, 0, 0) != 0)
    {
        const LONG hooked_at = InterlockedCompareExchange(&g_last_hook_frame, 0, 0);
        if (frame > hooked_at + 3600 && (frame % 3600) != 0)
            return;
    }

    __try
    {
        scan_and_hook(frame == g_delay_frames);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        file_log("[PreUI-FSR] exception while scanning modules");
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Add-on 入口
// -----------------------------------------------------------------------------
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    init_paths(addon_module);
    rotate_log_if_large();
    file_log("[PreUI-FSR] ===== AddonInit v%s =====", PREUI_VERSION);
    file_log("[PreUI-FSR] module dir: %ls", g_base_dir);
    load_config();
    file_log("[PreUI-FSR] enabled=%d delay=%d trace=%d dump=%d",
             g_enabled ? 1 : 0, g_delay_frames, g_trace ? 1 : 0, g_dump_desc ? 1 : 0);

    if (!reshade::register_addon(addon_module, reshade_module))
    {
        file_log("[PreUI-FSR] register_addon failed");
        return false;
    }

    if (g_enabled)
    {
        reshade::register_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&on_destroy_effect_runtime);
        reshade::register_event<reshade::addon_event::init_command_list>(&on_init_command_list);
        reshade::register_event<reshade::addon_event::reset_command_list>(&on_reset_command_list);
        reshade::register_event<reshade::addon_event::close_command_list>(&on_close_command_list);
        reshade::register_event<reshade::addon_event::execute_command_list>(&on_execute_command_list);
        reshade::register_event<reshade::addon_event::reshade_present>(&on_reshade_present);

        const MH_STATUS status = MH_Initialize();
        file_log("[PreUI-FSR] MH_Initialize -> %d", static_cast<int>(status));
    }

    log_line(reshade::log::level::info, "[PreUI-FSR] add-on loaded (enabled=%d)", g_enabled ? 1 : 0);
    file_log("[PreUI-FSR] AddonInit done");
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
    file_log("[PreUI-FSR] ===== AddonUninit =====");

    if (g_slot_count > 0)
    {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_slot_count = 0;
    }

    if (g_enabled)
    {
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&on_destroy_effect_runtime);
        reshade::unregister_event<reshade::addon_event::init_command_list>(&on_init_command_list);
        reshade::unregister_event<reshade::addon_event::reset_command_list>(&on_reset_command_list);
        reshade::unregister_event<reshade::addon_event::close_command_list>(&on_close_command_list);
        reshade::unregister_event<reshade::addon_event::execute_command_list>(&on_execute_command_list);
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
    }

    reshade::unregister_addon(addon_module, reshade_module);
}
