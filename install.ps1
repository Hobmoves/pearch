# pearch installer for Windows
#
#   irm https://raw.githubusercontent.com/Hobmoves/pearch/main/install.ps1 | iex
#
# Installs pearch.exe for the current user (no admin needed), adds it to PATH,
# creates a Start Menu shortcut with the Ctrl+Alt+Space hotkey and an
# Explorer "Search with pearch" menu entry, then shows a quick tour.
#
# Optional environment variables:
#   PEARCH_VERSION   = v1.0.0   install a specific release instead of the latest
#   PEARCH_NO_LAUNCH = 1        don't offer to open pearch at the end
#   PEARCH_REPO      = owner/repo  install from a fork

& {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is much faster without the progress bar
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    } catch {}

    $Repo = if ($env:PEARCH_REPO) { $env:PEARCH_REPO } else { 'Hobmoves/pearch' }
    $Asset = 'pearch-windows-x64.exe'
    $Dir = Join-Path $env:LOCALAPPDATA 'Programs\pearch'
    $Exe = Join-Path $Dir 'pearch.exe'
    $Work = Join-Path ([IO.Path]::GetTempPath()) ('pearch-install-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $Work | Out-Null
    $Download = Join-Path $Work $Asset

    function Write-Step([string]$Text) {
        Write-Host '  > ' -ForegroundColor Green -NoNewline
        Write-Host $Text
    }
    function Write-Note([string]$Text) { Write-Host "    $Text" -ForegroundColor DarkGray }

    Write-Host ''
    Write-Host '  p e a r c h' -ForegroundColor Yellow -NoNewline
    Write-Host '   installing instant file search' -ForegroundColor Gray
    Write-Host ''

    $got = $false

    # 1. Running from a clone (.\install.ps1): build that source.
    $fromClone = $PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot 'CMakeLists.txt'))

    function Build-FromSource([string]$Source) {
        if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { return $false }
        Write-Step "Building from source (this takes a minute)"
        Push-Location $Source
        $ErrorActionPreference = 'Continue'  # build tools write progress to stderr
        try {
            cmake --preset release -DPEARCH_BUILD_TESTS=OFF -DPEARCH_BUILD_BENCH=OFF | Out-Null
            if ($LASTEXITCODE -ne 0) { return $false }
            cmake --build --preset release --parallel | Out-Null
            if ($LASTEXITCODE -ne 0) { return $false }
            $built = Get-ChildItem -Path (Join-Path $Source 'build\release') -Recurse -Filter pearch.exe |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if (-not $built) { return $false }
            Copy-Item $built.FullName $Download -Force
            return $true
        } finally {
            Pop-Location
        }
    }

    if ($fromClone) { $got = Build-FromSource $PSScriptRoot }

    # 2. The release download (public repositories).
    if (-not $got) {
        $url = if ($env:PEARCH_VERSION) {
            "https://github.com/$Repo/releases/download/$($env:PEARCH_VERSION)/$Asset"
        } else {
            "https://github.com/$Repo/releases/latest/download/$Asset"
        }
        Write-Step "Downloading $Asset"
        try {
            Invoke-WebRequest -Uri $url -OutFile $Download -UseBasicParsing
            $got = (Test-Path $Download) -and ((Get-Item $Download).Length -gt 100KB)
        } catch {
            Write-Note "No download at $url"
        }
    }

    # 3. GitHub CLI, which also works for private repositories.
    if (-not $got -and (Get-Command gh -ErrorAction SilentlyContinue)) {
        Write-Step 'Trying the GitHub CLI'
        $ghArgs = @('release', 'download', '--repo', $Repo, '--pattern', $Asset, '--dir', $Work, '--clobber')
        if ($env:PEARCH_VERSION) { $ghArgs = @('release', 'download', $env:PEARCH_VERSION) + $ghArgs[2..($ghArgs.Length - 1)] }
        $ErrorActionPreference = 'Continue'  # Windows PowerShell treats native stderr as errors
        & gh @ghArgs 2>$null | Out-Null
        $ErrorActionPreference = 'Stop'
        $got = Test-Path $Download
    }

    # 4. Build the latest source.
    if (-not $got -and (Get-Command git -ErrorAction SilentlyContinue)) {
        $src = Join-Path $Work 'src'
        $ErrorActionPreference = 'Continue'
        git clone --depth 1 "https://github.com/$Repo.git" $src 2>$null | Out-Null
        $cloned = $LASTEXITCODE -eq 0
        $ErrorActionPreference = 'Stop'
        if ($cloned) { $got = Build-FromSource $src }
    }

    if (-not $got) {
        Write-Host ''
        Write-Host "  Couldn't get pearch.exe." -ForegroundColor Red
        Write-Note "No release was found at github.com/$Repo/releases, and building from source"
        Write-Note "needs git, CMake and Visual Studio (with the C++ workload)."
        throw 'pearch was not installed.'
    }

    # Install.
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    Get-Process pearch -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe } | Stop-Process -Force
    Copy-Item $Download $Exe -Force
    Unblock-File $Exe -ErrorAction SilentlyContinue
    Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue
    Write-Step "Installed to $Dir"

    # PATH, Start Menu shortcut + Ctrl+Alt+Space, Explorer menu (all per-user).
    & $Exe install
    if (($env:Path -split ';') -notcontains $Dir) { $env:Path = "$env:Path;$Dir" }  # usable in this window too

    # The tour.
    & $Exe tour

    if (-not $env:PEARCH_NO_LAUNCH) {
        Write-Host '  Press ' -NoNewline -ForegroundColor Gray
        Write-Host 'Enter' -NoNewline -ForegroundColor Green
        Write-Host ' to open pearch now (it indexes your files the first time), or Ctrl+C to finish. ' -NoNewline -ForegroundColor Gray
        $null = Read-Host
        & $Exe
    }
}
