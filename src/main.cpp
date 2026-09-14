// SPDX-License-Identifier: GPL-2.0-only
#include "runtime.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <cairo/cairo-xlib.h>
#include <gtk/gtk.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <vector>

namespace {

struct Options {
  std::filesystem::path skin;
  std::optional<std::filesystem::path> output;
  bool headless = false;
  bool click = false;
  std::optional<std::pair<int, int>> click_at;
  int auto_exit_ms = 0;
  int updates = 0;
  int x = 100;
  int y = 100;
  int monitor_x = 0;
  int monitor_y = 0;
  int monitor_width = 0;
  int monitor_height = 0;
  double monitor_refresh_hz = 60.0;
  std::optional<std::string> monitor;
  std::optional<std::filesystem::path> state_file;
  std::optional<std::string> state_section;
};

void Usage() {
  std::cerr << "Usage: rainmeter-linux [--headless] [--output-dir DIR] [--click] "
               "[--auto-exit-ms MS] [--updates COUNT] [--click-at X,Y] [--monitor OUTPUT] [--position X,Y] "
               "[--state-file FILE --state-section CONFIG] SKIN.ini\n";
}

bool ParseInt(const std::string& text, int& value) {
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (!end || *end != '\0') return false;
  value = static_cast<int>(parsed);
  return true;
}

std::optional<Options> ParseArgs(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--headless") {
      options.headless = true;
    } else if (arg == "--click") {
      options.click = true;
    } else if (arg == "--click-at" && i + 1 < argc) {
      const std::string position = argv[++i];
      const auto comma = position.find(',');
      int x = 0, y = 0;
      if (comma == std::string::npos || !ParseInt(position.substr(0, comma), x) ||
          !ParseInt(position.substr(comma + 1), y)) return std::nullopt;
      options.click_at = std::make_pair(x, y);
    } else if (arg == "--output-dir" && i + 1 < argc) {
      options.output = argv[++i];
    } else if (arg == "--auto-exit-ms" && i + 1 < argc) {
      if (!ParseInt(argv[++i], options.auto_exit_ms) || options.auto_exit_ms < 0) return std::nullopt;
    } else if (arg == "--updates" && i + 1 < argc) {
      if (!ParseInt(argv[++i], options.updates) || options.updates < 0) return std::nullopt;
    } else if (arg == "--position" && i + 1 < argc) {
      const std::string position = argv[++i];
      const auto comma = position.find(',');
      if (comma == std::string::npos || !ParseInt(position.substr(0, comma), options.x) ||
          !ParseInt(position.substr(comma + 1), options.y)) return std::nullopt;
    } else if (arg == "--monitor" && i + 1 < argc) {
      options.monitor = argv[++i];
    } else if (arg == "--state-file" && i + 1 < argc) {
      options.state_file = argv[++i];
    } else if (arg == "--state-section" && i + 1 < argc) {
      options.state_section = argv[++i];
    } else if (!arg.empty() && arg[0] == '-') {
      return std::nullopt;
    } else if (options.skin.empty()) {
      options.skin = arg;
    } else {
      return std::nullopt;
    }
  }
  if (options.skin.empty()) return std::nullopt;
  return options;
}

bool ApplyMonitor(Options& options, std::string& error) {
  if (!options.monitor) return true;
  Display* display = XOpenDisplay(nullptr);
  if (!display) {
    error = "Unable to open X display while resolving monitor " + *options.monitor;
    return false;
  }
  const Window root = DefaultRootWindow(display);
  XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, root);
  bool found = false;
  if (resources) {
    for (int index = 0; index < resources->noutput && !found; ++index) {
      XRROutputInfo* output = XRRGetOutputInfo(display, resources, resources->outputs[index]);
      if (!output) continue;
      const std::string name(output->name, static_cast<size_t>(output->nameLen));
      if (name == *options.monitor && output->connection == RR_Connected && output->crtc) {
        XRRCrtcInfo* crtc = XRRGetCrtcInfo(display, resources, output->crtc);
        if (crtc) {
          options.monitor_x = crtc->x;
          options.monitor_y = crtc->y;
          options.monitor_width = static_cast<int>(crtc->width);
          options.monitor_height = static_cast<int>(crtc->height);
          for (int mode_index = 0; mode_index < resources->nmode; ++mode_index) {
            const auto& mode = resources->modes[mode_index];
            if (mode.id != crtc->mode || mode.hTotal == 0 || mode.vTotal == 0) continue;
            double refresh = static_cast<double>(mode.dotClock) /
                (static_cast<double>(mode.hTotal) * mode.vTotal);
            if (mode.modeFlags & RR_DoubleScan) refresh /= 2.0;
            if (mode.modeFlags & RR_Interlace) refresh *= 2.0;
            options.monitor_refresh_hz = std::clamp(refresh, 1.0, 1000.0);
            break;
          }
          options.x += crtc->x;
          options.y += crtc->y;
          const auto width = std::to_string(crtc->width);
          const auto height = std::to_string(crtc->height);
          setenv("RM_WORKAREAWIDTH", width.c_str(), 1);
          setenv("RM_WORKAREAHEIGHT", height.c_str(), 1);
          XRRFreeCrtcInfo(crtc);
          found = true;
        }
      }
      XRRFreeOutputInfo(output);
    }
    XRRFreeScreenResources(resources);
  }
  XCloseDisplay(display);
  if (!found) error = "Connected X11 output not found: " + *options.monitor;
  return found;
}

void SavePosition(const Options& options) {
  if (!options.state_file || !options.state_section) return;
  GKeyFile* state = g_key_file_new();
  GError* error = nullptr;
  g_key_file_load_from_file(state, options.state_file->c_str(), G_KEY_FILE_KEEP_COMMENTS, &error);
  if (error) {
    g_error_free(error);
    error = nullptr;
  }
  g_key_file_set_integer(state, options.state_section->c_str(), "WindowX", options.x - options.monitor_x);
  g_key_file_set_integer(state, options.state_section->c_str(), "WindowY", options.y - options.monitor_y);
  g_key_file_save_to_file(state, options.state_file->c_str(), &error);
  if (error) g_error_free(error);
  g_key_file_unref(state);
}

struct WindowSettings {
  bool draggable = true;
  bool save_position = true;
  bool keep_on_screen = true;
  bool snap_edges = true;
  bool click_through = false;
  bool context_menu = true;
  bool favorite = false;
  int alpha = 255;
  int z_position = 0;
  int on_hover = 0;
};

WindowSettings LoadWindowSettings(const Options& options) {
  WindowSettings settings;
  if (!options.state_file || !options.state_section) return settings;
  GKeyFile* state = g_key_file_new();
  GError* error = nullptr;
  if (!g_key_file_load_from_file(state, options.state_file->c_str(), G_KEY_FILE_NONE, &error)) {
    if (error) g_error_free(error);
    g_key_file_unref(state);
    return settings;
  }
  auto read_bool = [&](const char* key, bool fallback) {
    GError* item_error = nullptr;
    const bool value = g_key_file_get_boolean(state, options.state_section->c_str(), key, &item_error);
    if (item_error) {
      g_error_free(item_error);
      return fallback;
    }
    return value;
  };
  auto read_int = [&](const char* key, int fallback) {
    GError* item_error = nullptr;
    const int value = g_key_file_get_integer(state, options.state_section->c_str(), key, &item_error);
    if (item_error) {
      g_error_free(item_error);
      return fallback;
    }
    return value;
  };
  settings.draggable = read_bool("Draggable", true);
  settings.save_position = read_bool("SavePosition", true);
  settings.keep_on_screen = read_bool("KeepOnScreen", true);
  settings.snap_edges = read_bool("SnapEdges", true);
  settings.click_through = read_bool("ClickThrough", false);
  settings.context_menu = read_bool("ContextMenu", true);
  settings.favorite = read_bool("Favorite", false);
  settings.alpha = std::clamp(read_int("AlphaValue", 255), 1, 255);
  settings.z_position = std::clamp(read_int("AlwaysOnTop", 0), -2, 2);
  settings.on_hover = std::clamp(read_int("OnHover", 0), 0, 3);
  g_key_file_unref(state);
  return settings;
}

std::filesystem::path SiblingExecutable(const char* name) {
  std::error_code error;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
  return error ? std::filesystem::path(name) : self.parent_path() / name;
}

void Spawn(const std::vector<std::string>& values) {
  std::vector<gchar*> arguments;
  for (const auto& value : values) arguments.push_back(const_cast<gchar*>(value.c_str()));
  arguments.push_back(nullptr);
  GError* error = nullptr;
  if (!g_spawn_async(nullptr, arguments.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error)) {
    std::cerr << error->message << '\n';
    g_error_free(error);
  }
}

void OpenDefault(const std::filesystem::path& path) {
  GError* error = nullptr;
  gchar* uri = g_filename_to_uri(std::filesystem::absolute(path).c_str(), nullptr, &error);
  if (uri && !g_app_info_launch_default_for_uri(uri, nullptr, &error)) {
    g_free(uri);
    uri = nullptr;
  }
  if (uri) g_free(uri);
  if (error) {
    std::cerr << error->message << '\n';
    g_error_free(error);
  }
}

struct ContextActions;
struct VariantAction {
  ContextActions* context;
  std::filesystem::path skin;
};

struct SettingAction {
  ContextActions* context;
  std::string key;
  std::string value;
};

struct CustomMenuAction {
  ContextActions* context;
  std::string action;
};

struct ContextActions {
  bool* running;
  bool* refresh;
  std::filesystem::path skin;
  const Options* options;
  const WindowSettings* settings;
  rm::Runtime* runtime;
  GtkWidget* popup = nullptr;
  std::vector<std::unique_ptr<VariantAction>> variants;
  std::vector<std::unique_ptr<SettingAction>> setting_actions;
  std::vector<std::unique_ptr<CustomMenuAction>> custom_actions;
};

void PersistSetting(SettingAction& action) {
  const auto& options = *action.context->options;
  if (!options.state_file || !options.state_section) return;
  GKeyFile* state = g_key_file_new();
  GError* error = nullptr;
  g_key_file_load_from_file(state, options.state_file->c_str(), G_KEY_FILE_KEEP_COMMENTS, &error);
  if (error) { g_error_free(error); error = nullptr; }
  g_key_file_set_string(state, options.state_section->c_str(), action.key.c_str(), action.value.c_str());
  g_key_file_save_to_file(state, options.state_file->c_str(), &error);
  if (error) {
    std::cerr << error->message << '\n';
    g_error_free(error);
  } else {
    *action.context->refresh = true;
    *action.context->running = false;
  }
  g_key_file_unref(state);
}

void ShowContextMenu(ContextActions& actions, bool custom_only = false) {
  if (actions.popup) gtk_widget_destroy(actions.popup);
  actions.variants.clear();
  actions.setting_actions.clear();
  actions.custom_actions.clear();
  GtkWidget* menu = gtk_menu_new();
  actions.popup = menu;
  g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkWidget* widget, ContextActions* value) {
    if (value->popup == widget) value->popup = nullptr;
    gtk_widget_destroy(widget);
  }), &actions);
  auto append_item = [&](GtkWidget* parent, const char* label, GCallback callback, gpointer data) {
    GtkWidget* item = gtk_menu_item_new_with_label(label);
    if (callback) g_signal_connect(item, "activate", callback, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(parent), item);
    return item;
  };
  auto append_separator = [](GtkWidget* parent) {
    gtk_menu_shell_append(GTK_MENU_SHELL(parent), gtk_separator_menu_item_new());
  };
  auto append_setting = [&](GtkWidget* parent, const char* label, const std::string& key,
                            const std::string& value, bool checked, GSList** group = nullptr) {
    GtkWidget* item = group ? gtk_radio_menu_item_new_with_label(*group, label) :
                              gtk_check_menu_item_new_with_label(label);
    if (group) *group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), checked);
    auto setting = std::make_unique<SettingAction>(SettingAction{&actions, key, value});
    g_signal_connect(item, "activate", G_CALLBACK(+[](GtkCheckMenuItem* widget, SettingAction* action) {
      if (GTK_IS_RADIO_MENU_ITEM(widget) && !gtk_check_menu_item_get_active(widget)) return;
      PersistSetting(*action);
    }), setting.get());
    gtk_menu_shell_append(GTK_MENU_SHELL(parent), item);
    actions.setting_actions.push_back(std::move(setting));
    return item;
  };

  const auto custom_items = actions.runtime->ContextItems();
  for (const auto& custom : custom_items) {
    if (custom.title == "-" || custom.title == "---") {
      append_separator(menu);
      continue;
    }
    auto action = std::make_unique<CustomMenuAction>(CustomMenuAction{&actions, custom.action});
    GtkWidget* item = append_item(menu, custom.title.c_str(),
        G_CALLBACK(+[](GtkMenuItem*, CustomMenuAction* value) {
          if (!value->action.empty()) value->context->runtime->ExecuteAction(value->action);
        }), action.get());
    gtk_widget_set_sensitive(item, !custom.action.empty());
    actions.custom_actions.push_back(std::move(action));
  }
  if (custom_only && !custom_items.empty()) {
    gtk_widget_show_all(menu);
    GdkDisplay* display = gdk_display_get_default();
    GdkDevice* pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
    GdkRectangle anchor{0, 0, 1, 1};
    gdk_device_get_position(pointer, nullptr, &anchor.x, &anchor.y);
    gtk_menu_popup_at_rect(GTK_MENU(menu), gdk_get_default_root_window(), &anchor,
                           GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
    return;
  }
  if (!custom_items.empty()) append_separator(menu);

  GtkWidget* config = append_item(menu, actions.skin.parent_path().filename().c_str(),
      G_CALLBACK(+[](GtkMenuItem*, ContextActions* value) {
        OpenDefault(value->skin.parent_path());
      }), &actions);
  gtk_widget_set_sensitive(config, TRUE);

  GtkWidget* variants = append_item(menu, "Variants", nullptr, nullptr);
  GtkWidget* variant_menu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(variants), variant_menu);
  std::vector<std::filesystem::path> paths;
  std::error_code scan_error;
  for (const auto& entry : std::filesystem::directory_iterator(actions.skin.parent_path(), scan_error)) {
    if (entry.is_regular_file() && entry.path().extension() == ".ini") paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());
  for (const auto& path : paths) {
    auto action = std::make_unique<VariantAction>(VariantAction{&actions, path});
    append_item(variant_menu, path.filename().c_str(),
        G_CALLBACK(+[](GtkMenuItem*, VariantAction* value) {
          Spawn({SiblingExecutable("rainmeter").string(), "--load", value->skin.string()});
        }), action.get());
    actions.variants.push_back(std::move(action));
  }

  append_separator(menu);
  GtkWidget* settings_item = append_item(menu, "Settings", nullptr, nullptr);
  GtkWidget* settings_menu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(settings_item), settings_menu);

  GtkWidget* position_item = append_item(settings_menu, "Position", nullptr, nullptr);
  GtkWidget* position_menu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(position_item), position_menu);

  GtkWidget* display_item = append_item(position_menu, "Display monitor", nullptr, nullptr);
  GtkWidget* display_menu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(display_item), display_menu);
  GSList* display_group = nullptr;
  Display* xdisplay = XOpenDisplay(nullptr);
  if (xdisplay) {
    const Window root = DefaultRootWindow(xdisplay);
    XRRScreenResources* resources = XRRGetScreenResourcesCurrent(xdisplay, root);
    if (resources) {
      for (int index = 0; index < resources->noutput; ++index) {
        XRROutputInfo* output = XRRGetOutputInfo(xdisplay, resources, resources->outputs[index]);
        if (!output) continue;
        if (output->connection == RR_Connected && output->crtc) {
          const std::string name(output->name, static_cast<size_t>(output->nameLen));
          append_setting(display_menu, name.c_str(), "Monitor", name,
                         actions.options->monitor && *actions.options->monitor == name,
                         &display_group);
        }
        XRRFreeOutputInfo(output);
      }
      XRRFreeScreenResources(resources);
    }
    XCloseDisplay(xdisplay);
  }
  if (!display_group) {
    GtkWidget* unavailable = append_item(display_menu, "No connected displays", nullptr, nullptr);
    gtk_widget_set_sensitive(unavailable, FALSE);
  }
  append_separator(position_menu);
  GSList* position_group = nullptr;
  const struct { const char* label; int value; } positions[] = {
      {"Stay topmost", 2}, {"Topmost", 1}, {"Normal", 0}, {"Bottom", -1}, {"On desktop", -2}};
  for (const auto& position : positions) {
    append_setting(position_menu, position.label, "AlwaysOnTop", std::to_string(position.value),
                   actions.settings->z_position == position.value, &position_group);
  }

  GtkWidget* transparency_item = append_item(settings_menu, "Transparency", nullptr, nullptr);
  GtkWidget* transparency_menu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(transparency_item), transparency_menu);
  GSList* transparency_group = nullptr;
  for (int percent = 0; percent <= 100; percent += 10) {
    const int alpha = std::max(1, 255 * (100 - percent) / 100);
    append_setting(transparency_menu, (percent == 100 ? "~100%" : std::to_string(percent) + "%").c_str(),
                   "AlphaValue", std::to_string(alpha),
                   std::abs(actions.settings->alpha - alpha) <= 13, &transparency_group);
  }

  GtkWidget* hover_item = append_item(settings_menu, "On hover", nullptr, nullptr);
  GtkWidget* hover_menu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(hover_item), hover_menu);
  GSList* hover_group = nullptr;
  const char* hover_labels[] = {"Do nothing", "Hide", "Fade in", "Fade out"};
  for (int mode = 0; mode < 4; ++mode) {
    append_setting(hover_menu, hover_labels[mode], "OnHover", std::to_string(mode),
                   actions.settings->on_hover == mode, &hover_group);
  }
  append_separator(settings_menu);
  append_setting(settings_menu, "Click through", "ClickThrough",
                 actions.settings->click_through ? "false" : "true", actions.settings->click_through);
  append_setting(settings_menu, "Draggable", "Draggable",
                 actions.settings->draggable ? "false" : "true", actions.settings->draggable);
  append_setting(settings_menu, "Keep on screen", "KeepOnScreen",
                 actions.settings->keep_on_screen ? "false" : "true", actions.settings->keep_on_screen);
  append_setting(settings_menu, "Save position", "SavePosition",
                 actions.settings->save_position ? "false" : "true", actions.settings->save_position);
  append_setting(settings_menu, "Snap to edges", "SnapEdges",
                 actions.settings->snap_edges ? "false" : "true", actions.settings->snap_edges);
  append_setting(settings_menu, "Favorite", "Favorite",
                 actions.settings->favorite ? "false" : "true", actions.settings->favorite);

  append_separator(menu);
  GtkWidget* manage = append_item(menu, "Manage skin",
      G_CALLBACK(+[](GtkMenuItem*, ContextActions* value) {
    Spawn({SiblingExecutable("rainmeter").string(), "--select", value->skin.string()});
  }), &actions);

  GtkWidget* edit = append_item(menu, "Edit skin",
      G_CALLBACK(+[](GtkMenuItem*, ContextActions* value) {
    OpenDefault(value->skin);
  }), &actions);

  GtkWidget* refresh = append_item(menu, "Refresh skin",
      G_CALLBACK(+[](GtkMenuItem*, ContextActions* value) {
    *value->refresh = true;
    *value->running = false;
  }), &actions);

  GtkWidget* rainmeter = append_item(menu, "Rainmeter", nullptr, nullptr);
  GtkWidget* rainmeter_menu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(rainmeter), rainmeter_menu);
  append_item(rainmeter_menu, "Manage",
      G_CALLBACK(+[](GtkMenuItem*, ContextActions*) {
        Spawn({SiblingExecutable("rainmeter").string()});
      }), &actions);
  append_item(rainmeter_menu, "Help",
      G_CALLBACK(+[](GtkMenuItem*, ContextActions*) {
        GError* error = nullptr;
        g_app_info_launch_default_for_uri("https://docs.rainmeter.net/manual/", nullptr, &error);
        if (error) { std::cerr << error->message << '\n'; g_error_free(error); }
      }), &actions);
  append_separator(rainmeter_menu);
  append_item(rainmeter_menu, "Open skins folder",
      G_CALLBACK(+[](GtkMenuItem*, ContextActions* value) {
        auto root = value->skin;
        while (!root.empty() && root.filename() != "Skins") root = root.parent_path();
        OpenDefault(root.empty() ? value->skin.parent_path() : root);
      }), &actions);
  append_item(rainmeter_menu, "Edit settings",
      G_CALLBACK(+[](GtkMenuItem*, ContextActions* value) {
        if (value->options->state_file) OpenDefault(*value->options->state_file);
      }), &actions);
  append_item(rainmeter_menu, "Refresh all",
      G_CALLBACK(+[](GtkMenuItem*, ContextActions*) {
        Spawn({SiblingExecutable("rainmeter").string(), "--refresh-all"});
      }), &actions);
  append_item(rainmeter_menu, "Exit",
      G_CALLBACK(+[](GtkMenuItem*, ContextActions*) {
        Spawn({SiblingExecutable("rainmeter").string(), "--quit"});
      }), &actions);

  append_separator(menu);
  GtkWidget* unload = append_item(menu, "Unload skin",
      G_CALLBACK(+[](GtkMenuItem*, ContextActions* value) {
    *value->running = false;
  }), &actions);
  (void)manage;
  (void)edit;
  (void)refresh;
  (void)unload;
  gtk_widget_show_all(menu);
  GdkDisplay* display = gdk_display_get_default();
  GdkDevice* pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
  GdkRectangle anchor{0, 0, 1, 1};
  gdk_device_get_position(pointer, nullptr, &anchor.x, &anchor.y);
  gtk_menu_popup_at_rect(GTK_MENU(menu), gdk_get_default_root_window(), &anchor,
                         GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
}

Visual* FindArgbVisual(Display* display, int screen, int& depth) {
  XVisualInfo query{};
  query.screen = screen;
  int count = 0;
  XVisualInfo* visuals = XGetVisualInfo(display, VisualScreenMask, &query, &count);
  Visual* result = nullptr;
  for (int i = 0; i < count; ++i) {
    const auto* format = XRenderFindVisualFormat(display, visuals[i].visual);
    if (visuals[i].depth == 32 && format && format->type == PictTypeDirect &&
        format->direct.alphaMask) {
      result = visuals[i].visual;
      depth = visuals[i].depth;
      break;
    }
  }
  XFree(visuals);
  return result;
}

void SetAtomList(Display* display, Window window, const char* property,
                 std::initializer_list<const char*> values) {
  std::vector<Atom> atoms;
  for (const char* value : values) atoms.push_back(XInternAtom(display, value, False));
  XChangeProperty(display, window, XInternAtom(display, property, False), XA_ATOM, 32,
                  PropModeReplace, reinterpret_cast<unsigned char*>(atoms.data()), atoms.size());
}

void MoveTopLevel(Display* display, Window root, Window window, int x, int y) {
  Window current = window;
  for (;;) {
    Window returned_root = 0, parent = 0, *children = nullptr;
    unsigned int count = 0;
    if (!XQueryTree(display, current, &returned_root, &parent, &children, &count)) break;
    if (children) XFree(children);
    if (!parent || parent == root) break;
    current = parent;
  }
  XMoveWindow(display, current, x, y);
}

int RunX11(rm::Runtime& runtime, Options& options) {
  const WindowSettings settings = LoadWindowSettings(options);
  Display* display = XOpenDisplay(nullptr);
  if (!display) {
    std::cerr << "Unable to open X display; use --headless for offscreen conformance.\n";
    return 1;
  }
  const int screen = DefaultScreen(display);
  int depth = DefaultDepth(display, screen);
  Visual* visual = FindArgbVisual(display, screen, depth);
  if (!visual) visual = DefaultVisual(display, screen);
  const Window root = RootWindow(display, screen);
  const Colormap colormap = XCreateColormap(display, root, visual, AllocNone);
  XSetWindowAttributes attributes{};
  attributes.colormap = colormap;
  attributes.border_pixel = 0;
  attributes.background_pixel = 0;
  attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                          EnterWindowMask | LeaveWindowMask | StructureNotifyMask;
  const Window window = XCreateWindow(display, root, options.x, options.y, runtime.Width(), runtime.Height(),
                                      0, depth, InputOutput, visual,
                                      CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attributes);
  XStoreName(display, window, "Rainmeter Linux Skin");
  XSizeHints size_hints{};
  size_hints.flags = USPosition | USSize | PPosition | PSize;
  size_hints.x = options.x;
  size_hints.y = options.y;
  size_hints.width = runtime.Width();
  size_hints.height = runtime.Height();
  XSetWMNormalHints(display, window, &size_hints);
  XClassHint class_hint{const_cast<char*>("rainmeter-linux"), const_cast<char*>("RainmeterLinux")};
  XSetClassHint(display, window, &class_hint);
  if (settings.z_position == -2) {
    SetAtomList(display, window, "_NET_WM_WINDOW_TYPE", {"_NET_WM_WINDOW_TYPE_DESKTOP"});
  } else {
    SetAtomList(display, window, "_NET_WM_WINDOW_TYPE", {"_NET_WM_WINDOW_TYPE_UTILITY"});
  }
  std::vector<const char*> window_states = {
      "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER", "_NET_WM_STATE_STICKY"};
  if (settings.z_position > 0) window_states.push_back("_NET_WM_STATE_ABOVE");
  if (settings.z_position < 0) window_states.push_back("_NET_WM_STATE_BELOW");
  std::vector<Atom> state_atoms;
  for (const char* state : window_states) state_atoms.push_back(XInternAtom(display, state, False));
  XChangeProperty(display, window, XInternAtom(display, "_NET_WM_STATE", False), XA_ATOM, 32,
                  PropModeReplace, reinterpret_cast<unsigned char*>(state_atoms.data()),
                  state_atoms.size());
  if (settings.click_through) {
    XShapeCombineRectangles(display, window, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
  }

  struct MotifHints { unsigned long flags, functions, decorations; long input_mode; unsigned long status; };
  const MotifHints hints{2, 0, 0, 0, 0};
  XChangeProperty(display, window, XInternAtom(display, "_MOTIF_WM_HINTS", False),
                  XInternAtom(display, "_MOTIF_WM_HINTS", False), 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&hints), 5);
  const Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, window, const_cast<Atom*>(&wm_delete), 1);

  cairo_surface_t* surface = cairo_xlib_surface_create(display, window, visual,
                                                        runtime.Width(), runtime.Height());
  cairo_t* cr = cairo_create(surface);
  cairo_surface_t* frame = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       runtime.Width(), runtime.Height());
  cairo_t* frame_cr = cairo_create(frame);
  int current_alpha = settings.on_hover == 3 ? 255 : settings.alpha;
  auto repaint = [&] {
    runtime.Render(frame_cr);
    cairo_surface_flush(frame);
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, frame, 0, 0);
    if (current_alpha < 255) cairo_paint_with_alpha(cr, current_alpha / 255.0);
    else cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_flush(surface);
    XFlush(display);
  };
  XMapRaised(display, window);
  XSync(display, False);
  MoveTopLevel(display, root, window, options.x, options.y);
  repaint();
  std::cout << "WINDOW_ID=" << window << '\n';
  std::cout.flush();

  const auto started = std::chrono::steady_clock::now();
  const double frame_rate = runtime.FrameRate() > 0.0 ?
      runtime.FrameRate() : options.monitor_refresh_hz;
  const auto update_period = std::chrono::microseconds(runtime.UpdateInterval() == 0 ?
      static_cast<long long>(std::llround(1000000.0 / frame_rate)) :
      static_cast<long long>(std::max(1, runtime.UpdateInterval())) * 1000LL);
  auto next_update = started + update_period;
  uint64_t update_count = 0;
  bool running = true;
  bool refresh_requested = false;
  bool dragging = false;
  bool moved = false;
  Time last_left_release = 0;
  int last_left_x = 0, last_left_y = 0;
  int press_root_x = 0, press_root_y = 0, start_x = options.x, start_y = options.y;
  ContextActions context{&running, &refresh_requested, options.skin, &options, &settings, &runtime,
                         nullptr, {}, {}, {}};
  while (running) {
    runtime.Tick();
    for (const auto& action : runtime.TakeHostActions()) {
      if (action.verb == "activate" || action.verb == "toggle") {
        std::vector<std::string> command{SiblingExecutable("rainmeter").string(),
                                         action.verb == "toggle" ? "--toggle-config" : "--activate-config",
                                         action.config};
        if (!action.variant.empty()) {
          command.push_back("--variant");
          command.push_back(action.variant);
        }
        Spawn(command);
      } else if (action.verb == "custom-menu") {
        ShowContextMenu(context, true);
      } else if (action.verb == "skin-menu") {
        ShowContextMenu(context);
      } else if (action.verb == "deactivate-current") {
        running = false;
      } else if (action.verb == "refresh-current") {
        refresh_requested = true;
        running = false;
      } else if (action.verb == "deactivate" || action.verb == "refresh") {
        Spawn({SiblingExecutable("rainmeter").string(),
               action.verb == "deactivate" ? "--deactivate-config" : "--refresh-config",
               action.config});
      }
    }
    while (gtk_events_pending()) gtk_main_iteration_do(FALSE);
    while (XPending(display)) {
      XEvent event{};
      XNextEvent(display, &event);
      if (event.type == Expose && event.xexpose.count == 0) {
        repaint();
      } else if (event.type == EnterNotify && settings.on_hover != 0) {
        current_alpha = settings.on_hover == 1 || settings.on_hover == 3 ? settings.alpha : 255;
        if (settings.on_hover == 1) current_alpha = 0;
        repaint();
      } else if (event.type == LeaveNotify && settings.on_hover != 0) {
        current_alpha = settings.on_hover == 2 ? settings.alpha : 255;
        repaint();
      } else if (event.type == ButtonPress && event.xbutton.button == Button1 && settings.draggable) {
        dragging = true;
        moved = false;
        press_root_x = event.xbutton.x_root;
        press_root_y = event.xbutton.y_root;
        start_x = options.x;
        start_y = options.y;
      } else if (event.type == ButtonRelease && event.xbutton.button == Button3 &&
                 settings.context_menu) {
        if (!runtime.RightClick(event.xbutton.x, event.xbutton.y)) ShowContextMenu(context);
      } else if (event.type == MotionNotify && dragging) {
        const int dx = event.xmotion.x_root - press_root_x;
        const int dy = event.xmotion.y_root - press_root_y;
        if (std::abs(dx) > 2 || std::abs(dy) > 2) moved = true;
        options.x = start_x + dx;
        options.y = start_y + dy;
        if (settings.snap_edges) {
          const int left = options.monitor_x;
          const int top = options.monitor_y;
          const int right = left + options.monitor_width - runtime.Width();
          const int bottom = top + options.monitor_height - runtime.Height();
          if (std::abs(options.x - left) <= 10) options.x = left;
          if (std::abs(options.y - top) <= 10) options.y = top;
          if (std::abs(options.x - right) <= 10) options.x = right;
          if (std::abs(options.y - bottom) <= 10) options.y = bottom;
        }
        if (settings.keep_on_screen && options.monitor_width > 0 && options.monitor_height > 0) {
          const int maximum_x = std::max(options.monitor_x,
              options.monitor_x + options.monitor_width - runtime.Width());
          const int maximum_y = std::max(options.monitor_y,
              options.monitor_y + options.monitor_height - runtime.Height());
          options.x = std::clamp(options.x, options.monitor_x, maximum_x);
          options.y = std::clamp(options.y, options.monitor_y, maximum_y);
        }
        MoveTopLevel(display, root, window, options.x, options.y);
      } else if (event.type == ButtonRelease && event.xbutton.button == Button1) {
        dragging = false;
        if (moved && settings.save_position) {
          SavePosition(options);
        } else {
          const bool double_click = last_left_release != 0 &&
              event.xbutton.time - last_left_release <= 450 &&
              std::abs(event.xbutton.x - last_left_x) <= 4 &&
              std::abs(event.xbutton.y - last_left_y) <= 4;
          if (!double_click || !runtime.DoubleClick(event.xbutton.x, event.xbutton.y)) {
            runtime.Click(event.xbutton.x, event.xbutton.y);
          }
          last_left_release = event.xbutton.time;
          last_left_x = event.xbutton.x;
          last_left_y = event.xbutton.y;
          repaint();
        }
      } else if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == wm_delete) {
        running = false;
      }
    }
    if (options.auto_exit_ms > 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
            >= options.auto_exit_ms) {
      running = false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (runtime.UpdateInterval() >= 0 && now >= next_update) {
      runtime.Update();
      repaint();
      ++update_count;
      do next_update += update_period; while (next_update <= std::chrono::steady_clock::now());
    }
    if (running) {
      pollfd descriptor{ConnectionNumber(display), POLLIN, 0};
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          next_update - std::chrono::steady_clock::now()).count();
      poll(&descriptor, 1, static_cast<int>(std::clamp<int64_t>(remaining, 0, 16)));
    }
  }

  if (std::getenv("RM_FPS_LOG")) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "UPDATE_FPS=" << (seconds > 0.0 ? update_count / seconds : 0.0)
              << " UPDATES=" << update_count << " ELAPSED=" << seconds << '\n';
  }

  cairo_destroy(frame_cr);
  cairo_surface_destroy(frame);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  XDestroyWindow(display, window);
  XFreeColormap(display, colormap);
  XCloseDisplay(display);
  return refresh_requested ? 75 : 0;
}

}

int main(int argc, char** argv) {
  gtk_init_check(&argc, &argv);
  auto options = ParseArgs(argc, argv);
  if (!options) {
    Usage();
    return 2;
  }
  std::string error;
  if (!ApplyMonitor(*options, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  bool artifacts_written = false;
  for (;;) {
    auto runtime = rm::Runtime::Load(options->skin, error);
    if (!runtime) {
      std::cerr << error << '\n';
      return 1;
    }
    if (!artifacts_written) for (int update = 0; update < options->updates; ++update) runtime->Update();
    if (!artifacts_written && options->click_at) {
      runtime->Click(options->click_at->first, options->click_at->second);
    }
    if (!artifacts_written && options->output &&
        !runtime->WriteArtifacts(*options->output, options->click, error)) {
      std::cerr << error << '\n';
      return 1;
    }
    artifacts_written = true;
    if (options->headless) return 0;
    const int result = RunX11(*runtime, *options);
    if (result != 75) return result;
  }
}
