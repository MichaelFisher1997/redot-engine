/**************************************************************************/
/*  opencode_editor_plugin.cpp                                            */
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

#include "opencode_editor_plugin.h"

#include "editor/editor_node.h"

void OpenCodeEditorPlugin::_on_input_submitted(const String &p_text) {
	if (p_text.strip_edges().is_empty()) {
		return;
	}

	chat_log->add_text("\nUser: " + p_text + "\n");
	input_field->set_text("");

	Dictionary params;
	params["sessionId"] = client->get_sessionId();

	Array prompt;
	Dictionary block;
	block["type"] = "text";
	block["text"] = p_text;
	prompt.push_back(block);

	params["prompt"] = prompt;
	client->send_request("session/prompt", params);
}

void OpenCodeEditorPlugin::_on_client_message(const Dictionary &p_message) {
	if (p_message.has("method")) {
		String method = p_message["method"];
		if (method == "window/logMessage") {
			Dictionary params = p_message["params"];
			chat_log->add_text("\nAgent Log: " + String(params["message"]) + "\n");
		} else if (method == "session/update") {
			Dictionary params = p_message["params"];
			Dictionary update = params["update"];
			if (update.has("sessionUpdate") && String(update["sessionUpdate"]) == "agent_message_chunk") {
				Dictionary content = update["content"];
				if (content.has("text")) {
					chat_log->add_text(content["text"]);
				}
			}
		}
	} else if (p_message.has("result")) {
		Variant result = p_message["result"];
		if (result.get_type() == Variant::DICTIONARY) {
			Dictionary d = result;
			if (d.has("capabilities")) {
				chat_log->add_text("\nAgent Connected and Ready.\n");
				client->send_notification("initialized", Dictionary());
			}
		}
	}
}

void OpenCodeEditorPlugin::_on_model_selected(int p_index) {
	String model = model_selector->get_item_text(p_index);
	client->set_model(model);
	client->stop();
	client->start();
	chat_log->add_text("\nSwitched to model: " + model + "\n");
}

void OpenCodeEditorPlugin::_populate_models() {
	List<String> args;
	args.push_back("models");
	String output;
	Error err = OS::get_singleton()->execute("opencode", args, &output);
	if (err == OK) {
		Vector<String> lines = output.split("\n");
		model_selector->clear();
		for (int i = 0; i < lines.size(); i++) {
			String m = lines[i].strip_edges();
			if (!m.is_empty()) {
				model_selector->add_item(m);
			}
		}
	}
}

void OpenCodeEditorPlugin::_notification(int p_what) {
	// Handled in constructor
}

void OpenCodeEditorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_input_submitted", "text"), &OpenCodeEditorPlugin::_on_input_submitted);
	ClassDB::bind_method(D_METHOD("_on_client_message", "message"), &OpenCodeEditorPlugin::_on_client_message);
	ClassDB::bind_method(D_METHOD("_on_model_selected", "index"), &OpenCodeEditorPlugin::_on_model_selected);
}

void OpenCodeEditorPlugin::initialize() {
	EditorNode::add_editor_plugin(memnew(OpenCodeEditorPlugin));
}

OpenCodeEditorPlugin::OpenCodeEditorPlugin() {
	main_control = memnew(VBoxContainer);
	main_control->set_name("OpenCode");

	HBoxContainer *toolbar = memnew(HBoxContainer);
	main_control->add_child(toolbar);

	toolbar->add_child(memnew(Label("Model: ")));
	model_selector = memnew(OptionButton);
	model_selector->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	model_selector->connect("item_selected", callable_mp(this, &OpenCodeEditorPlugin::_on_model_selected));
	toolbar->add_child(model_selector);

	chat_log = memnew(RichTextLabel);
	chat_log->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	chat_log->set_scroll_follow(true);
	chat_log->set_selection_enabled(true);
	chat_log->set_context_menu_enabled(true);
	main_control->add_child(chat_log);

	input_field = memnew(LineEdit);
	input_field->set_placeholder("Ask OpenCode...");
	input_field->connect("text_submitted", callable_mp(this, &OpenCodeEditorPlugin::_on_input_submitted));
	main_control->add_child(input_field);

	// Add to bottom panel
	add_control_to_bottom_panel(main_control, "OpenCode");

	client.instantiate();
	client->connect("message_received", callable_mp(this, &OpenCodeEditorPlugin::_on_client_message));

	_populate_models();
	if (model_selector->get_item_count() > 0) {
		client->set_model(model_selector->get_item_text(0));
	}

	client->start();

	chat_log->add_text("OpenCode ACP Client starting...\n");
}

OpenCodeEditorPlugin::~OpenCodeEditorPlugin() {
	// Cleanup if needed
}
