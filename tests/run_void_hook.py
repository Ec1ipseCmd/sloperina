#!/usr/bin/env python3
"""Optional AArch64 hook execution check (pip install unicorn).

Run after a native build. Pass --tool-prefix /path/to/aarch64-none-elf-.
Only the compiled mod and generated IPS are used; no game data is needed.
The C predicate is ABI-mocked here and covered separately by run_voids.py.
"""
import argparse
import os
from pathlib import Path
import struct, subprocess
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tool-prefix', default='aarch64-none-elf-')
parser.add_argument('--elf', type=Path, default=root/'bin/v131072/ootgz.elf')
args=parser.parse_args()
prefix=args.tool_prefix
elf=args.elf.resolve()
flags={'creationflags':subprocess.CREATE_NO_WINDOW} if os.name=='nt' else {}
suffix='.exe' if os.name=='nt' else ''
out=root/'obj/tests/void-shim-text.bin'
out.parent.mkdir(parents=True,exist_ok=True)
symbols=subprocess.check_output([str(prefix)+'nm'+suffix,'-n',str(elf)],text=True,**flags)
sym={line.split()[2]:int(line.split()[0],16) for line in symbols.splitlines() if len(line.split())==3}
subprocess.run([str(prefix)+'objcopy'+suffix,'-O','binary','-j','.text',str(elf),str(out)],check=True,**flags)
assert sym['__module_start']==0
code=out.read_bytes()
patch=next(elf.parent.glob('*.ips')).read_bytes()
records={};i=5
while i<len(patch)-4:
 a,n=struct.unpack_from('>IH',patch,i);records[a-0x100]=patch[i+6:i+6+n];i+=6+n
main=0x80000000; module=0x100000; player=0x2000000;stack=0x3008000
regs=[globals()['UC_ARM64_REG_X'+str(i)] for i in range(29)]+[UC_ARM64_REG_FP,UC_ARM64_REG_LR]
for installed,blocked in ((False,False),(True,False),(True,True)):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 u.mem_map(module,(len(code)+4095)&~4095);u.mem_write(module,code)
 for page in (main+0x82c000,main+0x3c46000,main+0x4e9a000):u.mem_map(page,4096)
 for a in (0x82c690,0x3c464a0):u.mem_write(main+a,records[a])
 u.mem_write(main+0x4e9a4e8,struct.pack('<Q',module+sym['void_state_entry'] if installed else 0))
 u.mem_map(player,0x2000);u.mem_map(stack-0x8000,0x10000)
 for i,r in enumerate(regs):u.reg_write(r,0x10000+i)
 u.reg_write(UC_ARM64_REG_X8,9);u.reg_write(UC_ARM64_REG_X19,player);u.reg_write(UC_ARM64_REG_SP,stack)
 saved={r:u.reg_read(r) for r in regs[19:]+[UC_ARM64_REG_X8,UC_ARM64_REG_SP]}
 calls=[]
 def hook(uc,pc,size,unused):
  if pc==module+sym['voids_transition_target']:
   assert uc.reg_read(UC_ARM64_REG_X0)==player and uc.reg_read(UC_ARM64_REG_X1)==9
   assert uc.reg_read(UC_ARM64_REG_SP)%16==0
   ret=uc.reg_read(UC_ARM64_REG_LR)
   calls.append(pc)
   # An ABI-conforming C function may destroy all these registers/flags.
   for r in regs[:19]:uc.reg_write(r,0xDEAD)
   uc.reg_write(UC_ARM64_REG_LR,0xBAD)
   uc.reg_write(UC_ARM64_REG_NZCV,0xF0000000)
   uc.reg_write(UC_ARM64_REG_X0,main+(0x82c614 if blocked else 0x3c464b0))
   uc.reg_write(UC_ARM64_REG_PC,ret)
  elif pc in (main+0x82c694,main+0x82c614):uc.emu_stop()
 u.hook_add(UC_HOOK_CODE,hook)
 u.emu_start(main+0x82c690,0,count=100)
 assert u.reg_read(UC_ARM64_REG_PC)==main+(0x82c614 if blocked else 0x82c694)
 assert len(calls)==int(installed)
 assert all(u.reg_read(r)==v for r,v in saved.items())
 assert struct.unpack('<I',u.mem_read(player+0x191c,4))[0]==(0 if blocked else 9)
 print('PASS AArch64 execution: module',installed,'blocked',blocked,'native state/store, live registers, LR and stack')
