#!/usr/bin/env python3
# AP_FLAKE8_CLEAN
"""Check the real ARM USB PMA copy code at -O3 using Unicorn Cortex-M0.

Requires arm-none-eabi-gcc on PATH, and Python packages unicorn and pyelftools.
No hardware is needed. This tests memory accesses, not USB enumeration.
Use --source to run the same checks against an older HAL implementation.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MODE_MCLASS, UC_MODE_THUMB
from unicorn.arm_const import (
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
    UC_ARM_REG_R3, UC_ARM_REG_SP, UC_CPU_ARM_CORTEX_M0,
)

FLASH = 0x08000000
RAM = 0x20000000
USB = 0x40005C00
PMA = USB + 0x400
RETURN = FLASH + 0xF000


def compile_hal(root, source, output):
    obj = output.with_suffix('.o')
    subprocess.run([
        'arm-none-eabi-gcc', '-mcpu=cortex-m0', '-mthumb', '-O3', '-std=gnu11',
        '-DSTM32F072xB', '-ffunction-sections', '-fdata-sections',
        '-I' + str(root / 'inc'), '-I' + str(root / 'inc/f0'),
        '-I' + str(root / 'hal/stm32f0xx/Inc'), '-c', str(source), '-o', str(obj),
    ], check=True)
    subprocess.run([
        'arm-none-eabi-gcc', '-mcpu=cortex-m0', '-mthumb', '-nostdlib',
        '-Wl,--gc-sections,-e,PCD_WritePMA,-u,PCD_ReadPMA,-Ttext=0x08000000',
        str(obj), '-o', str(output),
    ], check=True)


def check(elf_path):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M0)
    uc.mem_map(FLASH, 0x10000)
    uc.mem_map(RAM, 0x4000)
    uc.mem_map(PMA, 0x1000)
    with elf_path.open('rb') as stream:
        elf = ELFFile(stream)
        for section in elf.iter_sections():
            if section['sh_flags'] & 2 and section['sh_type'] != 'SHT_NOBITS':
                uc.mem_write(section['sh_addr'], section.data())
        symbols = {sym.name: sym['st_value'] for sym in elf.get_section_by_name('.symtab').iter_symbols()}

    errors = []
    bounds = {}

    def access_hook(_uc, access, address, size, value, data):
        if PMA <= address < PMA + 0x1000:
            if size != 2 or address % 2:
                errors.append(f'PMA access width/alignment: {size} bytes at {address:#x}')
            lo, hi = bounds['pma']
        elif RAM <= address < RAM + 0x1000:
            lo, hi = bounds['ram']
        else:
            return  # Stack traffic is outside the user-buffer region.
        if address < lo or address + size > hi:
            errors.append(f'Out-of-bounds access: {size} bytes at {address:#x}, allowed [{lo:#x}, {hi:#x})')

    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, access_hook)
    lengths = list(range(65)) + [127, 128, 255, 256, 511, 512]
    total = failed = 0
    examples = []
    width_example = None
    for name in ('PCD_WritePMA', 'PCD_ReadPMA'):
        for length in lengths:
            for alignment in range(4):
                for offset in (0x40, 0x42):
                    total += 1
                    errors.clear()
                    buffer = RAM + 0x100 + alignment
                    packet = PMA + offset
                    rounded = (length + 1) & ~1
                    payload = bytes((i * 37 + length) & 0xFF for i in range(length))
                    uc.mem_write(RAM, b'\xA5' * 0x1000)
                    uc.mem_write(PMA, b'\x5A' * 0x1000)
                    if name == 'PCD_WritePMA':
                        uc.mem_write(buffer, payload)
                    else:
                        uc.mem_write(packet, payload)
                    ram_before = bytes(uc.mem_read(RAM, 0x1000))
                    pma_before = bytes(uc.mem_read(PMA, 0x1000))
                    bounds.update(ram=(buffer, buffer + length), pma=(packet, packet + rounded))
                    for reg, value in (
                        (UC_ARM_REG_R0, USB), (UC_ARM_REG_R1, buffer),
                        (UC_ARM_REG_R2, offset), (UC_ARM_REG_R3, length),
                        (UC_ARM_REG_SP, RAM + 0x3F00), (UC_ARM_REG_LR, RETURN | 1),
                    ):
                        uc.reg_write(reg, value)
                    try:
                        uc.emu_start(symbols[name], RETURN, count=20000)
                        if uc.reg_read(UC_ARM_REG_PC) != RETURN:
                            errors.append('Function did not return within instruction budget')
                    except UcError as exc:
                        errors.append(str(exc))
                    ram_after = bytes(uc.mem_read(RAM, 0x1000))
                    pma_after = bytes(uc.mem_read(PMA, 0x1000))
                    if name == 'PCD_WritePMA':
                        expected = bytearray(pma_before)
                        expected[offset:offset + length] = payload
                        # The unused high byte of an odd final PMA word is unspecified.
                        if length % 2:
                            expected[offset + length] = pma_after[offset + length]
                        if pma_after != expected or ram_after != ram_before:
                            errors.append('Payload or guard bytes changed incorrectly')
                    else:
                        expected = bytearray(ram_before)
                        start = buffer - RAM
                        expected[start:start + length] = payload
                        if ram_after != expected or pma_after != pma_before:
                            errors.append('Payload or guard bytes changed incorrectly')
                    if errors:
                        failed += 1
                        for error in errors:
                            if error.startswith('PMA access width') and width_example is None:
                                width_example = f'{name} length={length}: {error}'
                        if len(examples) < 8:
                            examples.append(f'{name} length={length} alignment={alignment} offset={offset:#x}: {errors[0]}')
    if width_example:
        print('First invalid PMA access:', width_example)
    for example in examples:
        print(example)
    print(f'{total - failed}/{total} cases passed; {failed} failed')
    return failed == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    source = args.source or args.root / 'hal/stm32f0xx/Src/stm32f0xx_hal_pcd.c'
    with tempfile.TemporaryDirectory(prefix='node-usb-pma-') as temporary:
        elf = Path(temporary) / 'pma.elf'
        compile_hal(args.root, source, elf)
        return 0 if check(elf) else 1


if __name__ == '__main__':
    raise SystemExit(main())
