$ErrorActionPreference = "Stop"

$ToolDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Source = Join-Path $ToolDir "dist\kicadgym-mcp-server.exe"
$BaselineBoard = Join-Path $ToolDir "kicadgym_empty.kicad_pcb"
$KiCadBin = Resolve-Path (Join-Path $ToolDir "..\..\build\install\local-msvc-win64-release\bin")
$Destination = Join-Path $KiCadBin "mcp"

New-Item -ItemType Directory -Path $Destination -Force | Out-Null
Copy-Item -LiteralPath $Source -Destination $Destination -Force
Copy-Item -LiteralPath $BaselineBoard -Destination $Destination -Force
Write-Host "Staged KiCadGym MCP package to $Destination"
