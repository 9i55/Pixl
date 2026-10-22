<#
Publishes a new Pixl version. Everyone running Pixl sees "Update to <version>" in the title bar
within a few hours (or the next time they open it) and can install it with one click.

  .\tools\release.ps1 -Version 1.2.0 -Notes "What changed in this version"

It sets the version in src\version.hpp, builds, makes Pixl.zip plus Pixl.exe and its SHA-256
(the app checks the download against it), commits, tags and creates the GitHub release.
Needs Visual Studio 2022 (C++), git and the GitHub CLI (gh) signed in to the repo's account.
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Notes = "",
    [switch]$Draft
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must look like 1.2.0" }
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$parts = $Version.Split('.')
$tag = "v$Version"

if (git tag --list $tag) { throw "$tag already exists" }
if (git status --porcelain) { Write-Host "Committing the changes in the working tree with the release." }

# 1. Version
$v = Get-Content src\version.hpp -Raw
$v = $v -replace '(#define PIXL_VERSION_MAJOR )\d+', "`${1}$($parts[0])"
$v = $v -replace '(#define PIXL_VERSION_MINOR )\d+', "`${1}$($parts[1])"
$v = $v -replace '(#define PIXL_VERSION_PATCH )\d+', "`${1}$($parts[2])"
$v = $v -replace '(#define PIXL_VERSION_STR )"[^"]*"', "`${1}`"$Version`""
$v = $v -replace '(#define PIXL_VERSION_RC )[\d,]+', "`${1}$($parts[0]),$($parts[1]),$($parts[2]),0"
Set-Content src\version.hpp $v -NoNewline -Encoding ascii

# 2. Build
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
& $msbuild Pixl.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
$exe = Join-Path $root 'build\MasonEAX\Pixl.exe'
$built = (Get-Item $exe).VersionInfo.ProductVersion
if ($built -ne $Version) { throw "Built exe says $built, expected $Version" }

# 3. Files for the release
$out = Join-Path $root 'build\release'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $out | Out-Null
Copy-Item $exe "$out\Pixl.exe"
Copy-Item LICENSE "$out\LICENSE.txt"
Copy-Item NOTICE-OctopusEAX.txt "$out\NOTICE-OctopusEAX.txt"  # the original MIT notice must ship with every copy
$hash = (Get-FileHash "$out\Pixl.exe" -Algorithm SHA256).Hash.ToLower()
Set-Content "$out\Pixl.exe.sha256" "$hash  Pixl.exe" -Encoding ascii
Compress-Archive -Path "$out\Pixl.exe", "$out\LICENSE.txt", "$out\NOTICE-OctopusEAX.txt" -DestinationPath "$out\Pixl.zip" -Force

# 4. Commit, tag, push, release
# git and gh print notes on stderr; Windows PowerShell would treat them as errors, so check exit codes instead.
$ErrorActionPreference = 'Continue'
function Step([string]$what, [scriptblock]$run) {
    & $run 2>&1 | ForEach-Object { "$_" } | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$what failed" }
}
git add -A 2>&1 | Out-Null
git diff --cached --quiet
if ($LASTEXITCODE -ne 0) { Step 'git commit' { git commit -m "Pixl $Version" } }
Step 'git tag' { git tag $tag }
Step 'git push' { git push origin HEAD --tags }
if (-not $Notes) { $Notes = "Pixl $Version" }
$body = "$Notes`n`n**New here?** Download ``Pixl.zip``, unzip it into a folder that stays, and run ``Pixl.exe``.`n**Already have Pixl 1.1 or newer?** Click **Update to $Version** at the top of Pixl."
$ghArgs = @('release', 'create', $tag, "$out\Pixl.zip", "$out\Pixl.exe", "$out\Pixl.exe.sha256", '--title', "Pixl $Version", '--notes', $body)
if ($Draft) { $ghArgs += '--draft' } else { $ghArgs += '--latest' }
Step 'gh release create' { gh @ghArgs }
Write-Host "Released Pixl $Version"
