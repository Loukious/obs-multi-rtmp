param([string] $MakeNsis = 'makensis.exe')
$ErrorActionPreference = 'Stop'
$Workspace = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$TestRoot = Join-Path $Workspace "build_x64/installer-migration-$([guid]::NewGuid().ToString('N'))"
if (![System.IO.Path]::GetFullPath($TestRoot).StartsWith($Workspace + [System.IO.Path]::DirectorySeparatorChar)) {
    throw 'Migration fixtures must stay inside the workspace'
}
function Fixture([string] $RelativePath) {
    $FilePath = Join-Path $TestRoot $RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path $FilePath) | Out-Null
    [System.IO.File]::WriteAllText($FilePath, 'fixture')
}
$Removed = @(
    'ProgramData plugins/obs-multi-rtmp/bin/64bit/obs-multi-rtmp.dll',
    'ProgramData plugins/obs-multi-rtmp/bin/64bit/obs-multi-rtmp.pdb',
    'Custom OBS/obs-plugins/64bit/obs-multi-rtmp.dll',
    'Custom OBS/obs-plugins/64bit/obs-multi-rtmp.pdb',
    'Custom OBS/data/obs-plugins/obs-multi-rtmp/locale/en-US.ini'
)
$Preserved = @(
    'ProgramData plugins/obs-multi-rtmp/obs-multi-rtmp.dll',
    'ProgramData plugins/obs-multi-rtmp/data/locale/en-US.ini',
    'ProgramData plugins/other-plugin/other-plugin.dll',
    'ProgramData plugins/obs-multi-rtmp/bin/64bit/unrelated.dll',
    'Custom OBS/bin/64bit/obs64.exe',
    'Custom OBS/obs-plugins/64bit/other-plugin.dll',
    'Custom OBS/data/obs-plugins/other-plugin/locale/en-US.ini',
    'Custom OBS/config/obs-studio/plugin_config/obs-multi-rtmp/cookies_fixture.json',
    'Invalid OBS/obs-plugins/64bit/obs-multi-rtmp.dll',
    'Locked OBS/bin/64bit/obs64.exe',
    'Locked OBS/obs-plugins/64bit/obs-multi-rtmp.dll',
    'Locked OBS/data/obs-plugins/obs-multi-rtmp/locale/en-US.ini'
)
foreach ($Path in ($Removed + $Preserved)) { Fixture $Path }
$TestExe = Join-Path $TestRoot 'migration.exe'
& $MakeNsis '/V2' "/DTEST_ROOT=$TestRoot" "/DTEST_OUTPUT=$TestExe" "$PSScriptRoot/windows-installer-migration-test.nsi"
if ($LASTEXITCODE) { throw 'Migration test installer compilation failed' }
$Locked = [System.IO.File]::Open((Join-Path $TestRoot 'Locked OBS/obs-plugins/64bit/obs-multi-rtmp.dll'),
    [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
try {
    $Process = Start-Process -FilePath $TestExe -WindowStyle Hidden -PassThru -Wait
    if ($Process.ExitCode) { throw 'Migration test installer failed' }
} finally { $Locked.Dispose() }
foreach ($Path in $Removed) {
    if (Test-Path -LiteralPath (Join-Path $TestRoot $Path)) { throw "Legacy file was not removed: $Path" }
}
foreach ($Path in $Preserved) {
    if (!(Test-Path -LiteralPath (Join-Path $TestRoot $Path))) { throw "Unrelated or locked file was removed: $Path" }
}
$Report = Get-Content -LiteralPath (Join-Path $TestRoot 'result.ini') -Raw
if ($Report -notmatch '(?s)\[Migration\]\s*Failed=\s*\[Locked\]\s*Failed=1') {
    throw "Unexpected migration result: $Report"
}
Write-Output 'PASS: legacy migration, repeat installation, unrelated files, user settings and locked-file handling'
