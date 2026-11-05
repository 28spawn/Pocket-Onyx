# custom-fw/tools/Read-Sources.ps1 - the source manifest reader.
#
# custom-fw/sources.txt is the single place that says which modules each build
# target compiles. Every build script (build.ps1, emu/build_dll.ps1,
# emu/run_emu.ps1) reads it through these two functions, so a new module can
# never be silently forgotten by one target.
#
#   . custom-fw\tools\Read-Sources.ps1
#   $m = Read-SourceManifest
#   $files = Get-SourceFiles -Target "emu" -Sections $m      # relative to custom-fw/

function Read-SourceManifest {
    param([string]$Path = "custom-fw\sources.txt")
    $sections = @{}
    $section = ""
    foreach ($line in Get-Content $Path) {
        $t = ($line -split "#", 2)[0].Trim()
        if (-not $t) { continue }
        if ($t -match "^\[(.+)\]$") {
            $section = $Matches[1].ToLower()
            if (-not $sections.ContainsKey($section)) {
                $sections[$section] = @{ include = @(); exclude = @() }
            }
            continue
        }
        if ($t -match "^(include|exclude)\s*=\s*(.*)$") {
            $key = $Matches[1].ToLower()
            $items = @($Matches[2] -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ })
            $sections[$section][$key] = $items
        }
    }
    return $sections
}

function Get-SourceFiles {
    param(
        [string]$Target,
        [hashtable]$Sections,
        [string]$FwRoot = "custom-fw"
    )
    $key = $Target.ToLower()
    if (-not $Sections.ContainsKey($key)) { throw "sources.txt has no target '$Target'" }
    $spec = $Sections[$key]

    $fwAbs = (Resolve-Path $FwRoot).Path
    $all = @(Get-ChildItem "$FwRoot\src" -Recurse -Filter *.c | ForEach-Object {
        $_.FullName.Substring($fwAbs.Length + 1).Replace("\", "/")
    })

    if ($spec.include.Count -gt 0) {
        $out = @()
        foreach ($pat in $spec.include) {
            if ($pat.StartsWith("@")) {
                $out += Get-SourceFiles -Target $pat.Substring(1) -Sections $Sections -FwRoot $FwRoot
            } elseif ($pat.Contains("*") -or $pat.Contains("?")) {
                $hits = @($all | Where-Object { $_ -like $pat })
                if ($hits.Count -eq 0) { throw "sources.txt: '$pat' matches nothing" }
                $out += $hits
            } else {
                $out += $pat
            }
        }
        return @($out | Sort-Object -Unique)
    }

    $excl = @($spec.exclude)
    return @($all | Where-Object { $excl -notcontains $_ } | Sort-Object)
}
