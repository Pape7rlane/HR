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
struct Mod { std::string name; uintptr_t base; const ElfW(Phdr)* ph; size_t n; };

int collect(dl_phdr_info* info, size_t, void* p) {
    auto& v = *static_cast<std::vector<Mod>*>(p);
    std::string name = info->dlpi_name ? info->dlpi_name : "";
    auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.empty() || !info->dlpi_addr) return 0;      // 主程序映像和空名字跳过
    v.push_back({name, uintptr_t(info->dlpi_addr), info->dlpi_phdr, size_t(info->dlpi_phnum)});
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
            std::memcpy(&h, m.ph + k * sizeof(h), sizeof(h));
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
        std::memcpy(&h, ph + k * sizeof(h), sizeof(h));
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
}  // namespace

namespace ms::probe {

void apiTable(const std::string& path) {
    std::vector<Mod> mods;
    dl_iterate_phdr(collect, &mods);
    if (mods.empty()) throw std::runtime_error("看不到任何已加载模块");

    using TableFn = void* (*)();
    // 已经在进程里了，NOLOAD 拿句柄；拿不到就走全局符号表，两条都不成才是真的没有。
    TableFn getTable = nullptr;
    if (void* lib = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD)) {
        getTable = reinterpret_cast<TableFn>(dlsym(lib, "il2cpp_get_api_table"));
    }
    if (!getTable) getTable = reinterpret_cast<TableFn>(dlsym(RTLD_DEFAULT, "il2cpp_get_api_table"));
    if (!getTable) throw std::runtime_error("找不到 il2cpp_get_api_table（既没有 NOLOAD 句柄，全局符号表里也没有）");

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
    std::snprintf(line, sizeof line, "\nil2cpp_get_api_table @ %p (%s)\n%s\n",
                  reinterpret_cast<void*>(getTable), fnAt.text.c_str(),
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
