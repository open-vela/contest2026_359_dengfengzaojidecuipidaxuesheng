"""Resolve invariant local control-flow relocations using GNU ld, not custom ISA encoders.

NuttX's RV32 loader asserts on valid zero-distance branch relocations. Local
branches and calls never depend on the load address. Resolve these at package
time, verify at two unrelated bases, and retain all address-dependent imports.
"""
from pathlib import Path
import struct
import subprocess


class Elf32:
    def __init__(self, data):
        self.data = bytearray(data)
        h = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
        if h[0][:7] != b'\x7fELF\x01\x01\x01' or h[2] != 243:
            raise ValueError('expected little-endian RV32 ELF')
        self.shoff, self.shsize = h[6], h[11]
        self.sections = [struct.unpack_from('<10I', data, h[6]+i*h[11]) for i in range(h[12])]
        strings = self.section(h[13])
        self.names = [strings[s[0]:].split(b'\0', 1)[0].decode() for s in self.sections]

    def section(self, index):
        s = self.sections[index]
        return bytes(self.data[s[4]:s[4]+s[5]])

    def named(self, name):
        return self.section(self.names.index(name))


def resolve_local(raw, output, prefix, directory):
    raw, output, directory = Path(raw), Path(output), Path(directory)
    elf = Elf32(raw.read_bytes())
    text_index = elf.names.index('.text')
    text = elf.sections[text_index]
    symindex = elf.names.index('.symtab')
    symbols = [struct.unpack_from('<IIIBBH', elf.data, p)
               for p in range(elf.sections[symindex][4],
                              elf.sections[symindex][4]+elf.sections[symindex][5], 16)]
    strings = elf.section(elf.sections[symindex][6])
    definitions = []
    for i, symbol in enumerate(symbols):
        if symbol[5] == 0 and symbol[0]:
            name = strings[symbol[0]:].split(b'\0', 1)[0].decode()
            definitions.append(f'{name} = 0x{0x40000000+i*16:x};')
    references = []
    for base in [0x10000000, 0x48000000]:
        script = directory/f'resolve-{base:x}.ld'
        # No relaxation, GC, or section reordering: preserve all input offsets.
        script.write_text('SECTIONS { . = 0x%x;\n'%base+
            '\n'.join(f' {name} : {{ *({name}) }}' for name in ['.text','.rodata','.data','.bss'])+
            '\n}\n'+'\n'.join(definitions)+'\n')
        linked = directory/f'resolve-{base:x}.elf'
        subprocess.run([prefix+'ld', '--no-relax', '-T', str(script), str(raw),
                        '-o', str(linked)], check=True, capture_output=True)
        ref = Elf32(linked.read_bytes())
        reference = ref.named('.text')
        if len(reference) != text[5]:
            raise ValueError('GNU reference changed text layout')
        references.append(reference)
    fixed = removed = normalized = 0
    for index, section in enumerate(elf.sections):
        if section[1] != 4:
            continue
        kept = bytearray()
        for p in range(section[4], section[4]+section[5], 12):
            offset, info, addend = struct.unpack_from('<IIi', elf.data, p)
            kind, symbol = info & 255, symbols[info >> 8]
            if kind == 51:  # R_RISCV_RELAX is a linker hint; ld used --no-relax.
                removed += 1
                continue
            local = section[7] == text_index and symbol[5] == text_index
            if kind in [16,17,44,45] and not local:
                raise ValueError('nonlocal branch relocation cannot be pre-resolved')
            if local and kind in [16,17,18,19,44,45]:
                size = 2 if kind in [44,45] else 8 if kind in [18,19] else 4
                if offset+size > text[5] or not 0 <= symbol[1]+addend < text[5]:
                    raise ValueError('local control flow outside text')
                a, b = (ref[offset:offset+size] for ref in references)
                if a != b:
                    raise ValueError('control flow depends on module load address')
                elf.data[text[4]+offset:text[4]+offset+size] = a
                fixed += 1
            else:
                # NuttX adds S-immediates/CALL fields instead of replacing
                # them. GNU assembler may already encode the RELA addend
                # (e.g. sw ..., symbol+16). Clear only those immediate bits;
                # the retained explicit addend supplies the full value.
                if section[7] == text_index and kind in [18,19,28]:
                    instruction = text[4]+offset
                    word = struct.unpack_from('<I',elf.data,instruction)[0]
                    if kind == 28:
                        if word & 0x7f not in [0x23,0x27]:
                            raise ValueError('LO12_S does not reference a store')
                        struct.pack_into('<I',elf.data,instruction,word & ~0xfe000f80)
                    else:
                        second = struct.unpack_from('<I',elf.data,instruction+4)[0]
                        if word & 0x7f != 0x17 or second & 0x7f != 0x67:
                            raise ValueError('CALL does not reference AUIPC/JALR')
                        struct.pack_into('<II',elf.data,instruction,word & 0xfff,second & 0xfffff)
                    normalized += 1
                kept += elf.data[p:p+12]
        elf.data[section[4]:section[4]+len(kept)] = kept
        struct.pack_into('<I', elf.data, elf.shoff+index*elf.shsize+20, len(kept))
    output.write_bytes(elf.data)
    return {'local_control_flow_resolved': fixed, 'relax_hints_removed': removed,
            'address_immediates_normalized': normalized,
            'gnu_reference_bases': ['0x10000000','0x48000000']}
