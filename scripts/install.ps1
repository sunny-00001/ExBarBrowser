#requires -Version 5.1
<#
    ExBarBrowser per-user registration.

    Default: adds context-menu verbs only (does NOT change how folders open).
    -Takeover: also makes ExBarBrowser the default handler for folders.
               The previous HKCU\Software\Classes\Folder\shell key is exported
               to backup\Folder-shell.reg first.

    Everything is HKCU, no admin required. Nothing is applied automatically.
#>
[CmdletBinding()]
param([switch]$Takeover)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\ExBarBrowser.exe'
$exe = [System.IO.Path]::GetFullPath($exe)
if (-not (Test-Path -LiteralPath $exe)) { throw "ExBarBrowser.exe not found: $exe" }

function Set-Verb([string]$root, [string]$label, [string]$cmd) {
    New-Item -Path $root -Force | Out-Null
    Set-ItemProperty -Path $root -Name '(default)' -Value $label
    Set-ItemProperty -Path $root -Name 'Icon' -Value "$exe,0"
    New-Item -Path "$root\command" -Force | Out-Null
    Set-ItemProperty -Path "$root\command" -Name '(default)' -Value $cmd
}

Set-Verb 'HKCU:\Software\Classes\Folder\shell\ExBarBrowser' `
         'Open with ExBar' "`"$exe`" `"%1`""
Set-Verb 'HKCU:\Software\Classes\Directory\Background\shell\ExBarBrowser' `
         'Open in ExBar' "`"$exe`" `"%V`""

if ($Takeover) {
    $backupDir = Join-Path $PSScriptRoot '..\backup'
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    $reg = Join-Path $backupDir 'Folder-shell.reg'
    & reg.exe export 'HKCU\Software\Classes\Folder\shell' $reg /y | Out-Null
    Set-ItemProperty -Path 'HKCU:\Software\Classes\Folder\shell' -Name '(default)' -Value 'ExBarBrowser'
    Set-Content -LiteralPath (Join-Path $PSScriptRoot '..\.takeover') -Value '1' -Encoding ascii
    Write-Host "Takeover enabled. Backup: $reg"
}

Write-Host 'Installed per-user verbs.'
