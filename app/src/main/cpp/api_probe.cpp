#include "api_probe.hpp"
#include "core.hpp"
#include "logfmt.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Mod { std::string name; std::string path; uintptr_t base; const ElfW(Phdr)* ph; size_t n; };

int collect(dl_phdr_info* info, size_t, void* p) {
    auto& v = *static_cast<std::vector<Mod>*>(p);
    std::string path = info->dlpi_name ? info->dlpi_name : "";
    std::string name = path;
    auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.empty() || !info->dlpi_addr) return 0;      // 主程序映像和空名字跳过
    v.push_back({name, path, uintptr_t(info->dlpi_addr), info->dlpi_phdr, size_t(info->dlpi_phnum)});
    return 0;
}

// 一个地址落在哪个模块的哪一段；同时告诉调用方这段可不可以读、是不是代码。
struct Place { std::string text; bool readable = false; bool exec = false; bool known = false; };

Place locate(const std::vector<Mod>& mods, uintptr_t p) {
    Place out;
    char b[128];
    for (const auto& m : mods) {
        for (size_t k = 0; k < m.n; k++) {
            ElfW(Phdr) h;
            std::memcpy(&h, m.ph + k, sizeof(h));
            if (h.p_type != PT_LOAD) continue;
            uintptr_t start = m.base + uintptr_t(h.p_vaddr);
            if (p < start || p - start >= uintptr_t(h.p_memsz)) continue;
            const char* seg = (h.p_flags & PF_X) ? "text" : (h.p_flags & PF_W) ? "data" : "rodata";
            std::snprintf(b, sizeof b, "%s+%#zx/%s", m.name.c_str(), size_t(p - start), seg);
            out.text = b;
            out.readable = true;
            out.exec = (h.p_flags & PF_X) != 0;
            out.known = true;
            return out;
        }
    }
    std::snprintf(b, sizeof b, "?+%#zx", size_t(p));
    out.text = b;
    return out;
}

// 开头几条指令的原始编码。只在确认落在可执行段后才读。
std::string codeWords(const std::vector<Mod>& mods, uintptr_t p, int instructionCount) {
    if (!p) return "-";
    Place where = locate(mods, p);
    if (!where.known || !where.exec) return "-";
    std::string out;
    char b[16];
    for (int i = 0; i < instructionCount; i++) {
        uint32_t w = 0;
        std::memcpy(&w, reinterpret_cast<const void*>(p + uintptr_t(i) * 4), 4);
        std::snprintf(b, sizeof b, "%s%08x", i ? " " : "", w);
        out += b;
    }
    return out;
}

// 这个指针是不是一个"直接 B 到别处"的桩：是就给出目标，方便判断该不该跟过去。
std::string thunk(const std::vector<Mod>& mods, uintptr_t p) {
    if (!p) return "";
    Place where = locate(mods, p);
    if (!where.known || !where.exec) return "";
    uint32_t w = 0;
    std::memcpy(&w, reinterpret_cast<const void*>(p), 4);
    if ((w & 0xFC000000u) != 0x14000000u) return "";
    int64_t imm = int64_t(w & 0x03FFFFFFu);
    if (imm & 0x02000000) imm -= 0x04000000;
    uintptr_t target = uintptr_t(int64_t(p) + imm * 4);
    char b[160];
    std::snprintf(b, sizeof b, " -> B %s", locate(mods, target).text.c_str());
    return b;
}

std::string buildIdOf(uintptr_t base, const ElfW(Phdr)* ph, size_t n) {
    for (size_t k = 0; k < n; k++) {
        ElfW(Phdr) h;
        std::memcpy(&h, ph + k, sizeof(h));
        if (h.p_type != PT_NOTE || h.p_memsz > 1024 * 1024) continue;
        auto* note = reinterpret_cast<const uint8_t*>(base + uintptr_t(h.p_vaddr));
        size_t at = 0;
        while (at + 12 <= h.p_memsz) {
            uint32_t namesz = 0, descsz = 0, type = 0;
            std::memcpy(&namesz, note + at, 4);
            std::memcpy(&descsz, note + at + 4, 4);
            std::memcpy(&type, note + at + 8, 4);
            at += 12;
            if (at + namesz > h.p_memsz) break;
            std::string name(reinterpret_cast<const char*>(note + at), namesz);
            while (!name.empty() && name.back() == '\0') name.pop_back();
            at += (namesz + 3) & ~size_t(3);
            if (at + descsz > h.p_memsz) break;
            if (name == "GNU" && type == 3) {
                std::string hex;
                char b[3];
                for (uint32_t i = 0; i < descsz; i++) { std::snprintf(b, sizeof b, "%02x", note[at + i]); hex += b; }
                return hex;
            }
            at += (descsz + 3) & ~size_t(3);
        }
    }
    return "";
}

// 在指定模块的 .dynsym 里按名字找符号，返回运行时地址；找不到返回 0。
// 为什么不直接用 dlsym：崩铁的 so 是 android_dlopen_ext 加进它自己的 linker namespace 的，
// 我们从私有目录 dlopen 进来的这个 so 去 dlsym，NOLOAD 拿不到句柄，RTLD_DEFAULT 也只搜自己那个 namespace。
// dl_iterate_phdr 给的是进程级的列表，能看见；符号表就在 PT_DYNAMIC 里，自己查一遍最稳。
uintptr_t dynSymbol(const Mod& m, const char* want, std::string& why) {
    const ElfW(Dyn)* dyn = nullptr;
    for (size_t i = 0; i < m.n; i++) {
        ElfW(Phdr) h;
        std::memcpy(&h, m.ph + i, sizeof(h));
        if (h.p_type == PT_DYNAMIC) dyn = reinterpret_cast<const ElfW(Dyn)*>(m.base + uintptr_t(h.p_vaddr));
    }
    if (!dyn) { why = "PT_DYNAMIC 没找到"; return 0; }
    uintptr_t symtab = 0, strtab = 0, hash = 0, gnuhash = 0;
    size_t strsz = 0, syment = sizeof(ElfW(Sym));
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        if (d->d_tag == DT_SYMTAB) symtab = d->d_un.d_ptr;
        else if (d->d_tag == DT_STRTAB) strtab = d->d_un.d_ptr;
        else if (d->d_tag == DT_STRSZ) strsz = size_t(d->d_un.d_val);
        else if (d->d_tag == DT_SYMENT) syment = size_t(d->d_un.d_val);
        else if (d->d_tag == DT_HASH) hash = d->d_un.d_ptr;
        else if (d->d_tag == 0x6ffffef5) gnuhash = d->d_un.d_ptr;   // DT_GNU_HASH
    }
    // bionic 已经把这两个重定位成绝对地址；万一给的是偏移就补上基址。
    auto fix = [&](uintptr_t p) { return (p && p < m.base) ? m.base + p : p; };
    symtab = fix(symtab); strtab = fix(strtab); hash = fix(hash); gnuhash = fix(gnuhash);
    if (!symtab || !strtab || !strsz || syment < sizeof(ElfW(Sym))) {
        char b[160];
        std::snprintf(b, sizeof b, "动态段不全 symtab=%#zx strtab=%#zx strsz=%zu syment=%zu", size_t(symtab), size_t(strtab), strsz, syment);
        why = b; return 0;
    }

    size_t count = 0;
    if (hash) {
        count = reinterpret_cast<const uint32_t*>(hash)[1];        // DT_HASH：nchain 就是符号总数
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
        why = "既没有 DT_HASH 也没有 DT_GNU_HASH"; return 0;
    }
    if (count > 400000) count = 400000;

    auto* syms = reinterpret_cast<const ElfW(Sym)*>(symtab);
    for (size_t i = 0; i < count; i++) {
        uint32_t nameOff = uint32_t(syms[i].st_name);
        if (!nameOff || nameOff >= strsz || !syms[i].st_value) continue;
        const char* n = reinterpret_cast<const char*>(strtab + nameOff);
        if (std::strcmp(n, want) == 0) return m.base + uintptr_t(syms[i].st_value);
    }
    char b[200];
    std::snprintf(b, sizeof b, "扫了 %zu 个符号没命中（symtab=%#zx strtab=%#zx）", count, size_t(symtab), size_t(strtab));
    why = b;
    return 0;
}
}  // namespace

namespace ms::probe {

// 已知版本的关键符号 RVA。这些值是 tools/scan_game.py 从 APK 里算出来的，
// 跟运行时 dl_iterate_phdr 拿到的 Build ID 核对后才用——对不上就不碰。
// 走这条比查符号表直接，符号表那条留着兜未来版本。
struct KnownSymbol { const char* buildId; const char* name; uintptr_t rva; };
static const KnownSymbol kKnown[] = {
    {"8c76cb8b4f65ae2a", "il2cpp_get_api_table", 0x6dcdc4c},
};

void apiTable(const std::string& path) {
    std::vector<Mod> mods;
    dl_iterate_phdr(collect, &mods);
    if (mods.empty()) throw std::runtime_error("看不到任何已加载模块");

    using TableFn = void* (*)();
    // 已经在进程里了，NOLOAD 拿句柄；拿不到就走全局符号表，两条都不成才是真的没有。
    // 三种拿法依次试：按 soname、按 dl_iterate_phdr 看到的完整路径、全局符号表。
    // 崩铁是 android_dlopen_ext 加载的，linker namespace 跟我们的 so 不一定是同一个，
    // 按 soname 可能命中不了，完整路径最可靠。
    // 先自己查符号表（绕开 linker namespace），再退回 dlsym 三条老路。
    TableFn getTable = nullptr;
    uintptr_t bySymbol = 0;
    std::string why, how;
    for (const auto& m : mods) {
        if (m.name != "libil2cpp.so") continue;
        const std::string bid = buildIdOf(m.base, m.ph, m.n);
        for (const auto& k : kKnown) {
            if (bid == k.buildId && std::strcmp(k.name, "il2cpp_get_api_table") == 0) {
                bySymbol = m.base + k.rva; how = "已知 RVA"; break;
            }
        }
        if (!bySymbol) { bySymbol = dynSymbol(m, "il2cpp_get_api_table", why); if (bySymbol) how = "符号表"; }
        if (bySymbol) break;
    }
    if (bySymbol) getTable = reinterpret_cast<TableFn>(bySymbol);
    auto tryHandle = [&](void* h) {
        if (h && !getTable) getTable = reinterpret_cast<TableFn>(dlsym(h, "il2cpp_get_api_table"));
    };
    tryHandle(dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD));
    for (const auto& m : mods) if (m.name == "libil2cpp.so" && !getTable) tryHandle(dlopen(m.path.c_str(), RTLD_NOW | RTLD_NOLOAD));
    if (!getTable) getTable = reinterpret_cast<TableFn>(dlsym(RTLD_DEFAULT, "il2cpp_get_api_table"));
    if (!getTable) {
        std::string seen;
        for (const auto& m : mods) if (m.name.find("cpp") != std::string::npos || m.name.find("unity") != std::string::npos) seen += m.name + " ";
        throw std::runtime_error("找不到 il2cpp_get_api_table（符号表：" + (why.empty() ? std::string("未尝试") : why) + "）；进程里相关的库：" + (seen.empty() ? std::string("（没有名字含 cpp/unity 的模块）") : seen));
    }

    std::string out;
    char line[512];
    out += "# HSRBox —— il2cpp API 表探测\n";
    out += "# 只读：调用 il2cpp_get_api_table() 本身不解引用任何参数，也不挂钩。\n";

    const Mod* managed = nullptr;
    for (const auto& m : mods) if (m.name == "libil2cpp.so") managed = &m;
    std::snprintf(line, sizeof line, "libil2cpp.so base=%#zx build_id=%s\n",
                  managed ? size_t(managed->base) : 0,
                  managed ? buildIdOf(managed->base, managed->ph, managed->n).c_str() : "");
    out += line;
    for (const auto& m : mods) {
        if (m.name != "libunity.so" && m.name != "libil2cpp.so") continue;
        std::snprintf(line, sizeof line, "%-20s base=%#zx build_id=%s\n",
                      m.name.c_str(), size_t(m.base), buildIdOf(m.base, m.ph, m.n).c_str());
        out += line;
    }

    Place fnAt = locate(mods, reinterpret_cast<uintptr_t>(getTable));
    std::snprintf(line, sizeof line, "\nil2cpp_get_api_table @ %p（来源 %s）(%s)\n%s\n",
                  reinterpret_cast<void*>(getTable), how.c_str(), fnAt.text.c_str(),
                  codeWords(mods, reinterpret_cast<uintptr_t>(getTable), 6).c_str());
    out += line;

    void* table = getTable();
    if (!table) {
        out += "\n返回了空指针——这张表要么由别的地方填，要么这条路走不通。\n";
        write(path, out);
        throw std::runtime_error("il2cpp_get_api_table 返回空指针");
    }

    uintptr_t t = reinterpret_cast<uintptr_t>(table);
    Place tableAt = locate(mods, t);
    std::snprintf(line, sizeof line, "table = %#zx (%s)，%s\n\n",
                  size_t(t), tableAt.text.c_str(), tableAt.readable ? "可读" : "不在任何已加载段里，放弃读取");
    out += line;
    if (!tableAt.readable) {
        write(path, out);
        throw std::runtime_error("表指针不在任何已加载段内");
    }

    // 先读 8 项看看像不像一张函数表：全零或者全落不到已知模块，就别往下读了。
    auto readSlot = [&](size_t i) -> uintptr_t {
        uintptr_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(t + i * sizeof(uintptr_t)), sizeof(v));
        return v;
    };
    size_t plausible = 0;
    for (size_t i = 0; i < 8; i++) {
        uintptr_t v = readSlot(i);
        if (v && locate(mods, v).known) plausible++;
    }
    const size_t limit = plausible >= 4 ? 256 : 32;
    std::snprintf(line, sizeof line, "前 8 项里 %zu 个落在已加载模块，按 %zu 项展开\n\n", plausible, limit);
    out += line;

    std::snprintf(line, sizeof line, "%-4s %-18s %-34s %s\n", "idx", "指针", "落点", "开头指令");
    out += line;
    for (size_t i = 0; i < limit; i++) {
        uintptr_t v = readSlot(i);
        Place where = locate(mods, v);
        std::string tunk = thunk(mods, v);
        std::snprintf(line, sizeof line, "%-4zu %#018zx %-34s %s%s\n",
                      i, size_t(v), where.text.c_str(),
                      codeWords(mods, v, 4).c_str(), tunk.c_str());
        out += line;
    }

    out += "\n# 说明：text 段里的指针才可能是函数；data/rodata 的是数据表。\n";
    out += "#       B 打头的桩，-> 后面是它真正去的地方。\n";
    write(path, out);
}

}  // namespace ms::probe
