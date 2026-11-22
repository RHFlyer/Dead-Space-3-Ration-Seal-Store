<#
.SYNOPSIS
  Builds the release zip of one DS3 Mod Loader add-on, locally. It never uploads or publishes anything.

.DESCRIPTION
  For a mod repo laid out like this (the folder name is the mod's folder in the game's plugins folder):

    <Folder>\mod.ini          the manifest: name=, id=, version=, min_loader=, description=, file= for a DLL mod
    <Folder>\*.cpp            the source (a DLL mod), built by CMakeLists.txt into build\plugins\<Folder>\
    CHANGELOG.txt             one entry per version, newest first
    LICENSE.txt
    README.md                 must name the DS3 Mod Loader's /releases/latest and min_loader in its first 15 lines
    ds3mod.h, ds3mod.h.pin    the loader's SDK header, pinned to the loader release named in min_loader= (DLL mods)
    docs\zip-readme-notes.txt optional: extra paragraphs for the README.txt in the zip

  makes, in release\:

    <Folder>-v<version>.zip   one folder, plugins\<Folder>\, with mod.ini, the built DLL, README.txt (written here),
                              CHANGELOG.txt, LICENSE.txt and SHA256SUMS.txt. Players unzip it into the game folder.
    <Folder>-v<version>-SHA256SUMS.txt   the zip's checksum, for the download page

  The zip never holds the player's settings file, a log or a state file, so unzipping a new version over an old one
  keeps the player's switch and settings (the DS3 Mod Loader's sdk/README.md, "Releasing and updating your mod").

  -CheckOnly runs only the checks (no build, no zip). The build check on GitHub runs it on every push.

.EXAMPLE
  From the repo folder, in PowerShell:
  & .\scripts\package-mod.ps1
  & .\scripts\package-mod.ps1 -CheckOnly
#>
param(
    [string]$Folder = '',              # default: the one top-level folder that has a mod.ini
    [switch]$CheckOnly,
    [switch]$SkipBuild,
    [string]$RepoUrl = 'https://github.com/RHFlyer/Dead-Space-3-Ration-Seal-Store',   # this mod's repo; filled in when the repo is made
    [string]$LoaderUrl = 'https://github.com/RHFlyer/Dead-Space-3-Mod-Loader'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'

foreach ($u in @($RepoUrl, $LoaderUrl)) {
    if (-not $u.StartsWith('https://')) { throw "a repo address was not filled in ($u): pass -RepoUrl / -LoaderUrl" }
}

if (-not $Folder) {
    $found = @(Get-ChildItem -Directory -LiteralPath $repo | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'mod.ini') })
    if ($found.Count -ne 1) { throw "pass -Folder: $($found.Count) top-level folders have a mod.ini" }
    $Folder = $found[0].Name
}
$modDir = Join-Path $repo $Folder

# ---- Checks ----
function Read-Manifest([string]$path) {
    $m = @{}
    $inMod = $false
    foreach ($line in Get-Content -LiteralPath $path) {
        if ($line -match '^\s*\[(.+)\]') { $inMod = $Matches[1] -ieq 'mod'; continue }
        if ($inMod -and $line -match '^\s*([a-z_]+)\s*=\s*(.*)$') { $m[$Matches[1]] = $Matches[2].Trim() }
    }
    foreach ($k in 'name', 'id', 'version', 'min_loader', 'description') {
        if (-not $m[$k]) { throw "$path has no $k= in [mod]" }
    }
    if ($m.version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$') { throw "${path}: version=$($m.version) is not three numbers like 1.2.0" }
    if ($m.min_loader -notmatch '^\d+\.\d+\.\d+$') { throw "${path}: min_loader=$($m.min_loader) is not three numbers like 1.0.0" }
    # The loader keeps 159 characters of a description; the Mods panel would cut a longer one mid-word.
    if ($m.description.Length -gt 159) { throw "${path}: description= is $($m.description.Length) characters; the Mods panel shows 159" }
    return $m
}

# A file's git blob id with its line ends as git stores them (LF), so a Windows checkout (CRLF) gives the same id.
function Get-BlobId([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $lf = New-Object Collections.Generic.List[byte]
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 13 -and $i + 1 -lt $bytes.Length -and $bytes[$i + 1] -eq 10) { continue }
        $lf.Add($bytes[$i])
    }
    $head = [Text.Encoding]::ASCII.GetBytes("blob $($lf.Count)`0")
    $sha1 = [Security.Cryptography.SHA1]::Create()
    try { $hash = $sha1.ComputeHash([byte[]]($head + $lf.ToArray())) } finally { $sha1.Dispose() }
    return -join ($hash | ForEach-Object { $_.ToString('x2') })
}

$m = Read-Manifest (Join-Path $modDir 'mod.ini')
$isDll = [bool]$m.file

# The settings file (ini=) and state files are the player's: never in the repo's mod folder, never in the zip.
$stray = @(Get-ChildItem -LiteralPath $modDir -File | Where-Object { $_.Extension -eq '.ini' -and $_.Name -ne 'mod.ini' })
if ($stray) { throw "settings files in ${Folder}: $($stray.Name -join ', ') (the player's, never shipped)" }

$changelog = Join-Path $repo 'CHANGELOG.txt'
if (-not (Test-Path -LiteralPath $changelog -PathType Leaf)) { throw 'missing: CHANGELOG.txt (one entry per version, newest first)' }
$first = Get-Content -LiteralPath $changelog | Where-Object { $_ -match '^\d' } | Select-Object -First 1
if (-not $first -or $first -notmatch "^$([regex]::Escape($m.version))(\s|$)") {
    throw "CHANGELOG.txt: the newest entry is '$first', not $($m.version) (mod.ini version=)"
}

if ($isDll) {
    $pinFile = Join-Path $repo 'ds3mod.h.pin'
    $pin = @{}
    foreach ($line in Get-Content -LiteralPath $pinFile) {
        if ($line -match '^\s*([a-z]+)\s*=\s*(.*)$') { $pin[$Matches[1]] = $Matches[2].Trim() }
    }
    $blob = Get-BlobId (Join-Path $repo 'ds3mod.h')
    if ($blob -ne $pin.blob) { throw "ds3mod.h is not the pinned copy (blob $blob, ds3mod.h.pin says $($pin.blob)). Don't edit it: copy a loader release's header and change the pin." }
    if ($pin.loader -ne $m.min_loader) { throw "ds3mod.h.pin is from loader $($pin.loader), but mod.ini min_loader=$($m.min_loader): build against the oldest loader you support" }
    $api = if ((Get-Content -Raw -LiteralPath (Join-Path $repo 'ds3mod.h')) -match '#define DS3MOD_API_VERSION (\d+)') { $Matches[1] } else { '?' }
    if ($api -ne $pin.api) { throw "ds3mod.h says API $api, ds3mod.h.pin says api=$($pin.api)" }
}

# The README tells players that the mod needs the DS3 Mod Loader, which version, and where to get it.
$readmeTop = (Get-Content -LiteralPath (Join-Path $repo 'README.md') -TotalCount 15) -join "`n"
if (-not $readmeTop.Contains("$LoaderUrl/releases/latest") -or -not $readmeTop.Contains($m.min_loader)) {
    throw "README.md: its first 15 lines must link $LoaderUrl/releases/latest and name loader $($m.min_loader) (mod.ini min_loader=)"
}

Write-Output "Checks passed: $($m.name) $($m.version) (folder $Folder, id $($m.id)), needs DS3 Mod Loader $($m.min_loader)."
if ($CheckOnly) { return }

# ---- Build ----
if ($isDll -and -not $SkipBuild) {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) { $cmake = 'C:\Program Files\CMake\bin\cmake.exe' }
    & $cmake -S $repo -B $build -A Win32 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
    & $cmake --build $build --config Release
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

# ---- Stage ----
$releaseDir = Join-Path $repo 'release'
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
$base = "$Folder-v$($m.version)"
$stage = Join-Path $releaseDir $base
if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
$dir = Join-Path $stage "plugins\$Folder"
New-Item -ItemType Directory -Force -Path $dir | Out-Null

# A text file with Windows line endings, so Notepad on any Windows shows it right.
function Put-Text([string]$text, [string]$name) {
    [IO.File]::WriteAllText((Join-Path $dir $name), $text.Replace("`r`n", "`n").Replace("`n", "`r`n"))
}
function Format-Wrapped([string]$text, [int]$width = 115) {
    $out = @(); $line = ''
    foreach ($w in $text -split '\s+') {
        if ($line -and ($line.Length + 1 + $w.Length) -gt $width) { $out += $line; $line = $w }
        elseif ($line) { $line += " $w" } else { $line = $w }
    }
    if ($line) { $out += $line }
    return $out -join "`n"
}

Copy-Item -LiteralPath (Join-Path $modDir 'mod.ini') -Destination (Join-Path $dir 'mod.ini')
if ($isDll) {
    $dll = Join-Path $build "plugins\$Folder\$($m.file)"
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw "missing: $dll (build first, or drop -SkipBuild)" }
    Copy-Item -LiteralPath $dll -Destination (Join-Path $dir $m.file)
}
Put-Text (Get-Content -Raw -LiteralPath $changelog) 'CHANGELOG.txt'
Put-Text (Get-Content -Raw -LiteralPath (Join-Path $repo 'LICENSE.txt')) 'LICENSE.txt'

$title = "$($m.name) $($m.version), an add-on for the DS3 Mod Loader"
$notesFile = Join-Path $repo 'docs\zip-readme-notes.txt'
$notes = if (Test-Path -LiteralPath $notesFile) { "`n" + (Get-Content -Raw -LiteralPath $notesFile).Trim() + "`n" } else { '' }
$by = if ($m.author) { ", by $($m.author)" } else { '' }
# The mod's name and folder make this paragraph's length vary, so it is wrapped like the description.
$install = Format-Wrapped ("Install: close the game and unzip this download into the game folder, next to deadspace3.exe, " +
    "so that you get plugins\$Folder\mod.ini. Start the game, pause, choose `"mods`" and switch `"$($m.name)`" on " +
    "(new mods start off), then restart the game: a newly switched-on mod starts working after a restart.")
Put-Text @"
$title
$('=' * $title.Length)

Made for the DS3 Mod Loader, and needs it: version $($m.min_loader) or later, installed in the game folder
(dinput8.dll next to deadspace3.exe). Without the loader this folder does nothing. Get the loader here:
$LoaderUrl/releases/latest

$(Format-Wrapped $m.description)
$notes
$install

Update: close the game, unzip the new version into the game folder over the old one, and let Windows replace the
files. Your on/off switch and your settings are kept: the download never holds them. CHANGELOG.txt says what
changed. A copy from the DS3 Mod Loader's add-ons download is the same mod: unzipping this over it updates it.

Remove: delete plugins\$Folder.

Its page, with newer versions, the source and where to report bugs:
$RepoUrl
MIT licence (LICENSE.txt)$by.

"@ 'README.txt'

# ---- Check what is staged ----
$files = @(Get-ChildItem -Recurse -File -LiteralPath $stage)
$bad = $files | Where-Object {
    $_.Name -match '\.(log|sav|dmp|bin|exe|pdb|ilk|obj)$' -or ($_.Extension -eq '.ini' -and $_.Name -ne 'mod.ini') -or
    -not $_.FullName.StartsWith("$dir\")
}
if ($bad) { throw "refusing to package: $($bad.FullName -join ', ')" }
foreach ($f in $files | Where-Object { $_.Extension -in '.dll', '.asi' }) {
    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x14C) { throw "$($f.Name) is not a 32-bit DLL" }
}

# SHA256SUMS.txt: every other file, by its path inside plugins\<Folder>\. \n line ends, so sha256sum -c works too.
$sums = foreach ($f in $files | Sort-Object FullName) {
    '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash.ToLower(), $f.FullName.Substring($dir.Length + 1).Replace('\', '/')
}
[IO.File]::WriteAllText((Join-Path $dir 'SHA256SUMS.txt'), (($sums -join "`n") + "`n"))

# ---- Zip: / in entry names, one fixed date on every entry, entries sorted, so the same files make the same zip ----
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = "$stage.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
$zipDate = [DateTimeOffset]::new(2026, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem -Recurse -File -LiteralPath $stage | Sort-Object FullName) {
        $entry = $archive.CreateEntry($f.FullName.Substring($stage.Length + 1).Replace('\', '/'),
                                      [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = $zipDate
        $out = $entry.Open()
        $in = [IO.File]::OpenRead($f.FullName)
        try { $in.CopyTo($out) } finally { $in.Dispose(); $out.Dispose() }
    }
} finally { $archive.Dispose() }

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLower()
$sumsFile = Join-Path $releaseDir "$base-SHA256SUMS.txt"
[IO.File]::WriteAllText($sumsFile, "$hash  $base.zip`n")
Write-Output "Made $zip ($((Get-ChildItem -Recurse -File -LiteralPath $stage).Count) files)"
Write-Output "SHA256 $hash  $base.zip"
Write-Output 'Nothing was uploaded. Test the zip in a copy of the game folder before you share it.'
