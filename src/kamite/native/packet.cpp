// Native packet building (rung 3, claude/native-next.md): the EE code that builds the DMA chain the three native VU1
// programs read. Step a writes the same packets natively, function by function from the leaves up (K's order, Nov 21:
// claude/rung3-step-a-handoff.md); switch KAMITE_NATIVE_PACKET (unset = the checked ones, 1 = all, 0 = none). Each
// run<Env>() is the recompiled function's effect (SLUS_215.03): the same loads and stores in the same order, the same
// registers left behind (ee_ctx.h: how the generated code writes them), the guest calls' cycles; footprints recorded.

#include "kamite/native/native.h"
#include "kamite/native/ee_ctx.h"
#include "kamite/native/libvu0_core.h"
#include "kamite/texture_slots.h"

#include <algorithm>
#include <bit>
#include <vector>

namespace kamite::native::packet
{
    namespace
    {
        namespace e = kamite::native::ee;

        // ---- func_0014FF00: link a packet into an ordering-table bucket ----------------------------------------------
        // a0 = the bucket list's descriptor (+0 the slots, +0x20 the first index), a1 = the index, a2 = the packet,
        // a3 = its last tag. The slot (index clamped to the first) becomes a NEXT tag to the packet; the packet's last
        // tag becomes a NEXT to what the slot linked to before (its QWC / flags kept). The four accesses' offsets:
        struct Link
        {
            uint32_t desc0, desc20, slot, tail;
        };
        constexpr uint32_t kLink = 0x14FF00u;

        // Where the link reads and writes, from the registers it will be called with (a0, a1 whole, a3); false when
        // any of it is outside plain RAM.
        template <class Env>
        bool linkPlan(Env &env, uint32_t a0, uint64_t a1, uint32_t a3, Link &link)
        {
            if (!plainRam(a0 + 0x20u, 4u, link.desc20) || !plainRam(a0, 4u, link.desc0) || !plainRam(a3, 8u, link.tail))
                return false;
            const uint64_t first = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(e::read32(env, link.desc20))));
            const uint32_t slots = e::read32(env, link.desc0);
            const uint64_t lowest = first < a1 ? first : a1;
            const uint32_t slot = slots + ((static_cast<uint32_t>(first) - static_cast<uint32_t>(lowest)) << 4);
            return plainRam(slot, 8u, link.slot); // lwu slot + 4, sd slot
        }

        // The link on the context (ctx->pc = ra after it), its plan made from the same registers.
        template <class Env>
        void linkOn(Env &env, R5900Context *ctx, const Link &link)
        {
            e::setS32(ctx, e::v1, e::read32(env, link.desc20));
            e::setS32(ctx, e::t1, 0x20000000u);
            e::setS32(ctx, e::t0, e::read32(env, link.desc0));
            e::setLow64(ctx, e::a2, e::u64(ctx, e::a2) << 32);
            e::setLow64(ctx, e::a0, e::u64(ctx, e::v1) < e::u64(ctx, e::a1) ? 1u : 0u);
            e::move(ctx, e::v0, e::v1);
            if (e::u64(ctx, e::a0) == 0u)
                e::moveVec(ctx, e::v0, e::a1);
            e::setLow64(ctx, e::a2, e::u64(ctx, e::a2) | e::u64(ctx, e::t1));
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) - e::u32(ctx, e::v0));
            e::setS32(ctx, e::a1, 0x0FFF0000u);
            e::setLow64(ctx, e::a1, e::u64(ctx, e::a1) | 0xFFFFu);
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) << 4);
            e::setS32(ctx, e::t0, e::u32(ctx, e::t0) + e::u32(ctx, e::v1));
            const uint32_t slot = link.slot;
            e::setLow64(ctx, e::a0, e::read32(env, (slot + 4u) & PS2_RAM_MASK)); // lwu
            e::write64(env, slot, e::u64(ctx, e::a2));
            e::setLow64(ctx, e::a0, e::u64(ctx, e::a0) << 32);
            e::setLow64(ctx, e::v0, e::read64(env, link.tail));
            e::setLow64(ctx, e::v0, e::u64(ctx, e::v0) & e::u64(ctx, e::a1));
            e::setLow64(ctx, e::v0, e::u64(ctx, e::v0) | e::u64(ctx, e::t1));
            e::setLow64(ctx, e::v0, e::u64(ctx, e::v0) | e::u64(ctx, e::a0));
            const uint32_t target = e::u32(ctx, e::ra);
            e::write64(env, link.tail, e::u64(ctx, e::v0)); // the jr's delay slot
            e::jrRa(ctx, 0x14FF50u, target, true);
        }

        template <class Env>
        bool link(Env &env, R5900Context *ctx)
        {
            Link plan;
            if (!linkPlan(env, e::u32(ctx, e::a0), e::u64(ctx, e::a1), e::u32(ctx, e::a3), plan))
                return false;
            linkOn(env, ctx, plan);
            return true;
        }

        // ---- func_0031A650 / func_0031A600: queue a packet into bucket a1 (< 10) of the frame's table a0 ----------
        // The frame's half (the word at 0x7476B0 & 1) picks the descriptor at a0 + 0xB00 + a1 * 8 + half * 4; then
        // 0x14FF00. 0x31A650 (a2 = the index, a3 = the packet, t0 = its last tag, or a3's when t0 is 0); 0x31A600
        // (a2 = the index, a3 = the packet and its last tag).
        constexpr uint32_t kQueue = 0x31A650u, kQueueOne = 0x31A600u, kFrameHalf = 0x7476B0u;

        // The frame's [sp - 16, sp) slot: plain and clear of what the link touches.
        bool queueFrame(const R5900Context *ctx, uint32_t &frame)
        {
            return plainRam(e::u32(ctx, e::sp) - 16u, 8u, frame);
        }
        bool clearOf(uint32_t frame, const Link &link)
        {
            return !e::overlaps(frame, 8u, link.desc0, 4u) && !e::overlaps(frame, 8u, link.desc20, 4u) &&
                   !e::overlaps(frame, 8u, link.slot, 8u) && !e::overlaps(frame, 8u, link.tail, 8u);
        }
        // The descriptor: the lw in the jal's delay slot, at a0 + 0xB00 + half * 4 + a1 * 8 (the code's 32-bit sums).
        // Both reads come after the frame's sd ra: keep them clear of it.
        template <class Env>
        bool descriptorOf(Env &env, uint32_t frame, uint32_t a0, uint32_t a1, uint32_t &descriptor)
        {
            uint32_t half, at;
            if (!plainRam(kFrameHalf, 4u, half) || e::overlaps(frame, 8u, half, 4u))
                return false;
            const uint32_t where = a0 + (((e::read32(env, half) & 1u) << 2) + (a1 << 3));
            if (!plainRam(where + 0xB00u, 4u, at) || e::overlaps(frame, 8u, at, 4u))
                return false;
            descriptor = e::read32(env, at);
            return true;
        }
        template <class Env>
        void leaveQueue(Env &env, R5900Context *ctx, uint32_t frame, uint32_t jrSite)
        {
            e::setLow64(ctx, e::ra, e::read64(env, frame));
            const uint32_t target = e::u32(ctx, e::ra);
            e::setS32(ctx, e::sp, e::u32(ctx, e::sp) + 16u);
            e::jrRa(ctx, jrSite, target, true);
        }

        // 0x31A650's plan from the registers it will be called with (a caller running it inline predicts them), made
        // before it changes anything; `reads` lists what it reads, for the caller's overlap checks.
        struct QueuePlan
        {
            uint32_t frame;
            bool inRange;
            Link plan;
            uint32_t half, at; // the frame half word, the descriptor's slot in the table
        };
        template <class Env>
        bool queuePlan(Env &env, uint32_t sp, uint32_t a0, uint64_t a1, uint64_t a2, uint32_t a3, uint64_t t0, QueuePlan &q)
        {
            if (!plainRam(sp - 16u, 8u, q.frame))
                return false;
            q.inRange = static_cast<int64_t>(a1) < 10;
            q.plan = Link{};
            q.half = q.at = 0u;
            if (!q.inRange)
                return true;
            // the registers 0x14FF00 will get: a0 = the descriptor, a1 = a2, a2 = a3, a3 = t0 (a3 when t0 is 0)
            if (!plainRam(kFrameHalf, 4u, q.half) || e::overlaps(q.frame, 8u, q.half, 4u))
                return false;
            const uint32_t where = a0 + (((e::read32(env, q.half) & 1u) << 2) + (static_cast<uint32_t>(a1) << 3));
            if (!plainRam(where + 0xB00u, 4u, q.at) || e::overlaps(q.frame, 8u, q.at, 4u))
                return false;
            const uint32_t descriptor = e::read32(env, q.at);
            const uint32_t tail = t0 == 0u ? a3 : static_cast<uint32_t>(t0);
            return linkPlan(env, descriptor, a2, tail, q.plan) && clearOf(q.frame, q.plan);
        }
        // Does [at, at + size) touch anything the plan reads (or writes before reading back)?
        bool queueTouches(const QueuePlan &q, uint32_t at, uint32_t size)
        {
            if (e::overlaps(at, size, q.frame, 8u))
                return true;
            if (!q.inRange)
                return false;
            return e::overlaps(at, size, q.half, 4u) || e::overlaps(at, size, q.at, 4u) ||
                   e::overlaps(at, size, q.plan.desc0, 4u) || e::overlaps(at, size, q.plan.desc20, 4u) ||
                   e::overlaps(at, size, q.plan.slot, 8u) || e::overlaps(at, size, q.plan.tail, 8u);
        }
        // From 0x31A650's entry, its plan made from these registers; false = stopped at the link's guest call.
        template <class Env>
        bool queueBody(Env &env, R5900Context *ctx, const QueuePlan &q)
        {
            const uint32_t frame = q.frame;
            const bool inRange = q.inRange;
            e::move(ctx, e::t1, e::a2);
            e::setS32(ctx, e::sp, e::u32(ctx, e::sp) - 16u);
            e::move(ctx, e::a2, e::a3);
            e::write64(env, frame, e::u64(ctx, e::ra));
            e::move(ctx, e::t2, e::a0);
            e::setLow64(ctx, e::v0, inRange ? 1u : 0u);
            e::move(ctx, e::a3, e::t0); // the beqz's delay slot
            e::delaySlot(ctx, 0x31A668u);
            if (!inRange)
            {
                leaveQueue(env, ctx, frame, 0x31A6A4u);
                return true;
            }
            e::setS32(ctx, e::v0, 0x00740000u);
            e::setS32(ctx, e::a0, e::u32(ctx, e::a1) << 3);
            e::setS32(ctx, e::v1, e::read32(env, kFrameHalf & PS2_RAM_MASK));
            if (e::u64(ctx, e::a3) == 0u)
                e::moveVec(ctx, e::t0, e::a2);
            e::move(ctx, e::a3, e::t0);
            e::move(ctx, e::a1, e::t1);
            e::setLow64(ctx, e::v1, e::u64(ctx, e::v1) & 1u);
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) << 2);
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) + e::u32(ctx, e::a0));
            e::setS32(ctx, e::v1, e::u32(ctx, e::t2) + e::u32(ctx, e::v1));
            e::jal(ctx, 0x31A698u, kLink, true);
            e::setS32(ctx, e::a0, e::read32(env, (e::u32(ctx, e::v1) + 0xB00u) & PS2_RAM_MASK));
            if (env.call())
                return false;
            linkOn(env, ctx, q.plan);
            leaveQueue(env, ctx, frame, 0x31A6A4u);
            return true;
        }

        template <class Env>
        bool queue(Env &env, R5900Context *ctx)
        {
            QueuePlan q;
            if (!queuePlan(env, e::u32(ctx, e::sp), e::u32(ctx, e::a0), e::u64(ctx, e::a1), e::u64(ctx, e::a2),
                           e::u32(ctx, e::a3), e::u64(ctx, e::t0), q))
                return false;
            queueBody(env, ctx, q);
            return true;
        }

        template <class Env>
        bool queueOne(Env &env, R5900Context *ctx)
        {
            uint32_t frame;
            if (!queueFrame(ctx, frame))
                return false;
            const bool inRange = e::s64(ctx, e::a1) < 10;
            Link plan{};
            if (inRange)
            {
                // 0x14FF00 gets a0 = the descriptor, a1 = a2, a2 = a3, a3 = a3
                uint32_t descriptor;
                if (!descriptorOf(env, frame, e::u32(ctx, e::a0), e::u32(ctx, e::a1), descriptor))
                    return false;
                if (!linkPlan(env, descriptor, e::u64(ctx, e::a2), e::u32(ctx, e::a3), plan) || !clearOf(frame, plan))
                    return false;
            }
            e::setS32(ctx, e::sp, e::u32(ctx, e::sp) - 16u);
            e::move(ctx, e::t0, e::a2);
            e::write64(env, frame, e::u64(ctx, e::ra));
            e::move(ctx, e::t1, e::a0);
            e::setLow64(ctx, e::v0, inRange ? 1u : 0u);
            e::move(ctx, e::a2, e::a3); // the beqz's delay slot
            e::delaySlot(ctx, 0x31A614u);
            if (!inRange)
            {
                leaveQueue(env, ctx, frame, 0x31A648u);
                return true;
            }
            e::setS32(ctx, e::v0, 0x00740000u);
            e::setS32(ctx, e::a0, e::u32(ctx, e::a1) << 3);
            e::setS32(ctx, e::v1, e::read32(env, kFrameHalf & PS2_RAM_MASK));
            e::move(ctx, e::a1, e::t0);
            e::setLow64(ctx, e::v1, e::u64(ctx, e::v1) & 1u);
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) << 2);
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) + e::u32(ctx, e::a0));
            e::setS32(ctx, e::v1, e::u32(ctx, e::t1) + e::u32(ctx, e::v1));
            e::jal(ctx, 0x31A63Cu, kLink, true);
            e::setS32(ctx, e::a0, e::read32(env, (e::u32(ctx, e::v1) + 0xB00u) & PS2_RAM_MASK));
            if (env.call())
                return true;
            linkOn(env, ctx, plan);
            leaveQueue(env, ctx, frame, 0x31A648u);
            return true;
        }

        // ==== batch 2 (the batch-2 chat, Nov 21): Tag_set_rate's small helpers, the texture packets ===================
        // Each body is the disassembly, one op:: helper per instruction (ee_ctx.h), after entry checks that prove every
        // access plain RAM (and decline what the original would loop on for billions of iterations).
        using namespace e::op;
        bool plain(uint32_t address, uint32_t size)
        {
            uint32_t offset;
            return plainRam(address, size, offset);
        }
        // The GS register table at 0x4A5D80 (8-byte entries; index & 0xFF, and the next one).
        constexpr uint32_t kGsTable = 0x4A5D80u;
        bool gsTablePlain() { return plain(kGsTable, 0x100u * 8u + 8u); }

        // ---- func_00155D60 (a0 = node, a1 = value, a2 = index): an empty countdown on the byte at +0x385, then
        // *(a0 + a2 * 0xB0 + 0x30) = a1. A byte of 0 counts from -1 (~4G backward branches): declined.
        constexpr uint32_t kCountdown = 0x155D60u;
        uint32_t countdownTarget(const R5900Context *ctx)
        {
            return e::u32(ctx, e::a2) * 0xB0u + e::u32(ctx, e::a0) + 0x30u; // mult's low word + addu
        }
        template <class Env>
        bool countdownCheck(Env &env, const R5900Context *ctx)
        {
            uint32_t count;
            if (!plainRam(e::u32(ctx, e::a0) + 0x385u, 1u, count) || !plain(countdownTarget(ctx), 8u))
                return false;
            return *env.at(count, 1u) != 0u;
        }
        // From the entry, with the checks passed; false = stopped at a checkpoint (ctx left there).
        template <class Env>
        bool countdownOn(Env &env, R5900Context *ctx)
        {
            lbu(env, ctx, e::v0, e::a0, 0x385);
            addiu(ctx, e::v0, e::v0, -1);
            for (;;)
            {
                const bool taken = e::u64(ctx, e::v0) != 0u;
                addiu(ctx, e::v0, e::v0, -1); // the bnez's delay slot
                e::delaySlot(ctx, 0x155D78u);
                if (!taken)
                    break;
                ctx->pc = 0x155D68u;
                if (env.checkpoint(32u))
                    return false;
            }
            addiu(ctx, e::v0, e::zero, 0xB0);
            e::mult(ctx, e::v1, e::a2, e::v0);
            addu(ctx, e::v0, e::v1, e::a0);
            const uint32_t target = e::u32(ctx, e::ra);
            sd(env, ctx, e::a1, e::v0, 0x30);
            e::jrRa(ctx, 0x155D8Cu, target, true);
            return true;
        }
        template <class Env>
        bool countdown(Env &env, R5900Context *ctx)
        {
            if (!countdownCheck(env, ctx))
                return false;
            countdownOn(env, ctx);
            return true;
        }

        // ---- cModel_getMeshPtr 0x14B6F8 (a0 = model, a1 = index): the mesh list (+0x25C, next +0x404), the a1-th mesh
        // (the first one past the list's end). Observed (body_detail, bone_tweaks, olivia_coat): natives run behind the
        // observers; a native caller runs them itself (env.observe).
        constexpr uint32_t kGetMeshPtr = 0x14B6F8u;
        constexpr int kMaxMeshes = 4096;
        // The walk the code will make, reads only: false when an address is outside plain RAM or the list runs past
        // kMaxMeshes.
        template <class Env>
        bool meshCheck(Env &env, uint32_t model, uint64_t index)
        {
            uint32_t head;
            if (!plainRam(model + 0x25Cu, 4u, head))
                return false;
            if (index == 0u)
                return true;
            uint32_t node = e::read32(env, head), at;
            uint32_t v1 = static_cast<uint32_t>(index) - 1u;
            for (int i = 0; i < kMaxMeshes; ++i)
            {
                if (!plainRam(node + 0x404u, 4u, at))
                    return false;
                node = e::read32(env, at);
                if (node == 0u)
                    return true;
                v1 -= 1u;
                if (v1 == 0xFFFFFFFFu)
                    return true;
            }
            return false;
        }
        template <class Env>
        bool meshOn(Env &env, R5900Context *ctx)
        {
            addiu(ctx, e::v1, e::a1, -1);
            const bool first = e::u64(ctx, e::a1) == 0u;
            lw(env, ctx, e::v0, e::a0, 0x25C); // the beqz's delay slot
            e::delaySlot(ctx, 0x14B6FCu);
            if (first)
            {
                ctx->pc = e::u32(ctx, e::ra);
                return true;
            }
            addiu(ctx, e::a1, e::zero, -1);
            lw(env, ctx, e::v0, e::v0, 0x404);
            for (;;)
            {
                const bool more = e::u64(ctx, e::v0) != 0u;
                addiu(ctx, e::v1, e::v1, -1); // the bnez's delay slot
                e::delaySlot(ctx, 0x14B710u);
                if (!more)
                {
                    const uint32_t target = e::u32(ctx, e::ra);
                    lw(env, ctx, e::v0, e::a0, 0x25C);
                    e::jrRa(ctx, 0x14B718u, target, true);
                    return true;
                }
                if (e::u64(ctx, e::v1) == e::u64(ctx, e::a1)) // bnel not taken: its delay slot skipped
                {
                    ctx->pc = e::u32(ctx, e::ra);
                    return true;
                }
                lw(env, ctx, e::v0, e::v0, 0x404);
                e::delaySlot(ctx, 0x14B720u);
                ctx->pc = 0x14B710u;
                if (env.checkpoint(32u))
                    return false;
            }
        }
        template <class Env>
        bool getMeshPtr(Env &env, R5900Context *ctx)
        {
            if (!meshCheck(env, e::u32(ctx, e::a0), e::u64(ctx, e::a1)))
                return false;
            meshOn(env, ctx);
            return true;
        }

        // ---- ForwardNodeUpdate_14D768 (a0 = model, a1, a2): for every mesh node from getMeshPtr(model, 0): 0x155D60(
        // node, a1, a2). A 0x40 frame: s0-s2 (sq), ra (sd).
        constexpr uint32_t kForwardNodes = 0x14D768u;
        constexpr int kMaxNodes = 256;
        struct Span
        {
            uint32_t at, size;
        };
        bool overlapsAny(const Span &w, const Span *spans, int count)
        {
            for (int i = 0; i < count; ++i)
                if (e::overlaps(w.at, w.size, spans[i].at, spans[i].size))
                    return true;
            return false;
        }
        template <class Env>
        bool forwardNodes(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp);
            uint32_t frame;
            if ((sp0 & 0xFu) != 0u || !plainRam(sp0 - 0x40u, 0x40u, frame))
                return false;
            // The walk: getMeshPtr(a0, 0) = *(a0 + 0x25C); per node its byte +0x385, its write, its next (+0x404). No
            // write may land on anything read after it (a later node's fields, the list head) or on the frame.
            const uint32_t a0 = e::u32(ctx, e::a0);
            uint32_t head;
            if (!plainRam(a0 + 0x25Cu, 4u, head))
                return false;
            static thread_local Span reads[2 * kMaxNodes + 2], writes[kMaxNodes + 1];
            int readCount = 0, writeCount = 0;
            reads[readCount++] = Span{head, 4u};
            writes[writeCount++] = Span{frame, 0x40u}; // written first, read back last
            const uint32_t index = e::u32(ctx, e::a2);
            uint32_t node = e::read32(env, head);
            for (int i = 0; node != 0u; ++i)
            {
                uint32_t count, next;
                const uint32_t target = index * 0xB0u + node + 0x30u;
                if (i == kMaxNodes || !plainRam(node + 0x385u, 1u, count) || !plain(target, 8u) ||
                    !plainRam(node + 0x404u, 4u, next) || *env.at(count, 1u) == 0u)
                    return false;
                reads[readCount++] = Span{count, 1u};
                reads[readCount++] = Span{next, 4u};
                writes[writeCount++] = Span{target & PS2_RAM_MASK, 8u};
                node = e::read32(env, next);
            }
            for (int i = 0; i < writeCount; ++i)
                if (overlapsAny(writes[i], reads, readCount) || (i > 0 && overlapsAny(writes[i], writes, 1)))
                    return false;

            addiu(ctx, e::sp, e::sp, -0x40);
            sqOp(env, ctx, e::s2, e::sp, 0x10);
            sqOp(env, ctx, e::s1, e::sp, 0x20);
            daddu(ctx, e::s2, e::a1, e::zero);
            sqOp(env, ctx, e::s0, e::sp, 0x30);
            daddu(ctx, e::s1, e::a2, e::zero);
            sd(env, ctx, e::ra, e::sp, 0);
            e::jal(ctx, 0x14D784u, kGetMeshPtr, true);
            daddu(ctx, e::a1, e::zero, e::zero);
            if (env.call())
                return true;
            env.observe(kGetMeshPtr, ctx);
            if (!meshOn(env, ctx))
                return true;
            daddu(ctx, e::s0, e::v0, e::zero);
            if (e::u64(ctx, e::s0) == 0u)
            {
                lqOp(env, ctx, e::s0, e::sp, 0x30); // the beql's delay slot
                e::delaySlot(ctx, 0x14D790u);
            }
            else
            {
                daddu(ctx, e::a0, e::s0, e::zero);
                for (;;)
                {
                    daddu(ctx, e::a1, e::s2, e::zero);
                    e::jal(ctx, 0x14D7A4u, kCountdown, true);
                    daddu(ctx, e::a2, e::s1, e::zero);
                    if (env.call())
                        return true;
                    if (!countdownOn(env, ctx))
                        return true;
                    lw(env, ctx, e::s0, e::s0, 0x404);
                    const bool more = e::u64(ctx, e::s0) != 0u;
                    daddu(ctx, e::a0, e::s0, e::zero); // the bnez's delay slot
                    e::delaySlot(ctx, 0x14D7B0u);
                    if (!more)
                        break;
                    ctx->pc = 0x14D7A0u;
                    if (env.checkpoint(32u))
                        return true;
                }
                lqOp(env, ctx, e::s0, e::sp, 0x30);
            }
            lqOp(env, ctx, e::s1, e::sp, 0x20);
            lqOp(env, ctx, e::s2, e::sp, 0x10);
            ld(env, ctx, e::ra, e::sp, 0);
            const uint32_t target = e::u32(ctx, e::ra);
            addiu(ctx, e::sp, e::sp, 0x40);
            e::jrRa(ctx, 0x14D7C8u, target, true);
            return true;
        }

        // ---- PostInc 0x15B0F0 (a0 = size): the frame's bump allocator (pointer at 0x566E10, the frame half's 0x60000
        // bytes at 0x506E10): v0 = the old pointer and the pointer moves, or v0 = 0 when the block won't fit.
        constexpr uint32_t kPostInc = 0x15B0F0u;
        template <class Env>
        bool postInc(Env &env, R5900Context *ctx)
        {
            lui(ctx, e::v1, 0x74);
            lui(ctx, e::a1, 0x6);
            lw(env, ctx, e::v0, e::v1, 0x76B0);
            lui(ctx, e::a3, 0x56);
            lw(env, ctx, e::a2, e::a3, 0x6E10);
            lui(ctx, e::v1, 0x50);
            andi(ctx, e::v0, e::v0, 0x1);
            addiu(ctx, e::v1, e::v1, 0x6E10);
            e::mult(ctx, e::v0, e::v0, e::a1);
            addu(ctx, e::a0, e::a2, e::a0);
            addu(ctx, e::v0, e::v0, e::v1);
            sltu(ctx, e::v0, e::v0, e::a0);
            const bool full = e::u64(ctx, e::v0) != 0u;
            daddu(ctx, e::v0, e::a2, e::zero); // the bnez's delay slot
            e::delaySlot(ctx, 0x15B120u);
            const uint32_t target = e::u32(ctx, e::ra);
            if (full)
            {
                daddu(ctx, e::v0, e::zero, e::zero);
                e::jrRa(ctx, 0x15B130u, target, true);
                return true;
            }
            sw(env, ctx, e::a0, e::a3, 0x6E10);
            e::jrRa(ctx, 0x15B128u, target, true);
            return true;
        }

        // The pair of GS table words packed as the code does: (entry.hi | 0x30000000) low, (entry.lo & 0x0FFFFFFF) high;
        // these three bodies spell it out op by op.

        // ---- func_00155AE8 (a0 = base, a1 = index, a2 = record): two tags into record a2 of base (+0x70 / +0x80, records
        // 0xB0 bytes): entries index / index + 1 of the GS table, or a 0x10000000 tag first when index < 0.
        constexpr uint32_t kTwoTags = 0x155AE8u;
        template <class Env>
        bool twoTags(Env &env, R5900Context *ctx)
        {
            const uint32_t record = e::u32(ctx, e::a0) + e::u32(ctx, e::a2) * 0xB0u; // LO's low word after the madd
            if (!gsTablePlain() || !plain(record + 0x70u, 8u) || !plain(record + 0x80u, 8u))
                return false;
            ctx->lo = e::u64(ctx, e::a0); // mtlo
            daddu(ctx, e::t3, e::a1, e::zero);
            const bool negative = e::s64(ctx, e::t3) < 0;
            daddu(ctx, e::t4, e::a2, e::zero); // the bltz's delay slot
            e::delaySlot(ctx, 0x155AF0u);
            if (!negative)
            {
                andi(ctx, e::t3, e::t3, 0xFF);
                lui(ctx, e::a3, 0x4A);
                addiu(ctx, e::a3, e::a3, 0x5D80);
                sll(ctx, e::v1, e::t3, 3);
                addu(ctx, e::a0, e::v1, e::a3);
                addiu(ctx, e::t0, e::a3, 0x4);
                addu(ctx, e::v1, e::v1, e::t0);
                lw(env, ctx, e::a1, e::a0, 0);
                lw(env, ctx, e::v0, e::v1, 0);
                lui(ctx, e::t2, 0x3000);
                addiu(ctx, e::t1, e::zero, 0xB0);
                lui(ctx, e::a2, 0xFFF);
                ori(ctx, e::a2, e::a2, 0xFFFF);
                e::madd(ctx, e::t1, e::t4, e::t1);
                or_(ctx, e::v0, e::v0, e::t2);
                and_(ctx, e::a1, e::a1, e::a2);
                dsll32(ctx, e::v0, e::v0, 0);
                dsll32(ctx, e::a1, e::a1, 0);
                dsrl32(ctx, e::v0, e::v0, 0);
                addiu(ctx, e::v1, e::t3, 0x1);
                or_(ctx, e::v0, e::v0, e::a1);
                sll(ctx, e::v1, e::v1, 3);
                sd(env, ctx, e::v0, e::t1, 0x70);
                addu(ctx, e::t0, e::v1, e::t0);
                addu(ctx, e::v1, e::v1, e::a3);
                lw(env, ctx, e::v0, e::t0, 0);
                lw(env, ctx, e::a0, e::v1, 0);
                or_(ctx, e::v0, e::v0, e::t2);
                and_(ctx, e::a0, e::a0, e::a2);
                dsll32(ctx, e::v0, e::v0, 0);
                dsrl32(ctx, e::v0, e::v0, 0);
                dsll32(ctx, e::a0, e::a0, 0);
                or_(ctx, e::v0, e::v0, e::a0);
                const uint32_t target = e::u32(ctx, e::ra);
                sd(env, ctx, e::v0, e::t1, 0x80);
                e::jrRa(ctx, 0x155B7Cu, target, true);
                return true;
            }
            addiu(ctx, e::a2, e::zero, 0xB0);
            andi(ctx, e::t3, e::t3, 0xFF);
            e::madd(ctx, e::a2, e::t4, e::a2);
            addiu(ctx, e::a1, e::t3, 0x1);
            lui(ctx, e::v0, 0x4A);
            lui(ctx, e::a0, 0x1000);
            addiu(ctx, e::v0, e::v0, 0x5D80);
            sll(ctx, e::a1, e::a1, 3);
            addu(ctx, e::v1, e::v0, e::a1);
            lui(ctx, e::a3, 0x3000);
            sd(env, ctx, e::a0, e::a2, 0x70);
            addu(ctx, e::a1, e::a1, e::v0);
            lui(ctx, e::a0, 0xFFF);
            lw(env, ctx, e::v0, e::v1, 0x4);
            ori(ctx, e::a0, e::a0, 0xFFFF);
            lw(env, ctx, e::v1, e::a1, 0);
            or_(ctx, e::v0, e::v0, e::a3);
            and_(ctx, e::v1, e::v1, e::a0);
            dsll32(ctx, e::v0, e::v0, 0);
            dsrl32(ctx, e::v0, e::v0, 0);
            dsll32(ctx, e::v1, e::v1, 0);
            or_(ctx, e::v0, e::v0, e::v1);
            const uint32_t target = e::u32(ctx, e::ra);
            sd(env, ctx, e::v0, e::a2, 0x80);
            e::jrRa(ctx, 0x155BDCu, target, true);
            return true;
        }

        // ---- func_00155BE8 (a0 = object, a1 = index, a2 = slot): the slot's packet (*(a0 + a2 * 4 + 0x220)) gets
        // +0x60 = 0x10000000 and +0x50 = the index's GS table pair (0x10000000 when index < 0).
        constexpr uint32_t kSlotTags = 0x155BE8u;
        template <class Env>
        bool slotTags(Env &env, R5900Context *ctx)
        {
            uint32_t slot;
            if (!gsTablePlain() || !plainRam(e::u32(ctx, e::a0) + (e::u32(ctx, e::a2) << 2) + 0x220u, 4u, slot))
                return false;
            const uint32_t packet = e::read32(env, slot);
            if (!plain(packet + 0x50u, 8u) || !plain(packet + 0x60u, 8u))
                return false;
            sll(ctx, e::a2, e::a2, 2);
            addu(ctx, e::a0, e::a0, e::a2);
            const bool negative = e::s64(ctx, e::a1) < 0;
            lw(env, ctx, e::a3, e::a0, 0x220); // the bltz's delay slot
            e::delaySlot(ctx, 0x155BF0u);
            const uint32_t target = e::u32(ctx, e::ra);
            if (!negative)
            {
                andi(ctx, e::a1, e::a1, 0xFF);
                lui(ctx, e::v0, 0x4A);
                addiu(ctx, e::v0, e::v0, 0x5D80);
                sll(ctx, e::a0, e::a1, 3);
                addu(ctx, e::a1, e::v0, e::a0);
                lui(ctx, e::a2, 0x3000);
                lw(env, ctx, e::v1, e::a1, 0x4);
                addu(ctx, e::a0, e::a0, e::v0);
                lw(env, ctx, e::a1, e::a0, 0);
                lui(ctx, e::v0, 0xFFF);
                ori(ctx, e::v0, e::v0, 0xFFFF);
                or_(ctx, e::v1, e::v1, e::a2);
                and_(ctx, e::a1, e::a1, e::v0);
                dsll32(ctx, e::v1, e::v1, 0);
                dsrl32(ctx, e::v1, e::v1, 0);
                dsll32(ctx, e::a1, e::a1, 0);
                or_(ctx, e::v1, e::v1, e::a1);
                lui(ctx, e::v0, 0x1000);
                sd(env, ctx, e::v0, e::a3, 0x60);
                sd(env, ctx, e::v1, e::a3, 0x50);
                e::jrRa(ctx, 0x155C44u, target, true);
                return true;
            }
            lui(ctx, e::v0, 0x1000);
            sd(env, ctx, e::v0, e::a3, 0x60);
            sd(env, ctx, e::v0, e::a3, 0x50);
            e::jrRa(ctx, 0x155C54u, target, true);
            return true;
        }

        // ---- func_00155C68 (a0 = first tag, a1 = second tag, a2 = index): *a0 = the index's GS table pair (0x10000000
        // when index < 0), *a1 = index + 1's.
        constexpr uint32_t kTagPair = 0x155C68u;
        template <class Env>
        bool tagPair(Env &env, R5900Context *ctx)
        {
            if (!gsTablePlain() || !plain(e::u32(ctx, e::a0), 8u) || !plain(e::u32(ctx, e::a1), 8u))
                return false;
            daddu(ctx, e::t3, e::a0, e::zero);
            daddu(ctx, e::t2, e::a2, e::zero);
            const bool negative = e::s64(ctx, e::t2) < 0;
            daddu(ctx, e::t4, e::a1, e::zero); // the bltz's delay slot
            e::delaySlot(ctx, 0x155C70u);
            const uint32_t target = e::u32(ctx, e::ra);
            if (!negative)
            {
                andi(ctx, e::t2, e::t2, 0xFF);
                lui(ctx, e::a3, 0x4A);
                addiu(ctx, e::a3, e::a3, 0x5D80);
                sll(ctx, e::v1, e::t2, 3);
                addu(ctx, e::a1, e::v1, e::a3);
                addiu(ctx, e::t0, e::a3, 0x4);
                addu(ctx, e::v1, e::v1, e::t0);
                lw(env, ctx, e::a0, e::a1, 0);
                lw(env, ctx, e::v0, e::v1, 0);
                lui(ctx, e::t1, 0x3000);
                lui(ctx, e::a2, 0xFFF);
                addiu(ctx, e::a1, e::t2, 0x1);
                ori(ctx, e::a2, e::a2, 0xFFFF);
                or_(ctx, e::v0, e::v0, e::t1);
                dsll32(ctx, e::v0, e::v0, 0);
                and_(ctx, e::a0, e::a0, e::a2);
                dsrl32(ctx, e::v0, e::v0, 0);
                dsll32(ctx, e::a0, e::a0, 0);
                or_(ctx, e::v0, e::v0, e::a0);
                sll(ctx, e::a1, e::a1, 3);
                sd(env, ctx, e::v0, e::t3, 0);
                addu(ctx, e::t0, e::a1, e::t0);
                addu(ctx, e::a1, e::a1, e::a3);
                lw(env, ctx, e::v0, e::t0, 0);
                lw(env, ctx, e::v1, e::a1, 0);
                or_(ctx, e::v0, e::v0, e::t1);
                and_(ctx, e::v1, e::v1, e::a2);
                dsll32(ctx, e::v0, e::v0, 0);
                dsrl32(ctx, e::v0, e::v0, 0);
                dsll32(ctx, e::v1, e::v1, 0);
                or_(ctx, e::v0, e::v0, e::v1);
                sd(env, ctx, e::v0, e::t4, 0);
                e::jrRa(ctx, 0x155CF4u, target, true);
                return true;
            }
            andi(ctx, e::t2, e::t2, 0xFF);
            lui(ctx, e::v0, 0x1000);
            addiu(ctx, e::a1, e::t2, 0x1);
            lui(ctx, e::v1, 0x4A);
            sd(env, ctx, e::v0, e::t3, 0);
            addiu(ctx, e::v1, e::v1, 0x5D80);
            sll(ctx, e::a1, e::a1, 3);
            lui(ctx, e::a2, 0x3000);
            addu(ctx, e::a0, e::v1, e::a1);
            lw(env, ctx, e::v0, e::a0, 0x4);
            addu(ctx, e::a1, e::a1, e::v1);
            lw(env, ctx, e::a0, e::a1, 0);
            lui(ctx, e::v1, 0xFFF);
            ori(ctx, e::v1, e::v1, 0xFFFF);
            or_(ctx, e::v0, e::v0, e::a2);
            and_(ctx, e::a0, e::a0, e::v1);
            dsll32(ctx, e::v0, e::v0, 0);
            dsrl32(ctx, e::v0, e::v0, 0);
            dsll32(ctx, e::a0, e::a0, 0);
            or_(ctx, e::v0, e::v0, e::a0);
            sd(env, ctx, e::v0, e::t4, 0);
            e::jrRa(ctx, 0x155D4Cu, target, true);
            return true;
        }

        // ---- the texture packets: func_00150DC8 / func_00150F68 / func_00151168 ------------------------------------
        // Each takes the next of an object's 0x280 packet records (counter at +0x3C; records 0x180 bytes from +0x40, the
        // frame's half 0xC0 in), writes a 0x80-byte GS packet there and queues it (0x31A650 into the table at 0x754C80:
        // bucket 1 order 0xE for kind 0 / 3, bucket 6 order 8 for kind 2, bucket 1 order 2 for kind 1). v0 = -1 when the
        // GS flag 0x800 (at 0x747A2C) is set (unless the sign argument is negative) or the records are used up, else 0.
        // A 0x10 frame (ra). The plan predicts every address before anything changes; the queue's plan is made from the
        // registers it will get and no write here may touch what it reads.
        constexpr uint32_t kTexture = 0x150DC8u, kTextureBlock = 0x150F68u, kTextureStrip = 0x151168u;
        constexpr uint32_t kGsFlag = 0x747A2Cu, kOrderTable = 0x754C80u;
        struct TextureWrites
        {
            Span spans[12];
            int count = 0;
            bool ok = true;
            void add(uint32_t address, uint32_t size)
            {
                uint32_t offset;
                if (!plainRam(address, size, offset))
                    ok = false;
                else if (count < 12)
                    spans[count++] = Span{offset, size};
            }
        };
        struct TexturePlan
        {
            uint32_t frame;
            bool early = false;  // returns -1 before the record
            bool full = false;   // the counter is 0x280: returns -1
            uint32_t record = 0; // a3 / t6
            bool queued = false;
            QueuePlan q{};
        };
        // The head the three share: frame, GS flag (when `sign` >= 0), the counter. Reads that set an address come
        // after the frame's sd: kept clear of it.
        template <class Env>
        bool textureHead(Env &env, uint32_t sp0, uint32_t object, int64_t sign, TexturePlan &t, uint32_t &counter)
        {
            uint32_t flag, count, half;
            if (!plainRam(sp0 - 0x10u, 8u, t.frame))
                return false;
            if (sign >= 0)
            {
                if (!plainRam(kGsFlag, 4u, flag) || e::overlaps(t.frame, 8u, flag, 4u))
                    return false;
                if ((e::read32(env, flag) & 0x800u) != 0u)
                {
                    t.early = true;
                    return true;
                }
            }
            if (!plainRam(object + 0x3Cu, 4u, count) || e::overlaps(t.frame, 8u, count, 4u))
                return false;
            const uint32_t c = e::read32(env, count);
            if (c == 0x280u)
            {
                t.full = true;
                return true;
            }
            if (!plainRam(kFrameHalf, 4u, half) || e::overlaps(t.frame, 8u, half, 4u))
                return false;
            counter = count;
            t.record = object + (c * 0x180u + 0x40u) + (e::read32(env, half) & 1u) * 0xC0u;
            return true;
        }
        // The queue for kind `kind` (0 / 3: bucket 1 order 0xE, 2: 6 / 8, 1: 1 / 2, else none), and the writes kept
        // clear of what it reads.
        template <class Env>
        bool textureQueue(Env &env, uint32_t sp0, uint64_t kind, TexturePlan &t, const TextureWrites &w)
        {
            if (!w.ok)
                return false;
            uint32_t bucket = 0, order = 0;
            if (kind == 0u || kind == 3u)
                bucket = 1u, order = 0xEu;
            else if (kind == 2u)
                bucket = 6u, order = 8u;
            else if (kind == 1u)
                bucket = 1u, order = 2u;
            t.queued = bucket != 0u;
            if (!t.queued)
                return true;
            const uint64_t tail = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(t.record + 0x80u)));
            if (!queuePlan(env, sp0 - 0x10u, kOrderTable, bucket, order, t.record, tail, t.q))
                return false;
            for (int i = 0; i < w.count; ++i)
                if (queueTouches(t.q, w.spans[i].at, w.spans[i].size))
                    return false;
            return true;
        }
        // The common tail: ld ra; jr ra (addiu sp, 16) at 0x...5C / 60.
        template <class Env>
        void textureReturn(Env &env, R5900Context *ctx, uint32_t jrSite)
        {
            ld(env, ctx, e::ra, e::sp, 0);
            const uint32_t target = e::u32(ctx, e::ra);
            addiu(ctx, e::sp, e::sp, 0x10);
            e::jrRa(ctx, jrSite, target, true);
        }
        // The queue call: jal 0x31A650 (addiu t0, a3, 0x80 in its delay slot) and the queue inline; false = stopped.
        template <class Env>
        bool textureCall(Env &env, R5900Context *ctx, uint32_t site, const QueuePlan &q)
        {
            e::jal(ctx, site, kQueue, true);
            addiu(ctx, e::t0, e::a3, 0x80);
            if (env.call())
                return false;
            return queueBody(env, ctx, q);
        }

        // func_00150DC8 (a0 = object, a1 = the texture's holder (+4: its record), a2 = the address word, a3 = sign / mode
        // bit, t0 = kind).
        template <class Env>
        bool texture(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), object = e::u32(ctx, e::a0);
            TexturePlan t;
            uint32_t counter = 0;
            if (!textureHead(env, sp0, object, e::s64(ctx, e::a3), t, counter))
                return false;
            if (!t.early && !t.full)
            {
                uint32_t holder;
                if (!plainRam(e::u32(ctx, e::a1) + 4u, 4u, holder) || e::overlaps(holder, 4u, t.frame, 8u) ||
                    e::overlaps(holder, 4u, counter, 4u))
                    return false;
                const uint32_t tex = e::read32(env, holder);
                if (!plain((tex + 0x10u) & ~0xFu, 16u) || !plain((tex + 0x30u) & ~0xFu, 16u) || !plain(tex + 0x50u, 4u))
                    return false;
                TextureWrites w;
                const uint32_t r = t.record;
                w.add(sp0 - 0x10u, 8u), w.add(object + 0x3Cu, 4u), w.add(r + 0xCu, 4u), w.add(r, 8u);
                w.add((r + 0x20u) & ~0xFu, 16u), w.add(r + 0x24u, 4u), w.add((r + 0x40u) & ~0xFu, 16u);
                w.add(r + 0x70u, 8u), w.add(r + 0x7Cu, 4u);
                if (!textureQueue(env, sp0, e::u64(ctx, e::t0), t, w))
                    return false;
            }

            addiu(ctx, e::sp, e::sp, -0x10);
            daddu(ctx, e::t1, e::a0, e::zero);
            sd(env, ctx, e::ra, e::sp, 0);
            daddu(ctx, e::t3, e::a1, e::zero);
            daddu(ctx, e::t2, e::a3, e::zero);
            const bool negative = e::s64(ctx, e::t2) < 0;
            daddu(ctx, e::t4, e::a2, e::zero);
            e::delaySlot(ctx, 0x150DDCu);
            if (!negative)
            {
                lui(ctx, e::v1, 0x74);
                lw(env, ctx, e::v0, e::v1, 0x7A2C);
                andi(ctx, e::v0, e::v0, 0x800);
                const bool set = e::u64(ctx, e::v0) != 0u;
                addiu(ctx, e::v0, e::zero, -1);
                e::delaySlot(ctx, 0x150DF0u);
                if (set)
                {
                    textureReturn(env, ctx, 0x150F60u);
                    return true;
                }
            }
            lw(env, ctx, e::a2, e::t1, 0x3C);
            addiu(ctx, e::v0, e::zero, 0x280);
            const bool more = e::u64(ctx, e::a2) != e::u64(ctx, e::v0);
            lui(ctx, e::v0, 0x74);
            e::delaySlot(ctx, 0x150E00u);
            if (!more)
            {
                addiu(ctx, e::v0, e::zero, -1);
                e::delaySlot(ctx, 0x150E08u);
                textureReturn(env, ctx, 0x150F60u);
                return true;
            }
            addiu(ctx, e::v1, e::zero, 0x180);
            lw(env, ctx, e::a0, e::v0, 0x76B0);
            e::mult(ctx, e::v1, e::a2, e::v1);
            addiu(ctx, e::v0, e::zero, 0xC0);
            lui(ctx, e::a1, 0xFFF);
            andi(ctx, e::a0, e::a0, 0x1);
            addiu(ctx, e::a2, e::a2, 0x1);
            e::mult(ctx, e::a0, e::a0, e::v0);
            ori(ctx, e::a1, e::a1, 0xFFFF);
            addiu(ctx, e::v1, e::v1, 0x40);
            sw(env, ctx, e::a2, e::t1, 0x3C);
            addu(ctx, e::v1, e::t1, e::v1);
            lui(ctx, e::a2, 0x2000);
            ori(ctx, e::a2, e::a2, 0x5);
            lui(ctx, e::v0, 0x5000);
            lw(env, ctx, e::t1, e::t3, 0x4);
            addu(ctx, e::a3, e::v1, e::a0);
            ori(ctx, e::v0, e::v0, 0x5);
            addiu(ctx, e::v1, e::a3, 0x70);
            sw(env, ctx, e::v0, e::a3, 0xC);
            and_(ctx, e::v1, e::v1, e::a1);
            xori(ctx, e::a0, e::t2, 0x1);
            dsll32(ctx, e::v1, e::v1, 0);
            andi(ctx, e::a0, e::a0, 0x1);
            or_(ctx, e::v1, e::v1, e::a2);
            sd(env, ctx, e::v1, e::a3, 0);
            lqOp(env, ctx, e::v0, e::t1, 0x10);
            const bool zero = e::u64(ctx, e::a0) == 0u;
            sqOp(env, ctx, e::v0, e::a3, 0x20);
            e::delaySlot(ctx, 0x150E7Cu);
            if (!zero)
            {
                lw(env, ctx, e::v0, e::a3, 0x24);
                addu(ctx, e::v0, e::v0, e::t4);
                e::delaySlot(ctx, 0x150E88u);
            }
            else
            {
                lui(ctx, e::v0, 0x1);
                or_(ctx, e::v0, e::t4, e::v0);
            }
            sw(env, ctx, e::v0, e::a3, 0x24);
            lqOp(env, ctx, e::v1, e::t1, 0x30);
            lui(ctx, e::v0, 0xFFF);
            ori(ctx, e::v0, e::v0, 0xFFFF);
            addiu(ctx, e::a0, e::t1, 0x50);
            sqOp(env, ctx, e::v1, e::a3, 0x40);
            and_(ctx, e::a0, e::a0, e::v0);
            lui(ctx, e::a1, 0x3000);
            dsll32(ctx, e::a0, e::a0, 0);
            lw(env, ctx, e::v1, e::t1, 0x50);
            lui(ctx, e::a2, 0x5000);
            andi(ctx, e::v1, e::v1, 0x7FFF);
            addiu(ctx, e::v1, e::v1, 0x1);
            or_(ctx, e::v1, e::v1, e::a1);
            or_(ctx, e::v1, e::v1, e::a0);
            sd(env, ctx, e::v1, e::a3, 0x70);
            lw(env, ctx, e::v0, e::t1, 0x50);
            andi(ctx, e::v0, e::v0, 0x7FFF);
            addiu(ctx, e::v0, e::v0, 0x1);
            or_(ctx, e::v0, e::v0, e::a2);
            const uint64_t kind = e::u64(ctx, e::t0);
            sw(env, ctx, e::v0, e::a3, 0x7C);
            e::delaySlot(ctx, 0x150EE8u);
            if (kind != 0u)
            {
                addiu(ctx, e::v0, e::zero, 0x3);
                addiu(ctx, e::v0, e::zero, 0x2);
                e::delaySlot(ctx, 0x150EF4u);
            }
            if (kind == 0u || kind == 3u)
            {
                lui(ctx, e::a0, 0x75);
                addiu(ctx, e::a1, e::zero, 0x1);
                addiu(ctx, e::a0, e::a0, 0x4C80);
                addiu(ctx, e::a2, e::zero, 0xE);
                e::delaySlot(ctx, 0x150F08u);
            }
            else
            {
                addiu(ctx, e::v0, e::zero, 0x1); // bne t0, v0 (2)'s delay slot
                e::delaySlot(ctx, 0x150F10u);
                if (kind == 2u)
                {
                    lui(ctx, e::a0, 0x75);
                    addiu(ctx, e::a1, e::zero, 0x6);
                    addiu(ctx, e::a0, e::a0, 0x4C80);
                    addiu(ctx, e::a2, e::zero, 0x8);
                }
                else
                {
                    daddu(ctx, e::v0, e::zero, e::zero); // bne t0, v0 (1)'s delay slot
                    e::delaySlot(ctx, 0x150F38u);
                    if (kind == 1u)
                    {
                        lui(ctx, e::a0, 0x75);
                        addiu(ctx, e::a1, e::zero, 0x1);
                        addiu(ctx, e::a0, e::a0, 0x4C80);
                        addiu(ctx, e::a2, e::zero, 0x2);
                        if (!textureCall(env, ctx, 0x150F50u, t.q))
                            return true;
                        daddu(ctx, e::v0, e::zero, e::zero);
                    }
                    textureReturn(env, ctx, 0x150F60u);
                    return true;
                }
            }
            if (!textureCall(env, ctx, 0x150F28u, t.q))
                return true;
            daddu(ctx, e::v0, e::zero, e::zero);
            e::delaySlot(ctx, 0x150F30u);
            textureReturn(env, ctx, 0x150F60u);
            return true;
        }

        // The kind dispatch the two builders below share (t9 / t8 = kind): queue sites 0x...24 / 0x...4C (block) or
        // 0x...20 / 0x...4C (strip), branch sites from `base` (the beqz kind's address).
        template <class Env>
        bool textureKind(Env &env, R5900Context *ctx, int kindReg, uint32_t beqzSite, uint32_t firstJal, uint32_t secondJal,
                         uint32_t jrSite, const QueuePlan &q)
        {
            const uint64_t kind = e::u64(ctx, kindReg);
            // beqz kind (its delay slot ran in the caller), addiu v0, 3; bne kind, v0 (addiu v0, 2)
            if (kind != 0u)
            {
                addiu(ctx, e::v0, e::zero, 0x3);
                addiu(ctx, e::v0, e::zero, 0x2);
                e::delaySlot(ctx, beqzSite + 0xCu);
            }
            if (kind == 0u || kind == 3u)
            {
                daddu(ctx, e::a3, e::t6, e::zero);
                lui(ctx, e::a0, 0x75);
                addiu(ctx, e::a0, e::a0, 0x4C80);
                addiu(ctx, e::a1, e::zero, 0x1);
                addiu(ctx, e::a2, e::zero, 0xE);
                e::delaySlot(ctx, beqzSite + 0x24u);
            }
            else
            {
                addiu(ctx, e::v0, e::zero, 0x1); // bne kind, v0 (2)'s delay slot
                e::delaySlot(ctx, beqzSite + 0x2Cu);
                if (kind == 2u)
                {
                    daddu(ctx, e::a3, e::t6, e::zero);
                    lui(ctx, e::a0, 0x75);
                    addiu(ctx, e::a0, e::a0, 0x4C80);
                    addiu(ctx, e::a1, e::zero, 0x6);
                    addiu(ctx, e::a2, e::zero, 0x8);
                }
                else
                {
                    daddu(ctx, e::v0, e::zero, e::zero); // bne kind, v0 (1)'s delay slot
                    e::delaySlot(ctx, beqzSite + 0x58u);
                    if (kind == 1u)
                    {
                        daddu(ctx, e::a3, e::t6, e::zero);
                        lui(ctx, e::a0, 0x75);
                        addiu(ctx, e::a0, e::a0, 0x4C80);
                        addiu(ctx, e::a1, e::zero, 0x1);
                        addiu(ctx, e::a2, e::zero, 0x2);
                        if (!textureCall(env, ctx, secondJal, q))
                            return false;
                        daddu(ctx, e::v0, e::zero, e::zero);
                    }
                    textureReturn(env, ctx, jrSite);
                    return true;
                }
            }
            if (!textureCall(env, ctx, firstJal, q))
                return false;
            daddu(ctx, e::v0, e::zero, e::zero);
            e::delaySlot(ctx, firstJal + 0x8u);
            textureReturn(env, ctx, jrSite);
            return true;
        }

        // func_00150F68 (a0 = object, a1 = the base address word, a2 = format (0, 0x13 or 0x14 build; else v0 = 0), a3,
        // t0, t1 = width, t2, t3 = sign, the stack word = kind). t7 = width (format 0x13), width * 4 (0) or width / 2
        // (0x14); a `break` when t7 is 0 (declined).
        template <class Env>
        bool textureBlock(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), object = e::u32(ctx, e::a0);
            uint32_t arg;
            if (!plainRam(sp0, 4u, arg)) // lw t9, 0x10(sp) after the frame: sp0 itself
                return false;
            TexturePlan t;
            uint32_t counter = 0;
            if (!textureHead(env, sp0, object, e::s64(ctx, e::t3), t, counter))
                return false;
            const uint64_t format = e::u64(ctx, e::a2);
            const bool built = format == 0x13u || format == 0x14u || format == 0u;
            if (!t.early && !t.full && built)
            {
                const uint32_t w = e::u32(ctx, e::t1);
                const uint32_t t7 = format == 0x13u ? w : format == 0u ? w << 2 : static_cast<uint32_t>((static_cast<int32_t>(w + (w >> 31))) >> 1);
                if (t7 == 0u && (format == 0x13u ? e::u64(ctx, e::t1) == 0u : true))
                    return false;
                TextureWrites wr;
                const uint32_t r = t.record;
                wr.add(sp0 - 0x10u, 8u), wr.add(object + 0x3Cu, 4u), wr.add(r, 8u), wr.add(r + 0xCu, 4u);
                wr.add(r + 0x20u, 8u), wr.add(r + 0x40u, 8u), wr.add(r + 0x60u, 8u), wr.add(r + 0x70u, 8u), wr.add(r + 0x7Cu, 4u);
                if (e::overlaps(arg, 4u, t.frame, 8u))
                    return false;
                if (!textureQueue(env, sp0, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(e::read32(env, arg)))), t, wr))
                    return false;
            }
            else if (!t.early && !t.full)
            {
                // the counter still moves: its write is all
                if (!plain(object + 0x3Cu, 4u))
                    return false;
            }

            addiu(ctx, e::sp, e::sp, -0x10);
            daddu(ctx, e::t6, e::a0, e::zero);
            sd(env, ctx, e::ra, e::sp, 0);
            daddu(ctx, e::t4, e::a1, e::zero);
            daddu(ctx, e::t5, e::a2, e::zero);
            daddu(ctx, e::t8, e::t1, e::zero);
            const bool negative = e::s64(ctx, e::t3) < 0;
            lw(env, ctx, e::t9, e::sp, 0x10);
            e::delaySlot(ctx, 0x150F80u);
            if (!negative)
            {
                lui(ctx, e::v1, 0x74);
                lw(env, ctx, e::v0, e::v1, 0x7A2C);
                andi(ctx, e::v0, e::v0, 0x800);
                const bool set = e::u64(ctx, e::v0) != 0u;
                addiu(ctx, e::v0, e::zero, -1);
                e::delaySlot(ctx, 0x150F94u);
                if (set)
                {
                    textureReturn(env, ctx, 0x151160u);
                    return true;
                }
            }
            lw(env, ctx, e::a2, e::t6, 0x3C);
            addiu(ctx, e::v0, e::zero, 0x280);
            const bool more = e::u64(ctx, e::a2) != e::u64(ctx, e::v0);
            lui(ctx, e::a0, 0x74);
            e::delaySlot(ctx, 0x150FA4u);
            if (!more)
            {
                addiu(ctx, e::v0, e::zero, -1);
                e::delaySlot(ctx, 0x150FACu);
                textureReturn(env, ctx, 0x151160u);
                return true;
            }
            addiu(ctx, e::v0, e::zero, 0x180);
            lw(env, ctx, e::v1, e::a0, 0x76B0);
            e::mult(ctx, e::v0, e::a2, e::v0);
            addiu(ctx, e::a1, e::zero, 0xC0);
            addiu(ctx, e::a0, e::a2, 0x1);
            andi(ctx, e::v1, e::v1, 0x1);
            sw(env, ctx, e::a0, e::t6, 0x3C);
            e::mult(ctx, e::v1, e::v1, e::a1);
            addiu(ctx, e::a0, e::zero, 0x13);
            addiu(ctx, e::v0, e::v0, 0x40);
            addu(ctx, e::v0, e::t6, e::v0);
            const uint64_t fmt = e::u64(ctx, e::t5);
            addu(ctx, e::t6, e::v0, e::v1); // the beq's delay slot
            e::delaySlot(ctx, 0x150FE0u);
            if (fmt == 0x13u)
            {
                daddu(ctx, e::t7, e::t8, e::zero); // b's delay slot
                e::delaySlot(ctx, 0x15101Cu);
            }
            else
            {
                slti(ctx, e::v0, e::t5, 0x14);
                const bool low = e::u64(ctx, e::v0) != 0u;
                addiu(ctx, e::v0, e::zero, 0x14);
                e::delaySlot(ctx, 0x150FECu);
                if (low)
                {
                    daddu(ctx, e::v0, e::zero, e::zero);
                    e::delaySlot(ctx, 0x150FF4u);
                    if (fmt != 0u)
                    {
                        ld(env, ctx, e::ra, e::sp, 0); // b 0x151160's delay slot
                        e::delaySlot(ctx, 0x150FFCu);
                        const uint32_t target = e::u32(ctx, e::ra);
                        addiu(ctx, e::sp, e::sp, 0x10);
                        e::jrRa(ctx, 0x151160u, target, true);
                        return true;
                    }
                    sll(ctx, e::t7, e::t8, 2);
                    e::delaySlot(ctx, 0x151014u);
                }
                else
                {
                    daddu(ctx, e::v0, e::zero, e::zero);
                    e::delaySlot(ctx, 0x151004u);
                    if (fmt != 0x14u)
                    {
                        ld(env, ctx, e::ra, e::sp, 0);
                        e::delaySlot(ctx, 0x15100Cu);
                        const uint32_t target = e::u32(ctx, e::ra);
                        addiu(ctx, e::sp, e::sp, 0x10);
                        e::jrRa(ctx, 0x151160u, target, true);
                        return true;
                    }
                    srl(ctx, e::v0, e::t8, 31);
                    addu(ctx, e::v0, e::t8, e::v0);
                    sra(ctx, e::t7, e::v0, 1);
                }
            }
            e::mult(ctx, e::zero, e::t7, e::t2);
            lui(ctx, e::v0, 0xFFF);
            ori(ctx, e::v0, e::v0, 0xFFFF);
            addiu(ctx, e::a0, e::t6, 0x70);
            and_(ctx, e::t4, e::t4, e::v0);
            and_(ctx, e::a0, e::a0, e::v0);
            lui(ctx, e::v1, 0x3000);
            dsll32(ctx, e::a1, e::t0, 16);
            e::setLow64(ctx, e::a2, ctx->lo); // mflo
            dsll32(ctx, e::t1, e::a3, 0);
            srl(ctx, e::a2, e::a2, 4);
            or_(ctx, e::t1, e::t1, e::a1);
            or_(ctx, e::v1, e::a2, e::v1);
            ori(ctx, e::v0, e::a2, 0x8000);
            dsll32(ctx, e::t0, e::t2, 0);
            dsll32(ctx, e::v0, e::v0, 0);
            dsll32(ctx, e::v1, e::v1, 0);
            // beql t7, zero -> break: t7 is never 0 here (the checks); not taken, its delay slot skipped
            dsll32(ctx, e::a0, e::a0, 0);
            dsll32(ctx, e::t5, e::t5, 24);
            dsrl32(ctx, e::v0, e::v0, 0);
            dsrl32(ctx, e::v1, e::v1, 0);
            dsll32(ctx, e::t4, e::t4, 0);
            lui(ctx, e::a3, 0x2000);
            ori(ctx, e::a3, e::a3, 0x6);
            lui(ctx, e::a1, 0x5000);
            ori(ctx, e::t2, e::zero, 0x8000);
            dsll32(ctx, e::t2, e::t2, 12);
            lui(ctx, e::t3, 0x5000);
            or_(ctx, e::a0, e::a0, e::a3);
            or_(ctx, e::t1, e::t1, e::t5);
            or_(ctx, e::t0, e::t8, e::t0);
            or_(ctx, e::v0, e::v0, e::t2);
            or_(ctx, e::v1, e::v1, e::t4);
            or_(ctx, e::a2, e::a2, e::t3);
            ori(ctx, e::a1, e::a1, 0x6);
            sd(env, ctx, e::a0, e::t6, 0);
            sw(env, ctx, e::a1, e::t6, 0xC);
            sd(env, ctx, e::t1, e::t6, 0x20);
            sd(env, ctx, e::t0, e::t6, 0x40);
            sd(env, ctx, e::v0, e::t6, 0x60);
            sd(env, ctx, e::v1, e::t6, 0x70);
            sw(env, ctx, e::a2, e::t6, 0x7C); // the beqz t9's delay slot
            e::delaySlot(ctx, 0x1510DCu);
            if (!textureKind(env, ctx, e::t9, 0x1510DCu, 0x151124u, 0x151150u, 0x151160u, t.q))
                return true;
            return true;
        }

        // func_00151168 (a0 = object, a1 = the base address word, a2 = format (0x13 / 0x14 halve or quarter the width,
        // else as is), a3, t1 = width, t2 = height, t3 = sign, the stack word = kind).
        template <class Env>
        bool textureStrip(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), object = e::u32(ctx, e::a0);
            uint32_t arg;
            if (!plainRam(sp0, 4u, arg))
                return false;
            TexturePlan t;
            uint32_t counter = 0;
            if (!textureHead(env, sp0, object, e::s64(ctx, e::t3), t, counter))
                return false;
            if (!t.early && !t.full)
            {
                TextureWrites wr;
                const uint32_t r = t.record;
                wr.add(sp0 - 0x10u, 8u), wr.add(object + 0x3Cu, 4u), wr.add(r, 8u), wr.add(r + 0xCu, 4u);
                wr.add(r + 0x20u, 8u), wr.add(r + 0x40u, 8u), wr.add(r + 0x60u, 8u), wr.add(r + 0x70u, 8u), wr.add(r + 0x7Cu, 4u);
                if (e::overlaps(arg, 4u, t.frame, 8u))
                    return false;
                if (!textureQueue(env, sp0, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(e::read32(env, arg)))), t, wr))
                    return false;
            }

            addiu(ctx, e::sp, e::sp, -0x10);
            daddu(ctx, e::t7, e::t1, e::zero);
            sd(env, ctx, e::ra, e::sp, 0);
            daddu(ctx, e::t0, e::a0, e::zero);
            daddu(ctx, e::t5, e::a1, e::zero);
            daddu(ctx, e::t4, e::a2, e::zero);
            daddu(ctx, e::t9, e::a3, e::zero);
            daddu(ctx, e::t1, e::t2, e::zero);
            const bool negative = e::s64(ctx, e::t3) < 0;
            lw(env, ctx, e::t8, e::sp, 0x10);
            e::delaySlot(ctx, 0x151188u);
            if (!negative)
            {
                lui(ctx, e::v1, 0x74);
                lw(env, ctx, e::v0, e::v1, 0x7A2C);
                andi(ctx, e::v0, e::v0, 0x800);
                const bool set = e::u64(ctx, e::v0) != 0u;
                addiu(ctx, e::v0, e::zero, -1);
                e::delaySlot(ctx, 0x15119Cu);
                if (set)
                {
                    textureReturn(env, ctx, 0x15135Cu);
                    return true;
                }
            }
            lw(env, ctx, e::a0, e::t0, 0x3C);
            addiu(ctx, e::v0, e::zero, 0x280);
            const bool more = e::u64(ctx, e::a0) != e::u64(ctx, e::v0);
            lui(ctx, e::v1, 0x74);
            e::delaySlot(ctx, 0x1511ACu);
            if (!more)
            {
                addiu(ctx, e::v0, e::zero, -1);
                e::delaySlot(ctx, 0x1511B4u);
                textureReturn(env, ctx, 0x15135Cu);
                return true;
            }
            addiu(ctx, e::v0, e::zero, 0x180);
            lw(env, ctx, e::a1, e::v1, 0x76B0);
            e::mult(ctx, e::v0, e::a0, e::v0);
            addiu(ctx, e::a2, e::zero, 0xC0);
            srl(ctx, e::v1, e::t7, 31);
            andi(ctx, e::a1, e::a1, 0x1);
            addiu(ctx, e::a0, e::a0, 0x1);
            e::mult(ctx, e::a1, e::a1, e::a2);
            addu(ctx, e::v1, e::t7, e::v1);
            addiu(ctx, e::v0, e::v0, 0x40);
            sw(env, ctx, e::a0, e::t0, 0x3C);
            addu(ctx, e::v0, e::t0, e::v0);
            sra(ctx, e::t7, e::v1, 1);
            addiu(ctx, e::a0, e::zero, 0x14);
            const uint64_t fmt = e::u64(ctx, e::t4);
            addu(ctx, e::t6, e::v0, e::a1); // the bne's delay slot
            e::delaySlot(ctx, 0x1511F4u);
            if (fmt == 0x14u)
            {
                addiu(ctx, e::v0, e::zero, -1);
                addiu(ctx, e::v1, e::t1, 0x3);
                slt(ctx, e::v0, e::v0, e::t1);
                e::movn(ctx, e::v1, e::t1, e::v0);
                sra(ctx, e::t1, e::v1, 2); // b's delay slot
                e::delaySlot(ctx, 0x15120Cu);
                e::mult(ctx, e::a3, e::t7, e::t1);
            }
            else
            {
                addiu(ctx, e::v0, e::zero, 0x13);
                e::mult(ctx, e::a3, e::t7, e::t1); // the bne's delay slot
                e::delaySlot(ctx, 0x151218u);
                if (fmt == 0x13u)
                {
                    srl(ctx, e::v0, e::t1, 31);
                    addu(ctx, e::v0, e::t1, e::v0);
                    sra(ctx, e::t1, e::v0, 1);
                    e::mult(ctx, e::a3, e::t7, e::t1);
                }
            }
            addiu(ctx, e::v0, e::zero, -1);
            slt(ctx, e::v0, e::v0, e::t7);
            addiu(ctx, e::a0, e::t7, 0x3F);
            e::movn(ctx, e::a0, e::t7, e::v0);
            lui(ctx, e::v1, 0xFFF);
            lui(ctx, e::a1, 0x3000);
            ori(ctx, e::v1, e::v1, 0xFFFF);
            sra(ctx, e::a3, e::a3, 2);
            addiu(ctx, e::v0, e::t6, 0x70);
            or_(ctx, e::a1, e::a3, e::a1);
            ori(ctx, e::a2, e::a3, 0x8000);
            and_(ctx, e::t3, e::t5, e::v1);
            and_(ctx, e::v0, e::v0, e::v1);
            sra(ctx, e::a0, e::a0, 6);
            dsll32(ctx, e::a2, e::a2, 0);
            dsll32(ctx, e::a1, e::a1, 0);
            dsll32(ctx, e::v0, e::v0, 0);
            dsll32(ctx, e::t2, e::t9, 0);
            dsll32(ctx, e::a0, e::a0, 16);
            dsll32(ctx, e::t1, e::t1, 0);
            dsrl32(ctx, e::a2, e::a2, 0);
            dsrl32(ctx, e::a1, e::a1, 0);
            dsll32(ctx, e::t3, e::t3, 0);
            lui(ctx, e::t0, 0x2000);
            ori(ctx, e::t0, e::t0, 0x6);
            lui(ctx, e::v1, 0x5000);
            ori(ctx, e::t4, e::zero, 0x8000);
            dsll32(ctx, e::t4, e::t4, 12);
            lui(ctx, e::t5, 0x5000);
            or_(ctx, e::v0, e::v0, e::t0);
            or_(ctx, e::t2, e::t2, e::a0);
            or_(ctx, e::t1, e::t7, e::t1);
            or_(ctx, e::a2, e::a2, e::t4);
            or_(ctx, e::a1, e::a1, e::t3);
            or_(ctx, e::a3, e::a3, e::t5);
            ori(ctx, e::v1, e::v1, 0x6);
            sd(env, ctx, e::v0, e::t6, 0);
            sw(env, ctx, e::v1, e::t6, 0xC);
            sd(env, ctx, e::t2, e::t6, 0x20);
            sd(env, ctx, e::t1, e::t6, 0x40);
            sd(env, ctx, e::a2, e::t6, 0x60);
            sd(env, ctx, e::a1, e::t6, 0x70);
            sw(env, ctx, e::a3, e::t6, 0x7C); // the beqz t8's delay slot
            e::delaySlot(ctx, 0x1512D8u);
            textureKind(env, ctx, e::t8, 0x1512D8u, 0x151320u, 0x15134Cu, 0x15135Cu, t.q);
            return true;
        }

        // ---- func_002D8070 (a0 = light set, a1 = out, a2 = colour out, f12 = distance): the colour's three channels
        // (+0x16158.. as words, negatives as 0) packed into *a2; out +0x1C = the fade 255 * (1 + near / (far - near) -
        // f12 / (far - near)) clamped to 0..255 (255 when f12 <= 0).
        constexpr uint32_t kLightFade = 0x2D8070u;
        // The fade's maths where the host is exact (operands below 2^126, a normal divisor, no result past FLT_MAX - 1
        // ulp); false otherwise (the recompiled function: ps2ee for those).
        struct Fade
        {
            float d, q0, q1, s0, s1, m;
        };
        bool fadeExact(uint32_t nearBits, uint32_t farBits, float f12, Fade &r)
        {
            constexpr uint32_t kLimit = 0x7E800000u; // 2^126
            if ((nearBits & 0x7FFFFFFFu) >= kLimit || (farBits & 0x7FFFFFFFu) >= kLimit || (floatBits(f12) & 0x7FFFFFFFu) >= kLimit)
                return false;
            const float n = bitsFloat(nearBits), f = bitsFloat(farBits);
            r.d = subScalar(f, n);
            const uint32_t ed = (floatBits(r.d) >> 23) & 0xFFu;
            if (ed == 0u || ed == 255u)
                return false;
            r.q0 = divScalar(n, r.d);
            r.q1 = divScalar(f12, r.d);
            r.s0 = addScalar(r.q0, 1.0f);
            r.s1 = subScalar(r.s0, r.q1);
            r.m = mulScalar(r.s1, 255.0f);
            return !fpuPastMax(r.q0) && !fpuPastMax(r.q1) && !fpuPastMax(r.s0) && !fpuPastMax(r.s1) && !fpuPastMax(r.m);
        }
        template <class Env>
        bool lightFade(Env &env, R5900Context *ctx)
        {
            const uint32_t base = e::u32(ctx, e::a0) + 0x10000u;
            uint32_t fields;
            if (!plainRam(base + 0x6150u, 0x14u, fields) || !plain(e::u32(ctx, e::a2), 8u) || !plain(e::u32(ctx, e::a1) + 0x1Cu, 4u))
                return false;
            // f12 > 0 by the compare key: the fade's maths (after the colour's sd, which must not land on its inputs)
            if (e::fpuKey(0u) < e::fpuKey(floatBits(ctx->f[12])))
            {
                uint32_t out;
                plainRam(e::u32(ctx, e::a2), 8u, out);
                Fade r;
                if (e::overlaps(out, 8u, fields, 8u) ||
                    !fadeExact(e::read32(env, fields), e::read32(env, fields + 4u), ctx->f[12], r))
                    return false;
            }
            daddu(ctx, e::a3, e::a0, e::zero);
            // the three channels: lwc1 f1, c.lt.s f1, f0 (0); bc1t (daddu reg, 0); cvt.w.s + mfc1 when not negative
            const int regs[3] = {e::t0, e::a0, e::v1};
            const uint32_t sites[3] = {0x2D808Cu, 0x2D80B4u, 0x2D80DCu};
            for (int k = 0; k < 3; ++k)
            {
                lui(ctx, e::v0, 0x1);
                if (k == 0)
                {
                    addu(ctx, e::v0, e::a3, e::v0);
                    e::mtc1(ctx, e::zero, 0);
                }
                else
                {
                    e::mtc1(ctx, e::zero, 0);
                    addu(ctx, e::v0, e::a3, e::v0);
                }
                lwc1(env, ctx, 1, e::v0, 0x6158 + 4 * k);
                e::setC(ctx, e::cLt(ctx, 1, 0));
                daddu(ctx, regs[k], e::zero, e::zero);
                e::delaySlot(ctx, sites[k]);
                if (!e::cBit(ctx))
                {
                    e::setFBits(ctx, 1, static_cast<uint32_t>(fpuToWord(ctx->f[1])));
                    e::mfc1(ctx, regs[k], 1);
                }
            }
            dsll32(ctx, e::v0, e::t0, 0);
            dsll32(ctx, e::a0, e::a0, 0);
            e::mtc1(ctx, e::zero, 0);
            dsrl32(ctx, e::v0, e::v0, 0);
            dsrl(ctx, e::a0, e::a0, 24);
            dsll32(ctx, e::v1, e::v1, 0);
            or_(ctx, e::v0, e::v0, e::a0);
            dsrl(ctx, e::v1, e::v1, 16);
            e::setC(ctx, e::cLt(ctx, 0, 12));
            or_(ctx, e::v0, e::v0, e::v1);
            sd(env, ctx, e::v0, e::a2, 0); // the bc1f's delay slot
            e::delaySlot(ctx, 0x2D8114u);
            if (e::cBit(ctx))
            {
                lui(ctx, e::v0, 0x1);
                lui(ctx, e::at, 0x3F80);
                e::mtc1(ctx, e::at, 2);
                addu(ctx, e::v0, e::a3, e::v0);
                lui(ctx, e::at, 0x437F);
                e::mtc1(ctx, e::at, 3);
                lwc1(env, ctx, 0, e::v0, 0x6150);
                lwc1(env, ctx, 1, e::v0, 0x6154);
                ctx->f[1] = subScalar(ctx->f[1], ctx->f[0]);
                ctx->f[0] = divScalar(ctx->f[0], ctx->f[1]);
                ctx->f[1] = divScalar(ctx->f[12], ctx->f[1]);
                ctx->f[0] = addScalar(ctx->f[0], ctx->f[2]);
                ctx->f[0] = subScalar(ctx->f[0], ctx->f[1]);
                ctx->f[0] = mulScalar(ctx->f[0], ctx->f[3]); // b's delay slot
                e::delaySlot(ctx, 0x2D8150u);
            }
            else
            {
                lui(ctx, e::at, 0x437F);
                e::mtc1(ctx, e::at, 0);
            }
            e::mtc1(ctx, e::zero, 1);
            e::setC(ctx, e::cLt(ctx, 0, 1));
            if (e::cBit(ctx))
            {
                e::setFBits(ctx, 0, e::fBits(ctx, 1)); // bc1tl's delay slot
                e::delaySlot(ctx, 0x2D8170u);
            }
            lui(ctx, e::at, 0x437F);
            e::mtc1(ctx, e::at, 1);
            e::setC(ctx, e::cLt(ctx, 1, 0));
            if (e::cBit(ctx))
            {
                e::setFBits(ctx, 0, e::fBits(ctx, 1));
                e::delaySlot(ctx, 0x2D818Cu);
            }
            const uint32_t target = e::u32(ctx, e::ra);
            swc1(env, ctx, 0, e::a1, 0x1C);
            e::jrRa(ctx, 0x2D8194u, target, true);
            return true;
        }

        // ---- cModel_nodeLayerScreen 0x14BE38 (a0 = model, a1 = node): the node's layer: 2 (0x14 with the model's
        // flag 0x2000), 0x1E when the node is translucent-flagged and the model's alpha * its three colour bytes / 255
        // comes to exactly 1.0, 0x20 when the node's 0x40000000 flag and the word at 0x747AA8 are set.
        constexpr uint32_t kNodeLayer = 0x14BE38u;
        template <class Env>
        bool nodeLayer(Env &env, R5900Context *ctx)
        {
            const uint32_t model = e::u32(ctx, e::a0), node = e::u32(ctx, e::a1);
            uint32_t flags, nflags, alpha;
            if (!plainRam(model + 0x250u, 4u, flags) || !plainRam(node + 0x380u, 4u, nflags) ||
                !plainRam(model + 0x24Cu, 4u, alpha) || !plain(model + 0x2B6u, 2u) || !plain(model + 0x2BCu, 1u) ||
                !plain(0x747AA8u, 4u))
                return false;
            if ((e::read32(env, nflags) & 0x20000000u) != 0u && (e::read32(env, alpha) & 0x7FFFFFFFu) >= 0x7E800000u)
                return false; // alpha * (<= 1) stays below FLT_MAX for |alpha| < 2^126
            daddu(ctx, e::a2, e::a0, e::zero);
            daddu(ctx, e::t0, e::a1, e::zero);
            lw(env, ctx, e::v0, e::a2, 0x250);
            addiu(ctx, e::a3, e::zero, 0x2);
            lw(env, ctx, e::v1, e::t0, 0x380);
            addiu(ctx, e::a1, e::zero, 0x14);
            andi(ctx, e::v0, e::v0, 0x2000);
            lui(ctx, e::a0, 0x2000);
            and_(ctx, e::v1, e::v1, e::a0);
            const bool plainNode = e::u64(ctx, e::v1) == 0u;
            e::movn(ctx, e::a3, e::a1, e::v0); // the beqz's delay slot
            e::delaySlot(ctx, 0x14BE5Cu);
            if (!plainNode)
            {
                lbu(env, ctx, e::v0, e::a2, 0x2B6);
                lui(ctx, e::at, 0x437F);
                e::mtc1(ctx, e::at, 3);
                e::mtc1(ctx, e::v0, 4);
                ctx->f[4] = fpuFromSmallWord(static_cast<int32_t>(e::fBits(ctx, 4)));
                lbu(env, ctx, e::v1, e::a2, 0x2B7);
                lbu(env, ctx, e::v0, e::a2, 0x2BC);
                e::mtc1(ctx, e::v1, 2);
                ctx->f[2] = fpuFromSmallWord(static_cast<int32_t>(e::fBits(ctx, 2)));
                lwc1(env, ctx, 0, e::a2, 0x24C);
                ctx->f[4] = divScalar(ctx->f[4], ctx->f[3]);
                lui(ctx, e::at, 0x3F80);
                e::mtc1(ctx, e::at, 5);
                e::mtc1(ctx, e::v0, 1);
                ctx->f[1] = fpuFromSmallWord(static_cast<int32_t>(e::fBits(ctx, 1)));
                ctx->f[2] = divScalar(ctx->f[2], ctx->f[3]);
                ctx->f[1] = divScalar(ctx->f[1], ctx->f[3]);
                ctx->f[0] = mulScalar(ctx->f[0], ctx->f[4]);
                ctx->f[0] = mulScalar(ctx->f[0], ctx->f[2]);
                ctx->f[0] = mulScalar(ctx->f[0], ctx->f[1]);
                e::setC(ctx, e::cEq(ctx, 0, 5));
                if (e::cBit(ctx))
                {
                    addiu(ctx, e::a3, e::zero, 0x1E); // bc1tl's delay slot
                    e::delaySlot(ctx, 0x14BEC8u);
                }
            }
            lw(env, ctx, e::v0, e::t0, 0x380);
            lui(ctx, e::v1, 0x4000);
            and_(ctx, e::v0, e::v0, e::v1);
            const bool layered = e::u64(ctx, e::v0) != 0u;
            lui(ctx, e::v0, 0x74); // the beqz's delay slot
            e::delaySlot(ctx, 0x14BEDCu);
            if (layered)
            {
                addiu(ctx, e::a0, e::zero, 0x20);
                lw(env, ctx, e::v1, e::v0, 0x7AA8);
                e::movn(ctx, e::a3, e::a0, e::v1);
            }
            const uint32_t target = e::u32(ctx, e::ra);
            daddu(ctx, e::v0, e::a3, e::zero);
            e::jrRa(ctx, 0x14BEF0u, target, true);
            return true;
        }

        // ==== batches 3-4, the new method (K's answers on the method, Nov 21) ====================================
        // Readable C++ written from the decomp's matched C (tools/native/sources.csv has each one's file, commit and
        // hash; decomp_watch.py flags a change), fields by the decomp's names (include/godhand/cModel.h at 75cb123).
        // These natives are kRenderPath | kChargeAtEnd: they set what lasts (RAM the game keeps, the return value,
        // the callee-saved registers, pc) and leave scratch registers and the dead stack as they fall; their cycles are
        // the original's (8 per guest call, PS2X_EE_LOOP_CYCLES per backward branch) summed and charged at the end, so
        // they decline unless the env runs at end (the flag guarantees it). Order of work: a plan reads every field and
        // proves every access plain RAM, clear of the stack bytes the original's frames write (so its reads and ours
        // see the same bytes); observers of an inlined observed function run next; then the writes.
        namespace model
        {
            // cModel (CMODEL_COMMON) and cModelNode fields.
            constexpr uint32_t kPacket = 0x220u;      // char *packet[2]: the model's own draw packet, by frame parity
            constexpr uint32_t kMeshHead = 0x25Cu;    // cModelNode *meshHead
            constexpr uint32_t kId = 0x2ACu;          // short id
            constexpr uint32_t kDrawKind = 0x2C0u;    // int drawKind
            constexpr uint32_t kNodeDispFlags = 0x380u; // cModelNode.dispFlags
            constexpr int32_t kKindPerNode = 0xA;     // CMODEL_KIND_PER_NODE
            constexpr uint32_t kNodeSorted = 0x4080u; // CMODEL_NODE_SORTED
            constexpr uint32_t kFrameParity = 0x7476B0u; // D_007476B0: the frame counter (its low bit picks the half)
        }

        // Plain-RAM reads for a plan: each access proved plain and clear of [stackLow, stackHigh) (the bytes below the
        // entry sp the original's frames write before it reads), else ok = false and the native declines.
        template <class Env>
        struct Reader
        {
            Env &env;
            uint32_t stackLow, stackHigh;
            bool ok = true;
            Footprint::Range *log = nullptr; // when set: every read's range (offset, size), for "no write onto a read"
            int logCap = 0, logCount = 0;
            bool span(uint32_t address, uint32_t size, uint32_t &offset)
            {
                if (!plainRam(address, size, offset) || e::overlaps(offset, size, stackLow, stackHigh - stackLow))
                    return ok = false;
                if (log)
                {
                    if (logCount == logCap)
                        return ok = false;
                    log[logCount++] = Footprint::Range{offset, size};
                }
                return true;
            }
            uint8_t u8(uint32_t address)
            {
                uint32_t at;
                return span(address, 1u, at) ? *env.at(at, 1u) : 0u;
            }
            int8_t s8(uint32_t address) { return static_cast<int8_t>(u8(address)); }
            uint16_t u16(uint32_t address)
            {
                uint32_t at;
                if (!span(address, 2u, at))
                    return 0;
                uint16_t v;
                std::memcpy(&v, env.at(at, 2u), 2);
                return v;
            }
            uint64_t u64(uint32_t address)
            {
                uint32_t at;
                return span(address, 8u, at) ? e::read64(env, at) : 0u;
            }
            uint32_t u32(uint32_t address)
            {
                uint32_t at;
                return span(address, 4u, at) ? e::read32(env, at) : 0u;
            }
            int32_t s32(uint32_t address) { return static_cast<int32_t>(u32(address)); }
            int16_t s16(uint32_t address)
            {
                uint32_t at;
                if (!span(address, 2u, at))
                    return 0;
                int16_t v;
                std::memcpy(&v, env.at(at, 2u), 2);
                return v;
            }
        };

        // 0x31A650's work from its plan (queuePlan), readable: the link 0x14FF00 makes. The slot becomes a NEXT tag to
        // the packet; the packet's last tag becomes a NEXT to what the slot linked to before, its QWC / flags kept.
        template <class Env>
        void linkPacket(Env &env, const Link &link, uint32_t packet)
        {
            const uint64_t before = e::read32(env, (link.slot + 4u) & PS2_RAM_MASK); // lwu: the slot's old address
            e::write64(env, link.slot, (static_cast<uint64_t>(packet) << 32) | 0x20000000u);
            const uint64_t tag = e::read64(env, link.tail);
            e::write64(env, link.tail, (tag & 0x0FFFFFFFu) | 0x20000000u | (before << 32));
        }

        // The return to the caller (pc = ra; sp, s0-s7 and ra as at entry: these natives never change them).
        void returnToCaller(R5900Context *ctx)
        {
            ctx->pc = e::u32(ctx, e::ra);
            ctx->in_delay_slot = false;
        }

        // ---- func_0014C5A0: queue the model's own draw packet (decomp src/cod/0012cc70.c) ------------------------
        // void func_0014C5A0(cModel *self, short id): the packet of this frame's half, 0x70 bytes, into the order table
        // D_00754C80: a per-node model (drawKind 0xA) by its first mesh node's sorted flag (kind 2 with `id`, else kind 1
        // index 0xD), any other model by its own drawKind and id.
        constexpr uint32_t kQueueModel = 0x14C5A0u;
        constexpr uint32_t kQueueModelFrame = 0x50u; // its frame (0x40) + 0x31A650's (0x10)

        template <class Env>
        bool queueModel(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kQueueModelFrame, kQueueModelFrame, stackTop))
                return false;
            Reader<Env> rd{env, stackTop, stackTop + kQueueModelFrame};
            const uint32_t self = e::u32(ctx, e::a0);
            const int64_t id = static_cast<int16_t>(e::u32(ctx, e::a1)); // short id
            // the plan, in the original's order: the parity, cModel_getMeshPtr(self, 0) (no walk for 0), the node's
            // flags, the packet, the kind
            const uint32_t parity = rd.u32(model::kFrameParity) & 1u;
            const uint32_t node = rd.u32(self + model::kMeshHead);
            const uint32_t flags = rd.u32(node + model::kNodeDispFlags);
            const uint32_t start = rd.u32(self + model::kPacket + parity * 4u);
            const int32_t kind = rd.s32(self + model::kDrawKind);
            const uint32_t end = start + 0x70u;
            int64_t bucket, index;
            if (kind == model::kKindPerNode)
            {
                const bool sorted = (flags & model::kNodeSorted) != 0u;
                bucket = sorted ? 2 : 1;
                index = sorted ? id : 0xD;
            }
            else
            {
                bucket = kind;
                index = rd.s16(self + model::kId);
            }
            if (!rd.ok || end == 0u) // end 0 takes 0x31A600 (a packet at -0x70): never in practice, the original runs
                return false;
            // 0x31A650(D_00754C80, bucket, index, start, end), its sp 0x40 below ours
            QueuePlan q;
            if (!queuePlan(env, sp - 0x40u, 0x754C80u, static_cast<uint64_t>(bucket), static_cast<uint64_t>(index),
                           start, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(end))), q))
                return false;
            if (q.inRange &&
                (e::overlaps(q.half, 4u, stackTop, kQueueModelFrame) || e::overlaps(q.at, 4u, stackTop, kQueueModelFrame) ||
                 e::overlaps(q.plan.desc0, 4u, stackTop, kQueueModelFrame) ||
                 e::overlaps(q.plan.desc20, 4u, stackTop, kQueueModelFrame) ||
                 e::overlaps(q.plan.slot, 8u, stackTop, kQueueModelFrame) ||
                 e::overlaps(q.plan.tail, 8u, stackTop, kQueueModelFrame)))
                return false;

            // cModel_getMeshPtr's entry observers, with the context they'd see (a1 = 0, ra = 0x14C5D4, sp - 0x40)
            env.call();
            {
                const uint64_t ra = e::u64(ctx, e::ra);
                e::setLow64(ctx, e::a1, 0u);
                e::setS32(ctx, e::ra, 0x14C5D4u);
                e::setS32(ctx, e::sp, sp - 0x40u);
                ctx->pc = kGetMeshPtr;
                env.observe(kGetMeshPtr, ctx);
                e::setLow64(ctx, e::ra, ra);
                e::setS32(ctx, e::sp, sp);
            }
            env.call(); // the jal to 0x31A650
            if (q.inRange)
            {
                env.call(); // its jal to 0x14FF00
                linkPacket(env, q.plan, start);
            }
            returnToCaller(ctx);
            return true;
        }

        // ==== batches 3-4b: the texture cache's misses (K's order, Nov 22) ======================================
        // A miss in func_00151398 inserts the texture's entry and builds its uploads: func_00151630 classifies the file
        // by its header ("TIM2", or "TIM3" with byte 5 != 6: the TIM2 loader func_00151990; "TIM3" with byte 5 = 6:
        // func_00151CC0; "TM..": func_00151FA0; anything else: func_00151688, the game's own pack). A builder writes each
        // picture's TEX0 into the manager's register table (w + 0x3DC88, by frame half and the cursor w + 0x3F8A8, the
        // table func_00151368 reads), queues its image and CLUT uploads (func_00150F68 / 00151168 / 00150DC8: a GS packet
        // in the object's records, linked into the order table D_00754C80) and moves the VRAM cursors (*pa, *pb). The
        // builders are unmatched in the decomp (75cb123): our own C++ from the disassembly, the decomp's names where it
        // has them. Their reads follow their own writes (each link reads what the last one wrote; 0x151688 relocates its
        // pack in place), so they run on a Txn: every read sees the writes made so far and nothing reaches RAM before the
        // end, so a decline midway changes nothing.

        // The Txn's working storage, one per thread, reused (a native never opens a second one while one is open:
        // busy declines).
        struct TxnStore
        {
            struct Write
            {
                uint32_t at;
                uint32_t size;
                alignas(16) uint8_t bytes[16];
            };
            static constexpr uint32_t kWrites = 16384u;
            static constexpr uint32_t kHash = 65536u; // 8-byte granules written (granule + 1; 0 = empty)
            std::vector<Write> writes;
            std::vector<uint32_t> hash, used;
            bool busy = false;
        };
        inline TxnStore &txnStore()
        {
            thread_local TxnStore store;
            if (store.writes.empty())
            {
                store.writes.resize(TxnStore::kWrites);
                store.hash.assign(TxnStore::kHash, 0u);
                store.used.reserve(TxnStore::kWrites * 3u);
            }
            return store;
        }

        // Reads and writes of guest RAM in the original's order, held until commit(). Every access is plain RAM and clear
        // of [stackLow, stackHigh) (the bytes below the entry sp that the original's frames write), else ok = false and
        // the native declines (nothing written). A read returns the latest write over each byte, else RAM.
        template <class Env>
        struct Txn
        {
            Env &env;
            uint32_t stackLow, stackHigh;
            bool ok = true;
            TxnStore *store = nullptr;
            uint32_t count = 0;
            Txn(Env &e, uint32_t low, uint32_t high) : env(e), stackLow(low), stackHigh(high)
            {
                TxnStore &s = txnStore();
                if (s.busy)
                    ok = false;
                else
                    s.busy = true, store = &s;
            }
            ~Txn()
            {
                if (!store)
                    return;
                for (uint32_t slot : store->used)
                    store->hash[slot] = 0u;
                store->used.clear();
                store->busy = false;
            }
            Txn(const Txn &) = delete;
            Txn &operator=(const Txn &) = delete;

            bool span(uint32_t address, uint32_t size, uint32_t &offset)
            {
                if (!ok)
                    return false;
                if (!plainRam(address, size, offset) || e::overlaps(offset, size, stackLow, stackHigh - stackLow))
                    return ok = false;
                return true;
            }
            static uint32_t slotOf(uint32_t granule) { return (granule * 0x9E3779B1u) >> 16; }
            bool written(uint32_t granule) const
            {
                for (uint32_t i = slotOf(granule);; i = (i + 1u) & (TxnStore::kHash - 1u))
                {
                    const uint32_t v = store->hash[i];
                    if (v == 0u)
                        return false;
                    if (v == granule + 1u)
                        return true;
                }
            }
            void mark(uint32_t granule)
            {
                for (uint32_t i = slotOf(granule);; i = (i + 1u) & (TxnStore::kHash - 1u))
                {
                    const uint32_t v = store->hash[i];
                    if (v == granule + 1u)
                        return;
                    if (v == 0u)
                    {
                        store->hash[i] = granule + 1u;
                        store->used.push_back(i);
                        return;
                    }
                }
            }
            void read(uint32_t address, uint32_t size, void *out)
            {
                uint32_t at;
                if (!span(address, size, at))
                {
                    std::memset(out, 0, size);
                    return;
                }
                std::memcpy(out, env.at(at, size), size);
                if (count == 0u) // nothing written yet: RAM is the answer (most reads of a hit path)
                    return;
                bool hit = false;
                for (uint32_t g = at >> 3; g <= (at + size - 1u) >> 3; ++g)
                    hit = hit || written(g);
                if (!hit)
                    return;
                // the latest write over each byte: newest first, until every byte is settled
                uint32_t settled = 0u;
                const uint32_t all = size >= 32u ? ~0u : (1u << size) - 1u;
                uint8_t *o = static_cast<uint8_t *>(out);
                for (uint32_t i = count; i-- > 0u && settled != all;)
                {
                    const TxnStore::Write &w = store->writes[i];
                    const uint32_t lo = std::max(w.at, at), hi = std::min(w.at + w.size, at + size);
                    for (uint32_t b = lo; b < hi; ++b)
                        if (!(settled & (1u << (b - at))))
                        {
                            o[b - at] = w.bytes[b - w.at];
                            settled |= 1u << (b - at);
                        }
                }
            }
            void write(uint32_t address, uint32_t size, const void *in)
            {
                uint32_t at;
                if (!span(address, size, at))
                    return;
                if (count == TxnStore::kWrites || store->used.size() + 3u > TxnStore::kHash / 2u)
                {
                    ok = false;
                    return;
                }
                TxnStore::Write &w = store->writes[count++];
                w.at = at;
                w.size = size;
                std::memcpy(w.bytes, in, size);
                for (uint32_t g = at >> 3; g <= (at + size - 1u) >> 3; ++g)
                    mark(g);
            }
            template <class T>
            T get(uint32_t address)
            {
                T v;
                read(address, sizeof(T), &v);
                return v;
            }
            uint8_t u8(uint32_t a) { return get<uint8_t>(a); }
            int8_t s8(uint32_t a) { return static_cast<int8_t>(get<uint8_t>(a)); }
            uint16_t u16(uint32_t a) { return get<uint16_t>(a); }
            int16_t s16(uint32_t a) { return static_cast<int16_t>(get<uint16_t>(a)); }
            uint32_t u32(uint32_t a) { return get<uint32_t>(a); }
            int32_t s32(uint32_t a) { return static_cast<int32_t>(get<uint32_t>(a)); }
            uint64_t u64(uint32_t a) { return get<uint64_t>(a); }
            void w8(uint32_t a, uint8_t v) { write(a, 1u, &v); }
            void w16(uint32_t a, uint16_t v) { write(a, 2u, &v); }
            void w32(uint32_t a, uint32_t v) { write(a, 4u, &v); }
            void w64(uint32_t a, uint64_t v) { write(a, 8u, &v); }
            // lq / sq: the address's low 4 bits cleared
            void lq(uint32_t a, uint8_t (&v)[16]) { read(a & ~0xFu, 16u, v); }
            void sq(uint32_t a, const uint8_t (&v)[16]) { write(a & ~0xFu, 16u, v); }
            void commit()
            {
                for (uint32_t i = 0; i < count; ++i)
                {
                    const TxnStore::Write &w = store->writes[i];
                    std::memcpy(env.at(w.at, w.size), w.bytes, w.size);
                }
            }
        };

        inline uint64_t sx32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

        // 0x31A650(D_00754C80, bucket, order, packet, tail) and its link 0x14FF00, on a Txn (queuePlan + linkPacket's
        // work in the original's order): a bucket below 10 picks this half's descriptor; the slot (the order clamped to
        // the descriptor's first) becomes a NEXT tag to the packet, the packet's last tag (tail; the packet when the
        // tail register is 0) a NEXT to what the slot linked before.
        template <class Env>
        void txnQueue(Txn<Env> &t, uint64_t bucket, uint64_t order, uint32_t packet, uint32_t tail)
        {
            t.env.call(); // jal 0x31A650
            if (static_cast<int64_t>(bucket) >= 10)
                return;
            const uint32_t where = kOrderTable + (((t.u32(kFrameHalf) & 1u) << 2) + (static_cast<uint32_t>(bucket) << 3));
            const uint32_t descriptor = t.u32(where + 0xB00u);
            const uint32_t last = tail != 0u ? tail : packet;
            t.env.call(); // jal 0x14FF00
            const uint64_t first = sx32(t.u32(descriptor + 0x20u));
            const uint32_t slots = t.u32(descriptor);
            const uint64_t lowest = first < order ? first : order;
            const uint32_t slot = slots + ((static_cast<uint32_t>(first) - static_cast<uint32_t>(lowest)) << 4);
            const uint64_t before = t.u32(slot + 4u); // lwu
            t.w64(slot, (static_cast<uint64_t>(packet) << 32) | 0x20000000u);
            const uint64_t tag = t.u64(last);
            t.w64(last, (tag & 0x0FFFFFFFu) | 0x20000000u | (before << 32));
        }
        // The three upload functions' queue: kind 0 / 3 bucket 1 order 0xE, 2: 6 / 8, 1: 1 / 2, else none (a 64-bit
        // compare of the kind register).
        template <class Env>
        void txnQueueKind(Txn<Env> &t, uint64_t kind, uint32_t record)
        {
            uint32_t bucket = 0, order = 0;
            if (kind == 0u || kind == 3u)
                bucket = 1u, order = 0xEu;
            else if (kind == 2u)
                bucket = 6u, order = 8u;
            else if (kind == 1u)
                bucket = 1u, order = 2u;
            if (bucket != 0u)
                txnQueue(t, bucket, order, record, record + 0x80u);
        }
        // The upload functions' head: -1 when the GS flag 0x800 is set (unless `sign` < 0) or the object's 0x280
        // records are used up; else the record (object + 0x40 + n * 0x180, this half's 0xC0 in) and the counter moved.
        template <class Env>
        bool uploadRecord(Txn<Env> &t, uint32_t object, int64_t sign, uint32_t &record)
        {
            if (sign >= 0 && (t.u32(kGsFlag) & 0x800u) != 0u)
                return false;
            const uint32_t n = t.u32(object + 0x3Cu);
            if (n == 0x280u)
                return false;
            const uint32_t half = t.u32(kFrameHalf) & 1u;
            t.w32(object + 0x3Cu, n + 1u);
            record = object + (n * 0x180u + 0x40u) + half * 0xC0u;
            return true;
        }

        // func_00150F68: an image upload (a1 = the source, a2 = the PSM: 0x13, 0x14 or 0 build a packet; a3 = DBP, t0 =
        // DBW, t1 = width, t2 = height, t3 = sign, the stack word = the kind). Returns v0 (-1 / 0); false = decline (its
        // `break 7`, a zero transfer width).
        template <class Env>
        bool txnImageUp(Txn<Env> &t, uint32_t object, uint64_t src, uint64_t psm, uint64_t dbp, uint64_t dbw,
                        uint64_t width, uint64_t height, int64_t sign, uint64_t kind)
        {
            uint32_t r;
            if (!uploadRecord(t, object, sign, r))
                return t.ok;
            uint64_t t7;
            if (psm == 0x13u)
                t7 = width;
            else if (static_cast<int64_t>(psm) < 0x14)
            {
                if (psm != 0u)
                    return t.ok;
                t7 = sx32(static_cast<uint32_t>(width) << 2);
            }
            else if (psm == 0x14u)
            {
                const uint32_t w = static_cast<uint32_t>(width);
                t7 = sx32(static_cast<uint32_t>(static_cast<int32_t>(w + (w >> 31)) >> 1));
            }
            else
                return t.ok;
            if (t7 == 0u)
                return false; // break 7
            const uint32_t lo = static_cast<uint32_t>(static_cast<int64_t>(static_cast<int32_t>(t7)) *
                                                      static_cast<int64_t>(static_cast<int32_t>(height)));
            const uint32_t qwc = lo >> 4;
            t.w64(r, (static_cast<uint64_t>((r + 0x70u) & 0x0FFFFFFFu) << 32) | 0x20000006u);
            t.w32(r + 0xCu, 0x50000006u);
            t.w64(r + 0x20u, (dbp << 32) | (dbw << 48) | (psm << 56));
            t.w64(r + 0x40u, width | (height << 32));
            t.w64(r + 0x60u, static_cast<uint64_t>(qwc | 0x8000u) | (0x8000ull << 44));
            t.w64(r + 0x70u, static_cast<uint64_t>(qwc | 0x30000000u) | (static_cast<uint64_t>(static_cast<uint32_t>(src) & 0x0FFFFFFFu) << 32));
            t.w32(r + 0x7Cu, qwc | 0x50000000u);
            txnQueueKind(t, kind, r);
            return t.ok;
        }

        // func_00151168: a strip upload (decomp 00151168.c: a1 = src, a2 = psm, a3 = dbp, t1 = width, t2 = height,
        // t3 = mode, the stack word = the kind; width halved, height quartered for 0x14 / halved for 0x13).
        template <class Env>
        bool txnStripUp(Txn<Env> &t, uint32_t object, uint64_t src, uint64_t psm, uint64_t dbp, uint64_t width,
                        uint64_t height, int64_t mode, uint64_t kind)
        {
            uint32_t r;
            if (!uploadRecord(t, object, mode, r))
                return t.ok;
            const uint32_t w32 = static_cast<uint32_t>(width);
            const int32_t w = static_cast<int32_t>(w32 + (w32 >> 31)) >> 1;
            uint64_t h = height;
            if (psm == 0x14u)
            {
                const uint32_t v = static_cast<int64_t>(height) > -1 ? static_cast<uint32_t>(height) : static_cast<uint32_t>(height) + 3u;
                h = sx32(static_cast<uint32_t>(static_cast<int32_t>(v) >> 2));
            }
            else if (psm == 0x13u)
            {
                const uint32_t v = static_cast<uint32_t>(height);
                h = sx32(static_cast<uint32_t>(static_cast<int32_t>(v + (v >> 31)) >> 1));
            }
            const uint32_t lo = static_cast<uint32_t>(static_cast<int64_t>(w) * static_cast<int64_t>(static_cast<int32_t>(h)));
            const uint32_t qwc = static_cast<uint32_t>(static_cast<int32_t>(lo) >> 2);
            const int32_t bw = (w > -1 ? w : w + 0x3F) >> 6;
            t.w64(r, (static_cast<uint64_t>((r + 0x70u) & 0x0FFFFFFFu) << 32) | 0x20000006u);
            t.w32(r + 0xCu, 0x50000006u);
            t.w64(r + 0x20u, (dbp << 32) | (static_cast<uint64_t>(static_cast<uint32_t>(bw)) << 48));
            t.w64(r + 0x40u, sx32(static_cast<uint32_t>(w)) | (h << 32));
            t.w64(r + 0x60u, static_cast<uint64_t>(qwc | 0x8000u) | (0x8000ull << 44));
            t.w64(r + 0x70u, static_cast<uint64_t>(qwc | 0x30000000u) | (static_cast<uint64_t>(static_cast<uint32_t>(src) & 0x0FFFFFFFu) << 32));
            t.w32(r + 0x7Cu, qwc | 0x50000000u);
            txnQueueKind(t, kind, r);
            return t.ok;
        }

        // func_00150DC8: an upload from a prepared GS packet (a1 = its holder, +4 the packet; a2 = the VRAM base: added
        // to the packet's BITBLTBUF word, or (a3 odd, a CLUT) or-ed with 0x10000 in its place; t0 = the kind).
        template <class Env>
        bool txnPacketUp(Txn<Env> &t, uint32_t object, uint32_t holder, uint64_t base, int64_t sign, uint64_t kind)
        {
            uint32_t r;
            if (!uploadRecord(t, object, sign, r))
                return t.ok;
            const uint32_t p = t.u32(holder + 4u);
            t.w32(r + 0xCu, 0x50000005u);
            t.w64(r, (static_cast<uint64_t>((r + 0x70u) & 0x0FFFFFFFu) << 32) | 0x20000005u);
            uint8_t q[16];
            t.lq(p + 0x10u, q);
            t.sq(r + 0x20u, q);
            if (sign & 1)
                t.w32(r + 0x24u, static_cast<uint32_t>(base | 0x10000u));
            else
                t.w32(r + 0x24u, t.u32(r + 0x24u) + static_cast<uint32_t>(base));
            t.lq(p + 0x30u, q);
            t.sq(r + 0x40u, q);
            const uint32_t count = (t.u32(p + 0x50u) & 0x7FFFu) + 1u;
            t.w64(r + 0x70u, (static_cast<uint64_t>((p + 0x50u) & 0x0FFFFFFFu) << 32) | (count | 0x30000000u));
            t.w32(r + 0x7Cu, ((t.u32(p + 0x50u) & 0x7FFFu) + 1u) | 0x50000000u);
            txnQueueKind(t, kind, r);
            return t.ok;
        }

        // func_00309C30(psm, width): TBW, the buffer width in 64-texel units (even for 0x13 / 0x14), 0 for a PSM it
        // doesn't know (decomp 00309c30.c). Its jump table (0x4502E0) read like the code reads it: a target this
        // doesn't know declines.
        template <class Env>
        bool txnBufferWidth(Txn<Env> &t, uint64_t psm, uint32_t width, uint32_t &tbw)
        {
            tbw = 0u;
            if (psm >= 0x3Bu) // sltiu: the whole register
                return t.ok;
            const uint32_t target = t.u32(0x4502E0u + (static_cast<uint32_t>(psm) << 2));
            const uint32_t v = width + 0x3Fu;
            const int32_t w = (static_cast<int32_t>(v) > -1 ? static_cast<int32_t>(v) : static_cast<int32_t>(width + 0x7Eu)) >> 6;
            if (target == 0x309C54u)
                tbw = static_cast<uint32_t>(w);
            else if (target == 0x309C70u)
                tbw = static_cast<uint32_t>(w + (w & 1));
            else if (target != 0x309C94u)
                return t.ok = false;
            return t.ok;
        }

        // The TEX0 the TIM loaders build from the picture's own (GsTex0 & ~PSM, its PSM from the format table): TBP =
        // the VRAM cursor, TBW, TCC set, TFX / CPSM / CSA / CLD cleared then CLD = 1, CBP = the CLUT cursor.
        inline uint64_t timTex0(uint64_t s0, uint32_t tbp, uint32_t tbw, uint32_t cbp)
        {
            s0 = (s0 & ~0x3FFFull) | (tbp & 0x3FFFu);
            s0 = (s0 & 0xFFFFFFFFFFF03FFFull) | (static_cast<uint64_t>(tbw & 0x3Fu) << 14);
            s0 |= 0x400000000ull;
            s0 &= 0xFF87FFFFFFFFFFFFull & 0xFF7FFFFFFFFFFFFFull & 0xE0FFFFFFFFFFFFFFull & 0x1FFFFFFFFFFFFFFFull;
            s0 |= 0x2000000000000000ull;
            s0 &= 0xFFF8001FFFFFFFFFull;
            return s0 | (static_cast<uint64_t>(cbp & 0x3FFFu) << 37);
        }
        constexpr uint32_t kTexCounters = 0x38000u; // w + 0x38000: +0x5C88 the register table, +0x78A8 the cursor,
                                                   // +0x78AA / +0x78AB the image / CLUT uploads (bytes)
        constexpr uint32_t kTim2Formats = 0x41ED68u, kTim3Formats = 0x41ED80u, kTmFormats = 0x41ED98u;

        template <class Env>
        void txnBump8(Txn<Env> &t, uint32_t at) { t.w8(at, static_cast<uint8_t>(t.u8(at) + 1u)); }

        // The texture_slots fix (texture_slots.h) where its hook sits in the TIM2 loader (0x151C70): the picture's size
        // grown to its real footprint, while the flag is on.
        inline bool textureSlotsOn()
        {
            static const uint32_t mask = ps2xPatchFlagMask("texture_slots");
            return (g_ps2xPatchFlags.load(std::memory_order_relaxed) & mask) != 0u;
        }

        // A CLUT upload after a TIM picture: 16 x 16 for 8-bit pictures (type 5) and any CLUT not 0x40 bytes, else 8 x 2
        // (`small`: the TM loader's 8 x 8).
        template <class Env>
        bool txnClut(Txn<Env> &t, uint32_t w, uint32_t src, uint32_t cbp, uint64_t type, bool big, uint64_t smallH)
        {
            t.env.call(); // jal 0x150F68
            return big ? txnImageUp(t, w, src, 0u, sx32(cbp), 1u, 0x10u, 0x10u, 0, sx32(static_cast<uint32_t>(type)))
                       : txnImageUp(t, w, src, 0u, sx32(cbp), 1u, 8u, smallH, 0, sx32(static_cast<uint32_t>(type)));
        }

        // func_00151990, the TIM2 loader (w, name = the file, pa / pb = the VRAM / CLUT cursors, type = the kind): its
        // pictures (+0x6 of them) from +0x10 (+0x80 when byte 5 is set), 0x30 apart.
        constexpr uint32_t kUploadTim2 = 0x151990u, kUploadTim3 = 0x151CC0u, kUploadPack = 0x151688u, kUploadTm = 0x151FA0u;
        constexpr int kTextureLoopLimit = 4096; // pictures / entries before declining
        template <class Env>
        bool txnTim2(Txn<Env> &t, uint32_t w, uint32_t name, uint32_t pa, uint32_t pb, uint64_t type)
        {
            const uint32_t n = t.u16(name + 6u);
            uint32_t s5 = t.u8(name + 5u) == 0u ? name + 0x10u : name + 0x80u;
            uint32_t fp = t.u32(pa);
            uint32_t s7 = t.u32(pb);
            if (n > static_cast<uint32_t>(kTextureLoopLimit))
                return false;
            const uint32_t c = w + kTexCounters;
            for (uint32_t i = 0; i < n && t.ok; ++i)
            {
                if (i != 0u)
                    t.env.checkpoint(32u); // bnez at 0x151C74
                const uint32_t psm = t.u32(kTim2Formats + ((static_cast<uint32_t>(t.u8(s5 + 0x13u)) - 1u) << 2));
                uint64_t s0 = t.u64(s5 + 0x18u) & 0xFFFFFFFFFC0FFFFFull;
                const uint32_t width = t.u16(s5 + 0x14u);
                const uint32_t image = s5 + t.u16(s5 + 0xCu);
                s0 |= static_cast<uint64_t>(psm & 0x3Fu) << 20;
                const uint32_t height = t.u16(s5 + 0x16u);
                t.w64(s5 + 0x18u, s0);
                t.env.call(); // jal 0x309C30
                uint32_t tbw;
                if (!txnBufferWidth(t, sx32(psm), width, tbw))
                    return false;
                const uint32_t cursor = t.u16(c + 0x78A8u);
                const uint32_t half = t.u32(kFrameHalf) & 1u;
                const uint32_t clut = (s5 + t.u16(s5 + 0xCu)) + t.u32(s5 + 8u);
                t.w64(c + (half << 3) + (cursor << 4) + 0x5C88u, timTex0(s0, fp, tbw, s7));
                t.w16(c + 0x78A8u, static_cast<uint16_t>(t.u16(c + 0x78A8u) + 1u));
                t.env.call(); // jal 0x150F68
                if (!txnImageUp(t, w, image, sx32(psm), sx32(fp), tbw, width, height, 0, sx32(static_cast<uint32_t>(type))))
                    return false;
                txnBump8(t, c + 0x78AAu);
                const bool big = t.u8(s5 + 0x13u) == 5u || t.u32(s5 + 4u) != 0x40u;
                if (!txnClut(t, w, clut, s7, type, big, 2u))
                    return false;
                s7 += 4u;
                txnBump8(t, c + 0x78ABu);
                uint32_t size = t.u32(s5 + 8u);
                if (textureSlotsOn() && (s5 & PS2_RAM_MASK) + 0x30u <= PS2_RDRAM_SIZE)
                {
                    const uint32_t header = s5; // the hook's $s5 - 0x30 (s5 already moved on there)
                    size = texture_slots::fittedSize(t.u8(header + 0x13u), t.u16(header + 0x14u), t.u16(header + 0x16u), size);
                }
                s5 += 0x30u;
                fp += size >> 8;
            }
            t.w32(pa, fp);
            t.w32(pb, s7);
            return t.ok;
        }

        // func_00151CC0, the TIM3 loader (one picture at +0x10, its strip upload func_00151168, its formats table).
        template <class Env>
        bool txnTim3(Txn<Env> &t, uint32_t w, uint32_t name, uint32_t pa, uint32_t pb, uint64_t type)
        {
            const uint32_t s6 = name + 0x10u;
            const uint32_t format = t.u8(s6 + 0x13u);
            uint64_t s0 = t.u64(s6 + 0x18u) & 0xFFFFFFFFFC0FFFFFull;
            const uint32_t psm = t.u32(kTim3Formats + ((format - 1u) << 2));
            const uint32_t cur = t.u32(pa);
            s0 |= static_cast<uint64_t>(psm & 0x3Fu) << 20;
            uint32_t s7 = t.u32(pb);
            const uint32_t width = t.u16(s6 + 0x14u);
            const uint32_t height = t.u16(s6 + 0x16u);
            const uint32_t image = s6 + t.u16(s6 + 0xCu);
            t.w64(s6 + 0x18u, s0);
            t.env.call(); // jal 0x309C30
            uint32_t tbw;
            if (!txnBufferWidth(t, sx32(psm), width, tbw))
                return false;
            const uint32_t imageSize = t.u32(s6 + 8u);
            const uint32_t c = w + kTexCounters;
            const uint32_t cursor = t.u16(c + 0x78A8u);
            const uint32_t half = t.u32(kFrameHalf) & 1u;
            const uint32_t clut = (s6 + t.u16(s6 + 0xCu)) + imageSize;
            t.w64(c + (half << 3) + (cursor << 4) + 0x5C88u, timTex0(s0, cur, tbw, s7));
            t.w16(c + 0x78A8u, static_cast<uint16_t>(t.u16(c + 0x78A8u) + 1u));
            t.env.call(); // jal 0x151168
            if (!txnStripUp(t, w, image, sx32(psm), sx32(cur), width, height, 0, sx32(static_cast<uint32_t>(type))))
                return false;
            txnBump8(t, c + 0x78AAu);
            const bool big = t.u8(s6 + 0x13u) == 5u || t.u32(s6 + 4u) != 0x40u;
            if (!txnClut(t, w, clut, s7, type, big, 2u))
                return false;
            s7 += 4u;
            txnBump8(t, c + 0x78ABu);
            t.w32(pa, cur + (t.u32(s6 + 8u) >> 8));
            t.w32(pb, s7);
            return t.ok;
        }

        // func_00151FA0, the TM loader: one T4 strip of the whole image block (128 wide), then per picture (the low byte
        // of +4; 0 = 256) its TEX0 and CLUT, the pictures' offsets from +0x10.
        template <class Env>
        bool txnTm(Txn<Env> &t, uint32_t w, uint32_t name, uint32_t pa, uint32_t pb, uint64_t type)
        {
            const uint32_t s5 = name;
            const uint32_t base = s5 + 0x10u;
            const uint32_t images = t.u32(s5 + 0x10u) + s5;
            const uint32_t pictures = t.u32(s5 + 4u);
            uint32_t s4 = t.u32(pa);
            const uint32_t t2 = (t.u32(s5 + 0xCu) >> 13) << 7;
            const uint32_t s0start = images + (pictures << 6);
            uint32_t s3 = t.u32(pb);
            t.env.call(); // jal 0x151168
            if (!txnStripUp(t, w, s0start, 0x14u, sx32(s4), 0x80u, sx32(t2), 0, sx32(static_cast<uint32_t>(type))))
                return false;
            const uint32_t c = w + kTexCounters;
            txnBump8(t, c + 0x78AAu);
            uint32_t s6 = s0start + t.u32(s5 + 0xCu);
            uint32_t s7 = pictures & 0xFFu;
            uint32_t index = 0u;
            for (int loop = 0;; ++loop)
            {
                if (!t.ok)
                    return false;
                const uint32_t s1 = t.u32(base + (index << 2)) + s5 + 0x10u;
                uint64_t s0 = t.u64(s1 + 0x18u) & 0xFFFFFFFFFC0FFFFFull;
                const uint32_t width = t.u16(s1 + 0x14u);
                const uint32_t psm = t.u32(kTmFormats + ((static_cast<uint32_t>(t.u8(s1 + 0x13u)) - 1u) << 2));
                s0 |= static_cast<uint64_t>(psm & 0x3Fu) << 20;
                t.w64(s1 + 0x18u, s0);
                t.env.call(); // jal 0x309C30
                uint32_t tbw;
                if (!txnBufferWidth(t, sx32(psm), width, tbw))
                    return false;
                s0 = timTex0(s0, s4, tbw, s3);
                const uint32_t clut = s6;
                s6 = clut + t.u32(s1 + 4u);
                t.env.call(); // jal 0x150F68
                const bool big = t.u8(s1 + 0x13u) == 5u;
                if (!(big ? txnImageUp(t, w, clut, 0u, sx32(s3), 1u, 0x10u, 0x10u, 0, sx32(static_cast<uint32_t>(type)))
                          : txnImageUp(t, w, clut, 0u, sx32(s3), 1u, 8u, 8u, 0, sx32(static_cast<uint32_t>(type)))))
                    return false;
                const uint8_t clutUploads = t.u8(c + 0x78ABu);
                const uint32_t cursor = t.u16(c + 0x78A8u);
                t.w8(c + 0x78ABu, static_cast<uint8_t>(clutUploads + 1u));
                index = (index + 1u) & 0xFFu;
                s3 += 4u;
                s7 = (s7 - 1u) & 0xFFu;
                const uint32_t half = t.u32(kFrameHalf) & 1u;
                t.w64(c + (half << 3) + (cursor << 4) + 0x5C88u, s0);
                t.w16(c + 0x78A8u, static_cast<uint16_t>(t.u16(c + 0x78A8u) + 1u));
                s4 += t.u32(s1 + 8u) >> 8;
                if (s7 == 0u)
                    break;
                t.env.checkpoint(32u); // bnez at 0x152274
            }
            t.w32(pa, s4);
            t.w32(pb, s3);
            return t.ok;
        }

        // func_00151688, the game's own pack: +0 its entry count (bit 31: relocated), +4 / +6 the image size, +8 / +0xC
        // the image and CLUT holders' offsets (+0x10 / +0x12 of them, 0x10 each), entries of 0x18 from +0x18 (+8 a
        // TEX0 whose TBP / CBP are offsets from the cursors). It moves the cursors first, writes each entry's TEX0,
        // relocates the holders' packet pointers once, then uploads every image and CLUT packet (func_00150DC8).
        template <class Env>
        bool txnPack(Txn<Env> &t, uint32_t w, uint32_t name, uint32_t pa, uint32_t pb, uint64_t type)
        {
            const uint32_t s2 = name;
            const uint32_t cur = t.u32(pa);
            const uint32_t h = t.u16(s2 + 6u), wd = t.u16(s2 + 4u);
            const int32_t area = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(static_cast<int32_t>(wd)) * static_cast<int32_t>(h)));
            const uint32_t pbv = t.u32(pb);
            const uint32_t s1start = s2 + t.u32(s2 + 8u);
            const uint32_t s4start = s2 + t.u32(s2 + 0xCu);
            const uint32_t fp = t.u32(s2) & 0x7FFFFFFFu;
            t.w32(pa, cur + static_cast<uint32_t>(area / 64));
            t.w32(pb, t.u32(pb) + (fp << 2));
            const uint32_t half = t.u32(kFrameHalf) & 1u;
            if (fp > static_cast<uint32_t>(kTextureLoopLimit))
                return false;
            const uint32_t c = w + kTexCounters;
            for (uint32_t k = 0; k < fp && t.ok; ++k)
            {
                if (k != 0u)
                    t.env.checkpoint(32u); // bnez at 0x151820
                const uint32_t entry = s2 + 0x18u + 0x18u * k;
                uint64_t s0 = t.u64(entry + 8u);
                s0 = (s0 & ~0x3FFFull) | (((s0 & 0x3FFFu) + cur) & 0x3FFFu);
                t.env.call(); // jal __muldi3 (the CBP offset * 4)
                const uint64_t cbp = (((s0 >> 37) & 0x3FFFu) * 4u + pbv) & 0x3FFFu;
                const uint32_t cursor = t.u16(c + 0x78A8u);
                s0 = ((s0 & 0xFFF8001FFFFFFFFFull) | (cbp << 37)) | 0x400000000ull;
                t.w64(c + 0x5C88u + ((half << 3) + (cursor << 4)), s0);
                t.w16(c + 0x78A8u, static_cast<uint16_t>(t.u16(c + 0x78A8u) + 1u));
            }
            if (t.s32(s2) >= 0)
            {
                // relocate the holders' packet pointers (+4) once, from their section's start
                for (int list = 0; list < 2; ++list)
                {
                    const uint32_t countAt = s2 + (list ? 0x12u : 0x10u);
                    const uint32_t start = list ? s4start : s1start;
                    uint32_t holder = start;
                    if (t.u16(countAt) == 0u)
                        continue;
                    for (uint32_t j = 0;;)
                    {
                        t.w32(holder + 4u, t.u32(holder + 4u) + start);
                        ++j;
                        holder += 0x10u;
                        if (!t.ok || j > static_cast<uint32_t>(kTextureLoopLimit))
                            return false;
                        if (!(j < t.u16(countAt)))
                            break;
                        t.env.checkpoint(32u);
                    }
                }
                t.w32(s2, t.u32(s2) | 0x80000000u);
            }
            uint32_t holder = s2 + t.u32(s2 + 8u);
            uint32_t clutHolder = s2 + t.u32(s2 + 0xCu);
            if (t.u16(s2 + 0x10u) != 0u)
                for (uint32_t j = 0;;)
                {
                    t.env.call(); // jal 0x150DC8
                    if (!txnPacketUp(t, w, holder, sx32(cur), 0, sx32(static_cast<uint32_t>(type))))
                        return false;
                    ++j;
                    txnBump8(t, c + 0x78AAu);
                    holder += 0x10u;
                    if (j > static_cast<uint32_t>(kTextureLoopLimit))
                        return false;
                    if (!(j < t.u16(s2 + 0x10u)))
                        break;
                    t.env.checkpoint(32u);
                }
            if (t.u16(s2 + 0x12u) != 0u)
            {
                uint32_t clut = pbv;
                for (uint32_t j = 0;;)
                {
                    t.env.call(); // jal 0x150DC8
                    if (!txnPacketUp(t, w, clutHolder, sx32(clut), 1, sx32(static_cast<uint32_t>(type))))
                        return false;
                    ++j;
                    clut += 4u;
                    txnBump8(t, c + 0x78ABu);
                    clutHolder += 0x10u;
                    if (j > static_cast<uint32_t>(kTextureLoopLimit))
                        return false;
                    if (!(j < t.u16(s2 + 0x12u)))
                        break;
                    t.env.checkpoint(32u);
                }
            }
            return t.ok;
        }

        // func_00151630: the file's class by its header (decomp 00151630.c).
        template <class R>
        int textureClass(R &t, uint32_t name)
        {
            if (t.s8(name) == 0x54)
            {
                const int8_t b3 = t.s8(name + 3u);
                if (b3 == 0x33)
                    return t.s8(name + 5u) == 6 ? 1 : 0;
                if (b3 == 0x32)
                    return 0;
                if (t.s8(name + 1u) == 0x4D)
                    return 3;
            }
            return 2;
        }
        template <class Env>
        bool txnBuild(Txn<Env> &t, int kind, uint32_t w, uint32_t name, uint32_t pa, uint32_t pb, uint64_t type)
        {
            switch (kind)
            {
            case 0: return txnTim2(t, w, name, pa, pb, type);
            case 1: return txnTim3(t, w, name, pa, pb, type);
            case 2: return txnPack(t, w, name, pa, pb, type);
            case 3: return txnTm(t, w, name, pa, pb, type);
            default: return t.ok;
            }
        }

        // func_00151398's miss with room: the entry {name, *pa, *pb, cursor} at `entry`, then the builder; v = the
        // cursor before (lhu).
        template <class Env>
        bool txnMiss(Txn<Env> &t, uint32_t w, uint64_t name, uint64_t type, uint32_t entry, int32_t &v)
        {
            const uint32_t pa = w + (type == 2u ? 0x3DC6Cu : type == 1u ? 0x3DC78u : 0x3DC60u);
            const uint32_t pb = pa + 4u;
            const uint32_t cursorAt = w + kTexCounters + 0x78A8u;
            v = t.u16(cursorAt);
            t.w32(entry, static_cast<uint32_t>(name));
            t.w32(entry + 4u, t.u32(pa));
            t.w32(entry + 8u, t.u32(pb));
            t.w32(entry + 0xCu, t.u16(cursorAt));
            t.env.call(); // jal 0x151630
            const int kind = textureClass(t, static_cast<uint32_t>(name));
            if (kind >= 0 && kind <= 3)
                t.env.call(); // jal the builder
            return t.ok && txnBuild(t, kind, w, static_cast<uint32_t>(name), pa, pb, type);
        }

        // The builders and the classification as natives of their own (reached from 0x151398's original when it runs).
        constexpr uint32_t kTextureClass = 0x151630u;
        template <class Env>
        bool textureClassNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            Reader<Env> rd{env, 0u, 0u};
            const int kind = textureClass(rd, e::u32(ctx, e::a0));
            if (!rd.ok)
                return false;
            e::setLow64(ctx, e::v0, static_cast<uint64_t>(static_cast<int64_t>(kind)));
            returnToCaller(ctx);
            return true;
        }
        constexpr uint32_t kBuilderWindow = 0x140u; // a builder's frame (<= 0xF0) + an upload's (0x10) + 0x31A650's
        template <class Env, int Kind>
        bool builderNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kBuilderWindow, kBuilderWindow, stackTop))
                return false;
            Txn<Env> t{env, stackTop, stackTop + kBuilderWindow};
            if (!t.ok || !txnBuild(t, Kind, e::u32(ctx, e::a0), e::u32(ctx, e::a1), e::u32(ctx, e::a2), e::u32(ctx, e::a3),
                                   e::u64(ctx, e::t0)))
                return false;
            if (!env.commitOk())
                return false;
            t.commit();
            returnToCaller(ctx);
            return true;
        }

        // ---- func_00151398: the texture cache's lookup (decomp src/cod/00151398.c) -------------------------------
        // int func_00151398(char *w, unsigned int name, int type): the texture `name` of this kind's table in the
        // manager w (type 2 from w + 0x3D2F0, type 1 from w + 0x3DC50, else from w + 0x3C990; 16-byte entries Ent
        // {name, b, c, v} walked down to a name of -1): a hit returns its VRAM block (e->v). A miss inserts the entry
        // and builds the upload (func_00151990 / 151CC0 / 151688 / 151FA0, texture_slots' hook inside 0x151990), or
        // returns 0 on a full table: this native takes the hits and the full table (nothing written: v0 = 0)
        // and declines a miss with room (the original runs). The cache lives one frame (func_00150AA8 sets every name
        // to -1 and the VRAM cursor to 0), so a texture's first lookup in a frame is always a miss.
        // The code compares the whole registers: type and name as 64-bit values (an entry's name sign-extended by its
        // lw), the sentinel -1 the same way.
        constexpr uint32_t kTextureLookup = 0x151398u;
        constexpr uint32_t kTextureLookupFrame = 0x70u;
        constexpr int kTextureLookupLimit = 256; // entries walked before declining (each table holds 0x95)

        // A hit: v = the entry's VRAM block. A full table (the walk ends on the kind's last entry, still the -1 end:
        // no room to insert): v = 0, nothing written. backEdges = the loop's backward branches taken, one cycle charge
        // each (bne at 0x151414 / 0x151484 / 0x1514E4; the default kind's full return is a backward beq at 0x1514F4 too,
        // types 2 and 1 branch forward). False on a miss with room (the original inserts and builds the upload) or an
        // unreadable / endless table.
        template <class R>
        bool textureHit(R &rd, uint32_t w, uint64_t name, uint64_t type, int32_t &v, int &backEdges,
                        uint32_t *missAt = nullptr)
        {
            uint32_t entry = w + (type == 2u ? 0x3D2F0u : type == 1u ? 0x3DC50u : 0x3C990u);
            const uint32_t last = w + (type == 2u ? 0x3C9A0u : type == 1u ? 0x3D300u : 0x3C040u);
            auto nameAt = [&](uint32_t at) { return static_cast<uint64_t>(static_cast<int64_t>(rd.s32(at))); };
            constexpr uint64_t kEnd = ~0ull; // Ent.name 0xFFFFFFFF
            auto full = [&] {
                v = 0;
                if (type != 2u && type != 1u)
                    ++backEdges;
                return rd.ok;
            };
            backEdges = 0;
            uint64_t current = nameAt(entry);
            if (!rd.ok)
                return false;
            auto missed = [&] {
                if (missAt)
                    *missAt = entry;
                return false;
            };
            if (current == kEnd)
                return entry == last ? full() : missed(); // an empty table: a miss with room
            for (;; ++backEdges)
            {
                if (current == name)
                {
                    v = rd.s32(entry + 0xCu);
                    return rd.ok;
                }
                entry -= 0x10u;
                current = nameAt(entry);
                if (!rd.ok || backEdges + 1 >= kTextureLookupLimit)
                    return false;
                if (current == kEnd)
                    return entry == last ? full() : missed();
            }
        }

        // A miss with room: the walk again on a Txn, the entry and the builder (txnMiss); v0 = the cursor before.
        constexpr uint32_t kTextureMissWindow = kTextureLookupFrame + kBuilderWindow;
        template <class Env>
        bool textureLookupMiss(Env &env, R5900Context *ctx)
        {
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kTextureMissWindow, kTextureMissWindow, stackTop))
                return false;
            Txn<Env> t{env, stackTop, stackTop + kTextureMissWindow};
            const uint32_t w = e::u32(ctx, e::a0);
            const uint64_t name = e::u64(ctx, e::a1), type = e::u64(ctx, e::a2);
            int32_t v;
            int backEdges;
            uint32_t missAt = 0u;
            if (!t.ok || textureHit(t, w, name, type, v, backEdges, &missAt) || missAt == 0u)
                return false;
            for (int i = 0; i < backEdges; ++i)
                env.checkpoint(32u);
            if (!txnMiss(t, w, name, type, missAt, v))
                return false;
            if (!env.commitOk())
                return false;
            t.commit();
            e::setS32(ctx, e::v0, static_cast<uint32_t>(v));
            returnToCaller(ctx);
            return true;
        }

        template <class Env>
        bool textureLookup(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kTextureLookupFrame, kTextureLookupFrame, stackTop))
                return false;
            Reader<Env> rd{env, stackTop, stackTop + kTextureLookupFrame};
            int32_t v;
            int backEdges;
            uint32_t missAt = 0u;
            if (!textureHit(rd, e::u32(ctx, e::a0), e::u64(ctx, e::a1), e::u64(ctx, e::a2), v, backEdges, &missAt))
                return rd.ok && missAt != 0u && textureLookupMiss(env, ctx);
            for (int i = 0; i < backEdges; ++i)
                env.checkpoint(32u);
            e::setS32(ctx, e::v0, static_cast<uint32_t>(v));
            returnToCaller(ctx);
            return true;
        }

        // ---- func_00148BD8: a mesh node's texture registers (decomp src/cod/00148bd8.c) --------------------------
        // void func_00148BD8(cModelNode *node, int type, int name, int mode): the texture `name` of kind `type` from the
        // texture manager D_004664A0 (func_00151398: inline, its hit / full-table paths; a miss with room declines before anything is
        // written), then for each of the node's meshes (data->+0xA of them, data = node->data) whose entry byte +0x16
        // isn't 0xFF: its packet word (node + 0x300, 16 per frame half) + 0x20 gets the manager's 64-bit register for
        // that texture (func_00151368: the table at D_004664A0 + 0x3DC88, by half and texture) with the TBP field
        // (bits 37-50) moved by the node's signed block offset (+0x387) and bits 35-36 = mode & 3.
        constexpr uint32_t kMeshTextures = 0x148BD8u;
        constexpr uint32_t kMeshTexturesFrame = 0xB0u + kTextureLookupFrame; // its frame + 0x151398's (0x151368: none)
        constexpr uint32_t kTextureManager = 0x4664A0u;                     // D_004664A0
        constexpr int kMeshTexturesLimit = 128;                             // meshes (more declines)

        // func_00148BD8's body on a caller's Txn (rung 3 step a: its callers, cModel_setNodeMaterial and
        // ForwardAttackByMode, run it inline): the lookup (a hit, or a miss with room: insert + builder, which writes
        // the register rows the meshes then read), then the meshes, in the original's order. type / name / a3 as the
        // registers hold them. False = decline (nothing reaches RAM: the caller's Txn isn't committed).
        template <class Env>
        bool txnMeshTextures(Txn<Env> &t, uint32_t node, uint64_t type, uint64_t name, uint64_t a3)
        {
            Env &env = t.env;
            const uint32_t data = t.u32(node + 0x388u);
            const int n = t.u16(data + 0xAu);
            int32_t texture;
            int lookupBackEdges;
            uint32_t missAt = 0u;
            if (!t.ok || n > kMeshTexturesLimit)
                return false;
            const bool hit = textureHit(t, kTextureManager, name, type, texture, lookupBackEdges, &missAt);
            if (!hit && (!t.ok || missAt == 0u))
                return false;
            env.call(); // jal 0x151398
            for (int i = 0; i < lookupBackEdges; ++i)
                env.checkpoint(32u);
            if (!hit && !txnMiss(t, kTextureManager, name, type, missAt, texture))
                return false;
            const uint32_t half = t.u32(model::kFrameParity) & 1u;
            const uint64_t mode = (a3 & 3u) << 35;
            for (int i = 0; i < n && t.ok; ++i)
            {
                if (i != 0)
                    env.checkpoint(32u);
                const uint32_t entry = t.u32(data + 0x20u + 4u * static_cast<uint32_t>(i)) + data;
                if (t.u8(entry + 0x16u) == 0xFFu)
                    continue;
                const uint32_t packet = t.u32(node + 0x300u + (half << 6) + 4u * static_cast<uint32_t>(i));
                env.call(); // jal 0x151368: the frame half read again, the texture's row
                const uint32_t row = kTextureManager + 0x3DC88u + ((t.u32(model::kFrameParity) & 1u) << 3) +
                                     (static_cast<uint32_t>(texture) << 4);
                const uint64_t reg = t.u64(row);
                const uint64_t block = ((reg >> 37) & 0x3FFFu) + static_cast<uint64_t>(static_cast<int64_t>(t.s8(node + 0x387u)));
                t.w64(packet + 0x20u, (((reg & 0xFFF8001FFFFFFFFFull) | ((block & 0x3FFFu) << 37)) & 0xFFFFFFE7FFFFFFFFull) | mode);
            }
            return t.ok;
        }

        // The thin wrapper: func_00148BD8 on its own Txn (the plan below takes this path for the texture's first lookup
        // in the frame).
        constexpr uint32_t kMeshTexturesMissWindow = kMeshTexturesFrame + kBuilderWindow;
        template <class Env>
        bool meshTexturesTxn(Env &env, R5900Context *ctx)
        {
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kMeshTexturesMissWindow, kMeshTexturesMissWindow, stackTop))
                return false;
            Txn<Env> t{env, stackTop, stackTop + kMeshTexturesMissWindow};
            if (!t.ok ||
                !txnMeshTextures(t, e::u32(ctx, e::a0), e::u64(ctx, e::a1), e::u64(ctx, e::a2), e::u64(ctx, e::a3)))
                return false;
            if (!env.commitOk())
                return false;
            t.commit();
            returnToCaller(ctx);
            return true;
        }

        template <class Env>
        bool meshTextures(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kMeshTexturesFrame, kMeshTexturesFrame, stackTop))
                return false;
            Footprint::Range reads[kMeshTexturesLimit * 5 + 16];
            Reader<Env> rd{env, stackTop, stackTop + kMeshTexturesFrame};
            rd.log = reads;
            rd.logCap = static_cast<int>(sizeof(reads) / sizeof(reads[0]));
            const uint32_t node = e::u32(ctx, e::a0);
            // the plan, in the original's order
            const uint32_t data = rd.u32(node + 0x388u);
            const int n = rd.u16(data + 0xAu);
            int32_t texture;
            int lookupBackEdges;
            uint32_t missAt = 0u;
            if (!rd.ok || n > kMeshTexturesLimit)
                return false;
            if (!textureHit(rd, kTextureManager, e::u64(ctx, e::a2), e::u64(ctx, e::a1), texture, lookupBackEdges, &missAt))
                return rd.ok && missAt != 0u && meshTexturesTxn(env, ctx);
            const uint32_t half = rd.u32(model::kFrameParity) & 1u;
            const uint64_t mode = (e::u64(ctx, e::a3) & 3u) << 35;
            struct Write
            {
                uint32_t offset;
                uint64_t value;
            } writes[kMeshTexturesLimit];
            int count = 0;
            for (int i = 0; i < n; ++i)
            {
                const uint32_t entry = rd.u32(data + 0x20u + 4u * static_cast<uint32_t>(i)) + data;
                if (rd.u8(entry + 0x16u) == 0xFFu)
                    continue;
                const uint32_t packet = rd.u32(node + 0x300u + (half << 6) + 4u * static_cast<uint32_t>(i));
                // func_00151368(D_004664A0, texture): the frame half read again, the texture's 16-byte row
                const uint32_t row = kTextureManager + 0x3DC88u + ((rd.u32(model::kFrameParity) & 1u) << 3) +
                                     (static_cast<uint32_t>(texture) << 4);
                const uint64_t reg = rd.u64(row);
                const uint64_t block = ((reg >> 37) & 0x3FFFu) + static_cast<uint64_t>(static_cast<int64_t>(rd.s8(node + 0x387u)));
                uint32_t at;
                if (!plainRam(packet + 0x20u, 8u, at) || e::overlaps(at, 8u, stackTop, kMeshTexturesFrame))
                    return false;
                writes[count++] = Write{at, ((((reg & 0xFFF8001FFFFFFFFFull) | ((block & 0x3FFFu) << 37)) &
                                              0xFFFFFFE7FFFFFFFFull) | mode)};
            }
            if (!rd.ok)
                return false;
            // no write may land on anything read (the original reads some of it after earlier writes)
            Write sorted[kMeshTexturesLimit];
            std::copy(writes, writes + count, sorted);
            std::sort(sorted, sorted + count, [](const Write &a, const Write &b) { return a.offset < b.offset; });
            for (int r = 0; r < rd.logCount; ++r)
            {
                const Footprint::Range &read = reads[r];
                const Write *it = std::lower_bound(sorted, sorted + count, read.address < 7u ? 0u : read.address - 7u,
                                                   [](const Write &w, uint32_t v) { return w.offset < v; });
                if (it != sorted + count && it->offset < read.address + read.size)
                    return false;
            }

            // the cycles: 0x151398's jal and loop, 0x151368's jal per mesh written, the loop's back-edges
            env.call();
            for (int i = 0; i < lookupBackEdges; ++i)
                env.checkpoint(32u);
            for (int i = 0; i < count; ++i)
                env.call();
            for (int i = 1; i < n; ++i)
                env.checkpoint(32u);
            for (int i = 0; i < count; ++i)
                e::write64(env, writes[i].offset, writes[i].value);
            returnToCaller(ctx);
            return true;
        }

        // ---- func_00148D30: a node's textures by mesh (unmatched at 75cb123; batches 3-4b) -------------------------
        // void func_00148D30(cModelNode *node, int kind): when the room has a pack (D_00747A9C, Sys + 0x5FC), for each
        // mesh (data->+0xA of them) whose entry byte +0x16 (t) isn't 0xFF: the texture's name from the pack (the node's
        // dispFlags 0x02000000: the t-th "T32" / "TM2" entry, SearchDataByTwoNames 0x31CDC0; else cIDBase_getSlotPtr
        // 0x1522C0: a TM pack's t-th picture, any other file itself), its VRAM block from the cache (func_00151398 with
        // `kind`: a miss inserts and builds), its register row (func_00151368) into the mesh's packet word + 0x20. All on
        // a Txn: a miss's builder writes what later meshes read.
        constexpr uint32_t kNodeTextures = 0x148D30u;
        constexpr uint32_t kRoomPack = 0x747A9Cu;          // D_00747A9C (Sys + 0x5FC)
        constexpr uint32_t kNameT32 = 0x41EA50u, kNameTM2 = 0x41EA58u; // "T32", "TM2"
        constexpr uint32_t kNodeTexturesWindow = 0xB0u + kTextureLookupFrame + kBuilderWindow;
        constexpr uint32_t kPackSearchLimit = 0x10000u;    // pack entries walked before declining

        // SearchDataByTwoNames(pack, nameA, nameB, nth) (decomp 0031cdc0.c): the nth entry named *nameA or *nameB
        // (*nameA read once, *nameB per entry not matching it), its data (pack + offset); 0 if none. A found entry
        // leaves by a backward branch (one loop charge), each entry after the first by the loop's.
        template <class Env>
        bool txnSearchTwo(Txn<Env> &t, uint64_t pack64, uint32_t nameA, uint32_t nameB, uint64_t nth, uint64_t &v)
        {
            v = 0u;
            if (pack64 == 0u)
                return t.ok;
            const uint32_t pack = static_cast<uint32_t>(pack64);
            const uint32_t count = t.u32(pack);
            if (count == 0u)
                return t.ok;
            if (sx32(count) > kPackSearchLimit)
                return t.ok = false;
            const uint32_t a = t.u32(nameA);
            uint32_t at = pack + (count << 2);
            uint64_t found = 0u;
            for (uint64_t i = 1u;;)
            {
                at += 4u;
                const uint32_t name = t.u32(at);
                if (!t.ok)
                    return false;
                if (name == a || name == t.u32(nameB))
                {
                    if (found == nth)
                    {
                        t.env.checkpoint(32u); // beq at 0x31CE18 (backward)
                        v = sx32(t.u32(pack + (static_cast<uint32_t>(i) << 2)) + pack);
                        return t.ok;
                    }
                    ++found;
                }
                ++i;
                if (sx32(count) < i)
                    return t.ok;
                t.env.checkpoint(32u); // beqz at 0x31CE28
            }
        }

        // func_00148D30's body on a caller's Txn (ForwardAttackByMode runs it inline). False = decline.
        template <class Env>
        bool txnNodeTextures(Txn<Env> &t, uint32_t node, uint64_t kind)
        {
            Env &env = t.env;
            if (t.u32(kRoomPack) != 0u)
            {
                const bool named = (t.u32(node + model::kNodeDispFlags) & 0x02000000u) != 0u;
                const uint32_t data = t.u32(node + 0x388u);
                const uint32_t half = t.u32(model::kFrameParity) & 1u;
                const uint32_t n = t.u16(data + 0xAu);
                if (n > static_cast<uint32_t>(kMeshTexturesLimit) * 32u)
                    return false;
                for (uint32_t i = 0; i < n && t.ok; ++i)
                {
                    if (i != 0u)
                        env.checkpoint(32u); // bnez at 0x148E24 / 0x148EC4
                    const uint32_t entry = t.u32(data + 0x20u + 4u * i) + data;
                    const uint32_t slot = t.u8(entry + 0x16u);
                    if (slot == 0xFFu)
                        continue;
                    const uint64_t pack = sx32(t.u32(kRoomPack));
                    uint64_t name;
                    env.call(); // jal 0x31CDC0 / 0x1522C0
                    if (named)
                    {
                        if (!txnSearchTwo(t, pack, kNameT32, kNameTM2, slot, name))
                            return false;
                    }
                    else
                    {
                        env.call(); // its jal 0x151630
                        const uint32_t obj = static_cast<uint32_t>(pack);
                        name = textureClass(t, obj) == 3 ? sx32(t.u32(obj + (slot << 2) + 0x10u) + obj) : pack;
                    }
                    int32_t texture;
                    int backEdges;
                    uint32_t missAt = 0u;
                    env.call(); // jal 0x151398
                    if (!textureHit(t, kTextureManager, name, kind, texture, backEdges, &missAt))
                    {
                        if (!t.ok || missAt == 0u)
                            return false;
                        for (int k = 0; k < backEdges; ++k)
                            env.checkpoint(32u);
                        if (!txnMiss(t, kTextureManager, name, kind, missAt, texture))
                            return false;
                    }
                    else
                        for (int k = 0; k < backEdges; ++k)
                            env.checkpoint(32u);
                    const uint32_t packet = t.u32(node + 0x300u + (half << 6) + 4u * i);
                    env.call(); // jal 0x151368
                    const uint32_t row = kTextureManager + 0x3DC88u + ((t.u32(model::kFrameParity) & 1u) << 3) +
                                         (static_cast<uint32_t>(texture) << 4);
                    t.w64(packet + 0x20u, t.u64(row));
                }
            }
            return t.ok;
        }

        // The thin wrapper: func_00148D30 on its own Txn.
        template <class Env>
        bool nodeTextures(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kNodeTexturesWindow, kNodeTexturesWindow, stackTop))
                return false;
            Txn<Env> t{env, stackTop, stackTop + kNodeTexturesWindow};
            if (!t.ok || !txnNodeTextures(t, e::u32(ctx, e::a0), e::u64(ctx, e::a1)))
                return false;
            if (!env.commitOk())
                return false;
            t.commit();
            returnToCaller(ctx);
            return true;
        }

        // func_00151398 inline on a Txn (the jal, the walk's loop charges; a miss inserts and builds): v = the block.
        template <class Env>
        bool txnLookup(Txn<Env> &t, uint64_t name, uint64_t kind, int32_t &v)
        {
            int backEdges;
            uint32_t missAt = 0u;
            t.env.call(); // jal 0x151398
            const bool hit = textureHit(t, kTextureManager, name, kind, v, backEdges, &missAt);
            if (!hit && (!t.ok || missAt == 0u))
                return false;
            for (int k = 0; k < backEdges; ++k)
                t.env.checkpoint(32u);
            return hit || txnMiss(t, kTextureManager, name, kind, missAt, v);
        }

        // ---- func_00148690: a node's material textures (unmatched at 75cb123; batches 3-4b) ------------------------
        // void func_00148690(cModelNode *node, int kind, u8 *remap, int mode): per mesh, its texture index t (entry byte
        // +0x16; through `remap` when given and its byte isn't 0xFF, unless the node's +0x410 has 0x40000000; 0 when
        // dispFlags has 4; 0xFF skips the mesh), the texture by dispFlags: 0x04000000 or none of 0x0A000000: one lookup of
        // the node's name (+0x400) for the node, the mesh's block = that + t; 0x08000000: cIDBase_getSlotPtr(manager,
        // +0x400, t) per mesh; 0x02000000: SearchDataByTwoNames(+0x400, "T32", "TM2", t) per mesh. Then the row
        // (func_00151368) into the mesh's packet word + 0x20, written again with its CBP moved by the node's block offset
        // (+0x387) and bits 35-36 = mode & 3 (as func_00148BD8). On a Txn.
        constexpr uint32_t kNodeMaterial = 0x148690u;
        constexpr uint32_t kNodeMaterialWindow = 0xC0u + kTextureLookupFrame + kBuilderWindow;

        // func_00148690's body on a caller's Txn (cModel_setNodeMaterial runs it inline): kind / remap / a3 as the
        // registers hold them. False = decline.
        template <class Env>
        bool txnNodeMaterial(Txn<Env> &t, uint32_t node, uint64_t kind, uint64_t remap, uint64_t a3)
        {
            Env &env = t.env;
            const uint64_t mode = (a3 & 3u) << 35;
            const uint32_t data = t.u32(node + 0x388u);
            const uint32_t flags = t.u32(node + model::kNodeDispFlags);
            const uint32_t n = t.u16(data + 0xAu);
            // 0: one lookup for the node; 1: cIDBase_getSlotPtr per mesh; 2: SearchDataByTwoNames per mesh
            const int how = (flags & 0x04000000u) ? 0 : (flags & 0x08000000u) ? 1 : (flags & 0x02000000u) ? 2 : 0;
            int32_t base = 0;
            if (how == 0 && !txnLookup(t, sx32(t.u32(node + 0x400u)), kind, base))
                return false;
            const uint32_t half = t.u32(model::kFrameParity) & 1u;
            if (n > static_cast<uint32_t>(kMeshTexturesLimit) * 32u)
                return false;
            for (uint32_t i = 0; i < n && t.ok; ++i)
            {
                if (i != 0u)
                    env.checkpoint(32u); // bnez at 0x1487F8 / 0x148938 / 0x148A68 / 0x148BA0
                const uint32_t entry = t.u32(data + 0x20u + 4u * i) + data;
                uint32_t slot = t.u8(entry + 0x16u);
                if (remap != 0u)
                {
                    const uint32_t at = static_cast<uint32_t>(remap) + slot;
                    if (t.u8(at) != 0xFFu && (t.u32(node + 0x410u) & 0x40000000u) == 0u)
                        slot = t.u8(at);
                }
                if (t.u32(node + model::kNodeDispFlags) & 4u)
                    slot = 0u;
                if (slot == 0xFFu)
                    continue;
                const uint32_t word = node + 0x300u + (half << 6) + 4u * i;
                uint32_t packet;
                int32_t texture;
                if (how == 0)
                {
                    packet = t.u32(word);
                    texture = static_cast<int32_t>(static_cast<uint32_t>(base) + slot);
                }
                else
                {
                    const uint64_t obj = sx32(t.u32(node + 0x400u));
                    packet = t.u32(word);
                    uint64_t name;
                    env.call(); // jal 0x1522C0 / 0x31CDC0
                    if (how == 1)
                    {
                        env.call(); // its jal 0x151630
                        const uint32_t o = static_cast<uint32_t>(obj);
                        name = textureClass(t, o) == 3 ? sx32(t.u32(o + (slot << 2) + 0x10u) + o) : obj;
                    }
                    else if (!txnSearchTwo(t, obj, kNameT32, kNameTM2, slot, name))
                        return false;
                    if (!txnLookup(t, name, kind, texture))
                        return false;
                }
                env.call(); // jal 0x151368
                const uint32_t row = kTextureManager + 0x3DC88u + ((t.u32(model::kFrameParity) & 1u) << 3) +
                                     (static_cast<uint32_t>(texture) << 4);
                const uint64_t reg = t.u64(row);
                t.w64(packet + 0x20u, reg);
                const uint64_t block = (((reg >> 37) & 0x3FFFu) + static_cast<uint64_t>(static_cast<int64_t>(t.s8(node + 0x387u)))) & 0x3FFFu;
                t.w64(packet + 0x20u, (((reg & 0xFFF8001FFFFFFFFFull) | (block << 37)) & 0xFFFFFFE7FFFFFFFFull) | mode);
            }
            return t.ok;
        }

        // The thin wrapper: func_00148690 on its own Txn.
        template <class Env>
        bool nodeMaterial(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kNodeMaterialWindow, kNodeMaterialWindow, stackTop))
                return false;
            Txn<Env> t{env, stackTop, stackTop + kNodeMaterialWindow};
            if (!t.ok || !txnNodeMaterial(t, e::u32(ctx, e::a0), e::u64(ctx, e::a1), e::u64(ctx, e::a2), e::u64(ctx, e::a3)))
                return false;
            if (!env.commitOk())
                return false;
            t.commit();
            returnToCaller(ctx);
            return true;
        }

        // ---- cModel_setNodeMaterial 0x14B4D0 (decomp src/cod/0014b4d0.c) and ForwardAttackByMode 0x14B5D8 (decomp
        // src/cod/00137ae8.c): rung 3 step a, Txn compositions --------------------------------------------------------
        // void cModel_setNodeMaterial(cModel *self, cModelNode *node): mode = 3 when the node blends with the model's
        // alpha (dispFlags CMODEL_NODE_ALPHA) and cModel_alpha(self) == 1.0f, else 0; then the node's special pass
        // (CMODEL_NODE_SPECIAL and D_00747AA8 set): func_00148BD8(node, self->drawPrio, D_00747AA8, mode), else
        // func_00148690(node, self->drawPrio, self->texSlot when texFlags has CMODEL_TEX_EXCHANGE else 0, mode).
        // int ForwardAttackByMode_14B5D8(cModel *self, cModelNode *node): node->info->flags CMODEL_MESH_SPECIAL and
        // D_00747AA8 set: func_00148BD8(node, self->drawPrio, D_00747AA8, 0), else func_00148D30(node, self->drawPrio).
        // Both run their callee's body inline on one Txn (the callee's own natives' bodies), one jal charged for it.
        // Their callers (cModel_Tag_set_rate) ignore v0: void on the relaxed bar.
        constexpr uint32_t kSetNodeMaterial = 0x14B4D0u;
        constexpr uint32_t kForwardAttack = 0x14B5D8u;
        constexpr uint32_t kSpecialPass = 0x747AA8u; // D_00747AA8: the special pass's texture name
        constexpr uint32_t kModelTint = 0x24Cu;      // cModel.tint.w (the tint's alpha)
        constexpr uint32_t kModelDrawPrio = 0x2C4u, kModelTexFlags = 0x254u, kModelTexSlot = 0x2D4u;
        constexpr uint32_t kNodeAlpha = 0x20000000u, kNodeSpecial = 0x40000000u; // CMODEL_NODE_ALPHA / _SPECIAL
        constexpr uint32_t kTexExchange = 0x10000000u;                             // CMODEL_TEX_EXCHANGE
        constexpr uint32_t kMeshSpecial = 0x200000u;                               // CMODEL_MESH_SPECIAL
        // their frame (0x10) + the deepest callee's window
        constexpr uint32_t kNodeCallerWindow =
            0x10u + std::max(std::max(kMeshTexturesMissWindow, kNodeMaterialWindow), kNodeTexturesWindow);

        // cModel_alpha(self) == 1.0f (decomp cModel.h, inline at 0x14B4F4): tint.w * (alphaA / 255) * (alphaB / 255) *
        // (alphaC / 255) in the code's order (cvt.s.w of the bytes, the three div.s, then the three mul.s), c.eq.s with
        // 1.0. The host matches the runtime's default float rules there when |tint.w| < 2^126 (the quotients are in
        // [0, 1], the divisor normal, no product past FLT_MAX - 1 ulp); else exact = false (decline), as
        // cModel_nodeLayerScreen does for the same maths.
        template <class R>
        bool modelAlphaOne(R &t, uint32_t self, bool &exact)
        {
            const uint32_t a = t.u8(self + 0x2B6u), b = t.u8(self + 0x2B7u), c = t.u8(self + 0x2BCu);
            const uint32_t w = t.u32(self + kModelTint);
            exact = (w & 0x7FFFFFFFu) < 0x7E800000u;
            if (!exact)
                return false;
            const float k = 255.0f;
            const float qa = divScalar(fpuFromSmallWord(static_cast<int32_t>(a)), k);
            const float qb = divScalar(fpuFromSmallWord(static_cast<int32_t>(b)), k);
            const float qc = divScalar(fpuFromSmallWord(static_cast<int32_t>(c)), k);
            float p = mulScalar(bitsFloat(w), qa);
            p = mulScalar(p, qb);
            p = mulScalar(p, qc);
            return e::fpuKey(floatBits(p)) == e::fpuKey(0x3F800000u);
        }

        template <class Env>
        bool setNodeMaterial(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kNodeCallerWindow, kNodeCallerWindow, stackTop))
                return false;
            Txn<Env> t{env, stackTop, stackTop + kNodeCallerWindow};
            const uint32_t self = e::u32(ctx, e::a0), node = e::u32(ctx, e::a1);
            uint64_t mode = 0u;
            if (t.u32(node + model::kNodeDispFlags) & kNodeAlpha)
            {
                bool exact;
                const bool one = modelAlphaOne(t, self, exact);
                if (!exact)
                    return false;
                if (one)
                    mode = 3u;
            }
            bool done;
            if ((t.u32(node + model::kNodeDispFlags) & kNodeSpecial) != 0u && t.u32(kSpecialPass) != 0u)
            {
                const uint64_t pass = sx32(t.u32(kSpecialPass));
                const uint64_t prio = sx32(t.u32(self + kModelDrawPrio));
                env.call(); // jal 0x148BD8
                done = txnMeshTextures(t, node, prio, pass, mode);
            }
            else
            {
                const uint64_t remap = (t.u32(self + kModelTexFlags) & kTexExchange) ? sx32(self + kModelTexSlot) : 0u;
                const uint64_t prio = sx32(t.u32(self + kModelDrawPrio));
                env.call(); // jal 0x148690
                done = txnNodeMaterial(t, node, prio, remap, mode);
            }
            if (!done || !t.ok)
                return false;
            if (!env.commitOk())
                return false;
            t.commit();
            returnToCaller(ctx);
            return true;
        }

        template <class Env>
        bool forwardAttack(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kNodeCallerWindow, kNodeCallerWindow, stackTop))
                return false;
            Txn<Env> t{env, stackTop, stackTop + kNodeCallerWindow};
            const uint32_t self = e::u32(ctx, e::a0), node = e::u32(ctx, e::a1);
            bool done;
            const uint32_t info = t.u32(node + 0x414u);
            const uint32_t pass = (t.u32(info + 0x34u) & kMeshSpecial) ? t.u32(kSpecialPass) : 0u;
            const uint64_t prio = sx32(t.u32(self + kModelDrawPrio));
            env.call(); // jal 0x148BD8 / 0x148D30
            if (pass != 0u)
                done = txnMeshTextures(t, node, prio, sx32(pass), 0u);
            else
                done = txnNodeTextures(t, node, prio);
            if (!done || !t.ok)
                return false;
            if (!env.commitOk())
                return false;
            t.commit();
            returnToCaller(ctx);
            return true;
        }

        // ---- func_0014B9A0: a node's UV-scroll packet (decomp src/cod/0014b9a0.c) --------------------------------
        // void func_0014B9A0(cModel *self, cModelNode *node, short id): the model's slot tags (func_00155BE8: kind 2,
        // or 0x14 for a CMODEL_F_LATE model), then a 0x120-byte packet from the frame allocator (PostInc; full: return):
        // a CNT tag, a VIF header + eight constant quads (D_003BD860..890; +0x3C = 255.0), an identity matrix whose last
        // row is (uvScroll u, v, 0, 1) (CopyMatrix then UnitMatrix then the row), the GS table's two tags (func_00155C68),
        // the node's own packet word (+0x90 of this half's packet), an end; queued like func_0014C5A0 with the node's
        // flags. vf0 must be (0, 0, 0, 1) (UnitMatrix builds from it).
        constexpr uint32_t kScrollPacket = 0x14B9A0u;
        constexpr uint32_t kScrollPacketFrame = 0xD0u + 0x10u; // its frame + 0x31A650's (the other callees: leaves)
        constexpr uint32_t kGsTagTable = 0x4A5D80u;            // D_004A5D80: {w0, w1} per GS table entry
        constexpr uint32_t kFrameAllocator = 0x566E10u;         // D_00566E10: the frame's bump pointer
        constexpr uint32_t kObjFlags = 0x250u;                  // cModel.objFlags
        constexpr uint32_t kLate = 0x2000u;                     // CMODEL_F_LATE

        template <class Env>
        uint64_t gsTag(Reader<Env> &rd, uint32_t index) // TAG64(D_004A5D80[index])
        {
            const uint32_t w0 = rd.u32(kGsTagTable + index * 8u), w1 = rd.u32(kGsTagTable + index * 8u + 4u);
            return static_cast<uint64_t>(w1 | 0x30000000u) | (static_cast<uint64_t>(w0 & 0x0FFFFFFFu) << 32);
        }

        template <class Env>
        bool scrollPacket(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            alignas(16) const uint32_t unitW[4] = {0u, 0u, 0u, 0x3F800000u};
            if (std::memcmp(&ctx->vu0_vf[0], unitW, 16) != 0)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kScrollPacketFrame, kScrollPacketFrame, stackTop))
                return false;
            Footprint::Range reads[96]; // ~70: the eight copies and CopyMatrix's source read by the word
            Reader<Env> rd{env, stackTop, stackTop + kScrollPacketFrame};
            rd.log = reads;
            rd.logCap = 96;
            const uint32_t self = e::u32(ctx, e::a0), node = e::u32(ctx, e::a1);
            const int64_t id = static_cast<int16_t>(e::u32(ctx, e::a2));

            // the plan, in the original's order
            const uint32_t half = rd.u32(model::kFrameParity) & 1u;
            const uint32_t kind = (rd.u32(self + kObjFlags) & kLate) ? 0x14u : 2u;
            // func_00155BE8(self, kind, half): the model's packet of this half, +0x50 / +0x60
            const uint32_t slots = rd.u32(self + model::kPacket + half * 4u);
            const uint64_t slotTag = gsTag(rd, kind);
            // PostInc(0x120): the half's 0x60000-byte area from D_00506E10
            rd.log = nullptr; // (the allocator word: read, then written by PostInc itself; any other read is logged)
            const uint32_t cur = rd.u32(kFrameAllocator);
            rd.log = reads;
            const uint32_t next = cur + 0x120u;
            const uint32_t limit = 0x506E10u + (rd.u32(model::kFrameParity) & 1u) * 0x60000u;
            const bool full = limit < next;
            const uint32_t pkt = full ? 0u : cur;
            uint32_t slotAt, allocAt;
            if (!rd.ok || !plainRam(slots + 0x50u, 0x18u, slotAt) || !plainRam(kFrameAllocator, 4u, allocAt))
                return false;
            alignas(16) uint8_t packet[0x120];
            uint32_t pktAt = 0;
            QueuePlan q{};
            if (pkt != 0u)
            {
                auto copy16 = [&](uint32_t to, uint32_t from) {
                    for (uint32_t k = 0; k < 16u; k += 4u)
                    {
                        const uint32_t word = rd.u32(from + k);
                        std::memcpy(packet + to + k, &word, 4);
                    }
                };
                auto put32 = [&](uint32_t to, uint32_t v) { std::memcpy(packet + to, &v, 4); };
                auto put64 = [&](uint32_t to, uint64_t v) { std::memcpy(packet + to, &v, 8); };
                std::memset(packet, 0, sizeof(packet));
                put64(0x00u, 0x1000000Du);
                put32(0x18u, 0x01000404u);
                put32(0x1Cu, 0x6C0C00C8u);
                copy16(0x20u, 0x3BD860u);
                copy16(0x30u, 0x3BD870u);
                copy16(0x40u, 0x3BD870u);
                copy16(0x50u, 0x3BD880u);
                copy16(0x60u, 0x3BD880u);
                copy16(0x70u, 0x3BD880u);
                copy16(0x80u, 0x3BD880u);
                copy16(0x90u, 0x3BD890u);
                put32(0x3Cu, 0x437F0000u); // 255.0f
                for (uint32_t k = 0; k < 64u; k += 4u) // CopyMatrix(m, D_004A6940) reads it (UnitMatrix overwrites m)
                    (void)rd.u32(0x4A6940u + k);
                put32(0xA0u, 0x3F800000u); // UnitMatrix(m): m = pkt + 0xA0
                put32(0xB4u, 0x3F800000u);
                put32(0xC8u, 0x3F800000u);
                put32(0xD0u, rd.u32(node + 0x38Cu)); // the last row: uvScroll u, v, 0, 1
                put32(0xD4u, rd.u32(node + 0x390u));
                put32(0xDCu, 0x3F800000u);
                put64(0xE0u, gsTag(rd, kind));       // func_00155C68(pkt + 0xE0, pkt + 0xF0, kind)
                put64(0xF0u, gsTag(rd, kind + 1u));
                put64(0x100u, rd.u64(node + half * 0xB0u + 0x90u));
                // the queue, as func_0014C5A0's: per-node models by this node's sorted flag
                const int32_t drawKind = rd.s32(self + model::kDrawKind);
                const uint32_t flags = rd.u32(node + model::kNodeDispFlags);
                int64_t bucket, index;
                if (drawKind == model::kKindPerNode)
                {
                    const bool sorted = (flags & model::kNodeSorted) != 0u;
                    bucket = sorted ? 2 : 1;
                    index = sorted ? id : 0xD;
                }
                else
                {
                    bucket = drawKind;
                    index = rd.s16(self + model::kId);
                }
                const uint32_t last = pkt + 0x110u;
                if (!rd.ok || !plainRam(pkt, 0x120u, pktAt) || last == 0u ||
                    !queuePlan(env, sp - 0xD0u, 0x754C80u, static_cast<uint64_t>(bucket), static_cast<uint64_t>(index),
                               pkt, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(last))), q))
                    return false;
                if (e::overlaps(pktAt, 0x120u, stackTop, kScrollPacketFrame))
                    return false;
                if (q.inRange && (e::overlaps(q.half, 4u, stackTop, kScrollPacketFrame) ||
                                  e::overlaps(q.at, 4u, stackTop, kScrollPacketFrame) ||
                                  e::overlaps(q.plan.desc0, 4u, stackTop, kScrollPacketFrame) ||
                                  e::overlaps(q.plan.desc20, 4u, stackTop, kScrollPacketFrame) ||
                                  e::overlaps(q.plan.slot, 8u, stackTop, kScrollPacketFrame) ||
                                  e::overlaps(q.plan.tail, 8u, stackTop, kScrollPacketFrame) ||
                                  e::overlaps(q.half, 4u, pktAt, 0x120u) || e::overlaps(q.at, 4u, pktAt, 0x120u) ||
                                  e::overlaps(q.plan.desc0, 4u, pktAt, 0x120u) ||
                                  e::overlaps(q.plan.desc20, 4u, pktAt, 0x120u)))
                    return false;
            }
            // no write (the slot tags, the allocator, the packet) onto anything read, nor onto each other or the frame
            const Footprint::Range writes[3] = {{slotAt, 0x18u}, {allocAt, 4u}, {pktAt, pkt ? 0x120u : 0u}};
            for (const Footprint::Range &w : writes)
            {
                if (w.size == 0u)
                    continue;
                if (e::overlaps(w.address, w.size, stackTop, kScrollPacketFrame))
                    return false;
                for (int r = 0; r < rd.logCount; ++r)
                    if (e::overlaps(w.address, w.size, reads[r].address, reads[r].size))
                        return false;
            }
            if (pkt && (e::overlaps(slotAt, 0x18u, pktAt, 0x120u) || e::overlaps(allocAt, 4u, pktAt, 0x120u)))
                return false;
            if (e::overlaps(slotAt, 0x18u, allocAt, 4u))
                return false;

            // the writes and the cycles, in the original's order
            env.call(); // func_00155BE8
            e::write64(env, slotAt, slotTag);
            e::write64(env, slotAt + 0x10u, 0x10000000u);
            env.call(); // PostInc
            if (!full)
                e::write32(env, allocAt, next);
            if (pkt != 0u)
            {
                for (int loop = 0; loop < 8 * 3; ++loop) // the eight 4-float copies' back-edges
                    env.checkpoint(32u);
                env.call(); // CopyMatrix
                env.call(); // UnitMatrix
                for (int loop = 0; loop < 3; ++loop) // the last row's copy
                    env.checkpoint(32u);
                std::memcpy(env.at(pktAt, 0x120u), packet, 0x120u);
                env.call(); // func_00155C68
                env.call(); // 0x31A650
                if (q.inRange)
                {
                    env.call(); // its 0x14FF00
                    linkPacket(env, q.plan, pkt);
                }
            }
            returnToCaller(ctx);
            return true;
        }

        // ---- func_00154240: a node's lighting block (rung 3 step a; unmatched in the decomp at baee8f4: our C++ from the
        // disassembly, its callees' bodies inline) -----------------------------------------------------------------------
        // void func_00154240(cModel *self, cModelNode *node), called by cModel_Tag_set_rate for every drawn node. Skips a
        // hidden node (dispFlags & 1). The block = node + 0x160 + 0xD0 * this frame's half. colour = (tint.xyz * the node's
        // +0x3A0..3A8, tint.w * node +0x3AC * cModel_alpha's three byte quotients). Light rows: dispFlags 0x800000 fixed
        // (block +0x40 = colour, +0x10..30 / 50..70 = D_003BD880, +0x80 = D_003BD890); node +0x410 0x20000000 the same with
        // +0x80 = MulVector(light set +0x30, colour); model +0x250 0x800000 func_002D8008 (+0x4C = 1.0, +0x80 = that
        // product); else func_002D7E50 (+0x10 = NormalLightMatrix of the set's three directions with row 3 = colour, +0x50
        // = the set's colours, row 3 = set +0x40, clamped 0..1). Then model 0x80000: +0x80 = D_003BD870; the env pass
        // (dispFlags 0x20000000 and colour.w == 1.0: +0xC0 = (node +0x38C, +0x390, 0, 1), +0x90 = the eye direction from
        // the model minus D_0061A900, both normalized through capVu0Length, +0xB0 = info +0x58, +0x8C scaled by info +0x59
        // and the set's +0x16210..18), or the eye itself (dispFlags 0x40000000 and D_00747AA8), or +0xC0 as above unless
        // 0x10000; model 0x2000: +0x90 = CopyMatrix(D_004A6940); then the packet's +0x30: 0 and +0x2C = 255 (dispFlags
        // 0x200000 or D_00747A2C & 0x1000), else func_002D8070(set, block +0x10, packet +0x30, id * 0.1).
        // On a Txn in the original's order of loads and stores (the block is read back: +0x8C, the 2D7E50 rows); the maths
        // is the recompiled ops' under the default float rules, declining any op the host can't do exactly there (a
        // result at or past FLT_MAX - 1 ulp, a divisor outside exponents 1-254). Void (the caller's next call overwrites
        // v0); f20 untouched (the original saves and restores it).
        constexpr uint32_t kNodeLighting = 0x154240u;
        constexpr uint32_t kLightSet = 0x604700u;      // the world light object (func_002D7E50 / 2D8008 / 2D8070's a0)
        constexpr uint32_t kViewPointer = 0x5CAFF0u;   // D_005CAFF0: the camera (+0x210 the eye)
        // its frame (0x120) + func_002D7E50's (0x50) + sceVu0NormalLightMatrix's (0x60): the deepest chain
        constexpr uint32_t kNodeLightingWindow = 0x120u + 0x50u + 0x60u;

        inline bool pastMaxBits(uint32_t bits) { return (bits & 0x7FFFFFFFu) > 0x7F7FFFFDu; }

        // The ops of one native under the default float rules, exact on the host where it does them; ok = false
        // otherwise (the native declines: the original takes ps2ee there).
        struct ExactOps
        {
            bool ok = true;
            float fpu(float r)
            {
                if (fpuPastMax(r))
                    ok = false;
                return r;
            }
            float mul(float a, float b) { return fpu(mulScalar(a, b)); }
            float add(float a, float b) { return fpu(addScalar(a, b)); }
            float sub(float a, float b) { return fpu(subScalar(a, b)); }
            // div.s / VDIV: the host's quotient for a divisor of exponent 1-254 and a dividend not 255 (ps2EeDiv)
            float div(float a, float b)
            {
                const uint32_t ea = (floatBits(a) >> 23) & 0xFFu, eb = (floatBits(b) >> 23) & 0xFFu;
                if (eb - 1u >= 254u || ea == 255u)
                    ok = false;
                return fpu(divScalar(a, b));
            }
            // a VU result: the lanes in Mask (bit 0 = x) checked
            template <int Mask>
            Vec4 vu(Vec4 r)
            {
                if (((Mask & 1) && pastMaxBits(laneBits<0>(r))) || ((Mask & 2) && pastMaxBits(laneBits<1>(r))) ||
                    ((Mask & 4) && pastMaxBits(laneBits<2>(r))) || ((Mask & 8) && pastMaxBits(laneBits<3>(r))))
                    ok = false;
                return r;
            }
            // the sum of squares (vmul.xyz, vaddy.x, vaddz.x) and VSQRT: |v.xyz| as capVu0Length / sceVu0Normalize get it
            float root(Vec4 v)
            {
                const Vec4 s = vu<7>(mul4(v, v));
                const float x = vuLane(addScalar(lane<0>(s), lane<1>(s)));
                const float sum = vuLane(addScalar(x, lane<2>(s)));
                uint16_t status = 0;
                return libvu0::vuSqrt(status, sum);
            }
            static Vec4 mul4(Vec4 a, Vec4 b) { return kamite::native::mul(a, b); }
            float vuLane(float r) { return fpu(r); }
        };

        // A constant multiplier as the game has it in a register: through a volatile, so the compiler can't fold x * 1.0
        // to x or x * -1.0 to -x (no flush of a denormal then).
        inline float registerConstant(float f)
        {
            volatile float v = f;
            return v;
        }

        // func_00154240's store: everything it writes lies in the node's block (0xD0 bytes, held here until commit) plus
        // one 8-byte word of the model's packet; reads see the block's writes byte by byte, a read of the packet word
        // after its write or a write anywhere else declines. Every access plain RAM and clear of the stack window, as
        // Txn's. (The general Txn's per-write hashing cost more than this function's own work.)
        template <class Env>
        struct BlockTxn
        {
            Env &env;
            uint32_t stackLow, stackHigh;
            bool ok = true;
            uint32_t block = 0;   // the block's RAM offset (valid once setBlock ran)
            bool hasBlock = false;
            uint32_t extraAt = 0; // the packet word
            uint64_t extra = 0;
            bool hasExtra = false;
            static constexpr uint32_t kBlock = 0xD0u;
            alignas(16) uint8_t bytes[kBlock]; // the block as it stands: RAM at setBlock, then our writes
            uint64_t written[2] = {0u, 0u};    // a bit per block halfword written (commit writes only those)
            BlockTxn(Env &e, uint32_t low, uint32_t high) : env(e), stackLow(low), stackHigh(high) {}
            KM_NATIVE_INLINE bool span(uint32_t address, uint32_t size, uint32_t &offset)
            {
                if (!ok)
                    return false;
                // plainRam's answer, quickly for RAM below the I/O range (KUSEG, KSEG0 / KSEG1)
                const uint32_t phys = address - 0x80000000u < 0x40000000u ? address & 0x1FFFFFFFu : address;
                if (phys < PS2_IO_BASE && phys + size <= PS2_IO_BASE)
                {
                    offset = address & PS2_RAM_MASK;
                    if (offset + size > PS2_RAM_MASK + 1u)
                        return ok = false;
                }
                else if (!plainRam(address, size, offset))
                    return ok = false;
                if (e::overlaps(offset, size, stackLow, stackHigh - stackLow))
                    return ok = false;
                return true;
            }
            // the block (its writes are halfwords and up, so it must be 2-aligned); nothing written yet
            void setBlock(uint32_t address)
            {
                hasBlock = (address & 1u) == 0u && span(address, kBlock, block);
                if (hasBlock)
                    std::memcpy(bytes, env.at(block, kBlock), kBlock);
                else
                    ok = false;
            }
            template <uint32_t N>
            KM_NATIVE_INLINE void read(uint32_t address, void *out)
            {
                uint32_t at;
                if (!span(address, N, at))
                {
                    std::memset(out, 0, N);
                    return;
                }
                const uint32_t b = at - block;
                if (hasBlock && b <= kBlock - N) // wholly in the block (unsigned: below it wraps high)
                {
                    std::memcpy(out, bytes + b, N);
                    return;
                }
                std::memcpy(out, env.at(at, N), N);
                if (hasExtra && e::overlaps(at, N, extraAt, 8u))
                    ok = false; // read after the packet word's write: never in practice
                else if (hasBlock && e::overlaps(at, N, block, kBlock))
                {
                    uint8_t *o = static_cast<uint8_t *>(out);
                    for (uint32_t i = 0; i < N; ++i)
                        if (at + i - block < kBlock)
                            o[i] = bytes[at + i - block];
                }
            }
            template <uint32_t N>
            KM_NATIVE_INLINE void write(uint32_t address, const void *in)
            {
                static_assert(N % 2u == 0u);
                uint32_t at;
                if (!span(address, N, at))
                    return;
                const uint32_t b = at - block;
                if (b > kBlock - N || (b & 1u))
                {
                    ok = false; // outside the block (the packet word goes through writeExtra)
                    return;
                }
                std::memcpy(bytes + b, in, N);
                constexpr uint64_t kRun = (1ull << (N / 2u)) - 1u; // N / 2 halfwords from h
                const uint32_t h = b / 2u;
                if (h < 64u)
                {
                    written[0] |= kRun << h;
                    if (h + N / 2u > 64u)
                        written[1] |= kRun >> (64u - h);
                }
                else
                    written[1] |= kRun << (h - 64u);
            }
            void writeExtra(uint32_t address, uint64_t v)
            {
                uint32_t at;
                if (!span(address, 8u, at))
                    return;
                if (hasExtra || e::overlaps(at, 8u, block, kBlock))
                {
                    ok = false;
                    return;
                }
                extraAt = at;
                extra = v;
                hasExtra = true;
            }
            template <class T>
            KM_NATIVE_INLINE T get(uint32_t address)
            {
                T v;
                read<sizeof(T)>(address, &v);
                return v;
            }
            uint8_t u8(uint32_t a) { return get<uint8_t>(a); }
            int16_t s16(uint32_t a) { return static_cast<int16_t>(get<uint16_t>(a)); }
            uint32_t u32(uint32_t a) { return get<uint32_t>(a); }
            uint64_t u64(uint32_t a) { return get<uint64_t>(a); }
            void w16(uint32_t a, uint16_t v) { write<2>(a, &v); }
            void w32(uint32_t a, uint32_t v) { write<4>(a, &v); }
            void w64(uint32_t a, uint64_t v) { write<8>(a, &v); }
            void lq(uint32_t a, uint8_t (&v)[16]) { read<16>(a & ~0xFu, v); }
            void sq(uint32_t a, const uint8_t (&v)[16]) { write<16>(a & ~0xFu, v); }
            void commit()
            {
                for (uint32_t w = 0; w < 2u; ++w) // runs of written halfwords (a run across the words: two copies)
                    for (uint64_t m = written[w]; m != 0u;)
                    {
                        const uint32_t lo = static_cast<uint32_t>(std::countr_zero(m));
                        const uint64_t rest = ~(m >> lo);
                        const uint32_t n = rest == 0u ? 64u - lo : static_cast<uint32_t>(std::countr_zero(rest));
                        const uint32_t at = 2u * (64u * w + lo);
                        std::memcpy(env.at(block + at, 2u * n), bytes + at, 2u * n);
                        m = n + lo >= 64u ? 0u : m & ~(((1ull << n) - 1u) << lo);
                    }
                if (hasExtra)
                    std::memcpy(env.at(extraAt, 8u), &extra, 8u);
            }
        };

        template <class T>
        Vec4 txnQuad(T &t, uint32_t address)
        {
            alignas(16) uint8_t q[16];
            t.lq(address, q);
            return load(q);
        }
        template <class T>
        void txnStoreQuad(T &t, uint32_t address, Vec4 v)
        {
            alignas(16) uint8_t q[16];
            store(q, v);
            t.sq(address, q);
        }
        template <class T>
        float txnFloat(T &t, uint32_t address) { return bitsFloat(t.u32(address)); }
        // The 4-word copy loops (lwc1 / swc1, counter 3 down to -1: three backward branches), from guest memory
        template <class T>
        void txnCopy4(T &t, uint32_t src, uint32_t dst)
        {
            const uint32_t gap = (src - dst) & PS2_RAM_MASK, back = (dst - src) & PS2_RAM_MASK;
            if (gap == 0u || (gap >= 16u && back >= 16u)) // no partial overlap: the word order can't show
            {
                uint8_t v[16];
                t.template read<16>(src, v);
                t.template write<16>(dst, v);
            }
            else
                for (uint32_t i = 0; i < 16u; i += 4u)
                    t.w32(dst + i, t.u32(src + i));
            for (int k = 0; k < 3; ++k)
                t.env.checkpoint(32u);
        }
        // ... and from a value the original keeps in its frame
        template <class T>
        void txnPut4(T &t, Vec4 v, uint32_t dst)
        {
            uint8_t q[16];
            store(q, v);
            t.template write<16>(dst, q);
            for (int k = 0; k < 3; ++k)
                t.env.checkpoint(32u);
        }

        // capVu0Length through its thunk 0x101E28 (two guest calls), then the caller's normalisation: 0 < len (the compare
        // key): (v.xyz * (1 / len), v.w) by mul.s, else vf0 (0, 0, 0, 1).
        template <class T>
        Vec4 eyeNormalize(T &t, ExactOps &x, Vec4 v)
        {
            t.env.call(); // jal 0x101E28
            t.env.call(); // its jal capVu0Length
            const float len = x.root(v); // vaddq.x vf4, vf0, Q: 0 + Q
            if (!(e::fpuKey(0u) < e::fpuKey(floatBits(len))))
                return bitsOf(0u, 0u, 0u, 0x3F800000u);
            const float inv = x.div(registerConstant(1.0f), len);
            return bitsOf(floatBits(x.mul(lane<0>(v), inv)), floatBits(x.mul(lane<1>(v), inv)),
                          floatBits(x.mul(lane<2>(v), inv)), laneBits<3>(v));
        }

        template <class Env>
        bool nodeLighting(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd || !e::vf0Constant(ctx))
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackTop;
            if (!plainRam(sp - kNodeLightingWindow, kNodeLightingWindow, stackTop))
                return false;
            BlockTxn<Env> t{env, stackTop, stackTop + kNodeLightingWindow};
            ExactOps x;
            const uint32_t self = e::u32(ctx, e::a0), node = e::u32(ctx, e::a1);
            const uint32_t flags = t.u32(node + 0x380u);
            const uint32_t half = t.u32(model::kFrameParity) & 1u;
            if (!t.ok)
                return false;
            if (flags & 1u)
            {
                returnToCaller(ctx);
                return true;
            }
            const uint32_t block = node + 0x160u + half * 0xD0u;
            t.setBlock(block);

            // the colour (its frame's sp+0..0xC)
            const float k255 = 255.0f;
            const float qa = x.div(fpuFromSmallWord(t.u8(self + 0x2B6u)), k255);
            float w = x.mul(txnFloat(t, self + kModelTint), txnFloat(t, node + 0x3ACu));
            const float qb = x.div(fpuFromSmallWord(t.u8(self + 0x2B7u)), k255);
            const float qc = x.div(fpuFromSmallWord(t.u8(self + 0x2BCu)), k255);
            const float cx = x.mul(txnFloat(t, self + 0x240u), txnFloat(t, node + 0x3A0u));
            w = x.mul(w, qa);
            const float cy = x.mul(txnFloat(t, self + 0x244u), txnFloat(t, node + 0x3A4u));
            w = x.mul(w, qb);
            const float cz = x.mul(txnFloat(t, self + 0x248u), txnFloat(t, node + 0x3A8u));
            w = x.mul(w, qc);
            const Vec4 colour = bitsOf(floatBits(cx), floatBits(cy), floatBits(cz), floatBits(w));
            constexpr uint32_t kFixedRow = 0x3BD880u, kFixedLast = 0x3BD890u, kFixedAlt = 0x3BD870u;

            if (flags & 0x800000u)
            {
                txnPut4(t, colour, block + 0x40u);
                txnCopy4(t, kFixedRow, block + 0x50u);
                txnCopy4(t, kFixedRow, block + 0x60u);
                txnCopy4(t, kFixedRow, block + 0x70u);
                txnCopy4(t, kFixedLast, block + 0x80u);
                txnCopy4(t, kFixedRow, block + 0x10u);
                txnCopy4(t, kFixedRow, block + 0x20u);
                txnCopy4(t, kFixedRow, block + 0x30u);
            }
            else if (t.u32(node + 0x410u) & 0x20000000u)
            {
                txnPut4(t, colour, block + 0x40u);
                txnCopy4(t, kFixedRow, block + 0x50u);
                txnCopy4(t, kFixedRow, block + 0x60u);
                txnCopy4(t, kFixedRow, block + 0x70u);
                env.call(); // sceVu0MulVector(block +0x80, set +0x30, colour)
                txnStoreQuad(t, block + 0x80u, x.vu<15>(mul(txnQuad(t, kLightSet + 0x30u), colour)));
                txnCopy4(t, kFixedRow, block + 0x10u);
                txnCopy4(t, kFixedRow, block + 0x20u);
                txnCopy4(t, kFixedRow, block + 0x30u);
            }
            else if (t.u32(self + 0x250u) & 0x800000u)
            {
                (void)txnQuad(t, t.u32(self + 0xF0u)); // copied to its frame (func_002D8008 doesn't read it)
                env.call(); // func_002D8008
                t.w32(block + 0x4Cu, 0x3F800000u);
                env.call(); // its sceVu0MulVector into its frame
                const Vec4 p = x.vu<15>(mul(txnQuad(t, kLightSet + 0x30u), colour));
                txnPut4(t, p, block + 0x80u);
            }
            else
            {
                (void)t.u32(self + 0xF0u); // t0, t2: loaded, unused by func_002D7E50
                (void)t.u8(self + 0x2CCu);
                env.call(); // func_002D7E50(set, model, block +0x10, block +0x50, ..., colour)
                env.call(); // its sceVu0NormalLightMatrix(block +0x10, the set's three directions)
                const uint32_t out = block + 0x10u;
                for (uint32_t k = 0; k < 3u; ++k)
                {
                    env.call(); // ScaleVector(-1)
                    const Vec4 v = x.vu<15>(mul(txnQuad(t, kLightSet + 0x161A0u + 0x10u * k), splat(registerConstant(-1.0f))));
                    env.call(); // Normalize
                    const float r = x.root(v);
                    const float q = x.div(registerConstant(1.0f), r); // VDIV Q = vf0.w / (0 + root)
                    const Vec4 n = blend<7>(splat(0.0f), x.vu<7>(mul(v, splat(q))));
                    txnStoreQuad(t, out + 0x10u * k, n);
                }
                t.w32(out + 0x30u, 0u);
                t.w32(out + 0x3Cu, 0x3F800000u);
                t.w32(out + 0x38u, 0u);
                t.w32(out + 0x34u, 0u);
                env.call(); // TransposeMatrix(out, out)
                {
                    uint32_t m[4][4];
                    for (uint32_t r = 0; r < 4u; ++r)
                    {
                        alignas(16) uint8_t q[16];
                        t.lq(out + 0x10u * r, q);
                        std::memcpy(m[r], q, 16);
                    }
                    for (uint32_t r = 0; r < 4u; ++r)
                    {
                        alignas(16) uint8_t q[16];
                        const uint32_t row[4] = {m[0][r], m[1][r], m[2][r], m[3][r]};
                        std::memcpy(q, row, 16);
                        t.sq(out + 0x10u * r, q);
                    }
                }
                txnPut4(t, colour, out + 0x30u); // the ambient row
                const uint32_t colours = block + 0x50u;
                txnCopy4(t, kLightSet + 0x161D0u, colours);
                txnCopy4(t, kLightSet + 0x161E0u, colours + 0x10u);
                txnCopy4(t, kLightSet + 0x161F0u, colours + 0x20u);
                txnCopy4(t, kLightSet + 0x40u, colours + 0x30u);
                Vec4 rows[4];
                rows[3] = txnQuad(t, colours + 0x30u);
                for (uint32_t r = 0; r < 3u; ++r)
                    rows[r] = txnQuad(t, colours + 0x10u * r);
                rows[3] = blend<7>(rows[3], x.vu<7>(mul(rows[3], splat(registerConstant(1.0f)))));
                for (Vec4 &row : rows)
                    row = vuMin(vuMax(row, splat(0.0f)), splat(1.0f));
                for (uint32_t r = 0; r < 4u; ++r)
                    txnStoreQuad(t, colours + 0x10u * r, rows[r]);
            }
            const uint32_t modelFlags = t.u32(self + 0x250u);
            if (modelFlags & 0x80000u)
                txnCopy4(t, kFixedAlt, block + 0x80u);

            const uint32_t flags2 = t.u32(node + 0x380u);
            if ((flags2 & 0x20000000u) && e::fpuKey(floatBits(w)) == e::fpuKey(0x3F800000u))
            {
                const uint32_t u = t.u32(node + 0x38Cu), v = t.u32(node + 0x390u);
                txnPut4(t, bitsOf(u, v, 0u, 0x3F800000u), block + 0xC0u);
                const uint32_t pos = t.u32(self + 0xF0u);
                const uint32_t view = t.u32(kViewPointer);
                const Vec4 eye = txnQuad(t, view + 0x210u), at = txnQuad(t, pos);
                const Vec4 d = blend<7>(eye, x.vu<7>(sub(eye, at)));
                const Vec4 n1 = eyeNormalize(t, x, d);
                const Vec4 l = txnQuad(t, kLightSet + 0x16200u);
                const Vec4 d2 = blend<7>(n1, x.vu<7>(sub(n1, l)));
                const Vec4 n2 = eyeNormalize(t, x, d2);
                t.w32(block + 0x98u, laneBits<2>(n2));
                t.w64(block + 0x90u, laneBits<0>(n2) | static_cast<uint64_t>(laneBits<1>(n2)) << 32);
                const uint32_t info = t.u32(node + 0x414u);
                t.w16(block + 0xB0u, t.u8(info + 0x58u));
                const float scale = txnFloat(t, block + 0x8Cu);
                const uint32_t info2 = t.u32(node + 0x414u);
                const float s0 = txnFloat(t, kLightSet + 0x16210u);
                const uint32_t b = t.u8(info2 + 0x59u);
                const float s1 = txnFloat(t, kLightSet + 0x16214u), s2 = txnFloat(t, kLightSet + 0x16218u);
                float sum = x.add(s0, s1);
                float f = fpuFromSmallWord(static_cast<int32_t>(b));
                f = x.add(f, f);
                sum = x.add(sum, s2);
                f = x.div(f, 15.0f);
                sum = x.div(sum, 3.0f);
                f = x.mul(f, 0.5f);
                f = x.mul(f, sum);
                t.w32(block + 0x8Cu, floatBits(x.mul(scale, f)));
            }
            else if ((flags2 & 0x40000000u) && t.u32(kSpecialPass) != 0u)
            {
                const uint32_t view = t.u32(kViewPointer);
                t.w64(block + 0x90u, t.u64(view + 0x210u));
                t.w32(block + 0x98u, t.u32(view + 0x218u));
            }
            else if (!(flags2 & 0x10000u)) // set: the bnel skips (model +0x250 read in its delay slot, as below)
            {
                const uint32_t u = t.u32(node + 0x38Cu), v = t.u32(node + 0x390u);
                txnPut4(t, bitsOf(u, v, 0u, 0x3F800000u), block + 0xC0u);
            }
            if (t.u32(self + 0x250u) & 0x2000u)
            {
                env.call(); // sceVu0CopyMatrix(block +0x90, D_004A6940)
                Vec4 m[4];
                for (uint32_t r = 0; r < 4u; ++r)
                    m[r] = txnQuad(t, 0x4A6940u + 0x10u * r);
                for (uint32_t r = 0; r < 4u; ++r)
                    txnStoreQuad(t, block + 0x90u + 0x10u * r, m[r]);
            }

            const uint32_t packetSlot = self + model::kPacket + half * 4u;
            if ((t.u32(node + 0x380u) & 0x200000u) || (t.u32(0x747A2Cu) & 0x1000u))
            {
                t.writeExtra(t.u32(packetSlot) + 0x30u, 0u);
                t.w32(block + 0x2Cu, 0x437F0000u);
            }
            else
            {
                const int32_t id = t.s16(self + model::kId);
                const uint32_t colourOut = t.u32(packetSlot) + 0x30u;
                const float f12 = x.mul(fpuFromSmallWord(id), bitsFloat(0x3DCCCCCDu));
                env.call(); // func_002D8070(set, block +0x10, packet +0x30, f12)
                uint64_t ch[3];
                for (uint32_t k = 0; k < 3u; ++k)
                {
                    const uint32_t c = t.u32(kLightSet + 0x16158u + 4u * k);
                    ch[k] = e::fpuKey(c) < e::fpuKey(0u) ? 0u : static_cast<uint32_t>(fpuToWord(bitsFloat(c)));
                }
                t.writeExtra(colourOut, ch[0] | ch[1] << 8 | ch[2] << 16);
                float fade = 255.0f;
                if (e::fpuKey(0u) < e::fpuKey(floatBits(f12)))
                {
                    const float n = txnFloat(t, kLightSet + 0x16150u), fr = txnFloat(t, kLightSet + 0x16154u);
                    const float dd = x.sub(fr, n);
                    float r = x.div(n, dd);
                    const float q1 = x.div(f12, dd);
                    r = x.add(r, registerConstant(1.0f));
                    r = x.sub(r, q1);
                    fade = x.mul(r, 255.0f);
                }
                if (e::fpuKey(floatBits(fade)) < e::fpuKey(0u))
                    fade = bitsFloat(0u);
                if (e::fpuKey(0x437F0000u) < e::fpuKey(floatBits(fade)))
                    fade = 255.0f;
                t.w32(block + 0x2Cu, floatBits(fade));
            }
            if (!t.ok || !x.ok)
                return false;
            if (!env.commitOk())
                return false;
            t.commit();
            returnToCaller(ctx);
            return true;
        }

        // ---- tanf 0x3A1AE8 (libm: fdlibm's float s_tanf / __kernel_tanf / __ieee754_rem_pio2f; rung 3 step a) ------------
        // The game's code op for op (never the host's tanf: not bit-exact). |x| <= 0x3F490FDA: __kernel_tanf(x, 0, 1);
        // |x| <= 0x4016CBE3 (< 3pi/4): rem_pio2f's n = +-1 path, then __kernel_tanf(y0, y1, -1). Past that (rem_pio2f's
        // medium / huge paths, kernel_rem_pio2f, x - x past FLT_MAX) the native declines: tanf's callers pass half a field
        // of view in radians, so that is a FOV of 270 degrees and up. T[0..12] are read from the ELF's table at 0x45EFF0.
        constexpr uint32_t kTanf = 0x3A1AE8u, kTanTable = 0x45EFF0u;
        struct TanTable
        {
            float t[13];
        };
        template <class Env>
        bool tanTable(Env &env, TanTable &tt)
        {
            uint32_t off;
            if (!plainRam(kTanTable, 52u, off))
                return false;
            std::memcpy(tt.t, env.at(off, 52u), 52u);
            return true;
        }
        inline float negBits(float f) { return bitsFloat(floatBits(f) ^ 0x80000000u); } // neg.s
        inline float maskLow12(float f) { return bitsFloat(floatBits(f) & 0xFFFFF000u); }

        // __kernel_tanf 0x3A4830 (x, y, iy = +-1)
        inline float kernelTanf(const TanTable &tt, ExactOps &x, float xf, float yf, int iy)
        {
            const float *T = tt.t;
            const uint32_t hx = floatBits(xf), ix = hx & 0x7FFFFFFFu;
            if (ix < 0x31800000u) // |x| < 2^-28: cvt.w.s gives 0, so the tiny path always
            {
                if (iy == 1)
                    return xf;
                if (ix == 0u)
                    x.ok = false; // 1 / fabsf(x): a guest call and a zero divisor
                return x.div(bitsFloat(0xBF800000u), xf);
            }
            const bool big = ix > 0x3F2CA13Fu; // |x| >= 0.6744
            if (big)
            {
                if (static_cast<int32_t>(hx) < 0)
                {
                    xf = negBits(xf);
                    yf = negBits(yf);
                }
                const float z0 = x.sub(bitsFloat(0x3F490FDAu), xf); // pio4 - x
                const float w0 = x.sub(bitsFloat(0x33222168u), yf); // pio4lo - y
                xf = x.add(z0, w0);
                yf = bitsFloat(0u);
            }
            const float z = x.mul(xf, xf);
            const float w = x.mul(z, z);
            const float s = x.mul(z, xf);
            const float t0s = x.mul(T[0], s);
            // the two Horner chains as the original interleaves them (evens from T12, odds from T11)
            float ev = x.add(T[10], x.mul(w, T[12]));
            float od = x.add(T[9], x.mul(w, T[11]));
            ev = x.add(T[8], x.mul(w, ev));
            od = x.add(T[7], x.mul(w, od));
            ev = x.add(T[6], x.mul(w, ev));
            od = x.add(T[5], x.mul(w, od));
            ev = x.add(T[4], x.mul(w, ev));
            od = x.add(T[3], x.mul(w, od));
            ev = x.add(T[2], x.mul(w, ev));
            float r = x.add(T[1], x.mul(w, od));
            const float v = x.mul(z, ev);
            float u = x.mul(s, x.add(r, v));
            u = x.mul(z, x.add(u, yf));
            r = x.add(x.add(yf, u), t0s);
            const float wv = x.add(xf, r);
            if (big)
            {
                const float fy = fpuFromSmallWord(iy);
                const float sign = fpuFromSmallWord(1 - ((static_cast<int32_t>(hx) >> 30) & 2));
                float q = x.div(x.mul(wv, wv), x.add(wv, fy));
                q = x.sub(xf, x.sub(q, r));
                q = x.add(q, q); // 2.0 * q as the original has it: add.s q, q
                return x.mul(sign, x.sub(fy, q));
            }
            if (iy == 1)
                return wv;
            const float z2 = maskLow12(wv);
            const float v2 = x.sub(z2, xf);
            const float a = x.div(bitsFloat(0xBF800000u), wv);
            const float rv = x.sub(r, v2);
            const float t = maskLow12(a);
            const float s2 = x.add(x.mul(t, z2), registerConstant(1.0f));
            return x.add(t, x.mul(a, x.add(s2, x.mul(t, rv))));
        }

        // tanf's body: calls = the guest calls it makes (kernel_tanf, and rem_pio2f past pi/4)
        inline float tanfBody(const TanTable &tt, ExactOps &x, float a, int &calls)
        {
            const uint32_t hx = floatBits(a), ix = hx & 0x7FFFFFFFu;
            calls = 1;
            if (ix <= 0x3F490FDAu)
                return kernelTanf(tt, x, a, bitsFloat(0u), 1);
            if (ix > 0x4016CBE3u)
            {
                x.ok = false;
                return a;
            }
            calls = 2;
            // __ieee754_rem_pio2f's n = +-1 path (pio2_1 / pio2_1t, or pio2_2 / pio2_2t near pi/2)
            const bool positive = static_cast<int32_t>(hx) > 0;
            const bool nearHalfPi = (ix & 0xFFFFFFF0u) == 0x3FC90FD0u;
            const float pio2_1 = bitsFloat(0x3FC90F80u);
            float z = positive ? x.sub(a, pio2_1) : x.add(a, pio2_1);
            float tail = bitsFloat(0x37354443u);
            if (nearHalfPi)
            {
                const float pio2_2 = bitsFloat(0x37354400u);
                z = positive ? x.sub(z, pio2_2) : x.add(z, pio2_2);
                tail = bitsFloat(0x2E85A308u);
            }
            const float y0 = positive ? x.sub(z, tail) : x.add(z, tail);
            const float d = x.sub(z, y0);
            const float y1 = positive ? x.sub(d, tail) : x.add(d, tail);
            return kernelTanf(tt, x, y0, y1, -1);
        }

        template <class Env>
        bool tanfNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            TanTable tt;
            if (!tanTable(env, tt))
                return false;
            ExactOps x;
            int calls = 0;
            const float r = tanfBody(tt, x, ctx->f[12], calls);
            if (!x.ok)
                return false;
            for (int i = 0; i < calls; ++i)
                env.call();
            if (!env.commitOk())
                return false;
            e::setFBits(ctx, 0, floatBits(r));
            returnToCaller(ctx);
            return true;
        }

        // ---- sceVu0ViewScreenMatrix 0x30AA40 (a0 = m; f12 scrz, f13 ax, f14 ay, f15 cx, f16 cy, f17 zmin, f18 zmax,
        // f19 nearz, farz = the caller's 0(sp)) ----------------------------------------------------------------------------
        // d = -nearz + farz; A = ((farz * nearz) * (-zmin + zmax)) / d; B = ((-zmax * nearz) + (zmin * farz)) / d.
        // m = Unit with [0][0] = [1][1] = scrz, [2][2] = [3][3] = 0, [2][3] = [3][2] = 1; tmp (its frame) = Unit with
        // [0][0] = ax, [1][1] = ay, [2][2] = A, [3] = (cx, cy, B, 1); MulMatrix(m, tmp, m). Calls: UnitMatrix x2, MulMatrix
        // (its three back-edges). Void; f20-f26 / s0 / ra saved and restored (untouched here).
        constexpr uint32_t kViewScreen = 0x30AA40u;
        struct Matrix
        {
            Vec4 r[4];
        };
        inline bool finiteRows(const Matrix &m)
        {
            return libvu0::finiteInputs(m.r[0]) && libvu0::finiteInputs(m.r[1]) && libvu0::finiteInputs(m.r[2]) &&
                   libvu0::finiteInputs(m.r[3]);
        }
        // sceVu0MulMatrix (a0 = out, a1 = A, a2 = B): out row k = A's rows by B row k (VU0: mula x, madda y, madda z,
        // madd w). Exact for rows under 2^62 (finiteInputs); false otherwise (the native declines).
        inline bool mulMatrixRows(const Matrix &a, const Matrix &b, Matrix &out)
        {
            if (!finiteRows(a) || !finiteRows(b))
                return false;
            Matrix o;
            for (int k = 0; k < 4; ++k)
                o.r[k] = libvu0::mulRow(a.r[0], a.r[1], a.r[2], a.r[3], b.r[k]).r;
            out = o;
            return true;
        }
        inline bool viewScreenBody(ExactOps &x, float scrz, float ax, float ay, float cx, float cy, float zmin, float zmax,
                                   float nearz, float farz, Matrix &m)
        {
            const float dz = x.add(negBits(zmin), zmax);
            float A = x.mul(farz, nearz);
            float B = x.mul(negBits(zmax), nearz);
            const float zf = x.mul(zmin, farz);
            const float d = x.add(negBits(nearz), farz);
            A = x.mul(A, dz);
            B = x.add(B, zf);
            A = x.div(A, d);
            B = x.div(B, d);
            constexpr uint32_t one = 0x3F800000u;
            const uint32_t s = floatBits(scrz);
            const Matrix mm{{bitsOf(s, 0u, 0u, 0u), bitsOf(0u, s, 0u, 0u), bitsOf(0u, 0u, 0u, one), bitsOf(0u, 0u, one, 0u)}};
            const Matrix tmp{{bitsOf(floatBits(ax), 0u, 0u, 0u), bitsOf(0u, floatBits(ay), 0u, 0u),
                              bitsOf(0u, 0u, floatBits(A), 0u), bitsOf(floatBits(cx), floatBits(cy), floatBits(B), one)}};
            return mulMatrixRows(tmp, mm, m);
        }
        constexpr int kViewScreenCalls = 3, kMulMatrixBacks = 3;

        template <class Env>
        bool viewScreenNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp), mAddr = e::u32(ctx, e::a0);
            uint32_t frame, farzAt, m;
            if (!e::vf0Constant(ctx) || (sp & 0xFu) != 0u || (mAddr & 0xFu) != 0u || !plainRam(sp - 0xA0u, 0xA0u, frame) ||
                !plainRam(sp, 4u, farzAt) || !plainRam(mAddr, 64u, m) || e::overlaps(m, 64u, frame, 0xA0u))
                return false;
            ExactOps x;
            Matrix out;
            if (!viewScreenBody(x, ctx->f[12], ctx->f[13], ctx->f[14], ctx->f[15], ctx->f[16], ctx->f[17], ctx->f[18],
                                ctx->f[19], bitsFloat(e::read32(env, farzAt)), out) ||
                !x.ok)
                return false;
            for (int i = 0; i < kViewScreenCalls; ++i)
                env.call();
            for (int i = 0; i < kMulMatrixBacks; ++i)
                env.checkpoint(32u);
            if (!env.commitOk())
                return false;
            uint8_t *dst = env.at(m, 64u);
            for (int k = 0; k < 4; ++k)
                store(dst + 16 * k, out.r[k]);
            returnToCaller(ctx);
            return true;
        }

        // ---- func_0014C7E8: a model's per-frame matrices (rung 3 step a; unmatched in the decomp; our C++ from the
        // disassembly, the callees' bodies inline) -------------------------------------------------------------------------
        // void func_0014C7E8(cModel *self), called by cModel_Tag_set_rate. idx = D_007476B0 & 1; buf = self->[552 + 4 idx]
        // (+16 the view matrix, +80 the second, +272 the third); cam = D_005CAFF0. buf+16 = cam's matrix, buf+272 = Unit,
        // buf+80 = cam +192 (D_004A6900 with flags 0x2000; flags = self +592). Flags 0x80000: a fixed camera at z = 50
        // (half 1); then flags 0x1000000: the same at z = 145 times self's two matrices (half 2; both can run, the second
        // overwrites). Each half: C = CameraMatrix((0,0,z,1), (0,0,-1,1), (0,1,0,1)) (constant inputs: its result below,
        // as the recompiled function gives it: shadow_test camera); t = tanf(fov * pi / 360); scrz = ((D_003C1190 /
        // D_00586B88) * 0.5) / t; V = ViewScreenMatrix(scrz, (float)D_003C1180 / 640, D_00586B88, self +304 + 2048,
        // self +308 + 2048, 1, 65535, 0.1, 600) with [0][0] and [1][1] negated; buf+16 = MtxMultiply(V, C) (half 2: then
        // MulMatrix(buf+16, self, buf+16) and MulMatrix(buf+16, self +128, buf+16), inline VU0); q = 160 / scrz (tanf runs
        // twice: same input, same result); P = Unit, [0][0] = [1][1] = -(0.2 / (q + q)), [2] = (0,0,0x3F800AEC,1),
        // [3] = (0,0,0xBE4CD58A,0); P = MulMatrix(P, C); (InversMatrix(sp+320, P): dead, overwritten); buf+80 =
        // MulMatrix(self +128, P) (inline); buf+272 = Unit with [0][0] = ((scrz * ax) * q) / 0.1, [1][1] =
        // ((scrz * ay) * q) / 0.1, [2][2] = -32767, [3] = (2048, 2048, 32768, 1). Then for the n = self +692 nodes from
        // **(self +632) along node +324: func_001503B8(node, self->[560 + 4 idx] + 16 + 64 k) = node +0x80's matrix times a
        // Unit with row 3's xyz = node +0xC0..C8. n = 0 (the do-while would run 2^32 times) declines.
        // Everything is read before anything is written: the native declines when a write would land on anything read
        // (or on another write, or the stack window), so the original's order of loads and stores can't show. Void.
        constexpr uint32_t kModelMatrices = 0x14C7E8u;
        // its frame (0x2C0) + the deepest callee chain (CameraMatrix 0xB0, tanf 0x20 + rem_pio2f 0x50 + kernel_tanf 0x10,
        // ViewScreenMatrix 0xA0, func_001503B8 0x50 + MtxMultiply 0x10): 0x100 covers each
        constexpr uint32_t kModelMatricesWindow = 0x2C0u + 0x100u;
        // CameraMatrix((0, 0, z, 1), (0, 0, -1, 1), (0, 1, 0, 1)) for z = 50 and z = 145, row by row (x, y, z, w)
        constexpr uint32_t kFixedCamera[2][16] = {
            {0xBF800000u, 0u, 0u, 0u, 0u, 0x3F800000u, 0u, 0u, 0u, 0u, 0xBF800000u, 0u, 0u, 0u, 0x42480000u, 0x3F800000u},
            {0xBF800000u, 0u, 0u, 0u, 0u, 0x3F800000u, 0u, 0u, 0u, 0u, 0xBF800000u, 0u, 0u, 0u, 0x43110000u, 0x3F800000u}};
        inline Matrix fixedCamera(int half)
        {
            const uint32_t *c = kFixedCamera[half];
            return Matrix{{bitsOf(c[0], c[1], c[2], c[3]), bitsOf(c[4], c[5], c[6], c[7]), bitsOf(c[8], c[9], c[10], c[11]),
                           bitsOf(c[12], c[13], c[14], c[15])}};
        }
        inline Matrix unitMatrix()
        {
            constexpr uint32_t one = 0x3F800000u;
            return Matrix{{bitsOf(one, 0u, 0u, 0u), bitsOf(0u, one, 0u, 0u), bitsOf(0u, 0u, one, 0u), bitsOf(0u, 0u, 0u, one)}};
        }
        inline Vec4 negLanes01(Vec4 v, int lane) // neg.s of one lane (0 = x, 1 = y)
        {
            alignas(16) uint32_t w[4];
            store(w, v);
            w[lane] ^= 0x80000000u;
            return bitsOf(w[0], w[1], w[2], w[3]);
        }

        // The reads: plain RAM, 16-aligned where the original uses lq / lqc2 (it masks; the native wants no masking),
        // clear of the stack window; their ranges kept for the overlap check against the writes.
        template <class Env>
        struct MatricesReads
        {
            Env &env;
            uint32_t stackLow;
            bool ok = true;
            uint32_t at[24], size[24];
            int count = 0;
            bool span(uint32_t address, uint32_t n, uint32_t &off)
            {
                if (!ok || !plainRam(address, n, off) || e::overlaps(off, n, stackLow, kModelMatricesWindow))
                    return ok = false;
                return true;
            }
            bool keep(uint32_t address, uint32_t n, uint32_t &off)
            {
                if (!span(address, n, off))
                    return false;
                if (count == 24)
                    return ok = false;
                at[count] = off;
                size[count++] = n;
                return true;
            }
            uint32_t u32(uint32_t address)
            {
                uint32_t off;
                return keep(address, 4u, off) ? e::read32(env, off) : 0u;
            }
            float f32(uint32_t address) { return bitsFloat(u32(address)); }
            uint8_t u8(uint32_t address)
            {
                uint32_t off;
                return keep(address, 1u, off) ? *env.at(off, 1u) : 0u;
            }
            Matrix rows(uint32_t address)
            {
                Matrix m{};
                uint32_t off;
                if ((address & 0xFu) != 0u)
                    ok = false;
                else if (keep(address, 64u, off))
                {
                    const uint8_t *p = env.at(off, 64u);
                    for (int k = 0; k < 4; ++k)
                        m.r[k] = load(p + 16 * k);
                }
                return m;
            }
        };

        template <class Env>
        bool modelMatrices(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp = e::u32(ctx, e::sp);
            uint32_t stackLow;
            // vf0 = (0, 0, 0, 1): the unit matrices (UnitMatrix, func_001503B8's frame) come from it
            if (!e::vf0Constant(ctx) || (sp & 0xFu) != 0u ||
                !plainRam(sp - kModelMatricesWindow, kModelMatricesWindow, stackLow))
                return false;
            MatricesReads<Env> rd{env, stackLow};
            ExactOps x;
            int calls = 0, backs = 0;
            const uint32_t self = e::u32(ctx, e::a0);
            const uint32_t idx = rd.u32(0x7476B0u) & 1u;
            const uint32_t buf = rd.u32(self + 552u + 4u * idx);
            const uint32_t cam = rd.u32(kViewPointer);
            Matrix view = rd.rows(cam);      // buf+16
            Matrix third = unitMatrix();     // buf+272
            Matrix second = rd.rows(cam + 192u); // buf+80
            const uint32_t flags = rd.u32(self + 592u);
            calls += 3;
            if (flags & 0x2000u)
            {
                second = rd.rows(0x4A6900u);
                calls += 1;
            }
            if (!rd.ok)
                return false;

            if (flags & 0x1080000u)
            {
                TanTable tt;
                uint32_t tableOff;
                if (!rd.keep(kTanTable, 52u, tableOff))
                    return false;
                std::memcpy(tt.t, env.at(tableOff, 52u), 52u);
                const float fov = rd.f32(cam + 592u);
                const float f1190 = rd.f32(0x3C1190u), f6b88 = rd.f32(0x586B88u);
                const int32_t i1180 = static_cast<int32_t>(rd.u32(0x3C1180u));
                const float cxIn = rd.f32(self + 304u), cyIn = rd.f32(self + 308u);
                const Matrix m128 = rd.rows(self + 128u);
                const Matrix m0 = (flags & 0x1000000u) ? rd.rows(self) : Matrix{};
                if (!rd.ok || i1180 >= (1 << 24) || i1180 <= -(1 << 24))
                    return false;
                // the halves' shared scalars (each half computes them the same way, from the same inputs)
                const float a = x.div(x.mul(fov, bitsFloat(0x40490FDBu)), bitsFloat(0x43B40000u));
                int tanCalls = 0;
                const float t = tanfBody(tt, x, a, tanCalls);
                const float scrz = x.div(x.mul(x.div(f1190, f6b88), bitsFloat(0x3F000000u)), t);
                const float ax = x.div(fpuFromSmallWord(i1180), bitsFloat(0x44200000u));
                const float cx = x.add(cxIn, bitsFloat(0x45000000u)), cy = x.add(cyIn, bitsFloat(0x45000000u));
                const float q = x.div(bitsFloat(0x43200000u), scrz);
                const float p = negBits(x.div(bitsFloat(0x3E4CCCCDu), x.add(q, q)));
                const float tenth = bitsFloat(0x3DCCCCCDu);
                const float qx = x.div(x.mul(x.mul(scrz, ax), q), tenth);
                const float qy = x.div(x.mul(x.mul(scrz, f6b88), q), tenth);
                Matrix vs;
                if (!viewScreenBody(x, scrz, ax, f6b88, cx, cy, registerConstant(1.0f), bitsFloat(0x477FFF00u), tenth,
                                    bitsFloat(0x44160000u), vs))
                    return false;
                vs.r[0] = negLanes01(vs.r[0], 0);
                vs.r[1] = negLanes01(vs.r[1], 1);
                const uint32_t pb = floatBits(p);
                const Matrix pm{{bitsOf(pb, 0u, 0u, 0u), bitsOf(0u, pb, 0u, 0u), bitsOf(0u, 0u, 0x3F800AECu, 0x3F800000u),
                                 bitsOf(0u, 0u, 0xBE4CD58Au, 0u)}};
                const Matrix qm{{bitsOf(floatBits(qx), 0u, 0u, 0u), bitsOf(0u, floatBits(qy), 0u, 0u),
                                 bitsOf(0u, 0u, 0xC6FFFE00u, 0u), bitsOf(0x45000000u, 0x45000000u, 0x47000000u, 0x3F800000u)}};
                for (int half = 0; half < 2; ++half)
                {
                    if (!(flags & (half == 0 ? 0x80000u : 0x1000000u)))
                        continue;
                    const Matrix c = fixedCamera(half);
                    Matrix pc;
                    if (!mulMatrixRows(vs, c, view) || !mulMatrixRows(pm, c, pc) || !mulMatrixRows(pc, m128, second))
                        return false;
                    if (half == 1 && (!mulMatrixRows(m0, view, view) || !mulMatrixRows(view, m128, view)))
                        return false;
                    third = qm;
                    // CameraMatrix 1 + 7, tanf x2, ViewScreenMatrix 1 + 3, MtxMultiply 2, CopyMatrix, MtxCopy 2, UnitMatrix,
                    // MulMatrix, InversMatrix, UnitMatrix, CopyMatrix; three MulMatrix loops
                    calls += 22 + 2 * (1 + tanCalls);
                    backs += 3 * kMulMatrixBacks;
                }
            }

            // the node list: func_001503B8 per node (jal, its MtxMultiply jal, MulMatrix jal and loop), the loop back-edges
            const uint32_t n = rd.u8(self + 692u);
            const uint32_t outs = rd.u32(self + 560u + 4u * idx);
            uint32_t node = n ? rd.u32(rd.u32(self + 632u)) : 0u;
            uint32_t bufOff, outsOff;
            if (!rd.ok || n == 0u || (buf & 0xFu) != 0u || (outs & 0xFu) != 0u || !x.ok ||
                !rd.span(buf + 16u, 0x140u, bufOff) || !rd.span(outs + 16u, 64u * n, outsOff) ||
                e::overlaps(bufOff, 0x140u, outsOff, 64u * n))
                return false;
            // writes: buf +16..+144 and +272..+336 (the 0x140 span checked whole), outs +16.. (64 n)
            auto hitsWrites = [&](uint32_t off, uint32_t size) {
                return e::overlaps(off, size, bufOff, 0x80u) || e::overlaps(off, size, bufOff + 0x100u, 0x40u) ||
                       e::overlaps(off, size, outsOff, 64u * n);
            };
            for (int i = 0; i < rd.count; ++i)
                if (hitsWrites(rd.at[i], rd.size[i]))
                    return false;
            // each node's product into a buffer first: nothing lands until every node is checked
            constexpr uint32_t one = 0x3F800000u;
            alignas(16) uint8_t nodeOut[255u * 64u];
            for (uint32_t k = 0; k < n; ++k)
            {
                uint32_t mOff, fOff, linkOff;
                if ((node & 0xFu) != 0u || !rd.span(node + 0x80u, 64u, mOff) || !rd.span(node + 0xC0u, 12u, fOff) ||
                    !rd.span(node + 324u, 4u, linkOff) || hitsWrites(mOff, 64u) || hitsWrites(fOff, 12u) ||
                    hitsWrites(linkOff, 4u))
                    return false;
                const uint8_t *pm = env.at(mOff, 64u);
                const Matrix nm{{load(pm), load(pm + 16), load(pm + 32), load(pm + 48)}};
                const uint32_t f0 = e::read32(env, fOff), f1 = e::read32(env, fOff + 4u), f2 = e::read32(env, fOff + 8u);
                if (!libvu0::finiteInputs(bitsOf(f0, f1, f2, 0u)))
                    return false;
                const Matrix fm{{bitsOf(one, 0u, 0u, 0u), bitsOf(0u, one, 0u, 0u), bitsOf(0u, 0u, one, 0u), bitsOf(f0, f1, f2, one)}};
                Matrix r;
                if (!mulMatrixRows(nm, fm, r))
                    return false;
                for (int j = 0; j < 4; ++j)
                    store(nodeOut + 64u * k + 16u * j, r.r[j]);
                node = e::read32(env, linkOff);
                calls += 3;
                backs += kMulMatrixBacks + (k + 1u < n ? 1 : 0);
            }
            for (int i = 0; i < calls; ++i)
                env.call();
            for (int i = 0; i < backs; ++i)
                env.checkpoint(32u);
            if (!env.commitOk())
                return false;
            // every check passed: the writes (disjoint spans, so their order can't show)
            uint8_t *b = env.at(bufOff, 0x140u);
            for (int k = 0; k < 4; ++k)
            {
                store(b + 16 * k, view.r[k]);
                store(b + 0x40 + 16 * k, second.r[k]);
                store(b + 0x100 + 16 * k, third.r[k]);
            }
            std::memcpy(env.at(outsOff, 64u * n), nodeOut, 64u * n);
            returnToCaller(ctx);
            return true;
        }
    }

    void registerFunctions()
    {
        // Step a, batch 1: checked since K's 16:44 build of e881eeb (shadowed: 0 mismatches over ~20M calls, every
        // .ram.bin the reference's); recorded footprints.
        registerFunction(NativeFunction{"func_0014FF00", kLink, "packet", true, &link<RealEnv>, &link<ShadowEnv>, nullptr});
        registerFunction(NativeFunction{"func_0031A650", kQueue, "packet", true, &queue<RealEnv>, &queue<ShadowEnv>, nullptr});
        registerFunction(NativeFunction{"func_0031A600", kQueueOne, "packet", true, &queueOne<RealEnv>, &queueOne<ShadowEnv>,
                                        nullptr});

        // Step a, batch 2: checked since K's 21:42 build of 99e32b8 shadowed them in every replay (~42M calls, 0
        // mismatches, 0 declines, 0 skipped; every .ram.bin the reference's). Recorded footprints.
        auto add = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                      bool (*shadow)(ShadowEnv &, R5900Context *)) {
            registerFunction(NativeFunction{name, address, "packet", true, real, shadow, nullptr});
        };
        add("func_00155D60", kCountdown, &countdown<RealEnv>, &countdown<ShadowEnv>);
        add("cModel_getMeshPtr", kGetMeshPtr, &getMeshPtr<RealEnv>, &getMeshPtr<ShadowEnv>);
        add("ForwardNodeUpdate_14D768", kForwardNodes, &forwardNodes<RealEnv>, &forwardNodes<ShadowEnv>);
        add("PostInc_D_00566E10_0015B0F0_15B0F0", kPostInc, &postInc<RealEnv>, &postInc<ShadowEnv>);
        add("func_00155AE8", kTwoTags, &twoTags<RealEnv>, &twoTags<ShadowEnv>);
        add("func_00155BE8", kSlotTags, &slotTags<RealEnv>, &slotTags<ShadowEnv>);
        add("func_00155C68", kTagPair, &tagPair<RealEnv>, &tagPair<ShadowEnv>);
        add("func_00150DC8", kTexture, &texture<RealEnv>, &texture<ShadowEnv>);
        add("func_00150F68", kTextureBlock, &textureBlock<RealEnv>, &textureBlock<ShadowEnv>);
        add("func_00151168", kTextureStrip, &textureStrip<RealEnv>, &textureStrip<ShadowEnv>);
        add("func_002D8070", kLightFade, &lightFade<RealEnv>, &lightFade<ShadowEnv>);
        add("cModel_nodeLayerScreen", kNodeLayer, &nodeLayer<RealEnv>, &nodeLayer<ShadowEnv>);

        // Batches 3-4 (the new method): readable natives from the decomp's C, kRenderPath | kChargeAtEnd, unchecked
        // (dev replays shadow them; play runs the originals) until K's builds prove them. Proven: 0x14C5A0 and 0x14B9A0
        // (K's 05:35 build of 02ab933: 437K / 6.6K calls shadowed, 0 mismatches, 0 declines) and 0x151398 (K's 06:50
        // build of 9d10281: 3.3M calls matched; its declines, a third, are each texture's first lookup in a frame:
        // func_00150AA8 empties the cache every frame); 0x148BD8 with the misses native (batches 3-4b, below).
        auto addReadable = [](const char *name, uint32_t address, bool checked, bool (*real)(RealEnv &, R5900Context *),
                              bool (*shadow)(ShadowEnv &, R5900Context *), uint32_t returns) {
            NativeFunction fn{name, address, "packet", checked, real, shadow, nullptr};
            fn.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
            fn.returns = returns;
            registerFunction(fn);
        };
        addReadable("func_0014C5A0", kQueueModel, true, &queueModel<RealEnv>, &queueModel<ShadowEnv>, 0u);
        // 0x151398 with its miss path (insert + builders on a Txn): K's 08:32 build of 881a8dd shadowed 5.05M calls, 0
        // mismatches, 0 declines (the TM builder's branch never ran in a replay: sandbox-proven only).
        addReadable("func_00151398", kTextureLookup, true, &textureLookup<RealEnv>, &textureLookup<ShadowEnv>,
                    NativeFunction::kReturnsV0);
        addReadable("func_00148BD8", kMeshTextures, true, &meshTextures<RealEnv>, &meshTextures<ShadowEnv>, 0u);
        addReadable("func_0014B9A0", kScrollPacket, true, &scrollPacket<RealEnv>, &scrollPacket<ShadowEnv>, 0u);
        // Batches 3-4b (Nov 22): the texture cache's misses. 0x151398 and 0x148BD8 above run them inline (on a Txn);
        // the classification and the four builders also stand alone (reached when 0x151398's original runs). Checked
        // on K's 08:32 build of 881a8dd (every replay shadowed: 0 mismatches, 0 declines; 0x148BD8 292 calls in
        // TEST_407, 0x148690 1.34M, 0x148D30 1.08M, 0x151630 7.5M, the builders 1.75M); 0x151FA0 (TM) had no call: stays
        // shadowed.
        addReadable("func_00151630", kTextureClass, true, &textureClassNative<RealEnv>, &textureClassNative<ShadowEnv>,
                    NativeFunction::kReturnsV0);
        addReadable("func_00151990", kUploadTim2, true, &builderNative<RealEnv, 0>, &builderNative<ShadowEnv, 0>, 0u);
        addReadable("func_00151CC0", kUploadTim3, true, &builderNative<RealEnv, 1>, &builderNative<ShadowEnv, 1>, 0u);
        addReadable("func_00151688", kUploadPack, true, &builderNative<RealEnv, 2>, &builderNative<ShadowEnv, 2>, 0u);
        addReadable("func_00151FA0", kUploadTm, false, &builderNative<RealEnv, 3>, &builderNative<ShadowEnv, 3>, 0u);
        addReadable("func_00148D30", kNodeTextures, true, &nodeTextures<RealEnv>, &nodeTextures<ShadowEnv>, 0u);
        addReadable("func_00148690", kNodeMaterial, true, &nodeMaterial<RealEnv>, &nodeMaterial<ShadowEnv>, 0u);
        // Rung 3 step a, the rest of batches 3-4 (Nov 22): the callers of the Txn natives, their callees' bodies inline on
        // one Txn. Checked on K's 10:49 build of 224a9a9 (every replay shadowed: 1.35M / 1.10M calls, 0 mismatches, 0
        // declines; every .ram.bin the reference's).
        addReadable("cModel_setNodeMaterial", kSetNodeMaterial, true, &setNodeMaterial<RealEnv>,
                    &setNodeMaterial<ShadowEnv>, 0u);
        addReadable("ForwardAttackByMode_14B5D8", kForwardAttack, true, &forwardAttack<RealEnv>,
                    &forwardAttack<ShadowEnv>, 0u);
        // func_00154240, a node's lighting block (its callees inline on one BlockTxn). Checked on K's 14:30 build of
        // ed4a13e (every replay shadowed: 1.41M calls, 0 mismatches, 0 declines; every .ram.bin the reference's).
        addReadable("func_00154240", kNodeLighting, true, &nodeLighting<RealEnv>, &nodeLighting<ShadowEnv>, 0u);
        // Rung 3 step a, the 0x14C7E8 batch (Nov 23): tanf, func_0030AA40 (= sceVu0ViewScreenMatrix) and func_0014C7E8
        // composing them with the matrix natives' maths (reads first, then the writes). Unchecked: dev replays shadow them.
        addReadable("tanf", kTanf, false, &tanfNative<RealEnv>, &tanfNative<ShadowEnv>, NativeFunction::kReturnsF0);
        addReadable("func_0030AA40", kViewScreen, false, &viewScreenNative<RealEnv>, &viewScreenNative<ShadowEnv>, 0u);
        addReadable("func_0014C7E8", kModelMatrices, false, &modelMatrices<RealEnv>, &modelMatrices<ShadowEnv>, 0u);
    }
}
