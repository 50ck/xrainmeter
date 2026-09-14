// SPDX-License-Identifier: GPL-2.0-only
#include <gtk/gtk.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

enum { COL_ACTIVE, COL_LABEL, COL_INDEX, COL_IS_SKIN, COL_ICON, COL_COUNT };

struct App;
struct Skin {
  App* app = nullptr;
  fs::path path;
  std::string config;
  std::string variant;
  GtkTreeIter iter{};
  GSubprocess* process = nullptr;
};

struct App {
  GtkApplication* application = nullptr;
  GtkWidget* window = nullptr;
  GtkWidget* tree = nullptr;
  GtkTreeStore* store = nullptr;
  GtkWidget* monitor = nullptr;
  GtkWidget* x = nullptr;
  GtkWidget* y = nullptr;
  GtkWidget* position = nullptr;
  GtkWidget* load_order = nullptr;
  GtkWidget* transparency = nullptr;
  GtkWidget* on_hover = nullptr;
  GtkWidget* draggable = nullptr;
  GtkWidget* click_through = nullptr;
  GtkWidget* keep_on_screen = nullptr;
  GtkWidget* save_position = nullptr;
  GtkWidget* snap_edges = nullptr;
  GtkWidget* favorite = nullptr;
  GtkWidget* context_menu = nullptr;
  GtkWidget* metadata = nullptr;
  GtkWidget* status = nullptr;
  fs::path skins;
  fs::path state_path;
  fs::path runtime;
  GKeyFile* state = g_key_file_new();
  std::vector<std::unique_ptr<Skin>> rows;
  bool shutting_down = false;
  bool restored = false;
  bool held = false;
  bool game_mode = false;
};

void Scan(App& app);

fs::path Xdg(const char* variable, const fs::path& fallback) {
  if (const char* value = std::getenv(variable); value && *value) return value;
  return fallback;
}

fs::path Home() {
  if (const char* value = std::getenv("HOME"); value && *value) return value;
  return fs::current_path();
}

std::string NormalizeConfig(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  while (!value.empty() && value.front() == '/') value.erase(value.begin());
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

fs::path SiblingExecutable(const char* name) {
  std::error_code error;
  const auto self = fs::read_symlink("/proc/self/exe", error);
  return error ? fs::path(name) : self.parent_path() / name;
}

fs::path FindExecutable(const char* name) {
  const auto sibling = SiblingExecutable(name);
  if (fs::is_regular_file(sibling)) return sibling;
  gchar* found = g_find_program_in_path(name);
  if (!found) return name;
  fs::path result = found;
  g_free(found);
  return result;
}

void OpenDefault(App& app, const fs::path& path) {
  GError* error = nullptr;
  gchar* uri = g_filename_to_uri(fs::absolute(path).c_str(), nullptr, &error);
  if (uri && !g_app_info_launch_default_for_uri(uri, nullptr, &error)) {
    g_free(uri);
    uri = nullptr;
  }
  if (uri) g_free(uri);
  if (error) {
    gtk_label_set_text(GTK_LABEL(app.status), error->message);
    g_error_free(error);
  }
}

void Save(App& app) {
  std::error_code error;
  fs::create_directories(app.state_path.parent_path(), error);
  GError* save_error = nullptr;
  if (!g_key_file_save_to_file(app.state, app.state_path.c_str(), &save_error)) {
    gtk_label_set_text(GTK_LABEL(app.status), save_error->message);
    g_error_free(save_error);
  }
}

bool StateBool(App& app, const std::string& group, const char* key, bool fallback = false) {
  GError* error = nullptr;
  const bool value = g_key_file_get_boolean(app.state, group.c_str(), key, &error);
  if (error) {
    g_error_free(error);
    return fallback;
  }
  return value;
}

int StateInt(App& app, const std::string& group, const char* key, int fallback) {
  GError* error = nullptr;
  const int value = g_key_file_get_integer(app.state, group.c_str(), key, &error);
  if (error) {
    g_error_free(error);
    return fallback;
  }
  return value;
}

std::string StateString(App& app, const std::string& group, const char* key,
                        const std::string& fallback = {}) {
  GError* error = nullptr;
  gchar* value = g_key_file_get_string(app.state, group.c_str(), key, &error);
  if (error) {
    g_error_free(error);
    return fallback;
  }
  std::string result = value;
  g_free(value);
  return result;
}

void SetActive(Skin& skin, bool active) {
  gtk_tree_store_set(skin.app->store, &skin.iter, COL_ACTIVE, active, -1);
  g_key_file_set_boolean(skin.app->state, skin.config.c_str(), "Active", active);
  if (active) g_key_file_set_string(skin.app->state, skin.config.c_str(), "Variant", skin.variant.c_str());
  Save(*skin.app);
}

void ChildDone(GObject* source, GAsyncResult* result, gpointer data) {
  auto& skin = *static_cast<Skin*>(data);
  GError* error = nullptr;
  g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);
  if (error) g_error_free(error);
  if (skin.process == G_SUBPROCESS(source)) {
    skin.process = nullptr;
    if (!skin.app->shutting_down) SetActive(skin, false);
  }
  g_object_unref(source);
}

void Stop(Skin& skin, bool remember = true) {
  if (skin.process) {
    GSubprocess* process = skin.process;
    skin.process = nullptr;
    g_subprocess_force_exit(process);
  }
  if (remember) SetActive(skin, false);
}

void Launch(Skin& skin) {
  App& app = *skin.app;
  for (auto& row : app.rows) {
    if (row.get() != &skin && row->config == skin.config && row->process) Stop(*row);
  }
  if (skin.process) g_subprocess_force_exit(skin.process);

  const std::string monitor = StateString(app, skin.config, "Monitor", "HDMI-0");
  const std::string position = std::to_string(StateInt(app, skin.config, "WindowX", 0)) + "," +
                               std::to_string(StateInt(app, skin.config, "WindowY", 0));
  const std::string state_file = app.state_path.string();
  const std::string runtime = app.runtime.string();
  const std::string path = skin.path.string();
  const gchar* arguments[] = {runtime.c_str(), "--monitor", monitor.c_str(), "--position", position.c_str(),
                              "--state-file", state_file.c_str(), "--state-section", skin.config.c_str(),
                              path.c_str(), nullptr};
  GError* error = nullptr;
  skin.process = g_subprocess_newv(arguments, G_SUBPROCESS_FLAGS_NONE, &error);
  if (!skin.process) {
    gtk_label_set_text(GTK_LABEL(app.status), error->message);
    g_error_free(error);
    return;
  }
  SetActive(skin, true);
  gtk_label_set_text(GTK_LABEL(app.status), ("Loaded " + skin.config + " / " + skin.variant).c_str());
  g_subprocess_wait_async(skin.process, nullptr, ChildDone, &skin);
}

Skin* Selected(App& app) {
  GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree));
  GtkTreeModel* model = nullptr;
  GtkTreeIter iter{};
  if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return nullptr;
  int index = -1;
  gtk_tree_model_get(model, &iter, COL_INDEX, &index, -1);
  return index >= 0 && static_cast<size_t>(index) < app.rows.size() ? app.rows[index].get() : nullptr;
}

void SelectSkin(App& app, Skin& skin) {
  GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(app.store), &skin.iter);
  gtk_tree_view_expand_to_path(GTK_TREE_VIEW(app.tree), path);
  gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree)), &skin.iter);
  gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(app.tree), path, nullptr, TRUE, 0.5, 0.0);
  gtk_tree_path_free(path);
}

void SaveControls(App& app, Skin& skin) {
  const char* monitor = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app.monitor));
  if (monitor) g_key_file_set_string(app.state, skin.config.c_str(), "Monitor", monitor);
  g_key_file_set_integer(app.state, skin.config.c_str(), "WindowX",
                         gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app.x)));
  g_key_file_set_integer(app.state, skin.config.c_str(), "WindowY",
                         gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app.y)));
  const char* z_position = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app.position));
  const char* hover = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app.on_hover));
  g_key_file_set_integer(app.state, skin.config.c_str(), "AlwaysOnTop",
                         z_position ? std::atoi(z_position) : 0);
  g_key_file_set_integer(app.state, skin.config.c_str(), "LoadOrder",
                         gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app.load_order)));
  const int transparent = gtk_range_get_value(GTK_RANGE(app.transparency));
  g_key_file_set_integer(app.state, skin.config.c_str(), "AlphaValue",
                         std::max(1, 255 * (100 - transparent) / 100));
  g_key_file_set_integer(app.state, skin.config.c_str(), "OnHover", hover ? std::atoi(hover) : 0);
  g_key_file_set_boolean(app.state, skin.config.c_str(), "Draggable",
                         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.draggable)));
  g_key_file_set_boolean(app.state, skin.config.c_str(), "ClickThrough",
                         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.click_through)));
  g_key_file_set_boolean(app.state, skin.config.c_str(), "KeepOnScreen",
                         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.keep_on_screen)));
  g_key_file_set_boolean(app.state, skin.config.c_str(), "SavePosition",
                         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.save_position)));
  g_key_file_set_boolean(app.state, skin.config.c_str(), "SnapEdges",
                         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.snap_edges)));
  g_key_file_set_boolean(app.state, skin.config.c_str(), "Favorite",
                         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.favorite)));
  g_key_file_set_boolean(app.state, skin.config.c_str(), "ContextMenu",
                         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.context_menu)));
  Save(app);
}

void SelectionChanged(GtkTreeSelection*, gpointer data) {
  auto& app = *static_cast<App*>(data);
  Skin* skin = Selected(app);
  if (!skin) {
    gtk_label_set_text(GTK_LABEL(app.metadata), "Select a skin .ini file.");
    return;
  }
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.x), StateInt(app, skin->config, "WindowX", 0));
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.y), StateInt(app, skin->config, "WindowY", 0));
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.position),
                              std::to_string(StateInt(app, skin->config, "AlwaysOnTop", 0)).c_str());
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.load_order),
                            StateInt(app, skin->config, "LoadOrder", 0));
  const int alpha = std::clamp(StateInt(app, skin->config, "AlphaValue", 255), 1, 255);
  gtk_range_set_value(GTK_RANGE(app.transparency), 100 - alpha * 100 / 255);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.on_hover),
                              std::to_string(StateInt(app, skin->config, "OnHover", 0)).c_str());
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.draggable),
                               StateBool(app, skin->config, "Draggable", true));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.click_through),
                               StateBool(app, skin->config, "ClickThrough"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.keep_on_screen),
                               StateBool(app, skin->config, "KeepOnScreen", true));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.save_position),
                               StateBool(app, skin->config, "SavePosition", true));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.snap_edges),
                               StateBool(app, skin->config, "SnapEdges", true));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.favorite),
                               StateBool(app, skin->config, "Favorite"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.context_menu),
                               StateBool(app, skin->config, "ContextMenu", true));
  gtk_label_set_text(GTK_LABEL(app.metadata),
                     ("Config: " + skin->config + "\nFile: " + skin->path.string()).c_str());
  const auto wanted = StateString(app, skin->config, "Monitor", "HDMI-0");
  GtkTreeModel* model = gtk_combo_box_get_model(GTK_COMBO_BOX(app.monitor));
  GtkTreeIter iter{};
  int index = 0;
  if (gtk_tree_model_get_iter_first(model, &iter)) {
    do {
      gchar* value = nullptr;
      gtk_tree_model_get(model, &iter, 0, &value, -1);
      if (value && wanted == value) gtk_combo_box_set_active(GTK_COMBO_BOX(app.monitor), index);
      g_free(value);
      ++index;
    } while (gtk_tree_model_iter_next(model, &iter));
  }
}

void LoadClicked(GtkButton*, gpointer data) {
  auto& app = *static_cast<App*>(data);
  if (Skin* skin = Selected(app)) {
    SaveControls(app, *skin);
    Launch(*skin);
  }
}

void ToggleSelected(App& app, Skin& skin) {
  if (skin.process) Stop(skin);
  else {
    SaveControls(app, skin);
    Launch(skin);
  }
}

void ActiveToggled(GtkCellRendererToggle*, gchar* tree_path, gpointer data) {
  auto& app = *static_cast<App*>(data);
  GtkTreePath* path = gtk_tree_path_new_from_string(tree_path);
  GtkTreeIter iter{};
  if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app.store), &iter, path)) {
    int index = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(app.store), &iter, COL_INDEX, &index, -1);
    if (index >= 0 && static_cast<size_t>(index) < app.rows.size()) {
      gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree)), &iter);
      ToggleSelected(app, *app.rows[index]);
    }
  }
  gtk_tree_path_free(path);
}

void UnloadClicked(GtkButton*, gpointer data) {
  auto& app = *static_cast<App*>(data);
  if (Skin* skin = Selected(app)) Stop(*skin);
}

void RefreshClicked(GtkButton*, gpointer data) {
  auto& app = *static_cast<App*>(data);
  if (Skin* skin = Selected(app)) {
    SaveControls(app, *skin);
    Launch(*skin);
  }
}

void ApplyClicked(GtkButton*, gpointer data) {
  auto& app = *static_cast<App*>(data);
  if (Skin* skin = Selected(app)) {
    SaveControls(app, *skin);
    if (skin->process) Launch(*skin);
    gtk_label_set_text(GTK_LABEL(app.status), "Skin settings applied.");
  }
}

void RefreshAll(App& app) {
  Scan(app);
  for (auto& row : app.rows) if (row->process) Launch(*row);
  gtk_label_set_text(GTK_LABEL(app.status), "Refreshed all active skins.");
}

void SaveLayout(GtkButton*, gpointer data) {
  auto& app = *static_cast<App*>(data);
  GtkWidget* dialog = gtk_file_chooser_dialog_new("Save Rainmeter layout", GTK_WINDOW(app.window),
      GTK_FILE_CHOOSER_ACTION_SAVE, "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, nullptr);
  const fs::path directory = app.state_path.parent_path() / "Layouts";
  std::error_code directory_error;
  fs::create_directories(directory, directory_error);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), directory.c_str());
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "Layout.ini");
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    Save(app);
    std::error_code error;
    fs::copy_file(app.state_path, filename, fs::copy_options::overwrite_existing, error);
    gtk_label_set_text(GTK_LABEL(app.status), error ? error.message().c_str() : "Layout saved.");
    g_free(filename);
  }
  gtk_widget_destroy(dialog);
}

void LoadLayout(GtkButton*, gpointer data) {
  auto& app = *static_cast<App*>(data);
  GtkWidget* dialog = gtk_file_chooser_dialog_new("Load Rainmeter layout", GTK_WINDOW(app.window),
      GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL, "Load", GTK_RESPONSE_ACCEPT, nullptr);
  const fs::path directory = app.state_path.parent_path() / "Layouts";
  std::error_code directory_error;
  fs::create_directories(directory, directory_error);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), directory.c_str());
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    Save(app);
    std::error_code backup_error;
    fs::copy_file(app.state_path, directory / "@Backup.ini", fs::copy_options::overwrite_existing,
                  backup_error);
    GKeyFile* layout = g_key_file_new();
    GError* error = nullptr;
    if (g_key_file_load_from_file(layout, filename, G_KEY_FILE_KEEP_COMMENTS, &error)) {
      for (auto& row : app.rows) if (row->process) Stop(*row, false);
      g_key_file_unref(app.state);
      app.state = layout;
      Save(app);
      for (auto& row : app.rows) {
        const bool active = StateBool(app, row->config, "Active") &&
                            StateString(app, row->config, "Variant") == row->variant;
        gtk_tree_store_set(app.store, &row->iter, COL_ACTIVE, active, -1);
        if (active) Launch(*row);
      }
      gtk_label_set_text(GTK_LABEL(app.status), "Layout loaded; previous state saved as @Backup.ini.");
    } else {
      gtk_label_set_text(GTK_LABEL(app.status), error->message);
      g_error_free(error);
      g_key_file_unref(layout);
    }
    g_free(filename);
  }
  gtk_widget_destroy(dialog);
}

void ToggleGameMode(GtkButton* button, gpointer data) {
  auto& app = *static_cast<App*>(data);
  app.game_mode = !app.game_mode;
  if (app.game_mode) {
    for (auto& row : app.rows) if (row->process) Stop(*row, false);
    gtk_button_set_label(button, "Stop game mode");
    gtk_label_set_text(GTK_LABEL(app.status), "Game mode: active skins are temporarily unloaded.");
  } else {
    for (auto& row : app.rows) {
      if (StateBool(app, row->config, "Active") &&
          StateString(app, row->config, "Variant") == row->variant) Launch(*row);
    }
    gtk_button_set_label(button, "Start game mode");
    gtk_label_set_text(GTK_LABEL(app.status), "Game mode stopped; active skins restored.");
  }
}

void OpenSelected(App& app, bool edit) {
  Skin* skin = Selected(app);
  if (!skin) return;
  OpenDefault(app, edit ? skin->path : skin->path.parent_path());
}

GtkTreeIter EnsureFolder(App& app, const GtkTreeIter* parent, const std::string& name) {
  GtkTreeModel* model = GTK_TREE_MODEL(app.store);
  GtkTreeIter current{};
  if (gtk_tree_model_iter_children(model, &current, const_cast<GtkTreeIter*>(parent))) {
    do {
      gchar* label = nullptr;
      gboolean is_skin = FALSE;
      gtk_tree_model_get(model, &current, COL_LABEL, &label, COL_IS_SKIN, &is_skin, -1);
      const bool match = !is_skin && label && name == label;
      g_free(label);
      if (match) return current;
    } while (gtk_tree_model_iter_next(model, &current));
  }
  GtkTreeIter folder{};
  gtk_tree_store_append(app.store, &folder, const_cast<GtkTreeIter*>(parent));
  gtk_tree_store_set(app.store, &folder, COL_ACTIVE, FALSE, COL_LABEL, name.c_str(),
                     COL_INDEX, -1, COL_IS_SKIN, FALSE, COL_ICON, "folder", -1);
  return folder;
}

void AddSkin(App& app, const fs::path& path) {
  const auto relative = path.lexically_relative(app.skins);
  if (relative.empty()) return;
  for (const auto& part : relative) {
    if (!part.string().empty() && part.string().front() == '@') return;
  }
  auto skin = std::make_unique<Skin>();
  skin->app = &app;
  skin->path = path;
  skin->variant = path.filename().string();
  skin->config = relative.parent_path().generic_string();
  const int index = static_cast<int>(app.rows.size());
  const bool active = StateBool(app, skin->config, "Active") &&
                      StateString(app, skin->config, "Variant") == skin->variant;
  GtkTreeIter parent{};
  bool has_parent = false;
  for (const auto& component : relative.parent_path()) {
    parent = EnsureFolder(app, has_parent ? &parent : nullptr, component.string());
    has_parent = true;
  }
  gtk_tree_store_append(app.store, &skin->iter, has_parent ? &parent : nullptr);
  gtk_tree_store_set(app.store, &skin->iter, COL_ACTIVE, active,
                     COL_LABEL, skin->variant.c_str(), COL_INDEX, index,
                     COL_IS_SKIN, TRUE, COL_ICON, "text-x-generic", -1);
  app.rows.push_back(std::move(skin));
}

void Scan(App& app) {
  std::vector<fs::path> paths;
  std::error_code error;
  if (fs::is_directory(app.skins, error)) {
    for (const auto& entry : fs::recursive_directory_iterator(app.skins, error)) {
      if (entry.is_regular_file() && entry.path().extension() == ".ini") paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  for (const auto& path : paths) {
    bool known = false;
    std::error_code equivalent_error;
    for (const auto& row : app.rows) {
      if (fs::equivalent(row->path, path, equivalent_error)) known = true;
      equivalent_error.clear();
    }
    if (!known) AddSkin(app, path);
  }
}

struct ImportJob {
  App* app;
  fs::path package;
  GSubprocess* process;
};

void ImportDone(GObject*, GAsyncResult* result, gpointer data) {
  std::unique_ptr<ImportJob> job(static_cast<ImportJob*>(data));
  GError* error = nullptr;
  const bool waited = g_subprocess_wait_finish(job->process, result, &error);
  if (waited && g_subprocess_get_successful(job->process)) {
    Scan(*job->app);
    gtk_label_set_text(GTK_LABEL(job->app->status),
        ("Installed " + job->package.filename().string() + "; skins are now in the sidebar.").c_str());
  } else {
    const std::string message = error ? error->message : "The .rmskin installer failed.";
    gtk_label_set_text(GTK_LABEL(job->app->status), message.c_str());
  }
  if (error) g_error_free(error);
  g_object_unref(job->process);
}

void ImportPackage(App& app, const fs::path& package) {
  const std::string installer = FindExecutable("rainmeter-rmskin").string();
  const std::string filename = fs::absolute(package).string();
  const gchar* arguments[] = {installer.c_str(), "--no-launch", filename.c_str(), nullptr};
  GError* error = nullptr;
  GSubprocess* process = g_subprocess_newv(arguments, G_SUBPROCESS_FLAGS_NONE, &error);
  if (!process) {
    gtk_label_set_text(GTK_LABEL(app.status), error->message);
    g_error_free(error);
    return;
  }
  gtk_label_set_text(GTK_LABEL(app.status), ("Installing " + package.filename().string() + "…").c_str());
  auto* job = new ImportJob{&app, package, process};
  g_subprocess_wait_async(process, nullptr, ImportDone, job);
}

void PackagesDropped(GtkWidget*, GdkDragContext* context, gint, gint, GtkSelectionData* selection,
                     guint, guint time, gpointer data) {
  auto& app = *static_cast<App*>(data);
  bool accepted = false;
  gchar** uris = gtk_selection_data_get_uris(selection);
  if (uris) {
    for (size_t index = 0; uris[index]; ++index) {
      GError* error = nullptr;
      gchar* filename = g_filename_from_uri(uris[index], nullptr, &error);
      std::string extension = filename ? fs::path(filename).extension().string() : "";
      std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      if (filename && extension == ".rmskin") {
        ImportPackage(app, filename);
        accepted = true;
      }
      if (filename) g_free(filename);
      if (error) g_error_free(error);
    }
    g_strfreev(uris);
  }
  gtk_drag_finish(context, accepted, false, time);
}

gboolean HideWindow(GtkWidget* widget, GdkEvent*, gpointer) {
  gtk_widget_hide(widget);
  return TRUE;
}

void OpenSettingsClicked(GtkButton*, gpointer data) {
  auto& app = *static_cast<App*>(data);
  OpenDefault(app, app.state_path);
}

void AddMonitors(GtkComboBoxText* combo) {
  Display* display = XOpenDisplay(nullptr);
  if (!display) {
    gtk_combo_box_text_append_text(combo, "HDMI-0");
    return;
  }
  XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, DefaultRootWindow(display));
  if (resources) {
    for (int index = 0; index < resources->noutput; ++index) {
      XRROutputInfo* output = XRRGetOutputInfo(display, resources, resources->outputs[index]);
      if (output && output->connection == RR_Connected && output->crtc) {
        gtk_combo_box_text_append_text(combo, std::string(output->name, output->nameLen).c_str());
      }
      if (output) XRRFreeOutputInfo(output);
    }
    XRRFreeScreenResources(resources);
  }
  XCloseDisplay(display);
  gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

void BuildWindow(App& app) {
  app.window = gtk_application_window_new(app.application);
  gtk_window_set_title(GTK_WINDOW(app.window), "Manage Rainmeter");
  gtk_window_set_default_size(GTK_WINDOW(app.window), 940, 620);
  g_signal_connect(app.window, "delete-event", G_CALLBACK(HideWindow), nullptr);
  const GtkTargetEntry drop_targets[] = {{const_cast<gchar*>("text/uri-list"), 0, 0}};
  gtk_drag_dest_set(app.window, GTK_DEST_DEFAULT_ALL, drop_targets, 1, GDK_ACTION_COPY);
  g_signal_connect(app.window, "drag-data-received", G_CALLBACK(PackagesDropped), &app);

  GtkWidget* root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(root_box), 10);
  gtk_container_add(GTK_CONTAINER(app.window), root_box);
  GtkWidget* notebook = gtk_notebook_new();
  gtk_box_pack_start(GTK_BOX(root_box), notebook, TRUE, TRUE, 0);
  GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), outer, gtk_label_new("Skins"));
  GtkWidget* columns = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position(GTK_PANED(columns), 285);
  gtk_box_pack_start(GTK_BOX(outer), columns, TRUE, TRUE, 0);
  GtkWidget* left_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget* right_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(left_panel), 6);
  gtk_container_set_border_width(GTK_CONTAINER(right_panel), 6);
  gtk_paned_pack1(GTK_PANED(columns), left_panel, FALSE, FALSE);
  gtk_paned_pack2(GTK_PANED(columns), right_panel, TRUE, FALSE);

  GtkWidget* active_skins = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(active_skins), "Active skins");
  gtk_combo_box_set_active(GTK_COMBO_BOX(active_skins), 0);
  gtk_box_pack_start(GTK_BOX(left_panel), active_skins, FALSE, FALSE, 0);

  app.store = gtk_tree_store_new(COL_COUNT, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_INT,
                                 G_TYPE_BOOLEAN, G_TYPE_STRING);
  app.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.store));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app.tree), FALSE);
  gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(app.tree), TRUE);
  gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(app.tree), 8);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app.store), COL_LABEL, GTK_SORT_ASCENDING);
  GtkCellRenderer* active_renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(active_renderer, "toggled", G_CALLBACK(ActiveToggled), &app);
  GtkTreeViewColumn* skin_column = gtk_tree_view_column_new();
  GtkCellRenderer* icon_renderer = gtk_cell_renderer_pixbuf_new();
  GtkCellRenderer* name_renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(skin_column, active_renderer, FALSE);
  gtk_tree_view_column_add_attribute(skin_column, active_renderer, "active", COL_ACTIVE);
  gtk_tree_view_column_add_attribute(skin_column, active_renderer, "visible", COL_IS_SKIN);
  gtk_tree_view_column_pack_start(skin_column, icon_renderer, FALSE);
  gtk_tree_view_column_add_attribute(skin_column, icon_renderer, "icon-name", COL_ICON);
  gtk_tree_view_column_pack_start(skin_column, name_renderer, TRUE);
  gtk_tree_view_column_add_attribute(skin_column, name_renderer, "text", COL_LABEL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree), skin_column);
  gtk_tree_view_set_expander_column(GTK_TREE_VIEW(app.tree), skin_column);
  GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_widget_set_size_request(scroll, 270, 360);
  gtk_container_add(GTK_CONTAINER(scroll), app.tree);
  gtk_box_pack_start(GTK_BOX(left_panel), scroll, TRUE, TRUE, 0);
  GtkWidget* package = gtk_button_new_with_label("Create .rmskin package…");
  gtk_widget_set_sensitive(package, FALSE);
  gtk_widget_set_tooltip_text(package, "Package creation is not implemented yet.");
  gtk_box_pack_start(GTK_BOX(left_panel), package, FALSE, FALSE, 0);

  app.metadata = gtk_label_new("Select a skin configuration.");
  gtk_label_set_xalign(GTK_LABEL(app.metadata), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(app.metadata), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(right_panel), app.metadata, FALSE, FALSE, 0);
  GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_pack_start(GTK_BOX(right_panel), separator, FALSE, FALSE, 4);

  GtkWidget* settings = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(settings), 8);
  gtk_grid_set_row_spacing(GTK_GRID(settings), 6);
  gtk_grid_attach(GTK_GRID(settings), gtk_label_new("Monitor"), 0, 0, 1, 1);
  app.monitor = gtk_combo_box_text_new();
  AddMonitors(GTK_COMBO_BOX_TEXT(app.monitor));
  gtk_grid_attach(GTK_GRID(settings), app.monitor, 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(settings), gtk_label_new("X"), 2, 0, 1, 1);
  app.x = gtk_spin_button_new_with_range(-32768, 32768, 1);
  gtk_grid_attach(GTK_GRID(settings), app.x, 3, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(settings), gtk_label_new("Y"), 4, 0, 1, 1);
  app.y = gtk_spin_button_new_with_range(-32768, 32768, 1);
  gtk_grid_attach(GTK_GRID(settings), app.y, 5, 0, 1, 1);

  gtk_grid_attach(GTK_GRID(settings), gtk_label_new("Position"), 0, 1, 1, 1);
  app.position = gtk_combo_box_text_new();
  for (const auto& item : std::vector<std::pair<const char*, const char*>>{
           {"2", "Stay topmost"}, {"1", "Topmost"}, {"0", "Normal"},
           {"-1", "Bottom"}, {"-2", "On desktop"}}) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.position), item.first, item.second);
  }
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.position), "0");
  gtk_grid_attach(GTK_GRID(settings), app.position, 1, 1, 2, 1);
  gtk_grid_attach(GTK_GRID(settings), gtk_label_new("Load order"), 3, 1, 1, 1);
  app.load_order = gtk_spin_button_new_with_range(-32768, 32768, 1);
  gtk_grid_attach(GTK_GRID(settings), app.load_order, 4, 1, 2, 1);

  gtk_grid_attach(GTK_GRID(settings), gtk_label_new("Transparency"), 0, 2, 1, 1);
  app.transparency = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 99, 10);
  gtk_scale_set_value_pos(GTK_SCALE(app.transparency), GTK_POS_RIGHT);
  gtk_widget_set_hexpand(app.transparency, TRUE);
  gtk_grid_attach(GTK_GRID(settings), app.transparency, 1, 2, 3, 1);
  gtk_grid_attach(GTK_GRID(settings), gtk_label_new("On hover"), 4, 2, 1, 1);
  app.on_hover = gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.on_hover), "0", "Do nothing");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.on_hover), "1", "Hide");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.on_hover), "2", "Fade in");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.on_hover), "3", "Fade out");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.on_hover), "0");
  gtk_grid_attach(GTK_GRID(settings), app.on_hover, 5, 2, 1, 1);

  GtkWidget* toggles = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(toggles), 12);
  gtk_grid_set_row_spacing(GTK_GRID(toggles), 4);
  app.draggable = gtk_check_button_new_with_label("Draggable");
  app.click_through = gtk_check_button_new_with_label("Click through");
  app.keep_on_screen = gtk_check_button_new_with_label("Keep on screen");
  app.save_position = gtk_check_button_new_with_label("Save position");
  app.snap_edges = gtk_check_button_new_with_label("Snap to edges");
  app.favorite = gtk_check_button_new_with_label("Favorite");
  app.context_menu = gtk_check_button_new_with_label("Context menu");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.context_menu), TRUE);
  int toggle_index = 0;
  for (GtkWidget* toggle : {app.draggable, app.click_through, app.keep_on_screen,
                            app.save_position, app.snap_edges, app.favorite, app.context_menu}) {
    gtk_grid_attach(GTK_GRID(toggles), toggle, toggle_index % 2, toggle_index / 2, 1, 1);
    ++toggle_index;
  }
  gtk_grid_attach(GTK_GRID(settings), toggles, 0, 3, 6, 1);
  gtk_box_pack_start(GTK_BOX(right_panel), settings, FALSE, FALSE, 0);

  GtkWidget* buttons = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(buttons), 5);
  gtk_grid_set_row_spacing(GTK_GRID(buttons), 5);
  int button_index = 0;
  for (const auto& item : std::vector<std::pair<const char*, GCallback>>{
           {"Load", G_CALLBACK(LoadClicked)}, {"Unload", G_CALLBACK(UnloadClicked)},
           {"Refresh", G_CALLBACK(RefreshClicked)}}) {
    GtkWidget* button = gtk_button_new_with_label(item.first);
    g_signal_connect(button, "clicked", item.second, &app);
    gtk_grid_attach(GTK_GRID(buttons), button, button_index % 4, button_index / 4, 1, 1);
    ++button_index;
  }
  GtkWidget* edit = gtk_button_new_with_label("Edit");
  g_signal_connect_swapped(edit, "clicked", G_CALLBACK(+[](App* value) { OpenSelected(*value, true); }), &app);
  gtk_grid_attach(GTK_GRID(buttons), edit, button_index % 4, button_index / 4, 1, 1);
  ++button_index;
  GtkWidget* folder = gtk_button_new_with_label("Open folder");
  g_signal_connect_swapped(folder, "clicked", G_CALLBACK(+[](App* value) { OpenSelected(*value, false); }), &app);
  gtk_grid_attach(GTK_GRID(buttons), folder, button_index % 4, button_index / 4, 1, 1);
  ++button_index;
  GtkWidget* rescan = gtk_button_new_with_label("Refresh all");
  g_signal_connect_swapped(rescan, "clicked", G_CALLBACK(+[](App* value) { RefreshAll(*value); }), &app);
  gtk_grid_attach(GTK_GRID(buttons), rescan, button_index % 4, button_index / 4, 1, 1);
  ++button_index;
  GtkWidget* apply = gtk_button_new_with_label("Apply settings");
  g_signal_connect(apply, "clicked", G_CALLBACK(ApplyClicked), &app);
  gtk_grid_attach(GTK_GRID(buttons), apply, button_index % 4, button_index / 4, 1, 1);
  ++button_index;
  GtkWidget* close = gtk_button_new_with_label("Close");
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_hide), app.window);
  gtk_grid_attach(GTK_GRID(buttons), close, button_index % 4, button_index / 4, 1, 1);
  gtk_box_pack_end(GTK_BOX(right_panel), buttons, FALSE, FALSE, 0);

  app.status = gtk_label_new("Select a skin configuration.");
  gtk_widget_set_halign(app.status, GTK_ALIGN_START);
  gtk_box_pack_end(GTK_BOX(right_panel), app.status, FALSE, FALSE, 0);
  g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree)), "changed",
                   G_CALLBACK(SelectionChanged), &app);
  g_signal_connect(app.tree, "row-activated", G_CALLBACK(+[](GtkTreeView*, GtkTreePath*, GtkTreeViewColumn*,
                                                               gpointer data) {
    auto& value = *static_cast<App*>(data);
    if (Skin* skin = Selected(value)) ToggleSelected(value, *skin);
  }), &app);

  GtkWidget* layouts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width(GTK_CONTAINER(layouts), 18);
  GtkWidget* layout_help = gtk_label_new(
      "Save and restore active variants, monitor assignments, positions, and skin settings.\n"
      "Loading a layout first saves the current state as @Backup.ini.");
  gtk_label_set_xalign(GTK_LABEL(layout_help), 0.0);
  gtk_box_pack_start(GTK_BOX(layouts), layout_help, FALSE, FALSE, 0);
  GtkWidget* layout_buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
  GtkWidget* save_layout = gtk_button_new_with_label("Save new layout…");
  GtkWidget* load_layout = gtk_button_new_with_label("Load saved layout…");
  g_signal_connect(save_layout, "clicked", G_CALLBACK(SaveLayout), &app);
  g_signal_connect(load_layout, "clicked", G_CALLBACK(LoadLayout), &app);
  gtk_container_add(GTK_CONTAINER(layout_buttons), save_layout);
  gtk_container_add(GTK_CONTAINER(layout_buttons), load_layout);
  gtk_box_pack_start(GTK_BOX(layouts), layout_buttons, FALSE, FALSE, 0);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), layouts, gtk_label_new("Layouts"));

  GtkWidget* game = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width(GTK_CONTAINER(game), 18);
  GtkWidget* game_help = gtk_label_new(
      "Game mode temporarily unloads active skins and restores the same set when stopped.\n"
      "Automatic full-screen / process detection is not implemented yet.");
  gtk_label_set_xalign(GTK_LABEL(game_help), 0.0);
  gtk_box_pack_start(GTK_BOX(game), game_help, FALSE, FALSE, 0);
  GtkWidget* game_toggle = gtk_button_new_with_label("Start game mode");
  g_signal_connect(game_toggle, "clicked", G_CALLBACK(ToggleGameMode), &app);
  gtk_box_pack_start(GTK_BOX(game), game_toggle, FALSE, FALSE, 0);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), game, gtk_label_new("Game mode"));

  GtkWidget* general = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(general), 18);
  gtk_box_pack_start(GTK_BOX(general), gtk_label_new(
      "Linux runtime settings\n\n"
      "Language follows the desktop locale. Folders and editors follow desktop MIME defaults.\n"
      "Update installation, Windows notification-area behavior, Direct2D acceleration,\n"
      "and Rainmeter's Windows log viewer have no compatible implementation yet."), FALSE, FALSE, 0);
  GtkWidget* open_settings = gtk_button_new_with_label("Edit settings file");
  g_signal_connect(open_settings, "clicked", G_CALLBACK(OpenSettingsClicked), &app);
  gtk_box_pack_start(GTK_BOX(general), open_settings, FALSE, FALSE, 0);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), general, gtk_label_new("Settings"));
  Scan(app);
}

void Restore(App& app) {
  if (app.restored) return;
  app.restored = true;
  for (auto& row : app.rows) {
    if (StateBool(app, row->config, "Active") &&
        StateString(app, row->config, "Variant") == row->variant) Launch(*row);
  }
}

void Activate(GtkApplication*, gpointer data) {
  auto& app = *static_cast<App*>(data);
  if (!app.held) {
    g_application_hold(G_APPLICATION(app.application));
    app.held = true;
  }
  if (!app.window) BuildWindow(app);
  else Scan(app);
  Restore(app);
  gtk_widget_show_all(app.window);
  gtk_window_present(GTK_WINDOW(app.window));
}

int CommandLine(GApplication* application, GApplicationCommandLine* command_line, gpointer data) {
  auto& app = *static_cast<App*>(data);
  int count = 0;
  gchar** arguments = g_application_command_line_get_arguments(command_line, &count);
  std::string load;
  std::string select;
  std::string monitor;
  bool quit = false;
  bool refresh_all = false;
  std::string activate_config;
  std::string toggle_config;
  std::string deactivate_config;
  std::string refresh_config;
  std::string variant;
  int x = 0, y = 0;
  for (int index = 1; index < count; ++index) {
    const std::string argument = arguments[index];
    if (argument == "--monitor" && index + 1 < count) monitor = arguments[++index];
    else if (argument == "--quit") quit = true;
    else if (argument == "--refresh-all") refresh_all = true;
    else if (argument == "--activate-config" && index + 1 < count) activate_config = arguments[++index];
    else if (argument == "--toggle-config" && index + 1 < count) toggle_config = arguments[++index];
    else if (argument == "--deactivate-config" && index + 1 < count) deactivate_config = arguments[++index];
    else if (argument == "--refresh-config" && index + 1 < count) refresh_config = arguments[++index];
    else if (argument == "--variant" && index + 1 < count) variant = arguments[++index];
    else if (argument == "--select" && index + 1 < count) select = arguments[++index];
    else if (argument == "--position" && index + 1 < count) {
      const std::string value = arguments[++index];
      const auto comma = value.find(',');
      if (comma != std::string::npos) {
        x = std::atoi(value.substr(0, comma).c_str());
        y = std::atoi(value.substr(comma + 1).c_str());
      }
    } else if (argument == "--load" && index + 1 < count) load = arguments[++index];
    else if (!argument.empty() && argument.front() != '-') load = argument;
  }
  g_strfreev(arguments);
  if (quit) {
    app.shutting_down = true;
    for (auto& row : app.rows) if (row->process) Stop(*row, false);
    if (app.held) {
      g_application_release(application);
      app.held = false;
    }
    g_application_quit(application);
    return 0;
  }
  g_application_activate(application);
  if (refresh_all) RefreshAll(app);
  if (!activate_config.empty()) {
    Skin* chosen = nullptr;
    const auto config_name = NormalizeConfig(activate_config);
    for (auto& row : app.rows) {
      if (NormalizeConfig(row->config) != config_name) continue;
      if (!variant.empty() && NormalizeConfig(row->variant) != NormalizeConfig(variant)) continue;
      if (!chosen || row->variant == StateString(app, row->config, "Variant")) chosen = row.get();
    }
    if (chosen) {
      SelectSkin(app, *chosen);
      Launch(*chosen);
    }
  }
  if (!toggle_config.empty()) {
    Skin* chosen = nullptr;
    const auto config_name = NormalizeConfig(toggle_config);
    for (auto& row : app.rows) {
      if (NormalizeConfig(row->config) != config_name) continue;
      if (!variant.empty() && NormalizeConfig(row->variant) != NormalizeConfig(variant)) continue;
      if (row->process) {
        Stop(*row);
        chosen = nullptr;
        break;
      }
      if (!chosen || row->variant == StateString(app, row->config, "Variant")) chosen = row.get();
    }
    if (chosen) {
      SelectSkin(app, *chosen);
      Launch(*chosen);
    }
  }
  if (!deactivate_config.empty() || !refresh_config.empty()) {
    const bool refresh = !refresh_config.empty();
    const auto config_name = NormalizeConfig(refresh ? refresh_config : deactivate_config);
    for (auto& row : app.rows) {
      if (NormalizeConfig(row->config) != config_name || !row->process) continue;
      if (refresh) Launch(*row);
      else Stop(*row);
    }
  }
  const std::string requested = !load.empty() ? load : select;
  if (!requested.empty()) {
    const fs::path wanted = fs::absolute(requested);
    std::string extension = wanted.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    if (extension == ".rmskin") {
      ImportPackage(app, wanted);
      return 0;
    }
    Skin* found = nullptr;
    std::error_code equivalent_error;
    for (auto& row : app.rows) {
      if (fs::equivalent(row->path, wanted, equivalent_error)) found = row.get();
      equivalent_error.clear();
    }
    if (!found && fs::is_regular_file(wanted)) {
      AddSkin(app, wanted);
      found = app.rows.back().get();
    }
    if (found) {
      SelectSkin(app, *found);
      if (!load.empty()) {
        if (!monitor.empty()) g_key_file_set_string(app.state, found->config.c_str(), "Monitor", monitor.c_str());
        g_key_file_set_integer(app.state, found->config.c_str(), "WindowX", x);
        g_key_file_set_integer(app.state, found->config.c_str(), "WindowY", y);
        Launch(*found);
      }
    }
  }
  return 0;
}

}

int main(int argc, char** argv) {
  App app;
  app.skins = Xdg("XDG_DATA_HOME", Home() / ".local/share") / "rainmeter-linux/Skins";
  app.state_path = Xdg("XDG_CONFIG_HOME", Home() / ".config") / "rainmeter-linux/state.ini";
  app.runtime = SiblingExecutable("rainmeter-linux");
  GError* error = nullptr;
  g_key_file_load_from_file(app.state, app.state_path.c_str(), G_KEY_FILE_KEEP_COMMENTS, &error);
  if (error) g_error_free(error);
  app.application = gtk_application_new("net.rainmeter.linux", G_APPLICATION_HANDLES_COMMAND_LINE);
  g_signal_connect(app.application, "activate", G_CALLBACK(Activate), &app);
  g_signal_connect(app.application, "command-line", G_CALLBACK(CommandLine), &app);
  const int result = g_application_run(G_APPLICATION(app.application), argc, argv);
  app.shutting_down = true;
  for (auto& row : app.rows) if (row->process) g_subprocess_force_exit(row->process);
  g_object_unref(app.application);
  g_key_file_unref(app.state);
  return result;
}
