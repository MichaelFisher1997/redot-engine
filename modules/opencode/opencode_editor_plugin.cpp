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

#include "core/io/json.h"
#include "editor/editor_node.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/separator.h"
#include "scene/resources/style_box_flat.h"

// ============================================================================
// UI Creation
// ============================================================================

void OpenCodeEditorPlugin::_create_toolbar() {
	toolbar = memnew(HBoxContainer);
	toolbar->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	Label *model_label = memnew(Label);
	model_label->set_text("Model:");
	toolbar->add_child(model_label);

	model_selector = memnew(OptionButton);
	model_selector->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	model_selector->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	model_selector->connect("item_selected", callable_mp(this, &OpenCodeEditorPlugin::_on_model_selected));
	toolbar->add_child(model_selector);

	Control *spacer = memnew(Control);
	spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	toolbar->add_child(spacer);

	status_label = memnew(Label);
	status_label->set_text("Connecting...");
	status_label->add_theme_color_override("font_color", Color(0.6, 0.6, 0.6));
	toolbar->add_child(status_label);

	main_control->add_child(toolbar);
	HSeparator *sep = memnew(HSeparator);
	main_control->add_child(sep);
}

void OpenCodeEditorPlugin::_create_chat_area() {
	scroll_container = memnew(ScrollContainer);
	scroll_container->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	scroll_container->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	scroll_container->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);

	messages_container = memnew(VBoxContainer);
	messages_container->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	messages_container->add_theme_constant_override("separation", 12 * EDSCALE);
	scroll_container->add_child(messages_container);

	main_control->add_child(scroll_container);
}

void OpenCodeEditorPlugin::_create_input_area() {
	PanelContainer *input_bg = memnew(PanelContainer);
	Ref<StyleBoxFlat> input_style;
	input_style.instantiate();
	input_style->set_bg_color(Color(0.1, 0.1, 0.1, 0.0));
	input_style->set_content_margin_all(10 * EDSCALE);
	input_bg->add_theme_style_override("panel", input_style);
	main_control->add_child(input_bg);

	input_container = memnew(HBoxContainer);
	input_container->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	input_bg->add_child(input_container);

	input_field = memnew(LineEdit);
	input_field->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	input_field->set_placeholder("Ask OpenCode... (Enter to send)");
	input_field->set_editable(false);
	input_field->connect("text_submitted", callable_mp(this, &OpenCodeEditorPlugin::_on_input_submitted));
	input_container->add_child(input_field);

	send_button = memnew(Button);
	send_button->set_text("Send");
	send_button->set_disabled(true);
	send_button->connect("pressed", callable_mp(this, &OpenCodeEditorPlugin::_on_send_pressed));
	input_container->add_child(send_button);

	stop_button = memnew(Button);
	stop_button->set_text("Stop");
	stop_button->set_visible(false);
	stop_button->add_theme_color_override("font_color", Color(1, 0.4, 0.4));
	stop_button->connect("pressed", callable_mp(this, &OpenCodeEditorPlugin::_on_stop_pressed));
	input_container->add_child(stop_button);
}

// ============================================================================
// Message Display
// ============================================================================

void OpenCodeEditorPlugin::_add_user_message(const String &p_text) {
	HBoxContainer *row = memnew(HBoxContainer);
	messages_container->add_child(row);
	Control *spacer = memnew(Control);
	spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	spacer->set_stretch_ratio(1.0);
	row->add_child(spacer);

	PanelContainer *panel = memnew(PanelContainer);
	panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	panel->set_stretch_ratio(4.0);
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_bg_color(Color(0.25, 0.35, 0.6, 0.8));
	style->set_corner_radius_all(12 * EDSCALE);
	style->set_corner_radius(CORNER_BOTTOM_RIGHT, 2 * EDSCALE);
	style->set_content_margin_all(10 * EDSCALE);
	panel->add_theme_style_override("panel", style);

	VBoxContainer *vbox = memnew(VBoxContainer);
	Label *header = memnew(Label);
	header->set_text("You");
	header->add_theme_color_override("font_color", Color(0.8, 0.9, 1.0));
	header->add_theme_font_size_override("font_size", 10 * EDSCALE);
	header->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	vbox->add_child(header);

	RichTextLabel *content = memnew(RichTextLabel);
	content->set_use_bbcode(true);
	content->set_fit_content(true);
	content->set_scroll_active(false);
	content->set_selection_enabled(true);
	content->set_text(p_text);
	vbox->add_child(content);

	panel->add_child(vbox);
	row->add_child(panel);
	_scroll_to_bottom();
}

void OpenCodeEditorPlugin::_add_system_message(const String &p_text, const Color &p_color) {
	Label *label = memnew(Label);
	label->set_text(p_text);
	label->add_theme_color_override("font_color", p_color);
	label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	messages_container->add_child(label);
	_scroll_to_bottom();
}

void OpenCodeEditorPlugin::_ensure_block(BlockType p_type) {
	if (current_block_type == p_type && current_message_label) {
		return;
	}
	if (current_message_label) {
		current_message_label = nullptr;
		current_message_panel = nullptr;
		current_message_text = "";
	}
	current_block_type = p_type;
	current_message_panel = memnew(PanelContainer);
	Ref<StyleBoxFlat> style;
	style.instantiate();
	VBoxContainer *vbox = memnew(VBoxContainer);
	Label *header = memnew(Label);
	if (p_type == BLOCK_THINKING) {
		style->set_bg_color(Color(0.12, 0.12, 0.12, 0.6));
		style->set_corner_radius_all(8 * EDSCALE);
		style->set_content_margin_all(8 * EDSCALE);
		style->set_border_width_all(1);
		style->set_border_color(Color(0.3, 0.3, 0.3, 0.3));
		header->set_text("Thinking...");
		header->add_theme_color_override("font_color", Color(0.5, 0.5, 0.5));
		header->add_theme_font_size_override("font_size", 10 * EDSCALE);
		current_message_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		messages_container->add_child(current_message_panel);
	} else {
		style->set_bg_color(Color(0.18, 0.18, 0.2, 0.8));
		style->set_corner_radius_all(12 * EDSCALE);
		style->set_corner_radius(CORNER_BOTTOM_LEFT, 2 * EDSCALE);
		style->set_content_margin_all(10 * EDSCALE);
		style->set_border_width_all(1);
		style->set_border_color(Color(0.3, 0.3, 0.35, 0.5));
		header->set_text("OpenCode");
		header->add_theme_color_override("font_color", Color(0.5, 0.8, 0.6));
		header->add_theme_font_size_override("font_size", 10 * EDSCALE);
		current_message_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		current_message_panel->set_stretch_ratio(4.0);
		HBoxContainer *row = memnew(HBoxContainer);
		messages_container->add_child(row);
		row->add_child(current_message_panel);
		Control *spacer = memnew(Control);
		spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		spacer->set_stretch_ratio(1.0);
		row->add_child(spacer);
	}
	current_message_panel->add_theme_style_override("panel", style);
	vbox->add_child(header);
	current_message_label = memnew(RichTextLabel);
	current_message_label->set_use_bbcode(true);
	current_message_label->set_fit_content(true);
	current_message_label->set_scroll_active(false);
	current_message_label->set_selection_enabled(true);
	if (p_type == BLOCK_THINKING) {
		current_message_label->add_theme_color_override("default_color", Color(0.5, 0.5, 0.5));
	}
	vbox->add_child(current_message_label);
	current_message_panel->add_child(vbox);
	current_message_text = "";
}

void OpenCodeEditorPlugin::_process_chunk(const String &p_text, BlockType p_type) {
	parse_buffer += p_text;
	while (true) {
		if (!in_thinking_tag) {
			int open_tag = parse_buffer.find("<thinking>");
			if (open_tag != -1) {
				String msg = parse_buffer.substr(0, open_tag);
				if (!msg.is_empty()) {
					_ensure_block(p_type);
					_append_agent_message(msg);
				}
				parse_buffer = parse_buffer.substr(open_tag + 10);
				in_thinking_tag = true;
				_ensure_block(BLOCK_THINKING);
			} else {
				int last_lt = parse_buffer.rfind("<");
				if (last_lt != -1 && parse_buffer.length() - last_lt < 10) {
					if (last_lt > 0) {
						String safe_msg = parse_buffer.substr(0, last_lt);
						_ensure_block(p_type);
						_append_agent_message(safe_msg);
						parse_buffer = parse_buffer.substr(last_lt);
					}
					break;
				} else {
					if (!parse_buffer.is_empty()) {
						_ensure_block(p_type);
						_append_agent_message(parse_buffer);
						parse_buffer = "";
					}
					break;
				}
			}
		} else {
			int close_tag = parse_buffer.find("</thinking>");
			if (close_tag != -1) {
				String msg = parse_buffer.substr(0, close_tag);
				if (!msg.is_empty()) {
					_ensure_block(BLOCK_THINKING);
					_append_agent_message(msg);
				}
				parse_buffer = parse_buffer.substr(close_tag + 11);
				in_thinking_tag = false;
				_ensure_block(p_type);
			} else {
				int last_lt = parse_buffer.rfind("<");
				if (last_lt != -1 && parse_buffer.length() - last_lt < 11) {
					if (last_lt > 0) {
						String safe_msg = parse_buffer.substr(0, last_lt);
						_ensure_block(BLOCK_THINKING);
						_append_agent_message(safe_msg);
						parse_buffer = parse_buffer.substr(last_lt);
					}
					break;
				} else {
					if (!parse_buffer.is_empty()) {
						_ensure_block(BLOCK_THINKING);
						_append_agent_message(parse_buffer);
						parse_buffer = "";
					}
					break;
				}
			}
		}
	}
}

void OpenCodeEditorPlugin::_append_agent_message(const String &p_text) {
	if (!current_message_label) {
		return;
	}
	current_message_text += p_text;
	current_message_label->set_text(current_message_text);
	_scroll_to_bottom();
}

void OpenCodeEditorPlugin::_end_agent_message() {
	current_message_label = nullptr;
	current_message_panel = nullptr;
	current_message_text = "";
}

void OpenCodeEditorPlugin::_add_tool_call(const String &p_tool, const String &p_args) {
	PanelContainer *panel = memnew(PanelContainer);
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_bg_color(Color(0.3, 0.25, 0.1, 0.3));
	style->set_corner_radius_all(6 * EDSCALE);
	style->set_content_margin_all(8 * EDSCALE);
	style->set_border_width_all(1);
	style->set_border_color(Color(0.5, 0.4, 0.2, 0.5));
	panel->add_theme_style_override("panel", style);
	VBoxContainer *vbox = memnew(VBoxContainer);
	Label *header = memnew(Label);
	header->set_text("Tool: " + p_tool);
	header->add_theme_color_override("font_color", Color(0.9, 0.7, 0.3));
	header->add_theme_font_size_override("font_size", 10 * EDSCALE);
	vbox->add_child(header);
	if (!p_args.is_empty()) {
		RichTextLabel *args_label = memnew(RichTextLabel);
		args_label->set_use_bbcode(true);
		args_label->set_fit_content(true);
		args_label->set_scroll_active(false);
		args_label->set_text("[code]" + p_args + "[/code]");
		args_label->add_theme_color_override("default_color", Color(0.6, 0.6, 0.6));
		vbox->add_child(args_label);
	}
	panel->add_child(vbox);
	messages_container->add_child(panel);
	_scroll_to_bottom();
}

void OpenCodeEditorPlugin::_add_tool_result(const String &p_result, bool p_success) {
	Label *label = memnew(Label);
	label->set_text(p_success ? "Tool completed" : "Tool failed: " + p_result);
	label->add_theme_color_override("font_color", p_success ? Color(0.4, 0.7, 0.4) : Color(0.8, 0.3, 0.3));
	label->add_theme_font_size_override("font_size", 10 * EDSCALE);
	messages_container->add_child(label);
	_scroll_to_bottom();
}

void OpenCodeEditorPlugin::_scroll_to_bottom() {
	callable_mp(scroll_container, &ScrollContainer::set_v_scroll).call_deferred(999999);
}

void OpenCodeEditorPlugin::_on_input_submitted(const String &p_text) {
	if (p_text.strip_edges().is_empty() || is_processing) {
		return;
	}
	if (!session_ready) {
		return;
	}
	String session_id = client->get_sessionId();
	if (session_id.is_empty()) {
		return;
	}
	_end_agent_message();
	parse_buffer = "";
	in_thinking_tag = false;
	current_block_type = BLOCK_AGENT;
	_add_user_message(p_text);
	input_field->set_text("");
	Dictionary params;
	params["sessionId"] = session_id;

	Array prompt;
	Dictionary sys_block;
	sys_block["type"] = "text";
	sys_block["text"] = "### DEVELOPER NOTE\nYou are running inside the Redot Engine editor. Use these tools for editor actions:\n- `editor/openFile` (path/uri): Opens file in editor tabs.\n- `editor/createAndOpenScript` (path/uri, content): Creates and opens script.\n- `editor/createNode` (type, name, properties): Adds node to scene.\n- `editor/showNotification` (message): Shows editor toast.\n- `fs/listDirectory` (path): Lists files.\nPREFER `editor/openFile` over `fs/readTextFile` to show files to the user.";
	prompt.push_back(sys_block);

	Dictionary block;
	block["type"] = "text";
	block["text"] = p_text;
	prompt.push_back(block);
	params["prompt"] = prompt;
	client->send_request("session/prompt", params);
	_set_processing(true);
}

void OpenCodeEditorPlugin::_on_send_pressed() {
	_on_input_submitted(input_field->get_text());
}

void OpenCodeEditorPlugin::_on_stop_pressed() {
	String session_id = client->get_sessionId();
	if (!session_id.is_empty()) {
		Dictionary params;
		params["sessionId"] = session_id;
		// Send multiple cancellation variants to ensure the agent stops
		client->send_request("session/cancel", params);
		client->send_notification("session/cancel", params);
		client->send_request("session/stop", params);
		client->send_notification("session/interrupt", params);
	}
	_set_processing(false);
	_end_agent_message();
	parse_buffer = "";
	in_thinking_tag = false;
	_add_system_message("Cancelled", Color(0.8, 0.6, 0.2));
}

void OpenCodeEditorPlugin::_on_client_message(const Dictionary &p_message) {
	if (p_message.has("method")) {
		String method = p_message["method"];
		if (method == "session/update") {
			Dictionary params = p_message["params"];
			Dictionary update = params["update"];
			if (update.has("sessionUpdate")) {
				String type = update["sessionUpdate"];
				if (type == "agent_thought_chunk") {
					if (update.has("content") && Dictionary(update["content"]).has("text")) {
						_process_chunk(Dictionary(update["content"])["text"], BLOCK_THINKING);
					}
				} else if (type == "agent_message_chunk") {
					if (update.has("content") && Dictionary(update["content"]).has("text")) {
						_process_chunk(Dictionary(update["content"])["text"], BLOCK_AGENT);
					}
				} else if (type == "agent_message_done" || type == "turn_complete") {
					_set_processing(false);
				} else if (type == "tool_call") {
					String name = "unknown";
					static const char *keys[] = { "name", "method", "toolName", "tool", "function", "command", nullptr };

					// Deep check for name
					for (int i = 0; keys[i]; i++) {
						if (update.has(keys[i])) {
							name = update[keys[i]];
							break;
						}
					}

					// If still unknown, check if it's nested (e.g. update["call"]["name"])
					if (name == "unknown") {
						for (int i = 0; keys[i]; i++) {
							if (update.has("call") && Dictionary(update["call"]).has(keys[i])) {
								name = Dictionary(update["call"])[keys[i]];
								break;
							}
							if (update.has("toolCall") && Dictionary(update["toolCall"]).has(keys[i])) {
								name = Dictionary(update["toolCall"])[keys[i]];
								break;
							}
						}
					}

					if (name == "unknown") {
						print_line("OpenCode: Failed to find tool name in update: " + JSON::stringify(update));
					}

					_add_tool_call(name, update.has("arguments") ? JSON::stringify(update["arguments"]) : "");
				} else if (type == "tool_result") {
					_add_tool_result(update.has("result") ? String(update["result"]) : "", update.has("success") ? bool(update["success"]) : true);
				}
			}
		} else if (method != "window/logMessage" && p_message.has("id")) {
			_add_tool_call(method, p_message.has("params") ? JSON::stringify(p_message["params"]) : "");
		}
	} else if (p_message.has("result")) {
		Dictionary d = p_message["result"];
		if (d.has("sessionId")) {
			session_ready = true;
			input_field->set_editable(true);
			send_button->set_disabled(false);
			_update_status("Ready");
			_add_system_message("Session created. Ready for messages.", Color(0.4, 0.7, 0.4));
		} else if (d.has("stopReason")) {
			_set_processing(false);
		}
	} else if (p_message.has("error")) {
		_add_system_message("Error: " + (Dictionary(p_message["error"]).has("message") ? String(Dictionary(p_message["error"])["message"]) : "Unknown"), Color(0.8, 0.3, 0.3));
		_set_processing(false);
	}
}

void OpenCodeEditorPlugin::_on_client_connection_lost(const String &p_reason) {
	_add_system_message("Connection lost: " + p_reason, Color(0.8, 0.3, 0.3));
	_update_status("Disconnected");
	session_ready = false;
	input_field->set_editable(false);
	send_button->set_disabled(true);
	_set_processing(false);
}

void OpenCodeEditorPlugin::_on_model_selected(int p_index) {
	_add_system_message("Model: " + model_selector->get_item_text(p_index), Color(0.6, 0.6, 0.6));
}

void OpenCodeEditorPlugin::_populate_models() {
	model_selector->clear();
	model_selector->add_item("zen-bigpickel");
	model_selector->add_item("opencode/claude-sonnet-4");
	model_selector->add_item("opencode/gpt-5");
	model_selector->add_item("opencode/gemini-3-flash");
	model_selector->add_item("zai-coding-plan/glm-4.7");
	model_selector->select(0);
}

void OpenCodeEditorPlugin::_set_processing(bool p_p) {
	is_processing = p_p;
	input_field->set_editable(session_ready);
	send_button->set_visible(!p_p);
	send_button->set_disabled(p_p || !session_ready);
	stop_button->set_visible(p_p);
	_update_status(p_p ? "Processing..." : (session_ready ? "Ready" : "Offline"));
}

void OpenCodeEditorPlugin::_update_status(const String &p_s) {
	status_label->set_text(p_s);
	Color c = (p_s == "Ready") ? Color(0.4, 0.7, 0.4) : ((p_s == "Processing...") ? Color(0.7, 0.7, 0.3) : Color(0.8, 0.3, 0.3));
	status_label->add_theme_color_override("font_color", c);
}

void OpenCodeEditorPlugin::_notification(int p_what) {}
void OpenCodeEditorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_input_submitted", "text"), &OpenCodeEditorPlugin::_on_input_submitted);
	ClassDB::bind_method(D_METHOD("_on_send_pressed"), &OpenCodeEditorPlugin::_on_send_pressed);
	ClassDB::bind_method(D_METHOD("_on_stop_pressed"), &OpenCodeEditorPlugin::_on_stop_pressed);
	ClassDB::bind_method(D_METHOD("_on_client_message", "message"), &OpenCodeEditorPlugin::_on_client_message);
	ClassDB::bind_method(D_METHOD("_on_client_connection_lost", "reason"), &OpenCodeEditorPlugin::_on_client_connection_lost);
	ClassDB::bind_method(D_METHOD("_on_model_selected", "index"), &OpenCodeEditorPlugin::_on_model_selected);
}

void OpenCodeEditorPlugin::initialize() {
	EditorNode::add_editor_plugin(memnew(OpenCodeEditorPlugin));
}

OpenCodeEditorPlugin::OpenCodeEditorPlugin() {
	main_control = memnew(VBoxContainer);
	main_control->set_name("OpenCode");
	main_control->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	_create_toolbar();
	_create_chat_area();
	_create_input_area();
	add_control_to_bottom_panel(main_control, "OpenCode");
	client.instantiate();
	client->connect("message_received", callable_mp(this, &OpenCodeEditorPlugin::_on_client_message));
	client->connect("connection_lost", callable_mp(this, &OpenCodeEditorPlugin::_on_client_connection_lost));
	_populate_models();
	if (client->start() != OK) {
		_update_status("Error");
	}
}
OpenCodeEditorPlugin::~OpenCodeEditorPlugin() {}
