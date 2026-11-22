# Publishes a dev build of main as the tester build: K's Build-Kamite.ps1 starts it (hidden) after every normal build,
# so testers get each build as soon as K does, with nothing to publish by hand.
#
# What it does: packs build\port\bin like a player release (no saves, mods, symbols, captures or the builder's disc
# path) into "God Hand Rearmed (tester)\" with a GHR.ini whose [Launcher] Channel=tester, and puts it on the rolling
# GitHub pre-release tagged tester-build as GHR-tester-<commit>-<commit time>.zip (the older zip is deleted). Testers
# download that zip once; from then on the launcher's newspaper offers each newer build (launcher_update.cpp,
# checkTester). Players' launchers skip the release (its tag is not a version).
#
# Sign-in: git's own stored GitHub sign-in (Git Credential Manager). The first time it may open GitHub's sign-in
# window; after that it is silent. Skips anything that is not a build of main, and a commit already published.
# GHR.pdb of the last two published builds is kept in tester-symbols\ for testers' crash logs.
# Log: tester-publish-log.txt next to the builder.
param(
    [Parameter(Mandatory = $true)][string]$Repo,
    [Parameter(Mandatory = $true)][string]$Bin,
    [Parameter(Mandatory = $true)][string]$Commit, # the full commit the build was made from (the builder passes it)
    [string]$LogDir = $PSScriptRoot
)

# Continue, as in Build-Kamite.ps1: under Stop, Windows PowerShell 5.1 turns a native command's stderr into a
# terminating error. Cmdlets that must stop the run say -ErrorAction Stop; .NET exceptions and throw stop it anyway.
$ErrorActionPreference = 'Continue'
$Owner = 'God-Hand-Rearmed'
$Name = 'God-Hand-Rearmed'
$Tag = 'tester-build'
$Api = "https://api.github.com/repos/$Owner/$Name"
$Log = Join-Path $LogDir 'tester-publish-log.txt'
$Utf8 = New-Object System.Text.UTF8Encoding $false
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { } # git's output (the CHANGELOG is UTF-8)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$env:GIT_TERMINAL_PROMPT = '0' # never wait on a hidden console; Git Credential Manager's own window still shows

function Say([string]$msg) {
    $line = '[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
    Write-Host $line
    [System.IO.File]::AppendAllText($Log, $line + "`r`n", $Utf8)
}

# Zips with '/' between folders (issue #25): Windows PowerShell 5.1's ZipFile.CreateFromDirectory writes '\', which
# the zip format doesn't allow and Linux unzippers take as part of the file name. The source folder's name is the top
# folder in the zip.
function Write-Zip([string]$Source, [string]$Zip) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
    $src = (Get-Item -LiteralPath $Source).FullName.TrimEnd('\')
    $top = (Split-Path -Leaf $src) + '/'
    $level = [System.IO.Compression.CompressionLevel]::Optimal
    $archive = [System.IO.Compression.ZipFile]::Open($Zip, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($item in Get-ChildItem -LiteralPath $src -Recurse -Force) {
            $name = $top + $item.FullName.Substring($src.Length + 1).Replace('\', '/')
            if ($item.PSIsContainer) {
                if (-not (Get-ChildItem -LiteralPath $item.FullName -Force | Select-Object -First 1)) { [void]$archive.CreateEntry($name + '/') }
            } else {
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $item.FullName, $name, $level)
            }
        }
    } finally { $archive.Dispose() }
}

# One publish at a time: a second build finishing meanwhile waits here.
$mutex = New-Object System.Threading.Mutex($false, 'Local\GHR-Publish-Tester')
try { [void]$mutex.WaitOne() } catch [System.Threading.AbandonedMutexException] { } # a crashed run's: ours now
try {
    [System.IO.File]::WriteAllText($Log, '', $Utf8)
    Say "Tester build: commit $Commit"

    # ---- a build of main only ------------------------------------------------------------------------------------
    $main = "$(git -C $Repo rev-parse origin/main 2>$null)".Trim()
    if ($main -ne $Commit) { Say "Not a build of main (origin/main is $main): not published."; exit 0 }
    $short = "$(git -C $Repo rev-parse --short=7 $Commit)".Trim() # the same as the build's KAMITE_GIT_COMMIT
    $time = "$(git -C $Repo log -1 --format=%ct $Commit)".Trim()
    $when = "$(git -C $Repo log -1 --format=%ci $Commit)".Trim()
    if ($when.Length -ge 16) { $when = $when.Substring(0, 16) }
    if (-not $short -or $time -notmatch '^\d+$') { throw "could not read commit $Commit in $Repo" }
    foreach ($f in @('GHR.exe', 'pcsx2gs.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Bin $f))) { throw "$f is missing from $Bin" }
    }

    # ---- the sign-in -------------------------------------------------------------------------------------------
    if (-not "$(git config --get credential.helper)".Trim()) { throw 'git has no credential helper (Git Credential Manager), so there is no GitHub sign-in to use' }
    $fill = @('protocol=https', 'host=github.com', '') | git credential fill 2>$null
    $user = ($fill | Where-Object { $_ -like 'username=*' } | Select-Object -First 1)
    $token = ($fill | Where-Object { $_ -like 'password=*' } | Select-Object -First 1)
    if (-not $token) { throw 'no GitHub sign-in (Git Credential Manager gave none)' }
    $user = "$user".Substring(9)
    $token = "$token".Substring(9)
    $H = @{ Authorization = "token $token"; Accept = 'application/vnd.github+json'; 'User-Agent' = 'GHR-Publish-Tester';
            'X-GitHub-Api-Version' = '2022-11-28' }

    # ---- the release as it is ----------------------------------------------------------------------------------
    $rel = $null
    try { $rel = Invoke-RestMethod -Uri "$Api/releases/tags/$Tag" -Headers $H -Method Get -ErrorAction Stop }
    catch {
        $code = 0
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        if ($code -eq 401) { throw 'GitHub refused the stored sign-in (401): it has expired or was revoked' }
        if ($code -ne 404) { throw }
    }
    # The sign-in worked: keep it stored.
    @('protocol=https', 'host=github.com', "username=$user", "password=$token", '') | git credential approve 2>$null
    $prev = $null
    if ($rel) {
        foreach ($a in @($rel.assets)) {
            if ($a.name -match '^GHR-tester-([0-9a-fA-F]+)-(\d+)\.zip$') { $prev = $Matches[1] }
        }
    }
    if ($prev -and ($prev -eq $short)) { Say "Build $short is already the tester build: nothing to do."; exit 0 }

    # ---- what's new: the CHANGELOG lines added since the last tester build (else the commit subjects) ----------
    $notes = @()
    if ($prev) {
        $diff = git -C $Repo diff $prev $Commit -- CHANGELOG.md 2>$null
        if ($LASTEXITCODE -eq 0) { $notes = @($diff | Where-Object { $_ -match '^\+- ' } | ForEach-Object { $_.Substring(1) }) }
        if ($notes.Count -eq 0) { $notes = @(git -C $Repo log --format='- %s' "$prev..$Commit" 2>$null | Select-Object -First 30) }
    }
    if ($notes.Count -eq 0) { $notes = @("- $(git -C $Repo log -1 --format=%s $Commit)") }
    $body = "Tester build of main, $short ($when). Not for players: the numbered releases are.`n`n" + ($notes -join "`n")

    # ---- the zip -----------------------------------------------------------------------------------------------
    $root = Join-Path $env:TEMP "GHR-Tester-$short"
    $stage = Join-Path $root 'God Hand Rearmed (tester)'
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    # Program files only, as Make Release packs them (no saves, logs, caches, characters, developer tools, symbols or
    # dumps; mods\characters are built from K's disc).
    & robocopy $Bin $stage /E /NFL /NDL /NJH /NJS /NP /XD logs memcards mc0 mc1 states cases cache game_data mods tools shadercache_pcsx2 vu1_capture textures 'Texture Replacements' /XF *.pdb *.ilk *.exp *.lib *.log GHR.ini kamite.ini godhand.ini controls.ini kamite.exe godhand.exe pcsx2_gs_test.exe ps2_recomp.exe Replay-Cases.cmd README-FIRST.txt ram_*.bin gif_*.bin *_trace.txt update-cleanup.txt *.update-old* *.update-new | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "copying $Bin failed (robocopy $LASTEXITCODE)" }
    # GHR.ini: the template with the players' values (as Make Release), no disc image, and the tester channel.
    $ini = [System.IO.File]::ReadAllText((Join-Path $Repo 'GHR.ini')).Replace("`r`n", "`n").Replace("`n", "`r`n")
    $set = @{ 'Iso' = ''; 'Headless' = '0'; 'TimeLimitSeconds' = '0'; 'DumpOnTimeLimit' = '0'; 'Renderer' = 'pcsx2'; 'LogFile' = 'logs/kamite.log' }
    foreach ($k in $set.Keys) { $ini = [regex]::Replace($ini, "(?m)^$k=[^\r\n]*", ($k + '=' + $set[$k]).Replace('$', '$$')) }
    $launcher = [regex]'(?m)^\[Launcher\]\r?$'
    if (-not $launcher.IsMatch($ini)) { throw 'GHR.ini has no [Launcher] section' }
    $ini = $launcher.Replace($ini, "[Launcher]`r`n# Tester build: the newspaper offers each new build of main (tools/win/Publish-Tester.ps1).`r`nChannel=tester", 1)
    [System.IO.File]::WriteAllText((Join-Path $stage 'GHR.ini'), $ini, $Utf8)
    Copy-Item -LiteralPath (Join-Path $Repo 'LICENSE') -Destination $stage -Force -ErrorAction Stop
    New-Item -ItemType Directory -Path (Join-Path $stage 'docs') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Repo 'docs\licenses') -Destination (Join-Path $stage 'docs') -Recurse -Force -ErrorAction Stop
    $readme = @"
God Hand: Rearmed - tester build of main, $short ($when)

1. Double-click GHR.exe (Windows may warn about an unknown publisher: More info > Run anyway).
2. Install: choose your own God Hand disc image (.iso, USA) once.
3. New builds: the newspaper in the launcher's corner says so; UPDATE NOW installs it. Your settings, saves, save
   states and cases stay.

Every developer tool is in (F2 menu, test cases, dumps). Bug reports: name the build ($short) and attach
logs\kamite.log; for a 60 FPS bug, a recorded case (bind "Record a test case" in the launcher's Options >
Controls > Hotkeys, or F2 > Controls).
"@
    [System.IO.File]::WriteAllText((Join-Path $stage 'README-FIRST.txt'), $readme.Replace("`r`n", "`n").Replace("`n", "`r`n"), $Utf8)
    $zipName = "GHR-tester-$short-$time.zip"
    $zip = Join-Path $root $zipName
    Write-Zip $stage $zip
    $mb = [math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)
    Say "Packed $zipName ($mb MB)"

    # ---- publish -----------------------------------------------------------------------------------------------
    $json = @{ body = $body; name = 'Tester build (main)'; prerelease = $true }
    if (-not $rel) {
        $json.tag_name = $Tag
        $json.target_commitish = $Commit
        $rel = Invoke-RestMethod -Uri "$Api/releases" -Headers $H -Method Post -ContentType 'application/json; charset=utf-8' `
            -Body ([System.Text.Encoding]::UTF8.GetBytes(($json | ConvertTo-Json))) -ErrorAction Stop
        Say 'Created the tester-build release'
    }
    foreach ($a in @($rel.assets)) { # a same-named leftover from a failed upload
        if ($a.name -eq $zipName) { Invoke-RestMethod -Uri "$Api/releases/assets/$($a.id)" -Headers $H -Method Delete -ErrorAction Stop | Out-Null }
    }
    Say 'Uploading...'
    $upload = "https://uploads.github.com/repos/$Owner/$Name/releases/$($rel.id)/assets?name=$zipName"
    Invoke-RestMethod -Uri $upload -Headers $H -Method Post -ContentType 'application/zip' -InFile $zip -TimeoutSec 7200 -ErrorAction Stop | Out-Null
    # The older zips go only now, so a tester never finds the release empty.
    foreach ($a in @($rel.assets)) {
        if ($a.name -ne $zipName) { Invoke-RestMethod -Uri "$Api/releases/assets/$($a.id)" -Headers $H -Method Delete -ErrorAction Stop | Out-Null }
    }
    Invoke-RestMethod -Uri "$Api/releases/$($rel.id)" -Headers $H -Method Patch -ContentType 'application/json; charset=utf-8' `
        -Body ([System.Text.Encoding]::UTF8.GetBytes(($json | ConvertTo-Json))) -ErrorAction Stop | Out-Null
    # The tag follows the build (the release page shows its commit); not needed by the launcher.
    try {
        Invoke-RestMethod -Uri "$Api/git/refs/tags/$Tag" -Headers $H -Method Patch -ContentType 'application/json' `
            -Body (@{ sha = $Commit; force = $true } | ConvertTo-Json) -ErrorAction Stop | Out-Null
    } catch { Say "(the tag could not be moved: $($_.Exception.Message))" }

    # Symbols for testers' crash logs: the last two published builds.
    $symbols = Join-Path $LogDir 'tester-symbols'
    $pdb = Join-Path $Bin 'GHR.pdb'
    if (Test-Path -LiteralPath $pdb) {
        New-Item -ItemType Directory -Path $symbols -Force | Out-Null
        Copy-Item -LiteralPath $pdb -Destination (Join-Path $symbols "GHR-$short.pdb") -Force
        Get-ChildItem -LiteralPath $symbols -Filter 'GHR-*.pdb' | Sort-Object LastWriteTime -Descending | Select-Object -Skip 2 |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    Say "DONE: build $short is the tester build (https://github.com/$Owner/$Name/releases/tag/$Tag)"
}
catch {
    Say "FAILED: $($_.Exception.Message)"
    exit 1
}
finally {
    $mutex.ReleaseMutex()
}
