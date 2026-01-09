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
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "scene/main/node.h"

void OpenCodeACPClient::_thread_func(void *p_userdata) {
	OpenCodeACPClient *self = static_cast<OpenCodeACPClient *>(p_userdata);

	while (self->thread_running.is_set()) {
		if (self->pipe.is_valid()) {
			if (self->process_id != 0 && !OS::get_singleton()->is_process_running(self->process_id)) {
				self->call_deferred(SNAME("_on_process_exited"));
				break;
			}

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

	print_line("OpenCodeACPClient: Tool Request: " + method);

	Dictionary result;
	Dictionary error;

	if (method == "fs/readTextFile") {
		result = _handle_fs_read_text_file(params);
	} else if (method == "fs/writeTextFile") {
		result = _handle_fs_write_text_file(params);
	} else if (method == "fs/listDirectory") {
		result = _handle_fs_list_directory(params);
	} else if (method == "terminal/execute") {
		result = _handle_terminal_execute(params);
	} else if (method == "editor/showNotification") {
		result = _handle_editor_show_notification(params);
	} else if (method == "editor/createNode") {
		result = _handle_editor_create_node(params);
	} else if (method == "editor/createAndOpenScript") {
		result = _handle_editor_create_and_open_script(params);
	} else if (method == "editor/openFile" || method == "window/showDocument") {
		result = _handle_editor_open_file(params);
	} else {
		error["code"] = -32601;
		error["message"] = "Method not found: " + method;
	}

	send_response(id, result, error);
	emit_signal(SNAME("message_received"), p_request);
}

void OpenCodeACPClient::send_response(const Variant &p_id, const Dictionary &p_result, const Dictionary &p_error) {
	if (pipe.is_valid()) {
		Dictionary response;
		if (!p_error.is_empty()) {
			int code = p_error.has("code") ? int(p_error["code"]) : -32603;
			String message = p_error.has("message") ? String(p_error["message"]) : "Unknown error";
			response = rpc->make_response_error(code, message, p_id);
		} else {
			response = rpc->make_response(p_result, p_id);
		}
		pipe->store_line(JSON::stringify(response));
	}
}

String OpenCodeACPClient::_sanitize_path(const String &p_path) {
	String path = p_path.strip_edges();
	int last_protocol = path.rfind("res:/");
	if (last_protocol != -1) {
		int offset = 5;
		if (path.length() > last_protocol + offset && path[last_protocol + offset] == '/') {
			offset++;
		}
		path = "res://" + path.substr(last_protocol + offset).lstrip("/");
	} else if (path.begins_with("file://")) {
		path = path.substr(7);
	}
	String project_path = ProjectSettings::get_singleton()->get_resource_path();
	if (!project_path.is_empty() && path.begins_with(project_path)) {
		path = "res://" + path.substr(project_path.length()).lstrip("/");
	}
	if (path.begins_with("res:/") && !path.begins_with("res://")) {
		path = "res://" + path.substr(5).lstrip("/");
	}
	print_line("OpenCodeACPClient: Sanitized path '" + p_path + "' -> '" + path + "'");
	return path;
}

Dictionary OpenCodeACPClient::_handle_fs_read_text_file(const Dictionary &p_params) {
	Dictionary result;
	String path;
	if (p_params.has("path")) {
		path = p_params["path"];
	} else if (p_params.has("uri")) {
		path = p_params["uri"];
	} else {
		return result;
	}
	path = _sanitize_path(path);
	Error err;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ, &err);
	if (f.is_valid()) {
		result["text"] = f->get_as_text();
	}
	return result;
}

Dictionary OpenCodeACPClient::_handle_fs_write_text_file(const Dictionary &p_params) {
	Dictionary result;
	String path;
	if (p_params.has("path")) {
		path = p_params["path"];
	} else if (p_params.has("uri")) {
		path = p_params["uri"];
	} else {
		return result;
	}
	if (!p_params.has("content")) {
		return result;
	}
	path = _sanitize_path(path);
	Error err;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE, &err);
	if (f.is_valid()) {
		f->store_string(p_params["content"]);
		result["success"] = true;
	}
	return result;
}

Dictionary OpenCodeACPClient::_handle_fs_list_directory(const Dictionary &p_params) {
	Dictionary result;
	String path;
	if (p_params.has("path")) {
		path = p_params["path"];
	} else if (p_params.has("uri")) {
		path = p_params["uri"];
	} else {
		return result;
	}
	path = _sanitize_path(path);
	Ref<DirAccess> d = DirAccess::open(path);
	if (d.is_valid()) {
		Array files;
		d->list_dir_begin();
		String f = d->get_next();
		while (!f.is_empty()) {
			if (f != "." && f != "..") {
				files.push_back(f);
			}
			f = d->get_next();
		}
		result["files"] = files;
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
	List<String> args;
	args.push_back("-c");
	String final_cmd = command;
	if (!cwd.is_empty()) {
		final_cmd = "cd \"" + cwd + "\" && " + command;
	}
	args.push_back(final_cmd);
	String output;
	int exit_code = 0;
	OS::get_singleton()->execute("/bin/sh", args, &output, &exit_code, true);
	result["stdout"] = output;
	result["exitCode"] = exit_code;
	return result;
}

Dictionary OpenCodeACPClient::_handle_editor_show_notification(const Dictionary &p_params) {
	Dictionary result;
	if (!p_params.has("message")) {
		return result;
	}
	EditorNode::get_singleton()->show_warning(p_params["message"]);
	result["success"] = true;
	return result;
}

Dictionary OpenCodeACPClient::_handle_editor_create_node(const Dictionary &p_params) {
	Dictionary result;
	if (!p_params.has("type")) {
		return result;
	}
	String type = p_params["type"];
	String name = p_params.has("name") ? String(p_params["name"]) : type;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		result["error"] = "No edited scene root.";
		return result;
	}
	Object *obj = ClassDB::instantiate(type);
	if (!obj) {
		result["error"] = "Could not instantiate: " + type;
		return result;
	}
	Node *node = Object::cast_to<Node>(obj);
	if (!node) {
		memdelete(obj);
		result["error"] = "Not a Node: " + type;
		return result;
	}
	node->set_name(name);
	if (p_params.has("properties")) {
		Dictionary props = p_params["properties"];
		Array keys = props.keys();
		for (int i = 0; i < keys.size(); i++) {
			node->set(keys[i], props[keys[i]]);
		}
	}
	root->add_child(node);
	node->set_owner(root);
	result["success"] = true;
	return result;
}

Dictionary OpenCodeACPClient::_handle_editor_create_and_open_script(const Dictionary &p_params) {
	Dictionary result;
	String path;
	if (p_params.has("path")) {
		path = p_params["path"];
	} else if (p_params.has("uri")) {
		path = p_params["uri"];
	} else {
		return result;
	}
	if (!p_params.has("content")) {
		return result;
	}
	path = _sanitize_path(path);
	Error err;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE, &err);
	if (f.is_valid()) {
		f->store_string(p_params["content"]);
		f.unref();
		EditorInterface::get_singleton()->select_file(path);
		EditorInterface::get_singleton()->edit_resource(ResourceLoader::load(path));
		result["success"] = true;
	} else {
		result["error"] = "Failed to create script.";
	}
	return result;
}

Dictionary OpenCodeACPClient::_handle_editor_open_file(const Dictionary &p_params) {
	Dictionary result;
	String path;
	if (p_params.has("path")) {
		path = p_params["path"];
	} else if (p_params.has("uri")) {
		path = p_params["uri"];
	} else {
		return result;
	}
	path = _sanitize_path(path);
	EditorInterface::get_singleton()->select_file(path);
	EditorInterface::get_singleton()->edit_resource(ResourceLoader::load(path));
	result["success"] = true;
	return result;
}

void OpenCodeACPClient::_handle_rpc_response(const Dictionary &p_response) {
	if (p_response.has("id")) {
		int id = p_response["id"];
		if (id == 0) {
			Dictionary session_params;
			String resource_path = ProjectSettings::get_singleton()->get_resource_path();
			if (resource_path.is_empty()) {
				resource_path = OS::get_singleton()->get_executable_path().get_base_dir();
			}
			session_params["cwd"] = ProjectSettings::get_singleton()->globalize_path(resource_path);
			Array mcp_servers;
			session_params["mcpServers"] = mcp_servers;
			send_request("session/new", session_params);
		} else if (id == 1) {
			Dictionary result = p_response["result"];
			if (result.has("sessionId")) {
				sessionId = result["sessionId"];
				Dictionary update_params;
				update_params["sessionId"] = sessionId;
				Dictionary update_obj;
				update_obj["sessionUpdate"] = "available_commands_update";
				Array cmds;
				cmds.push_back("fs/readTextFile");
				cmds.push_back("fs/writeTextFile");
				cmds.push_back("fs/listDirectory");
				cmds.push_back("terminal/execute");
				cmds.push_back("editor/openFile");
				cmds.push_back("editor/createAndOpenScript");
				cmds.push_back("editor/createNode");
				cmds.push_back("editor/showNotification");
				cmds.push_back("window/showDocument");
				update_obj["commands"] = cmds;
				update_params["update"] = update_obj;
				send_notification("session/update", update_params);
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
	Vector<String> paths;
	paths.push_back("/home/micqdf/.npm-global/bin/opencode");
	paths.push_back("/usr/local/bin/opencode");
	paths.push_back("/usr/bin/opencode");
	String env = OS::get_singleton()->get_environment("OPENCODE_PATH");
	if (!env.is_empty()) {
		paths.insert(0, env);
	}
	String opencode_path;
	Dictionary res;
	for (int i = 0; i < paths.size(); i++) {
		opencode_path = paths[i];
		if (FileAccess::exists(opencode_path)) {
			res = OS::get_singleton()->execute_with_pipe(opencode_path, args);
			if (res.has("pid") && int(res["pid"]) != 0) {
				break;
			}
		}
	}
	if (!res.has("pid") || int(res["pid"]) == 0) {
		opencode_path = "opencode";
		res = OS::get_singleton()->execute_with_pipe(opencode_path, args);
	}
	if (!res.has("pid") || int(res["pid"]) == 0) {
		opencode_path = "/bin/sh";
		args.clear();
		args.push_back("-lc");
		args.push_back("opencode acp");
		res = OS::get_singleton()->execute_with_pipe(opencode_path, args);
	}

	if (res.has("pid") && int(res["pid"]) != 0) {
		process_id = res["pid"];
		pipe = res["stdio"];
		thread_running.set();
		thread.start(_thread_func, this);

		Dictionary params;
		params["protocolVersion"] = 1;
		Dictionary ci;
		ci["name"] = "redot-engine";
		ci["version"] = "1.0.0";
		params["clientInfo"] = ci;

		Array cmds;
		cmds.push_back("fs/readTextFile");
		cmds.push_back("fs/writeTextFile");
		cmds.push_back("fs/listDirectory");
		cmds.push_back("terminal/execute");
		cmds.push_back("editor/openFile");
		cmds.push_back("editor/createAndOpenScript");
		cmds.push_back("editor/createNode");
		cmds.push_back("editor/showNotification");
		cmds.push_back("window/showDocument");

		Array tools;
		{
			Dictionary t;
			t["name"] = "editor/openFile";
			t["description"] = "Open a file in editor.";
			Dictionary p;
			Dictionary ph;
			ph["type"] = "string";
			p["path"] = ph;
			Dictionary po;
			po["type"] = "object";
			po["properties"] = p;
			Array r;
			r.push_back("path");
			po["required"] = r;
			t["parameters"] = po;
			tools.push_back(t);
		}

		Dictionary cap;
		Dictionary fs;
		fs["readTextFile"] = true;
		fs["writeTextFile"] = true;
		fs["listDirectory"] = true;
		cap["fs"] = fs;
		cap["terminal"] = true;
		Dictionary win;
		win["showDocument"] = true;
		cap["window"] = win;
		Dictionary ed;
		ed["showNotification"] = true;
		ed["createNode"] = true;
		ed["createAndOpenScript"] = true;
		ed["openFile"] = true;
		cap["editor"] = ed;
		cap["available_commands"] = cmds;
		cap["tools"] = tools;
		params["clientCapabilities"] = cap;
		params["commands"] = cmds;
		params["tools"] = tools;

		send_request("initialize", params);
		return OK;
	}
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
		int id = (p_method == "initialize") ? 0 : ((p_method == "session/new") ? 1 : 2);
		pipe->store_line(JSON::stringify(rpc->make_request(p_method, p_params, id)));
	}
}

void OpenCodeACPClient::send_notification(const String &p_method, const Dictionary &p_params) {
	if (pipe.is_valid()) {
		pipe->store_line(JSON::stringify(rpc->make_notification(p_method, p_params)));
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
