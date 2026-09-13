"""Deterministic end-to-end regression coverage for the MCP stdio server.

Usage:
    python3 modules/mcp/tests/test_mcp_e2e.py --binary /path/to/redot
"""

import argparse
import json
import queue
import subprocess
import tempfile
import threading
import time
from collections import deque
from pathlib import Path

REQUEST_TIMEOUT = 15.0


class MCPClient:
    """Small JSON-RPC stdio client with timeout-safe stream readers."""

    def __init__(self, command):
        self.command = command
        self.process = None
        self.messages = queue.Queue()
        self.stderr_lines = deque(maxlen=200)
        self.stdout_noise = deque(maxlen=50)
        self.pending = {}
        self.next_id = 1
        self.stdout_thread = None
        self.stderr_thread = None

    def start(self):
        self.process = subprocess.Popen(
            self.command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self.stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self.stdout_thread.start()
        self.stderr_thread.start()

    def _read_stdout(self):
        try:
            for line in self.process.stdout:
                line = line.strip()
                if not line:
                    continue
                try:
                    self.messages.put(("json", json.loads(line)))
                except json.JSONDecodeError:
                    self.messages.put(("noise", line))
        finally:
            self.messages.put(("eof", None))

    def _read_stderr(self):
        for line in self.process.stderr:
            self.stderr_lines.append(line.rstrip())

    def _diagnostics(self):
        parts = []
        if self.stderr_lines:
            parts.append("stderr:\n" + "\n".join(self.stderr_lines))
        if self.stdout_noise:
            parts.append("non-JSON stdout:\n" + "\n".join(self.stdout_noise))
        return "\n".join(parts)

    def _send(self, message):
        if self.process.poll() is not None:
            raise AssertionError("MCP server exited before request. " + self._diagnostics())
        self.process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.process.stdin.flush()

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self._send(message)

    def request(self, method, params):
        request_id = self.next_id
        self.next_id += 1
        self._send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})

        if request_id in self.pending:
            return self.pending.pop(request_id)

        deadline = time.monotonic() + REQUEST_TIMEOUT
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(
                    "Timed out waiting for JSON-RPC response to %s. %s" % (method, self._diagnostics())
                )
            try:
                kind, value = self.messages.get(timeout=remaining)
            except queue.Empty:
                raise AssertionError(
                    "Timed out waiting for JSON-RPC response to %s. %s" % (method, self._diagnostics())
                )
            if kind == "noise":
                self.stdout_noise.append(value)
                continue
            if kind == "eof":
                raise AssertionError("MCP server closed stdout. " + self._diagnostics())
            if "id" not in value:
                continue
            if value["id"] == request_id:
                return value
            self.pending[value["id"]] = value

    def close(self):
        if not self.process:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5.0)
        if self.stdout_thread:
            self.stdout_thread.join(timeout=1.0)
        if self.stderr_thread:
            self.stderr_thread.join(timeout=1.0)


def assert_success(response, description):
    assert "error" not in response, "%s returned JSON-RPC error: %r" % (description, response)
    result = response.get("result")
    assert isinstance(result, dict), "%s returned no result: %r" % (description, response)
    assert not result.get("isError", False), "%s returned tool error: %r" % (description, response)
    return result


def assert_tool_error(response, description):
    assert "error" not in response, "%s returned JSON-RPC error: %r" % (description, response)
    result = response.get("result")
    assert isinstance(result, dict) and result.get("isError") is True, "%s should return a tool error: %r" % (
        description,
        response,
    )


def tool_text(result):
    return "\n".join(item.get("text", "") for item in result.get("content", []) if item.get("type") == "text")


def schema_permits(schema, value):
    """Check the small JSON Schema subset relevant to scalar value schemas."""
    if schema is True or not isinstance(schema, dict):
        return schema is not False
    if "const" in schema and value != schema["const"]:
        return False
    if "enum" in schema and value not in schema["enum"]:
        return False
    if "anyOf" in schema and not any(schema_permits(item, value) for item in schema["anyOf"]):
        return False
    if "oneOf" in schema and sum(schema_permits(item, value) for item in schema["oneOf"]) != 1:
        return False
    if "allOf" in schema and not all(schema_permits(item, value) for item in schema["allOf"]):
        return False
    if "not" in schema and schema_permits(schema["not"], value):
        return False

    schema_type = schema.get("type")
    if isinstance(schema_type, list):
        return any(schema_permits({"type": item}, value) for item in schema_type)
    if schema_type is None:
        return True
    if schema_type == "string":
        return isinstance(value, str)
    if schema_type == "boolean":
        return isinstance(value, bool)
    if schema_type == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if schema_type == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if schema_type == "null":
        return value is None
    return False


def call_tool(client, name, arguments):
    return client.request("tools/call", {"name": name, "arguments": arguments})


def run_regression(binary):
    with tempfile.TemporaryDirectory(prefix="redot-mcp-e2e-") as project_directory:
        project = Path(project_directory)
        (project / "project.godot").write_text('[application]\nconfig/name="MCP E2E Regression"\n', encoding="utf-8")

        client = MCPClient([str(binary), "--headless", "--mcp-server", "--path", str(project)])
        client.start()
        try:
            initialize = client.request(
                "initialize",
                {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {},
                    "clientInfo": {"name": "redot-mcp-e2e", "version": "1.0"},
                },
            )
            assert_success(initialize, "initialize")
            client.notify("notifications/initialized", {})

            tools_result = assert_success(client.request("tools/list", {}), "tools/list")
            tools = {tool["name"]: tool for tool in tools_result.get("tools", [])}
            for tool_name in ("scene_action", "resource_action", "project_config"):
                value_schema = tools[tool_name]["inputSchema"]["properties"]["value"]
                for scalar in ("text", 1.25, True, None):
                    assert schema_permits(value_schema, scalar), "%s value schema rejects scalar %r: %r" % (
                        tool_name,
                        scalar,
                        value_schema,
                    )

            script_path = "res://global_node.gd"
            script_content = "class_name MCPRegressionNode2D\nextends Node2D\n"
            assert_success(
                call_tool(
                    client,
                    "project_config",
                    {"action": "create_file_res", "path": script_path, "content": script_content},
                ),
                "create_file_res",
            )

            duplicate_script_path = "res://duplicate_global_class.gd"
            duplicate_class = call_tool(
                client,
                "project_config",
                {
                    "action": "create_file_res",
                    "path": duplicate_script_path,
                    "content": script_content,
                },
            )
            assert_tool_error(duplicate_class, "reject duplicate global class")
            assert not (project / "duplicate_global_class.gd").exists(), "failed class registration left a file behind"

            class_scene = "res://global_class.tscn"
            assert_success(
                call_tool(
                    client,
                    "scene_action",
                    {"action": "create", "scene_path": class_scene, "node_type": "MCPRegressionNode2D"},
                ),
                "scene_action.create global class",
            )
            class_node = assert_success(
                call_tool(client, "scene_action", {"action": "get_node", "scene_path": class_scene, "node_path": "."}),
                "inspect global class scene",
            )
            assert json.loads(tool_text(class_node))["script"] == script_path

            external_script_path = "res://externally_scanned_node.gd"
            (project / "externally_scanned_node.gd").write_text(
                "class_name MCPExternallyScannedNode\nextends Node\n", encoding="utf-8"
            )
            scan = subprocess.run(
                [str(binary), "--headless", "--editor", "--path", str(project), "--quit-after", "30"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=30.0,
            )
            assert scan.returncode == 0, "editor class scan failed:\n" + scan.stderr
            external_scene = "res://externally_scanned_class.tscn"
            assert_success(
                call_tool(
                    client,
                    "scene_action",
                    {
                        "action": "create",
                        "scene_path": external_scene,
                        "node_type": "MCPExternallyScannedNode",
                    },
                ),
                "scene_action.create externally scanned class",
            )
            external_node = assert_success(
                call_tool(
                    client,
                    "scene_action",
                    {"action": "get_node", "scene_path": external_scene, "node_path": "."},
                ),
                "inspect externally scanned class scene",
            )
            assert json.loads(tool_text(external_node))["script"] == external_script_path

            direct_scene = "res://direct_script.tscn"
            assert_success(
                call_tool(
                    client, "scene_action", {"action": "create", "scene_path": direct_scene, "node_type": "Node2D"}
                ),
                "create direct script scene",
            )
            assert_success(
                call_tool(
                    client,
                    "scene_action",
                    {"action": "set_prop", "scene_path": direct_scene, "property": "z_index", "value": 7},
                ),
                "set scalar scene property",
            )
            assert_success(
                call_tool(
                    client,
                    "scene_action",
                    {"action": "set_prop", "scene_path": direct_scene, "property": "script", "value": script_path},
                ),
                "attach direct script resource",
            )

            envelope_scene = "res://envelope_script.tscn"
            assert_success(
                call_tool(
                    client, "scene_action", {"action": "create", "scene_path": envelope_scene, "node_type": "Node2D"}
                ),
                "create envelope script scene",
            )
            assert_success(
                call_tool(
                    client,
                    "scene_action",
                    {
                        "action": "set_prop",
                        "scene_path": envelope_scene,
                        "property": "script",
                        "value": {"resource_path": script_path},
                    },
                ),
                "attach enveloped script resource",
            )
            direct_tscn = (project / "direct_script.tscn").read_text(encoding="utf-8")
            assert "ExtResource(" in direct_tscn, "script reference should be an ExtResource:\n" + direct_tscn
            assert "script = ExtResource(" in direct_tscn, direct_tscn
            assert "z_index = 7" in direct_tscn, direct_tscn
            envelope_tscn = (project / "envelope_script.tscn").read_text(encoding="utf-8")
            assert "script = ExtResource(" in envelope_tscn, envelope_tscn

            assert_tool_error(
                call_tool(
                    client,
                    "scene_action",
                    {"action": "set_prop", "scene_path": direct_scene, "property": "does_not_exist", "value": 1},
                ),
                "invalid scene property",
            )

            resource_path = "res://style_box.tres"
            assert_success(
                call_tool(
                    client, "resource_action", {"action": "create", "path": resource_path, "type": "StyleBoxFlat"}
                ),
                "create resource",
            )
            assert_success(
                call_tool(
                    client,
                    "resource_action",
                    {"action": "modify", "path": resource_path, "property": "corner_radius_top_left", "value": 9},
                ),
                "modify scalar resource property",
            )
            resource_text = (project / "style_box.tres").read_text(encoding="utf-8")
            assert "corner_radius_top_left = 9" in resource_text, resource_text
            assert_tool_error(
                call_tool(
                    client,
                    "resource_action",
                    {"action": "modify", "path": resource_path, "property": "does_not_exist", "value": 1},
                ),
                "invalid resource property",
            )

        finally:
            client.close()


def main():
    parser = argparse.ArgumentParser(description="Run MCP stdio end-to-end regression checks.")
    parser.add_argument("--binary", required=True, help="Path to the Redot editor binary")
    args = parser.parse_args()

    binary = Path(args.binary).expanduser().resolve()
    if not binary.is_file():
        parser.error("binary does not exist: %s" % binary)

    run_regression(binary)
    print("MCP E2E regression passed.")


if __name__ == "__main__":
    main()
