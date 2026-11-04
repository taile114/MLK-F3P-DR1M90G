#requires -Version 7.2
param(
    [Parameter(Mandatory=$true)][string]$RequestFile,
    [Alias('Port')][ValidatePattern('^COM[1-9][0-9]*$')][string]$PortName = 'COM4'
)
$ErrorActionPreference = 'Stop'
$request = Get-Content -LiteralPath $RequestFile -Raw | ConvertFrom-Json
$serialName = $PortName
$port = [System.IO.Ports.SerialPort]::new($serialName,115200,[System.IO.Ports.Parity]::None,8,[System.IO.Ports.StopBits]::One)
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.DtrEnable = $false
$port.RtsEnable = $false
$port.Encoding = [System.Text.UTF8Encoding]::new($false)
$port.ReadBufferSize = 65536
$port.WriteTimeout = 5000
$report = [ordered]@{ port=$serialName; format='115200 8N1 None'; steps=@(); status='running' }

function RemotePath([string]$Path) {
    if ($Path -notmatch '^/[A-Za-z0-9_./=-]+$' -or $Path.Contains('/../')) { throw 'Unsafe remote path' }
    return "'$Path'"
}

function Command([string]$Code, [int]$TimeoutSeconds=15) {
    $tag = [guid]::NewGuid().ToString('N').Substring(0,12)
    $begin = "__F3P_BEGIN_$tag"
    $end = "__F3P_END_$tag"
    $wire = "printf '\n$begin\n'; { $Code; }; printf '\n$end`:%s\n' `"`$?`"`r"
    if ($wire.Length -ge 500) { throw 'Command exceeds conservative console line limit; upload a script instead' }
    $port.Write($wire)
    $received = [System.Text.StringBuilder]::new()
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $pattern = '(?ms)^' + [regex]::Escape($begin) + '\r?\n(.*?)\r?\n' + [regex]::Escape($end) + ':(\d+)\r?\n'
    while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $text = $port.ReadExisting()
        if ($text) { [void]$received.Append($text) }
        if ($received.ToString().Contains($end + ':')) {
            $match = [regex]::Match($received.ToString(),$pattern)
            if ($match.Success) {
                return [pscustomobject]@{ output=$match.Groups[1].Value.Trim(); exit_code=[int]$match.Groups[2].Value }
            }
        }
        [System.Threading.Thread]::Sleep(15)
    }
    $report.last_response = $received.ToString()
    throw "Serial deadline reached after $TimeoutSeconds seconds; no further commands sent"
}

function Checked([string]$Code,[int]$TimeoutSeconds=15) {
    $result = Command $Code $TimeoutSeconds
    if ($result.exit_code -ne 0) { throw "Remote exit=$($result.exit_code): $($result.output)" }
    return $result
}

try {
    $port.Open()
    [void]$port.ReadExisting()
    if ($request.reset_owned_continuation) {
        $port.Write(([string][char]3) + "`r")
        [System.Threading.Thread]::Sleep(100)
    }
    $port.Write(([string][char]21) + "`r")
    [System.Threading.Thread]::Sleep(100)
    foreach ($step in $request.steps) {
        switch ($step.operation) {
            'command' {
                $timeout = if ($step.timeout_seconds) { [int]$step.timeout_seconds } else { 15 }
                $result = Command ([string]$step.command) $timeout
                $report.steps += @{ operation='command'; command=$step.command; output=$result.output; exit_code=$result.exit_code }
                if ($result.exit_code -ne 0) { throw "Remote command failed: exit=$($result.exit_code)" }
            }
            'upload' {
                $bytes = [System.IO.File]::ReadAllBytes([string]$step.local_path)
                $sha = [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
                $target = RemotePath ([string]$step.remote_path)
                $part = RemotePath ('/tmp/cdx-upl-' + [guid]::NewGuid().ToString('N').Substring(0,12))
                [void](Checked "test ! -e $target && (set -C; : > $part)")
                for ($offset=0; $offset -lt $bytes.Length; $offset+=192) {
                    $count = [Math]::Min(192,$bytes.Length-$offset)
                    $encoded = [Convert]::ToBase64String($bytes,$offset,$count)
                    [void](Checked "printf '%s' '$encoded' | base64 -d >> $part")
                    if (($offset % 24576) -eq 0) { Write-Output "upload_bytes=$([Math]::Min($offset+$count,$bytes.Length))/$($bytes.Length)" }
                }
                $verify = Checked "sha256sum $part"
                if ($verify.output.Split()[0] -ne $sha) { throw 'Serial upload SHA256 mismatch; partial file retained for inspection' }
                [void](Checked "test ! -e $target && mv $part $target")
                $report.steps += @{ operation='upload'; local_path=$step.local_path; remote_path=$step.remote_path; bytes=$bytes.Length; sha256=$sha; verified=$true }
            }
            'download' {
                if (Test-Path -LiteralPath $step.local_path) { throw 'Refusing to overwrite download destination' }
                $remote = RemotePath ([string]$step.remote_path)
                $hash = Checked "sha256sum $remote"
                $timeout = if ($step.timeout_seconds) { [int]$step.timeout_seconds } else { 180 }
                $sizeResult = Checked "wc -c < $remote"
                $length = [long]$sizeResult.output.Trim()
                if ($length -lt 0 -or $length -gt 67108864) { throw 'Serial download size outside supported 0..64 MiB range' }
                $chunkSize = 65536
                $stream = [System.IO.MemoryStream]::new()
                try {
                    for ([long]$offset=0; $offset -lt $length; $offset+=$chunkSize) {
                        $index = [long]($offset / $chunkSize)
                        for ($attempt=1; $attempt -le 2; $attempt++) {
                            # Kernel USB messages may otherwise splice into Base64 mid-line.
                            # Keep the kernel ring log; restore console verbosity in this subshell's EXIT trap.
                            $readBlock = '( read loglevel rest < /proc/sys/kernel/printk; dmesg -n 1 || exit 96; trap ''dmesg -n "$loglevel" || exit 97'' 0; dd if=' + $remote + ' bs=' + $chunkSize + ' skip=' + $index + ' count=1 2>/dev/null | base64 )'
                            $data = Checked $readBlock ([Math]::Min(30,[Math]::Max(15,$timeout)))
                            try {
                                $part = [Convert]::FromBase64String($data.output)
                                if ($part.Length -ne [Math]::Min($chunkSize,$length-$offset)) { throw 'Serial download chunk length differs' }
                                break
                            } catch {
                                $report.transfer_decode_error = @{chunk=$index; attempt=$attempt; response=$data.output}
                                if ($attempt -eq 2) { throw }
                                Write-Output "download_read_only_chunk_retry=$index"
                            }
                        }
                        $stream.Write($part,0,$part.Length)
                        Write-Output "download_bytes=$($stream.Length)/$length"
                    }
                    $bytes = $stream.ToArray()
                } finally { $stream.Dispose() }
                $sha = [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
                if ($sha -ne $hash.output.Split()[0]) { throw 'Serial download SHA256 mismatch' }
                [System.IO.File]::WriteAllBytes([string]$step.local_path,$bytes)
                $report.steps += @{ operation='download'; local_path=$step.local_path; remote_path=$step.remote_path; bytes=$bytes.Length; chunk_bytes=$chunkSize; kernel_console_level_during_chunk=1; console_level_restored_after_each_chunk=$true; sha256=$sha; verified=$true }
            }
            default { throw 'Unknown serial operation' }
        }
    }
    $report.status = 'passed'
} catch {
    $report.status = 'failed'
    $report.error = $_.Exception.Message
    throw
} finally {
    if ($port.IsOpen) { $port.Close() }
    $port.Dispose()
    $report.port_released = $true
    $json = ($report | ConvertTo-Json -Depth 10) + "`n"
    [System.IO.File]::WriteAllText([string]$request.report_path,$json,[System.Text.UTF8Encoding]::new($false))
    Write-Output "serial_status=$($report.status) report=$($request.report_path)"
}
