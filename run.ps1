param([Parameter(ValueFromRemainingArguments=$true)][string[]]$LaunchArgs)
$ErrorActionPreference = 'Stop'
$binary = Join-Path $PSScriptRoot 'build\bin\GrassStress.exe'
if (-not (Test-Path -LiteralPath $binary)) { & (Join-Path $PSScriptRoot 'build.ps1') }
& $binary @LaunchArgs
