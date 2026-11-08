"""Assemble ootmenu's give-item hook (see patch.py for what it does) and print the bytes for patch.py.

    python hook_asm.py            (needs: pip install keystone-engine capstone)
"""
import sys
from pathlib import Path

import capstone
import keystone

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import patch  # noqa: E402

UNLOCK, GETINT, SETINT, ADD, NOTIFY = 0x3C42470, 0x163D20, 0x163B70, 0x110D570, 0x110D0B0
REMOVE = 0x33A4FA0  # pouch remove: (manager, &name, count, 0, 0, 0)
MAIL = patch.MAILBOX
TAG = patch.TAG
HOOK = patch.HOOK
# (field hash, stack slot): FN at sp+0x34, x0 at sp+0x50/0x54, x1 at sp+0x58/0x5c
CALL_FIELDS = [(0xC66E58F2, 0x34), (0x54E43D73, 0x50), (0x6206D0F1, 0x54), (0x4D22AE4A, 0x58), (0x10BCEEAC, 0x5C)]


def _getint(h, slot):
    return f"""    ldr x0, [x21]
    str wzr, [sp, #{slot:#x}]
    add x1, sp, #{slot:#x}
    movz w2, #{h & 0xFFFF:#x}
    movk w2, #{h >> 16:#x}, lsl #16
    bl #{GETINT:#x}
"""


CALL_ARGS = "".join(_getint(h, slot) for h, slot in CALL_FIELDS)
N = len(patch.ITEMS)

ASM = f"""
hookstart:
    stp x29, x30, [sp, #-0xd0]!
    mov x29, sp
    stp x19, x20, [sp, #0x10]
    stp x21, x22, [sp, #0x20]
    bl #{UNLOCK:#x}                  // what the site did: unlock GameData (x0 = lock)
    adrp x21, #0x4c7e000
    ldr x21, [x21, #0x180]           // &GameData manager
    ldr x0, [x21]
    cbz x0, done
    str wzr, [sp, #0x30]
    add x1, sp, #0x30
    movz w2, #{MAIL & 0xFFFF:#x}
    movk w2, #{MAIL >> 16:#x}, lsl #16
    bl #{GETINT:#x}                  // GetInt(mgr, &v, MAILBOX)
    ldr w19, [sp, #0x30]
    cbz w19, done
    // Only values tagged 0x0CA7xxxx are ours: the game itself writes this field (save-date
    // text: SimpleIntForMessage3 = day), so anything else is left alone.
    lsr w8, w19, #16
    mov w9, #{TAG:#x}
    cmp w8, w9
    b.ne done
    ldr x0, [x21]
    mov w1, #0
    movz w2, #{MAIL & 0xFFFF:#x}
    movk w2, #{MAIL >> 16:#x}, lsl #16
    bl #{SETINT:#x}                  // SetInt(mgr, 0, MAILBOX): handle each request once
    and w20, w19, #0xff00            // bit 8 = IsEquipDirect, bit 9 = remove
    and w19, w19, #0xff              // item number, or 0x7f = call request
    cmp w19, #0x7f
    b.ne items                       // 0x7f: call main+FN(x0, x1), all read from message scratch ints
{CALL_ARGS}
    adr x8, hookstart                // main base = this hook's address - HOOK offset
    movz x9, #{HOOK & 0xFFFF:#x}
    movk x9, #{HOOK >> 16:#x}, lsl #16
    sub x8, x8, x9
    ldr w9, [sp, #0x34]
    add x8, x8, x9
    ldr x0, [sp, #0x50]
    ldr x1, [sp, #0x58]
    blr x8
    b done
items:
    cbz w19, done
    cmp w19, #{N}
    b.hi done
    adr x9, names
    sub w10, w19, #1
    add x9, x9, x10, lsl #5
    add x10, sp, #0x4c               // name struct at sp+0x40: {{top, size=0x80, buf}}
    str x10, [sp, #0x40]
    mov w11, #0x80
    str w11, [sp, #0x48]
    ldp x12, x13, [x9]
    stp x12, x13, [x10]
    ldp x12, x13, [x9, #16]
    stp x12, x13, [x10, #16]
    strb wzr, [x10, #32]
    adrp x8, #0x4c7e000
    ldr x8, [x8, #0xd0]
    ldr x0, [x8]                     // pouch manager
    cbz x0, done
    add x1, sp, #0x40
    tbz w20, #9, give                // bit 9 = remove
    // Remove one, as the game's AddOrRemovePouchContent node does for Num < 0 (main+0x28b5140):
    // main+0x33a4fa0(pouch manager, &name, count, 0, 0, 0). (AddPouch with a negative count does nothing.)
    mov w2, #1
    mov w3, #0
    mov w4, #0
    mov w5, #0
    bl #{REMOVE:#x}
    b hud
give:
    mov w2, #0
    mov w3, #1                       // count
    mov x4, #0                       // reason
    mov w5, #0
    mov w6, #0
    ubfx w7, w20, #8, #1             // IsEquipDirect from the request (the Saria scene passes false)
    sub sp, sp, #0x10
    strb wzr, [sp]                   // stack argument (bool)
    bl #{ADD:#x}
    add sp, sp, #0x10
    and w2, w0, #1
    adrp x8, #0x4e6b000
    add x8, x8, #0x718
    ldr x0, [x8]
    cbz x0, done
    add x1, sp, #0x40
    mov x3, #0
    bl #{NOTIFY:#x}
hud:
    // Refresh the HUD exactly as the SeqOneShotResetPlayerStatus node (main+0x32ddf40) does:
    // screen = UI manager's PlayerStatus screen; screen+0x384 = -1; screen+0x388 = 1;
    // pouch manager bytes +0x3b4d = 0, +0x3b49 (u16) = 0.
    adrp x8, #0x4c7e000
    ldr x8, [x8, #0x98]
    ldr x20, [x8]                    // UI manager
    cbz x20, done
    ldr x0, [x20, #0x48]
    ldr x8, [x0]
    ldr x8, [x8, #0x50]
    movz w1, #0x1c3c
    movk w1, #0x5c7f, lsl #16        // PlayerStatus screen
    blr x8                           // -> screen index
    ldr w8, [x20, #8]
    ldr x9, [x20, #0x10]
    cmp w0, w8
    csel w8, w0, wzr, lo
    ldr x0, [x9, w8, sxtw #3]
    adrp x8, #0x4c7e000
    ldr x8, [x8, #0x778]
    ldarb w8, [x8]                   // (the game uses ldaprb; ldarb is the same acquire load, stronger ordering)
    tbz w8, #0, done                 // type info not initialised yet
    cbz x0, done
    ldr x8, [x0]
    adrp x1, #0x4c7e000
    ldr x1, [x1, #0x770]
    ldr x8, [x8, #0x10]
    blr x8                           // checked cast to the PlayerStatus screen type
    cbz x0, done
    mov w8, #-1
    str w8, [x0, #0x384]
    adrp x8, #0x4c7e000
    ldr x8, [x8, #0xd0]
    ldr x8, [x8]
    mov w9, #0x3b4d
    strb wzr, [x8, x9]
    mov w9, #0x3b49
    strh wzr, [x8, x9]
    mov w8, #1
    strb w8, [x0, #0x388]
done:
    ldp x19, x20, [sp, #0x10]
    ldp x21, x22, [sp, #0x20]
    ldp x29, x30, [sp], #0xd0
    ret
    .balign 32
names:
"""


def main():
    ks = keystone.Ks(keystone.KS_ARCH_ARM64, keystone.KS_MODE_LITTLE_ENDIAN)
    code = bytes(ks.asm(ASM, patch.HOOK)[0])
    site = bytes(ks.asm(f"b #{patch.HOOK:#x}", patch.SITE)[0])
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for i in md.disasm(code, patch.HOOK):
        print(f"  {i.address:#x}: {i.mnemonic:<6} {i.op_str}")
    print("SITE_BYTES:", site.hex())
    print("HOOK_BYTES (code, names appended by patch.py):", code.hex())
    print("matches patch.py:", site == patch.SITE_BYTES and patch.HOOK_BYTES.startswith(code))


if __name__ == "__main__":
    main()
