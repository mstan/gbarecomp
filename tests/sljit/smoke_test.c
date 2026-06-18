/* tests/sljit/smoke_test.c — P2 sljit vendoring smoke test.
 *
 * Proves the vendored sljit (lib/sljit, BSD-2-Clause) produces working host
 * machine code on this build/host BEFORE the ARM Instr -> sljit emitter is
 * built on top of it (SLJIT.md P2). Mirrors the proven psxrecomp selftest:
 * JIT `sljit_sw f(sljit_sw a) { return a + 1234; }`, run it, check the result.
 *
 * Exit 0 = pass, non-zero = fail (wired into ctest as `sljit_smoke`). This is
 * the host-capability canary: if codegen or the executable allocator don't work
 * here, every higher emitter layer is moot, so we gate on it first. */

#include <stdio.h>

#include "sljitLir.h"

typedef sljit_sw(SLJIT_FUNC *SmokeFn)(sljit_sw);

int main(void) {
    struct sljit_compiler *C = sljit_create_compiler(NULL);
    if (!C) {
        printf("sljit_smoke: create_compiler failed\n");
        return 1;
    }

    /* `sljit_sw f(sljit_sw a)`: one word arg arrives in saved reg S0; ask for
     * one scratch, one saved, no locals. */
    sljit_emit_enter(C, 0, SLJIT_ARGS1(W, W), 1, 1, 0);
    sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_S0, 0, SLJIT_IMM, 1234);
    sljit_emit_return(C, SLJIT_MOV, SLJIT_R0, 0);

    void *code = sljit_generate_code(C, 0, NULL);
    sljit_uw code_size = sljit_get_generated_code_size(C);
    sljit_free_compiler(C);
    if (!code) {
        printf("sljit_smoke: generate_code failed\n");
        return 1;
    }

    SmokeFn fn = (SmokeFn)code;
    sljit_sw got = fn(1000);
    sljit_free_code(code, NULL);

    if (got != 2234) {
        printf("sljit_smoke: FAIL f(1000)=%ld expected 2234\n", (long)got);
        return 1;
    }
    printf("sljit_smoke: OK f(1000)=%ld (a+1234), code_size=%lu bytes\n",
           (long)got, (unsigned long)code_size);
    return 0;
}
