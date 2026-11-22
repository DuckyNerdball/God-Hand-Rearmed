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
       func_00151CC0_0x151cc0 func_00151FA0_0x151fa0 func_00309C30_0x309c30 ps2___muldi3_0x31de10
       func_00148D30_0x148d30 func_00148690_0x148690 SearchDataByTwoNames_0x31cdc0 cIDBase_getSlotPtr_0x1522c0"
    # three at a time: more runs the sandbox out of memory
    echo $F | tr ' ' '\n' | xargs -P 3 -I{} g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off $INC -c /tmp/gen/{}.cpp -o /tmp/nt/{}.o
    g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off $INC tools/native/shadow_test.cpp src/kamite/native/shadow.cpp \
        src/kamite/native/libvu0.cpp src/kamite/native/packet.cpp src/kamite/native/recording.cpp \
        src/kamite/texture_slots.cpp runtime/ps2xRuntime/src/lib/ps2_ee_float.cpp /tmp/nt/*.o -o /tmp/nt/shadow_test
    /tmp/nt/shadow_test 20000 /tmp/nt/vectors.bin    # natives, footprints, invisibility, detection; writes vectors
    /tmp/nt/shadow_test bench                        # ns per call (GCC: a hint only)
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
runtime's patch flags), and fills func_00309C30's jump table and the format tables from the ELF's values. A recorded
footprint past 256 ranges (a TM file of 256 pictures) is skipped, not compared.
