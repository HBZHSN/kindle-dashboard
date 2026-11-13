$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$downloads = @{
    'arm-linux-musleabi-cross.tgz' = 'https://download.wireguard.com/qemu-test/toolchains/20240917/arm-linux-musleabi-cross.tgz'
    'zlib-1.3.1.tar.gz' = 'https://zlib.net/fossils/zlib-1.3.1.tar.gz'
    'openssl-3.5.8.tar.gz' = 'https://github.com/openssl/openssl/releases/download/openssl-3.5.8/openssl-3.5.8.tar.gz'
    'curl-8.22.0.tar.xz' = 'https://curl.se/download/curl-8.22.0.tar.xz'
    'cacert.pem' = 'https://curl.se/ca/cacert.pem'
}
New-Item -ItemType Directory -Force -Path (Join-Path $root '.build') | Out-Null
foreach ($name in $downloads.Keys) {
    $target = Join-Path $root ".build/$name"
    if (-not (Test-Path -LiteralPath $target)) {
        Invoke-WebRequest -Uri $downloads[$name] -OutFile "$target.partial" -TimeoutSec 240
        Move-Item -LiteralPath "$target.partial" -Destination $target
    }
    Get-FileHash -Algorithm SHA256 -LiteralPath $target
}
Write-Host 'Also run tools/prepare_native_headers.py once for the font, PNG, YAML, HarfBuzz source archives and JSON header.'
