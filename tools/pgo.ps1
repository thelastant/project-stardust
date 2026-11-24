#Requires -Version 7
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GeneratedDir,
    [Parameter(Mandatory = $true)]
    [string]$GameDir,
    [string]$Libsre,
    [string]$LlvmBin = $env:SSHD_LLVM_BIN,
    [string]$PadScript,
    [ValidateRange(1, 256)]
    [int]$Parallel = 8
)

$ErrorActionPreference = 'Stop'

# Match tools/build.ps1: CMake's Windows SDK environment comes from the VS DevShell.
if (-not $env:VSCMD_VER) {
    if (-not (Get-Module -ListAvailable -Name VSSetup)) {
        throw "Install-Module VSSetup -Scope CurrentUser"
    }
    Import-Module VSSetup
    $vsInstance = Get-VSSetupInstance | Select-VSSetupInstance -Version '[18.0,19.0)'
    if ($null -eq $vsInstance) {
        throw "Visual Studio 18.x (VS 2026) with the Windows SDK was not found."
    }
    Import-Module (Join-Path $vsInstance.InstallationPath "Common7/Tools/Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell $vsInstance.InstanceId -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
}

if (-not $LlvmBin) {
    $defaultLlvmBin = 'C:\Program Files\LLVM\bin'
    if (Test-Path -LiteralPath (Join-Path $defaultLlvmBin 'llvm-profdata.exe')) {
        $LlvmBin = $defaultLlvmBin
    }
}
if (-not $LlvmBin -or -not (Test-Path -LiteralPath (Join-Path $LlvmBin 'llvm-profdata.exe'))) {
    throw "Set -LlvmBin to an LLVM bin directory containing clang, clang++, and llvm-profdata."
}
$LlvmBin = (Resolve-Path -LiteralPath $LlvmBin).ProviderPath
$env:PATH = "$LlvmBin$([System.IO.Path]::PathSeparator)$env:PATH"

$GeneratedDir = (Resolve-Path -LiteralPath $GeneratedDir).ProviderPath
$GameDir = (Resolve-Path -LiteralPath $GameDir).ProviderPath
if (-not $Libsre) {
    $inputsDir = Join-Path (Split-Path -Parent $GeneratedDir) 'inputs'
    $Libsre = Join-Path $inputsDir 'libsre.reloc.elf'
}
$Libsre = (Resolve-Path -LiteralPath $Libsre).ProviderPath
if ($PadScript) {
    $PadScript = (Resolve-Path -LiteralPath $PadScript).ProviderPath
}

foreach ($tool in @('clang', 'clang++', 'llvm-profdata')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "LLVM tool '$tool' was not found in -LlvmBin '$LlvmBin'."
    }
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath
Push-Location $repo
try {
    $commonArgs = @(
        "-DSSHD_PPU_GENERATED_DIR=$GeneratedDir",
        "-DSSHD_GAME_DIR=$GameDir"
    )
    $profileRoot = Join-Path $repo 'build/pgo-generate/profiles'
    $runId = Get-Date -Format 'yyyyMMdd-HHmmss'
    $runDir = Join-Path $profileRoot $runId
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $profileData = Join-Path $repo "build/sshd-$runId.profdata"

    Write-Host 'Configuring the instrumented Release build...'
    & cmake --preset pgo-generate @commonArgs
    if ($LASTEXITCODE -ne 0) { throw "PGO-instrumented CMake configure failed ($LASTEXITCODE)." }
    & cmake --build --preset pgo-generate --target sshd_game --parallel $Parallel
    if ($LASTEXITCODE -ne 0) { throw "PGO-instrumented build failed ($LASTEXITCODE)." }

    $envProfile = $env:LLVM_PROFILE_FILE
    try {
        $env:LLVM_PROFILE_FILE = Join-Path $runDir 'sshd-%p.profraw'
        $gameExe = Join-Path $repo 'build/pgo-generate/bin/sshd_game.exe'
        $gameArgs = @(
            '--game-dir', $GameDir,
            '--libsre', $Libsre,
            '--frame-limit', 'off',
            '--log-level', 'info',
            '--pad', 'keyboard'
        )
        if ($PadScript) {
            $gameArgs += @('--pad-script', $PadScript)
        }

        Write-Host 'Training run started. It has no elapsed-time limit; close the game when done.'
        & $gameExe @gameArgs
        $gameExit = $LASTEXITCODE
        Write-Host "Training process exited with code $gameExit."
    }
    finally {
        $env:LLVM_PROFILE_FILE = $envProfile
    }

    $rawProfiles = @(Get-ChildItem -LiteralPath $runDir -Filter '*.profraw' -File)
    if ($rawProfiles.Count -eq 0) {
        throw "No profile was written to '$runDir'. Close the game normally so it can flush its profile."
    }

    $mergeArgs = @('merge', "-output=$profileData") + @($rawProfiles | ForEach-Object { $_.FullName })
    & llvm-profdata @mergeArgs
    if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge failed ($LASTEXITCODE)." }

    Write-Host "Merged profile: $profileData"
    Write-Host 'Configuring the profile-optimized Release build...'
    & cmake --preset pgo-use @commonArgs "-DSSHD_PGO_PROFILE=$profileData"
    if ($LASTEXITCODE -ne 0) { throw "PGO-use CMake configure failed ($LASTEXITCODE)." }
    & cmake --build --preset pgo-use --target sshd_game --parallel $Parallel
    if ($LASTEXITCODE -ne 0) { throw "PGO-optimized build failed ($LASTEXITCODE)." }

    Write-Host "Optimized executable: $(Join-Path $repo 'build/pgo-use/bin/ssHD.exe')"
    Write-Host 'Compare this against the same workload on the release build; keep PGO only if it is faster.'
}
finally {
    Pop-Location
}
