/* main+82C690, inside the abyss classifier. The continuation uses w8/x19,
 * then overwrites x9..x11; its epilogue restores d8/x19..x21/fp/lr.
 * No caller-saved SIMD register, NZCV or other volatile GPR is live here.
 * Preserve w8 and LR across the C predicate; it preserves the callee-saved
 * registers. Never return with RET: this is a branch into a native frame. */
.section .text.void_state_entry, "ax", %progbits
.align 2
.global void_state_entry
.hidden void_state_entry
.type void_state_entry, %function
void_state_entry:
  stp x8, x30, [sp, #-16]!
  mov x0, x19
  mov w1, w8
  bl voids_transition_target
  mov x16, x0
  ldp x8, x30, [sp], #16
  br x16
.size void_state_entry, .-void_state_entry
