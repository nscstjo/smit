<#
Download the pinned OpenWrt platform adaptation resources, or verify the local cache.
SDK/toolchain hashes come from the official 25.12.5 target index.
Linux hash comes from target/linux/generic/kernel-6.12 inside that SDK.
Buildroot archive hash pins the observed GitHub archive of build revision f5dae5ece4.
No router writes, package installs or firmware changes are performed.
#>
[CmdletBinding()]
param(
    [string]$Destination = (Join-Path $PSScriptRoot '../../.local/openwrt/25.12.5/downloads'),
    [string]$Mirror = 'https://downloads.openwrt.org',
    [string]$KernelMirror = 'https://cdn.kernel.org/pub/linux/kernel/v6.x',
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
$base = "$($Mirror.TrimEnd('/'))/releases/25.12.5/targets/ramips/mt7621"
$resources = @(
    @('config.buildinfo', '8fe0607632ea3c2a5b50f33a0fb7355623c2c4cb7005f578e6d2013959242003'),
    @('feeds.buildinfo', 'e11279b01e7fea7f7d399e25e969d9382be6891071cbc1225804195224b27b52'),
    @('version.buildinfo', '80987aafdf5a7f03f7c534a27c78b5cc2a190af05e46e8bcd8a854c2a93c4001'),
    @('profiles.json', 'af85c85921cd9d1be1770851a99a1dbe7e81153a5ba5081f5b37b5916e434357'),
    @('openwrt-25.12.5-ramips-mt7621.manifest', 'a0d3803dd9da870834dd6f78c9960b45832ad71dff26758faf700b05175188f3'),
    @('openwrt-sdk-25.12.5-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64.tar.zst', '9962084f4131610e90e48bc864c6ceade0b238f15f335de5f672910532b20e9c'),
    @('openwrt-toolchain-25.12.5-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64.tar.zst', '04316e081dcaea0394d3a8eca761500c91d7d54e9d49ccb8f855ae257ea4f995'),
    @('linux-6.12.94.tar.xz', 'e998a232b9418db3301cb58468e291a4f41d6ab8306029b30d991f56251dc8d2'),
    @('openwrt-f5dae5ece4.tar.gz', '37cd224b007c1b59bebfd54558c8c4af86a633a2584db47aac205de93aac8a9e')
)
if (!(Test-Path -LiteralPath $Destination)) {
    if ($VerifyOnly) { throw "Cache missing: $Destination" }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
}
$Destination = (Resolve-Path -LiteralPath $Destination).Path
$results = foreach ($resource in $resources) {
    $name, $expected = $resource
    $url = if ($name -eq 'linux-6.12.94.tar.xz') {
        "$($KernelMirror.TrimEnd('/'))/$name"
    } elseif ($name -eq 'openwrt-f5dae5ece4.tar.gz') {
        'https://codeload.github.com/openwrt/openwrt/tar.gz/f5dae5ece4'
    } else { "$base/$name" }
    $file = Join-Path $Destination $name
    $hash = if (Test-Path -LiteralPath $file) { (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' }
    if ($hash -ne $expected) {
        if ($VerifyOnly) { throw "Missing or corrupt resource: $name" }
        $partial = "$file.part"
        & curl.exe --fail --location --silent --show-error --retry 2 --connect-timeout 20 --max-time 900 --speed-limit 1024 --speed-time 60 $url --output $partial
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $url (partial kept)" }
        $hash = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne $expected) { throw "SHA256 mismatch: $name (partial kept; not accepted)" }
        Move-Item -LiteralPath $partial -Destination $file -Force
    }
    Write-Host "Verified $name"
    [pscustomobject]@{ name = $name; bytes = (Get-Item -LiteralPath $file).Length; sha256 = $hash; requested_url = $url }
}
if (!$VerifyOnly) {
    foreach ($name in @('sha256sums', 'sha256sums.asc', 'sha256sums.sig')) {
        $file = Join-Path $Destination $name
        & curl.exe --fail --location --silent --show-error --retry 2 --connect-timeout 20 --max-time 90 "$base/$name" --output "$file.part"
        if ($LASTEXITCODE -ne 0) { throw "Metadata download failed: $name" }
        Move-Item -LiteralPath "$file.part" -Destination $file -Force
    }
    $results | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $Destination 'verified-resources.json') -Encoding utf8
}
$results
