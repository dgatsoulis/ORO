# =============================================================================
# publish_public.ps1 - stage the PUBLIC ORO tree from this (private) repository.
#
# ORO is developed in a private repository that carries CLAUDE.md, the project's
# internal working notes. That file is NOT published. Because it is present in most
# of the private repo's history, the public repository is a SEPARATE repo with its
# own clean history rather than a push of this one - deleting the file would not
# have made it private.
#
# This script stages what the public repo should contain. It takes its file list
# from `git ls-files`, so anything untracked - build output, scratch files, the beta
# staging tree - cannot leak by accident: it is not that the script skips them, it
# is that the script never sees them.
#
#   .\tools\publish_public.ps1 -Target C:\OrbiterDev\ORO-public
#   .\tools\publish_public.ps1 -Target C:\OrbiterDev\ORO-public -WhatIf
#
# After staging, commit and push from the target directory.
# =============================================================================
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Target,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
$Source = Split-Path -Parent $PSScriptRoot     # the repo root, whatever it is called

# Files that exist in the private repo and must NEVER reach the public one.
# Keep this list short and obvious; anything subtle belongs in .gitignore instead.
#
# OroParticlesSprites.cpp.shelved is a complete, designed, never-flown sprite particle
# system, kept here because its header carries the whole design and it may be revisited.
# It is held back (2026-09-12) because it is not in the project, does not build, and in a
# public repo it reads as abandoned code rather than as a shelf - which is the opposite of
# what someone reading this project for the first time needs. The private repo keeps it.
$Private = @('CLAUDE.md', 'HISTORY.md', 'OroParticlesSprites.cpp.shelved')
# HISTORY.md (2026-09-19): the session journals and superseded continuation blocks archived
# out of CLAUDE.md so the notes file stops loading at 40% of a session's context. Same
# audience as CLAUDE.md, same rule: tracked here, never published.

# Directory prefixes held back wholesale. Entries end in '/' and match on the path prefix.
# beta/reports/ carries the internal working documents - triage lists of tester reports and
# the per-effect plans. They are TRACKED (they must not exist only on one disk - the same rule
# that regenerates the client patch file every session), but they are not the project's public
# face: they quote testers by name and describe unreleased design. Move one out of this tree,
# or delete the prefix, to publish it. The testers' screenshots beside them are not tracked at
# all, so this script never sees those.
$PrivateDirs = @('beta/reports/')

function Test-Private([string]$rel) {
    if ($Private -contains $rel) { return $true }
    foreach ($d in $PrivateDirs) { if ($rel.StartsWith($d, 'OrdinalIgnoreCase')) { return $true } }
    return $false
}

Write-Host ""
Write-Host "ORO public staging" -ForegroundColor Cyan
Write-Host "  source : $Source"
Write-Host "  target : $Target"
if ($WhatIf) { Write-Host "  MODE   : dry run, nothing written" -ForegroundColor Yellow }
Write-Host ""

Push-Location $Source
try   { $tracked = @(git ls-files) }
finally { Pop-Location }

if ($tracked.Count -eq 0) { throw "git ls-files returned nothing - is $Source a git repository?" }

$publish = @($tracked | Where-Object { -not (Test-Private $_) })
$held    = @($tracked | Where-Object {      (Test-Private $_) })

# A private file that is NOT in the tracked list means this script's exclusion list has
# drifted from reality - fail loudly rather than quietly publishing everything.
foreach ($p in $Private) {
    if ($tracked -notcontains $p) {
        Write-Host "  NOTE: '$p' is not tracked in the source repo - exclusion had no effect." -ForegroundColor Yellow
    }
}
foreach ($d in $PrivateDirs) {
    if (-not ($tracked | Where-Object { $_.StartsWith($d, 'OrdinalIgnoreCase') })) {
        Write-Host "  NOTE: no tracked file under '$d' - exclusion had no effect." -ForegroundColor Yellow
    }
}

Write-Host ("  {0} tracked file(s): {1} to publish, {2} held back" -f $tracked.Count, $publish.Count, $held.Count)
foreach ($h in $held) { Write-Host "  HELD BACK: $h" -ForegroundColor Magenta }
Write-Host ""

if (-not $WhatIf) {
    if (-not (Test-Path $Target)) { New-Item -ItemType Directory -Path $Target -Force | Out-Null }

    # Clear previously staged content, but never the target's own .git directory.
    Get-ChildItem -Path $Target -Force | Where-Object { $_.Name -ne '.git' } |
        Remove-Item -Recurse -Force -Confirm:$false

    foreach ($rel in $publish) {
        $src = Join-Path $Source $rel
        $dst = Join-Path $Target $rel
        $dir = Split-Path -Parent $dst
        if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        Copy-Item -LiteralPath $src -Destination $dst -Force
    }

    # Belt and braces: make the PUBLIC repo ignore the private files as well as not
    # having them. Staging is careful, but a future `git add .` run by hand in the
    # target should not be able to commit CLAUDE.md even if someone copies it there.
    # This is appended here rather than kept in the source .gitignore because in the
    # source repo CLAUDE.md is tracked, and gitignore does not apply to tracked files.
    $gi = Join-Path $Target '.gitignore'
    $stanza = @(
        ''
        '# Private to the development repository - never published. See'
        '# tools/publish_public.ps1, which is what stages this tree.'
    ) + $Private + $PrivateDirs
    Add-Content -Path $gi -Value ($stanza -join "`n") -Encoding utf8
}

# ---- verification. Prove the exclusion RAN; do not infer it from the absence of an
# ---- error. A check that can pass because nothing happened is not a check.
Write-Host "Verification" -ForegroundColor Cyan
$fail = $false

if (-not $WhatIf) {
    $staged = @(Get-ChildItem -Path $Target -Recurse -File -Force |
                Where-Object { $_.FullName -notmatch '\\\.git\\' })
    Write-Host ("  staged files on disk : {0} (expected {1})" -f $staged.Count, $publish.Count)
    if ($staged.Count -ne $publish.Count) { Write-Host "  MISMATCH" -ForegroundColor Red; $fail = $true }

    foreach ($p in $Private) {
        if (Test-Path (Join-Path $Target $p)) {
            Write-Host "  LEAKED: $p is present in the target" -ForegroundColor Red; $fail = $true
        } else {
            Write-Host "  confirmed absent     : $p" -ForegroundColor Green
        }
    }

    # Positive spot-check: something that MUST be there. If the copy silently did
    # nothing, this catches it - "CLAUDE.md is absent" would otherwise pass trivially.
    foreach ($must in @('README.md','LICENSE','COPYING.LESSER','OroModule.cpp')) {
        if (Test-Path (Join-Path $Target $must)) {
            Write-Host "  confirmed present    : $must" -ForegroundColor Green
        } else {
            Write-Host "  MISSING: $must" -ForegroundColor Red; $fail = $true
        }
    }
}

Write-Host ""
if ($fail) { Write-Host "STAGING FAILED" -ForegroundColor Red; exit 1 }
Write-Host ("Staging OK{0}." -f $(if ($WhatIf) { " (dry run)" } else { "" })) -ForegroundColor Green
Write-Host ""
