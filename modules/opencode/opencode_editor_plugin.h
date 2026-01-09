/**************************************************************************/
/*  opencode_editor_plugin.h                                              */
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

#pragma once

#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/progress_bar.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/texture_rect.h"

#include "opencode_acp_client.h"

class OpenCodeEditorPlugin : public EditorPlugin {
	GDCLASS(OpenCodeEditorPlugin, EditorPlugin);

	// Main UI
	Control *main_control = nullptr;
	VBoxContainer *chat_container = nullptr;
	ScrollContainer *scroll_container = nullptr;
	VBoxContainer *messages_container = nullptr;
	HBoxContainer *input_container = nullptr;
	LineEdit *input_field = nullptr;
	Button *send_button = nullptr;
	Button *stop_button = nullptr;

	// Toolbar
	HBoxContainer *toolbar = nullptr;
	OptionButton *model_selector = nullptr;
	Label *status_label = nullptr;

	// State tracking
	Ref<OpenCodeACPClient> client;
	bool session_ready = false;
	bool is_processing = false;

	// Current message being streamed
	RichTextLabel *current_thinking_label = nullptr;
	RichTextLabel *current_message_label = nullptr;
	PanelContainer *current_message_panel = nullptr;
	String current_thinking_text;
	String current_message_text;

	enum BlockType {
		BLOCK_THINKING,
		BLOCK_AGENT
	};
	BlockType current_block_type = BLOCK_AGENT;
	String parse_buffer;
	bool in_thinking_tag = false;

	Dictionary pending_tool_calls;

	// UI Creation helpers
	void _create_toolbar();
	void _create_chat_area();
	void _create_input_area();

	// Message display
	void _add_user_message(const String &p_text);
	void _add_agent_message(const String &p_text);
	void _add_thinking_block(const String &p_text);
	void _add_system_message(const String &p_text, const Color &p_color = Color(0.7, 0.7, 0.7));
	void _add_tool_call(const String &p_tool, const String &p_args);
	void _add_tool_result(const String &p_result, bool p_success = true);
	void _scroll_to_bottom();

	// Streaming helpers
	void _start_thinking_block();
	void _append_thinking(const String &p_text);
	void _end_thinking_block();
	void _start_agent_message();
	void _append_agent_message(const String &p_text);
	void _end_agent_message();
	void _process_chunk(const String &p_text, BlockType p_type);
	void _ensure_block(BlockType p_type);

	// Event handlers
	void _on_input_submitted(const String &p_text);
	void _on_send_pressed();
	void _on_stop_pressed();
	void _on_client_message(const Dictionary &p_message);
	void _on_client_connection_lost(const String &p_reason);
	void _on_model_selected(int p_index);
	void _populate_models();

	// State management
	void _set_processing(bool p_processing);
	void _update_status(const String &p_status);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	virtual String get_plugin_name() const override { return "OpenCode"; }
	bool has_main_screen() const override { return false; }

	static void initialize();

	OpenCodeEditorPlugin();
	~OpenCodeEditorPlugin();
};
