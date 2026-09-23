<#
  Builds the plugin and both release archives into out/.

  Needs a MinGW-w64 GCC toolchain on PATH, or installed through winget as
  BrechtSanders.WinLibs.POSIX.UCRT, which this script will find on its own.
  Visual Studio is not required: the plugin exposes a C ABI and passes no C++
  objects across the boundary.
#>
[CmdletBinding()]
param(
    [string]$Version = '1.1'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $root 'out'

# ---------------------------------------------------------------- toolchain

function Resolve-Gpp {
    $onPath = Get-Command g++ -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $winget = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (Test-Path $winget) {
        $found = Get-ChildItem $winget -Filter 'g++.exe' -Recurse -ErrorAction SilentlyContinue |
                 Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    throw 'g++ not found. Install a toolchain: winget install BrechtSanders.WinLibs.POSIX.UCRT'
}

$gpp = Resolve-Gpp
Write-Host "toolchain : $gpp" -ForegroundColor Cyan

# ---------------------------------------------------------------- headers
# Fetched rather than vendored, so this repo carries no third-party source.

$inc = Join-Path $root 'plugin\include\reframework'
New-Item -ItemType Directory -Force $inc | Out-Null
foreach ($h in 'API.h', 'API.hpp') {
    $dest = Join-Path $inc $h
    if (Test-Path $dest) { continue }
    $url = "https://raw.githubusercontent.com/praydog/REFramework/master/include/reframework/$h"
    Write-Host "fetching  : $h" -ForegroundColor Cyan
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
}

# ---------------------------------------------------------------- compile

New-Item -ItemType Directory -Force $out | Out-Null
$dll = Join-Path $out 'WildsDualSenseBTRumble.dll'

& $gpp -std=c++17 -O2 -s -shared -o $dll (Join-Path $root 'plugin\plugin.cpp') `
    -I (Join-Path $root 'plugin\include') `
    -static -static-libgcc -static-libstdc++ -lsetupapi -Wall
if (-not (Test-Path $dll)) { throw 'compile failed' }
Write-Host ("built     : {0:N0} bytes" -f (Get-Item $dll).Length) -ForegroundColor Green

# ---------------------------------------------------------------- package
# The two releases differ by one line of lua plus the bundled README.

$luaSource = Get-Content (Join-Path $root 'lua\WildsDualSenseBTRumble.lua') -Raw -Encoding UTF8
$utf8NoBom = New-Object Text.UTF8Encoding $false

foreach ($build in @(
    @{ Lang = 'en'; Readme = 'README-en.txt'; Zip = "WildsDualSenseBTRumble-v$Version.zip" },
    @{ Lang = 'ko'; Readme = 'README-kr.txt'; Zip = "WildsDualSenseBTRumble-v$Version-KR.zip" }
)) {
    $stage = Join-Path $out ("stage-" + $build.Lang)
    if (Test-Path $stage) { Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.Delete() } }
    New-Item -ItemType Directory -Force (Join-Path $stage 'reframework\autorun') | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $stage 'reframework\plugins') | Out-Null

    $lua = $luaSource -replace 'local LANGUAGE    = "[a-z]+"', ('local LANGUAGE    = "' + $build.Lang + '"')
    [IO.File]::WriteAllText((Join-Path $stage 'reframework\autorun\WildsDualSenseBTRumble.lua'), $lua, $utf8NoBom)
    Copy-Item $dll (Join-Path $stage 'reframework\plugins') -Force
    Copy-Item (Join-Path $root ('dist\' + $build.Readme)) (Join-Path $stage 'README.txt') -Force

    $zip = Join-Path $out $build.Zip
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal -Force
    Write-Host ("packaged  : {0}  ({1:N0} bytes)" -f $build.Zip, (Get-Item $zip).Length) -ForegroundColor Green
}

Write-Host "`ndone -> $out" -ForegroundColor Cyan
