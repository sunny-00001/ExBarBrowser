# exbar-bridge.ps1 - Named-pipe bridge between ExBarBrowser and Node.
#
# WHY THIS LAYER EXISTS
#   Measured on this machine (2026-11-22): Node's net.createConnection("\\\\.\\pipe\\X")
#   fails with ENOENT for ExBarBrowser's AI pipe, while PowerShell's
#   System.IO.Pipes.NamedPipeClientStream connects to the SAME pipe successfully.
#   Both reach the named-pipe namespace; the difference is how libuv resolves
#   Windows pipe paths (as ordinary file paths). Workaround, not a fix on our side.
#
# PROTOCOL
#   One JSON line on stdin -> one line written to the pipe -> one response line
#   on stdout. The token is injected by STRING SUBSTITUTION on the raw line,
#   never by re-serialising: ConvertTo-Json mangled the payload and the server
#   answered "invalid_json" (measured). Passing the caller's bytes through
#   untouched is both safer and faster.
#
# IMPORTANT: this file is pure ASCII on purpose.
#   Windows PowerShell 5.1 reads .ps1 files as the ANSI code page (GBK here), so
#   UTF-8 comments become mojibake and can break parsing
#   ("TerminatorExpectedAtEndOfString"). Keep every line ASCII-only.
#
# Env vars:
#   EXBAR_RUNTIME_DIR profile runtime directory (default E:\ExBarBrowser\runtime)
#   EXBAR_PIPE      explicit pipe name   (else read ai-pipe.txt)
#   EXBAR_TOKEN     explicit token value (else read ai-token.txt)

param()

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$runtimeDir = $env:EXBAR_RUNTIME_DIR
if (-not $runtimeDir) { $runtimeDir = 'E:\ExBarBrowser\runtime' }

function Write-JsonLine($obj) {
    [Console]::Out.WriteLine(($obj | ConvertTo-Json -Compress -Depth 32))
    [Console]::Out.Flush()
}

function Fail($msg) {
    Write-JsonLine @{ ok = $false; error = $msg }
    exit 1
}

$pipePath = $env:EXBAR_PIPE
$token = $env:EXBAR_TOKEN

if (-not $pipePath) {
    $f = Join-Path $runtimeDir 'ai-pipe.txt'
    if (-not (Test-Path $f)) { Fail "missing $f (is ExBarBrowser running?)" }
    $pipePath = (Get-Content $f -Raw).Trim()
}
if (-not $token) {
    $f = Join-Path $runtimeDir 'ai-token.txt'
    if (-not (Test-Path $f)) { Fail "missing $f" }
    $token = (Get-Content $f -Raw).Trim()
}

# Locate the '{' that opens the TOP-LEVEL "params" member.
#
# Why a scan and not a regex: `"params"` also shows up inside string values
# (a tool may legitimately take an argument called params), and only a scan
# that tracks string/escape state can tell a key from a substring. Depth also
# matters: we want the params member of the request envelope (depth 1), never a
# nested object that happens to be called params.
#
# Returns the index of '{', or -1 when the request has no params object.
function Find-TopParamsBrace($s) {
    $depth = 0
    $inStr = $false
    $esc = $false
    $i = 0
    $n = $s.Length
    while ($i -lt $n) {
        $c = $s[$i]
        if ($inStr) {
            if ($esc) { $esc = $false }
            elseif ($c -eq '\') { $esc = $true }
            elseif ($c -eq '"') { $inStr = $false }
            $i++
            continue
        }
        if ($c -eq '"') {
            # Only a key at depth 1 can be the params member. Read the string
            # and check the next non-space char is ':' before accepting it.
            $start = $i
            $i++
            while ($i -lt $n) {
                if ($s[$i] -eq '\') { $i += 2; continue }
                if ($s[$i] -eq '"') { break }
                $i++
            }
            $lit = $s.Substring($start, $i - $start + 1)
            $j = $i + 1
            while ($j -lt $n -and [char]::IsWhiteSpace($s[$j])) { $j++ }
            if ($depth -eq 1 -and $lit -ceq '"params"' -and $j -lt $n -and $s[$j] -eq ':') {
                $j++
                while ($j -lt $n -and [char]::IsWhiteSpace($s[$j])) { $j++ }
                if ($j -lt $n -and $s[$j] -eq '{') { return $j }
                return -1          # params present but not an object
            }
            $i++
            continue
        }
        if ($c -eq '{' -or $c -eq '[') { $depth++ }
        elseif ($c -eq '}' -or $c -eq ']') { $depth-- }
        $i++
    }
    return -1
}

$pipeName = $pipePath -replace '^\\\\\.\\pipe\\', ''
if (-not $pipeName) { Fail "empty pipe name (ai-pipe.txt had: $pipePath)" }

# Connect with retry: the app may be restarting right now.
#
# PINNED ENDPOINTS MUST NOT FAIL OVER.
# Only the legacy path (no explicit EXBAR_PIPE) may re-read the discovery files
# on retry -- there, "follow the app across a restart" is the whole point.
# Once the parent process supplied EXBAR_PIPE / EXBAR_TOKEN it means THAT
# window: silently switching to another instance on retry would send commands
# to a window the user is not looking at, and an approval card would describe
# the wrong target. Such a failure must be reported, not papered over.
$pinned = [bool]$env:EXBAR_PIPE
$client = $null
$lastErr = ''
for ($i = 0; $i -lt 3; $i++) {
    try {
        $client = New-Object System.IO.Pipes.NamedPipeClientStream('.', $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
        $client.Connect(4000)
        break
    } catch {
        $lastErr = $_.Exception.Message
        if ($client) { try { $client.Dispose() } catch {} ; $client = $null }
        if ($pinned) { continue }        # pinned endpoint: retry in place, never switch target
        Start-Sleep -Milliseconds 600
        # Re-read discovery files each retry: the name changes after a restart.
        try {
            $f = Join-Path $runtimeDir 'ai-pipe.txt'
            if (Test-Path $f) { $pipeName = ((Get-Content $f -Raw).Trim()) -replace '^\\\\\.\\pipe\\','' }
            $tf = Join-Path $runtimeDir 'ai-token.txt'
            if (Test-Path $tf) { $token = (Get-Content $tf -Raw).Trim() }
        } catch {}
    }
}
if (-not $client -or -not $client.IsConnected) { Fail "cannot connect to pipe ${pipeName}: $lastErr" }

# UTF8Encoding($false) = no BOM. This is not a style choice:
# StreamWriter with Encoding::UTF8 emits a 3-byte BOM (EF BB BF) before the
# first write, and the server parses raw bytes as JSON -> the very first request
# of every session answered "invalid_json". Measured 2026-11-23: with the BOM it
# fails 100% of the time, without it the handshake succeeds.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$reader = New-Object System.IO.StreamReader($client, $utf8NoBom)
$writer = New-Object System.IO.StreamWriter($client, $utf8NoBom)
$writer.AutoFlush = $true

# Ready notice: Node waits for this before sending requests.
Write-JsonLine @{ ok = $true; ready = $true; pipe = $pipeName }

# Main loop. The token is injected by text substitution:
#   {"jsonrpc":...,"params":{...}}  ->  {"jsonrpc":...,"params":{...,"__token":"..."}}
# If the request has no "params" object we add one. No re-serialisation.
while ($null -ne ($line = [Console]::In.ReadLine())) {
    $line = $line.Trim()
    if (-not $line) { continue }

    $tokenFragment = '"__token":"' + $token + '"'
    # Find the TOP-LEVEL "params" object only (see Find-TopParamsBrace).
    $openIdx = Find-TopParamsBrace $line
    if ($openIdx -ge 0) {
        $next = $openIdx + 1
        while ($next -lt $line.Length -and [char]::IsWhiteSpace($line[$next])) { $next++ }
        if ($next -lt $line.Length -and $line[$next] -eq '}') {
            # params is empty: the existing '}' closes it, so insert the member
            # only. Adding a '}' here as well emitted one brace too many and the
            # server answered invalid_json (measured on tools/list).
            $out = $line.Insert($openIdx + 1, $tokenFragment)
        } else {
            # params has members -> insert token right after '{'
            $out = $line.Insert($openIdx + 1, $tokenFragment + ',')
        }
    } elseif ($line -match '"method"\s*:') {
        # No params at all: append one before the final closing brace.
        $idx = $line.LastIndexOf('}')
        if ($idx -lt 0) { Write-JsonLine @{ ok = $false; error = 'malformed request' }; continue }
        $out = $line.Substring(0, $idx) + ',"params":{' + $tokenFragment + '}' + $line.Substring($idx)
    } else {
        $out = $line
    }

    try { $writer.WriteLine($out) } catch {
        Write-JsonLine @{ ok = $false; error = ('write failed: ' + $_.Exception.Message) }
        break
    }

    $resp = $null
    try { $resp = $reader.ReadLine() } catch { break }
    if ($null -eq $resp) { break }
    [Console]::Out.WriteLine($resp)
    [Console]::Out.Flush()
}

try { $client.Dispose() } catch {}
Fail 'bridge closed'