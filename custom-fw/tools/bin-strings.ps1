# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
#Requires -Version 5.1
<#
.SYNOPSIS
    ASCII/UTF-16 string extractor for raw firmware binaries (pi32v2, UFW payloads, blobs).

.DESCRIPTION
    app.bin is a raw image with no ELF structure, so tools like `readelf`/`objdump -s` do not
    apply. This prints every printable run with its file offset and virtual address, optionally
    filtered by regex — the same technique that produced the string inspection in
    documentation.

    On the Pocket Amp the XIP flash base is 0x02000000, so VA = offset + 0x02000000. Confirm against
    the app_area_head entry point if your build differs (0x02000120 in V105).

.PARAMETER Path
    Binary to scan.

.PARAMETER Pattern
    Optional regex; only matching strings are printed. Omit to dump everything.

.PARAMETER MinLength
    Shortest run to report. Default 4.

.PARAMETER Base
    Virtual-address base. Default 0x02000000. Pass 0 to report offsets only.

.EXAMPLE
    .\bin-strings.ps1 -Path .\build_pi32\app.bin
    .\bin-strings.ps1 -Path .\app.bin -Pattern 'adc|codec|input|capture'
    .\bin-strings.ps1 -Path .\app.bin -Pattern 'PATCHEND|@M3|LMT' -MinLength 3

.NOTES
    Filter the STRING pool directly from binary image.


#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Path,
    [string]$Pattern,
    [int]$MinLength = 4,
    [uint64]$Base = 0x02000000
)

$ErrorActionPreference = 'Stop'
$bytes = [System.IO.File]::ReadAllBytes($Path)
$rx = if ($Pattern) { [regex]$Pattern } else { $null }

$sb = New-Object System.Text.StringBuilder
$start = 0
$total = 0
$shown = 0

function Flush-Run([int]$from, [int]$to) {
    $text = $sb.ToString()
    if ($text.Length -ge $script:MinLength) {
        $script:total++
        if ($null -eq $script:rx -or $script:rx.IsMatch($text)) {
            $script:shown++
            '{0,10}  0x{1:X8}  {2}' -f $from, ($from + $script:Base), $text
        }
    }
    [void]$sb.Clear()
}

for ($i = 0; $i -lt $bytes.Length; $i++) {
    $b = $bytes[$i]
    if ($b -ge 0x20 -and $b -le 0x7E) {
        if ($sb.Length -eq 0) { $start = $i }
        [void]$sb.Append([char]$b)
    } else {
        if ($sb.Length -gt 0) { Flush-Run $start $i }
    }
}
if ($sb.Length -gt 0) { Flush-Run $start $bytes.Length }

Write-Output ("`n{0}: {1} bytes, {2} strings >= {3} chars, {4} shown" -f `
    (Split-Path -Leaf $Path), $bytes.Length, $total, $MinLength, $shown)