#include "gles_capture.hpp"
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include "dobby.h"
#include <link.h>
#include <mutex>
#include <unordered_map>

namespace {

constexpr uint32_t kArrayBuffer = 0x8892;    // GL_ARRAY_BUFFER
constexpr uint32_t kElementBuffer = 0x8893;  // GL_ELEMENT_ARRAY_BUFFER
constexpr size_t kKeepBytes = 1024 * 1024;    // 每个缓冲最多留 1 MB
constexpr size_t kMinDeclared = 4096;        // 小于 4 KB 的多半是 UI/粒子，跳过
constexpr size_t kKeepBlobs = 256;           // 最多留 256 个（上传率约 13/秒，留少了会漏掉角色）

using GlBufferData = void (*)(uint32_t, int64_t, const void*, uint32_t);
using GlBufferSubData = void (*)(uint32_t, int64_t, int64_t, const void*);

using GlVertexAttribPointer = void (*)(uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void*);
using GlMapBufferRange = void* (*)(uint32_t, int64_t, int64_t, uint32_t);
using GlMapBuffer = void* (*)(uint32_t, uint32_t);
using GlUnmapBuffer = uint8_t (*)(uint32_t);
using GlDrawElements = void (*)(uint32_t, int32_t, uint32_t, const void*);
using GlBindBuffer = void (*)(uint32_t, uint32_t);

GlDrawElements g_drawOrig = nullptr;
GlBindBuffer g_bindOrig = nullptr;
std::atomic<uint64_t> g_boundArrayBuffer{0};      // 当前绑定的 ARRAY_BUFFER
std::mutex g_dm;
std::unordered_map<uint64_t, uint32_t> g_drawCount;   // buffer -> 被画了几次

GlMapBufferRange g_mapRangeOrig = nullptr;
GlMapBuffer g_mapOrig = nullptr;
GlUnmapBuffer g_unmapOrig = nullptr;
// 渲染线程单线程用，普通静态变量足够
uint32_t g_mapTarget = 0;
std::atomic<bool> g_patched{false};      // 实验：只改一次
std::atomic<uint64_t> g_patchHits{0};
constexpr int64_t kProbeSize = 25800;    // 第一轮试过的尺寸
// 批量试探：500~60000 顶点（6000~720000 字节）的主体网格，每个改首顶点
constexpr int64_t kBatchMin = 6000, kBatchMax = 720000;
constexpr int kBatchLimit = 30;
std::atomic<int> g_batchCount{0};
std::mutex g_bm;
std::vector<int64_t> g_batchHit;
void* g_mapPtr = nullptr;
int64_t g_mapLen = 0;
GlVertexAttribPointer g_attribOrig = nullptr;
std::mutex g_am;
std::vector<ms::gles::Attrib> g_attribs;
ms::gles::Attrib g_state[16];      // 按 index 记录当前声明
bool g_stateUsed[16] = {false};
// 客户端内存指针：offset 参数大到不可能是缓冲内偏移，说明顶点数据在游戏内存里，
// 完全绕过 glBufferData —— 这很可能就是角色网格走的路。
uintptr_t g_clientPtr = 0;
int32_t g_clientStride = 0;
std::atomic<int> g_clientCaptured{0};
constexpr int kClientLimit = 24;

GlBufferData g_glBufferData = nullptr;
GlBufferSubData g_glBufferSubData = nullptr;
GlBufferData g_glBufferDataOrig = nullptr;
GlBufferSubData g_glBufferSubDataOrig = nullptr;

std::atomic<uint64_t> g_arrayUploads{0}, g_indexUploads{0}, g_otherUploads{0}, g_totalBytes{0};
std::atomic<uint64_t> g_seq{0};
std::mutex g_m;
std::vector<ms::gles::Blob> g_out;
std::vector<std::tuple<uint64_t,uint64_t,uint32_t>> g_map;   // seq,handle,bytes

// 渲染线程里只做 memcpy + 一次短锁，绝不碰文件。
void record(uint32_t target, int64_t size, const void* data) {
    if (!data || size <= 0) return;
    g_totalBytes.fetch_add(uint64_t(size), std::memory_order_relaxed);
    if (target == kArrayBuffer) g_arrayUploads.fetch_add(1, std::memory_order_relaxed);
    else if (target == kElementBuffer) g_indexUploads.fetch_add(1, std::memory_order_relaxed);
    else g_otherUploads.fetch_add(1, std::memory_order_relaxed);
    if (target != kArrayBuffer) return;                 // 只看顶点缓冲
    if (uint64_t(size) < kMinDeclared) return;          // 太小，跳过（UI/粒子）
    if (g_out.size() >= kKeepBlobs) return;             // 先读 size，无锁快速返回
    std::lock_guard<std::mutex> l(g_m);
    if (g_out.size() >= kKeepBlobs) return;
    ms::gles::Blob b;
    for (int i = 0; i < 16; i++) if (g_stateUsed[i]) b.layout.push_back(g_state[i]);
    b.handle = g_boundArrayBuffer.load(std::memory_order_relaxed);
    b.seq = ++g_seq;
    b.declared = uint64_t(size);
    const size_t keep = size_t(size) > kKeepBytes ? kKeepBytes : size_t(size);
    b.bytes = uint32_t(keep);
    b.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + keep);
    g_map.emplace_back(b.seq, b.handle, b.bytes);
    g_out.push_back(std::move(b));
}

// 属性声明：去重后留存，最多 64 条。渲染线程里只做短锁 push_back。
void onAttribPointer(uint32_t index, int32_t size, uint32_t type, uint8_t normalized, int32_t stride, const void* ptr) {
    if (g_attribs.size() < 64) {
        std::lock_guard<std::mutex> l(g_am);
        if (g_attribs.size() < 64) {
            bool dup = false;
            for (const auto& a : g_attribs)
                if (a.index == index && a.size == uint32_t(size) && a.type == type &&
                    a.stride == uint32_t(stride) && a.offset == uintptr_t(ptr)) { dup = true; break; }
            if (!dup) g_attribs.push_back({index, uint32_t(size), type, normalized, uint32_t(stride), uintptr_t(ptr)});
        }
    }
    if (index < 16) {
        g_state[index] = {index, uint32_t(size), type, normalized, uint32_t(stride), uintptr_t(ptr)};
        g_stateUsed[index] = true;
    }
    // index 0 是位置；地址大到不可能是偏移就是客户端指针
    if (index == 0 && uintptr_t(ptr) > 0x10000 && stride > 0) {
        g_clientPtr = uintptr_t(ptr);
        g_clientStride = stride;
    }
    if (g_attribOrig) g_attribOrig(index, size, type, normalized, stride, ptr);
}

// 部分引擎不做 glBufferData，而是 map 一段显存直接写，unmap 时才落数据。
void* onMapBufferRange(uint32_t target, int64_t off, int64_t len, uint32_t access) {
    void* p = g_mapRangeOrig ? g_mapRangeOrig(target, off, len, access) : nullptr;
    if (p && target == kArrayBuffer) { g_mapTarget = target; g_mapPtr = p; g_mapLen = len; }
    return p;
}
void* onMapBuffer(uint32_t target, uint32_t access) {
    void* p = g_mapOrig ? g_mapOrig(target, access) : nullptr;
    if (p && target == kArrayBuffer) { g_mapTarget = target; g_mapPtr = p; g_mapLen = -1; }
    return p;
}
uint8_t onUnmapBuffer(uint32_t target) {
    if (target == kArrayBuffer && g_mapPtr && g_mapLen > 0) {
        record(target, g_mapLen, g_mapPtr);
        // 写权限验证：把第一个顶点的 x 推开 0.5，只做一次
        if (g_mapLen == kProbeSize && !g_patched.load(std::memory_order_relaxed)) {
            g_patched.store(true, std::memory_order_relaxed);
            g_patchHits.fetch_add(1, std::memory_order_relaxed);
            float* f = static_cast<float*>(g_mapPtr);
            f[0] += 0.5f;
        }
        g_mapPtr = nullptr; g_mapLen = 0;
    }
    return g_unmapOrig ? g_unmapOrig(target) : 1;
}

// glBindBuffer：记住当前绑定的顶点缓冲，绘制时才能归属
void onBindBuffer(uint32_t target, uint32_t buffer) {
    if (target == kArrayBuffer) g_boundArrayBuffer.store(buffer, std::memory_order_relaxed);
    if (g_bindOrig) g_bindOrig(target, buffer);
}

// glDrawElements：子网格数就是同一个缓冲被画的次数
void onDrawElements(uint32_t mode, int32_t count, uint32_t type, const void* indices) {
    // 客户端指针只记次数，不读内存 —— 那个地址随时可能失效，读了会卡死加载。
    if (g_clientPtr && g_clientStride > 0 && count > 0) {
        g_clientCaptured.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t buf = g_boundArrayBuffer.load(std::memory_order_relaxed);
    if (buf) {
        std::lock_guard<std::mutex> l(g_dm);
        if (g_drawCount.size() < 4096) g_drawCount[buf] += 1;
    }
    if (g_drawOrig) g_drawOrig(mode, count, type, indices);
}

void onBufferData(uint32_t target, int64_t size, const void* data, uint32_t usage) {
    record(target, size, data);
    // 批量试探：主体网格每个改一处，命中就记下尺寸
    if (target == kArrayBuffer && size >= kBatchMin && size <= kBatchMax && data &&
        g_batchCount.load(std::memory_order_relaxed) < kBatchLimit) {
        int n = g_batchCount.fetch_add(1, std::memory_order_relaxed);
        if (n < kBatchLimit) {
            std::vector<uint8_t> mod(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
            reinterpret_cast<float*>(mod.data())[0] += 0.5f;
            {std::lock_guard<std::mutex> l(g_bm); g_batchHit.push_back(size);}
            if (g_glBufferDataOrig) g_glBufferDataOrig(target, size, mod.data(), usage);
            return;
        }
    }
    if (target == kArrayBuffer && size == kProbeSize && data &&
        !g_patched.load(std::memory_order_relaxed)) {
        g_patched.store(true, std::memory_order_relaxed);
        g_patchHits.fetch_add(1, std::memory_order_relaxed);
        std::vector<uint8_t> mod(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        reinterpret_cast<float*>(mod.data())[0] += 0.5f;
        if (g_glBufferDataOrig) g_glBufferDataOrig(target, size, mod.data(), usage);
        return;
    }
    if (g_glBufferDataOrig) g_glBufferDataOrig(target, size, data, usage);
}

void onBufferSubData(uint32_t target, int64_t offset, int64_t size, const void* data) {
    // 只关心整体上传；subdata 常用于小块更新，留样意义不大，但计数要有。
    if (target == kArrayBuffer && size > 0) g_totalBytes.fetch_add(uint64_t(size), std::memory_order_relaxed);
    if (g_glBufferSubDataOrig) g_glBufferSubDataOrig(target, offset, size, data);
}

bool g_installed = false;
std::string g_detail;

}  // namespace

namespace ms::gles {

bool install() {
    if (g_installed) return true;
    // libGLESv2.so 由 EGL 在创建上下文时加载，等它出现。
    void* lib = nullptr;
    for (int i = 0; i < 240 && !lib; i++) {
        lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) usleep(250 * 1000);
    }
    if (!lib) { g_detail = "libGLESv2.so 一直没加载"; return false; }

    void* bd = dlsym(lib, "glBufferData");
    if (!bd) bd = dlsym(RTLD_DEFAULT, "glBufferData");
    if (!bd) { g_detail = "找不到 glBufferData"; return false; }

    g_glBufferData = reinterpret_cast<GlBufferData>(bd);
    if (DobbyHook(reinterpret_cast<void*>(bd), reinterpret_cast<dobby_dummy_func_t>(&onBufferData),
                  reinterpret_cast<dobby_dummy_func_t*>(&g_glBufferDataOrig)) != 0 || !g_glBufferDataOrig) {
        g_detail = "DobbyHook glBufferData 失败";
        return false;
    }

    void* de = dlsym(lib, "glDrawElements");
    if (de) DobbyHook(de, reinterpret_cast<dobby_dummy_func_t>(&onDrawElements),
                      reinterpret_cast<dobby_dummy_func_t*>(&g_drawOrig));
    void* bb = dlsym(lib, "glBindBuffer");
    if (bb) DobbyHook(bb, reinterpret_cast<dobby_dummy_func_t>(&onBindBuffer),
                      reinterpret_cast<dobby_dummy_func_t*>(&g_bindOrig));
    void* mpr = dlsym(lib, "glMapBufferRange");
    if (mpr) DobbyHook(mpr, reinterpret_cast<dobby_dummy_func_t>(&onMapBufferRange),
                       reinterpret_cast<dobby_dummy_func_t*>(&g_mapRangeOrig));
    void* mp = dlsym(lib, "glMapBuffer");
    if (mp) DobbyHook(mp, reinterpret_cast<dobby_dummy_func_t>(&onMapBuffer),
                      reinterpret_cast<dobby_dummy_func_t*>(&g_mapOrig));
    void* ub = dlsym(lib, "glUnmapBuffer");
    if (ub) DobbyHook(ub, reinterpret_cast<dobby_dummy_func_t>(&onUnmapBuffer),
                      reinterpret_cast<dobby_dummy_func_t*>(&g_unmapOrig));
    void* vap = dlsym(lib, "glVertexAttribPointer");
    if (!vap) vap = dlsym(RTLD_DEFAULT, "glVertexAttribPointer");
    if (vap) {
        DobbyHook(reinterpret_cast<void*>(vap), reinterpret_cast<dobby_dummy_func_t>(&onAttribPointer),
                  reinterpret_cast<dobby_dummy_func_t*>(&g_attribOrig));
    }
    void* bsd = dlsym(lib, "glBufferSubData");
    if (bsd) {
        g_glBufferSubData = reinterpret_cast<GlBufferSubData>(bsd);
        DobbyHook(reinterpret_cast<void*>(bsd), reinterpret_cast<dobby_dummy_func_t>(&onBufferSubData),
                  reinterpret_cast<dobby_dummy_func_t*>(&g_glBufferSubDataOrig));
    }
    g_installed = true;
    g_detail = "glBufferData + glMapBufferRange 已挂钩";
    return true;
}

Stats stats() {
    Stats s;
    s.arrayUploads = g_arrayUploads.load(std::memory_order_relaxed);
    s.indexUploads = g_indexUploads.load(std::memory_order_relaxed);
    s.otherUploads = g_otherUploads.load(std::memory_order_relaxed);
    s.totalBytes = g_totalBytes.load(std::memory_order_relaxed);
    s.installed = g_installed;
    s.detail = g_detail;
    return s;
}

uint64_t patchHits() { return g_patchHits.load(std::memory_order_relaxed); }

int clientCaptured() { return g_clientCaptured.load(std::memory_order_relaxed); }
intptr_t clientPtr() { return intptr_t(g_clientPtr); }
int clientStride() { return g_clientStride; }

std::vector<int64_t> batchHits() {
    std::lock_guard<std::mutex> l(g_bm);
    return g_batchHit;
}

std::vector<std::tuple<uint64_t,uint64_t,uint32_t>> blobMap() {
    std::lock_guard<std::mutex> l(g_m);
    return g_map;
}

std::vector<std::pair<uint64_t,uint32_t>> drawCounts() {
    std::lock_guard<std::mutex> l(g_dm);
    std::vector<std::pair<uint64_t,uint32_t>> r(g_drawCount.begin(), g_drawCount.end());
    return r;
}

std::vector<Attrib> attribs() {
    std::lock_guard<std::mutex> l(g_am);
    return g_attribs;
}

std::vector<Blob> take() {
    std::lock_guard<std::mutex> l(g_m);
    std::vector<Blob> r;
    r.swap(g_out);
    return r;
}

}  // namespace ms::gles
