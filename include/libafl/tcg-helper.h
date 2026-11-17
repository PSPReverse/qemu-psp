/*
 * No TCG_CALL_NO_RWG here: both helpers can leave the cpu loop and the exit
 * path reads the guest registers, so TCG must write its register-cached
 * globals back to the CPU state before the call. With the flag a breakpoint
 * after a register-writing instruction of the same block resumed with the
 * stale register value.
 */
DEF_HELPER_FLAGS_2(libafl_qemu_handle_breakpoint, 0, void, env, i64)
DEF_HELPER_FLAGS_3(libafl_qemu_handle_custom_insn, 0, void, env, i64, i32)
