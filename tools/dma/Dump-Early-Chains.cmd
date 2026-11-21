@echo off
rem Dev probe (claude/strip-list.md; the step-a chat, Nov 21): the first frames' DMA chains of two cases, with rung 3's
rem batch 1 natives on and off (KAMITE_NATIVE_OFF), as source pieces (KAMITE_CHAIN_DUMP). The cases are copied into
rem cases\probe\ first, so Replay-Cases' own files and fingerprints stay untouched. Double-click it after a build;
rem then tell Claude: it reads cases\probe\*.chain.*.dump (tools\dma\chain_dump_compare.py).
setlocal
cd /d "%~dp0"
if not exist cases\probe mkdir cases\probe
copy /y "cases\Female enemy grab (bitchslap attack) 2026-11-03_15-51-15.case" "cases\probe\female_grab.case" >nul
copy /y "cases\pickup_animations_and_subboss_intro.case" "cases\probe\pickup_subboss.case" >nul
set KAMITE_CHAIN_DUMP=12
for %%c in (female_grab pickup_subboss) do (
    for %%m in (on off on2) do (
        if "%%m"=="off" (set "KAMITE_NATIVE_OFF=sceVu0CopyMatrix,sceVu0UnitMatrix,sceVu0MulVector,sceVu0ScaleVector,sceVu0OuterProduct,sceVu0Normalize,sceVu0TransMatrix,sceVu0TransposeMatrix,sceVu0InversMatrix,sceVu0CameraMatrix,sceVu0NormalLightMatrix,capVu0Length,Tramp_001F7DF8_00101E28,MtxCopy,MtxMultiply,packet") else (set "KAMITE_NATIVE_OFF=")
        echo Replaying %%c at 30 FPS, batch 1 %%m...
        GHR.exe --headless --replay-case "cases\probe\%%c.case" --fps60 0
        if exist "cases\probe\%%c.case.30.chain.dump" move /y "cases\probe\%%c.case.30.chain.dump" "cases\probe\%%c.30.chain.%%m.dump" >nul
    )
)
echo.
echo Done: tell Claude to read cases\probe.
pause
