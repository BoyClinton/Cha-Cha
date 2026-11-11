# Brings main's tree onto the public branch `github` as one commit, without the internal docs (user decision
# 2026-11-11: they are never published; the public history was rewritten without them). Pushes nothing:
# `git push origin github:main` is a separate step, asked for every time (docs/HANDOFF.md, Publishing).
# Usage: powershell -File scripts/publish-sync.ps1 -Message "Cha-Cha x.y.z: ..."
param([Parameter(Mandatory = $true)][string]$Message)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $repo

# Internal working docs: notes, rules and plans for the sessions working on Cha-Cha, kept on main only
$internal = @('CLAUDE.md', 'PRODUCT.md', 'docs/HANDOFF.md', 'docs/research-log.md', 'docs/ROADMAP.md')

function Invoke-Git { & git @args; if ($LASTEXITCODE -ne 0) { throw "git $args failed" } }

if ((git branch --show-current) -ne 'main') { throw 'run from main' }
if (git status --porcelain --untracked-files=no) { throw 'commit or stash the changes on main first' }

Invoke-Git checkout -q github
try {
    Invoke-Git checkout main -- .
    # files main no longer has
    $gone = Compare-Object (git ls-tree -r --name-only github) (git ls-tree -r --name-only main) |
        Where-Object SideIndicator -eq '<=' | ForEach-Object InputObject
    foreach ($f in $gone) { Invoke-Git rm -q -- $f }
    # the internal docs leave the index and the working tree (main brings them back on checkout)
    Invoke-Git rm -q -f --ignore-unmatch -- @internal
    $left = git diff --cached --name-only -- @internal
    if ($left) { throw "internal docs still staged: $left" }
    if (-not (git diff --cached --name-only)) { Write-Host 'nothing to publish'; return }
    git diff --cached --stat | Select-Object -Last 1
    Invoke-Git commit -q -m $Message
    git log --oneline -1
} finally {
    Invoke-Git checkout -q main
}
