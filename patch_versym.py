#!/usr/bin/env python3
"""把指定符号的 glibc 版本标签改指向一个本机存在的最低版本。

背景: 在 glibc 2.31+ 上编译的二进制，会把 __libc_start_main、pthread_*
这类老符号也打上新标签（2.31+ 把 libpthread 并入 libc）。这些函数 ABI 与
GLIBC_2.17 完全一致，只是标签虚高。

脚本做两件事：
  1. 把该符号在 .gnu.version (versym) 里的版本索引改编到目标版本；
  2. 若原版本已无符号引用，把它从 .gnu.version_r 的 verneed/vernaux 链上
     摘掉 —— 加载器会逐条校验 verneed 记录，残留即报 version not found。
"""
import struct
import sys


class ELF:
    def __init__(self, data):
        self.data = data
        assert data[:4] == b"\x7fELF" and data[4] == 2, "仅支持 ELF64"
        self.e_shoff, = struct.unpack_from("<Q", data, 0x28)
        self.e_shentsize, self.e_shnum, self.e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            (name, typ, flags, addr, offset, size, link, info,
             align, entsize) = struct.unpack_from("<IIQQQQIIQQ", data, off)
            self.sections.append(dict(name_off=name, type=typ, offset=offset,
                                      size=size, link=link, entsize=entsize))
        shstr = self.sections[self.e_shstrndx]
        tab = data[shstr["offset"]:shstr["offset"] + shstr["size"]]
        for s in self.sections:
            s["name"] = self._str(tab, s["name_off"])
        st = self.find(".dynstr")
        self.dynstr = data[st["offset"]:st["offset"] + st["size"]]

    @staticmethod
    def _str(tab, off):
        end = tab.find(b"\x00", off)
        return tab[off:end].decode("utf-8", "replace")

    def find(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s

    def dynsyms(self):
        ds = self.find(".dynsym")
        out = []
        for i in range(ds["size"] // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = \
                struct.unpack_from("<IBBHQQ", self.data, ds["offset"] + i * 24)
            out.append((i, self._str(self.dynstr, st_name), st_shndx))
        return out

    def verneeds(self):
        """返回 verneed 记录列表，每条含其 vernaux 子项（含文件内偏移）。"""
        vn = self.find(".gnu.version_r")
        out = []
        off = vn["offset"]
        while off < vn["offset"] + vn["size"]:
            vn_version, vn_cnt, vn_file, vn_aux, vn_next = \
                struct.unpack_from("<HHIII", self.data, off)
            auxes = []
            aux = off + vn_aux
            for _ in range(vn_cnt):
                vna_hash, vna_flags, vna_other, vna_name, vna_next = \
                    struct.unpack_from("<IHHII", self.data, aux)
                auxes.append(dict(off=aux, hash=vna_hash, idx=vna_other,
                                  name=self._str(self.dynstr, vna_name),
                                  next=vna_next))
                if vna_next == 0:
                    break
                aux += vna_next
            out.append(dict(off=off, cnt=vn_cnt, auxes=auxes,
                            aux_off=vn_aux, next=vn_next))
            if vn_next == 0:
                break
            off += vn_next
        return out


def main():
    path, symname, want_ver = sys.argv[1], sys.argv[2], sys.argv[3]
    data = bytearray(open(path, "rb").read())
    elf = ELF(bytes(data))

    vs = elf.find(".gnu.version")
    if vs is None:
        print("没有 .gnu.version，跳过")
        return 0

    records = elf.verneeds()
    name_of, want_idx = {}, None
    for rec in records:
        for a in rec["auxes"]:
            name_of[a["idx"] & 0x7FFF] = a["name"]
            if a["name"] == want_ver:
                want_idx = a["idx"] & 0x7FFF
    if want_idx is None:
        print("文件中没有 %s 版本记录，无法指向" % want_ver)
        return 1

    targets = [(i, n) for (i, n, shndx) in elf.dynsyms() if n == symname and shndx == 0]
    if not targets:
        print("未找到未定义符号 %s" % symname)
        return 1

    dead, changed = set(), 0
    for idx, _ in targets:
        off = vs["offset"] + idx * 2
        cur, = struct.unpack_from("<H", data, off)
        cur_idx = cur & 0x7FFF
        if cur_idx == want_idx:
            print("%s 已是 %s，无需修改" % (symname, want_ver))
            continue
        struct.pack_into("<H", data, off, want_idx)
        print("%s: %s -> %s" % (symname, name_of.get(cur_idx, "?"), want_ver))
        dead.add(cur_idx)
        changed += 1

    if not changed:
        return 0

    # 还有符号在用的版本不能摘
    still_used = set()
    for i in range(vs["size"] // 2):
        v, = struct.unpack_from("<H", data, vs["offset"] + i * 2)
        still_used.add(v & 0x7FFF)

    for d in sorted(dead - still_used):
        for rec in elf.verneeds():
            auxes = rec["auxes"]
            pos = next((k for k, a in enumerate(auxes) if (a["idx"] & 0x7FFF) == d), None)
            if pos is None:
                continue
            a = auxes[pos]
            if len(auxes) == 1:
                struct.pack_into("<H", data, rec["off"] + 2, 0)      # vn_cnt = 0
            elif pos == 0:
                struct.pack_into("<I", data, rec["off"] + 8,          # vn_aux -> 下一项
                                 auxes[1]["off"] - rec["off"])
                struct.pack_into("<H", data, rec["off"] + 2, len(auxes) - 1)
            else:
                prev = auxes[pos - 1]
                struct.pack_into("<I", data, prev["off"] + 12,        # vna_next 相对自身
                                 (a["off"] + a["next"]) - prev["off"] if a["next"] else 0)
                struct.pack_into("<H", data, rec["off"] + 2, len(auxes) - 1)
            print("摘除版本需求: %s" % a["name"])
            break

    open(path, "r+b").write(bytes(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
