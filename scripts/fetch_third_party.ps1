# fetch_third_party.ps1 - download the exact third-party sources the Windows build uses.
#
#   scripts\fetch_third_party.ps1            # into third_party\ (skips folders that are already there)
#   scripts\fetch_third_party.ps1 -Force     # download and unpack again
#
# OpenJPEG and libjpeg-turbo are needed by the Qt application. zstd is needed to read
# zstd-compressed ZIQ recordings (uncompressed ZIQ works without it).
# Versions and SHA-256 hashes are pinned so the CI build and a local build use the same code.
[CmdletBinding()]
param(
    [switch]$Force,
    [string]$Destination = ''
)

$ErrorActionPreference = 'Stop'
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Destination)) { $Destination = Join-Path $root 'third_party' }
$Destination = [System.IO.Path]::GetFullPath($Destination)

$deps = @(
    @{
        Name = 'openjpeg-2.5.3'
        Url = 'https://github.com/uclouvain/openjpeg/archive/refs/tags/v2.5.3.tar.gz'
        Sha256 = '368FE0468228E767433C9EBDEA82AD9D801A3AD1E4234421F352C8B06E7AA707'
        Marker = 'CMakeLists.txt'
    },
    @{
        Name = 'libjpeg-turbo-3.1.2'
        Url = 'https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.2/libjpeg-turbo-3.1.2.tar.gz'
        Sha256 = '8F0012234B464CE50890C490F18194F913A7B1F4E6A03D6644179FA0F867D0CF'
        Marker = 'CMakeLists.txt'
    },
    @{
        Name = 'zstd-1.5.7'
        Url = 'https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz'
        Sha256 = 'EB33E51F49A15E023950CD7825CA74A4A2B43DB8354825AC24FC1B7EE09E6FA3'
        Marker = 'build\cmake\CMakeLists.txt'
    }
)

$tarPath = ''
$tarCommand = Get-Command tar.exe -ErrorAction SilentlyContinue
if ($tarCommand) {
    $tarPath = $tarCommand.Source
} else {
    $candidate = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (Test-Path -LiteralPath $candidate) { $tarPath = $candidate }
}
if ([string]::IsNullOrWhiteSpace($tarPath)) { throw 'tar.exe was not found. It ships with Windows 10 1803 and later.' }

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$cache = Join-Path $env:TEMP 'hrit-third-party'
New-Item -ItemType Directory -Force -Path $cache | Out-Null

foreach ($dep in $deps) {
    $target = Join-Path $Destination $dep.Name
    $marker = Join-Path $target $dep.Marker
    if ((Test-Path -LiteralPath $marker) -and -not $Force) {
        Write-Host "[ok] $($dep.Name) already present"
        continue
    }

    $archive = Join-Path $cache ($dep.Name + '.tar.gz')
    Write-Host "[..] downloading $($dep.Name)"
    Invoke-WebRequest -Uri $dep.Url -OutFile $archive -UseBasicParsing
    $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    if ($actual -ne $dep.Sha256) {
        Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
        throw "SHA-256 mismatch for $($dep.Name): expected $($dep.Sha256), got $actual"
    }

    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
    $before = @(Get-ChildItem -LiteralPath $Destination -Directory | Select-Object -ExpandProperty Name)
    Write-Host "[..] extracting $($dep.Name)"
    & $tarPath -xzf $archive -C $Destination
    if ($LASTEXITCODE -ne 0) { throw "tar failed for $($dep.Name) (exit $LASTEXITCODE)" }

    if (-not (Test-Path -LiteralPath $marker)) {
        $after = @(Get-ChildItem -LiteralPath $Destination -Directory | Select-Object -ExpandProperty Name)
        $newDirs = @($after | Where-Object { $before -notcontains $_ })
        if ($newDirs.Count -eq 1) {
            Move-Item -LiteralPath (Join-Path $Destination $newDirs[0]) -Destination $target
        }
    }
    if (-not (Test-Path -LiteralPath $marker)) {
        throw "Unexpected archive layout: $marker was not found after extracting $($dep.Name)."
    }
    Remove-Item -LiteralPath $archive -Force
    Write-Host "[ok] $($dep.Name)"
}

Write-Host "Third-party sources are in $Destination"
