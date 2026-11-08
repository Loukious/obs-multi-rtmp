param([Parameter(Mandatory)][string] $PackageDirectory)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Spec = Get-Content -LiteralPath "$PSScriptRoot/../buildspec.json" -Raw | ConvertFrom-Json
$Stem = "$($Spec.name)-$($Spec.version)-windows-x64"
foreach ($Legacy in @($false, $true)) {
    $Suffix = if ($Legacy) { '-OBS32' } else { '' }
    $Archive = [System.IO.Compression.ZipFile]::OpenRead((Join-Path $PackageDirectory "$Stem$Suffix.zip"))
    try {
        $BinaryDirectory = if ($Legacy) { "$($Spec.name)/bin/64bit" } else { $Spec.name }
        foreach ($Required in @("$BinaryDirectory/$($Spec.name).dll", "$($Spec.name)/data/locale/en-US.ini")) {
            $Entry = $Archive.GetEntry($Required)
            if (!$Entry -or !$Entry.Length) { throw "Missing or empty package entry: $Required" }
        }
        foreach ($Entry in $Archive.Entries) {
            if (!$Entry.FullName.StartsWith("$($Spec.name)/") -or $Entry.FullName.Contains('..')) {
                throw "Unexpected package path: $($Entry.FullName)"
            }
            if (!$Legacy -and $Entry.FullName.StartsWith("$($Spec.name)/bin/")) {
                throw "Legacy binary directory in standard package: $($Entry.FullName)"
            }
        }
    } finally { $Archive.Dispose() }
}
$Installer = [System.IO.File]::ReadAllBytes((Join-Path $PackageDirectory "$Stem-Installer.exe"))
if ($Installer.Length -lt 1024 -or $Installer[0] -ne 0x4d -or $Installer[1] -ne 0x5a -or
    !([System.Text.Encoding]::ASCII.GetString($Installer).Contains('NullsoftInst'))) {
    throw 'Missing or invalid NSIS installer'
}
Write-Output 'PASS: modern Windows layout, OBS 32 compatibility archive and NSIS installer'
