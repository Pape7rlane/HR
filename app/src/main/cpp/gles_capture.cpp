#include "gles_capture.hpp"
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include "dobby.h"
#include <link.h>
#include <mutex>

namespace {

constexpr uint32_t kArrayBuffer = 0x8892;    // GL_ARRAY_BUFFER
constexpr uint32_t kElementBuffer = 0x8893;  // GL_ELEMENT_ARRAY_BUFFER
constexpr size_t kKeepBytes = 1024 * 1024;    // 每个缓冲最多留 1 MB
constexpr size_t kMinDeclared = 4096;        // 小于 4 KB 的多半是 UI/粒子，跳过
constexpr size_t kKeepBlobs = 32;            // 最多留 32 个

using GlBufferData = void (*)(uint32_t, int64_t, const void*, uint32_t);
using GlBufferSubData = void (*)(uint32_t, int64_t, int64_t, const void*);

using GlVertexAttribPointer = void (*)(uint32_t, int32_t, uint32_t, uint8_t, int32_t, const void*);
GlVertexAttribPointer g_attribOrig = nullptr;
std::mutex g_am;
std::vector<ms::gles::Attrib> g_attribs;
ms::gles::Attrib g_state[16];      // 按 index 记录当前声明
bool g_stateUsed[16] = {false};

GlBufferData g_glBufferData = nullptr;
GlBufferSubData g_glBufferSubData = nullptr;
GlBufferData g_glBufferDataOrig = nullptr;
GlBufferSubData g_glBufferSubDataOrig = nullptr;

std::atomic<uint64_t> g_arrayUploads{0}, g_indexUploads{0}, g_otherUploads{0}, g_totalBytes{0};
std::atomic<uint64_t> g_seq{0};
std::mutex g_m;
std::vector<ms::gles::Blob> g_out;

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
    b.seq = ++g_seq;
    b.declared = uint64_t(size);
    const size_t keep = size_t(size) > kKeepBytes ? kKeepBytes : size_t(size);
    b.bytes = uint32_t(keep);
    b.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + keep);
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
    if (g_attribOrig) g_attribOrig(index, size, type, normalized, stride, ptr);
}

void onBufferData(uint32_t target, int64_t size, const void* data, uint32_t usage) {
    record(target, size, data);
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
    g_detail = "glBufferData 已挂钩";
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
