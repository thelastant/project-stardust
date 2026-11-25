#Requires -Version 7
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release', 'setup-release', 'profile', 'release-wwoff')]
    [string]$Preset = 'debug',
    [switch]$Clean,
    [switch]$Test,
    [switch]$Offline,
    [string]$EbootElf,
    [string]$GeneratedDir,
    [string]$GameDir,
    [string]$EbootBin,
    [string]$Pup,
    [string]$Rpcs3CryptoDir,
    [string]$LlvmBin = $env:SSHD_LLVM_BIN
)

$ErrorActionPreference = 'Stop'

# The Windows SDK environment comes from the VS DevShell; skip if already inside one.
if (-not $env:VSCMD_VER) {
    $vsPath = $null
    $vsId = $null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $requires = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
        $vsPath = (& $vswhere -latest -version '[18.0,19.0)' -products * -requires $requires -property installationPath)
        $vsId = (& $vswhere -latest -version '[18.0,19.0)' -products * -requires $requires -property instanceId)
        if ($vsPath) { $vsPath = $vsPath.Trim() }
        if ($vsId) { $vsId = $vsId.Trim() }
    }
    if (-not $vsPath -or -not $vsId) {
        if (-not (Get-Module -ListAvailable -Name VSSetup)) {
            throw "Visual Studio 18.x (VS 2026) with the C++ tools and Windows SDK was not found. Install it, or add the VSSetup module: Install-Module VSSetup -Scope CurrentUser"
        }
        Import-Module VSSetup
        $vsInstance = Get-VSSetupInstance | Select-VSSetupInstance -Version '[18.0,19.0)'
        if ($null -eq $vsInstance) {
            throw "Visual Studio 18.x (VS 2026) with the Windows SDK was not found."
        }
        $vsPath = $vsInstance.InstallationPath
        $vsId = $vsInstance.InstanceId
    }
    Import-Module (Join-Path $vsPath "Common7/Tools/Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell $vsId -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
}

# Prepended after the DevShell so bundled tools win over the ones it adds to PATH.
$toolchainPins = Join-Path $PSScriptRoot 'toolchain/bin-paths.json'
if (Test-Path -LiteralPath $toolchainPins) {
    $toolchainBins = @(Get-Content -LiteralPath $toolchainPins -Raw | ConvertFrom-Json)
    foreach ($bin in $toolchainBins) {
        if ($bin -and (Test-Path -LiteralPath $bin -PathType Container)) {
            $env:PATH = "$bin$([System.IO.Path]::PathSeparator)$env:PATH"
        }
    }
}
if ($LlvmBin) {
    $env:PATH = "$LlvmBin$([System.IO.Path]::PathSeparator)$env:PATH"
}

# Capture fully: stopping the pipeline early with Select-Object leaves $LASTEXITCODE unset.
$clangVersion = & clang --version
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$clangVersion | Select-Object -First 1

# Resolved against the caller's location, before Push-Location. The directory need not exist:
# CMake reports a missing one with the emit command to run.
if ($GeneratedDir) {
    $GeneratedDir = [System.IO.Path]::GetFullPath($GeneratedDir, $PWD.ProviderPath)
}
if ($GameDir) {
    $GameDir = [System.IO.Path]::GetFullPath($GameDir, $PWD.ProviderPath)
}

# These must exist; the paths are fixed here because Resolve-Path would otherwise run after Push-Location.
if ($EbootBin) {
    $EbootBin = (Resolve-Path -LiteralPath $EbootBin).ProviderPath
}
if ($Pup) {
    $Pup = (Resolve-Path -LiteralPath $Pup).ProviderPath
}
if ($Rpcs3CryptoDir) {
    $Rpcs3CryptoDir = (Resolve-Path -LiteralPath $Rpcs3CryptoDir).ProviderPath
}

Push-Location (Join-Path $PSScriptRoot '..')
try {
    if ($Clean) {
        $buildDir = Join-Path (Get-Location) "build/$Preset"
        if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
    }

    $configureArgs = @('--preset', $Preset)
    if ($EbootElf) {
        $configureArgs += "-DSSHD_EBOOT_ELF=$((Resolve-Path -LiteralPath $EbootElf).Path)"
    }
    if ($GeneratedDir) {
        $configureArgs += "-DSSHD_PPU_GENERATED_DIR=$GeneratedDir"
    }
    if ($GameDir) {
        $configureArgs += "-DSSHD_GAME_DIR=$GameDir"
    }
    if ($EbootBin) {
        $configureArgs += "-DSSHD_EBOOT_BIN=$EbootBin"
    }
    if ($Pup) {
        $configureArgs += "-DSSHD_PUP=$Pup"
    }
    if ($Offline) {
        $configureArgs += "-DSSHD_OFFLINE_BUILD=ON"
    }
    if ($Rpcs3CryptoDir) {
        $configureArgs += "-DSSHD_RPCS3_CRYPTO_DIR=$Rpcs3CryptoDir"
    }
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $buildTimer = [System.Diagnostics.Stopwatch]::StartNew()
    & cmake --build --preset $Preset
    $buildExit = $LASTEXITCODE
    $buildTimer.Stop()
    "build: {0:N1} s" -f $buildTimer.Elapsed.TotalSeconds
    if ($buildExit -ne 0) { exit $buildExit }

    if ($Test) {
        & ctest --preset $Preset
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}
finally {
    Pop-Location
}
