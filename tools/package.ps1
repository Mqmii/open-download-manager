# Builds a self-contained ZIP for a GitHub release.
#
# The goal is "download, unzip, run": no installer, no Visual C++
# Redistributable, no SDK on the user's machine. That means the MSVC runtime
# DLLs travel with the app -- ODM.exe imports MSVCP140 / VCRUNTIME140 /
# VCRUNTIME140_1, and a clean Windows install does not necessarily have them.
#
# Usage (from the repository root):
#   powershell -ExecutionPolicy Bypass -File tools/package.ps1 -Version 0.1.0

param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Config = 'Release',
    [string]$BuildDir = 'build'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path $root "$BuildDir\$Config"
if (-not (Test-Path (Join-Path $binDir 'ODM.exe'))) {
    throw "ODM.exe not found in $binDir. Build first: cmake --build $BuildDir --config $Config"
}

$name    = "ODM-v$Version-win-x64"
$stage   = Join-Path $root "dist\$name"
$zipPath = Join-Path $root "dist\$name.zip"
if (Test-Path $stage)   { Remove-Item $stage -Recurse -Force }
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

# --- the app itself -------------------------------------------------------
Copy-Item (Join-Path $binDir 'ODM.exe') $stage
Copy-Item (Join-Path $binDir '*.dll') $stage

# assets/ holds the interface AND Ultralight's own resources (cacert.pem,
# ICU data); the app will not start without them.
Copy-Item (Join-Path $binDir 'assets') $stage -Recurse

# --- MSVC runtime ---------------------------------------------------------
# Microsoft allows shipping these next to the executable ("central deployment"
# is not required). Taken from the installed VS redist folder.
$crtRoot = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT' `
    -Directory -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
if (-not $crtRoot) {
    throw 'Visual C++ redistributable DLLs not found. Install the "MSVC v143 - VS 2022 C++ x64 build tools" component.'
}
foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    Copy-Item (Join-Path $crtRoot.FullName $dll) $stage
}

# --- the browser extension ------------------------------------------------
# Shipped as a plain folder on purpose: Chrome refuses .crx files installed
# from outside the Web Store, so "Load unpacked" is the only route a user can
# actually take. That also means the folder has to stay where it is after
# unzipping, or Chrome drops the extension.
$extDst = Join-Path $stage 'extension'
Copy-Item (Join-Path $root 'extension') $extDst -Recurse
# Development-only files: a smoke-test page and a Node test runner, useless to
# a user and only good for raising questions.
Get-ChildItem $extDst -Include 'test_page.html', 'test_sniffer.js' -Recurse |
    Remove-Item -Force -ErrorAction SilentlyContinue

Set-Content -Path (Join-Path $stage 'INSTALL-EXTENSION.txt') -Encoding UTF8 -Value @'
Installing the browser extension
================================

The extension is what captures video from a page and hands browser downloads
over to ODM. It is not on the Chrome Web Store yet, so it is installed by hand:

  1. Start ODM.exe first.
  2. Open chrome://extensions in Chrome (edge://extensions in Edge).
  3. Turn on "Developer mode", top right.
  4. Click "Load unpacked" and pick the "extension" folder next to ODM.exe.

Keep this folder where it is. Chrome loads an unpacked extension from its
original path, so moving or deleting the folder disables it.

The extension finds ODM on 127.0.0.1:47923 by itself and the popup shows
whether it is connected. Nothing is sent anywhere else.
'@

# --- docs -----------------------------------------------------------------
Copy-Item (Join-Path $root 'README.md')  $stage
Copy-Item (Join-Path $root 'LICENSE')    $stage

# --- strip anything the running app leaves behind -------------------------
# bridge.token is a per-run secret, the console log is a debug artefact, and
# .part/.odmprog files would be someone's half-finished download.
Get-ChildItem $stage -Recurse -Include 'bridge.token', 'odm-console.log', '*.part', '*.odmprog', '*.vtrk', '*.atrk' |
    Remove-Item -Force -ErrorAction SilentlyContinue

# --- verify before zipping ------------------------------------------------
$required = @('ODM.exe', 'Ultralight.dll', 'UltralightCore.dll', 'WebCore.dll', 'AppCore.dll',
              'libcurl.dll', 'avformat-62.dll', 'avcodec-62.dll', 'avutil-60.dll', 'mediainfo.dll',
              'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$missing = $required | Where-Object { -not (Test-Path (Join-Path $stage $_)) }
if ($missing) { throw "Missing from the package: $($missing -join ', ')" }
foreach ($p in 'assets\index.html', 'assets\app.js', 'assets\app.css', 'assets\resources\cacert.pem',
                'extension\manifest.json', 'extension\background.js', 'extension\content.js',
                'extension\mse_probe.js', 'extension\icons\icon128.png', 'INSTALL-EXTENSION.txt') {
    if (-not (Test-Path (Join-Path $stage $p))) { throw "Missing from the package: $p" }
}

Compress-Archive -Path $stage -DestinationPath $zipPath -CompressionLevel Optimal
$sha = (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLower()

Write-Host ""
Write-Host "Package : $zipPath"
Write-Host "Size    : $([math]::Round((Get-Item $zipPath).Length / 1MB, 1)) MB"
Write-Host "SHA-256 : $sha"
Write-Host ""
Write-Host "Publish with:"
Write-Host "  gh release create v$Version `"$zipPath`" --title `"ODM v$Version`" --notes-file <notes.md>"
