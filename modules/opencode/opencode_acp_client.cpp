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
		if (self->pipe.is_valid() && !self->pipe->eof_reached()) {
			String line = self->pipe->get_line();
			if (!line.is_empty()) {
				Variant msg = JSON::parse_string(line);
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
				}
			}
		}
		OS::get_singleton()->delay_usec(10000);
	}
}

void OpenCodeACPClient::_handle_rpc_notification(const Dictionary &p_notification) {
	emit_signal(SNAME("message_received"), p_notification);
}

void OpenCodeACPClient::_handle_rpc_request(const Dictionary &p_request) {
	emit_signal(SNAME("message_received"), p_request);
}

void OpenCodeACPClient::_handle_rpc_response(const Dictionary &p_response) {
	if (p_response.has("id")) {
		int id = p_response["id"];
		if (id == 0) { // initialize response
			Dictionary result = p_response["result"];
			send_notification("initialized", Dictionary());

			// Create session
			Dictionary session_params;
			String resource_path = ProjectSettings::get_singleton()->get_resource_path();
			if (resource_path.is_empty()) {
				resource_path = OS::get_singleton()->get_executable_path().get_base_dir();
			}
			session_params["cwd"] = ProjectSettings::get_singleton()->globalize_path(resource_path);
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

	ADD_SIGNAL(MethodInfo("message_received", PropertyInfo(Variant::DICTIONARY, "message")));
}

Error OpenCodeACPClient::start() {
	List<String> args;
	args.push_back("acp");
	if (!selected_model.is_empty()) {
		args.push_back("--model");
		args.push_back(selected_model);
	}

	Dictionary res = OS::get_singleton()->execute_with_pipe("opencode", args);
	if (res.has("pid")) {
		process_id = res["pid"];
		pipe = res["stdio"];

		thread_running.set();
		thread.start(_thread_func, this);

		// Send initialize request
		Dictionary params;
		params["protocolVersion"] = 1;

		Dictionary client_info;
		client_info["name"] = "redot-engine";
		client_info["title"] = "Redot Engine";
		client_info["version"] = "1.0.0";
		params["clientInfo"] = client_info;

		Dictionary capabilities;
		params["clientCapabilities"] = capabilities;

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
