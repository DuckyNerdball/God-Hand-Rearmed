// Character detail. See model_detail.h.
#include "kamite/model_detail.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstring>

namespace kamite::model_detail
{
    namespace
    {
        constexpr uint32_t kMeshFar = 0x01000000u; // cMeshInfo.flags (+0x34): a distance mesh

        uint32_t r32(const uint8_t *rdram, uint32_t a)
        {
            a &= PS2_RAM_MASK;
            if (a > PS2_RAM_SIZE - 4u)
                return 0u;
            uint32_t v = 0;
            std::memcpy(&v, rdram + a, 4);
            return v;
        }

        // At the entry of cModel_Tag_set_scr / cModel_Tag_set_rate: $a0 the model, $a2 cModel_Trans's far flag.
        void onTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 6) == 0u)
                return;
            uint32_t node = r32(rdram, GPR_U32(ctx, 4) + 0x25Cu); // cModel.meshHead
            for (uint32_t n = 0; node != 0u && n < 256u; ++n)
            {
                const uint32_t info = r32(rdram, node + 0x414u); // cModelNode.info
                if (info != 0u && (r32(rdram, info + 0x34u) & kMeshFar) == 0u)
                {
                    SET_GPR_U32(ctx, 6, 0u); // a near mesh exists: draw the near meshes
                    return;
                }
                node = r32(rdram, node + 0x404u); // cModelNode.next
            }
        }
    }

    void install()
    {
        ps2xRegisterHostHook("km_no_lod", &onTag);
        ps2xSetPatchFlag("no_lod", true);
    }
}
