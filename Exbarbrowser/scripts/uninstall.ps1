#requires -Version 5.1
<#
    Remove ExBarBrowser per-user registration.

    - Deletes the two context-menu verbs.
    - If a takeover marker exists, removes the Folder\shell default value,
      which restores the system default ("open") behavior.
    - backup\Folder-shell.reg is kept for manual restore if needed.
#>
$ErrorActionPreference = 'SilentlyContinue'

Remove-Item 'HKCU:\Software\Classes\Folder\shell\ExBarBrowser' -Recurse -Force
Remove-Item 'HKCU:\Software\Classes\Directory\Background\shell\ExBarBrowser' -Recurse -Force

$marker = Join-Path $PSScriptRoot '..\.takeover'
if (Test-Path -LiteralPath $marker) {
    Remove-ItemProperty -Path 'HKCU:\Software\Classes\Folder\shell' -Name '(default)'
    Remove-Item -LiteralPath $marker -Force
    Write-Host 'Takeover removed; system default restored.'
}

Write-Host 'Per-user verbs removed.'
