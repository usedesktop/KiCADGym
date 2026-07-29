#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""KiCadGym MCP server backed by KiCad's official IPC API."""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import time
import traceback
import zipfile
from pathlib import Path
from typing import Any

from kipy import KiCad
from kipy.errors import ApiError
from kipy.proto.common.types import DocumentType


PROTOCOL_VERSION = "2025-06-18"
SERVER_INFO = {"name": "kicadgym-mcp-server", "version": "0.1.0"}
DEFAULT_CONNECT_TIMEOUT_MS = 30_000
DEFAULT_CALL_TIMEOUT_MS = 120_000
PROJECT_SUFFIXES = {
    ".kicad_pro",
    ".kicad_pcb",
    ".kicad_sch",
    ".kicad_prl",
    ".kicad_dru",
    ".kicad_jobset",
    ".kicad_wks",
}
PROJECT_TABLES = {"fp-lib-table", "sym-lib-table"}


TOOLS: list[dict[str, Any]] = [
    {
        "name": "execute_kicad_code",
        "description": "Execute Python against the active KiCadGym IPC session.",
        "inputSchema": {
            "type": "object",
            "properties": {"code": {"type": "string"}},
            "required": ["code"],
        },
    },
    {
        "name": "get_project_summary",
        "description": "Read a compact summary of open KiCad documents.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "inspect_project_state",
        "description": "Inspect open PCB and schematic state through the official KiCad IPC API.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "configure_session_artifacts",
        "description": "Set the checkpoint directory owned by Desktop.",
        "inputSchema": {
            "type": "object",
            "properties": {"checkpoint_dir": {"type": "string"}},
            "required": ["checkpoint_dir"],
        },
    },
    {
        "name": "snapshot_session",
        "description": "Archive the active KiCad project as a session checkpoint.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "snapshot_id": {"type": "string"},
                "state_id": {"type": "string"},
                "checkpoint_file": {"type": "string"},
            },
            "required": ["snapshot_id"],
        },
    },
    {
        "name": "restore_session",
        "description": "Restore a KiCadGym project checkpoint and revert open editors.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "snapshot_id": {"type": "string"},
                "checkpoint_file": {"type": "string"},
            },
            "required": ["snapshot_id"],
        },
    },
    {
        "name": "reset_session",
        "description": "Reset KiCadGym from a task checkpoint.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "target_state_id": {"type": "string"},
                "snapshot_id": {"type": "string"},
                "checkpoint_file": {"type": "string"},
            },
        },
    },
]


class McpServer:
    def __init__(self) -> None:
        self.stdin = sys.stdin.buffer
        self.stdout = sys.stdout.buffer
        self.kicad: KiCad | None = None
        self.snapshots: dict[str, str] = {}

    def serve(self) -> None:
        while True:
            frame = read_frame(self.stdin)
            if frame is None:
                return
            try:
                request = json.loads(frame.decode("utf-8"))
                response = self.handle_request(request)
            except Exception as exc:  # noqa: BLE001
                response = error_response(
                    safe_request_id(frame), -32603, str(exc), traceback.format_exc()
                )
            if response is not None:
                write_frame(self.stdout, response)

    def handle_request(self, request: dict[str, Any]) -> dict[str, Any] | None:
        method = request.get("method")
        request_id = request.get("id")
        if request_id is None:
            return None
        try:
            if method == "initialize":
                self.wait_for_kicad()
                return result_response(
                    request_id,
                    {
                        "protocolVersion": request.get("params", {}).get(
                            "protocolVersion", PROTOCOL_VERSION
                        ),
                        "capabilities": {"tools": {}},
                        "serverInfo": SERVER_INFO,
                    },
                )
            if method == "tools/list":
                return result_response(request_id, {"tools": TOOLS})
            if method == "tools/call":
                params = request.get("params") or {}
                return result_response(
                    request_id,
                    self.call_tool(str(params.get("name") or ""), params.get("arguments") or {}),
                )
            return error_response(request_id, -32601, f"unknown MCP method: {method}")
        except Exception as exc:  # noqa: BLE001
            return error_response(request_id, -32000, str(exc), traceback.format_exc())

    def wait_for_kicad(self) -> None:
        timeout_ms = int_env("KICADGYM_MCP_CONNECT_TIMEOUT_MS", DEFAULT_CONNECT_TIMEOUT_MS)
        deadline = time.monotonic() + timeout_ms / 1000
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                self.kicad = KiCad(socket_path=kicad_socket_path(), timeout_ms=1_000)
                self.kicad.ping()
                return
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                time.sleep(0.1)
        startup_log_path = checkpoint_dir() / "api-startup.log"
        startup_log = None
        if startup_log_path.is_file():
            try:
                startup_log = startup_log_path.read_text(encoding="utf-8").strip()
            except OSError:
                startup_log = None
        detail = f"; startup log: {startup_log}" if startup_log else ""
        raise RuntimeError(
            f"KiCadGym IPC API is not reachable at {kicad_socket_path()}: {last_error}{detail}"
        )

    def require_kicad(self) -> KiCad:
        if self.kicad is None:
            raise RuntimeError("KiCadGym is not initialized")
        return self.kicad

    def call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        try:
            if name == "execute_kicad_code":
                return tool_result(self.execute_code(str(arguments.get("code") or "")))
            if name == "get_project_summary":
                return tool_result({"ok": True, "project": self.project_summary()})
            if name == "inspect_project_state":
                return tool_result(self.inspect_project_state())
            if name == "configure_session_artifacts":
                return tool_result(configure_session_artifacts(arguments))
            if name == "snapshot_session":
                return tool_result(self.snapshot_session(arguments))
            if name == "restore_session":
                return tool_result(self.restore_session(arguments))
            if name == "reset_session":
                return tool_result(self.reset_session(arguments))
            return tool_result({"ok": False, "error": f"unknown tool: {name}"}, True)
        except Exception as exc:  # noqa: BLE001
            return tool_result({"ok": False, "error": str(exc), "traceback": traceback.format_exc()}, True)

    def execute_code(self, code: str) -> dict[str, Any]:
        if not code.strip():
            return {"ok": False, "error": "missing code"}
        kicad = self.require_kicad()
        try:
            board = kicad.get_board()
        except ApiError:
            board = None
        namespace: dict[str, Any] = {
            "__name__": "__kicadgym_mcp_exec__",
            "KiCad": KiCad,
            "kicad": kicad,
            "board": board,
            "result": None,
        }
        exec(code, namespace, namespace)  # noqa: S102
        return {
            "ok": True,
            "result": json_safe(namespace.get("result")),
            "project": self.project_summary(),
        }

    def open_documents(self) -> list[Any]:
        kicad = self.require_kicad()
        documents: list[Any] = []
        for document_type in (DocumentType.DOCTYPE_PCB, DocumentType.DOCTYPE_SCHEMATIC):
            try:
                documents.extend(kicad.get_open_documents(document_type))
            except ApiError:
                continue
        return documents

    def project_summary(self) -> dict[str, Any]:
        kicad = self.require_kicad()
        documents = self.open_documents()
        return {
            "version": str(kicad.get_version()),
            "document_count": len(documents),
            "documents": [document_json(document) for document in documents],
        }

    def inspect_project_state(self) -> dict[str, Any]:
        kicad = self.require_kicad()
        ui_state = load_ui_state()
        available_actions = [rl_env_action(action) for action in ui_state.get("visible_actions", [])]
        catalog_observations = []
        seen_catalog_ids: set[str] = set()
        for action in available_actions:
            catalog_id = str(action.get("metadata", {}).get("catalog_id") or "")
            if not catalog_id or catalog_id in seen_catalog_ids:
                continue
            seen_catalog_ids.add(catalog_id)
            observation = dict(action)
            observation["action_id"] = catalog_id
            observation["metadata"] = {**action["metadata"], "instance_id": None}
            catalog_observations.append(observation)
        result: dict[str, Any] = {
            "ok": True,
            "workspace": active_editor_name(self.open_documents()),
            "screen_state_hash": (
                f"kicad:{ui_state.get('captured_at_epoch_ms', 0)}:{len(available_actions)}"
            ),
            "available_actions": available_actions,
            "metadata": {
                "state_model": "global_catalog_observations_plus_visible_ui_state",
                "ui_state_schema": ui_state.get("schema"),
                "ui_capture_timestamp_epoch_ms": ui_state.get("captured_at_epoch_ms"),
                "action_catalog_observations": catalog_observations,
            },
            "project": self.project_summary(),
        }
        try:
            board = kicad.get_board()
            result["board"] = {
                "name": board.name,
                "footprints": len(board.get_footprints()),
                "pads": len(board.get_pads()),
                "tracks": len(board.get_tracks()),
                "vias": len(board.get_vias()),
                "zones": len(board.get_zones()),
                "copper_layers": board.get_copper_layer_count(),
            }
        except ApiError:
            result["board"] = None
        return result

    def snapshot_session(self, arguments: dict[str, Any]) -> dict[str, Any]:
        snapshot_id = str(arguments.get("snapshot_id") or "").strip()
        if not snapshot_id:
            return {"ok": False, "error": "snapshot_id is required"}
        documents = self.open_documents()
        if not documents:
            return {"ok": False, "error": "KiCadGym snapshot requires an open PCB or schematic document"}
        save_open_documents(self.require_kicad(), documents)
        project_root, files = project_files(documents)
        target = Path(str(arguments.get("checkpoint_file") or checkpoint_dir() / f"{snapshot_id}.zip"))
        target.parent.mkdir(parents=True, exist_ok=True)
        manifest = {
            "schema": "kicadgym.snapshot.v1",
            "snapshot_id": snapshot_id,
            "project_root": str(project_root),
            "files": [str(path.relative_to(project_root)) for path in files],
        }
        with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("kicadgym-snapshot.json", json.dumps(manifest, separators=(",", ":")))
            for path in files:
                relative = path.relative_to(project_root)
                archive.write(path, f"project/{relative.as_posix()}")
        self.snapshots[snapshot_id] = str(target)
        return {
            "ok": True,
            "snapshot_id": snapshot_id,
            "checkpoint_file": str(target),
            "project": self.project_summary(),
        }

    def restore_session(self, arguments: dict[str, Any]) -> dict[str, Any]:
        snapshot_id = str(arguments.get("snapshot_id") or "").strip()
        source_value = arguments.get("checkpoint_file") or self.snapshots.get(snapshot_id)
        if not source_value:
            return {"ok": False, "error": f"unknown snapshot: {snapshot_id}"}
        source = Path(str(source_value))
        if not source.is_file():
            return {"ok": False, "error": f"checkpoint file does not exist: {source}"}
        with zipfile.ZipFile(source, "r") as archive:
            manifest = json.loads(archive.read("kicadgym-snapshot.json").decode("utf-8"))
            project_root = Path(str(manifest["project_root"]))
            for relative_text in manifest.get("files", []):
                relative = safe_relative_path(str(relative_text))
                destination = project_root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(f"project/{relative.as_posix()}", "r") as src:
                    with destination.open("wb") as dst:
                        shutil.copyfileobj(src, dst)
        revert_open_documents(self.require_kicad())
        return {
            "ok": True,
            "snapshot_id": snapshot_id,
            "checkpoint_file": str(source),
            "project": self.project_summary(),
        }

    def reset_session(self, arguments: dict[str, Any]) -> dict[str, Any]:
        checkpoint_file = arguments.get("checkpoint_file")
        snapshot_id = str(arguments.get("snapshot_id") or "").strip()
        if not checkpoint_file and not snapshot_id:
            return {"ok": False, "error": "KiCadGym reset requires a checkpoint_file or snapshot_id"}
        return self.restore_session({"snapshot_id": snapshot_id, "checkpoint_file": checkpoint_file})


def kicad_socket_path() -> str:
    explicit = os.environ.get("KICAD_API_SOCKET")
    if explicit:
        return explicit
    port = int_env("KICAD_PORT", 9876)
    path = Path(tempfile.gettempdir()) / "kicadgym" / f"api-{port}.sock"
    return f"ipc://{path}"


def configure_session_artifacts(arguments: dict[str, Any]) -> dict[str, Any]:
    raw = str(arguments.get("checkpoint_dir") or "").strip()
    if not raw:
        return {"ok": False, "error": "checkpoint_dir is required"}
    previous_path = checkpoint_dir()
    path = Path(raw).resolve()
    path.mkdir(parents=True, exist_ok=True)
    source_ui_state = previous_path.parent / "ui-state.json"
    target_ui_state = path.parent / "ui-state.json"
    if source_ui_state.is_file() and source_ui_state != target_ui_state:
        target_ui_state.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_ui_state, target_ui_state)
    os.environ["KICADGYM_CHECKPOINT_DIR"] = str(path)
    return {"ok": True, "checkpoint_dir": str(path)}


def load_ui_state() -> dict[str, Any]:
    path = checkpoint_dir().parent / "ui-state.json"
    if not path.is_file():
        return {"schema": "kicadgym.ui_state.v1", "visible_actions": []}
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema") != "kicadgym.ui_state.v1":
        raise RuntimeError(f"unsupported KiCADGym UI state schema: {payload.get('schema')}")
    if not isinstance(payload.get("visible_actions"), list):
        raise RuntimeError("KiCADGym UI state is missing visible_actions")
    return payload


def rl_env_action(action: dict[str, Any]) -> dict[str, Any]:
    instance_id = str(action.get("instance_id") or action.get("catalog_id") or "")
    catalog_id = str(action.get("catalog_id") or "")
    if not instance_id or not catalog_id:
        raise RuntimeError("KiCADGym UI action is missing stable identity")
    bounds = action.get("bbox_win_px") or {}
    xmin = int(bounds.get("xmin") or 0)
    ymin = int(bounds.get("ymin") or 0)
    xmax = int(bounds.get("xmax") or xmin)
    ymax = int(bounds.get("ymax") or ymin)
    return {
        "action_id": instance_id,
        "label": str(action.get("label") or catalog_id),
        "action_type": str(action.get("kind") or "ui.control"),
        "enabled": bool(action.get("enabled", True)),
        "verifier_ids": list(action.get("verifier_ids") or []),
        "verifier_bindings": list(action.get("verifier_bindings") or []),
        "catalog_verifier_bindings": list(action.get("catalog_verifier_bindings") or []),
        "bounds": {
            "x": xmin,
            "y": ymin,
            "width": max(0, xmax - xmin),
            "height": max(0, ymax - ymin),
        },
        "metadata": {
            "source": "kicadgym.ui_state.v1",
            "catalog_id": catalog_id,
            "instance_id": instance_id,
            "button_type": action.get("button_type"),
            "command_id": action.get("command_id"),
            "semantic_action_id": action.get("semantic_action_id"),
            "catalog_verifier_ids": action.get("catalog_verifier_ids") or [],
            "raw_action": action,
        },
    }


def active_editor_name(documents: list[Any]) -> str | None:
    if any(int(getattr(document, "type", -1)) == int(DocumentType.DOCTYPE_PCB) for document in documents):
        return "pcb_editor"
    if any(int(getattr(document, "type", -1)) == int(DocumentType.DOCTYPE_SCHEMATIC) for document in documents):
        return "schematic_editor"
    return None


def checkpoint_dir() -> Path:
    value = os.environ.get("KICADGYM_CHECKPOINT_DIR")
    session_id = os.environ.get("KICADGYM_SESSION_ID", "session")
    path = Path(value) if value else Path(tempfile.gettempdir()) / "kicadgym" / session_id / "checkpoints"
    path.mkdir(parents=True, exist_ok=True)
    return path


def document_json(document: Any) -> dict[str, Any]:
    return {
        "type": int(document.type),
        "board_filename": getattr(document, "board_filename", "") or None,
        "project_name": getattr(document.project, "name", "") or None,
        "project_path": getattr(document.project, "path", "") or None,
    }


def save_open_documents(kicad: KiCad, documents: list[Any]) -> None:
    board_documents = [document for document in documents if int(document.type) == int(DocumentType.DOCTYPE_PCB)]
    if board_documents:
        kicad.get_board().save()
    # Schematic saving is exposed by the IPC API in current KiCad builds, but
    # kicad-python does not yet provide a top-level constructor. RunAction is
    # handled by the active editor and keeps the snapshot source deterministic.
    for action in ("common.Control.save", "eeschema.Save"):
        try:
            kicad.run_action(action)
        except ApiError:
            continue


def revert_open_documents(kicad: KiCad) -> None:
    kicad.get_board().revert()
    time.sleep(0.25)
    deadline = time.monotonic() + 5.0
    while True:
        try:
            kicad.get_board()
            return
        except ApiError:
            if time.monotonic() >= deadline:
                raise RuntimeError("KiCad board did not become available after restore")
            time.sleep(0.1)


def project_files(documents: list[Any]) -> tuple[Path, list[Path]]:
    roots = [Path(str(document.project.path)) for document in documents if str(document.project.path)]
    if not roots:
        board_paths = [Path(str(document.board_filename)) for document in documents if str(document.board_filename)]
        if not board_paths:
            raise RuntimeError("open KiCad documents do not expose a project path")
        project_root = board_paths[0].parent
    else:
        project_root = roots[0]
    files = sorted(
        path for path in project_root.iterdir()
        if path.is_file() and (path.suffix.lower() in PROJECT_SUFFIXES or path.name in PROJECT_TABLES)
    )
    if not files:
        raise RuntimeError(f"no KiCad project files found in {project_root}")
    return project_root, files


def safe_relative_path(value: str) -> Path:
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise RuntimeError(f"unsafe checkpoint path: {value}")
    return path


def read_frame(stream: Any) -> bytes | None:
    headers: list[bytes] = []
    while True:
        line = stream.readline()
        if line == b"":
            return None
        if line in (b"\r\n", b"\n"):
            break
        headers.append(line)
    content_length = None
    for header in headers:
        name, _, value = header.decode("ascii", errors="ignore").partition(":")
        if name.lower() == "content-length":
            content_length = int(value.strip())
            break
    if content_length is None:
        raise RuntimeError("missing Content-Length header")
    body = stream.read(content_length)
    if len(body) != content_length:
        raise RuntimeError("unexpected EOF while reading MCP frame body")
    return body


def write_frame(stream: Any, payload: dict[str, Any]) -> None:
    body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    stream.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
    stream.write(body)
    stream.flush()


def result_response(request_id: Any, result: dict[str, Any]) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def error_response(request_id: Any, code: int, message: str, data: Any | None = None) -> dict[str, Any]:
    error: dict[str, Any] = {"code": code, "message": message}
    if data is not None:
        error["data"] = data
    return {"jsonrpc": "2.0", "id": request_id, "error": error}


def tool_result(payload: dict[str, Any], is_error: bool = False) -> dict[str, Any]:
    return {
        "content": [{"type": "text", "text": json.dumps(payload, ensure_ascii=False, default=str)}],
        "structuredContent": payload,
        "isError": is_error,
    }


def json_safe(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, dict):
        return {str(key): json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [json_safe(item) for item in value]
    return str(value)


def int_env(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, str(default)))
    except ValueError:
        return default


def safe_request_id(frame: bytes) -> Any:
    try:
        value = json.loads(frame.decode("utf-8"))
        return value.get("id") if isinstance(value, dict) else None
    except Exception:
        return None


def main() -> int:
    McpServer().serve()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
