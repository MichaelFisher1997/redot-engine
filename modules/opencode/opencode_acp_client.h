/**************************************************************************/
/*  opencode_acp_client.h                                                 */
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

#pragma once

#include "core/object/ref_counted.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "modules/jsonrpc/jsonrpc.h"

class OpenCodeACPClient : public RefCounted {
	GDCLASS(OpenCodeACPClient, RefCounted);

	OS::ProcessID process_id = 0;
	Ref<FileAccess> pipe;

	Thread thread;
	SafeFlag thread_running;
	static void _thread_func(void *p_userdata);

	JSONRPC *rpc;
	String sessionId;
	String selected_model;

	void _handle_rpc_notification(const Dictionary &p_notification);
	void _handle_rpc_request(const Dictionary &p_request);
	void _handle_rpc_response(const Dictionary &p_response);

	// Tool Routing
	Dictionary _route_tool(const String &p_method, const Dictionary &p_params);

	// Tool implementations
	Dictionary _handle_fs_read_text_file(const Dictionary &p_params);
	Dictionary _handle_fs_write_text_file(const Dictionary &p_params);
	Dictionary _handle_fs_list_directory(const Dictionary &p_params);
	Dictionary _handle_terminal_execute(const Dictionary &p_params);
	Dictionary _handle_editor_show_notification(const Dictionary &p_params);
	Dictionary _handle_editor_create_node(const Dictionary &p_params);
	Dictionary _handle_editor_create_and_open_script(const Dictionary &p_params);
	Dictionary _handle_editor_open_file(const Dictionary &p_params);

	String _sanitize_path(const String &p_path);
	void _on_process_exited();

protected:
	static void _bind_methods();

public:
	Error start();
	void stop();

	void send_request(const String &p_method, const Dictionary &p_params);
	void send_request(const String &p_method, const Dictionary &p_params, int p_id);
	void send_notification(const String &p_method, const Dictionary &p_params);
	void send_response(const Variant &p_id, const Dictionary &p_result, const Dictionary &p_error = Dictionary());

	void execute_tool(const String &p_method, const Dictionary &p_params, const String &p_call_id);

	void set_model(const String &p_model) { selected_model = p_model; }
	String get_model() const { return selected_model; }
	String get_sessionId() const { return sessionId; }

	OpenCodeACPClient();
	~OpenCodeACPClient();
};
