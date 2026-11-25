# tools/native: host-side checks of the native EE functions

The native functions (src/kamite/native, claude/native-next.md) and the shadow harness, checked without a Windows
build. Needs the recompiled functions, which come from the game's ELF (not in the repo): build ps2_recomp and run it on
a copy of recomp/ whose kamite.toml points at SLUS_215.03:

    cmake -G Ninja -S runtime/ps2xRecomp -B /tmp/recomp && ninja -C /tmp/recomp
    cp -r recomp /tmp/rc && cp <SLUS_215.03> /tmp/rc/
    sed -i 's|^input = .*|input = "SLUS_215.03"|; s|^output = .*|output = "/tmp/gen"|' /tmp/rc/kamite.toml
    (cd /tmp/rc && /tmp/recomp/ps2_recomp kamite.toml)

Then (INC = the include list in claude/workspace.md "Checks without a Windows build", plus -I/tmp/gen and a stub dir
with kamite/git_commit.h):

    F="sceVu0MulMatrix_0x30a2e0 sceVu0ApplyMatrix_0x30a2b0 _sceVu0ecossin_0x30a5d0 sceVu0RotMatrixX_0x30a700
       sceVu0RotMatrixY_0x30a7b8 sceVu0RotMatrixZ_0x30a650 sceVu0CopyMatrix_0x30a548 sceVu0UnitMatrix_0x30a5a0
       sceVu0MulVector_0x30a4d0 sceVu0ScaleVector_0x30a4e8 sceVu0OuterProduct_0x30a328 sceVu0Normalize_0x30a370
       sceVu0TransMatrix_0x30a508 sceVu0TransposeMatrix_0x30a3b0 sceVu0InversMatrix_0x30a3f8 sceVu0CameraMatrix_0x30a8c8
       sceVu0NormalLightMatrix_0x30a978 capVu0Length_0x1f7df8 Tramp_001F7DF8_00101E28_0x101e28 MtxCopy_0x147528
       MtxMultiply_0x147548 func_0014FF00_0x14ff00 func_0031A650_0x31a650 func_0031A600_0x31a600
       func_001503B8_0x1503b8 func_001F8DF8_0x1f8df8 func_002D7E50_0x2d7e50 func_002D8008_0x2d8008
       func_002D8070_0x2d8070 func_00150DC8_0x150dc8 func_00150F68_0x150f68 func_00151168_0x151168
       func_00155AE8_0x155ae8 func_00155BE8_0x155be8 func_00155C68_0x155c68 func_00155D60_0x155d60
       PostInc_D_00566E10_0015B0F0_15B0F0_0x15b0f0 cModel_getMeshPtr_0x14b6f8 ForwardNodeUpdate_14D768_0x14d768
       cModel_nodeLayerScreen_0x14be38 func_0014C5A0_0x14c5a0
       func_00151398_0x151398 func_00148BD8_0x148bd8 func_00151368_0x151368
       func_0014B9A0_0x14b9a0 func_00151630_0x151630 func_00151688_0x151688 func_00151990_0x151990
       func_00151CC0_0x151cc0 func_00151FA0_0x151fa0 Tim2CalcBufWidth_0x309c30 ps2___muldi3_0x31de10
       func_00148D30_0x148d30 func_00148690_0x148690 SearchDataByTwoNames_0x31cdc0 cIDBase_getSlotPtr_0x1522c0
       cModel_setNodeMaterial_0x14b4d0 ForwardAttackByMode_14B5D8_0x14b5d8 func_00154240_0x154240
       tanf_0x3a1ae8 ps2___kernel_tanf_0x3a4830 ps2___ieee754_rem_pio2f_0x3a3768 ps2___kernel_rem_pio2f_0x3a3dd8
       func_003A19D8_0x3a19d8 scalbnf_0x3a4e28 floorf_0x3a4d18 copysignf_0x3a4f88 func_0030AA40_0x30aa40
       func_0014C7E8_0x14c7e8 cModel_Tag_set_rate_0x14ac90 func_0014D0E8_0x14d0e8 func_00153B00_0x153b00
       func_002D81A0_0x2d81a0 cModel_nodeLayer_0x14bef8 func_0014B810_0x14b810 cModel_queuePacket_0x14c6b8
       cModel_Tag_set_scr_0x14aab8 Adjust_theta_vec_0x1f7da0 MtxInitTransVec_0x1476d0 MtxInitRotVec_0x147728
       MtxInitScaleVec_0x147a08 MtxMulRotVec_0x147b60 MtxMulScaleVec_0x147bf8 sceVu0RotMatrix_0x30a870
       MtxMulTransVec_0x147b18 MtxInitCoord_0x147a48 cParts_calcLocal_0x150138 MtxInverse_0x147568
       func_001501D0_0x1501d0 MtxInitRotQuaternion_0x1478a8 CopyVec3ToField30_147C40_0x147c40
       ForwardVec3At30_147C60_0x147c60 func_00150710_0x150710 Quaternion_SetRotationAxis_0x159fc8 capVu0Cos_0x1f7fb8
       sinf_0x3a19f8 ps2___kernel_cosf_0x3a3c80 ps2___kernel_sinf_0x3a4728"
    # three at a time: more runs the sandbox out of memory
    echo $F | tr ' ' '\n' | xargs -P 3 -I{} g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off $INC -c /tmp/gen/{}.cpp -o /tmp/nt/{}.o
    g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off $INC tools/native/shadow_test.cpp src/kamite/native/shadow.cpp \
        src/kamite/native/libvu0.cpp src/kamite/native/packet.cpp src/kamite/native/motion.cpp src/kamite/native/recording.cpp \
        src/kamite/texture_slots.cpp src/kamite/model_detail.cpp runtime/ps2xRuntime/src/lib/ps2_ee_float.cpp \
        /tmp/nt/*.o -o /tmp/nt/shadow_test
    /tmp/nt/shadow_test 20000 /tmp/nt/vectors.bin    # natives, footprints, invisibility, detection; writes vectors
    /tmp/nt/shadow_test bench                        # ns per call (GCC: a hint only)
    KM_ONLY=<name> KM_BENCH_SETUPS=30 /tmp/nt/shadow_test bench   # one native, the mean over 30 setups (its paths)
    KM_BENCH_CALLS=20000 ...                             # runs per setup (default 2M; cModel_Tag_set_rate: 20000)
    KM_BENCH_NATIVE_CALLEES=1 ...                        # also the recompiled function with its callees' checked natives
    /tmp/nt/shadow_test rerun <case>.<rate>.native.bin   # a replay's saved mismatches, again here
    /tmp/nt/shadow_test vectors /tmp/nt/vectors.bin      # the maths vectors alone
    KM_ONLY=<name> KM_DIFF=1 /tmp/nt/shadow_test 3000   # one native's checks only, with the differing fields

The maths alone on every path (SSE, plain C++, NEON under qemu):

    g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off -Isrc tools/native/libvu0_check.cpp -o c && ./c /tmp/nt/vectors.bin
    g++ -std=c++20 -O2 -ffp-contract=off -DKAMITE_VU_SCALAR -Isrc tools/native/libvu0_check.cpp -o c && ./c ...
    aarch64-linux-gnu-g++ -std=c++20 -O2 -ffp-contract=off -static -Isrc tools/native/libvu0_check.cpp -o c && qemu-aarch64 ./c ...

A new native function: add its recompiled function to the build lines and to shadow_test's tables (the declarations,
main's recompiled list, setupBatch1 or its own setup for its arguments, the nested list if it calls another), and its
core maths to a vectors check. A native with a recorded footprint (footprint = nullptr) gets two more checks for free:
the recording run (ShadowEnv filled from RAM on first touch) must agree with the real run, and every byte the
recompiled function writes must lie in what the native recorded. KM_DIFF=1 prints the first mismatches' differing
context words and RAM.

Rung 3's batch 2 adds `<vectors>.b2` (the FPU scalar ops, VU0 max / min, cvt.w.s, VU0 mul by 1.0 / add, against the
runtime's own ops in the natives' domains). A constant 1.0 lets the compiler fold x * 1 to x (no flush of a denormal),
so the vectors take it through a volatile, as the game does from a register. Where the network refuses GitHub's archive
zips (the configure step fetches glslang, SPIRV-Headers, SPIRV-Tools and shaderc that way: thirdparty/pcsx2-gs/
shaderc.cmake), fetch each pinned commit / tag with git and pass -DFETCHCONTENT_SOURCE_DIR_KAMITE_<NAME>=<dir>.

## The decomp feed (rung 3+)

Every native has a line in `sources.csv`: where it came from, the decomp's matched C (file, decomp commit, a hash of the
function's C) or `asm` (the disassembly). Natives written from now on come from the decomp's C where it has one; print
the line with `decomp_watch.py <decomp> --record 0x<address>`. The weekly upstream watch runs

    git clone --depth 1 --filter=blob:none --sparse https://github.com/LucasPicoli/god-hand-decomp /tmp/decomp
    git -C /tmp/decomp sparse-checkout set config include src
    python3 tools/native/decomp_watch.py /tmp/decomp [--update]

which lists asm natives whose function is now matched C (REWRITE: from the C, the harness proves it equal), decomp
natives whose C changed since their commit (CHANGED), functions matched since `decomp_rung4.txt`'s commit in the rung-4
subsystems (RUNG4: motion, collision, cloth, effects, as tools/perf/subsystems.py classifies them; `--update` moves the
baseline), and natives missing from `sources.csv` (MANIFEST). `--show 0x<address>` prints one function's C.

## The relaxed bar (render path; K's answer 2 on the method, Nov 21)

`NativeFunction::flags`: `kRenderPath` compares what lasts (RAM outside the dead stack below the entry sp, v0 / v1's low
64 bits, f0, the callee-saved registers, the rest of the context, the cycles); scratch registers and the dead stack are
don't-care (native_function.h lastingContextByte / deadStackByte). `kChargeAtEnd` runs the native without mid-function
checkpoints: native.cpp and the harness set the env's `atEnd`, checkpoints only add up, `finish()` charges the sum once
(runNative); a call the original stopped at its own checkpoint is counted as `interrupted` (the .native.txt's comment
line per function), not compared. shadow_test covers both: relaxed detection cases (scratch / dead stack ignored, s0,
a live result and cycles caught) and twins of seven composites flagged both ways (checkNative + shadowed at a spare
address).

## The texture cache's misses (batches 3-4b, Nov 22)

func_00151398's miss (insert the entry, classify the file with func_00151630, run one of the four upload builders)
reads what it has just written (each queued upload's link reads the last link's writes; the game's pack is relocated in
place), so those natives run on a `Txn` (packet.cpp): reads see the writes made so far, nothing reaches RAM until
`commit()`, a decline midway changes nothing. shadow_test builds real TIM2 / TIM3 / TM / pack files and texture managers
for them (textureFile, textureManager), switches texture_slots per call (its hook is registered through stubs of the
runtime's patch flags), and fills Tim2CalcBufWidth's (0x309C30) jump table and the format tables from the ELF's values. A recorded
footprint past 256 ranges (a TM file of 256 pictures) is skipped, not compared.

## Txn compositions (rung 3 step a, Nov 22)

func_00148BD8 / 00148D30 / 00148690 are bodies on a caller's `Txn` (txnMeshTextures, txnNodeTextures,
txnNodeMaterial) plus thin wrappers that open their own. cModel_setNodeMaterial and ForwardAttackByMode open one Txn
(their 0x10 frame + the deepest callee's window) and run their callee's body inline, one jal charged for it.
cModel_alpha (setNodeMaterial's mode 3) uses divScalar / mulScalar for |tint.w| < 2^126 and declines outside, as
cModel_nodeLayerScreen does. shadow_test sets them up through their callees' setups (the special pass, or the other
path with the node's flag or D_00747AA8 clear, so a random name never reaches the cache).

## func_00154240, a node's lighting block (rung 3 step a, Nov 22)

Unmatched in the decomp (baee8f4): our C++ from the disassembly, its seven callees' bodies inline (the light rows by
four paths: fixed, MulVector, func_002D8008, func_002D7E50 with NormalLightMatrix; the env pass with two capVu0Length;
CopyMatrix; func_002D8070). It writes only the node's 0xD0-byte block and one packet word, so it runs on a `BlockTxn`
(packet.cpp): a snapshot of the block taking the writes, a halfword mask for commit, a quick plainRam answer for RAM
below the I/O range; the general Txn's per-write hashing made it slower than the original (GCC bench 410 vs 232 ns;
BlockTxn: 206 vs 457 ns over 30 setups; callgrind 22.5G vs 41.7G instructions). Its maths: `ExactOps` declines any op
past FLT_MAX - 1 ulp or a divide outside exponents 1-254 (the runtime's ps2ee cases); constants 1.0 / -1.0 go through a
volatile (`registerConstant`), else GCC folds x * 1.0 and a denormal escapes the flush (the first sandbox mismatch).
shadow_test's setup picks the four paths evenly, the env pass half the time (colour w = 1.0), all branches of the
packet tail; 17 planted bugs (one per path / branch, the cycles) all caught.

## tanf, func_0030AA40 and func_0014C7E8 (rung 3 step a, Nov 23)

tanf is the game's fdlibm float code op for op on ExactOps (never the host's tanf); rem_pio2f's medium / huge paths
(|x| past 3pi/4, a FOV of 270 degrees and up) decline. func_0014C7E8 reads everything first, computes into locals (the
node list into a buffer) and writes at the end; it declines when a write would land on anything it read, on another
write or on the stack window, so the original's order of loads and stores can't show. Its two fixed CameraMatrix results
are constants: `shadow_test camera` prints them from the recompiled function. shadow_test's setups: tanf's argument at
each path's edges (pi/4, 0.6744, near pi/2, 3pi/4, tiny / denormal), the ELF's T table (sometimes wild); the model's
flags (0x2000 / 0x80000 / 0x1000000 each half the time), FOVs (90 exactly, past 270), 1-255 nodes in a shuffled chain,
outputs aliased over an input, unaligned or in the stack window (each declines).

## cModel_Tag_set_rate fused (rung 3 step a's last item, Nov 23)

A model's per-frame tagging with its whole subtree inline (no guest call): the countdowns (ForwardNodeUpdate /
func_00155D60), func_0014C7E8, func_00154240 per node, func_00155BE8, per node the box test (func_001F8DF8),
setNodeMaterial / ForwardAttackByMode, nodeLayerScreen, func_00155AE8 and the queue, func_0014C5A0, and the UV-scroll
pass (PostInc, func_0014B9A0). The callees read what earlier ones wrote, so it runs on a `Journal` (packet.cpp): each
write goes to RAM at once, in the original's order, with its old bytes kept; a decline rolls them back newest first.
Reads need no lookup, and the Txn / BlockTxn bodies run on it unchanged (their bodies are templated on the store:
setNodeMaterialBody, forwardAttackBody, nodeLightingBody; func_0014C7E8 / 0014B9A0 hand their writes to it). no_lod's
hook (km_no_lod, at the entry) is carried inside; getMeshPtr's observers run where each call would have entered it.
Its recorded footprint runs to thousands of ranges: NativeFunction::maxRanges raises the limit for it alone.
shadow_test builds it from the material callee's setup (node 0, the texture manager), func_0014C7E8's (the model, its
camera, its parts; ordinary screen values and the ELF's T table unless wild) and clones of node 0 (the same textures,
each its own flags), with each pass's flags at random. A lesson from it: func_001F8DF8's corners go through the frame's
copy of the world matrix, whose last row's xyz its own corner stores zero (+0x60..+0x6B).

## Rung 4: motion (M1, Nov 25)

src/kamite/native/motion.cpp + motion_core.h (claude/rung4-motion-map.md), switch KAMITE_NATIVE_MOTION; every motion
native kRenderPath | kChargeAtEnd with returns = 0, allowed per batch by `caller_scan.py <ELF> <repo> --rels <God Hand
Extract\dll\debugoptimize> <addr>...` (0 sites reading a scratch register after the call, ELF and the 81 DLLs).
setupMotion gives them angles / orders / aliasing / frames; MtxInitRotVec's jump table (0x41EA00) and Adjust_theta_vec's
table (0x3BF110) are test regions. `<vectors>.motion` (Adjust_theta_vec, MtxInitRotVec, MtxMulRotVec, MtxMulScaleVec,
MtxInverse from the recompiled functions) runs in libvu0_check on SSE / plain C++ / NEON. Batch 2's composites chain
batch 1's bodies in registers (motion.cpp multiplyStep / rotationStep, each step checked as its own native checks it);
func_001501D0's twist path (+0x154 & 0x800) is declined, its callees are in the test as recompiled-only. checkNative counts a call as interrupted
only when the original's own checkpoint came due (its dueAt-th call), not on any differing pc: a native whose original
restores a frame it overwrote (another ra) is compared and fails (planted: MtxMulScaleVec's out in its frame).
New setups draw from g_rng only for their own functions: a draw for every native shifts all later inputs, and on a
shifted sequence a batch-3 setup drove the recompiled func_00151398 to a special address (the stub's abort, pc 0x1514E0)
in checkInvisible.
