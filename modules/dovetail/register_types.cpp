/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*  Dovetail app bridge module.                                           */
/**************************************************************************/

// The app bridge: a small localhost channel built into the Dovetail editor binary, alive in
// every editor and project manager window from launch. It lets the MCP sidecar see and adjust
// Dovetail itself (window screenshot, editor settings) when no game project is open.
//
// Wire format matches the game bridge (addons/dovetail/bridge/bridge.gd): one JSON object per
// line, {"id","tool","args","token"} in, {"id","ok","result"|"error"} out. Each instance writes
// <config>/Dovetail/app_bridge/<pid>.json with its port and token and removes it on exit.
// Deliberately small: info, screenshot of its own window, editor settings. Nothing else.

#include "register_types.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/variant/variant_utility.h"
#include "core/version.h"
#include "editor/settings/editor_settings.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/display/display_server.h"

class DovetailAppBridge : public Object {
	GDSOFTCLASS(DovetailAppBridge, Object);

	static constexpr int MAX_LINE = 1 << 20;

	struct Peer {
		Ref<StreamPeerTCP> tcp;
		String buffer;
	};

	Ref<TCPServer> server;
	Vector<Peer> peers;
	String token;
	String info_path;
	Dictionary info;
	uint64_t next_settings_check_msec = 0;

	static Dictionary ok(const Variant &p_id, const Dictionary &p_result) {
		Dictionary d;
		d["id"] = p_id;
		d["ok"] = true;
		d["result"] = p_result;
		return d;
	}

	static Dictionary fail(const Variant &p_id, const String &p_code, const String &p_message, const String &p_hint = String()) {
		Dictionary e;
		e["code"] = p_code;
		e["message"] = p_message;
		if (!p_hint.is_empty()) {
			e["hint"] = p_hint;
		}
		Dictionary d;
		d["id"] = p_id;
		d["ok"] = false;
		d["error"] = e;
		return d;
	}

	// Values as plain JSON: vectors as arrays, colours as "#rrggbbaa", anything else odd as text.
	static Variant to_json(const Variant &p_value) {
		switch (p_value.get_type()) {
			case Variant::NIL:
			case Variant::BOOL:
			case Variant::INT:
			case Variant::FLOAT:
			case Variant::STRING:
				return p_value;
			case Variant::STRING_NAME:
			case Variant::NODE_PATH:
				return String(p_value);
			case Variant::VECTOR2:
			case Variant::VECTOR2I: {
				Vector2 v = p_value;
				Array a;
				a.push_back(v.x);
				a.push_back(v.y);
				return a;
			}
			case Variant::COLOR:
				return "#" + Color(p_value).to_html(true);
			case Variant::ARRAY:
			case Variant::PACKED_STRING_ARRAY:
			case Variant::PACKED_INT32_ARRAY:
			case Variant::PACKED_INT64_ARRAY:
			case Variant::PACKED_FLOAT32_ARRAY:
			case Variant::PACKED_FLOAT64_ARRAY: {
				Array src = p_value;
				Array a;
				for (int i = 0; i < src.size(); i++) {
					a.push_back(to_json(src[i]));
				}
				return a;
			}
			case Variant::DICTIONARY: {
				Dictionary src = p_value;
				Dictionary d;
				for (const Variant &k : src.keys()) {
					d[String(k)] = to_json(src[k]);
				}
				return d;
			}
			default:
				return p_value.stringify();
		}
	}

	// Incoming JSON converted to the type the setting already has (JSON numbers are floats).
	static Variant from_json(const Variant &p_value, Variant::Type p_type) {
		if (p_type == Variant::NIL || p_value.get_type() == p_type) {
			return p_value;
		}
		if (p_type == Variant::COLOR && p_value.get_type() == Variant::STRING) {
			return Color::html(p_value);
		}
		if ((p_type == Variant::VECTOR2 || p_type == Variant::VECTOR2I) && p_value.get_type() == Variant::ARRAY) {
			Array a = p_value;
			if (a.size() == 2) {
				Vector2 v(a[0], a[1]);
				return p_type == Variant::VECTOR2 ? Variant(v) : Variant(Vector2i(v));
			}
		}
		return VariantUtilityFunctions::type_convert(p_value, p_type);
	}

	Dictionary tool_info(const Variant &p_id) {
		Dictionary r = info.duplicate();
		r.erase("token");
		DisplayServer *ds = DisplayServer::get_singleton();
		r["windows"] = ds->get_window_list().size();
		Dictionary screen;
		Size2i size = ds->screen_get_size();
		Array s;
		s.push_back(size.x);
		s.push_back(size.y);
		screen["size"] = s;
		screen["scale"] = ds->screen_get_scale();
		screen["count"] = ds->get_screen_count();
		r["screen"] = screen;
		return ok(p_id, r);
	}

	Dictionary tool_screenshot(const Variant &p_id, const Dictionary &p_args) {
		// The editor only redraws when something changes, so do not wait for a frame: the root
		// viewport's texture holds the last drawn frame, which is what the human sees.
		SceneTree *tree = SceneTree::get_singleton();
		if (!tree || !tree->get_root()) {
			return fail(p_id, "not_ready", "No main window yet");
		}
		Ref<Image> img = tree->get_root()->get_texture()->get_image();
		if (img.is_null() || img->is_empty()) {
			Dictionary r;
			r["note"] = "the window has not drawn anything yet; try again in a moment";
			return ok(p_id, r);
		}
		double scale = p_args.get("scale", 0.5);
		if (scale > 0.0 && scale < 1.0) {
			img->resize(MAX(1, int(img->get_width() * scale)), MAX(1, int(img->get_height() * scale)), Image::INTERPOLATE_LANCZOS);
		}
		String path = p_args.get("path", String());
		if (path.is_empty()) {
			path = OS::get_singleton()->get_cache_path().path_join("dovetail").path_join(vformat("app_%d.png", OS::get_singleton()->get_ticks_msec()));
		}
		DirAccess::make_dir_recursive_absolute(path.get_base_dir());
		Error err = img->save_png(path);
		if (err != OK) {
			return fail(p_id, "capture_failed", vformat("Could not save %s: %s", path, error_names[err]));
		}
		Dictionary r;
		r["path"] = path;
		r["width"] = img->get_width();
		r["height"] = img->get_height();
		// A black frame means the window had not drawn (or was minimised); say so, do not fail.
		bool black = true;
		const int step = MAX(1, MIN(img->get_width(), img->get_height()) / 64);
		for (int y = 0; y < img->get_height() && black; y += step) {
			for (int x = 0; x < img->get_width(); x += step) {
				Color c = img->get_pixel(x, y);
				if (c.r > 0.02 || c.g > 0.02 || c.b > 0.02) {
					black = false;
					break;
				}
			}
		}
		if (black) {
			r["note"] = "the captured image is fully black; the window may be minimised or not drawn yet";
		}
		return ok(p_id, r);
	}

	Dictionary tool_setting(const Variant &p_id, const Dictionary &p_args) {
		EditorSettings *es = EditorSettings::get_singleton();
		if (!es) {
			return fail(p_id, "not_ready", "Editor settings are not loaded");
		}
		es->reload_if_changed_on_disk(); // Answer with what the file holds now.
		String name = p_args.get("name", String());
		if (name.is_empty()) {
			return fail(p_id, "bad_args", "name is required", "e.g. run/window_placement/rect");
		}
		if (!es->has_setting(name)) {
			// Suggest the closest existing names.
			List<PropertyInfo> props;
			es->get_property_list(&props);
			Vector<Pair<float, String>> scored;
			for (const PropertyInfo &pi : props) {
				if (pi.usage & PROPERTY_USAGE_EDITOR) {
					scored.push_back(Pair<float, String>(-name.similarity(pi.name), pi.name));
				}
			}
			scored.sort_custom<PairSort<float, String>>();
			PackedStringArray close;
			for (int i = 0; i < MIN(5, scored.size()); i++) {
				close.push_back(scored[i].second);
			}
			return fail(p_id, "unknown_setting", name, "Similar: " + String(", ").join(close));
		}
		Variant current = es->get_setting(name);
		bool changed = false;
		if (p_args.has("value")) {
			Variant value = from_json(p_args["value"], current.get_type());
			if (value != current) {
				// set_setting marks it changed in this window, so the save below merges: other
				// settings changed on disk meanwhile are kept, this one is written.
				es->set_setting(name, value);
				// Same order as the Editor Settings dialog: tell the editor, then write the file.
				es->notify_changes();
				EditorSettings::save();
				changed = true;
			}
			current = es->get_setting(name);
		}
		Dictionary r;
		r["name"] = name;
		r["value"] = to_json(current);
		r["type"] = Variant::get_type_name(current.get_type());
		r["changed"] = changed;
		return ok(p_id, r);
	}

	// Returns the reply and whether to close the connection afterwards.
	Dictionary dispatch(const String &p_line, bool &r_close) {
		Variant parsed = JSON::parse_string(p_line);
		if (parsed.get_type() != Variant::DICTIONARY) {
			r_close = true;
			return fail(0, "bad_json", "Could not parse request");
		}
		Dictionary req = parsed;
		Variant id = req.get("id", 0);
		if (id.get_type() == Variant::FLOAT) {
			id = int64_t(id);
		}
		if (String(req.get("token", String())) != token) {
			r_close = true;
			return fail(id, "unauthorized", "Missing or wrong token", "Read the instance file in app_bridge/");
		}
		String tool = req.get("tool", String());
		Variant args_v = req.get("args", Dictionary());
		Dictionary args = args_v.get_type() == Variant::DICTIONARY ? Dictionary(args_v) : Dictionary();
		if (tool == "app.info") {
			return tool_info(id);
		} else if (tool == "app.screenshot") {
			return tool_screenshot(id, args);
		} else if (tool == "app.setting") {
			return tool_setting(id, args);
		}
		return fail(id, "unknown_tool", tool, "The app bridge answers app.info, app.screenshot and app.setting");
	}

	void poll() {
		// Editor settings changed on disk (an agent, another window) are taken within a second.
		const uint64_t now = OS::get_singleton()->get_ticks_msec();
		if (now >= next_settings_check_msec) {
			next_settings_check_msec = now + 1000;
			if (EditorSettings::get_singleton()) {
				EditorSettings::get_singleton()->reload_if_changed_on_disk();
			}
		}
		if (server.is_null() || !server->is_listening()) {
			return;
		}
		while (server->is_connection_available()) {
			Peer p;
			p.tcp = server->take_connection();
			peers.push_back(p);
		}
		for (int i = peers.size() - 1; i >= 0; i--) {
			Peer &p = peers.write[i];
			p.tcp->poll();
			bool close = p.tcp->get_status() != StreamPeerTCP::STATUS_CONNECTED;
			int n = close ? 0 : p.tcp->get_available_bytes();
			if (n > 0) {
				p.buffer += p.tcp->get_utf8_string(n);
				int nl;
				while (!close && (nl = p.buffer.find_char('\n')) >= 0) {
					String line = p.buffer.substr(0, nl).strip_edges();
					p.buffer = p.buffer.substr(nl + 1);
					if (!line.is_empty()) {
						Dictionary reply = dispatch(line, close);
						const CharString out = (JSON::stringify(reply, "", false) + "\n").utf8();
						p.tcp->put_data((const uint8_t *)out.get_data(), out.length());
					}
				}
				if (p.buffer.length() > MAX_LINE) {
					close = true;
				}
			}
			if (close) {
				p.tcp->disconnect_from_host();
				peers.remove_at(i);
			}
		}
	}

public:
	void start() {
		// Only in a real editor or project manager window; never in a game or a headless run.
		Engine *engine = Engine::get_singleton();
		if (!engine->is_editor_hint() && !engine->is_project_manager_hint()) {
			return;
		}
		if (!DisplayServer::get_singleton() || DisplayServer::get_singleton()->get_name() == "headless") {
			return;
		}
		SceneTree *tree = SceneTree::get_singleton();
		if (!tree) {
			ERR_PRINT("Dovetail app bridge: no SceneTree on the first frame; not started.");
			return;
		}
		server.instantiate();
		Error err = server->listen(0, IPAddress("127.0.0.1"));
		if (err != OK) {
			ERR_PRINT(vformat("Dovetail app bridge could not listen: %s", error_names[err]));
			server.unref();
			return;
		}
		uint8_t bytes[24];
		if (OS::get_singleton()->get_entropy(bytes, sizeof(bytes)) != OK) {
			ERR_PRINT("Dovetail app bridge: no entropy for a token; not started.");
			server->stop();
			server.unref();
			return;
		}
		token = String::hex_encode_buffer(bytes, sizeof(bytes));

		const bool pm = engine->is_project_manager_hint();
		String project;
		if (!pm) {
			project = ProjectSettings::get_singleton()->get_resource_path().simplify_path().trim_suffix("/");
		}
		const int64_t pid = OS::get_singleton()->get_process_id();
		info["port"] = server->get_local_port();
		info["token"] = token;
		info["pid"] = pid;
		info["mode"] = pm ? "project_manager" : "editor";
		info["project"] = project;
		info["version"] = GODOT_VERSION_FULL_CONFIG;
		info["executable"] = OS::get_singleton()->get_executable_path();

		// Next to editor_settings-*.tres: ~/Library/Application Support/Dovetail/app_bridge.
		String dir = OS::get_singleton()->get_config_path().path_join(OS::get_singleton()->get_godot_dir_name()).path_join("app_bridge");
		DirAccess::make_dir_recursive_absolute(dir);
		info_path = dir.path_join(vformat("%d.json", pid));
		Ref<FileAccess> f = FileAccess::open(info_path, FileAccess::WRITE);
		if (f.is_null()) {
			ERR_PRINT("Dovetail app bridge could not write " + info_path);
			info_path = String();
			server->stop();
			server.unref();
			return;
		}
		f->store_string(JSON::stringify(info, "", false));
		f->close();
		FileAccess::set_unix_permissions(info_path, FileAccess::UNIX_READ_OWNER | FileAccess::UNIX_WRITE_OWNER);

		tree->connect(SNAME("process_frame"), callable_mp(this, &DovetailAppBridge::poll));
		print_verbose(vformat("Dovetail app bridge listening on 127.0.0.1:%d", int(info["port"])));
	}

	void stop() {
		for (Peer &p : peers) {
			p.tcp->disconnect_from_host();
		}
		peers.clear();
		if (server.is_valid()) {
			server->stop();
			server.unref();
		}
		if (!info_path.is_empty()) {
			DirAccess::remove_absolute(info_path);
			info_path = String();
		}
	}
};

static DovetailAppBridge *app_bridge = nullptr;

void initialize_dovetail_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_EDITOR) {
		return;
	}
	// The main loop does not exist yet; start on the first frame, when SceneTree does.
	app_bridge = memnew(DovetailAppBridge);
	callable_mp(app_bridge, &DovetailAppBridge::start).call_deferred();
}

void uninitialize_dovetail_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_EDITOR || !app_bridge) {
		return;
	}
	app_bridge->stop();
	memdelete(app_bridge);
	app_bridge = nullptr;
}
