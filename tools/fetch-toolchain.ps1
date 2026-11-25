#Requires -Version 7
<#
.SYNOPSIS
Fetches the pinned open-source build tools (tools/toolchain.json) into tools/toolchain.

.DESCRIPTION
Downloads LLVM/Clang, CMake and Ninja, checks each asset's SHA-256, extracts it under
tools/toolchain/<name>-<version>/ and records its bin directory. A verified component is not
downloaded again. Writes tools/toolchain/bin-paths.json for build.ps1.

The Microsoft Windows SDK and the Vulkan SDK's shaderc are not fetched here: this script only
installs tools that are distributed as open source.

Exit codes: 0 success, 1 failure, 2 usage error, 3 a component is missing (-Check only).
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    # Only this component (llvm, cmake, ninja).
    [string]$Only,
    [switch]$Force,
    # Report what is ready without downloading anything.
    [switch]$Check,
    [Parameter(ValueFromRemainingArguments)]
    [string[]]$Unexpected
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if ($Unexpected) {
    [Console]::Error.WriteLine("fetch-toolchain: unexpected argument: $($Unexpected -join ' ')")
    [Console]::Error.WriteLine("usage: tools/fetch-toolchain.ps1 [-Only llvm|cmake|ninja] [-Force] [-Check]")
    exit 2
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-Sha256 {
    param([string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Extracts every entry under $Destination, rejecting anything that would land outside it.
function Expand-ZipSafely {
    param([string]$ZipPath, [string]$Destination)
    $destRoot = [System.IO.Path]::GetFullPath($Destination).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            $segments = $name -split '[\\/]'
            if ([System.IO.Path]::IsPathRooted($name) -or $name.Contains(':') -or ($segments -contains '..')) {
                throw "unsafe entry in the archive: '$name'"
            }
            $target = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($destRoot, $name))
            if (-not $target.StartsWith($destRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "archive entry escapes the target directory: '$name'"
            }
            if ($name.EndsWith('/') -or $name.EndsWith('\')) {
                [void][System.IO.Directory]::CreateDirectory($target)
                continue
            }
            [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($target))
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    }
    finally {
        $archive.Dispose()
    }
}

# An MSI is unpacked with an administrative install, which needs no elevation and does not
# register anything on the machine.
function Expand-MsiSafely {
    param([string]$MsiPath, [string]$Destination)
    [void][System.IO.Directory]::CreateDirectory($Destination)
    $process = Start-Process -FilePath 'msiexec.exe' -Wait -PassThru -NoNewWindow -ArgumentList @(
        '/a', "`"$MsiPath`"", '/qn', "TARGETDIR=`"$Destination`""
    )
    if ($process.ExitCode -ne 0) {
        throw "msiexec administrative install failed with exit code $($process.ExitCode)"
    }
}

function Find-BinDir {
    param([string]$Root, [string[]]$ExeNames)
    foreach ($exe in $ExeNames) {
        $hit = Get-ChildItem -LiteralPath $Root -Recurse -File -Filter $exe -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($hit) { return $hit.Directory.FullName }
    }
    return $null
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$pinFile = Join-Path $repoRoot 'tools/toolchain.json'
if (-not (Test-Path -LiteralPath $pinFile)) { throw "pin file not found: tools/toolchain.json" }
$pins = Get-Content -LiteralPath $pinFile -Raw | ConvertFrom-Json

$exeFor = @{ llvm = 'clang.exe'; cmake = 'cmake.exe'; ninja = 'ninja.exe' }

$toolchainRoot = Join-Path $repoRoot 'tools/toolchain'
$downloadDir = Join-Path $toolchainRoot 'download'
$components = @($pins.components | Where-Object { -not $Only -or $_.name -eq $Only })
if ($Only -and $components.Count -eq 0) {
    [Console]::Error.WriteLine("fetch-toolchain: unknown component '$Only'")
    exit 2
}

$binPaths = @()
$missing = @()
foreach ($component in $components) {
    $name = [string]$component.name
    $version = [string]$component.version
    $finalDir = Join-Path $toolchainRoot "$name-$version"
    $markerFile = Join-Path $finalDir '.verified-sha256'
    $binFile = Join-Path $finalDir '.bin'
    $expected = ([string]$component.sha256).ToLowerInvariant()
    if ($expected -notmatch '^[0-9a-f]{64}$') { throw "$name`: sha256 pin is not a 64-digit hex string" }

    $verified = (Test-Path -LiteralPath $markerFile -PathType Leaf) -and
        ((Get-Content -LiteralPath $markerFile -Raw).Trim() -eq $expected) -and
        (Test-Path -LiteralPath $binFile -PathType Leaf)
    if ($verified) {
        $binPaths += (Join-Path $finalDir (Get-Content -LiteralPath $binFile -Raw).Trim())
        "{0}: up to date ({1})" -f $name, $version
        continue
    }
    if ($Check) { $missing += "$name $version"; continue }

    $asset = [string]$component.asset
    $url = [string]$component.url
    $archive = Join-Path $downloadDir $asset
    if ((Test-Path -LiteralPath $archive -PathType Leaf) -and (Get-Sha256 $archive) -ne $expected) {
        Remove-Item -LiteralPath $archive -Force
    }
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        [void][System.IO.Directory]::CreateDirectory($downloadDir)
        $part = "$archive.part"
        "{0}: downloading {1}" -f $name, $url
        try {
            Invoke-WebRequest -Uri $url -OutFile $part
        }
        catch {
            if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
            throw "$name`: download failed: $($_.Exception.Message)"
        }
        Move-Item -LiteralPath $part -Destination $archive
    }
    $actual = Get-Sha256 $archive
    if ($actual -ne $expected) {
        Remove-Item -LiteralPath $archive -Force
        throw "$name`: SHA-256 mismatch: expected $expected, got $actual"
    }

    $stage = Join-Path $toolchainRoot ".stage-$name"
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    [void][System.IO.Directory]::CreateDirectory($stage)
    try {
        if ([string]$component.kind -eq 'msi') { Expand-MsiSafely -MsiPath $archive -Destination $stage }
        else { Expand-ZipSafely -ZipPath $archive -Destination $stage }
        $binDir = Find-BinDir -Root $stage -ExeNames @($exeFor[$name])
        if (-not $binDir) { throw "$name`: the archive has no $($exeFor[$name])" }
        $relative = [System.IO.Path]::GetRelativePath($stage, $binDir)
        if (Test-Path -LiteralPath $finalDir) { Remove-Item -LiteralPath $finalDir -Recurse -Force }
        Move-Item -LiteralPath $stage -Destination $finalDir
        Set-Content -LiteralPath (Join-Path $finalDir '.bin') -Value $relative -Encoding utf8 -NoNewline
        Set-Content -LiteralPath (Join-Path $finalDir '.verified-sha256') -Value $expected -Encoding utf8 -NoNewline
        $binPaths += (Join-Path $finalDir $relative)
        "{0}: installed ({1})" -f $name, $version
    }
    finally {
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    }
}

if ($Check) {
    if ($missing) {
        "fetch-toolchain: missing: $($missing -join ', ')"
        exit 3
    }
    "fetch-toolchain: all pinned tools are ready"
    exit 0
}

# build.ps1 reads this list to find the tools without changing the machine PATH.
$envFile = Join-Path $toolchainRoot 'bin-paths.json'
$existing = @()
if (Test-Path -LiteralPath $envFile) {
    $existing = @(Get-Content -LiteralPath $envFile -Raw | ConvertFrom-Json)
}
$all = @($existing + $binPaths | Select-Object -Unique | Where-Object { Test-Path -LiteralPath $_ -PathType Container })
if ($all.Count -gt 0) {
    [void][System.IO.Directory]::CreateDirectory($toolchainRoot)
    Set-Content -LiteralPath $envFile -Value (ConvertTo-Json -InputObject $all) -Encoding utf8
}
exit 0
