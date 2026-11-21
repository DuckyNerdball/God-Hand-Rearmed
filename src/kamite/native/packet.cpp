// Native packet building (rung 3, claude/native-next.md): the EE code that builds the DMA chain the three native VU1
// programs read. Step a writes the same packets natively, function by function from the leaves up (K's order, Nov 21:
// claude/rung3-step-a-handoff.md); switch KAMITE_NATIVE_PACKET (unset = the checked ones, 1 = all, 0 = none). Each
// run<Env>() is the recompiled function's effect (SLUS_215.03): the same loads and stores in the same order, the same
// registers left behind (ee_ctx.h: how the generated code writes them), the guest calls' cycles; footprints recorded.

#include "kamite/native/native.h"
#include "kamite/native/ee_ctx.h"

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
    }

    void registerFunctions()
    {
        // Step a, batch 1: checked since K's 16:44 build of e881eeb (shadowed: 0 mismatches over ~20M calls, every
        // .ram.bin the reference's); recorded footprints.
        registerFunction(NativeFunction{"func_0014FF00", kLink, "packet", true, &link<RealEnv>, &link<ShadowEnv>, nullptr});
        registerFunction(NativeFunction{"func_0031A650", kQueue, "packet", true, &queue<RealEnv>, &queue<ShadowEnv>, nullptr});
        registerFunction(NativeFunction{"func_0031A600", kQueueOne, "packet", true, &queueOne<RealEnv>, &queueOne<ShadowEnv>,
                                        nullptr});

        // Step a, batch 2 (the batch-2 chat, Nov 21): unchecked (dev replays shadow them); recorded footprints, the
        // node walk's limit raised (ForwardNodeUpdate: up to 256 nodes).
        auto add = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                      bool (*shadow)(ShadowEnv &, R5900Context *)) {
            registerFunction(NativeFunction{name, address, "packet", false, real, shadow, nullptr});
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
    }
}
