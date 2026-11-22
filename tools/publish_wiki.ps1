<#
.SYNOPSIS
    Syncs OpenScarlett's canonical wiki/*.md pages to the GitHub Wiki repository
    (https://github.com/californiaAlmonds/OpenScarlett.wiki.git).
.NOTES
    All temporary files are kept strictly inside <repo>\.tmp\wiki_sync (never C:\).
    GitHub only initializes the .wiki.git remote after the first page is created
    in the GitHub UI (Repository -> Wiki -> Create the first page).
#>
param(
    [string]$WikiRemote = "https://github.com/californiaAlmonds/OpenScarlett.wiki.git",
    [string]$CommitMessage = "docs(wiki): sync OpenScarlett wiki from main repo",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$WikiSrc  = Join-Path $RepoRoot "wiki"
$SyncDir  = Join-Path $RepoRoot ".tmp\wiki_sync"

if (-not (Test-Path (Join-Path $WikiSrc "Home.md"))) {
    throw "Wiki source folder not found or missing Home.md at: $WikiSrc"
}

Write-Host "Checking GitHub Wiki remote: $WikiRemote" -ForegroundColor Cyan
$probeOut = & git ls-remote $WikiRemote 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "GitHub Wiki repository is not initialized yet!" -ForegroundColor Yellow
    Write-Host "To enable it once:" -ForegroundColor Yellow
    Write-Host "  1. Open https://github.com/californiaAlmonds/OpenScarlett/wiki" -ForegroundColor White
    Write-Host "  2. Click 'Create the first page' and click 'Save Page' (any placeholder text is fine)." -ForegroundColor White
    Write-Host "  3. Re-run: powershell -ExecutionPolicy Bypass -File .\tools\publish_wiki.ps1" -ForegroundColor White
    exit 1
}

if (Test-Path $SyncDir) {
    Remove-Item -Recurse -Force $SyncDir
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $SyncDir) | Out-Null

& git clone $WikiRemote $SyncDir
if ($LASTEXITCODE -ne 0) {
    throw "Failed to clone $WikiRemote"
}

# Remove stale markdown pages in the wiki clone (keep .git)
Get-ChildItem -Path $SyncDir -Filter "*.md" -File | Remove-Item -Force

# Copy all canonical wiki/*.md files and rewrite relative *.md links to GitHub Wiki slugs
# (GitHub Wiki expects [Title](Page-Name) / [Title](Page-Name#anchor), without the .md suffix).
$pages = Get-ChildItem -Path $WikiSrc -Filter "*.md" -File | Sort-Object Name
foreach ($p in $pages) {
    $content = [System.IO.File]::ReadAllText($p.FullName, [System.Text.Encoding]::UTF8)
    $rewritten = [regex]::Replace(
        $content,
        '\]\((?!https?://)([^)#/]+)\.md(#[^)]*)?\)',
        ']($1$2)'
    )
    [System.IO.File]::WriteAllText(
        (Join-Path $SyncDir $p.Name),
        $rewritten,
        (New-Object System.Text.UTF8Encoding($false))
    )
}

Push-Location $SyncDir
try {
    & git add -A
    $status = & git status --porcelain
    if (-not $status) {
        Write-Host "GitHub Wiki is already up to date ($($pages.Count) pages)." -ForegroundColor Green
        exit 0
    }

    Write-Host "Staged wiki changes ($($pages.Count) pages):" -ForegroundColor Cyan
    & git status -s

    if ($DryRun) {
        Write-Host "DryRun enabled — skipping commit & push." -ForegroundColor Yellow
        exit 0
    }

    & git commit -m $CommitMessage
    if ($LASTEXITCODE -ne 0) { throw "git commit failed" }

    & git push origin HEAD
    if ($LASTEXITCODE -ne 0) { throw "git push failed" }

    Write-Host "Successfully published $($pages.Count) pages to $WikiRemote" -ForegroundColor Green
} finally {
    Pop-Location
}
