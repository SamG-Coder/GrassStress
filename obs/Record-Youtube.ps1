$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = 'C:\StressTest' }
$videoDir = Join-Path $root 'video'
$obsDir = Join-Path $root 'obs'
$overlayDir = Join-Path $obsDir 'overlays'
$obsExe = 'C:\Program Files\obs-studio\bin\64bit\obs64.exe'
$appExe = Join-Path $root 'build\bin\GrassStress.exe'
$csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$wsUrl = 'ws://127.0.0.1:4455/'
$wsPassword = 'TVUYcIHw6KzdZOfq'
$recordSeconds = 75

New-Item -ItemType Directory -Force -Path $videoDir, $overlayDir | Out-Null

function Ensure-Overlays {
    $needed = @('letterbox.png','vignette.png','pathtraced-bug.png')
    if ($needed | ForEach-Object { -not (Test-Path (Join-Path $overlayDir $_)) } | Where-Object { $_ }) {
        if (-not (Test-Path $csc)) { throw 'csc.exe not found; cannot build overlays.' }
        $src = Join-Path $root 'tools\MakeOverlays.cs'
        $out = Join-Path $env:TEMP 'MakeOverlays.exe'
        & $csc /nologo /target:exe /out:$out /r:System.Drawing.dll $src
        if ($LASTEXITCODE -ne 0) { throw 'Overlay compiler failed.' }
        & $out $overlayDir
    }
}

function Install-ObsConfig {
    $appData = Join-Path $env:APPDATA 'obs-studio'
    $profileDir = Join-Path $appData 'basic\profiles\GrassStress'
    $sceneDir = Join-Path $appData 'basic\scenes'
    New-Item -ItemType Directory -Force -Path $profileDir, $sceneDir | Out-Null
    Copy-Item (Join-Path $obsDir 'GrassStress.ini') (Join-Path $profileDir 'basic.ini') -Force
    Copy-Item (Join-Path $obsDir 'GrassStress.json') (Join-Path $sceneDir 'GrassStress.json') -Force

    $wsConfigPath = Join-Path $appData 'plugin_config\obs-websocket\config.json'
    $ws = Get-Content -Raw $wsConfigPath | ConvertFrom-Json
    $ws.server_enabled = $true
    $ws.auth_required = $true
    $ws.alerts_enabled = $false
    $ws | ConvertTo-Json | Set-Content -Encoding UTF8 $wsConfigPath

    $userIni = Join-Path $appData 'user.ini'
    $text = Get-Content -Raw $userIni
    $text = $text -replace 'ConfirmOnExit=true','ConfirmOnExit=false'
    $text = $text -replace 'Profile=.*','Profile=GrassStress'
    $text = $text -replace 'ProfileDir=.*','ProfileDir=GrassStress'
    $text = $text -replace 'SceneCollection=.*','SceneCollection=GrassStress'
    $text = $text -replace 'SceneCollectionFile=.*','SceneCollectionFile=GrassStress.json'
    Set-Content -Path $userIni -Value $text -Encoding UTF8
}

function Obs-AuthSecret([string]$password, [string]$salt, [string]$challenge) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $enc = [System.Text.Encoding]::UTF8
    $secretHash = $sha.ComputeHash($enc.GetBytes($password + $salt))
    $secret = [Convert]::ToBase64String($secretHash)
    $authHash = $sha.ComputeHash($enc.GetBytes($secret + $challenge))
    return [Convert]::ToBase64String($authHash)
}

function Ws-Send($ws, $obj) {
    $json = ($obj | ConvertTo-Json -Compress -Depth 8)
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $seg = New-Object System.ArraySegment[byte] -ArgumentList @(,$bytes)
    $ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).Wait()
}

function Ws-Recv($ws) {
    $buffer = New-Object byte[] 65536
    $seg = New-Object System.ArraySegment[byte] -ArgumentList @(,$buffer)
    $ms = New-Object System.IO.MemoryStream
    do {
        $result = $ws.ReceiveAsync($seg, [Threading.CancellationToken]::None).Result
        $ms.Write($buffer, 0, $result.Count)
    } while (-not $result.EndOfMessage)
    $text = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
    return $text | ConvertFrom-Json
}

function Connect-Obs {
    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    $ws.ConnectAsync([Uri]$wsUrl, [Threading.CancellationToken]::None).Wait()
    $hello = Ws-Recv $ws
    $auth = $null
    if ($hello.d.authentication) {
        $auth = Obs-AuthSecret $wsPassword $hello.d.authentication.salt $hello.d.authentication.challenge
    }
    $identify = @{ op = 1; d = @{ rpcVersion = 1 } }
    if ($auth) { $identify.d.authentication = $auth }
    Ws-Send $ws $identify
    $identified = Ws-Recv $ws
    if ($identified.op -ne 2) { throw "OBS identify failed: $($identified | ConvertTo-Json -Compress)" }
    return $ws
}

function Obs-Request($ws, [string]$type, $data) {
    $id = [guid]::NewGuid().ToString()
    $req = @{ op = 6; d = @{ requestType = $type; requestId = $id } }
    if ($data) { $req.d.requestData = $data }
    Ws-Send $ws $req
    while ($true) {
        $msg = Ws-Recv $ws
        if ($msg.op -eq 7 -and $msg.d.requestId -eq $id) { return $msg }
    }
}

Ensure-Overlays
if (-not (Test-Path $appExe)) { throw "GrassStress.exe missing. Build first." }
if (-not (Test-Path $obsExe)) { throw "OBS is not installed at $obsExe" }

Get-Process obs64,GrassStress -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
Install-ObsConfig

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NativeWin {
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
}
"@
Write-Host 'Launching GrassStress...'
$app = Start-Process -FilePath $appExe -ArgumentList '--record' -PassThru -WorkingDirectory (Split-Path $appExe)
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    $proc = Get-Process -Id $app.Id -ErrorAction SilentlyContinue
    if (-not $proc -or $proc.HasExited) { throw 'GrassStress failed to start.' }
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero -and
        [NativeWin]::IsWindowVisible($proc.MainWindowHandle)) { break }
    Start-Sleep -Milliseconds 400
}
$proc = Get-Process -Id $app.Id -ErrorAction SilentlyContinue
if (-not $proc -or -not [NativeWin]::IsWindowVisible($proc.MainWindowHandle)) {
    throw 'GrassStress window never became visible.'
}
Write-Host 'GrassStress is visible. Letting the first frames settle...'
Start-Sleep -Seconds 8

Write-Host 'Launching OBS...'
$obs = Start-Process -FilePath $obsExe -WorkingDirectory (Split-Path $obsExe) -ArgumentList '--disable-shutdown-check','--minimize-to-tray','--profile','GrassStress','--collection','GrassStress' -PassThru
$ws = $null
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline) {
    try { $ws = Connect-Obs; break } catch { Start-Sleep -Seconds 1 }
}
if (-not $ws) { throw 'Could not connect to OBS websocket.' }

Obs-Request $ws 'SetCurrentProgramScene' @{ sceneName = 'Cinematic' } | Out-Null
Obs-Request $ws 'SetInputSettings' @{
    inputName = 'Grass Window'
    overlay = $true
    inputSettings = @{
        window = 'GrassStress:GrassStressWindow:GrassStress.exe'
        method = 2
        cursor = $false
        client_area = $true
    }
} | Out-Null

Start-Sleep -Seconds 2
Write-Host "Recording $recordSeconds seconds..."
Obs-Request $ws 'StartRecord' $null | Out-Null
Start-Sleep -Seconds $recordSeconds
$stop = Obs-Request $ws 'StopRecord' $null
Write-Host ($stop | ConvertTo-Json -Depth 6)

$deadline = (Get-Date).AddSeconds(20)
$output = $null
while ((Get-Date) -lt $deadline) {
    $files = Get-ChildItem $videoDir -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
    if ($files) { $output = $files[0]; break }
    Start-Sleep -Milliseconds 500
}

try { $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure,'done',[Threading.CancellationToken]::None).Wait() } catch {}
Start-Sleep -Seconds 2
Get-Process obs64,GrassStress -ErrorAction SilentlyContinue | Stop-Process -Force

if ($output) {
    Write-Host "RECORDED: $($output.FullName)  ($([math]::Round($output.Length/1MB,1)) MB)"
} else {
    Write-Host 'Recording finished but no file was found in C:\StressTest\video'
    exit 1
}
