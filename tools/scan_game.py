#!/usr/bin/env python3
"""崩铁引擎库体检 / 地址表生成。

HSRBox 需要游戏自己那个 so 里一批 Unity 引擎函数的 RVA。这些地址随游戏版本变，所以每个版本一份表。
这个脚本把"这一版要什么、已经有什么"列清楚，并把结果写成 generated_offsets.inc。

用法：

  # 1) 直接读 APK（会列出里面的 so、Build ID、导出的 il2cpp API、Unity 版本）
  python3 tools/scan_game.py HonkaiStarRail.apk

  # 2) 读单个 so
  python3 tools/scan_game.py lib/arm64-v8a/libil2cpp.so

  # 3) 把填好的地址表编译进模块
  #    offsets.csv 每行：name,rva[,入口16字节hex]
  python3 tools/scan_game.py libil2cpp.so --map offsets.csv \
      --emit app/src/main/cpp/generated_offsets.inc

  # 4) 只想要一个空模板去填
  python3 tools/scan_game.py libil2cpp.so --template offsets.csv

不依赖任何第三方包：ELF 解析是这里自带的一小段。
"""
import argparse
import os
import struct
import sys
import zipfile

# ---------------------------------------------------------------- 需要的项
# 名字必须与 app/src/main/cpp/offsets.hpp 的 enum Offset 完全一致、顺序一致。
OFFSETS = [
    "tick", "manager",
    "getMesh", "setMesh", "getBones", "getMaterials", "setMaterials",
    "clone", "destroy", "getName", "getParent",
    "arrayNew", "objectNew", "pin", "unpin",
    "setReadable", "setIndexFormat", "getBindposes", "getPositions", "setPositions", "setNormals",
    "getUV", "setUV", "setUV2", "setUV3", "setUV4", "getColors", "setColors", "getTangents", "setTangents",
    "getTriangles", "setTriangles", "setSubmeshCount", "setSubmeshTriangles", "meshFinish", "meshUpload",
    "meshDynamic", "frameCount",
    "textureType", "textureInit", "texturePixels", "textureApply",
    "setHideFlags", "getHideFlags",
    "mainTexture", "newString", "hasProperty", "setTexture",
    "getTexture", "getShader", "getTextureIds", "propertyToId",
    "nativeAlloc", "nativeAllocator",
    "findObjectsOfType", "findObjectsNative",
    "componentsInChildren", "gameObject",
    "uiTextSet", "uiTextPopulate", "uiVertexClear", "behaviourEnabled",
    "componentByName", "lodPause", "lodResume", "lodSelfPaused", "lodCurrent",
    "receive",
    "lodBind", "meshBones",
    "wrapperFor",
    "typeObject", "classFromType",
    "meshSubmeshes", "meshVertices", "meshReadable", "meshIndexStart", "meshIndexCount",
    "textureWidth", "textureHeight", "textureFormat", "textureMips",
]
# 归 libil2cpp.so 的项：il2cpp 运行时（对象/数组分配、反射、脚本包装、字符串、GC 句柄）。
# 其余全在 libunity.so。与 offsets.hpp 的 abi::libOf 必须一致。
MANAGED = {"manager", "arrayNew", "objectNew", "typeObject", "classFromType",
           "wrapperFor", "newString", "pin", "unpin"}
def lib_of(name):
    return "libil2cpp.so" if name in MANAGED else "libunity.so"

# 这些项在挂钩前要逐条比对入口字节；表里没给 hex 就只做地址检查。
CHECKED = ("componentsInChildren", "gameObject", "receive", "lodBind", "meshBones")

# libil2cpp 导出的 API 与 HSRBox 能用上的对应关系（有导出就不需要 RVA）。
IL2CPP_EXPORTS = {
    "il2cpp_domain_get", "il2cpp_domain_get_assemblies", "il2cpp_domain_assembly_open",
    "il2cpp_assembly_get_image", "il2cpp_class_from_name", "il2cpp_class_get_method_from_name",
    "il2cpp_class_get_methods", "il2cpp_class_get_field_from_name", "il2cpp_field_get_offset",
    "il2cpp_method_get_pointer", "il2cpp_runtime_invoke", "il2cpp_object_new",
    "il2cpp_string_new", "il2cpp_thread_attach", "il2cpp_object_get_class",
    "il2cpp_class_get_name", "il2cpp_class_get_namespace", "il2cpp_free",
}


# ---------------------------------------------------------------- 极简 ELF64
class Elf:
    def __init__(self, data, path):
        # data 可以是 bytes 也可以是 mmap（libil2cpp.so 常常上百 MB，别整个读进来）
        self.data = data
        self.path = path
        if len(data) < 64 or data[:4] != b"\x7fELF":
            raise ValueError("不是 ELF 文件：" + path)
        if data[4] != 2:
            raise ValueError("只处理 64 位 ELF（HSRBox 只发 arm64-v8a）：" + path)
        if data[5] != 1:
            raise ValueError("只处理小端 ELF：" + path)
        (self.machine,) = struct.unpack_from("<H", data, 18)
        (self.phoff,) = struct.unpack_from("<Q", data, 32)
        (self.shoff,) = struct.unpack_from("<Q", data, 40)
        (self.phentsize,) = struct.unpack_from("<H", data, 54)
        (self.phnum,) = struct.unpack_from("<H", data, 56)
        (self.shentsize,) = struct.unpack_from("<H", data, 58)
        (self.shnum,) = struct.unpack_from("<H", data, 60)
        (self.shstrndx,) = struct.unpack_from("<H", data, 62)
        self.phdrs = [self._phdr(i) for i in range(self.phnum)]
        self.shdrs = [self._shdr(i) for i in range(self.shnum)]

    def _phdr(self, i):
        off = self.phoff + i * self.phentsize
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", self.data, off)
        return dict(type=p_type, flags=p_flags, offset=p_offset, vaddr=p_vaddr,
                    filesz=p_filesz, memsz=p_memsz)

    def _shdr(self, i):
        off = self.shoff + i * self.shentsize
        name, sh_type, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from(
            "<IIQQQQIIQQ", self.data, off)
        return dict(name=name, type=sh_type, flags=flags, addr=addr, offset=offset,
                    size=size, link=link, info=info, align=align, entsize=entsize)

    def sec_name(self, sh):
        if self.shstrndx >= len(self.shdrs):
            return ""
        strtab = self.shdrs[self.shstrndx]
        base = strtab["offset"] + sh["name"]
        end = self.data.find(b"\0", base)
        return self.data[base:end].decode("utf-8", "replace")

    def build_id(self):
        for ph in self.phdrs:
            if ph["type"] != 4:      # PT_NOTE
                continue
            off, end = ph["offset"], ph["offset"] + ph["filesz"]
            while off + 12 <= end:
                namesz, descsz, ntype = struct.unpack_from("<III", self.data, off)
                off += 12
                name = self.data[off:off + namesz].rstrip(b"\0")
                off += (namesz + 3) & ~3
                desc = self.data[off:off + descsz]
                off += (descsz + 3) & ~3
                if name == b"GNU" and ntype == 3:
                    return desc.hex()
        return ""

    def dynsyms(self):
        out = []
        for sh in self.shdrs:
            if sh["type"] not in (11,):          # SHT_DYNSYM
                continue
            strtab = self.shdrs[sh["link"]] if sh["link"] < len(self.shdrs) else None
            entsize = sh["entsize"] or 24
            for off in range(sh["offset"], sh["offset"] + sh["size"], entsize):
                if off + 24 > len(self.data):
                    break
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
                    "<IBBHQQ", self.data, off)
                if not st_name or not strtab:
                    continue
                base = strtab["offset"] + st_name
                end = self.data.find(b"\0", base)
                name = self.data[base:end].decode("utf-8", "replace")
                out.append(dict(name=name, value=st_value, size=st_size, shndx=st_shndx,
                                bind=st_info >> 4, type=st_info & 0xF))
        return out


# ---------------------------------------------------------------- 报告
def report(elf, label):
    print(f"== {label}")
    print(f"   文件      {os.path.basename(elf.path)}  ({len(elf.data):,} 字节)")
    arch = {183: "aarch64", 40: "arm", 62: "x86-64"}.get(elf.machine, f"machine {elf.machine}")
    print(f"   架构      {arch}")
    bid = elf.build_id()
    print(f"   Build ID  {bid or '（没有 .note.gnu.build-id）'}")
    ex = [s for s in elf.dynsyms() if s["bind"] == 1 and s["shndx"] != 0]
    print(f"   导出      动态符号 {len(ex)} 个，其中函数 {sum(1 for s in ex if s['type'] == 2)} 个")

    il2 = sorted(s for s in ex if s["name"].startswith("il2cpp_"))
    if il2:
        hit = [s["name"] for s in il2 if s["name"] in IL2CPP_EXPORTS]
        print(f"   il2cpp_*  导出 {len(il2)} 个，本端口需要的 {len(hit)}/{len(IL2CPP_EXPORTS)} 个")
        if hit:
            print("             可用：" + ", ".join(hit))
            print("             这一版可以走托管调用（il2cpp API），不必铺满原生 RVA。")
        elif il2 == ["il2cpp_get_api_table"]:
            print("             只有 il2cpp_get_api_table（崩铁 4.6.54 就是这种，而且是混淆过的桩）。")
            print("             托管调用这条路走不通，只能铺原生 RVA 表。")
        else:
            print("             有 il2cpp_ 前缀的符号，但都不是这个端口用的那几个。")
    else:
        print("   il2cpp_*  没有导出（单库构造，托管 API 走原生 RVA 表）")

    # Unity 版本：版本串就在 rodata 里
    import re
    vers = sorted({m.group(0).decode() for m in re.finditer(rb"20(1[6-9]|2[0-9])\.\d+\.\d+[abfp]\d+", elf.data)})
    if vers:
        print(f"   Unity     {', '.join(vers[:4])}")
    return bid


def template(rows_path):
    with open(rows_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# name,rva[,入口16字节hex]  —— rva 是 16 进制的文件偏移（相对模块基址）\n")
        for n in OFFSETS:
            tail = ", " if n in CHECKED else ""
            f.write(f"{n},0x0{tail}  # {lib_of(n)}\n")
    print(f"模板已写出：{rows_path}（{len(OFFSETS)} 行，rva 填 0x0 表示这一版还没找到）")


def read_map(rows_path):
    rva, hexes = {}, {}
    with open(rows_path, encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 2:
                raise SystemExit(f"地址表格式不对（要 name,rva[,hex]）：{raw!r}")
            name = parts[0]
            if name not in OFFSETS:
                raise SystemExit(f"地址表里有不认识的项：{name}")
            try:
                value = int(parts[1], 0)
            except ValueError:
                raise SystemExit(f"{name} 的 rva 不是数字：{parts[1]!r}")
            if value:
                rva[name] = value
            if len(parts) > 2 and parts[2]:
                h = parts[2].lower()
                if len(h) != 32 or any(c not in "0123456789abcdef" for c in h):
                    raise SystemExit(f"{name} 的入口字节要 32 个十六进制字符：{parts[2]!r}")
                hexes[name] = h
    return rva, hexes


def emit(build_id, managed_id, rva, hexes, out_path):
    missing = [n for n in OFFSETS if n not in rva]
    note = ("还缺：" + ", ".join(missing[:12]) + (" …" if len(missing) > 12 else "")) if missing else ""
    lines = [
        "// 由 tools/scan_game.py 生成，别手改：改 offsets.csv 再生成一次。",
        f"// libunity.so   {build_id}",
        f"// libil2cpp.so  {managed_id or '（单库构造，两套基址相同）'}",
        "inline std::vector<Profile>& profiles(){",
        " static std::vector<Profile> v{",
        '  Profile{"%s","%s","%s",0xc4e,0x0,0x0,0x0,{},{},{},' % (build_id, managed_id, note),
        "   {",
    ]
    for n in OFFSETS:
        if n in rva:
            lines.append("    {%s,0x%x}," % (n, rva[n]))
    lines.append("   },")
    lines.append("   {")
    for n, h in hexes.items():
        lines.append('    {%s,"%s"},' % (n, h))
    lines.append("   }}"); 
    lines.append(" };")
    lines.append(" return v;}")
    open(out_path, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print(f"已写出 {out_path}：{len(rva)}/{len(OFFSETS)} 项有地址"
          + ("，缺 " + ", ".join(missing) if missing else "，齐了"))
    print("记住 CMake 要带 -DHSRBOX_GENERATED_OFFSETS=ON，否则这个文件不会被 include。")


def apk_members(path):
    """返回 [(成员名, 大小)]，只列 arm64 的 so 和 metadata。"""
    with zipfile.ZipFile(path) as z:
        out = []
        for n in z.namelist():
            if n.endswith(".so") and "/arm64-v8a/" in n:
                out.append((n, z.getinfo(n).file_size))
            elif n.endswith("global-metadata.dat"):
                out.append((n, z.getinfo(n).file_size))
        return out


def open_member(path, member):
    """把 zip 里的成员流式解到临时文件，返回 (mmap, 临时路径)。"""
    import tempfile
    with zipfile.ZipFile(path) as z, z.open(member) as src:
        fd, tmp = tempfile.mkstemp(suffix=".so")
        with os.fdopen(fd, "wb") as dst:
            while True:
                chunk = src.read(1 << 20)
                if not chunk:
                    break
                dst.write(chunk)
    import mmap
    f = open(tmp, "rb")
    return mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ), tmp


def scan_input(path):
    """扫一个输入（APK / .so）。返回 [(标签, Build ID, tmp 路径或 None)]。"""
    found = []
    if zipfile.is_zipfile(path):
        members = apk_members(path)
        so_members = [(n, sz) for n, sz in members if n.endswith(".so")]
        print(f"== {os.path.basename(path)}：arm64-v8a 里 {len(so_members)} 个 so")
        for n, sz in so_members:
            print(f"   {sz:>12,}  {n}")
        with zipfile.ZipFile(path) as z:
            for n, _ in members:
                if n.endswith("global-metadata.dat"):
                    h = z.read(n)[:8]
                    ok = h[:4] == b"\xaf\x1b\xb1\xfa"
                    print(f"   metadata {n}：{'明文' if ok else '加密（头部 %s）' % h[:4].hex()}")
        for n, _ in so_members:
            data, tmp = open_member(path, n)
            try:
                elf = Elf(data, n)
                bid = report(elf, n)
                found.append((n, bid, tmp))
            except ValueError as e:
                print(f"   {n}：{e}")
                os.unlink(tmp)
        return found
    import mmap
    with open(path, "rb") as f:
        data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        elf = Elf(data, path)
        return [(path, report(elf, path), None)]


def pick_managed(found, want):
    if want:
        return want
    for pref in ("libil2cpp.so",):
        for name, bid, _ in found:
            if os.path.basename(name) == pref and bid:
                return bid
    return ""


def pick_build(found, want):
    """挑一个 so 的 Build ID 当这张表的 key。"""
    if want:
        for name, bid, _ in found:
            if want in name and bid:
                return bid
        raise SystemExit(f"没有名字含 {want!r} 的 so，或者它没有 Build ID")
    for pref in ("libunity.so", "libil2cpp.so", "libhkrpg.so", "libGame.so"):   # 地址表按引擎库走
        for name, bid, _ in found:
            if os.path.basename(name) == pref and bid:
                return bid
    for _, bid, _ in found:
        if bid:
            return bid
    raise SystemExit("这几个输入里没有任何 so 带 Build ID")


def main():
    ap = argparse.ArgumentParser(
        description="崩铁引擎库体检 / 地址表生成。可以给多个输入：base.apk + split_config.arm64_v8a.apk，或者一个目录。")
    ap.add_argument("libraries", nargs="+", help="APK / .so / 目录（split APK 全丢进来）")
    ap.add_argument("--lib", metavar="NAME", help="用哪个 so 的 Build ID（默认 libunity.so → libil2cpp.so → 第一个有 ID 的；崩铁的引擎在 libunity.so）")
    ap.add_argument("--build-id", metavar="HEX", help="直接指定引擎库 Build ID，绕过自动取")
    ap.add_argument("--managed-build-id", metavar="HEX", help="直接指定 libil2cpp.so 的 Build ID")
    ap.add_argument("--extract", metavar="DIR", help="把 arm64 的 so 都解出来（给 IDA 用）")
    ap.add_argument("--map", metavar="CSV", help="填好的地址表（name,rva[,hex]）")
    ap.add_argument("--emit", metavar="INC", help="写出 generated_offsets.inc")
    ap.add_argument("--template", metavar="CSV", help="写一个空地址表模板")
    args = ap.parse_args()

    inputs = []
    for lib in args.libraries:
        if not os.path.exists(lib):
            raise SystemExit("找不到：" + lib)
        if os.path.isdir(lib):
            inputs += [os.path.join(lib, f) for f in sorted(os.listdir(lib))
                       if f.lower().endswith((".apk", ".apks", ".xapk", ".so"))]
        else:
            inputs.append(lib)
    if not inputs:
        raise SystemExit("没有可扫的输入")

    found, tmps = [], []
    for path in inputs:
        try:
            got = scan_input(path)
        except ValueError as e:
            print(f"== 跳过 {path}：{e}")
            continue
        for name, bid, tmp in got:
            found.append((name, bid, tmp))
            if tmp:
                tmps.append(tmp)
        print()
    if not found:
        raise SystemExit("这些输入里没有 arm64 的 so")

    try:
        if args.extract:
            os.makedirs(args.extract, exist_ok=True)
            for name, _, tmp in found:
                if not tmp:
                    continue
                dst = os.path.join(args.extract, os.path.basename(name))
                with open(tmp, "rb") as src, open(dst, "wb") as out:
                    while True:
                        chunk = src.read(1 << 20)
                        if not chunk:
                            break
                        out.write(chunk)
                print(f"解出 {dst}")

        if args.template:
            template(args.template)

        if args.map:
            rva, hexes = read_map(args.map)
            if not rva:
                raise SystemExit("地址表里一个地址都没填")
            build = args.build_id or pick_build(found, args.lib)
            managed = args.managed_build_id or pick_managed(found, None)
            print(f"引擎库 Build ID {build}" + (f"，托管库 {managed}" if managed else "，没找到 libil2cpp.so"))
            emit(build, managed, rva, hexes, args.emit or "generated_offsets.inc")

        if not args.map and not args.template and not args.extract:
            print("下一步：python3 tools/scan_game.py " + " ".join(args.libraries)
                  + " --template offsets.csv")
            print("      填完 RVA 后：--map offsets.csv --emit app/src/main/cpp/generated_offsets.inc")
    finally:
        for tmp in tmps:
            if os.path.exists(tmp):
                os.unlink(tmp)


if __name__ == "__main__":
    main()
