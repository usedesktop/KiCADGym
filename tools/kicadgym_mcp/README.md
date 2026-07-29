# KiCadGym MCP package

KiCadGym uses KiCad's official IPC API through `kicad-python`. The packaged
stdio MCP executable is self-contained; users do not need Python, `uv`, or
`uvx` at runtime.

```powershell
.\build_windows.ps1
.\stage_to_kicad_build_windows.ps1
```

Desktop launches one KiCad process and one MCP process per session. The Gym
build enables a session-specific IPC socket only when
`KICADGYM_SESSION_ID` is present, so ordinary KiCad launches retain the
upstream preference behavior.
