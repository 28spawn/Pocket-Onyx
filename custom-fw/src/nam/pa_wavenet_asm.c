/* custom-fw/src/nam/pa_wavenet_asm.c - the CPU stage of the WaveNet block engine in pi32v2 assembly (device only): wn_asm_mix and wn_asm_head.
 *
 * ORIGIN. Written from the specification alone: the C reference wn_post_mix_c / wn_post_head_c in pa_wavenet.c (the arithmetic below is theirs), the pi32v2
 * assembler of the JieLi toolchain and the repository's own
 * assembly (pa_biquad_asm.c).
 *
 * WHAT IT COMPUTES, per row of three channels (all integer, exact; K = 42949632 = 0.01 * 2^32):
 *     a_j     = z_j                         if z_j >= 0,   else the high word of the signed 64-bit product z_j * K     (= floor(z_j * K / 2^32))
 *     h_j     = sat32(h_j + a_j)                                                      (head: this is all wn_asm_head does)
 *     acc_o   = bias_o + sum_j w[3j+o] * a_j                                          (64 bits; bias_o = b64[2o+1]:b64[2o])
 *     dst_o   = sat32( (int32)(acc_o >> 24) + src_o )                                 (>> arithmetic, then the low word)
 * Every a_j feeds the head sum and all three outputs, so a row is: 3 leaky ReLUs, 3 saturating adds, 9 multiply-accumulates, 3 shifts, 3 saturating adds.
 *
 * WHAT THE MACHINE OFFERS (found with the assembler; these decide the design):
 *   - 64-bit multiply-accumulate in one instruction ("rH_rL += ra * rb (s)"), a 64-bit arithmetic shift of the pair ("rH_rL >>>= 24", the wanted word is then rL),
 *     and a saturating 32-bit add ("rd = ra + rb (ssat)"): the whole kernel is those three plus loads and stores.
 *   - "ifs (ra < 0) { ... }" runs its body only for a negative operand, so the leaky ReLU costs one slot for a positive z and three for a negative one.
 *   - Two instructions can issue as one pair "A # B": A any ALU/MAC/shift instruction, B a single load or store in the short encoding (data register r0-r7
 *     with a pointer-based address; any register with an sp-based one). Multi-register loads/stores, 64-bit loads, ifs blocks and branches do not pair.
 *     So the only way to hide a memory access is under an arithmetic instruction: a row has 18 arithmetic instructions (3 h-sums, 9 MACs, 3 shifts,
 *     3 residual adds) and the loop is arranged so that the 18 single loads/stores that remain (9 weights, 3 src, 3 dst, 3 h write-backs) each ride under one of
 *     them. The remaining unpaired slots are the loop's skeleton: z load, three ifs, h load, three 64-bit bias loads, the branch.
 *
 * DESIGN (mix; head is the same without the second half).
 *   Registers:  r0 z, r1 h, r2 src, r3 dst (pointers, post-incremented by their loads/stores: no address arithmetic in the loop)   r4-r6 h, then the residual and the result
 *               r8-r10 a_0..a_2   r11 K   r12:r13 the accumulator, which doubles as the product scratch of the leaky ReLU   r14 row count   r7, r15 weight operands, alternating.
 *   Prologue:   the 9 weights and 3 biases are copied onto the stack (two multi-register loads + two pushes) so that the loop can fetch any weight with one
 *               sp-relative load, which is the only addressing the pair slot allows for the high registers; r2/r3 are then reloaded with src/dst.
 *   Loop:       z (one multi-load) -> leaky ReLU on each channel -> h load -> h-sums, the MAC chains of the three outputs and the write-backs of h,
 *               src and dst scheduled together. The three MAC chains run back to back on the one accumulator; between outputs only the 64-bit bias load
 *               (unpaired) and the shift/add tail are serial. Weight w_k is loaded one bundle before its MAC into r7/r15 alternately, so
 *               a register is never written in the bundle that reads it.
 *   Hazards:    the two halves of a pair are always independent (no read/write overlap), and no result is consumed in the bundle that produced it,
 *               so the code does not depend on the order in which the halves of a pair take effect. test/pi32emu.py asserts the pair rule on every pair it runs.
 *   Contract:   rows == 0 returns at once; z, h, src, dst are distinct arrays (src and dst may be one buffer, element for element); only h and dst are written;
 *               r4-r15 and sp are preserved.
 *
 * COST, in issue slots (a pair counts once) per row, from the emulator (test/test_wn_asm_emu.py), against the C fallback's ~172 (mix) / ~66 (head) instructions
 * on its common path:   mix 28 (all z >= 0) .. 31 (random signs) .. 34 (all z < 0);   head 10 .. 13 .. 16.
 *
 * The kernels are checked against the C reference in the emulator on every build, and again on the unit at load (wn_asm_selftest in pa_wavenet.c), which keeps
 * the C fallback if a single word disagrees.
 */
#include "pa_config.h"

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM) && !defined(PA_EMU_DLL) && !defined(__wasm__) && !defined(__EMSCRIPTEN__)
__asm__(
	".text\n"
	".globl wn_asm_mix\n"
	".p2align 1\n"
	"wn_asm_mix:\n"
	"\t[--sp] = {rets, r15-r4}   ; save the callee registers (13 words)\n"
	"\t{r12-r4} = [r2++]                   ; the 9 weights ...\n"
	"\t[--sp] = {r12-r4}                 ; ... onto the stack: w[k] at sp+24+4k\n"
	"\t{r9-r4} = [r3++]                    ; the 3 biases (lo, hi words) ...\n"
	"\t[--sp] = {r9-r4}                  ; ... at sp+8o\n"
	"\tr14 = [sp+120]                       ; rows (stack argument 7: entry sp+8, 112 bytes pushed)\n"
	"\tr2 = [sp+112]                        ; src\n"
	"\tr3 = [sp+116]                        ; dst\n"
	"\tif (r14 == 0) goto .Lmix_done\n"
	"\tr11 = 42949632                        ; K\n"
	".Lmix_loop:\n"
	"\t{r10-r8} = [r0++]                            ; z of the row -> r8..r10\n"
	"\tifs (r8 < 0) {                               ; a0 = z < 0 ? hi32(z*K) : z\n"
	"\t\tr13_r12 = r8 * r11 (s)\n"
	"\t\tr8 = r13\n"
	"\t}\n"
	"\tifs (r9 < 0) {                               ; a1 = z < 0 ? hi32(z*K) : z\n"
	"\t\tr13_r12 = r9 * r11 (s)\n"
	"\t\tr9 = r13\n"
	"\t}\n"
	"\tifs (r10 < 0) {                              ; a2 = z < 0 ? hi32(z*K) : z\n"
	"\t\tr13_r12 = r10 * r11 (s)\n"
	"\t\tr10 = r13\n"
	"\t}\n"
	"\tr13_r12 = d[sp+0]                            ; acc = bias 0 (64 bit)\n"
	"\t{r6-r4} = [r1++]                             ; h of the row -> r4..r6, h += 12 bytes\n"
	"\tr4 = r4 + r8 (ssat) #                        ; h0 = sat(h0 + a0)\n"
	"\t r15 = [sp+24]                                   ; w[0]\n"
	"\tr13_r12 += r8 * r15 (s) #                    ; acc0 += w[0] * a0\n"
	"\t r7 = [sp+36]                                    ; w[3]\n"
	"\tr13_r12 += r9 * r7 (s) #                     ; acc0 += w[3] * a1\n"
	"\t r15 = [sp+48]                                   ; w[6]\n"
	"\tr13_r12 += r10 * r15 (s) #                   ; acc0 += w[6] * a2\n"
	"\t [r1+-12] = r4                                   ; h0 back\n"
	"\tr13_r12 >>>= 24 #                            ; low word (r12) = acc0 >> 24\n"
	"\t r4 = [r2++=4]                                   ; residual src[0]\n"
	"\tr4 = r12 + r4 (ssat) #                       ; dst[0] = sat(r12 + src[0])\n"
	"\t r7 = [sp+28]                                    ; w[1]\n"
	"\tr13_r12 = d[sp+8]                            ; acc = bias 1 (64 bit)\n"
	"\tr13_r12 += r8 * r7 (s) #                     ; acc1 += w[1] * a0\n"
	"\t r15 = [sp+40]                                   ; w[4]\n"
	"\tr5 = r5 + r9 (ssat) #                        ; h1 = sat(h1 + a1)\n"
	"\t r7 = [sp+52]                                    ; w[7]\n"
	"\tr13_r12 += r9 * r15 (s) #                    ; acc1 += w[4] * a1\n"
	"\t [r1+-8] = r5                                    ; h1 back\n"
	"\tr13_r12 += r10 * r7 (s) #                    ; acc1 += w[7] * a2\n"
	"\t r5 = [r2++=4]                                   ; residual src[1]\n"
	"\tr13_r12 >>>= 24 #                            ; low word (r12) = acc1 >> 24\n"
	"\t r15 = [sp+32]                                   ; w[2]\n"
	"\tr5 = r12 + r5 (ssat) #                       ; dst[1] = sat(r12 + src[1])\n"
	"\t r7 = [sp+44]                                    ; w[5]\n"
	"\tr13_r12 = d[sp+16]                           ; acc = bias 2 (64 bit)\n"
	"\tr13_r12 += r8 * r15 (s) #                    ; acc2 += w[2] * a0\n"
	"\t [r3++=4] = r4                                   ; dst[0]\n"
	"\tr6 = r6 + r10 (ssat) #                       ; h2 = sat(h2 + a2)\n"
	"\t r15 = [sp+56]                                   ; w[8]\n"
	"\tr13_r12 += r9 * r7 (s) #                     ; acc2 += w[5] * a1\n"
	"\t [r1+-4] = r6                                    ; h2 back\n"
	"\tr13_r12 += r10 * r15 (s) #                   ; acc2 += w[8] * a2\n"
	"\t r6 = [r2++=4]                                   ; residual src[2]\n"
	"\tr13_r12 >>>= 24 #                            ; low word (r12) = acc2 >> 24\n"
	"\t [r3++=4] = r5                                   ; dst[1]\n"
	"\tr6 = r12 + r6 (ssat)                         ; dst[2] = sat(r12 + src[2])\n"
	"\t[r3++=4] = r6                                ; dst[2]\n"
	"\tif (--r14 != 0) goto .Lmix_loop              ; next row\n"
	".Lmix_done:\n"
	"\tsp += 60\n"
	"\t{pc, r15-r4} = [sp++]\n"
	".globl wn_asm_head\n"
	".p2align 1\n"
	"wn_asm_head:\n"
	"\t[--sp] = {rets, r13-r4}\n"
	"\tif (r2 == 0) goto .Lhead_done\n"
	"\tr3 = 42949632\n"
	".Lhead_loop:\n"
	"\t{r10-r8} = [r0++]                            ; z of the row -> r8..r10\n"
	"\tifs (r8 < 0) {                               ; a0 = z < 0 ? hi32(z*K) : z\n"
	"\t\tr13_r12 = r8 * r3 (s)\n"
	"\t\tr8 = r13\n"
	"\t}\n"
	"\tifs (r9 < 0) {                               ; a1 = z < 0 ? hi32(z*K) : z\n"
	"\t\tr13_r12 = r9 * r3 (s)\n"
	"\t\tr9 = r13\n"
	"\t}\n"
	"\tifs (r10 < 0) {                              ; a2 = z < 0 ? hi32(z*K) : z\n"
	"\t\tr13_r12 = r10 * r3 (s)\n"
	"\t\tr10 = r13\n"
	"\t}\n"
	"\t{r6-r4} = [r1++]                             ; h of the row -> r4..r6, h += 12 bytes\n"
	"\tr4 = r4 + r8 (ssat)                          ; h0 = sat(h0 + a0)\n"
	"\tr5 = r5 + r9 (ssat) #                        ; h1 = sat(h1 + a1)\n"
	"\t [r1+-12] = r4                                   ; h0 back\n"
	"\tr6 = r6 + r10 (ssat) #                       ; h2 = sat(h2 + a2)\n"
	"\t [r1+-8] = r5                                    ; h1 back\n"
	"\t[r1+-4] = r6                                 ; h2 back\n"
	"\tif (--r2 != 0) goto .Lhead_loop              ; next row\n"
	".Lhead_done:\n"
	"\t{pc, r13-r4} = [sp++]\n"
);
#endif
