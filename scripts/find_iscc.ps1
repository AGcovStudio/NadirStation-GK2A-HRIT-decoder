# Prints the full path of ISCC.exe (Inno Setup command-line compiler) or nothing.
$found = $null
# 1) the uninstall entries Inno Setup registers
$keys = @('HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall','HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall','HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')
foreach ($k in $keys) {
  Get-ChildItem $k -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like 'Inno Setup*' } | ForEach-Object {
    $loc = (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).InstallLocation
    if ($loc -and -not $found) { $p = Join-Path $loc 'ISCC.exe'; if (Test-Path $p) { $found = $p } }
  }
}
# 2) usual folders on every fixed drive
if (-not $found) {
  $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, (Join-Path $env:LOCALAPPDATA 'Programs'))
  foreach ($d in Get-PSDrive -PSProvider FileSystem) { $roots += $d.Root; $roots += (Join-Path $d.Root 'Program Files (x86)'); $roots += (Join-Path $d.Root 'Program Files') }
  foreach ($r in $roots) {
    if (-not $r -or $found) { continue }
    Get-ChildItem -LiteralPath $r -Directory -Filter 'Inno Setup*' -ErrorAction SilentlyContinue | ForEach-Object {
      $p = Join-Path $_.FullName 'ISCC.exe'; if (-not $found -and (Test-Path $p)) { $found = $p }
    }
  }
}
if ($found) { Write-Output $found }
