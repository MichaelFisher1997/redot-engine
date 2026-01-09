/**************************************************************************/
/*  opencode_acp_client.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             REDOT ENGINE                               */
/*                        https://redotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2024-present Redot Engine contributors                   */
/*                                          (see REDOT_AUTHORS.md)        */
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "opencode_acp_client.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"

void OpenCodeACPClient::_thread_func(void *p_userdata) {
	OpenCodeACPClient *self = static_cast<OpenCodeACPClient *>(p_userdata);

	while (self->thread_running.is_set()) {
		if (self->pipe.is_valid()) {
			// Check if process is still running
			if (self->process_id != 0 && !OS::get_singleton()->is_process_running(self->process_id)) {
				self->call_deferred(SNAME("_on_process_exited"));
				break;
			}

			// Check if there's data available to read
			uint64_t available = self->pipe->get_length();
			if (available > 0) {
				String line = self->pipe->get_line();
				if (!line.is_empty()) {
					bool is_json = false;
					for (int i = 0; i < line.length(); i++) {
						char32_t c = line[i];
						if (c <= 32) {
							continue;
						}
						if (c == '{') {
							is_json = true;
						}
						break;
					}

					if (is_json) {
						Ref<JSON> json;
						json.instantiate();
						if (json->parse(line) == OK) {
							Variant msg = json->get_data();
							if (msg.get_type() == Variant::DICTIONARY) {
								Dictionary d = msg;
								if (d.has("method")) {
									if (d.has("id")) {
										self->call_deferred(SNAME("_handle_rpc_request"), d);
									} else {
										self->call_deferred(SNAME("_handle_rpc_notification"), d);
									}
								} else if (d.has("result") || d.has("error")) {
									self->call_deferred(SNAME("_handle_rpc_response"), d);
								}
							} else {
								is_json = false;
							}
						} else {
							is_json = false;
						}
					}

					if (!is_json) {
						// Not a JSON dictionary, could be a log or banner
						Dictionary log_msg;
						log_msg["method"] = "window/logMessage";
						Dictionary params;
						params["message"] = line;
						log_msg["params"] = params;
						self->call_deferred(SNAME("_handle_rpc_notification"), log_msg);
					}
				}
			}
		}

		OS::get_singleton()->delay_usec(10000);
	}
}

void OpenCodeACPClient::_on_process_exited() {
	process_id = 0;
	emit_signal(SNAME("connection_lost"), "Process exited unexpectedly.");
}

void OpenCodeACPClient::_handle_rpc_notification(const Dictionary &p_notification) {
	emit_signal(SNAME("message_received"), p_notification);
}

void OpenCodeACPClient::_handle_rpc_request(const Dictionary &p_request) {
	if (!p_request.has("method") || !p_request.has("id")) {
		return;
	}

	String method = p_request["method"];
	Variant id = p_request["id"];
	Dictionary params = p_request.has("params") ? Dictionary(p_request["params"]) : Dictionary();

	Dictionary result;
	Dictionary error;

	if (method == "fs/readTextFile") {
		result = _handle_fs_read_text_file(params);
	} else if (method == "fs/writeTextFile") {
		result = _handle_fs_write_text_file(params);
	} else if (method == "terminal/execute") {
		result = _handle_terminal_execute(params);
	} else {
		// Method not found
		error["code"] = -32601; // Method not found
		error["message"] = "Method not found: " + method;
	}

	if (error.is_empty() && result.is_empty() && method != "fs/readTextFile") {
		// If no result and no error, but also not a successful void return (readTextFile returns content)
		// Actually, successful write/execute usually return something or empty dict.
		// Let's assume tool handlers set result or error.
	}

	send_response(id, result, error);

	// Still emit signal for UI updates/logging
	emit_signal(SNAME("message_received"), p_request);
}

void OpenCodeACPClient::send_response(const Variant &p_id, const Dictionary &p_result, const Dictionary &p_error) {
	if (pipe.is_valid()) {
		Dictionary response;
		if (!p_error.is_empty()) {
			int code = p_error.has("code") ? int(p_error["code"]) : -32603; // Internal error default
			String message = p_error.has("message") ? String(p_error["message"]) : "Unknown error";
			response = rpc->make_response_error(code, message, p_id);
		} else {
			response = rpc->make_response(p_result, p_id);
		}

		String json = JSON::stringify(response);
		pipe->store_line(json);
	}
}

Dictionary OpenCodeACPClient::_handle_fs_read_text_file(const Dictionary &p_params) {
	Dictionary result;
	if (!p_params.has("path")) {
		// Error handling should ideally return an error dict, but for now empty result implies failure?
		// ACP expects text content.
		return result;
	}
	String path = p_params["path"];
	Error err;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ, &err);
	if (f.is_valid()) {
		result["text"] = f->get_as_text();
	} else {
		// Return error? Or just null? ACP fs/readTextFile returns { text: string } | null
		// We'll return empty which results in null
	}
	return result;
}

Dictionary OpenCodeACPClient::_handle_fs_write_text_file(const Dictionary &p_params) {
	Dictionary result;
	if (!p_params.has("path") || !p_params.has("content")) {
		return result;
	}
	String path = p_params["path"];
	String content = p_params["content"];
	Error err;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE, &err);
	if (f.is_valid()) {
		f->store_string(content);
		result["success"] = true; // Convention
	}
	return result;
}

Dictionary OpenCodeACPClient::_handle_terminal_execute(const Dictionary &p_params) {
	Dictionary result;
	if (!p_params.has("command")) {
		return result;
	}
	String command = p_params["command"];
	String cwd = p_params.has("cwd") ? String(p_params["cwd"]) : "";

	// Security warning: executing arbitrary commands
	print_line("OpenCodeACPClient: Executing command: " + command);

	List<String> args;
	args.push_back("-c");

	String final_cmd = command;
	if (!cwd.is_empty()) {
		final_cmd = "cd \"" + cwd + "\" && " + command;
	}
	args.push_back(final_cmd);

	String output;
	int exit_code = 0;
	Error err = OS::get_singleton()->execute("/bin/sh", args, &output, &exit_code, true);
	if (err != OK) {
		result["error"] = "Failed to execute command: " + itos(err);
	}

	result["stdout"] = output;
	result["stderr"] = ""; // execute captures both in output usually unless separated
	result["exitCode"] = exit_code;

	return result;
}

void OpenCodeACPClient::_handle_rpc_response(const Dictionary &p_response) {
	if (p_response.has("id")) {
		int id = p_response["id"];
		if (id == 0) { // initialize response
			// Skip "initialized" notification - OpenCode ACP v1.1.6 doesn't support it

			// Create session
			Dictionary session_params;
			String resource_path = ProjectSettings::get_singleton()->get_resource_path();
			if (resource_path.is_empty()) {
				resource_path = OS::get_singleton()->get_executable_path().get_base_dir();
			}
			session_params["cwd"] = ProjectSettings::get_singleton()->globalize_path(resource_path);

			// OpenCode ACP v1.1.6 requires mcpServers as an array
			Array mcp_servers;
			session_params["mcpServers"] = mcp_servers;

			send_request("session/new", session_params);
		} else if (id == 1) { // session/new response
			Dictionary result = p_response["result"];
			if (result.has("sessionId")) {
				sessionId = result["sessionId"];
			}
		}
	}
	emit_signal(SNAME("message_received"), p_response);
}

void OpenCodeACPClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start"), &OpenCodeACPClient::start);
	ClassDB::bind_method(D_METHOD("stop"), &OpenCodeACPClient::stop);
	ClassDB::bind_method(D_METHOD("send_request", "method", "params"), &OpenCodeACPClient::send_request);
	ClassDB::bind_method(D_METHOD("send_notification", "method", "params"), &OpenCodeACPClient::send_notification);

	ClassDB::bind_method(D_METHOD("set_model", "model"), &OpenCodeACPClient::set_model);
	ClassDB::bind_method(D_METHOD("get_model"), &OpenCodeACPClient::get_model);

	ClassDB::bind_method(D_METHOD("_handle_rpc_notification", "notification"), &OpenCodeACPClient::_handle_rpc_notification);
	ClassDB::bind_method(D_METHOD("_handle_rpc_request", "request"), &OpenCodeACPClient::_handle_rpc_request);
	ClassDB::bind_method(D_METHOD("_handle_rpc_response", "response"), &OpenCodeACPClient::_handle_rpc_response);
	ClassDB::bind_method(D_METHOD("_on_process_exited"), &OpenCodeACPClient::_on_process_exited);

	ADD_SIGNAL(MethodInfo("message_received", PropertyInfo(Variant::DICTIONARY, "message")));
	ADD_SIGNAL(MethodInfo("connection_lost", PropertyInfo(Variant::STRING, "reason")));
}

Error OpenCodeACPClient::start() {
	List<String> args;
	args.push_back("acp");
	// Note: opencode acp doesn't accept --model flag
	// Model selection is done via session/setModel RPC call after session creation

	// Try to find opencode in common locations
	Vector<String> paths_to_try;
	paths_to_try.push_back("/home/micqdf/.npm-global/bin/opencode");
	paths_to_try.push_back("/usr/local/bin/opencode");
	paths_to_try.push_back("/usr/bin/opencode");

	// Also check if there's an OPENCODE_PATH environment variable
	String env_path = OS::get_singleton()->get_environment("OPENCODE_PATH");
	if (!env_path.is_empty()) {
		paths_to_try.insert(0, env_path);
	}

	String opencode_path;
	Dictionary res;

	// Try each path
	for (int i = 0; i < paths_to_try.size(); i++) {
		opencode_path = paths_to_try[i];
		if (FileAccess::exists(opencode_path)) {
			res = OS::get_singleton()->execute_with_pipe(opencode_path, args);
			if (res.has("pid") && int(res["pid"]) != 0) {
				break;
			}
		}
	}

	// Fallback: try PATH lookup
	if (!res.has("pid") || int(res["pid"]) == 0) {
		opencode_path = "opencode";
		res = OS::get_singleton()->execute_with_pipe(opencode_path, args);
	}

	// Last resort: use shell wrapper with explicit PATH
	if (!res.has("pid") || int(res["pid"]) == 0) {
		opencode_path = "/bin/sh";
		args.clear();
		args.push_back("-lc"); // Login shell to source profile
		String cmd = "opencode acp";
		args.push_back(cmd);
		res = OS::get_singleton()->execute_with_pipe(opencode_path, args);
	}

	if (res.has("pid") && int(res["pid"]) != 0) {
		process_id = res["pid"];
		pipe = res["stdio"];

		thread_running.set();
		thread.start(_thread_func, this);

		// Send initialize request with proper capabilities (matching Zed's implementation)
		Dictionary params;
		params["protocolVersion"] = 1;

		Dictionary client_info;
		client_info["name"] = "redot-engine";
		client_info["title"] = "Redot Engine";
		client_info["version"] = "1.0.0";
		params["clientInfo"] = client_info;

		// Client capabilities - declare what we support
		Dictionary capabilities;

		// File system capabilities
		Dictionary fs_caps;
		fs_caps["readTextFile"] = true;
		fs_caps["writeTextFile"] = true;
		capabilities["fs"] = fs_caps;

		// Terminal capability
		capabilities["terminal"] = true;

		// Meta capabilities (experimental features)
		Dictionary meta;
		meta["terminal_output"] = true;
		meta["terminal-auth"] = true;
		capabilities["_meta"] = meta;

		params["clientCapabilities"] = capabilities;

		send_request("initialize", params);

		return OK;
	}

	ERR_PRINT("OpenCodeACPClient: Failed to execute 'opencode'.");
	return ERR_CANT_FORK;
}

void OpenCodeACPClient::stop() {
	if (thread_running.is_set()) {
		thread_running.clear();
		thread.wait_to_finish();
	}

	if (process_id != 0) {
		OS::get_singleton()->kill(process_id);
		process_id = 0;
	}
}

void OpenCodeACPClient::send_request(const String &p_method, const Dictionary &p_params) {
	if (pipe.is_valid()) {
		int id = 2; // Default for chat
		if (p_method == "initialize") {
			id = 0;
		} else if (p_method == "session/new") {
			id = 1;
		}
		Dictionary req = rpc->make_request(p_method, p_params, id);
		String json = JSON::stringify(req);
		pipe->store_line(json);
	}
}

void OpenCodeACPClient::send_notification(const String &p_method, const Dictionary &p_params) {
	if (pipe.is_valid()) {
		Dictionary req = rpc->make_notification(p_method, p_params);
		String json = JSON::stringify(req);
		pipe->store_line(json);
	}
}

OpenCodeACPClient::OpenCodeACPClient() {
	rpc = memnew(JSONRPC);
}

OpenCodeACPClient::~OpenCodeACPClient() {
	stop();
	if (rpc) {
		memdelete(rpc);
	}
}
