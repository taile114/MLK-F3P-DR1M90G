#requires -Version 7.2
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^COM[1-9][0-9]*$')][string]$Port,
    [string]$Package = "$PSScriptRoot\fault-live-update.tar.gz"
)
$ErrorActionPreference = 'Stop'
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$checksum = Get-Content -LiteralPath ($packagePath + '.sha256') -Raw
if ($checksum -notmatch '^([0-9a-fA-F]{64})\s+') { throw 'Invalid checksum file' }
$expected = $Matches[1].ToLowerInvariant()
if ((Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'Local update archive checksum mismatch'
}
$helper = Join-Path $PSScriptRoot 'f3p_serial.ps1'
if (!(Test-Path -LiteralPath $helper)) {
    $helper = Join-Path $PSScriptRoot '../../../../npu/tools/f3p_serial.ps1'
}
if (!(Test-Path -LiteralPath $helper)) { throw 'Missing f3p_serial.ps1; copy the complete delivery folder' }
$logs = Join-Path $PSScriptRoot 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$tag = [Guid]::NewGuid().ToString('N').Substring(0,12)
$remote = "/tmp/fault-live-update-$tag.tar.gz"
$report = Join-Path $logs "transfer-$tag.json"
$requestPath = Join-Path $logs "request-$tag.json"
$request = @{
    report_path=$report
    steps=@(
        @{operation='command';command='test "$(id -u)" = 0';timeout_seconds=15},
        @{operation='upload';local_path=$packagePath;remote_path=$remote}
    )
}
[IO.File]::WriteAllText($requestPath,($request|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
& $helper -RequestFile $requestPath -PortName $Port
$result = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
if ($result.status -ne 'passed') { throw "Transfer failed; inspect $report" }
Write-Host "Transfer verified. Report: $report"
Write-Host 'Open the same serial port in PuTTY and execute these Linux commands:'
Write-Host "mkdir -p /tmp/fault-live-$tag"
Write-Host "gzip -dc $remote | tar -xf - -C /tmp/fault-live-$tag"
Write-Host "sh /tmp/fault-live-$tag/fault-live-update/install.sh"
