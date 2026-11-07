# Installs a released mmdbio on Windows:
#
#   irm https://raw.githubusercontent.com/IPGeolocation/mmdbio/main/install.ps1 | iex
#
# $env:MMDBIO_VERSION  the version (default: the latest release)
# $env:MMDBIO_DIR      where the binaries go (default %LOCALAPPDATA%\Programs\mmdbio)
# $env:MMDBIO_REPO     the GitHub repository (default IPGeolocation/mmdbio)
#
# The archive is checked against the release's SHA256SUMS, and the folder is
# added to your user PATH. No administrator rights are needed.
$ErrorActionPreference = 'Stop'

$repo = if ($env:MMDBIO_REPO) { $env:MMDBIO_REPO } else { 'IPGeolocation/mmdbio' }
$arch = if ([Environment]::Is64BitOperatingSystem) { 'amd64' } else { '386' }
$version = $env:MMDBIO_VERSION
if (-not $version) {
  $version = (Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest").tag_name
}
$version = $version.TrimStart('v')

$name = "mmdbio-$version-windows-$arch"
$base = "https://github.com/$repo/releases/download/v$version"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
  Write-Host "downloading $name"
  Invoke-WebRequest "$base/$name.zip" -OutFile "$tmp\$name.zip" -UseBasicParsing
  Invoke-WebRequest "$base/SHA256SUMS" -OutFile "$tmp\SHA256SUMS" -UseBasicParsing
  $line = Get-Content "$tmp\SHA256SUMS" | Where-Object { $_ -match " $([regex]::Escape("$name.zip"))$" }
  if (-not $line) { throw "$name.zip is not in SHA256SUMS" }
  $want = ($line -split ' ')[0]
  $got = (Get-FileHash "$tmp\$name.zip" -Algorithm SHA256).Hash.ToLower()
  if ($got -ne $want) { throw "checksum mismatch for $name.zip" }
  Expand-Archive "$tmp\$name.zip" -DestinationPath $tmp -Force

  $dir = if ($env:MMDBIO_DIR) { $env:MMDBIO_DIR } else { Join-Path $env:LOCALAPPDATA 'Programs\mmdbio' }
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
  Copy-Item "$tmp\$name\mmdbio.exe" "$dir\mmdbio.exe" -Force

  $path = [Environment]::GetEnvironmentVariable('Path', 'User')
  if (-not ($path -split ';' | Where-Object { $_ -eq $dir })) {
    [Environment]::SetEnvironmentVariable('Path', ($path.TrimEnd(';') + ";$dir").TrimStart(';'), 'User')
    Write-Host "added $dir to your PATH (open a new terminal to use it)"
  }
  Write-Host "installed $(& "$dir\mmdbio.exe" --version) to $dir"
  Write-Host "shell completion: mmdbio completion install powershell"
} finally {
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
