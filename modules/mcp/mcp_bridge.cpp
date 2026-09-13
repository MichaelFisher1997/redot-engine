/**************************************************************************/
/*  mcp_bridge.cpp                                                        */
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

/**
 * @file mcp_bridge.cpp
 *
 * [Add any documentation that applies to the entire file here!]
 */

#include "mcp_bridge.h"

#include "core/crypto/crypto_core.h"
#include "core/input/input.h"
#include "core/input/input_enums.h"
#include "core/input/input_map.h"
#include "core/io/json.h"
#include "core/math/math_funcs.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "scene/2d/node_2d.h"
#include "scene/gui/control.h"
#include "scene/main/canvas_item.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"
#include <functional>

MCPBridge *MCPBridge::singleton = nullptr;

static const int MCP_MAX_REQUEST_FRAME_SIZE = 1024 * 1024;
static const int MCP_MAX_RESPONSE_FRAME_SIZE = 64 * 1024 * 1024;
static const int MCP_MAX_TIMEOUT_MSEC = 120000;

MCPBridge::MCPBridge() {
	singleton = this;
	server.instantiate();
}

MCPBridge::~MCPBridge() {
	singleton = nullptr;
}

void MCPBridge::_bind_methods() {
	ClassDB::bind_method(D_METHOD("update"), &MCPBridge::update);
}

Error MCPBridge::start_server(int p_port) {
	MutexLock lock(mutex);
	is_host = true;
	if (p_port == 0) {
		for (int i = 10000; i < 11000; i++) {
			if (server->listen(i, IPAddress("127.0.0.1")) == OK) {
				port = i;
				fprintf(stderr, "[MCP] Bridge server listening on port %d\n", port);
				return OK;
			}
		}
		return ERR_ALREADY_IN_USE;
	} else {
		Error err = server->listen(p_port, IPAddress("127.0.0.1"));
		if (err == OK) {
			port = p_port;
			fprintf(stderr, "[MCP] Bridge server listening on port %d\n", port);
		}
		return err;
	}
}

bool MCPBridge::is_client_connected() const {
	MutexLock lock(mutex);
	if (connection.is_valid()) {
		Error poll_err = connection->poll();
		if (poll_err == OK && connection->get_status() == StreamPeerTCP::STATUS_CONNECTED) {
			return true;
		}
		if (poll_err != OK || connection->get_status() != StreamPeerTCP::STATUS_CONNECTING) {
			partial_data.clear();
			host_receive_buffer.clear();
		}
	}
	return false;
}

Error MCPBridge::connect_to_server(const String &p_host, int p_port) {
	MutexLock lock(mutex);
	is_host = false;
	if (connection.is_valid()) {
		connection->disconnect_from_host();
		connection.unref();
	}
	partial_data.clear();
	host_receive_buffer.clear();
	connection.instantiate();
	port = p_port;
	fprintf(stderr, "[MCP] Game process connecting to bridge at %s:%d\n", p_host.utf8().get_data(), port);
	return connection->connect_to_host(p_host, p_port);
}

Dictionary MCPBridge::send_command(const String &p_action, const Dictionary &p_args, bool p_wait_for_response, int p_timeout_msec) {
	// Keep the mutex for the complete request/response exchange. In particular,
	// update() must not poll or consume a response belonging to this command.
	MutexLock lock(mutex);
	Error poll_err = connection.is_valid() ? connection->poll() : ERR_UNAVAILABLE;
	if (!connection.is_valid() || poll_err != OK || connection->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		if (connection.is_valid()) {
			connection->disconnect_from_host();
			connection.unref();
			partial_data.clear();
			host_receive_buffer.clear();
		}
		Dictionary err;
		err["error"] = "Bridge not connected";
		return err;
	}
	Ref<StreamPeerTCP> conn = connection;

	Dictionary cmd;
	cmd["action"] = p_action;
	cmd["args"] = p_args;
	String command_id = String::num_uint64(next_command_id++);
	cmd["id"] = command_id;
	cmd["expects_response"] = p_wait_for_response;

	String json = JSON::stringify(cmd);
	CharString utf8 = json.utf8();
	const uint8_t newline = '\n';
	if (utf8.length() + 1 > MCP_MAX_REQUEST_FRAME_SIZE) {
		Dictionary err;
		err["error"] = "Bridge request exceeds frame limit";
		return err;
	}
	if (conn->put_data((const uint8_t *)utf8.get_data(), utf8.length()) != OK || conn->put_data(&newline, 1) != OK) {
		conn->disconnect_from_host();
		if (connection == conn) {
			connection.unref();
		}
		host_receive_buffer.clear();
		Dictionary err;
		err["error"] = "Bridge write failed";
		return err;
	}
	if (!p_wait_for_response) {
		return Dictionary();
	}

	// Wait for a matching response (blocking with timeout).
	uint64_t start_time = OS::get_singleton()->get_ticks_msec();
	uint64_t timeout_msec = CLAMP(p_timeout_msec > 0 ? p_timeout_msec : 5000, 1, MCP_MAX_TIMEOUT_MSEC);
	int receive_scan_offset = 0;
	while (OS::get_singleton()->get_ticks_msec() - start_time < timeout_msec) {
		if (conn->poll() != OK || conn->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			conn->disconnect_from_host();
			if (connection == conn) {
				connection.unref();
			}
			host_receive_buffer.clear();
			Dictionary err;
			err["error"] = "Bridge connection lost";
			return err;
		}
		int avail = conn->get_available_bytes();
		if (avail < 0) {
			conn->disconnect_from_host();
			if (connection == conn) {
				connection.unref();
			}
			host_receive_buffer.clear();
			Dictionary err;
			err["error"] = "Bridge read failed";
			return err;
		}
		bool read_data = false;
		if (avail > 0) {
			uint8_t buf[4096];
			int to_read = avail < (int)sizeof(buf) ? avail : (int)sizeof(buf);
			int read_bytes = 0;
			if (conn->get_partial_data(buf, to_read, read_bytes) != OK) {
				conn->disconnect_from_host();
				if (connection == conn) {
					connection.unref();
				}
				host_receive_buffer.clear();
				Dictionary err;
				err["error"] = "Bridge read failed";
				return err;
			}
			if (read_bytes > 0) {
				int old_len = host_receive_buffer.size();
				host_receive_buffer.resize(old_len + read_bytes);
				uint8_t *dst = host_receive_buffer.ptrw();
				memcpy(dst + old_len, buf, read_bytes);
				read_data = true;
			}
		}

		while (true) {
			int newline_pos = -1;
			for (int i = receive_scan_offset; i < host_receive_buffer.size(); i++) {
				if (host_receive_buffer[i] == '\n') {
					newline_pos = i;
					break;
				}
			}

			if (newline_pos < 0) {
				receive_scan_offset = host_receive_buffer.size();
				if (host_receive_buffer.size() > MCP_MAX_RESPONSE_FRAME_SIZE) {
					conn->disconnect_from_host();
					if (connection == conn) {
						connection.unref();
					}
					host_receive_buffer.clear();
					Dictionary err;
					err["error"] = "Bridge response exceeds frame limit";
					return err;
				}
				break;
			}
			if (newline_pos > MCP_MAX_RESPONSE_FRAME_SIZE) {
				conn->disconnect_from_host();
				if (connection == conn) {
					connection.unref();
				}
				host_receive_buffer.clear();
				Dictionary err;
				err["error"] = "Bridge response exceeds frame limit";
				return err;
			}

			String response_str = newline_pos > 0 ? String::utf8((const char *)host_receive_buffer.ptr(), newline_pos) : String();
			host_receive_buffer = host_receive_buffer.slice(newline_pos + 1);
			receive_scan_offset = 0;
			Variant res_var = JSON::parse_string(response_str);
			if (res_var.get_type() != Variant::DICTIONARY) {
				continue;
			}

			Dictionary response = res_var;
			if (response.has("id") && response["id"] != command_id) {
				continue;
			}
			response.erase("id");
			return response;
		}
		if (!read_data) {
			OS::get_singleton()->delay_usec(1000);
		}
	}

	// A late legacy response has no ID and cannot be distinguished from a
	// later request. Drop the connection so it cannot poison the next command.
	conn->disconnect_from_host();
	if (connection == conn) {
		connection.unref();
	}
	host_receive_buffer.clear();
	Dictionary err;
	err["error"] = "Bridge timeout";
	return err;
}

void MCPBridge::disconnect_peer() {
	MutexLock lock(mutex);
	if (connection.is_valid()) {
		connection->disconnect_from_host();
		connection.unref();
	}
	partial_data.clear();
	host_receive_buffer.clear();
}

void MCPBridge::update() {
	MutexLock lock(mutex);
	if (is_host) {
		bool current_ok = false;
		if (connection.is_valid()) {
			current_ok = connection->poll() == OK && connection->get_status() == StreamPeerTCP::STATUS_CONNECTED;
			if (!current_ok) {
				connection->disconnect_from_host();
				connection.unref();
				partial_data.clear();
				host_receive_buffer.clear();
			}
		}

		if (server->is_connection_available()) {
			// Only replace the connection if the current one is stale/disconnected,
			// so we never drop a peer mid-command from the bridge thread.
			if (!current_ok) {
				connection = server->take_connection();
				partial_data.clear();
				host_receive_buffer.clear();
				fprintf(stderr, "[MCP] Game process connected to bridge on host side\n");
			} else {
				// Drain and discard the extra pending connection.
				Ref<StreamPeerTCP> extra = server->take_connection();
				extra->disconnect_from_host();
				fprintf(stderr, "[MCP] Bridge: extra pending connection discarded (active connection in use)\n");
			}
		}
	} else {
		// Client (Game) side
		if (connection.is_valid()) {
			Error poll_err = connection->poll();
			if (poll_err != OK || connection->get_status() == StreamPeerTCP::STATUS_ERROR || connection->get_status() == StreamPeerTCP::STATUS_NONE) {
				connection->disconnect_from_host();
				connection.unref();
				partial_data.clear();
				host_receive_buffer.clear();
				return;
			}
			if (connection->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
				return;
			}

			Ref<StreamPeerTCP> conn = connection;
			while (true) {
				int available = conn->get_available_bytes();
				if (available < 0) {
					conn->disconnect_from_host();
					if (connection == conn) {
						connection.unref();
					}
					partial_data.clear();
					host_receive_buffer.clear();
					break;
				}
				if (available == 0) {
					break;
				}

				uint8_t c;
				int read_bytes = 0;
				if (conn->get_partial_data(&c, 1, read_bytes) != OK) {
					conn->disconnect_from_host();
					if (connection == conn) {
						connection.unref();
					}
					partial_data.clear();
					host_receive_buffer.clear();
					break;
				}
				if (read_bytes == 0) {
					break;
				}

				if (c != '\n') {
					partial_data.append(c);
					if (partial_data.size() > MCP_MAX_REQUEST_FRAME_SIZE) {
						conn->disconnect_from_host();
						if (connection == conn) {
							connection.unref();
						}
						partial_data.clear();
						host_receive_buffer.clear();
						break;
					}
					continue;
				}

				String cmd_str = partial_data.is_empty() ? String() : String::utf8((const char *)partial_data.ptr(), partial_data.size());
				partial_data.clear();

				mutex.unlock();
				Variant cmd_var = JSON::parse_string(cmd_str);
				Dictionary resp;
				bool send_response = false;
				if (cmd_var.get_type() == Variant::DICTIONARY) {
					Dictionary cmd = cmd_var;
					resp = _process_command(cmd);
					if (cmd.has("id")) {
						resp["id"] = cmd["id"];
					}
					send_response = cmd.get("expects_response", true);
				}
				mutex.lock();

				if (send_response && connection == conn) {
					if (conn->poll() != OK || conn->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
						conn->disconnect_from_host();
						connection.unref();
						partial_data.clear();
						host_receive_buffer.clear();
						break;
					}
					String resp_json = JSON::stringify(resp);
					CharString utf8 = resp_json.utf8();
					const uint8_t newline = '\n';
					if (conn->put_data((const uint8_t *)utf8.get_data(), utf8.length()) != OK || conn->put_data(&newline, 1) != OK) {
						conn->disconnect_from_host();
						if (connection == conn) {
							connection.unref();
						}
						partial_data.clear();
						host_receive_buffer.clear();
						break;
					}
				}
			}
		}
	}
}

void MCPBridge::_trigger_action_event(const StringName &p_action) {
	if (!InputMap::get_singleton()->has_action(p_action)) {
		return;
	}

	const List<Ref<InputEvent>> *events_ptr = InputMap::get_singleton()->action_get_events(p_action);
	if (!events_ptr) {
		return;
	}
	const List<Ref<InputEvent>> &events = *events_ptr;

	for (const Ref<InputEvent> &ev : events) {
		// Try to find a Key or Mouse Button to simulate
		Ref<InputEventKey> key_ev = ev;
		Ref<InputEventMouseButton> mouse_ev = ev;

		if (key_ev.is_valid() || mouse_ev.is_valid()) {
			Ref<InputEvent> press_ev = ev->duplicate();
			Ref<InputEvent> release_ev = ev->duplicate();

			if (key_ev.is_valid()) {
				Ref<InputEventKey> k = press_ev;
				k->set_pressed(true);
				Ref<InputEventKey> k_up = release_ev;
				k_up->set_pressed(false);
			} else if (mouse_ev.is_valid()) {
				Ref<InputEventMouseButton> m = press_ev;
				m->set_pressed(true);
				Ref<InputEventMouseButton> m_up = release_ev;
				m_up->set_pressed(false);
			}

			Input::get_singleton()->parse_input_event(press_ev);
			OS::get_singleton()->delay_usec(50000); // 50ms hold
			Input::get_singleton()->parse_input_event(release_ev);
			return; // Triggered one valid event for the action
		}
	}
}

Dictionary MCPBridge::_process_command(const Dictionary &p_cmd) {
	String action = p_cmd.get("action", "");
	Dictionary args = p_cmd.get("args", Dictionary());
	Dictionary resp;

	fprintf(stderr, "[MCP] Bridge processing command: %s\n", action.utf8().get_data());

	if (action == "capture") {
		SceneTree *st = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
		if (st) {
			Window *vp = st->get_root();
			if (!vp) {
				resp["error"] = "No root window found";
				return resp;
			}
			Ref<Texture2D> tex = vp->get_texture();
			if (tex.is_null()) {
				resp["error"] = "Viewport texture is missing";
				return resp;
			}
			Ref<Image> img = tex->get_image();
			if (img.is_null()) {
				resp["error"] = "Viewport returned no image (not rendered yet or headless)";
				return resp;
			}

			float scale = args.get("scale", 1.0);
			if (!Math::is_finite(scale) || scale < 0.1 || scale > 1.0) {
				resp["error"] = "Capture scale must be between 0.1 and 1.0";
				return resp;
			}
			if (scale != 1.0) {
				img->resize(img->get_width() * scale, img->get_height() * scale);
			}

			PackedByteArray png_buffer = img->save_png_to_buffer();
			if (png_buffer.is_empty()) {
				resp["error"] = "Failed to encode PNG";
				return resp;
			}
			resp["image_base64"] = CryptoCore::b64_encode_str(png_buffer.ptr(), png_buffer.size());
			resp["format"] = "png";
			resp["width"] = img->get_width();
			resp["height"] = img->get_height();
		} else {
			resp["error"] = "No scene tree found";
		}
	} else if (action == "click") {
		Vector2 pos;
		if (args.has("node_path")) {
			SceneTree *st = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
			if (!st) {
				resp["error"] = "No scene tree found";
				return resp;
			}
			Window *root_window = st->get_root();
			if (!root_window) {
				resp["error"] = "No root window found";
				return resp;
			}
			Node *node = root_window->get_node_or_null(args["node_path"]);
			Control *ctrl = Object::cast_to<Control>(node);
			if (ctrl) {
				pos = ctrl->get_screen_transform().get_origin() + ctrl->get_size() / 2.0;
			} else if (Object::cast_to<Node2D>(node)) {
				pos = Object::cast_to<Node2D>(node)->get_global_position();
			} else {
				resp["error"] = "Node not found or not a 2D element";
				return resp;
			}
		} else {
			pos = Vector2(args.get("x", 0), args.get("y", 0));
		}

		// 1. Move mouse to position
		Ref<InputEventMouseMotion> mm;
		mm.instantiate();
		mm->set_position(pos);
		mm->set_global_position(pos);
		Input::get_singleton()->parse_input_event(mm);

		// 2. Press down
		Ref<InputEventMouseButton> ev_down;
		ev_down.instantiate();
		ev_down->set_position(pos);
		ev_down->set_global_position(pos);
		ev_down->set_button_index(MouseButton::LEFT);
		ev_down->set_button_mask(MouseButtonMask::LEFT);
		ev_down->set_pressed(true);
		Input::get_singleton()->parse_input_event(ev_down);

		// 3. Wait 50ms to ensure engine processes the down state
		OS::get_singleton()->delay_usec(50000);

		// 4. Release
		Ref<InputEventMouseButton> ev_up;
		ev_up.instantiate();
		ev_up->set_position(pos);
		ev_up->set_global_position(pos);
		ev_up->set_button_index(MouseButton::LEFT);
		ev_up->set_button_mask(MouseButtonMask::NONE);
		ev_up->set_pressed(false);
		Input::get_singleton()->parse_input_event(ev_up);

		resp["status"] = "clicked";
		resp["pos"] = pos;
	} else if (action == "inspect_live") {
		SceneTree *st = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
		if (st) {
			Window *root_window = st->get_root();
			if (!root_window) {
				resp["error"] = "No root window found";
				return resp;
			}
			String path = args.get("path", ".");
			Node *node = (path == "." || path.is_empty()) ? (Node *)root_window : root_window->get_node_or_null(path);
			if (node) {
				bool recursive = args.get("recursive", false);
				int max_depth = args.get("depth", 5);

				std::function<Dictionary(Node *, int)> build_tree = [&](Node *p_node, int p_depth) -> Dictionary {
					Dictionary info;
					info["name"] = p_node->get_name();
					info["type"] = p_node->get_class();

					CanvasItem *ci = Object::cast_to<CanvasItem>(p_node);
					info["visible"] = ci ? ci->is_visible_in_tree() : true;

					if (Control *c = Object::cast_to<Control>(p_node)) {
						Vector2 sp = c->get_screen_transform().get_origin() + c->get_size() / 2.0;
						info["screen_pos"] = sp;
						info["size"] = c->get_size();
					} else if (Node2D *n2 = Object::cast_to<Node2D>(p_node)) {
						info["pos"] = n2->get_global_position();
					}

					if (recursive && p_depth < max_depth) {
						Array children;
						for (int i = 0; i < p_node->get_child_count(); i++) {
							Node *child = p_node->get_child(i);
							// Skip internal nodes to keep context clean
							String cname = child->get_name();
							if (cname.begins_with("@@")) {
								continue;
							}
							children.push_back(build_tree(child, p_depth + 1));
						}
						info["children"] = children;
					}
					return info;
				};

				resp["tree"] = build_tree(node, 0);
			} else {
				resp["error"] = "Node not found";
			}
		}
	} else if (action == "type") {
		String text = args.get("text", "");

		// Smart Fallback: Check if the text matches an Input Action (e.g. "ui_cancel")
		if (text.length() > 1 && !text.begins_with("[") && InputMap::get_singleton()->has_action(text)) {
			_trigger_action_event(text);
			resp["status"] = "triggered_action_fallback";
			return resp;
		}

		for (int i = 0; i < text.length(); i++) {
			String token;
			if (text[i] == '[' && text.find("]", i) != -1) {
				int end = text.find("]", i);
				token = text.substr(i + 1, end - i - 1);
				i = end;
			} else {
				token = String::chr(text[i]);
			}

			uint32_t k_val = (uint32_t)find_keycode(token);
			if (k_val == 0 && token.length() == 1) {
				k_val = (uint32_t)token.to_upper()[0];
			}

			Key code = (Key)(k_val & (uint32_t)KeyModifierMask::CODE_MASK);
			uint32_t mods = k_val & (uint32_t)KeyModifierMask::MODIFIER_MASK;
			char32_t unicode = 0;
			if (token.length() == 1) {
				unicode = token[0];
			}

			// Send Press
			Ref<InputEventKey> ev_down;
			ev_down.instantiate();
			ev_down->set_keycode(code);
			ev_down->set_physical_keycode(code);
			ev_down->set_unicode(unicode);
			ev_down->set_pressed(true);
			ev_down->set_shift_pressed(mods & (uint32_t)KeyModifierMask::SHIFT);
			ev_down->set_ctrl_pressed(mods & (uint32_t)KeyModifierMask::CTRL);
			ev_down->set_alt_pressed(mods & (uint32_t)KeyModifierMask::ALT);
			ev_down->set_meta_pressed(mods & (uint32_t)KeyModifierMask::META);
			Input::get_singleton()->parse_input_event(ev_down);

			// Send Release
			Ref<InputEventKey> ev_up = ev_down->duplicate();
			ev_up->set_pressed(false);
			Input::get_singleton()->parse_input_event(ev_up);
		}
		resp["status"] = "typed";
	} else if (action == "trigger_action") {
		String action_name = args.get("action_name", "");
		if (action_name.is_empty()) {
			resp["error"] = "Missing action_name";
		} else if (!InputMap::get_singleton()->has_action(action_name)) {
			resp["error"] = "Action not found: " + action_name;
		} else {
			_trigger_action_event(action_name);
			resp["status"] = "triggered_action";
		}
	} else if (action == "quit") {
		SceneTree *st = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
		if (st) {
			st->quit();
			resp["status"] = "quitting";
		} else {
			resp["error"] = "No scene tree found";
		}
	} else if (action == "wait") {
		resp["status"] = "wait_is_server_side";
	} else {
		resp["status"] = "error";
		resp["error"] = "unknown_action";
		resp["action"] = action;
	}

	return resp;
}
