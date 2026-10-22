<#
How many times Pixl was downloaded, per release.

  .\tools\stats.ps1

Pixl.zip  = downloads from the releases page (new people, or someone downloading again)
Pixl.exe  = updates installed with the Update button inside Pixl
These count downloads, not unique people. Needs the GitHub CLI (gh).
#>
$ErrorActionPreference = 'Stop'
$repo = '9i55/Pixl'
$releases = gh api "repos/$repo/releases" | ConvertFrom-Json
$rows = foreach ($r in $releases) {
    $count = { param($name) ($r.assets | Where-Object name -eq $name | Measure-Object download_count -Sum).Sum }
    [pscustomobject]@{
        Version   = $r.tag_name
        Published = ([datetime]$r.published_at).ToString('yyyy-MM-dd')
        Downloads = [int](& $count 'Pixl.zip')
        Updates   = [int](& $count 'Pixl.exe')
    }
}
$rows | Format-Table -AutoSize
"Total downloads (Pixl.zip): {0}" -f ($rows | Measure-Object Downloads -Sum).Sum
"Total updates   (Pixl.exe): {0}" -f ($rows | Measure-Object Updates -Sum).Sum
