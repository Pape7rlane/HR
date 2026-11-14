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
constexpr size_t kKeepBytes = 256 * 1024;    // 每个缓冲最多留 256 KB
constexpr size_t kKeepBlobs = 32;            // 最多留 32 个

using GlBufferData = void (*)(uint32_t, int64_t, const void*, uint32_t);
using GlBufferSubData = void (*)(uint32_t, int64_t, int64_t, const void*);

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
    if (g_out.size() >= kKeepBlobs) return;             // 先读 size，无锁快速返回
    std::lock_guard<std::mutex> l(g_m);
    if (g_out.size() >= kKeepBlobs) return;
    ms::gles::Blob b;
    b.seq = ++g_seq;
    b.declared = uint64_t(size);
    const size_t keep = size_t(size) > kKeepBytes ? kKeepBytes : size_t(size);
    b.bytes = uint32_t(keep);
    b.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + keep);
    g_out.push_back(std::move(b));
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

std::vector<Blob> take() {
    std::lock_guard<std::mutex> l(g_m);
    std::vector<Blob> r;
    r.swap(g_out);
    return r;
}

}  // namespace ms::gles
