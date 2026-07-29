$ErrorActionPreference = "Stop"

$ToolDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$VenvDir = Join-Path $ToolDir ".venv"
$Python = Join-Path $VenvDir "Scripts\python.exe"
$DistDir = Join-Path $ToolDir "dist"

if (-not (Test-Path -LiteralPath $Python)) {
  python -m venv $VenvDir
}

& $Python -m pip install --disable-pip-version-check "pyinstaller==6.21.0" "kicad-python==0.7.1"
& $Python -m PyInstaller `
  --noconfirm `
  --clean `
  --onefile `
  --name kicadgym-mcp-server `
  --collect-all kipy `
  --collect-all pynng `
  --distpath $DistDir `
  --workpath (Join-Path $ToolDir "build") `
  --specpath (Join-Path $ToolDir "spec") `
  (Join-Path $ToolDir "kicadgym_mcp_server.py")

Write-Host "Built $DistDir\kicadgym-mcp-server.exe"
