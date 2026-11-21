#pragma once

// The guest context as the generated code changes it, for native EE functions (rung 3): GPR writes the way the
// macros do them (ps2_runtime_macros.h), VU0 float registers as Vec4s, and guest RAM reached through env.at().
//   - SET_GPR_S32 / SET_GPR_U64 (addiu, lui, lw, daddu, or ...): the low doubleword; the upper doubleword stays.
//   - SET_GPR_VEC (lq, movz / movn, qmfc2): all 128 bits.
//   - JAL: ra = the return address, sign-extended (SET_GPR_U32); a branch or jump with a real (non-nop) delay slot
//     leaves ctx->branch_pc = its address and in_delay_slot = false (both compared with the whole context).

#include "kamite/native/native_function.h"
#include "kamite/native/vu_vec.h"

#include <cstring>

namespace kamite::native::ee
{
    enum Reg : int
    {
        zero = 0, at = 1, v0 = 2, v1 = 3, a0 = 4, a1 = 5, a2 = 6, a3 = 7, t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12,
        t5 = 13, t6 = 14, t7 = 15, s0 = 16, s1 = 17, s2 = 18, s3 = 19, s4 = 20, t8 = 24, t9 = 25, sp = 29, ra = 31,
    };

    KM_NATIVE_INLINE uint32_t u32(const R5900Context *ctx, int reg)
    {
        uint32_t value;
        std::memcpy(&value, &ctx->r[reg], sizeof(value));
        return value;
    }
    KM_NATIVE_INLINE uint64_t u64(const R5900Context *ctx, int reg)
    {
        uint64_t value;
        std::memcpy(&value, &ctx->r[reg], sizeof(value));
        return value;
    }
    KM_NATIVE_INLINE int64_t s64(const R5900Context *ctx, int reg) { return static_cast<int64_t>(u64(ctx, reg)); }
    KM_NATIVE_INLINE void setLow64(R5900Context *ctx, int reg, uint64_t value)
    {
        if (reg != 0)
            std::memcpy(&ctx->r[reg], &value, sizeof(value));
    }
    KM_NATIVE_INLINE void setS32(R5900Context *ctx, int reg, uint32_t value)
    {
        setLow64(ctx, reg, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value))));
    }
    // daddu rd, rs, zero
    KM_NATIVE_INLINE void move(R5900Context *ctx, int rd, int rs) { setLow64(ctx, rd, u64(ctx, rs)); }
    KM_NATIVE_INLINE void setVec(R5900Context *ctx, int reg, const void *bytes16)
    {
        if (reg != 0)
            std::memcpy(&ctx->r[reg], bytes16, 16);
    }
    // movz / movn: the whole register
    KM_NATIVE_INLINE void moveVec(R5900Context *ctx, int rd, int rs)
    {
        if (rd == 0)
            return;
        if (rs == 0)
            std::memset(&ctx->r[rd], 0, 16);
        else
            std::memcpy(&ctx->r[rd], &ctx->r[rs], 16);
    }

    KM_NATIVE_INLINE Vec4 vf(const R5900Context *ctx, int reg) { return load(&ctx->vu0_vf[reg]); }
    KM_NATIVE_INLINE void setVf(R5900Context *ctx, int reg, Vec4 v) { store(&ctx->vu0_vf[reg], v); }

    // VU0's constant register as the hardware holds it; natives that use vf0 check it first (cheap).
    KM_NATIVE_INLINE bool vf0Constant(const R5900Context *ctx)
    {
        static const uint32_t expect[4] = {0u, 0u, 0u, 0x3F800000u};
        return std::memcmp(&ctx->vu0_vf[0], expect, 16) == 0;
    }

    // A branch / jump at `site` whose delay slot is a real instruction: what the generated code leaves behind it.
    KM_NATIVE_INLINE void delaySlot(R5900Context *ctx, uint32_t site)
    {
        ctx->branch_pc = site;
        ctx->in_delay_slot = false;
    }
    // JAL from `site` (ra = site + 8), its delay slot real (branch_pc = site) or a nop.
    KM_NATIVE_INLINE void jal(R5900Context *ctx, uint32_t site, uint32_t target, bool realDelaySlot)
    {
        setS32(ctx, ra, site + 8u);
        if (realDelaySlot)
            delaySlot(ctx, site);
        ctx->pc = target;
    }
    // jr ra with a real delay slot already applied by the caller: branch_pc = site, pc = the target read before it.
    KM_NATIVE_INLINE void jrRa(R5900Context *ctx, uint32_t site, uint32_t target, bool realDelaySlot)
    {
        if (realDelaySlot)
            delaySlot(ctx, site);
        ctx->pc = target;
    }

    // RAM access through an env (RealEnv / ShadowEnv): offsets from plainRam(). lq / sq / lqc2 / sqc2 take the
    // address with its low 4 bits cleared; the callers pass such offsets.
    template <class Env>
    KM_NATIVE_INLINE uint32_t read32(Env &env, uint32_t offset)
    {
        uint32_t v;
        std::memcpy(&v, env.at(offset, 4u), 4);
        return v;
    }
    template <class Env>
    KM_NATIVE_INLINE uint64_t read64(Env &env, uint32_t offset)
    {
        uint64_t v;
        std::memcpy(&v, env.at(offset, 8u), 8);
        return v;
    }
    template <class Env>
    KM_NATIVE_INLINE void write32(Env &env, uint32_t offset, uint32_t v)
    {
        std::memcpy(env.at(offset, 4u), &v, 4);
    }
    template <class Env>
    KM_NATIVE_INLINE void write64(Env &env, uint32_t offset, uint64_t v)
    {
        std::memcpy(env.at(offset, 8u), &v, 8);
    }
    template <class Env>
    KM_NATIVE_INLINE Vec4 read128(Env &env, uint32_t offset)
    {
        return load(env.at(offset, 16u));
    }
    template <class Env>
    KM_NATIVE_INLINE void write128(Env &env, uint32_t offset, Vec4 v)
    {
        store(env.at(offset, 16u), v);
    }
    // lq into a GPR / sq from one (the whole register)
    template <class Env>
    KM_NATIVE_INLINE void lq(Env &env, R5900Context *ctx, int reg, uint32_t offset)
    {
        setVec(ctx, reg, env.at(offset, 16u));
    }
    template <class Env>
    KM_NATIVE_INLINE void sq(Env &env, const R5900Context *ctx, int reg, uint32_t offset)
    {
        uint8_t *p = env.at(offset, 16u);
        if (reg == 0)
            std::memset(p, 0, 16);
        else
            std::memcpy(p, &ctx->r[reg], 16);
    }

    // ---- batch 2's ops (rung 3, step a), as the generated code writes them -------------------------------------------
    KM_NATIVE_INLINE int32_t s32(const R5900Context *ctx, int reg) { return static_cast<int32_t>(u32(ctx, reg)); }
    // lbu (SET_GPR_ZE32): the byte zero-extended into the low doubleword
    KM_NATIVE_INLINE void setZe32(R5900Context *ctx, int reg, uint32_t value) { setLow64(ctx, reg, value); }
    // mult rd, rs, rt: LO / HI = the product's halves sign-extended, rd = LO (rd 0: LO / HI only)
    KM_NATIVE_INLINE void mult(R5900Context *ctx, int rd, int rs, int rt)
    {
        const int64_t r = static_cast<int64_t>(s32(ctx, rs)) * static_cast<int64_t>(s32(ctx, rt));
        ctx->lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(r)));
        ctx->hi = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(r >> 32)));
        setS32(ctx, rd, static_cast<uint32_t>(r));
    }
    // madd rd, rs, rt: HI:LO (their low words) += rs * rt; LO / HI sign-extended, rd = LO
    KM_NATIVE_INLINE void madd(R5900Context *ctx, int rd, int rs, int rt)
    {
        const uint64_t acc = ((ctx->hi & 0xFFFFFFFFull) << 32) | (ctx->lo & 0xFFFFFFFFull);
        const int64_t prod = static_cast<int64_t>(s32(ctx, rs)) * static_cast<int64_t>(s32(ctx, rt));
        const int64_t r = static_cast<int64_t>(acc + static_cast<uint64_t>(prod));
        ctx->lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(r)));
        ctx->hi = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(r >> 32)));
        setS32(ctx, rd, static_cast<uint32_t>(r));
    }
    // movn / movz rd, rs, rt: the whole register when rt is (non-)zero
    KM_NATIVE_INLINE void movn(R5900Context *ctx, int rd, int rs, int rt)
    {
        if (u64(ctx, rt) != 0u)
            moveVec(ctx, rd, rs);
    }

    // The FPU registers by their bits (lwc1 / swc1 / mtc1 / mfc1 move bits; mfc1 sign-extends into the GPR).
    KM_NATIVE_INLINE uint32_t fBits(const R5900Context *ctx, int reg)
    {
        uint32_t bits;
        std::memcpy(&bits, &ctx->f[reg], 4);
        return bits;
    }
    KM_NATIVE_INLINE void setFBits(R5900Context *ctx, int reg, uint32_t bits) { std::memcpy(&ctx->f[reg], &bits, 4); }
    KM_NATIVE_INLINE void mtc1(R5900Context *ctx, int gpr, int reg) { setFBits(ctx, reg, u32(ctx, gpr)); }
    KM_NATIVE_INLINE void mfc1(R5900Context *ctx, int gpr, int reg) { setS32(ctx, gpr, fBits(ctx, reg)); }
    // fcr31's condition bit (c.*.s writes it, bc1t / bc1f read it)
    KM_NATIVE_INLINE void setC(R5900Context *ctx, bool c) { ctx->fcr31 = c ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u); }
    KM_NATIVE_INLINE bool cBit(const R5900Context *ctx) { return (ctx->fcr31 & 0x800000u) != 0u; }
    // c.lt.s / c.eq.s: the runtime's compare key (sign-magnitude order, exponent-0 values as 0; ps2FpuCompareKey)
    KM_NATIVE_INLINE uint32_t fpuKey(uint32_t bits)
    {
        if ((bits & 0x7F800000u) == 0u)
            bits = 0u;
        return (bits & 0x80000000u) != 0u ? ~bits : bits ^ 0x80000000u;
    }
    KM_NATIVE_INLINE bool cLt(const R5900Context *ctx, int fs, int ft) { return fpuKey(fBits(ctx, fs)) < fpuKey(fBits(ctx, ft)); }
    KM_NATIVE_INLINE bool cEq(const R5900Context *ctx, int fs, int ft) { return fpuKey(fBits(ctx, fs)) == fpuKey(fBits(ctx, ft)); }

    // ---- one helper per instruction (batch 2): a native's body reads like the disassembly. Loads and stores take the
    // address the code computes (base + imm, 32-bit) and mask it: the native's entry checks proved it plain RAM.
    namespace op
    {
        KM_NATIVE_INLINE uint32_t ea(const R5900Context *ctx, int base, int32_t imm) { return u32(ctx, base) + static_cast<uint32_t>(imm); }
        KM_NATIVE_INLINE uint32_t ramOf(uint32_t address) { return address & PS2_RAM_MASK; }
        KM_NATIVE_INLINE void addiu(R5900Context *ctx, int rt, int rs, int32_t imm) { setS32(ctx, rt, u32(ctx, rs) + static_cast<uint32_t>(imm)); }
        KM_NATIVE_INLINE void addu(R5900Context *ctx, int rd, int rs, int rt) { setS32(ctx, rd, u32(ctx, rs) + u32(ctx, rt)); }
        KM_NATIVE_INLINE void daddu(R5900Context *ctx, int rd, int rs, int rt) { setLow64(ctx, rd, u64(ctx, rs) + u64(ctx, rt)); }
        KM_NATIVE_INLINE void lui(R5900Context *ctx, int rt, uint32_t imm) { setS32(ctx, rt, imm << 16); }
        KM_NATIVE_INLINE void andi(R5900Context *ctx, int rt, int rs, uint32_t imm) { setLow64(ctx, rt, u64(ctx, rs) & imm); }
        KM_NATIVE_INLINE void ori(R5900Context *ctx, int rt, int rs, uint32_t imm) { setLow64(ctx, rt, u64(ctx, rs) | imm); }
        KM_NATIVE_INLINE void xori(R5900Context *ctx, int rt, int rs, uint32_t imm) { setLow64(ctx, rt, u64(ctx, rs) ^ imm); }
        KM_NATIVE_INLINE void and_(R5900Context *ctx, int rd, int rs, int rt) { setLow64(ctx, rd, u64(ctx, rs) & u64(ctx, rt)); }
        KM_NATIVE_INLINE void or_(R5900Context *ctx, int rd, int rs, int rt) { setLow64(ctx, rd, u64(ctx, rs) | u64(ctx, rt)); }
        KM_NATIVE_INLINE void sll(R5900Context *ctx, int rd, int rt, int sa) { setS32(ctx, rd, u32(ctx, rt) << sa); }
        KM_NATIVE_INLINE void srl(R5900Context *ctx, int rd, int rt, int sa) { setS32(ctx, rd, u32(ctx, rt) >> sa); }
        KM_NATIVE_INLINE void sra(R5900Context *ctx, int rd, int rt, int sa) { setS32(ctx, rd, static_cast<uint32_t>(s32(ctx, rt) >> sa)); }
        KM_NATIVE_INLINE void dsll32(R5900Context *ctx, int rd, int rt, int sa) { setLow64(ctx, rd, u64(ctx, rt) << (32 + sa)); }
        KM_NATIVE_INLINE void dsrl32(R5900Context *ctx, int rd, int rt, int sa) { setLow64(ctx, rd, u64(ctx, rt) >> (32 + sa)); }
        KM_NATIVE_INLINE void dsrl(R5900Context *ctx, int rd, int rt, int sa) { setLow64(ctx, rd, u64(ctx, rt) >> sa); }
        KM_NATIVE_INLINE void slti(R5900Context *ctx, int rt, int rs, int32_t imm) { setLow64(ctx, rt, s64(ctx, rs) < imm ? 1u : 0u); }
        KM_NATIVE_INLINE void sltu(R5900Context *ctx, int rd, int rs, int rt) { setLow64(ctx, rd, u64(ctx, rs) < u64(ctx, rt) ? 1u : 0u); }
        KM_NATIVE_INLINE void slt(R5900Context *ctx, int rd, int rs, int rt) { setLow64(ctx, rd, s64(ctx, rs) < s64(ctx, rt) ? 1u : 0u); }
        template <class Env>
        KM_NATIVE_INLINE void lw(Env &env, R5900Context *ctx, int rt, int base, int32_t imm) { setS32(ctx, rt, read32(env, ramOf(ea(ctx, base, imm)))); }
        template <class Env>
        KM_NATIVE_INLINE void lbu(Env &env, R5900Context *ctx, int rt, int base, int32_t imm) { setZe32(ctx, rt, *env.at(ramOf(ea(ctx, base, imm)), 1u)); }
        template <class Env>
        KM_NATIVE_INLINE void ld(Env &env, R5900Context *ctx, int rt, int base, int32_t imm) { setLow64(ctx, rt, read64(env, ramOf(ea(ctx, base, imm)))); }
        template <class Env>
        KM_NATIVE_INLINE void sw(Env &env, const R5900Context *ctx, int rt, int base, int32_t imm) { write32(env, ramOf(ea(ctx, base, imm)), u32(ctx, rt)); }
        template <class Env>
        KM_NATIVE_INLINE void sd(Env &env, const R5900Context *ctx, int rt, int base, int32_t imm) { write64(env, ramOf(ea(ctx, base, imm)), u64(ctx, rt)); }
        template <class Env>
        KM_NATIVE_INLINE void lqOp(Env &env, R5900Context *ctx, int rt, int base, int32_t imm) { lq(env, ctx, rt, ramOf(ea(ctx, base, imm) & ~0xFu)); }
        template <class Env>
        KM_NATIVE_INLINE void sqOp(Env &env, const R5900Context *ctx, int rt, int base, int32_t imm) { sq(env, ctx, rt, ramOf(ea(ctx, base, imm) & ~0xFu)); }
        template <class Env>
        KM_NATIVE_INLINE void lwc1(Env &env, R5900Context *ctx, int ft, int base, int32_t imm) { setFBits(ctx, ft, read32(env, ramOf(ea(ctx, base, imm)))); }
        template <class Env>
        KM_NATIVE_INLINE void swc1(Env &env, const R5900Context *ctx, int ft, int base, int32_t imm) { write32(env, ramOf(ea(ctx, base, imm)), fBits(ctx, ft)); }
        template <class Env>
        KM_NATIVE_INLINE void lqc2(Env &env, R5900Context *ctx, int vf, int base, int32_t imm) { setVf(ctx, vf, read128(env, ramOf(ea(ctx, base, imm) & ~0xFu))); }
        template <class Env>
        KM_NATIVE_INLINE void sqc2(Env &env, const R5900Context *ctx, int vf, int base, int32_t imm)
        {
            std::memcpy(env.at(ramOf(ea(ctx, base, imm) & ~0xFu), 16u), &ctx->vu0_vf[vf], 16);
        }
    }

    // A guest address -> its RAM offset, or false (outside plain RAM).
    KM_NATIVE_INLINE bool offsetOf(uint32_t address, uint32_t size, uint32_t &offset) { return plainRam(address, size, offset); }
    KM_NATIVE_INLINE bool overlaps(uint32_t a, uint32_t aSize, uint32_t b, uint32_t bSize) { return a < b + bSize && b < a + aSize; }
}
