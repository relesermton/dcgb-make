# ============================================================================
# dcGCB bootstrap - builds the dcGCB tool itself with ONE compiler call
# (part of the "100x faster" pitch: no configure step, single TU, ~2-3 s)
#
#   powershell -File build.ps1            # build the tool (auto-compiler)
#   powershell -File build.ps1 -Target demo   # also build + run the example
#   powershell -File build.ps1 -Compiler gcc   # force g++
#   powershell -File build.ps1 -Compiler cl    # force MSVC (finds VsDevCmd)
#   powershell -File build.ps1 -Target clean
# ============================================================================
#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('tool', 'demo', 'clean', 'help')]
    [string]$Target = 'tool',
    [ValidateSet('auto', 'gcc', 'cl')]
    [string]$Compiler = 'auto'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = $PSScriptRoot
$binDir = Join-Path $root 'bin'
$srcFile = Join-Path $root 'src\dcgcb.cpp'
$exePath = Join-Path $binDir 'dcgcb.exe'

function Find-VsDevCmd {
    $roots = @("${env:ProgramFiles(x86)}\Microsoft Visual Studio", "${env:ProgramFiles}\Microsoft Visual Studio")
    $hits = @()
    foreach ($r in $roots) {
        if (Test-Path $r) {
            $hits += Get-ChildItem -Path $r -Recurse -Filter VsDevCmd.bat -ErrorAction SilentlyContinue
        }
    }
    return $hits | Sort-Object FullName -Descending | Select-Object -First 1
}

function Invoke-Bootstrap {
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $used = $null

    $wantsGcc = ($Compiler -eq 'gcc') -or ($Compiler -eq 'auto' -and (Get-Command g++ -ErrorAction SilentlyContinue))
    $wantsCl = (-not $wantsGcc) -or ($Compiler -eq 'cl')

    if ($wantsGcc -and (Get-Command g++ -ErrorAction SilentlyContinue)) {
        $used = 'g++'
        Write-Host "[dcg] bootstrap: compiling src\dcgcb.cpp with g++ ..."
        & g++ -std=c++17 -O2 -DNDEBUG -s -static -o $exePath $srcFile
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[dcg] -static unsupported; retrying without it"
            & g++ -std=c++17 -O2 -DNDEBUG -o $exePath $srcFile
            if ($LASTEXITCODE -ne 0) { throw "g++ bootstrap failed" }
        }
    }
    elseif ($wantsCl -and (Find-VsDevCmd)) {
        $vs = Find-VsDevCmd
        $used = 'cl'
        Write-Host "[dcg] bootstrap: compiling with cl (env: $($vs.FullName))"
        & cmd.exe /s /c " `"$($vs.FullName)`" -arch=x64 >nul && cl /nologo /std:c++17 /EHsc /O2 /DNDEBUG /Fe:`"$exePath`" `"$srcFile`" "
        if ($LASTEXITCODE -ne 0) { throw "cl bootstrap failed" }
    }
    else {
        throw "No compiler found. Install WinLibs g++ (winget install BrechtSanders.WinLibs.POSIX.UCRT) or MSVC Build Tools."
    }

    $sw.Stop()
    $size = (Get-Item $exePath).Length
    Write-Host ("[dcg] ok: bin\dcgcb.exe ({0}) in {1:N1} ms via {2}" -f (Format-Human $size), $sw.Elapsed.TotalMilliseconds, $used)
}

function Format-Human([long]$bytes) {
    if ($bytes -ge 1MB) { "{0:N1} MB" -f ($bytes / 1MB) }
    elseif ($bytes -ge 1KB) { "{0:N1} KB" -f ($bytes / 1KB) }
    else { "$bytes B" }
}

function Invoke-Demo {
    Invoke-Bootstrap
    Write-Host "`n[dcg] demo: dcgcb build examples\demo"
    Push-Location $root
    try {
        & $exePath build examples\demo
        if ($LASTEXITCODE -ne 0) { throw "demo generate/build failed" }
        $demoExe = Join-Path $root 'examples\demo\build\Demo.exe'
        Write-Host "`n[dcg] demo output:"
        & $demoExe
        if ($LASTEXITCODE -ne 0) { throw "demo exe failed" }
    }
    finally {
        Pop-Location
    }
    Write-Host "`n[dcg] demo OK - toolchain pipeline (dcGCB + ninja + g++) works end to end."
    Write-Host "      incremental check: re-run 'dcgcb build examples\demo' - ninja should print 'no work to do'."
}

function Invoke-Help {
    Write-Host @'
dcGCB bootstrap usage:
  powershell -File build.ps1                # build tool
  powershell -File build.ps1 -Target demo   # build tool + build/run example
  powershell -File build.ps1 -Compiler gcc|cl
  powershell -File build.ps1 -Target clean

After bootstrap, drive projects with the tool:
  bin\dcgcb.exe init <dir>        scaffold
  bin\dcgcb.exe build <dir>       generate build.ninja + ninja build
  bin\dcgcb.exe generate <dir>    collect + emit only
  bin\dcgcb.exe clean <dir>
'@
}

switch ($Target) {
    'tool' { Invoke-Bootstrap }
    'demo' { Invoke-Demo }
    'clean' {
        if (Test-Path $binDir) { Remove-Item -Recurse -Force $binDir; Write-Host "[dcg] removed $binDir" }
        $demoBuild = Join-Path $root 'examples\demo\build'
        if (Test-Path $demoBuild) { Remove-Item -Recurse -Force $demoBuild; Write-Host "[dcg] removed $demoBuild" }
    }
    'help' { Invoke-Help }
}
