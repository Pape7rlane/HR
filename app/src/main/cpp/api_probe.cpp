#include "api_probe.hpp"
#include "core.hpp"
#include "logfmt.hpp"
#include <csignal>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <dlfcn.h>
#include <link.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- 模块信息
// 关键：dl_iterate_phdr 回调里给的 dlpi_phdr 只在回调期间保证有效，回调返回后再去遍历它是踩野指针。
// 所以这里在回调内部就把需要的三样东西解析完存下来：
//   segs —— PT_LOAD 段（判断某个地址落在哪、能不能读、是不是代码）
//   dyn  —— PT_DYNAMIC 的内存地址（符号表从这里找）
//   buildId —— PT_NOTE 里的 GNU Build ID
struct Seg { uintptr_t start = 0; size_t size = 0; int flags = 0; };

struct Mod {
    std::string name;
    std::string path;
    uintptr_t base = 0;
    std::string buildId;
    std::vector<Seg> segs;
    const ElfW(Dyn)* dyn = nullptr;
};

// 在 PT_DYNAMIC 里按名字查符号。崩铁的 so 是 android_dlopen_ext 加进自己的 linker
// namespace 的，dlsym 看不到；但 dl_iterate_phdr 能看到模块，PT_DYNAMIC 也拿得到。
uintptr_t dynSymbol(const Mod& m, const char* want, std::string& why) {
    if (!m.dyn) { why = "没有 PT_DYNAMIC"; return 0; }
    uintptr_t symtab = 0, strtab = 0, hash = 0, gnuhash = 0;
    size_t strsz = 0, syment = 0;
    for (const ElfW(Dyn)* d = m.dyn; d->d_tag != DT_NULL; ++d) {
        if (d->d_tag == DT_SYMTAB) symtab = uintptr_t(d->d_un.d_ptr);
        else if (d->d_tag == DT_STRTAB) strtab = uintptr_t(d->d_un.d_ptr);
        else if (d->d_tag == DT_STRSZ) strsz = size_t(d->d_un.d_val);
        else if (d->d_tag == DT_SYMENT) syment = size_t(d->d_un.d_val);
        else if (d->d_tag == DT_HASH) hash = uintptr_t(d->d_un.d_ptr);
        else if (d->d_tag == 0x6ffffef5) gnuhash = uintptr_t(d->d_un.d_ptr);   // DT_GNU_HASH
    }
    auto fix = [&](uintptr_t p) { return (p && p < m.base) ? m.base + p : p; };
    symtab = fix(symtab); strtab = fix(strtab); hash = fix(hash); gnuhash = fix(gnuhash);
    if (!symtab || !strtab || !strsz) {
        char b[180];
        std::snprintf(b, sizeof b, "动态段不全 symtab=%#zx strtab=%#zx strsz=%zu",
                      size_t(symtab), size_t(strtab), strsz);
        why = b;
        return 0;
    }
    size_t count = 0;
    if (hash) {
        count = reinterpret_cast<const uint32_t*>(hash)[1];
    } else if (gnuhash) {
        auto* g = reinterpret_cast<const uint32_t*>(gnuhash);
        uint32_t nbuckets = g[0], symoffset = g[1], bloom = g[2];
        const uint32_t* buckets = g + 4 + size_t(bloom) * 2;
        const uint32_t* chains = buckets + nbuckets;
        uint32_t last = 0;
        for (uint32_t b = 0; b < nbuckets; b++) if (buckets[b] > last) last = buckets[b];
        if (last < symoffset) last = symoffset;
        count = last;
        while (count < 400000 && (chains[count - symoffset] & 1u) == 0) count++;
        count++;
    } else {
        why = "既没有 DT_HASH 也没有 DT_GNU_HASH";
        return 0;
    }
    if (count > 400000) count = 400000;
    auto* syms = reinterpret_cast<const ElfW(Sym)*>(symtab);
    for (size_t i = 0; i < count; i++) {
        uint32_t off = uint32_t(syms[i].st_name);
        if (!off || off >= strsz || !syms[i].st_value) continue;
        if (std::strcmp(reinterpret_cast<const char*>(strtab + off), want) == 0)
            return m.base + uintptr_t(syms[i].st_value);
    }
    char b[160];
    std::snprintf(b, sizeof b, "扫了 %zu 个符号没命中", count);
    why = b;
    return 0;
}

int collect(dl_phdr_info* info, size_t, void* p) {
    auto& v = *static_cast<std::vector<Mod>*>(p);
    const char* raw = info->dlpi_name ? info->dlpi_name : "";
    if (!raw[0] || !info->dlpi_addr) return 0;              // 主程序映像跳过

    Mod m;
    m.path = raw;
    const char* slash = std::strrchr(raw, '/');
    m.name = slash ? slash + 1 : raw;
    if (m.name.empty()) return 0;
    m.base = uintptr_t(info->dlpi_addr);

    for (size_t i = 0; i < size_t(info->dlpi_phnum); i++) {
        const ElfW(Phdr)& h = info->dlpi_phdr[i];
        if (h.p_type == PT_LOAD) {
            m.segs.push_back({m.base + uintptr_t(h.p_vaddr), size_t(h.p_memsz), int(h.p_flags)});
        } else if (h.p_type == PT_DYNAMIC) {
            m.dyn = reinterpret_cast<const ElfW(Dyn)*>(m.base + uintptr_t(h.p_vaddr));
        } else if (h.p_type == PT_NOTE && h.p_filesz < 1024 * 1024) {
            auto* note = reinterpret_cast<const uint8_t*>(m.base + uintptr_t(h.p_vaddr));
            const size_t total = size_t(h.p_filesz);
            size_t at = 0;
            while (at + 12 <= total) {
                uint32_t namesz = 0, descsz = 0, type = 0;
                std::memcpy(&namesz, note + at, 4);
                std::memcpy(&descsz, note + at + 4, 4);
                std::memcpy(&type, note + at + 8, 4);
                at += 12;
                if (at + namesz > total) break;
                std::string name(reinterpret_cast<const char*>(note + at), namesz);
                while (!name.empty() && name.back() == '\0') name.pop_back();
                at += (size_t(namesz) + 3) & ~size_t(3);
                if (at + descsz > total) break;
                if (name == "GNU" && type == 3 && m.buildId.empty()) {
                    char b[3];
                    for (uint32_t k = 0; k < descsz; k++) {
                        std::snprintf(b, sizeof b, "%02x", note[at + k]);
                        m.buildId += b;
                    }
                }
                at += (size_t(descsz) + 3) & ~size_t(3);
            }
        }
    }
    v.push_back(std::move(m));
    return 0;
}

struct Place { std::string text; bool readable = false; bool exec = false; bool known = false; };

Place locate(const std::vector<Mod>& mods, uintptr_t p) {
    Place out;
    char b[128];
    for (const auto& m : mods) {
        for (const auto& s : m.segs) {
            if (p < s.start || p - s.start >= s.size) continue;
            const char* kind = (s.flags & PF_X) ? "text" : (s.flags & PF_W) ? "data" : "rodata";
            std::snprintf(b, sizeof b, "%s+%#zx/%s", m.name.c_str(), size_t(p - m.base), kind);
            out.text = b;
            out.readable = true;
            out.exec = (s.flags & PF_X) != 0;
            out.known = true;
            return out;
        }
    }
    std::snprintf(b, sizeof b, "?+%#zx", size_t(p));
    out.text = b;
    return out;
}

std::string codeWords(const std::vector<Mod>& mods, uintptr_t p, int count) {
    if (!p) return "-";
    Place where = locate(mods, p);
    if (!where.known || !where.exec) return "-";
    std::string out;
    char b[16];
    for (int i = 0; i < count; i++) {
        uint32_t w = 0;
        std::memcpy(&w, reinterpret_cast<const void*>(p + uintptr_t(i) * 4), sizeof(w));
        std::snprintf(b, sizeof b, "%s%08x", i ? " " : "", w);
        out += b;
    }
    return out;
}

}  // namespace

namespace ms::probe {

// 探测全程的保护：任何一步踩到非法内存都跳回来，把"停在哪一步"落盘，进程不跟着死。
static sigjmp_buf g_guard;
static volatile sig_atomic_t g_armed = 0;
static const char* g_stage = "启动";
static std::string g_partial;
static std::string g_path;

static void onGuardSignal(int) { if (g_armed) siglongjmp(g_guard, 1); }

static size_t g_written = 0;
static void flush() {
    if (g_path.empty() || g_written >= g_partial.size()) return;
    try {
        std::ofstream f(g_path, std::ios::binary | std::ios::app);
        if (!f) return;
        f.write(g_partial.data() + g_written, std::streamsize(g_partial.size() - g_written));
        f.flush();
        g_written = g_partial.size();
    } catch (...) {}
}

static void stage(const char* s) { g_stage = s; }

// 已知版本的关键符号 RVA（tools/scan_game.py 从 APK 里算出来，和运行时 Build ID 核对后才用）。
struct KnownSymbol { const char* buildId; const char* name; uintptr_t rva; };
static const KnownSymbol kKnown[] = {
    {"8c76cb8b4f65ae2a", "il2cpp_get_api_table", 0x6dcdc4c},
};
// 0x6dcdc4c 处录到的首字：一条 B 指令，编码 0x17ff8ff2（内存字节 f2 8f ff 17），
// 跳去 0x6db1c14。注意这里是按 uint32 的值写的，不是十六进制文本的字节顺序。
static constexpr uint32_t kEntryWord = 0x17ff8ff2u;

void apiTable(const std::string& path, int round) {
    g_path = path;
    g_partial.clear();
    g_written = 0;
    char head[128];
    std::snprintf(head, sizeof head, "\n===== 第 %d 轮（启动后）=====\n", round);
    g_partial += head;

    auto old = ::signal(SIGSEGV, onGuardSignal);
    if (sigsetjmp(g_guard, 1) != 0) {
        g_armed = 0;
        ::signal(SIGSEGV, old);
        std::string msg = std::string("探测崩溃，停在：") + g_stage;
        g_partial += "\n!! " + msg + "\n";
        flush();
        throw std::runtime_error(msg);
    }
    g_armed = 1;

    stage("收集模块");
    std::vector<Mod> mods;
    dl_iterate_phdr(collect, &mods);
    {
        char b[128];
        std::snprintf(b, sizeof b, "已加载模块 %zu 个\n\n", mods.size());
        g_partial += b;
    }
    for (const auto& m : mods) {
        if (m.name != "libunity.so" && m.name != "libil2cpp.so") continue;
        char b[256];
        std::snprintf(b, sizeof b, "%-16s base=%#zx build=%s segs=%zu dynamic=%p\n",
                      m.name.c_str(), size_t(m.base), m.buildId.c_str(), m.segs.size(),
                      reinterpret_cast<const void*>(m.dyn));
        g_partial += b;
    }
    flush();

    stage("解析 il2cpp_get_api_table 地址");
    uintptr_t fnAddr = 0;
    std::string how, why;
    const Mod* managed = nullptr;
    for (const auto& m : mods) if (m.name == "libil2cpp.so") managed = &m;
    if (!managed) throw std::runtime_error("进程里没有 libil2cpp.so");

    for (const auto& k : kKnown) {
        if (managed->buildId == k.buildId && std::strcmp(k.name, "il2cpp_get_api_table") == 0) {
            fnAddr = managed->base + k.rva;
            how = "已知 RVA";
            break;
        }
    }
    if (!fnAddr) {
        uintptr_t got = 0;
        stage("查符号表");
        got = dynSymbol(*managed, "il2cpp_get_api_table", why);
        if (got) { fnAddr = got; how = "符号表"; }
    }
    if (!fnAddr) {
        g_partial += "\n找不到 il2cpp_get_api_table（" + (why.empty() ? std::string("已知表里没有这个 Build ID") : why) + "）\n";
        flush();
        throw std::runtime_error("找不到 il2cpp_get_api_table");
    }
    {
        char b[256];
        std::snprintf(b, sizeof b, "\nil2cpp_get_api_table = %#zx（来源 %s）%s\n入口前 8 条：%s\n",
                      size_t(fnAddr), how.c_str(), locate(mods, fnAddr).text.c_str(),
                      codeWords(mods, fnAddr, 8).c_str());
        g_partial += b;
    }
    flush();

    stage("校验入口字节");
    {
        uint32_t first = 0;
        std::memcpy(&first, reinterpret_cast<const void*>(fnAddr), sizeof(first));
        char b[220];
        std::snprintf(b, sizeof b, "入口首字 %08x，%s\n", first,
                      first == kEntryWord ? "与录制一致，地址正确" : "与录制不一致，地址可能算错");
        g_partial += b;
        flush();
        if (first != kEntryWord) {
            g_armed = 0;
            ::signal(SIGSEGV, old);
            throw std::runtime_error("入口字节不符，未调用（读到 " + std::to_string(first) + "）");
        }
    }

    stage("调用 il2cpp_get_api_table()");
    using TableFn = void* (*)();
    auto getTable = reinterpret_cast<TableFn>(fnAddr);
    void* table = getTable();
    stage("读取返回的表");
    if (!table) {
        g_partial += "\n返回空指针 —— 表不在这个函数手里。\n";
        flush();
        g_armed = 0;
        ::signal(SIGSEGV, old);
        throw std::runtime_error("il2cpp_get_api_table 返回空指针");
    }
    {
        Place where = locate(mods, reinterpret_cast<uintptr_t>(table));
        char b[220];
        std::snprintf(b, sizeof b, "\ntable = %p（%s），%s\n\n", table, where.text.c_str(),
                      where.readable ? "可读" : "不在已加载段内");
        g_partial += b;
        flush();
        if (!where.readable) {
            g_armed = 0;
            ::signal(SIGSEGV, old);
            throw std::runtime_error("表指针不在已加载段内");
        }
    }

    stage("展开表项");
    const auto t = reinterpret_cast<uintptr_t>(table);
    auto slot = [&](size_t i) {
        uintptr_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(t + i * sizeof(uintptr_t)), sizeof(v));
        return v;
    };
    size_t plausible = 0;
    for (size_t i = 0; i < 8; i++) if (slot(i) && locate(mods, slot(i)).known) plausible++;
    const size_t limit = plausible >= 4 ? 256 : 32;
    {
        char b[160];
        std::snprintf(b, sizeof b, "前 8 项里 %zu 个落在已加载模块，按 %zu 项展开\n\n%-4s %-18s %-32s %s\n",
                      plausible, limit, "idx", "指针", "落点", "开头指令");
        g_partial += b;
    }
    for (size_t i = 0; i < limit; i++) {
        uintptr_t v = slot(i);
        char b[300];
        std::snprintf(b, sizeof b, "%-4zu %#018zx %-32s %s\n", i, size_t(v),
                      locate(mods, v).text.c_str(), codeWords(mods, v, 3).c_str());
        g_partial += b;
    }

    g_armed = 0;
    ::signal(SIGSEGV, old);
    flush();
}


// ---------------------------------------------------------------- 内存扫描 metadata
namespace {
struct MapRange { uintptr_t start = 0, end = 0; bool anon = false; };
}

size_t memScanMetadata(const std::string& outPath) {
    std::ifstream maps("/proc/self/maps");
    if (!maps) throw std::runtime_error("/proc/self/maps 打不开");

    std::vector<MapRange> ranges;
    std::string line;
    size_t scanned = 0, skipped = 0;
    while (std::getline(maps, line)) {
        uintptr_t a = 0, b = 0;
        char perms[8] = {0};
        if (std::sscanf(line.c_str(), "%zx-%zx %7s", &a, &b, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        const size_t len = size_t(b - a);
        if (!len || len > 512u * 1024 * 1024) { skipped++; continue; }   // 超大区域跳过
        // 只扫匿名/堆，跳过文件映射（代码、asset、so）
        auto sp = line.find(' ');
        std::string path;
        for (int field = 0; field < 4 && sp != std::string::npos; field++) {
            if (field == 3) path = line.substr(sp + 1);
            sp = line.find(' ', sp + 1);
        }
        const bool anon = path.empty() || path[0] == '[' || path == "/dev/zero (deleted)";
        ranges.push_back({a, b, anon});
    }

    static constexpr uint8_t kMagic[4] = {0xaf, 0x1b, 0xb1, 0xfa};
    for (const auto& r : ranges) {
        if (!r.anon) continue;
        auto* p = reinterpret_cast<const uint8_t*>(r.start);
        const size_t len = size_t(r.end - r.start);
        const uint8_t* at = p;
        const uint8_t* stop = p + len;
        while (at + 4 <= stop) {
            const void* hit = memmem(at, size_t(stop - at), kMagic, 4);
            if (!hit) break;
            auto* h = static_cast<const uint8_t*>(hit);
            scanned += size_t(h - at) + 4;
            // 明文头：magic, version, stringLiteralOffset ...
            if (h + 32 <= stop) {
                int32_t version = 0;
                std::memcpy(&version, h + 4, 4);
                if (version >= 16 && version <= 40) {
                    const size_t dumpLen = size_t(stop - h);
                    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
                    if (f) { f.write(reinterpret_cast<const char*>(h), std::streamsize(dumpLen)); f.flush(); }
                    return f ? dumpLen : 0;
                }
            }
            at = h + 4;
        }
    }
    throw std::runtime_error("内存里没找到明文 metadata（扫了 " + std::to_string(ranges.size()) +
                             " 个区域，跳过 " + std::to_string(skipped) + " 个超大区域）");
}

}  // namespace ms::probe
