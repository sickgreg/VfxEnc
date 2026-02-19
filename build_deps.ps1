param(
    [Parameter(Mandatory = $true)]
    [string]$DepsDir,
    [Parameter(Mandatory = $true)]
    [string]$MpvDevDir,
    [Parameter(Mandatory = $true)]
    [string]$MpvRtDir,
    [Parameter(Mandatory = $true)]
    [string]$FfmpegDir
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Normalize-SingleTopDir([string]$dest) {
    $dirs = Get-ChildItem -Path $dest -Directory -ErrorAction SilentlyContinue
    $files = Get-ChildItem -Path $dest -File -ErrorAction SilentlyContinue
    if ($dirs.Count -eq 1 -and $files.Count -eq 0) {
        $inner = $dirs[0].FullName
        Get-ChildItem -Path $inner -Force | Move-Item -Destination $dest -Force
        Remove-Item -Recurse -Force $inner
    }
}

function Get-7zipPath {
    $cmd = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $env:ProgramFiles '7-Zip\7z.exe'),
        (Join-Path ${env:ProgramFiles(x86)} '7-Zip\7z.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

function Extract-Archive([string]$archive, [string]$dest) {
    $maxAttempts = 3
    for ($i = 1; $i -le $maxAttempts; $i++) {
        try {
            if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
            New-Item -ItemType Directory -Force -Path $dest | Out-Null
            $ext = [IO.Path]::GetExtension($archive)
            if ($ext -ieq '.zip') {
                Expand-Archive -Path $archive -DestinationPath $dest -Force
            } else {
                $seven = Get-7zipPath
                if (-not $seven) {
                    throw "7z.exe not found to extract $archive"
                }
                & $seven x -y "-o$dest" $archive | Out-Null
            }
            Normalize-SingleTopDir $dest
            return
        } catch {
            if ($i -ge $maxAttempts) { throw }
            Start-Sleep -Seconds 2
        }
    }
}

$dl = Join-Path $DepsDir 'downloads'
New-Item -ItemType Directory -Force -Path $dl, $MpvDevDir, $MpvRtDir, $FfmpegDir | Out-Null

$needMpvDev = -not (Get-ChildItem -Path $MpvDevDir -Recurse -Filter client.h -ErrorAction SilentlyContinue | Select-Object -First 1) -or
              -not (Get-ChildItem -Path $MpvDevDir -Recurse -Filter libmpv-2.dll -ErrorAction SilentlyContinue | Select-Object -First 1)
$needMpvRt  = -not (Get-ChildItem -Path $MpvRtDir -Recurse -Filter d3dcompiler_43.dll -ErrorAction SilentlyContinue | Select-Object -First 1)

if ($needMpvDev -or $needMpvRt) {
    Write-Host "[deps] Fetching mpv release metadata..."
    $r = Invoke-RestMethod 'https://api.github.com/repos/shinchiro/mpv-winbuild-cmake/releases/latest'
    $dev = $r.assets | Where-Object { $_.name -match 'mpv-dev-x86_64.*\.(zip|7z)$' } | Sort-Object { if ($_.name -match '\.zip$') { 0 } else { 1 } } | Select-Object -First 1
    $rt  = $r.assets | Where-Object { $_.name -match '^mpv-x86_64.*\.(zip|7z)$' } | Sort-Object { if ($_.name -match '\.zip$') { 0 } else { 1 } } | Select-Object -First 1

    if (-not $dev -or -not $rt) {
        throw 'Could not find mpv assets (dev/runtime) in latest release.'
    }

    if ($needMpvDev) {
        Write-Host "[deps] Downloading mpv dev..."
        $devPath = Join-Path $dl $dev.name
        if (-not (Test-Path $devPath)) { Invoke-WebRequest $dev.browser_download_url -OutFile $devPath }
        Extract-Archive $devPath $MpvDevDir
    }
    if ($needMpvRt) {
        Write-Host "[deps] Downloading mpv runtime..."
        $rtPath = Join-Path $dl $rt.name
        if (-not (Test-Path $rtPath)) { Invoke-WebRequest $rt.browser_download_url -OutFile $rtPath }
        Extract-Archive $rtPath $MpvRtDir
    }
} else {
    Write-Host "[deps] mpv already present."
}

function Test-FfmpegHasLibplacebo([string]$ffmpegExe) {
    if (-not (Test-Path $ffmpegExe)) { return $false }
    try {
        $filters = & $ffmpegExe -hide_banner -filters 2>$null
        return ($filters -match 'libplacebo')
    } catch {
        return $false
    }
}

$ffmpegExe = Get-ChildItem -Path $FfmpegDir -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1
$needFfmpeg = -not $ffmpegExe
if (-not $needFfmpeg) {
    if (-not (Test-FfmpegHasLibplacebo $ffmpegExe.FullName)) {
        Write-Host "[deps] ffmpeg present but missing libplacebo; upgrading to full build..."
        $needFfmpeg = $true
    }
}
if ($needFfmpeg) {
    Write-Host "[deps] Downloading ffmpeg (full build w/ libplacebo)..."
    $ffUrl = 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-full.7z'
    $ffPath = Join-Path $dl 'ffmpeg-release-full.7z'
    if (-not (Test-Path $ffPath)) { Invoke-WebRequest $ffUrl -OutFile $ffPath }
    Extract-Archive $ffPath $FfmpegDir
} else {
    Write-Host "[deps] ffmpeg already present."
}
