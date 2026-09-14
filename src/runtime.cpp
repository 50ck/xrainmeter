// SPDX-License-Identifier: GPL-2.0-only
#include "runtime.h"

#include "math_parser.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <codecvt>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <optional>
#include <regex>
#include <ctime>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace rm {
namespace {

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string PluginName(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  value = Lower(std::filesystem::path(value).filename().string());
  if (value.size() > 4 && value.substr(value.size() - 4) == ".dll") value.resize(value.size() - 4);
  static const std::unordered_map<std::string, std::string> aliases = {
      {"pluginquote", "quote"}, {"quoteplugin", "quote"},
      {"pluginpower", "power"}, {"powerplugin", "power"},
      {"pluginspeedfan", "speedfan"}, {"speedfanplugin", "speedfan"},
      {"pluginwin7audio", "win7audio"}, {"win7audioplugin", "win7audio"},
      {"pluginwindowmessage", "windowmessage"}, {"windowmessageplugin", "windowmessage"},
      {"pluginactiontimer", "actiontimer"}, {"pluginadvancedcpu", "advancedcpu"},
      {"pluginaudiolevel", "audiolevel"}, {"plugincoretemp", "coretemp"},
      {"pluginfileview", "fileview"}, {"pluginfolderinfo", "folderinfo"},
      {"plugininputtext", "inputtext"}, {"pluginperfmon", "perfmon"},
      {"pluginping", "ping"}, {"pluginresmon", "resmon"},
      {"pluginruncommand", "runcommand"}, {"pluginusagemonitor", "usagemonitor"},
      {"pluginitunes", "itunes"}, {"nowplayingplugin", "nowplaying"},
      {"webparserplugin", "webparser"}};
  if (const auto found = aliases.find(value); found != aliases.end()) return found->second;
  if (value.rfind("plugin", 0) == 0) value.erase(0, 6);
  if (value.size() > 6 && value.substr(value.size() - 6) == "plugin") value.resize(value.size() - 6);
  return value;
}

bool KnownPlugin(const std::string& name) {
  static const std::set<std::string> known = {
      "actiontimer", "advancedcpu", "audiolevel", "coretemp", "fileview", "folderinfo",
      "inputtext", "itunes", "nowplaying", "perfmon", "ping", "power", "quote", "resmon",
      "runcommand", "speedfan", "usagemonitor", "webparser", "win7audio", "windowmessage",
      "hwinfo", "mediakey", "process", "recyclemanager", "sysinfo", "topprocesses", "wifistatus"};
  static const std::set<std::string> color_plugins = {"chameleon", "syscolor"};
  return known.count(name) != 0 || color_plugins.count(name) != 0;
}

struct CommandResult {
  int status = -1;
  bool timed_out = false;
  std::string output;
};

CommandResult RunCapture(const std::vector<std::string>& arguments, int timeout_ms = 10000) {
  CommandResult result;
  if (arguments.empty()) return result;
  int descriptors[2];
  if (pipe(descriptors) != 0) return result;
  const pid_t child = fork();
  if (child == 0) {
    close(descriptors[0]);
    dup2(descriptors[1], STDOUT_FILENO);
    dup2(descriptors[1], STDERR_FILENO);
    close(descriptors[1]);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(descriptors[1]);
  if (child < 0) {
    close(descriptors[0]);
    return result;
  }
  fcntl(descriptors[0], F_SETFL, fcntl(descriptors[0], F_GETFL) | O_NONBLOCK);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool exited = false;
  int wait_status = 0;
  char buffer[4096];
  while (!exited || std::chrono::steady_clock::now() < deadline) {
    pollfd event{descriptors[0], POLLIN | POLLHUP, 0};
    poll(&event, 1, 50);
    for (;;) {
      const ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
      if (count <= 0) break;
      if (result.output.size() < 8U * 1024U * 1024U) {
        result.output.append(buffer, std::min<size_t>(static_cast<size_t>(count),
            8U * 1024U * 1024U - result.output.size()));
      }
    }
    exited = waitpid(child, &wait_status, WNOHANG) == child;
    if (exited && (event.revents & POLLHUP)) break;
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(child, SIGKILL);
      waitpid(child, &wait_status, 0);
      result.timed_out = true;
      exited = true;
      break;
    }
  }
  close(descriptors[0]);
  if (!result.timed_out && WIFEXITED(wait_status)) result.status = WEXITSTATUS(wait_status);
  return result;
}

std::vector<std::string> RegexCaptures(std::string pattern, const std::string& subject,
                                       std::string& error) {
  if (pattern.size() >= 2 && pattern.front() == '"' && pattern.back() == '"') {
    pattern = pattern.substr(1, pattern.size() - 2);
  }
  int code = 0;
  PCRE2_SIZE offset = 0;
  pcre2_code* expression = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.c_str()),
      PCRE2_ZERO_TERMINATED, PCRE2_UTF, &code, &offset, nullptr);
  if (!expression) {
    PCRE2_UCHAR message[256]{};
    pcre2_get_error_message(code, message, sizeof(message));
    error = "regular expression error at " + std::to_string(offset) + ": " +
            reinterpret_cast<const char*>(message);
    return {};
  }
  pcre2_match_data* matches = pcre2_match_data_create_from_pattern(expression, nullptr);
  const int count = pcre2_match(expression, reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(),
                                0, 0, matches, nullptr);
  std::vector<std::string> result;
  if (count > 0) {
    PCRE2_SIZE* positions = pcre2_get_ovector_pointer(matches);
    result.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
      const auto begin = positions[index * 2];
      const auto end = positions[index * 2 + 1];
      result.push_back(begin == PCRE2_UNSET ? std::string{} : subject.substr(begin, end - begin));
    }
  } else if (count < PCRE2_ERROR_NOMATCH) {
    error = "regular expression match failed with code " + std::to_string(count);
  }
  pcre2_match_data_free(matches);
  pcre2_code_free(expression);
  return result;
}

double ToNumber(const std::string& value) {
  char* end = nullptr;
  const double result = std::strtod(value.c_str(), &end);
  return end == value.c_str() ? 0.0 : result;
}

std::optional<std::string> ReadText(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) return std::nullopt;
  std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return Trim(value);
}

std::vector<std::filesystem::path> GlobFiles(const std::filesystem::path& root,
                                             const std::string& prefix,
                                             const std::string& suffix) {
  std::vector<std::filesystem::path> result;
  if (!std::filesystem::is_directory(root)) return result;
  std::error_code error;
  for (const auto& directory : std::filesystem::directory_iterator(root, error)) {
    if (error || !directory.is_directory()) continue;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path(), error)) {
      if (error || !entry.is_regular_file()) continue;
      const auto name = entry.path().filename().string();
      if (name.rfind(prefix, 0) == 0 && name.size() >= suffix.size() &&
          name.substr(name.size() - suffix.size()) == suffix) result.push_back(entry.path());
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::string Unquote(std::string value) {
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\'')) &&
      value.find(value.front(), 1) == value.size() - 1) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

std::string Json(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
              << std::dec << std::setfill(' ');
        } else {
          out << ch;
        }
    }
  }
  out << '"';
  return out.str();
}

std::string ReadUtf8(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Unable to read " + path.string());
  std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xef &&
      static_cast<uint8_t>(bytes[1]) == 0xbb && static_cast<uint8_t>(bytes[2]) == 0xbf) {
    return bytes.substr(3);
  }
  if (bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xff &&
      static_cast<uint8_t>(bytes[1]) == 0xfe) {
    std::u16string text;
    for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
      text.push_back(static_cast<char16_t>(static_cast<uint8_t>(bytes[i]) |
                                           (static_cast<uint8_t>(bytes[i + 1]) << 8)));
    }
    return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(text);
  }
  if (!g_utf8_validate(bytes.data(), static_cast<gssize>(bytes.size()), nullptr)) {
    gsize written = 0;
    GError* conversion_error = nullptr;
    char* converted = g_convert(bytes.data(), static_cast<gssize>(bytes.size()), "UTF-8", "WINDOWS-1252",
                                nullptr, &written, &conversion_error);
    if (!converted) {
      const std::string message = conversion_error ? conversion_error->message : "unknown conversion error";
      if (conversion_error) g_error_free(conversion_error);
      throw std::runtime_error("Unable to decode legacy skin " + path.string() + ": " + message);
    }
    std::string result(converted, written);
    g_free(converted);
    return result;
  }
  return bytes;
}

std::filesystem::path ResolveCaseInsensitive(std::filesystem::path path) {
  auto portable = path.string();
  std::replace(portable.begin(), portable.end(), '\\', '/');
  path = std::filesystem::absolute(portable);
  if (std::filesystem::exists(path)) return path;

  std::filesystem::path resolved = path.is_absolute() ? path.root_path() : std::filesystem::current_path();
  const auto relative = path.is_absolute() ? path.relative_path() : path;
  for (const auto& component : relative) {
    if (component == ".") continue;
    if (component == "..") {
      resolved = resolved.parent_path();
      continue;
    }
    const auto wanted = Lower(component.string());
    std::optional<std::filesystem::path> match;
    if (!std::filesystem::is_directory(resolved)) {
      throw std::runtime_error("Path component is not a directory: " + resolved.string());
    }
    for (const auto& entry : std::filesystem::directory_iterator(resolved)) {
      if (Lower(entry.path().filename().string()) == wanted) {
        if (match && entry.path().filename() != match->filename()) {
          throw std::runtime_error("Ambiguous case-insensitive path component: " + component.string());
        }
        match = entry.path();
      }
    }
    if (!match) throw std::runtime_error("Path component not found: " + component.string());
    resolved = *match;
  }
  return resolved;
}

struct Section {
  std::string name;
  std::map<std::string, std::string> options;
};

class Config {
 public:
  explicit Config(std::filesystem::path skin) : skin_(ResolveCaseInsensitive(std::move(skin))) {
    const auto resources = FindResources();
    builtins_["@"] = resources.string() + "/";
    builtins_["currentpath"] = skin_.parent_path().string() + "/";
    builtins_["currentfile"] = skin_.filename().string();
    const auto skins = FindSkinsPath();
    builtins_["skinspath"] = skins.string() + "/";
    auto relative = skin_.parent_path().lexically_relative(skins);
    const auto root = relative.empty() ? std::filesystem::path{} : *relative.begin();
    builtins_["rootconfig"] = root.string();
    builtins_["rootconfigpath"] = (skins / root).string() + "/";
    builtins_["currentconfig"] = relative.string();
    builtins_["currentconfigx"] = "0";
    builtins_["currentconfigy"] = "0";
    builtins_["currentconfigwidth"] = "0";
    builtins_["currentconfigheight"] = "0";
    builtins_["workareawidth"] = std::getenv("RM_WORKAREAWIDTH") ? std::getenv("RM_WORKAREAWIDTH") : "1920";
    builtins_["workareaheight"] = std::getenv("RM_WORKAREAHEIGHT") ? std::getenv("RM_WORKAREAHEIGHT") : "1080";
    for (const auto& fonts : {skins.parent_path() / "Fonts", resources / "Fonts"}) {
      if (std::filesystem::is_directory(fonts)) {
        for (const auto& entry : std::filesystem::directory_iterator(fonts)) {
          const auto extension = Lower(entry.path().extension().string());
          if (entry.is_regular_file() && (extension == ".ttf" || extension == ".otf")) {
            FcConfigAppFontAddFile(nullptr, reinterpret_cast<const FcChar8*>(entry.path().c_str()));
          }
        }
        FcConfigBuildFonts(nullptr);
      }
    }
    ParseFile(skin_, 0);
  }

  const std::vector<Section>& Sections() const { return sections_; }

  std::string Get(const std::string& section, const std::string& option,
                  const std::string& fallback = {}) const {
    const auto found = section_index_.find(Lower(section));
    if (found == section_index_.end()) return fallback;
    const auto value = sections_[found->second].options.find(Lower(option));
    if (value != sections_[found->second].options.end()) return Expand(value->second);
    if (Lower(option) != "meterstyle") {
      const auto style_value = sections_[found->second].options.find("meterstyle");
      if (style_value != sections_[found->second].options.end()) {
        std::vector<std::string> styles;
        std::istringstream names(Expand(style_value->second));
        std::string style;
        while (std::getline(names, style, '|')) styles.push_back(Trim(style));
        for (auto current = styles.rbegin(); current != styles.rend(); ++current) {
          const auto style_section = section_index_.find(Lower(*current));
          if (style_section == section_index_.end()) continue;
          const auto styled = sections_[style_section->second].options.find(Lower(option));
          if (styled != sections_[style_section->second].options.end()) return Expand(styled->second);
        }
      }
    }
    return fallback;
  }

  void Set(const std::string& section, const std::string& option, const std::string& value) {
    const auto found = section_index_.find(Lower(section));
    if (found == section_index_.end()) throw std::runtime_error("Unknown section: " + section);
    sections_[found->second].options[Lower(option)] = value;
  }

  void SetVariable(const std::string& name, const std::string& value) {
    variables_[Lower(name)] = Expand(value);
  }

  void SetBuiltin(const std::string& name, const std::string& value) {
    builtins_[Lower(name)] = value;
  }

  std::filesystem::path ResolvePath(std::string value) const {
    value = Expand(std::move(value));
    std::replace(value.begin(), value.end(), '\\', '/');
    std::filesystem::path path(value);
    if (!path.is_absolute()) path = skin_.parent_path() / path;
    return ResolveCaseInsensitive(path);
  }

  std::filesystem::path ResolveImagePath(const std::string& value) const {
    try {
      return ResolvePath(value);
    } catch (const std::exception&) {
      if (!std::filesystem::path(value).extension().empty()) throw;
      for (const char* extension : {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp"}) {
        try { return ResolvePath(value + extension); } catch (const std::exception&) {}
      }
      throw;
    }
  }

  std::string Expand(std::string value) const {
    size_t begin = 0;
    size_t replacements = 0;
    while ((begin = value.find('#', begin)) != std::string::npos) {
      const auto end = value.find('#', begin + 1);
      if (end == std::string::npos) break;
      if (++replacements >= 1000) {
        throw std::runtime_error("Maximum variable replacements reached");
      }
      if (end > begin + 2 && value[begin + 1] == '*' && value[end - 1] == '*') {
        value.erase(end - 1, 1);
        value.erase(begin + 1, 1);
        begin = end - 1;
        continue;
      }
      const auto key = Lower(value.substr(begin + 1, end - begin - 1));
      auto builtin = builtins_.find(key);
      auto variable = variables_.find(key);
      if (builtin == builtins_.end() && variable == variables_.end()) {
        begin = end;
        continue;
      }
      const auto& replacement = builtin != builtins_.end() ? builtin->second : variable->second;
      value.replace(begin, end - begin + 1, replacement);
      begin += replacement.size();
    }
    static const std::regex environment(R"(%([A-Za-z_][A-Za-z0-9_]*)%)");
    std::smatch match;
    size_t offset = 0;
    while (std::regex_search(value.cbegin() + static_cast<std::ptrdiff_t>(offset), value.cend(),
                             match, environment)) {
      const auto name = match[1].str();
      std::string replacement;
      if (Lower(name) == "number_of_processors") {
        replacement = std::to_string(std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN)));
      } else if (const char* item = std::getenv(name.c_str())) {
        replacement = item;
      } else {
        offset += static_cast<size_t>(match.position() + match.length());
        continue;
      }
      const size_t position = offset + static_cast<size_t>(match.position());
      value.replace(position, static_cast<size_t>(match.length()), replacement);
      offset = position + replacement.size();
    }
    return value;
  }

 private:
  std::filesystem::path FindResources() const {
    for (auto directory = skin_.parent_path(); !directory.empty(); directory = directory.parent_path()) {
      for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_directory() && Lower(entry.path().filename().string()) == "@resources") {
          return entry.path();
        }
      }
      if (directory == directory.root_path()) break;
    }
    return skin_.parent_path() / "@Resources";
  }

  std::filesystem::path FindSkinsPath() const {
    for (auto directory = skin_.parent_path(); !directory.empty(); directory = directory.parent_path()) {
      if (Lower(directory.filename().string()) == "skins") return directory;
      if (directory == directory.root_path()) break;
    }
    return skin_.parent_path();
  }

  size_t EnsureSection(const std::string& name) {
    const auto key = Lower(name);
    const auto found = section_index_.find(key);
    if (found != section_index_.end()) return found->second;
    const size_t index = sections_.size();
    section_index_[key] = index;
    sections_.push_back({name, {}});
    return index;
  }

  void ParseFile(const std::filesystem::path& unresolved, int depth) {
    if (depth > 100) throw std::runtime_error("Maximum include depth reached");
    const auto path = ResolveCaseInsensitive(unresolved);
    std::istringstream input(ReadUtf8(path));
    std::string line;
    size_t current = EnsureSection("");
    std::map<std::string, std::set<std::string>> seen;
    while (std::getline(input, line)) {
      line = Trim(line);
      if (line.empty() || line[0] == ';') continue;
      if (line.front() == '[' && line.back() == ']') {
        current = EnsureSection(Trim(line.substr(1, line.size() - 2)));
        continue;
      }
      const auto equals = line.find('=');
      if (equals == std::string::npos || equals == 0) continue;
      const auto key_original = Trim(line.substr(0, equals));
      const auto key = Lower(key_original);
      auto value = Trim(line.substr(equals + 1));
      const auto section_key = Lower(sections_[current].name);
      if (!seen[section_key].insert(key).second) continue;
      if (key.rfind("@include", 0) == 0) {
        value = Unquote(Expand(value));
        std::replace(value.begin(), value.end(), '\\', '/');
        std::filesystem::path include(value);
        if (!include.is_absolute()) include = path.parent_path() / include;
        ParseFile(include, depth + 1);
        continue;
      }
      value = Unquote(std::move(value));
      sections_[current].options[key] = value;
      if (section_key == "variables") variables_[key] = Expand(value);
    }
  }

  std::filesystem::path skin_;
  std::vector<Section> sections_;
  std::unordered_map<std::string, size_t> section_index_;
  std::unordered_map<std::string, std::string> variables_;
  std::unordered_map<std::string, std::string> builtins_;
};

double Formula(const std::string& text) {
  const auto wide = std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(text);
  double result = 0.0;
  if (const wchar_t* error = MathParser::CheckedParse(wide.c_str(), &result)) {
    throw std::runtime_error("Formula error: " +
                             std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(error));
  }
  return result;
}

int Integer(const std::string& value, int fallback = 0) {
  if (value.empty()) return fallback;
  return static_cast<int>(Formula(value));
}

std::vector<int> Integers(std::string value) {
  std::replace(value.begin(), value.end(), ';', ',');
  std::istringstream input(value);
  std::string part;
  std::vector<int> result;
  while (std::getline(input, part, ',')) result.push_back(Integer(Trim(part)));
  return result;
}

std::string Number(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(5) << value;
  auto result = out.str();
  while (!result.empty() && result.back() == '0') result.pop_back();
  if (!result.empty() && result.back() == '.') result.pop_back();
  return result.empty() ? "0" : result;
}

std::string Substitute(std::string value, const std::string& substitutions) {
  std::string normalized = Trim(substitutions);
  if (!normalized.empty() && normalized.front() != '"' && normalized.find("\":\"") != std::string::npos) {
    normalized.insert(normalized.begin(), '"');
  }
  static const std::regex pair(R"rm("([^"]*)"\s*:\s*"([^"]*)")rm");
  for (auto match = std::sregex_iterator(normalized.begin(), normalized.end(), pair);
       match != std::sregex_iterator(); ++match) {
    const auto from = (*match)[1].str();
    const auto to = (*match)[2].str();
    if (from.empty()) continue;
    for (size_t position = 0; (position = value.find(from, position)) != std::string::npos;) {
      value.replace(position, from.size(), to);
      position += to.size();
    }
  }
  return value;
}

struct Color {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  double a = 0.0;
};

Color ParseColor(std::string value, Color fallback) {
  if (value.empty()) return fallback;
  std::replace(value.begin(), value.end(), ';', ',');
  std::istringstream input(value);
  std::string part;
  std::vector<double> components;
  try {
    while (std::getline(input, part, ',')) components.push_back(Formula(Trim(part)) / 255.0);
  } catch (const std::exception&) {
    return fallback;
  }
  if (components.size() < 3 || components.size() > 4) return fallback;
  return {components[0], components[1], components[2], components.size() == 4 ? components[3] : 1.0};
}

cairo_surface_t* LoadImage(const std::filesystem::path& path, std::string& error,
                           Color tint = {1.0, 1.0, 1.0, 1.0}, double opacity = 1.0) {
  GError* load_error = nullptr;
  GdkPixbuf* image = gdk_pixbuf_new_from_file(path.c_str(), &load_error);
  if (!image) {
    error = load_error ? load_error->message : "image decoder failed";
    if (load_error) g_error_free(load_error);
    return nullptr;
  }
  const int width = gdk_pixbuf_get_width(image);
  const int height = gdk_pixbuf_get_height(image);
  const int channels = gdk_pixbuf_get_n_channels(image);
  const int source_stride = gdk_pixbuf_get_rowstride(image);
  const bool alpha = gdk_pixbuf_get_has_alpha(image);
  const auto* source = gdk_pixbuf_read_pixels(image);
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    error = cairo_status_to_string(cairo_surface_status(surface));
    cairo_surface_destroy(surface);
    g_object_unref(image);
    return nullptr;
  }
  auto* destination = cairo_image_surface_get_data(surface);
  const int destination_stride = cairo_image_surface_get_stride(surface);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto* pixel = source + y * source_stride + x * channels;
      const uint32_t source_alpha = alpha ? pixel[3] : 255U;
      const uint32_t a = static_cast<uint32_t>(std::clamp(
          source_alpha * tint.a * opacity, 0.0, 255.0));
      const uint32_t tinted_r = static_cast<uint32_t>(std::clamp(pixel[0] * tint.r, 0.0, 255.0));
      const uint32_t tinted_g = static_cast<uint32_t>(std::clamp(pixel[1] * tint.g, 0.0, 255.0));
      const uint32_t tinted_b = static_cast<uint32_t>(std::clamp(pixel[2] * tint.b, 0.0, 255.0));
      const uint32_t r = (tinted_r * a + 127U) / 255U;
      const uint32_t g = (tinted_g * a + 127U) / 255U;
      const uint32_t b = (tinted_b * a + 127U) / 255U;
      const uint32_t argb = (a << 24U) | (r << 16U) | (g << 8U) | b;
      std::memcpy(destination + y * destination_stride + x * 4, &argb, sizeof(argb));
    }
  }
  cairo_surface_mark_dirty(surface);
  g_object_unref(image);
  return surface;
}

void PaintImageSlice(cairo_t* cr, cairo_surface_t* image,
                     double source_x, double source_y, double source_width, double source_height,
                     double destination_x, double destination_y,
                     double destination_width, double destination_height) {
  if (source_width <= 0 || source_height <= 0 || destination_width <= 0 || destination_height <= 0) {
    return;
  }
  cairo_save(cr);
  cairo_rectangle(cr, destination_x, destination_y, destination_width, destination_height);
  cairo_clip(cr);
  cairo_translate(cr, destination_x, destination_y);
  cairo_scale(cr, destination_width / source_width, destination_height / source_height);
  cairo_set_source_surface(cr, image, -source_x, -source_y);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_restore(cr);
}

std::vector<std::string> SplitCommands(const std::string& action) {
  std::vector<std::string> result;
  for (size_t start = 0; start < action.size();) {
    start = action.find('[', start);
    if (start == std::string::npos) break;
    int depth = 1;
    size_t end = start + 1;
    for (; end < action.size() && depth; ++end) {
      if (action[end] == '[') ++depth;
      if (action[end] == ']') --depth;
    }
    if (depth) throw std::runtime_error("Unterminated action command near: " + action.substr(start, 512));
    result.push_back(action.substr(start + 1, end - start - 2));
    start = end;
  }
  return result;
}

std::vector<std::string> SplitTopLevel(const std::string& value, char delimiter) {
  std::vector<std::string> result;
  size_t start = 0;
  int parentheses = 0;
  bool quoted = false;
  for (size_t index = 0; index <= value.size(); ++index) {
    const char character = index < value.size() ? value[index] : delimiter;
    if (character == '"') quoted = !quoted;
    if (!quoted && character == '(') ++parentheses;
    if (!quoted && character == ')') --parentheses;
    if (character == delimiter && !quoted && parentheses == 0) {
      result.push_back(Trim(value.substr(start, index - start)));
      start = index + 1;
    }
  }
  return result;
}

std::pair<double, double> GradientEdgePoint(double angle, const Rect& rect) {
  angle = std::fmod(angle, 360.0);
  if (angle < 0.0) angle += 360.0;
  const double radians = angle * 3.14159265358979323846 / 180.0;
  const double rectangle_angle = std::atan2(rect.height, rect.width);
  const int quadrant = static_cast<int>(std::fmod(angle / 90.0, 4.0)) + 1;
  const double axis_angle = quadrant == 1 ? radians :
      quadrant == 2 ? 3.14159265358979323846 - radians :
      quadrant == 3 ? radians - 3.14159265358979323846 :
      2.0 * 3.14159265358979323846 - radians;
  const double half_diagonal = std::hypot(rect.width, rect.height) / 2.0;
  const double distance = half_diagonal * std::cos(std::abs(axis_angle - rectangle_angle));
  return {rect.x + rect.width / 2.0 + distance * std::cos(radians),
          rect.y + rect.height / 2.0 + distance * std::sin(radians)};
}

std::vector<std::string> Tokens(const std::string& command) {
  std::vector<std::string> result;
  std::string token;
  bool quoted = false;
  bool triple_quoted = false;
  bool token_started = false;
  for (size_t index = 0; index < command.size(); ++index) {
    const char ch = command[index];
    if (command.compare(index, 3, "\"\"\"") == 0) {
      if (!triple_quoted) {
        triple_quoted = true;
      } else if (index + 3 == command.size() ||
                 std::isspace(static_cast<unsigned char>(command[index + 3]))) {
        triple_quoted = false;
      } else {
        token += "\"\"\"";
      }
      token_started = true;
      index += 2;
      continue;
    }
    if (triple_quoted) {
      token += ch;
      token_started = true;
      continue;
    }
    if (ch == '"') {
      quoted = !quoted;
      token_started = true;
    } else if (std::isspace(static_cast<unsigned char>(ch)) && !quoted) {
      if (token_started) {
        result.push_back(token);
        token.clear();
        token_started = false;
      }
    } else {
      token += ch;
      token_started = true;
    }
  }
  if (token_started) result.push_back(token);
  return result;
}

}

struct Runtime::Impl {
  struct Measure {
    std::string name;
    std::string type;
    std::string formula;
    double minimum = 0.0;
    double maximum = 1.0;
    double value = 0.0;
    std::string string_value = "0";
    int processor = 0;
    bool total = false;
    bool invert = false;
    std::string drive;
    std::filesystem::path source_path;
    std::string plugin;
    std::string group;
    bool disabled = false;
    std::vector<std::string> captures;
    std::vector<std::filesystem::path> file_entries;
    std::unordered_map<std::string, uint64_t> previous_process_cpu;
    bool plugin_string = false;
    bool capability_reported = false;
    lua_State* lua = nullptr;
    bool lua_attempted = false;
    uint64_t previous_total = 0;
    uint64_t previous_idle = 0;
    uint64_t previous_counter = 0;
    uint64_t previous_counter2 = 0;
    bool dynamic = false;
    int64_t loop_start = 1;
    int64_t loop_end = 100;
    int64_t loop_increment = 1;
    int64_t loop_count = 0;
    int64_t loops_completed = 0;
    bool loop_started = false;
    bool loop_at_end = false;
    std::unordered_map<int, bool> condition_states;
  };
  struct Meter {
    std::string name;
    std::string type;
    std::string measure;
    std::string group;
    std::vector<std::string> measure_names;
    Rect rect;
    Color bar_color{0.0, 1.0, 0.0, 1.0};
    Color solid_color{};
    Color solid_color2{};
    bool solid_color2_defined = false;
    double gradient_angle = 0.0;
    std::array<double, 6> transformation{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    bool transformed = false;
    bool horizontal = false;
    std::string left_up_action;
    double normalized = 0.0;
    std::string text;
    std::string font_face = "Arial";
    double font_size = 10.0;
    Color font_color{0.0, 0.0, 0.0, 1.0};
    Color effect_color{0.0, 0.0, 0.0, 1.0};
    std::string align = "left";
    std::string string_case = "none";
    std::string string_style = "normal";
    std::string string_effect = "none";
    bool clip = false;
    bool autoscale = false;
    bool percentual = false;
    int decimals = -1;
    double scale = 1.0;
    std::string prefix;
    std::string postfix;
    bool hidden = false;
    bool width_defined = false;
    bool height_defined = false;
    int anchor_x = 0;
    int anchor_y = 0;
    cairo_surface_t* image = nullptr;
    std::string image_name;
    Color image_tint{1.0, 1.0, 1.0, 1.0};
    double image_opacity = 1.0;
    double image_source_width = 0.0;
    double image_source_height = 0.0;
    int bitmap_frames = 1;
    bool bitmap_zero_frame = false;
    std::string shape;
    Rect shape_rect;
    enum class ShapeKind { Rectangle, Ellipse } shape_kind = ShapeKind::Rectangle;
    Color shape_fill{1.0, 1.0, 1.0, 1.0};
    Color shape_stroke{};
    double shape_stroke_width = 1.0;
    double shape_corner_radius = 0.0;
    std::vector<double> shape_dashes;
    cairo_line_cap_t shape_line_cap = CAIRO_LINE_CAP_BUTT;
    bool shape_radial_gradient = false;
    std::vector<std::pair<double, Color>> shape_gradient;
    std::vector<double> shape_gradient_geometry;
    double shape_scale_x = 1.0;
    double shape_scale_y = 1.0;
    double shape_pivot_x = 0.0;
    double shape_pivot_y = 0.0;
    double shape_rotation = 0.0;
    Color line_color{0.0, 0.0, 0.0, 1.0};
    double line_width = 1.0;
    double line_start = -1.0;
    double line_length = 20.0;
    double start_angle = 0.0;
    double rotation_angle = 6.2832;
    int value_remainder = 0;
    bool solid = false;
    double offset_x = 0.0;
    double offset_y = 0.0;
    bool graph_autoscale = false;
    bool graph_flip = false;
    double graph_value = 0.0;
    bool graph_value_ready = false;
    bool dynamic = false;
    std::deque<double> history;
  };
  struct Event {
    size_t sequence;
    std::string type;
    std::string detail;
  };
  struct ScheduledAction {
    std::chrono::steady_clock::time_point due;
    std::string action;
  };

  explicit Impl(const std::filesystem::path& path) : config(path) {}

  Config config;
  std::vector<Measure> measures;
  std::vector<Meter> meters;
  std::unordered_map<std::string, std::string> web_cycle_cache;
  std::unordered_map<std::string, std::string> web_cycle_errors;
  std::vector<Event> events;
  std::vector<ScheduledAction> scheduled_actions;
  std::vector<HostAction> host_actions;
  std::string initial_snapshot;
  std::string post_snapshot;
  size_t sequence = 0;
  uint64_t update_counter = 1;
  int update_interval = 1000;
  double frame_rate = 0.0;
  double scroll_speed = 0.0;
  double smoothing_ms = 0.0;
  double scroll_remainder = 0.0;
  std::chrono::steady_clock::time_point scroll_updated_at;
  int default_update_divider = 1;
  std::string on_refresh_action;
  std::string on_update_action;
  std::string right_mouse_up_action;
  std::string left_mouse_double_click_action;
  std::filesystem::path background_path;
  int background_mode = 0;
  Rect background_margins;
  cairo_surface_t* background = nullptr;
  pid_t audio_pid = -1;
  int audio_fd = -1;
  std::string audio_device;
  std::vector<char> audio_bytes;
  bool audio_capture_ready = false;
  std::string audio_analysis_parent;
  double audio_rms = 0.0;
  double audio_peak = 0.0;
  std::vector<double> audio_bands;
  struct GpuInfo {
    std::string name;
    double temperature = 0.0;
    double utilization = 0.0;
    double memory_utilization = 0.0;
    double memory_used = 0.0;
    double memory_total = 0.0;
    double clock = 0.0;
    double power = 0.0;
    double fan = 0.0;
  };
  uint64_t gpu_info_cycle = 0;
  std::vector<GpuInfo> gpu_info;

  ~Impl() {
    if (background) cairo_surface_destroy(background);
    for (auto& meter : meters) if (meter.image) cairo_surface_destroy(meter.image);
    for (auto& measure : measures) if (measure.lua) lua_close(measure.lua);
    if (audio_fd >= 0) close(audio_fd);
    if (audio_pid > 0) {
      kill(audio_pid, SIGTERM);
      for (int attempt = 0; attempt < 50; ++attempt) {
        if (waitpid(audio_pid, nullptr, WNOHANG) == audio_pid) return;
        usleep(10000);
      }
      kill(audio_pid, SIGKILL);
      waitpid(audio_pid, nullptr, 0);
    }
  }

  void EventLog(std::string type, std::string detail) {
    events.push_back({++sequence, std::move(type), std::move(detail)});
  }

  Measure* FindMeasure(const std::string& name) {
    const auto key = Lower(name);
    for (auto& measure : measures) if (Lower(measure.name) == key) return &measure;
    return nullptr;
  }
  const Measure* FindMeasure(const std::string& name) const {
    return const_cast<Impl*>(this)->FindMeasure(name);
  }
  Meter* FindMeter(const std::string& name) {
    const auto key = Lower(name);
    for (auto& meter : meters) if (Lower(meter.name) == key) return &meter;
    return nullptr;
  }
  const Meter* FindMeter(const std::string& name) const {
    return const_cast<Impl*>(this)->FindMeter(name);
  }

  std::string ExpandMeasures(std::string value) const {
    for (const auto& measure : measures) {
      const std::string marker = "[" + measure.name + "]";
      for (size_t pos = 0; (pos = Lower(value).find(Lower(marker), pos)) != std::string::npos;) {
        value.replace(pos, marker.size(), measure.string_value);
        pos += measure.string_value.size();
      }
      const std::string dynamic_marker = "[&" + measure.name + "]";
      for (size_t pos = 0; (pos = Lower(value).find(Lower(dynamic_marker), pos)) != std::string::npos;) {
        value.replace(pos, dynamic_marker.size(), measure.string_value);
        pos += measure.string_value.size();
      }
    }
    static const std::regex section_reference(R"(\[([^\]:]+):([^\]]*)\])");
    std::smatch match;
    while (std::regex_search(value, match, section_reference)) {
      const auto section = match[1].str();
      const auto property = Lower(match[2].str());
      std::string replacement;
      if (const auto* measure = FindMeasure(section)) {
        if (property.empty()) {
          replacement = measure->string_value;
        } else if (property == "minvalue") {
          replacement = Number(measure->minimum);
        } else if (property == "maxvalue") {
          replacement = Number(measure->maximum);
        } else if (property == "%") {
          const double range = measure->maximum - measure->minimum;
          replacement = Number(range == 0.0 ? 0.0 :
              (measure->value - measure->minimum) * 100.0 / range);
        } else if (!property.empty() && property.front() == '/') {
          std::istringstream formatting(property.substr(1));
          std::string divisor_text, decimals_text;
          std::getline(formatting, divisor_text, ',');
          std::getline(formatting, decimals_text);
          const double divisor = ToNumber(Trim(divisor_text));
          const double formatted = divisor == 0.0 ? measure->value : measure->value / divisor;
          if (!Trim(decimals_text).empty()) {
            std::ostringstream output;
            output << std::fixed << std::setprecision(std::max(0, Integer(Trim(decimals_text)))) << formatted;
            replacement = output.str();
          } else {
            replacement = Number(formatted);
          }
        } else {
          replacement = Number(measure->value);
        }
      } else if (const auto* meter = FindMeter(section)) {
        replacement = property == "x" ? std::to_string(meter->rect.x) :
            property == "y" ? std::to_string(meter->rect.y) :
            property == "w" ? std::to_string(meter->rect.width) :
            property == "h" ? std::to_string(meter->rect.height) : "0";
      } else {
        replacement = "0";
      }
      value.replace(static_cast<size_t>(match.position()), static_cast<size_t>(match.length()), replacement);
    }
    return value;
  }

  double ReadNumber(const std::string& section, const std::string& option,
                    double fallback) {
    const auto value = ExpandMeasures(config.Get(section, option));
    if (value.empty()) return fallback;
    if (value.front() == '(') {
      try {
        return Formula(value);
      } catch (const std::exception& error) {
        EventLog("diagnostic", "Formula error in key \"" + option + "\" in [" +
            section + "]: " + error.what());
        return fallback;
      }
    }

    errno = 0;
    const double result = std::strtod(value.c_str(), nullptr);
    return errno == ERANGE ? fallback : result;
  }

  int ReadInteger(const std::string& section, const std::string& option, int fallback) {
    return static_cast<int>(ReadNumber(section, option, fallback));
  }

  int CurrentWidth() const {
    int width = std::max(0, background_margins.x);
    for (const auto& meter : meters) width = std::max(width, meter.rect.x + meter.rect.width);
    return width + background_margins.width;
  }

  int CurrentHeight() const {
    int height = std::max(0, background_margins.y);
    for (const auto& meter : meters) height = std::max(height, meter.rect.y + meter.rect.height);
    return height + background_margins.height;
  }

  void UpdateSizeBuiltins() {
    config.SetBuiltin("CURRENTCONFIGWIDTH", std::to_string(CurrentWidth()));
    config.SetBuiltin("CURRENTCONFIGHEIGHT", std::to_string(CurrentHeight()));
  }

  void Load() {
    const int skin_width = ReadInteger("Rainmeter", "SkinWidth", 0);
    const int skin_height = ReadInteger("Rainmeter", "SkinHeight", 0);
    config.SetBuiltin("CURRENTCONFIGWIDTH", std::to_string(skin_width));
    config.SetBuiltin("CURRENTCONFIGHEIGHT", std::to_string(skin_height));
    update_interval = ReadInteger("Rainmeter", "Update", 1000);
    if (!config.Get("Rainmeter", "Framerate").empty()) {
      frame_rate = std::clamp(ReadNumber("Rainmeter", "Framerate", 60.0), 1.0, 1000.0);
    }
    if (!config.Get("Rainmeter", "Speed").empty()) {
      scroll_speed = std::clamp(ReadNumber("Rainmeter", "Speed", 60.0), 1.0, 1000.0);
    }
    if (!config.Get("Rainmeter", "Smoothness").empty()) {
      smoothing_ms = std::clamp(ReadNumber("Rainmeter", "Smoothness", 0.0), 0.0, 1000.0);
    }
    default_update_divider = ReadInteger("Rainmeter", "DefaultUpdateDivider", 1);
    on_refresh_action = config.Get("Rainmeter", "OnRefreshAction");
    on_update_action = config.Get("Rainmeter", "OnUpdateAction");
    right_mouse_up_action = config.Get("Rainmeter", "RightMouseUpAction");
    left_mouse_double_click_action = config.Get("Rainmeter", "LeftMouseDoubleClickAction");
    background_mode = ReadInteger("Rainmeter", "BackgroundMode", 0);
    const auto margins = Integers(config.Get("Rainmeter", "BackgroundMargins", "0,0,0,0"));
    if (margins.size() == 4) background_margins = {margins[0], margins[1], margins[2], margins[3]};
    const auto background_name = config.Get("Rainmeter", "Background");
    if (!background_name.empty() &&
        (background_mode == 0 || background_mode == 3 || background_mode == 4)) {
      background_path = config.ResolveImagePath(background_name);
      std::string image_error;
      background = LoadImage(background_path, image_error);
      if (!background) EventLog("diagnostic", "unable to load background: " + image_error);
    }
    for (const auto& section : config.Sections()) {
      const auto type = config.Get(section.name, "Measure");
      if (Lower(type) == "calc") {
        Measure measure;
        measure.name = section.name;
        measure.type = type;
        measure.formula = config.Get(section.name, "Formula", "0");
        measure.minimum = ReadNumber(section.name, "MinValue", 0.0);
        measure.maximum = ReadNumber(section.name, "MaxValue", 1.0);
        measures.push_back(std::move(measure));
      } else if (Lower(type) == "time") {
        Measure measure;
        measure.name = section.name;
        measure.type = type;
        measure.formula = config.Get(section.name, "Format", "%H:%M");
        measures.push_back(std::move(measure));
      } else if (Lower(type) == "string") {
        Measure measure;
        measure.name = section.name;
        measure.type = type;
        measure.formula = config.Get(section.name, "String");
        measure.string_value = config.Expand(measure.formula);
        measure.value = ToNumber(measure.string_value);
        measures.push_back(std::move(measure));
      } else if (Lower(type) == "script") {
        Measure measure;
        measure.name = section.name;
        measure.type = type;
        measure.formula = config.Get(section.name, "ScriptFile");
        measures.push_back(std::move(measure));
      } else if (Lower(type) == "registry") {
        Measure measure;
        measure.name = section.name;
        measure.type = type;
        measures.push_back(std::move(measure));
      } else if (Lower(type) == "loop") {
        Measure measure;
        measure.name = section.name;
        measure.type = type;
        measure.loop_start = static_cast<int64_t>(ReadNumber(section.name, "StartValue", 1.0));
        measure.loop_end = static_cast<int64_t>(ReadNumber(section.name, "EndValue", 100.0));
        measure.loop_increment = static_cast<int64_t>(ReadNumber(section.name, "Increment", 1.0));
        measure.loop_count = std::max<int64_t>(0,
            static_cast<int64_t>(ReadNumber(section.name, "LoopCount", 0.0)));
        measure.minimum = static_cast<double>(std::min(measure.loop_start, measure.loop_end));
        measure.maximum = static_cast<double>(std::max(measure.loop_start, measure.loop_end));
        measures.push_back(std::move(measure));
      } else if (Lower(type) == "cpu" || Lower(type) == "physicalmemory" ||
                 Lower(type) == "swapmemory" || Lower(type) == "freediskspace" ||
                 Lower(type) == "netin" || Lower(type) == "netout" || Lower(type) == "uptime") {
        Measure measure;
        measure.name = section.name;
        measure.type = type;
        measure.processor = ReadInteger(section.name, "Processor", 0);
        measure.total = ReadInteger(section.name, "Total", 0) != 0;
        measure.invert = ReadInteger(section.name, "InvertMeasure", 0) != 0;
        measure.drive = config.Get(section.name, "Drive", "/");
        if (Lower(type) == "cpu") measure.maximum = 100.0;
        measures.push_back(std::move(measure));
      } else if (Lower(type) == "plugin") {
        Measure measure;
        measure.name = section.name;
        measure.type = "Plugin";
        measure.plugin = PluginName(config.Get(section.name, "Plugin"));
        if (measure.plugin == "quote") {
          const auto path_name = config.Get(section.name, "PathName");
          try {
            if (!path_name.empty()) measure.source_path = config.ResolvePath(path_name);
          } catch (const std::exception& exception) {
            EventLog("diagnostic", "unable to resolve QuotePlugin path in [" + section.name + "]: " +
                exception.what());
          }
        }
        if (!KnownPlugin(measure.plugin)) {
          EventLog("diagnostic", "third-party Windows plugin " +
              config.Get(section.name, "Plugin") + " has no native adapter in [" + section.name + "]");
          measure.capability_reported = true;
        }
        measures.push_back(std::move(measure));
      } else if (!type.empty()) {
        Measure measure;
        measure.name = section.name;
        measure.type = type;
        measure.string_value = "0";
        measures.push_back(std::move(measure));
        const auto plugin = config.Get(section.name, "Plugin");
        EventLog("diagnostic", "unsupported measure " + type +
            (plugin.empty() ? "" : " (" + plugin + ")") + " in [" + section.name + "]");
      }
    }
    for (auto& measure : measures) {
      measure.group = config.Get(measure.name, "Group");
      measure.disabled = ReadInteger(measure.name, "Disabled", 0) != 0;
      measure.dynamic = ReadInteger(measure.name, "DynamicVariables", 0) != 0;
      if (Lower(measure.type) != "loop") {
        const auto minimum = config.Get(measure.name, "MinValue");
        const auto maximum = config.Get(measure.name, "MaxValue");
        if (!minimum.empty()) measure.minimum = ReadNumber(measure.name, "MinValue", measure.minimum);
        if (!maximum.empty()) measure.maximum = ReadNumber(measure.name, "MaxValue", measure.maximum);
      }
    }
    for (const auto& section : config.Sections()) {
      const auto type = config.Get(section.name, "Meter");
      const auto meter_type = Lower(type);
      if (!meter_type.empty()) {
        Meter meter;
        meter.name = section.name;
        meter.type = type;
        meter.dynamic = ReadInteger(section.name, "DynamicVariables", 0) != 0;
        meter.group = config.Get(section.name, "Group");
        meter.measure = config.Get(section.name, "MeasureName");
        for (int index = 1; index <= 20; ++index) {
          const auto option = index == 1 ? "MeasureName" : "MeasureName" + std::to_string(index);
          const auto binding = config.Get(section.name, option);
          if (!binding.empty()) meter.measure_names.push_back(binding);
        }
        auto position = [&](const std::string& option, bool horizontal) {
          auto value = ExpandMeasures(config.Get(section.name, option, "0"));
          char relative = '\0';
          if (!value.empty() && (value.back() == 'r' || value.back() == 'R')) {
            relative = value.back();
            value.pop_back();
          }
          int result = 0;
          if (!value.empty()) {
            if (value.front() == '(') {
              try {
                result = static_cast<int>(Formula(value));
              } catch (const std::exception& error) {
                EventLog("diagnostic", "Formula error in key \"" + option + "\" in [" +
                    section.name + "]: " + error.what());
              }
            } else {
              result = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
            }
          }
          if (relative && !meters.empty()) {
            const auto& previous = meters.back().rect;
            result += horizontal ? meters.back().anchor_x : meters.back().anchor_y;
            if (relative == 'R') result += horizontal ? previous.width : previous.height;
          }
          return result;
        };
        meter.width_defined = !config.Get(section.name, "W").empty();
        meter.height_defined = !config.Get(section.name, "H").empty();
        meter.rect = {position("X", true), position("Y", false),
                      Integer(ExpandMeasures(config.Get(section.name, "W", "0"))),
                      Integer(ExpandMeasures(config.Get(section.name, "H", "0")))};
        meter.anchor_x = meter.rect.x;
        meter.anchor_y = meter.rect.y;
        meter.hidden = ReadInteger(section.name, "Hidden", 0) != 0;
        meter.bar_color = ParseColor(config.Get(section.name, "BarColor"), meter.bar_color);
        meter.solid_color = ParseColor(config.Get(section.name, "SolidColor"), meter.solid_color);
        const auto solid_color2 = config.Get(section.name, "SolidColor2");
        meter.solid_color2_defined = !solid_color2.empty();
        meter.solid_color2 = meter.solid_color2_defined ?
            ParseColor(solid_color2, meter.solid_color) : meter.solid_color;
        meter.gradient_angle = ReadNumber(section.name, "GradientAngle", 0.0);
        const auto transformation = config.Get(section.name, "TransformationMatrix");
        if (!transformation.empty()) {
          std::istringstream input(transformation);
          std::string component;
          size_t index = 0;
          while (index < meter.transformation.size() && std::getline(input, component, ';')) {
            meter.transformation[index++] = Formula(ExpandMeasures(config.Expand(Trim(component))));
          }
          meter.transformed = index == meter.transformation.size() && !std::getline(input, component, ';');
        }
        meter.horizontal = Lower(config.Get(section.name, "BarOrientation", "Vertical")) == "horizontal";
        meter.left_up_action = config.Get(section.name, "LeftMouseUpAction");
        if (meter_type == "string") {
          meter.text = config.Get(section.name, "Text");
          meter.font_face = config.Get(section.name, "FontFace", "Arial");
          meter.font_size = ReadNumber(section.name, "FontSize", 10.0);
          meter.font_color = ParseColor(config.Get(section.name, "FontColor"), meter.font_color);
          meter.effect_color = ParseColor(config.Get(section.name, "FontEffectColor"), meter.effect_color);
          meter.align = Lower(config.Get(section.name, "StringAlign", "Left"));
          meter.string_case = Lower(config.Get(section.name, "StringCase", "None"));
          meter.string_style = Lower(config.Get(section.name, "StringStyle", "Normal"));
          meter.string_effect = Lower(config.Get(section.name, "StringEffect", "None"));
          meter.clip = ReadInteger(section.name, "ClipString", 0) != 0;
          meter.autoscale = ReadInteger(section.name, "AutoScale", 0) != 0;
          meter.percentual = ReadInteger(section.name, "Percentual", 0) != 0;
          const auto decimals = config.Get(section.name, "NumOfDecimals");
          const auto scale = config.Get(section.name, "Scale", "1");
          meter.decimals = decimals.empty() ?
              ((meter.percentual || (!meter.autoscale && scale.find('.') == std::string::npos)) ? 0 : 1) :
              static_cast<int>(ReadNumber(section.name, "NumOfDecimals", 0.0));
          meter.scale = ReadNumber(section.name, "Scale", 1.0);
          meter.prefix = config.Get(section.name, "Prefix");
          meter.postfix = config.Get(section.name, "Postfix");
        } else if (meter_type == "image" || meter_type == "button" || meter_type == "rotator" ||
                   meter_type == "bitmap" ||
                   (meter_type == "bar" && !config.Get(section.name, "BarImage").empty())) {
          const std::string option = meter_type == "button" ? "ButtonImage" :
              meter_type == "bitmap" ? "BitmapImage" : meter_type == "bar" ? "BarImage" : "ImageName";
          const auto raw_image_name = config.Get(section.name, option);
          auto image_name = ExpandMeasures(raw_image_name);
          meter.image_tint = ParseColor(config.Get(section.name, "ImageTint"), meter.image_tint);
          meter.image_opacity = std::clamp(
              ReadNumber(section.name, "ImageAlpha", 255.0) / 255.0, 0.0, 1.0);
          if (image_name.empty() && !meter.measure.empty()) {
            if (const auto* bound = FindMeasure(meter.measure); bound && bound->plugin_string) {
              image_name = bound->string_value;
            }
          }
          if (image_name.rfind("file://", 0) == 0) {
            char* decoded = g_uri_unescape_string(image_name.c_str() + 7, nullptr);
            if (decoded) {
              image_name = decoded;
              g_free(decoded);
            }
          }
          if (!image_name.empty() && raw_image_name.find('[') == std::string::npos &&
              raw_image_name.find("%1") == std::string::npos) {
            try {
              const auto image_path = config.ResolveImagePath(image_name);
              std::string image_error;
              meter.image = LoadImage(image_path, image_error, meter.image_tint, meter.image_opacity);
              if (!meter.image) {
                EventLog("diagnostic", "unable to load " + option + " in [" + section.name + "]: " + image_error);
              } else {
                double image_width = cairo_image_surface_get_width(meter.image);
                double image_height = cairo_image_surface_get_height(meter.image);
                if (meter_type == "button") {
                  if (image_height > image_width) image_height /= 3.0;
                  else image_width /= 3.0;
                } else if (meter_type == "bitmap") {
                  meter.bitmap_frames = std::max(1, ReadInteger(section.name, "BitmapFrames", 1));
                  meter.bitmap_zero_frame = ReadInteger(section.name, "BitmapZeroFrame", 0) != 0;
                  if (image_height >= image_width) image_height /= meter.bitmap_frames;
                  else image_width /= meter.bitmap_frames;
                }
                meter.image_source_width = image_width;
                meter.image_source_height = image_height;
                if (meter_type != "rotator" && !meter.width_defined && !meter.height_defined) {
                  meter.rect.width = static_cast<int>(image_width);
                  meter.rect.height = static_cast<int>(image_height);
                } else if (meter_type != "rotator" && !meter.width_defined && image_height > 0) {
                  meter.rect.width = static_cast<int>(meter.rect.height * image_width / image_height);
                } else if (meter_type != "rotator" && !meter.height_defined && image_width > 0) {
                  meter.rect.height = static_cast<int>(meter.rect.width * image_height / image_width);
                }
              }
              meter.image_name = image_name;
            } catch (const std::exception& exception) {
              EventLog("diagnostic", "unable to resolve " + option + " in [" + section.name + "]: " +
                  exception.what());
            }
          }
          if (meter_type == "button") meter.left_up_action = config.Get(section.name, "ButtonCommand");
          if (meter_type == "rotator") {
            meter.offset_x = ReadNumber(section.name, "OffsetX", 0.0);
            meter.offset_y = ReadNumber(section.name, "OffsetY", 0.0);
            meter.start_angle = ReadNumber(section.name, "StartAngle", 0.0);
            meter.rotation_angle = ReadNumber(section.name, "RotationAngle", 6.2832);
            meter.value_remainder = ReadInteger(section.name,
              config.Get(section.name, "ValueRemainder").empty() ? "ValueReminder" : "ValueRemainder", 0);
          }
        } else if (meter_type == "shape") {
          meter.shape = config.Get(section.name, "Shape");
        } else if (meter_type == "roundline") {
          meter.line_color = ParseColor(config.Get(section.name, "LineColor"), meter.line_color);
          meter.line_width = ReadNumber(section.name, "LineWidth", 1.0);
          meter.line_start = ReadNumber(section.name, "LineStart", -1.0);
          meter.line_length = ReadNumber(section.name, "LineLength", 20.0);
          meter.start_angle = ReadNumber(section.name, "StartAngle", 0.0);
          meter.rotation_angle = ReadNumber(section.name, "RotationAngle", 6.2832);
          meter.value_remainder = ReadInteger(section.name,
              config.Get(section.name, "ValueRemainder").empty() ? "ValueReminder" : "ValueRemainder", 0);
          meter.solid = ReadInteger(section.name, "Solid", 0) != 0;
        } else if (meter_type == "line" || meter_type == "histogram") {
          meter.line_color = ParseColor(config.Get(section.name,
              meter_type == "line" ? "LineColor" : "PrimaryColor"),
              meter_type == "line" ? Color{1.0, 1.0, 1.0, 1.0} : Color{0.0, 1.0, 0.0, 1.0});
          meter.line_width = ReadNumber(section.name, "LineWidth", 1.0);
          meter.graph_autoscale = ReadInteger(section.name, "AutoScale", 0) != 0;
          meter.graph_flip = ReadInteger(section.name, "Flip", 0) != 0;
          meter.history.assign(std::max(0, meter.rect.width), 0.0);
        } else if (meter_type != "bar") {
          EventLog("diagnostic", "unsupported meter " + type + " in [" + section.name + "]");
        }
        meters.push_back(std::move(meter));
        UpdateMeter(meters.back().name);
      }
    }
    for (auto& measure : measures) if (!measure.disabled) UpdateMeasure(measure.name);
    for (auto& meter : meters) UpdateMeter(meter.name);
    UpdateSizeBuiltins();
    EventLog("lifecycle", "initial update complete");
    if (!on_refresh_action.empty()) {
      try {
        Execute(on_refresh_action);
      } catch (const std::exception& error) {
        EventLog("diagnostic", "OnRefreshAction: " + std::string(error.what()));
      }
    }
    scroll_updated_at = std::chrono::steady_clock::now();
  }

  void PluginUnavailable(Measure& measure, const std::string& reason) {
    if (!measure.capability_reported) {
      EventLog("diagnostic", measure.plugin + " adapter unavailable in [" + measure.name + "]: " + reason);
      measure.capability_reported = true;
    }
  }

  void SetPluginValue(Measure& measure, double number, std::string value = {}, bool is_string = false) {
    measure.value = number;
    measure.string_value = value.empty() && !is_string ? Number(number) : std::move(value);
    measure.plugin_string = is_string;
  }

  void RefreshGpuInfo() {
    if (gpu_info_cycle == update_counter) return;
    gpu_info_cycle = update_counter;
    gpu_info.clear();
    const auto result = RunCapture({"nvidia-smi", "--query-gpu=name,temperature.gpu,utilization.gpu,"
                                    "utilization.memory,memory.used,memory.total,clocks.current.graphics,"
                                    "power.draw,fan.speed", "--format=csv,noheader,nounits"}, 2500);
    if (result.status != 0) return;
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream fields(line);
      std::string field;
      std::vector<std::string> values;
      while (std::getline(fields, field, ',')) values.push_back(Trim(field));
      if (values.size() < 9) continue;
      GpuInfo info;
      info.name = values[0];
      info.temperature = ToNumber(values[1]);
      info.utilization = ToNumber(values[2]);
      info.memory_utilization = ToNumber(values[3]);
      info.memory_used = ToNumber(values[4]);
      info.memory_total = ToNumber(values[5]);
      info.clock = ToNumber(values[6]);
      info.power = ToNumber(values[7]);
      info.fan = ToNumber(values[8]);
      gpu_info.push_back(std::move(info));
    }
  }

  bool PrepareAudio(const Measure& parent) {
    const auto parent_option = [&](const std::string& name, const std::string& fallback = {}) {
      return config.Get(parent.name, name, fallback);
    };
    const auto port = Lower(parent_option("Port", "Output"));
    std::string device = parent_option("ID");
    if (device.empty()) device = port == "input" ? "@DEFAULT_SOURCE@" : "@DEFAULT_MONITOR@";
    if (audio_fd < 0 || device != audio_device) {
      audio_capture_ready = false;
      audio_analysis_parent.clear();
      if (audio_fd >= 0) close(audio_fd);
      if (audio_pid > 0) {
        kill(audio_pid, SIGTERM);
        waitpid(audio_pid, nullptr, 0);
      }
      audio_fd = -1;
      audio_pid = -1;
      audio_bytes.clear();
      int descriptors[2];
      if (pipe(descriptors) != 0) return false;
      const pid_t child = fork();
      if (child == 0) {
        close(descriptors[0]);
        dup2(descriptors[1], STDOUT_FILENO);
        const int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) dup2(null_fd, STDERR_FILENO);
        close(descriptors[1]);
        execlp("parec", "parec", "--raw", ("--device=" + device).c_str(), "--format=s16le",
               "--channels=2", "--rate=44100", "--latency-msec=25", nullptr);
        _exit(127);
      }
      close(descriptors[1]);
      if (child < 0) {
        close(descriptors[0]);
        return false;
      }
      audio_pid = child;
      audio_fd = descriptors[0];
      audio_device = device;
      fcntl(audio_fd, F_SETFL, fcntl(audio_fd, F_GETFL, 0) | O_NONBLOCK);
      pollfd descriptor{audio_fd, POLLIN, 0};
      poll(&descriptor, 1, 300);
    }
    if (!audio_capture_ready) {
      audio_capture_ready = true;
      char buffer[32768];
      for (;;) {
        const ssize_t count = read(audio_fd, buffer, sizeof(buffer));
        if (count > 0) audio_bytes.insert(audio_bytes.end(), buffer, buffer + count);
        else break;
      }
      constexpr size_t maximum_bytes = 44100 * 2 * sizeof(int16_t) / 2;
      if (audio_bytes.size() > maximum_bytes) {
        audio_bytes.erase(audio_bytes.begin(), audio_bytes.end() - maximum_bytes);
      }
    }
    const auto analysis_parent = Lower(parent.name);
    if (audio_analysis_parent == analysis_parent) return !audio_bytes.empty();
    audio_analysis_parent = analysis_parent;
    const size_t frame_count = audio_bytes.size() / (2 * sizeof(int16_t));
    if (frame_count < 64) return false;
    size_t sample_count = 64;
    while (sample_count * 2 <= std::min<size_t>(2048, frame_count)) sample_count *= 2;
    const size_t first = frame_count - sample_count;
    const auto channel = Lower(parent_option("Channel", "Avg"));
    std::vector<double> samples(sample_count);
    audio_peak = 0.0;
    double squares = 0.0;
    for (size_t index = 0; index < sample_count; ++index) {
      int16_t left = 0, right = 0;
      const size_t offset = (first + index) * 2 * sizeof(int16_t);
      std::memcpy(&left, audio_bytes.data() + offset, sizeof(left));
      std::memcpy(&right, audio_bytes.data() + offset + sizeof(left), sizeof(right));
      const double value = channel == "l" || channel == "left" ? left / 32768.0 :
          channel == "r" || channel == "right" ? right / 32768.0 :
          (left + right) / 65536.0;
      samples[index] = value;
      audio_peak = std::max(audio_peak, std::abs(value));
      squares += value * value;
    }
    audio_rms = std::sqrt(squares / sample_count);

    const int band_count = std::clamp(Integer(parent_option("Bands", "32")), 1, 256);
    const double minimum_frequency = std::max(1.0, Formula(parent_option("FreqMin", "20")));
    const double maximum_frequency = std::min(22050.0,
        std::max(minimum_frequency, Formula(parent_option("FreqMax", "20000"))));
    const double sensitivity = std::max(0.0, Formula(parent_option("Sensitivity", "1")));
    std::vector<std::complex<double>> spectrum(sample_count);
    for (size_t index = 0; index < sample_count; ++index) {
      const double window = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * index /
                                                std::max<size_t>(1, sample_count - 1));
      spectrum[index] = samples[index] * window;
    }
    for (size_t index = 1, reversed = 0; index < sample_count; ++index) {
      size_t bit = sample_count >> 1;
      for (; reversed & bit; bit >>= 1) reversed ^= bit;
      reversed ^= bit;
      if (index < reversed) std::swap(spectrum[index], spectrum[reversed]);
    }
    for (size_t length = 2; length <= sample_count; length <<= 1) {
      const std::complex<double> step = std::polar(1.0, -2.0 * 3.14159265358979323846 / length);
      for (size_t base = 0; base < sample_count; base += length) {
        std::complex<double> factor = 1.0;
        for (size_t offset = 0; offset < length / 2; ++offset) {
          const auto even = spectrum[base + offset];
          const auto odd = spectrum[base + offset + length / 2] * factor;
          spectrum[base + offset] = even + odd;
          spectrum[base + offset + length / 2] = even - odd;
          factor *= step;
        }
      }
    }
    audio_bands.assign(static_cast<size_t>(band_count), 0.0);
    for (int band = 0; band < band_count; ++band) {
      const double low_ratio = static_cast<double>(band) / band_count;
      const double high_ratio = static_cast<double>(band + 1) / band_count;
      const double low_frequency = minimum_frequency *
          std::pow(maximum_frequency / minimum_frequency, low_ratio);
      const double high_frequency = minimum_frequency *
          std::pow(maximum_frequency / minimum_frequency, high_ratio);
      const size_t low_bin = std::max<size_t>(1, static_cast<size_t>(low_frequency * sample_count / 44100.0));
      const size_t high_bin = std::min(sample_count / 2,
          std::max(low_bin + 1, static_cast<size_t>(std::ceil(high_frequency * sample_count / 44100.0))));
      double magnitude = 0.0;
      for (size_t bin = low_bin; bin < high_bin; ++bin) magnitude = std::max(magnitude, std::abs(spectrum[bin]));
      audio_bands[static_cast<size_t>(band)] = std::clamp(
          magnitude * 2.0 / sample_count * sensitivity, 0.0, 1.0);
    }
    return true;
  }

  static Impl* LuaRuntime(lua_State* state) {
    lua_getglobal(state, "__rainmeter_runtime");
    auto* runtime = static_cast<Impl*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return runtime;
  }

  static Measure* LuaSelf(lua_State* state) {
    lua_getglobal(state, "__rainmeter_measure");
    auto* measure = static_cast<Measure*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return measure;
  }

  static Measure* LuaMeasureObject(lua_State* state) {
    lua_getfield(state, 1, "__pointer");
    auto* measure = static_cast<Measure*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return measure;
  }

  static Meter* LuaMeterObject(lua_State* state) {
    lua_getfield(state, 1, "__pointer");
    auto* meter = static_cast<Meter*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return meter;
  }

  static int LuaSelfGetOption(lua_State* state) {
    auto* runtime = LuaRuntime(state);
    auto* measure = LuaSelf(state);
    const char* name = luaL_checkstring(state, 2);
    const char* fallback = lua_gettop(state) >= 3 ? lua_tostring(state, 3) : "";
    const auto value = runtime->config.Get(measure->name, name, fallback ? fallback : "");
    lua_pushlstring(state, value.data(), value.size());
    return 1;
  }

  static int LuaSelfGetNumberOption(lua_State* state) {
    auto* runtime = LuaRuntime(state);
    auto* measure = LuaSelf(state);
    const char* name = luaL_checkstring(state, 2);
    const double fallback = lua_gettop(state) >= 3 ? lua_tonumber(state, 3) : 0.0;
    try {
      lua_pushnumber(state, Formula(runtime->ExpandMeasures(
          runtime->config.Get(measure->name, name, Number(fallback)))));
    } catch (const std::exception&) {
      lua_pushnumber(state, fallback);
    }
    return 1;
  }

  static int LuaMeasureGetValue(lua_State* state) {
    auto* measure = LuaMeasureObject(state);
    lua_pushnumber(state, measure ? measure->value : 0.0);
    return 1;
  }

  static int LuaMeasureGetString(lua_State* state) {
    auto* measure = LuaMeasureObject(state);
    const std::string value = measure ? measure->string_value : "";
    lua_pushlstring(state, value.data(), value.size());
    return 1;
  }

  static int LuaMeasureGetRelativeValue(lua_State* state) {
    auto* measure = LuaMeasureObject(state);
    if (!measure) {
      lua_pushnumber(state, 0.0);
      return 1;
    }
    const double range = measure->maximum - measure->minimum;
    lua_pushnumber(state, range == 0.0 ? 0.0 :
        std::clamp((measure->value - measure->minimum) / range, 0.0, 1.0));
    return 1;
  }

  static int LuaMeterSetW(lua_State* state) {
    if (auto* meter = LuaMeterObject(state)) meter->rect.width = static_cast<int>(luaL_checknumber(state, 2));
    return 0;
  }

  static int LuaMeterSetH(lua_State* state) {
    if (auto* meter = LuaMeterObject(state)) meter->rect.height = static_cast<int>(luaL_checknumber(state, 2));
    return 0;
  }

  static int LuaMeterSetX(lua_State* state) {
    if (auto* meter = LuaMeterObject(state)) meter->rect.x = static_cast<int>(luaL_checknumber(state, 2));
    return 0;
  }

  static int LuaMeterSetY(lua_State* state) {
    if (auto* meter = LuaMeterObject(state)) meter->rect.y = static_cast<int>(luaL_checknumber(state, 2));
    return 0;
  }

  static void PushMeasureObject(lua_State* state, Measure* measure) {
    if (!measure) {
      lua_pushnil(state);
      return;
    }
    lua_newtable(state);
    lua_pushlightuserdata(state, measure);
    lua_setfield(state, -2, "__pointer");
    lua_pushcfunction(state, LuaMeasureGetValue);
    lua_setfield(state, -2, "GetValue");
    lua_pushcfunction(state, LuaMeasureGetString);
    lua_setfield(state, -2, "GetStringValue");
    lua_pushcfunction(state, LuaMeasureGetRelativeValue);
    lua_setfield(state, -2, "GetRelativeValue");
  }

  static void PushMeterObject(lua_State* state, Meter* meter) {
    if (!meter) {
      lua_pushnil(state);
      return;
    }
    lua_newtable(state);
    lua_pushlightuserdata(state, meter);
    lua_setfield(state, -2, "__pointer");
    lua_pushcfunction(state, LuaMeterSetW);
    lua_setfield(state, -2, "SetW");
    lua_pushcfunction(state, LuaMeterSetH);
    lua_setfield(state, -2, "SetH");
    lua_pushcfunction(state, LuaMeterSetX);
    lua_setfield(state, -2, "SetX");
    lua_pushcfunction(state, LuaMeterSetY);
    lua_setfield(state, -2, "SetY");
  }

  static int LuaSkinGetMeasure(lua_State* state) {
    auto* runtime = LuaRuntime(state);
    PushMeasureObject(state, runtime->FindMeasure(luaL_checkstring(state, 2)));
    return 1;
  }

  static int LuaSkinGetMeter(lua_State* state) {
    auto* runtime = LuaRuntime(state);
    PushMeterObject(state, runtime->FindMeter(luaL_checkstring(state, 2)));
    return 1;
  }

  static int LuaSkinReplaceVariables(lua_State* state) {
    auto* runtime = LuaRuntime(state);
    const auto value = runtime->config.Expand(luaL_checkstring(state, 2));
    lua_pushlstring(state, value.data(), value.size());
    return 1;
  }

  static int LuaSkinGetVariable(lua_State* state) {
    auto* runtime = LuaRuntime(state);
    const std::string name = luaL_checkstring(state, 2);
    const std::string marker = "#" + name + "#";
    const auto value = runtime->config.Expand(marker);
    if (value == marker && lua_gettop(state) >= 3) {
      lua_pushvalue(state, 3);
    } else {
      lua_pushlstring(state, value.data(), value.size());
    }
    return 1;
  }

  static int LuaSkinParseFormula(lua_State* state) {
    auto* runtime = LuaRuntime(state);
    try {
      lua_pushnumber(state, Formula(runtime->ExpandMeasures(
          runtime->config.Expand(luaL_checkstring(state, 2)))));
      return 1;
    } catch (const std::exception& error) {
      return luaL_error(state, "%s", error.what());
    }
  }

  static int LuaSkinMakePathAbsolute(lua_State* state) {
    auto* runtime = LuaRuntime(state);
    try {
      const auto value = runtime->config.ResolvePath(luaL_checkstring(state, 2)).string();
      lua_pushlstring(state, value.data(), value.size());
      return 1;
    } catch (const std::exception& error) {
      return luaL_error(state, "%s", error.what());
    }
  }

  static int LuaSkinBang(lua_State* state) {
    auto* runtime = LuaRuntime(state);
    const int count = lua_gettop(state);
    if (count < 2) return 0;
    std::string action = lua_tostring(state, 2) ? lua_tostring(state, 2) : "";
    if (count == 2 && !action.empty() && action.front() == '[') {
      runtime->Execute(action);
      return 0;
    }
    std::string command = "[" + action;
    for (int index = 3; index <= count; ++index) {
      const char* value = lua_tostring(state, index);
      command += " \"" + std::string(value ? value : "") + "\"";
    }
    command += ']';
    runtime->Execute(command);
    return 0;
  }

  void LuaError(Measure& measure, const std::string& phase) {
    const char* message = lua_tostring(measure.lua, -1);
    EventLog("diagnostic", "Lua " + phase + " error in [" + measure.name + "]: " +
             (message ? message : "unknown error"));
    lua_pop(measure.lua, 1);
  }

  bool InitializeLua(Measure& measure) {
    if (measure.lua_attempted) return measure.lua != nullptr;
    measure.lua_attempted = true;
    std::filesystem::path script;
    try {
      script = config.ResolvePath(measure.formula);
    } catch (const std::exception& error) {
      EventLog("diagnostic", "unable to resolve ScriptFile in [" + measure.name + "]: " + error.what());
      return false;
    }
    measure.lua = luaL_newstate();
    if (!measure.lua) return false;
    luaL_openlibs(measure.lua);
    lua_pushlightuserdata(measure.lua, this);
    lua_setglobal(measure.lua, "__rainmeter_runtime");
    lua_pushlightuserdata(measure.lua, &measure);
    lua_setglobal(measure.lua, "__rainmeter_measure");

    lua_newtable(measure.lua);
    lua_pushcfunction(measure.lua, LuaSelfGetOption);
    lua_setfield(measure.lua, -2, "GetOption");
    lua_pushcfunction(measure.lua, LuaSelfGetNumberOption);
    lua_setfield(measure.lua, -2, "GetNumberOption");
    lua_setglobal(measure.lua, "SELF");

    lua_newtable(measure.lua);
    for (const auto& function : std::vector<std::pair<const char*, lua_CFunction>>{
             {"GetMeasure", LuaSkinGetMeasure}, {"GetMeter", LuaSkinGetMeter},
             {"ReplaceVariables", LuaSkinReplaceVariables}, {"GetVariable", LuaSkinGetVariable},
             {"ParseFormula", LuaSkinParseFormula}, {"MakePathAbsolute", LuaSkinMakePathAbsolute},
             {"Bang", LuaSkinBang}}) {
      lua_pushcfunction(measure.lua, function.second);
      lua_setfield(measure.lua, -2, function.first);
    }
    lua_setglobal(measure.lua, "SKIN");
    if (luaL_loadfile(measure.lua, script.c_str()) != 0 || lua_pcall(measure.lua, 0, 0, 0) != 0) {
      LuaError(measure, "load");
      lua_close(measure.lua);
      measure.lua = nullptr;
      return false;
    }
    lua_getglobal(measure.lua, "Initialize");
    if (lua_isfunction(measure.lua, -1)) {
      if (lua_pcall(measure.lua, 0, 0, 0) != 0) LuaError(measure, "Initialize");
    } else {
      lua_pop(measure.lua, 1);
    }
    return true;
  }

  void UpdatePlugin(Measure& measure) {
    const auto option = [&](const std::string& name, const std::string& fallback = {}) {
      return config.Get(measure.name, name, fallback);
    };
    const auto unavailable = [&](const std::string& reason) { PluginUnavailable(measure, reason); };

    if (measure.plugin == "quote") {
      if (measure.source_path.empty()) return unavailable("PathName is missing or unresolved");
      try {
        SetPluginValue(measure, 0.0, Trim(ReadUtf8(measure.source_path)), true);
      } catch (const std::exception& exception) {
        unavailable(exception.what());
      }
      return;
    }

    if (measure.plugin == "actiontimer" || measure.plugin == "inputtext" ||
        measure.plugin == "runcommand" || measure.plugin == "mediakey") {
      SetPluginValue(measure, 0.0);
      return;
    }

    if (measure.plugin == "webparser") {
      const std::string raw_url = option("Url");
      const int index = std::max(0, Integer(option("StringIndex", "0")));
      if (raw_url.size() > 2 && raw_url.front() == '[' && raw_url.back() == ']') {
        const auto* parent = FindMeasure(raw_url.substr(1, raw_url.size() - 2));
        if (parent && parent->plugin == "webparser") {
          if (index < static_cast<int>(parent->captures.size())) {
            SetPluginValue(measure, ToNumber(parent->captures[index]), parent->captures[index], true);
            return;
          }
          return unavailable("capture " + std::to_string(index) + " is unavailable from [" + parent->name + "]");
        }
      }

      std::string source;
      std::string url = ExpandMeasures(raw_url);
      if (const auto failed = web_cycle_errors.find(url); failed != web_cycle_errors.end()) {
        return unavailable(failed->second);
      }
      if (const auto cached = web_cycle_cache.find(url); cached != web_cycle_cache.end()) {
        source = cached->second;
      } else {
      try {
        if (url.rfind("file://", 0) == 0) {
          source = ReadUtf8(url.substr(7));
        } else if (url.find("://") == std::string::npos) {
          source = ReadUtf8(config.ResolvePath(url));
        } else {
          const auto response = RunCapture({"curl", "--fail", "--location", "--silent", "--show-error",
                                            "--max-time", "10", url}, 12000);
          if (response.status != 0) {
            const auto reason = response.timed_out ? "request timed out" : Trim(response.output);
            web_cycle_errors[url] = reason;
            return unavailable(reason);
          }
          source = response.output;
        }
      } catch (const std::exception& exception) {
        web_cycle_errors[url] = exception.what();
        return unavailable(exception.what());
      }
        web_cycle_cache[url] = source;
      }
      if (Integer(option("Download", "0")) != 0) {
        try {
          const auto directory = std::filesystem::temp_directory_path() / "xrainmeter-webparser";
          std::filesystem::create_directories(directory);
          const auto path = directory / (std::to_string(std::hash<std::string>{}(url)) + ".download");
          std::ofstream output(path, std::ios::binary | std::ios::trunc);
          output.write(source.data(), static_cast<std::streamsize>(source.size()));
          if (!output) return unavailable("unable to write downloaded file " + path.string());
          measure.captures = {path.string()};
          SetPluginValue(measure, 0.0, path.string(), true);
          const auto finish = option("FinishAction");
          if (!finish.empty()) Execute(finish);
          return;
        } catch (const std::exception& exception) {
          return unavailable(exception.what());
        }
      }
      const auto expression = option("RegExp");
      if (expression.empty()) {
        measure.captures = {source};
      } else {
        std::string error;
        measure.captures = RegexCaptures(expression, source, error);
        if (!error.empty()) return unavailable(error);
        if (measure.captures.empty()) return unavailable("regular expression did not match");
      }
      const std::string value = index < static_cast<int>(measure.captures.size()) ? measure.captures[index] : "";
      SetPluginValue(measure, ToNumber(value), value, true);
      return;
    }

    if (measure.plugin == "power") {
      const auto state = Lower(option("PowerState"));
      if (state == "percent") measure.maximum = 100.0;
      else if (state == "status") measure.maximum = 4.0;
      else if (state == "status2") measure.maximum = 255.0;
      if (state == "mhz" || state == "hz") {
        double mhz = 0.0;
        if (const auto frequency = ReadText("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")) {
          mhz = ToNumber(*frequency) / 1000.0;
        } else {
          std::ifstream cpuinfo("/proc/cpuinfo");
          std::string line;
          while (std::getline(cpuinfo, line)) {
            if (Lower(line).rfind("cpu mhz", 0) == 0) {
              const auto colon = line.find(':');
              if (colon != std::string::npos) mhz = ToNumber(Trim(line.substr(colon + 1)));
              break;
            }
          }
        }
        if (mhz <= 0.0) return unavailable("CPU frequency is not exposed by the kernel");
        SetPluginValue(measure, state == "hz" ? mhz * 1000000.0 : mhz);
        return;
      }

      std::filesystem::path battery;
      bool online = false;
      std::error_code error;
      if (std::filesystem::is_directory("/sys/class/power_supply")) {
        for (const auto& entry : std::filesystem::directory_iterator("/sys/class/power_supply", error)) {
          const auto type = ReadText(entry.path() / "type").value_or("");
          if (Lower(type) == "battery" && battery.empty()) battery = entry.path();
          if ((Lower(type) == "mains" || Lower(type) == "usb") &&
              ReadText(entry.path() / "online").value_or("0") == "1") online = true;
        }
      }
      if (state == "acline") return SetPluginValue(measure, online ? 1.0 : 0.0);
      if (battery.empty()) {
        return unavailable("no battery is exposed by /sys/class/power_supply");
      }
      const auto status = Lower(ReadText(battery / "status").value_or("unknown"));
      const double capacity = ToNumber(ReadText(battery / "capacity").value_or("100"));
      if (state == "percent") return SetPluginValue(measure, capacity);
      if (state == "status") {
        const double value = status == "charging" ? 1.0 : capacity <= 5.0 ? 2.0 : capacity <= 20.0 ? 3.0 : 4.0;
        return SetPluginValue(measure, value);
      }
      if (state == "status2") return SetPluginValue(measure, status == "charging" ? 8.0 : capacity <= 5.0 ? 4.0 : capacity <= 20.0 ? 2.0 : 1.0);
      if (state == "lifetime") {
        double seconds = ToNumber(ReadText(battery / "time_to_empty_now").value_or("0"));
        if (seconds <= 0.0) {
          const double energy = ToNumber(ReadText(battery / "energy_now").value_or("0"));
          const double power = ToNumber(ReadText(battery / "power_now").value_or("0"));
          if (power > 0.0) seconds = energy / power * 3600.0;
        }
        if (seconds <= 0.0) return unavailable("battery lifetime is unknown");
        const int hours = static_cast<int>(seconds) / 3600;
        const int minutes = (static_cast<int>(seconds) / 60) % 60;
        std::ostringstream formatted;
        formatted << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes;
        return SetPluginValue(measure, seconds, formatted.str(), true);
      }
      return unavailable("unsupported PowerState=" + state);
    }

    if (measure.plugin == "speedfan" || measure.plugin == "coretemp") {
      std::string kind = Lower(option("SpeedFanType", "temperature"));
      const std::string prefix = kind == "fan" ? "fan" : kind == "voltage" ? "in" : "temp";
      auto sensors = GlobFiles("/sys/class/hwmon", prefix, "_input");
      const int number = std::max(0, Integer(option(measure.plugin == "speedfan" ? "SpeedFanNumber" : "CoreTempIndex", "0")));
      if (number >= static_cast<int>(sensors.size())) return unavailable("requested hwmon sensor index is not available");
      double value = ToNumber(ReadText(sensors[number]).value_or("0"));
      if (prefix == "temp") value /= 1000.0;
      if (prefix == "in") value /= 1000.0;
      const auto scale = Lower(option("SpeedFanScale", "c"));
      if (prefix == "temp" && scale == "f") value = value * 1.8 + 32.0;
      if (prefix == "temp" && scale == "k") value += 273.15;
      return SetPluginValue(measure, value);
    }

    if (measure.plugin == "perfmon") {
      const auto counter_name = Lower(option("PerfMonCounter"));
      std::ifstream diskstats("/proc/diskstats");
      std::string line;
      uint64_t read_bytes = 0;
      uint64_t write_bytes = 0;
      while (std::getline(diskstats, line)) {
        std::istringstream fields(line);
        int major = 0, minor = 0;
        std::string device;
        uint64_t reads = 0, reads_merged = 0, sectors_read = 0, read_ms = 0;
        uint64_t writes = 0, writes_merged = 0, sectors_written = 0;
        fields >> major >> minor >> device >> reads >> reads_merged >> sectors_read >> read_ms
               >> writes >> writes_merged >> sectors_written;
        if (!fields || device.rfind("loop", 0) == 0 || device.rfind("ram", 0) == 0) continue;
        read_bytes += sectors_read * 512ULL;
        write_bytes += sectors_written * 512ULL;
      }
      const uint64_t previous_read = measure.previous_counter;
      const uint64_t previous_write = measure.previous_counter2;
      measure.previous_counter = read_bytes;
      measure.previous_counter2 = write_bytes;
      double value = 0.0;
      if (previous_read && previous_write && update_interval > 0) {
        const double scale = 1000.0 / update_interval;
        if (counter_name.find("read") != std::string::npos) value = (read_bytes - previous_read) * scale;
        else if (counter_name.find("write") != std::string::npos) value = (write_bytes - previous_write) * scale;
        else value = ((read_bytes - previous_read) + (write_bytes - previous_write)) * scale;
      }
      return SetPluginValue(measure, value);
    }

    if (measure.plugin == "audiolevel") {
      const auto type = Lower(option("Type", "rms"));
      const Measure* parent = &measure;
      if (type == "band") {
        const auto parent_name = option("Parent");
        parent = FindMeasure(parent_name);
        if (!parent || parent->plugin != "audiolevel") return unavailable("AudioLevel Parent is invalid");
      }
      if (!PrepareAudio(*parent)) return unavailable("PulseAudio/PipeWire monitor produced no samples");
      double value = type == "peak" ? audio_peak : audio_rms;
      if (type == "band") {
        const int index = std::max(0, Integer(option("BandIdx", "0")));
        if (index >= static_cast<int>(audio_bands.size())) return unavailable("BandIdx is out of range");
        value = audio_bands[static_cast<size_t>(index)];
      } else if (type != "rms" && type != "peak") {
        return unavailable("unsupported AudioLevel Type=" + type);
      }
      measure.maximum = 1.0;
      if (Integer(option("DB", "0")) != 0) value = value > 0.0 ? 20.0 * std::log10(value) : -100.0;
      return SetPluginValue(measure, value);
    }

    if (measure.plugin == "win7audio") {
      measure.maximum = 100.0;
      const auto mute = RunCapture({"pactl", "get-sink-mute", "@DEFAULT_SINK@"}, 2000);
      const auto volume = RunCapture({"pactl", "get-sink-volume", "@DEFAULT_SINK@"}, 2000);
      if (volume.status != 0) return unavailable(Trim(volume.output).empty() ? "pactl cannot query the default sink" : Trim(volume.output));
      std::smatch match;
      static const std::regex percent(R"(([0-9]+)%)");
      if (!std::regex_search(volume.output, match, percent)) return unavailable("cannot parse pactl volume");
      const bool muted = mute.status == 0 && Lower(mute.output).find("yes") != std::string::npos;
      return SetPluginValue(measure, muted ? -1.0 : ToNumber(match[1].str()));
    }

    if (measure.plugin == "fileview") {
      const std::string raw_path = option("Path");
      const Measure* parent = nullptr;
      if (raw_path.size() > 2 && raw_path.front() == '[' && raw_path.back() == ']') {
        parent = FindMeasure(raw_path.substr(1, raw_path.size() - 2));
      }
      if (parent && parent->plugin == "fileview") {
        measure.file_entries = parent->file_entries;
      } else {
        std::filesystem::path folder;
        try {
          folder = config.ResolvePath(ExpandMeasures(raw_path));
        } catch (const std::exception& exception) {
          return unavailable(exception.what());
        }
        measure.file_entries.clear();
        const bool show_files = Integer(option("ShowFile", "1")) != 0;
        const bool show_folders = Integer(option("ShowFolder", "1")) != 0;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
          if (error) break;
          if ((entry.is_regular_file(error) && show_files) || (entry.is_directory(error) && show_folders)) {
            measure.file_entries.push_back(entry.path());
          }
        }
        std::sort(measure.file_entries.begin(), measure.file_entries.end(), [](const auto& left, const auto& right) {
          return Lower(left.filename().string()) < Lower(right.filename().string());
        });
        if (Integer(option("SortAscending", "1")) == 0) {
          std::reverse(measure.file_entries.begin(), measure.file_entries.end());
        }
      }
      const int index = Integer(option("Index", "0"));
      if (index <= 0) return SetPluginValue(measure, static_cast<double>(measure.file_entries.size()));
      if (index > static_cast<int>(measure.file_entries.size())) return SetPluginValue(measure, 0.0, "", true);
      const auto& path = measure.file_entries[static_cast<size_t>(index - 1)];
      const auto type = Lower(option("Type", "filename"));
      if (type == "filesize") {
        std::error_code error;
        return SetPluginValue(measure, path.has_filename() && std::filesystem::is_regular_file(path, error) ?
            static_cast<double>(std::filesystem::file_size(path, error)) : 0.0);
      }
      if (type == "filedate") {
        std::error_code error;
        const auto timestamp = std::filesystem::last_write_time(path, error).time_since_epoch().count();
        return SetPluginValue(measure, error ? 0.0 : static_cast<double>(timestamp));
      }
      std::string value = type == "filepath" ? path.string() : type == "extension" ? path.extension().string() :
          path.filename().string();
      if (type == "filename" && Integer(option("HideExtensions", "0")) != 0 && path.has_extension()) {
        value = path.stem().string();
      }
      return SetPluginValue(measure, 0.0, value, true);
    }

    if (measure.plugin == "folderinfo") {
      std::filesystem::path folder;
      const std::string raw_folder = option("Folder", option("Path"));
      try {
        if (raw_folder.size() > 2 && raw_folder.front() == '[' && raw_folder.back() == ']') {
          const auto* parent = FindMeasure(raw_folder.substr(1, raw_folder.size() - 2));
          if (parent && parent->plugin == "folderinfo") folder = parent->source_path;
        }
        if (folder.empty()) folder = config.ResolvePath(ExpandMeasures(raw_folder));
      } catch (const std::exception& exception) {
        return unavailable(exception.what());
      }
      measure.source_path = folder;
      uint64_t bytes = 0, files = 0, folders = 0;
      std::error_code error;
      const bool include_subfolders = Integer(option("IncludeSubFolders", "0")) != 0;
      const bool include_hidden = Integer(option("IncludeHiddenFiles", "0")) != 0;
      const auto count = [&](const auto& entry) {
        if (!include_hidden && entry.path().filename().string().rfind('.', 0) == 0) return;
        if (entry.is_directory(error)) ++folders;
        else if (entry.is_regular_file(error)) {
          ++files;
          bytes += entry.file_size(error);
        }
      };
      if (include_subfolders) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 folder, std::filesystem::directory_options::skip_permission_denied, error)) count(entry);
      } else {
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) count(entry);
      }
      const auto type = Lower(option("InfoType", "foldersize"));
      return SetPluginValue(measure, type.find("filecount") != std::string::npos ? files :
                                     type.find("foldercount") != std::string::npos ? folders : bytes);
    }

    if (measure.plugin == "advancedcpu") {
      auto names = [](std::string list) {
        std::set<std::string> result;
        std::istringstream stream(list);
        std::string name;
        while (std::getline(stream, name, ';')) if (!Trim(name).empty()) result.insert(Lower(Trim(name)));
        return result;
      };
      const auto includes = names(option("CPUInclude"));
      const auto excludes = names(option("CPUExclude"));
      const int top = Integer(option("TopProcess", "0"));
      const long ticks_per_second = std::max<long>(1, sysconf(_SC_CLK_TCK));
      uint64_t total_delta = 0, largest_delta = 0;
      std::string largest_name;
      std::unordered_map<std::string, uint64_t> current;
      std::error_code error;
      for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        const auto pid = entry.path().filename().string();
        if (!entry.is_directory() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        const auto stat = ReadText(entry.path() / "stat");
        if (!stat) continue;
        const auto left = stat->find('('), right = stat->rfind(')');
        if (left == std::string::npos || right == std::string::npos || right <= left) continue;
        const std::string process_name = stat->substr(left + 1, right - left - 1);
        const std::string lower_name = Lower(process_name);
        if ((!includes.empty() && !includes.count(lower_name)) || excludes.count(lower_name)) continue;
        std::istringstream fields(stat->substr(right + 2));
        std::vector<std::string> values;
        std::string value;
        while (fields >> value) values.push_back(value);
        if (values.size() <= 12) continue;
        const uint64_t cpu = static_cast<uint64_t>(std::strtoull(values[11].c_str(), nullptr, 10)) +
                             static_cast<uint64_t>(std::strtoull(values[12].c_str(), nullptr, 10));
        current[pid] = cpu;
        const auto old = measure.previous_process_cpu.find(pid);
        const uint64_t delta = old != measure.previous_process_cpu.end() && cpu >= old->second ? cpu - old->second : 0;
        total_delta += delta;
        if (delta > largest_delta) { largest_delta = delta; largest_name = process_name; }
      }
      measure.previous_process_cpu = std::move(current);
      const uint64_t selected = top == 0 ? total_delta : largest_delta;
      const double windows_ticks = static_cast<double>(selected) * 10000000.0 / ticks_per_second;
      return SetPluginValue(measure, windows_ticks, top == 2 ? largest_name : "", top == 2);
    }

    if (measure.plugin == "resmon") {
      const auto type = Lower(option("ResCountType", "gdi"));
      if (type == "window") return unavailable("Win32 window-object counts have no X11-equivalent semantics");
      if (type != "handle") return unavailable("GDI and USER object counts are intrinsically Win32-specific");
      const auto wanted = Lower(option("ProcessName"));
      uint64_t handles = 0;
      std::error_code error;
      for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        const auto pid = entry.path().filename().string();
        if (!entry.is_directory() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        if (!wanted.empty()) {
          const auto name = Lower(ReadText(entry.path() / "comm").value_or(""));
          if (name != wanted && name != wanted + ".exe") continue;
        }
        std::error_code descriptor_error;
        for (const auto& ignored : std::filesystem::directory_iterator(entry.path() / "fd", descriptor_error)) {
          (void)ignored;
          if (!descriptor_error) ++handles;
        }
      }
      return SetPluginValue(measure, static_cast<double>(handles));
    }

    if (measure.plugin == "usagemonitor") {
      const auto category = Lower(option("Category", "process"));
      const auto counter = Lower(option("Counter"));
      if (category != "process" || (counter.find("working set") == std::string::npos &&
                                    counter.find("private bytes") == std::string::npos)) {
        return unavailable("only Process working-set/private-byte counters currently map to /proc");
      }
      std::vector<std::pair<uint64_t, std::string>> processes;
      const long page_size = std::max<long>(1, sysconf(_SC_PAGESIZE));
      std::error_code error;
      for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        const auto pid = entry.path().filename().string();
        if (!entry.is_directory() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        std::ifstream statm(entry.path() / "statm");
        uint64_t size = 0, resident = 0;
        statm >> size >> resident;
        if (!statm) continue;
        processes.emplace_back((counter.find("private") != std::string::npos ? size : resident) * page_size,
                               ReadText(entry.path() / "comm").value_or(pid));
      }
      std::sort(processes.begin(), processes.end(), [](const auto& left, const auto& right) {
        return left.first > right.first;
      });
      const int index = std::max(1, Integer(option("Index", "1")));
      if (index > static_cast<int>(processes.size())) return SetPluginValue(measure, 0.0);
      const bool return_name = Lower(option("Name")) == "true" || Integer(option("Name", "0")) != 0;
      return SetPluginValue(measure, static_cast<double>(processes[index - 1].first),
                            return_name ? processes[index - 1].second : "", return_name);
    }

    if (measure.plugin == "ping") {
      const auto destination = option("DestAddress", option("Address"));
      if (destination.empty()) return unavailable("DestAddress is missing");
      const auto response = RunCapture({"ping", "-n", "-c", "1", "-W", "2", destination}, 3000);
      if (response.status != 0) return SetPluginValue(measure, -1.0);
      std::smatch match;
      static const std::regex latency(R"(time[=<]([0-9.]+))");
      return SetPluginValue(measure, std::regex_search(response.output, match, latency) ? ToNumber(match[1].str()) : -1.0);
    }

    if (measure.plugin == "nowplaying" || measure.plugin == "itunes") {
      GError* error = nullptr;
      GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
      if (!bus) {
        const std::string message = error ? error->message : "session D-Bus unavailable";
        if (error) g_error_free(error);
        return unavailable(message);
      }
      GVariant* names_reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
          "org.freedesktop.DBus", "ListNames", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE,
          1000, nullptr, &error);
      std::string player;
      if (names_reply) {
        GVariant* names = g_variant_get_child_value(names_reply, 0);
        GVariantIter iterator;
        const gchar* name = nullptr;
        g_variant_iter_init(&iterator, names);
        while (g_variant_iter_next(&iterator, "&s", &name)) {
          if (std::string(name).rfind("org.mpris.MediaPlayer2.", 0) == 0) {
            player = name;
            break;
          }
        }
        g_variant_unref(names);
        g_variant_unref(names_reply);
      }
      if (player.empty()) {
        const std::string message = error ? error->message : "no MPRIS media player is running";
        if (error) g_error_free(error);
        g_object_unref(bus);
        return unavailable(message);
      }
      GVariant* reply = g_dbus_connection_call_sync(bus, player.c_str(), "/org/mpris/MediaPlayer2",
          "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", "org.mpris.MediaPlayer2.Player"),
          G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
      g_object_unref(bus);
      if (!reply) {
        const std::string message = error ? error->message : "MPRIS property query failed";
        if (error) g_error_free(error);
        return unavailable(message);
      }
      GVariant* properties = g_variant_get_child_value(reply, 0);
      GVariant* metadata = g_variant_lookup_value(properties, "Metadata", G_VARIANT_TYPE("a{sv}"));
      const auto player_type = Lower(option("PlayerType", "title"));
      const gchar* string = nullptr;
      double number = 0.0;
      std::string value;
      if (player_type == "state") {
        measure.maximum = 2.0;
        g_variant_lookup(properties, "PlaybackStatus", "&s", &string);
        value = string ? string : "Stopped";
        number = value == "Playing" ? 1.0 : value == "Paused" ? 2.0 : 0.0;
      } else if (player_type == "volume") {
        measure.maximum = 100.0;
        g_variant_lookup(properties, "Volume", "d", &number);
        number *= 100.0;
      } else if (player_type == "position") {
        gint64 position = 0;
        g_variant_lookup(properties, "Position", "x", &position);
        number = position / 1000000.0;
      } else if (metadata) {
        const char* key = player_type == "album" ? "xesam:album" : player_type == "cover" ? "mpris:artUrl" : "xesam:title";
        if (player_type == "artist") {
          GVariant* artists = g_variant_lookup_value(metadata, "xesam:artist", G_VARIANT_TYPE("as"));
          if (artists && g_variant_n_children(artists)) {
            GVariant* first = g_variant_get_child_value(artists, 0);
            value = g_variant_get_string(first, nullptr);
            g_variant_unref(first);
          }
          if (artists) g_variant_unref(artists);
        } else if (player_type == "duration" || player_type == "progress") {
          gint64 length = 0, position = 0;
          g_variant_lookup(metadata, "mpris:length", "x", &length);
          g_variant_lookup(properties, "Position", "x", &position);
          number = player_type == "duration" ? length / 1000000.0 : length > 0 ? position * 100.0 / length : 0.0;
          if (player_type == "progress") measure.maximum = 100.0;
        } else if (g_variant_lookup(metadata, key, "&s", &string) && string) {
          value = string;
        }
      }
      if (metadata) g_variant_unref(metadata);
      g_variant_unref(properties);
      g_variant_unref(reply);
      const bool string_result = player_type == "title" || player_type == "artist" ||
          player_type == "album" || player_type == "cover" || player_type == "genre" ||
          player_type == "file";
      return SetPluginValue(measure, number, string_result ? value : std::string{}, string_result);
    }

    if (measure.plugin == "hwinfo") {
      const std::string id = option("HWiNFOID");
      const auto kind = Lower(option("HWiNFOType", "value"));
      const uint64_t numeric_id = static_cast<uint64_t>(std::strtoull(id.c_str(), nullptr, 10));
      const int sensor = static_cast<int>(numeric_id / 1000000ULL);
      const int entry = static_cast<int>(numeric_id % 10000ULL);
      auto cpu_name = [] {
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuinfo, line)) {
          if (Lower(line).rfind("model name", 0) != 0) continue;
          const auto colon = line.find(':');
          return Trim(colon == std::string::npos ? line : line.substr(colon + 1));
        }
        return std::string{};
      };
      auto hardware_values = [](const std::string& wanted_name, const std::string& prefix,
                                double divisor = 1.0) {
        std::vector<double> result;
        std::error_code error;
        for (const auto& directory : std::filesystem::directory_iterator("/sys/class/hwmon", error)) {
          if (!wanted_name.empty() && Lower(ReadText(directory.path() / "name").value_or("")) != wanted_name) continue;
          std::error_code entry_error;
          for (const auto& entry_path : std::filesystem::directory_iterator(directory.path(), entry_error)) {
            const auto filename = entry_path.path().filename().string();
            if (filename.rfind(prefix, 0) != 0 || filename.size() < 6 ||
                filename.substr(filename.size() - 6) != "_input") continue;
            result.push_back(ToNumber(ReadText(entry_path.path()).value_or("0")) / divisor);
          }
        }
        return result;
      };
      auto disk_models = [] {
        std::vector<std::string> result;
        std::error_code error;
        for (const auto& disk : std::filesystem::directory_iterator("/sys/block", error)) {
          const auto device = disk.path().filename().string();
          if (device.rfind("loop", 0) == 0 || device.rfind("ram", 0) == 0 ||
              device.rfind("dm-", 0) == 0) continue;
          auto model = Trim(ReadText(disk.path() / "device/model").value_or(""));
          if (model.empty()) model = device;
          result.push_back(model);
        }
        std::sort(result.begin(), result.end());
        return result;
      };
      RefreshGpuInfo();
      int gpu_index = 0;
      const auto lower_name = Lower(measure.name);
      const auto gpu_at = lower_name.find("gpu");
      if (gpu_at != std::string::npos && gpu_at + 3 < lower_name.size() &&
          std::isdigit(static_cast<unsigned char>(lower_name[gpu_at + 3]))) {
        gpu_index = std::max(0, lower_name[gpu_at + 3] - '1');
      }
      const bool gpu_sensor = sensor >= 9 || gpu_at != std::string::npos;
      const GpuInfo* gpu = gpu_index < static_cast<int>(gpu_info.size()) ? &gpu_info[gpu_index] : nullptr;
      if (kind == "sensorname") {
        if (gpu_sensor) {
          if (!gpu) return unavailable("the requested GPU is not exposed by nvidia-smi");
          return SetPluginValue(measure, 0.0, gpu->name, true);
        }
        const auto value = cpu_name();
        if (value.empty()) return unavailable("CPU model name is not exposed by /proc/cpuinfo");
        return SetPluginValue(measure, 0.0, value, true);
      }
      if (kind == "entryname") {
        if (sensor >= 6 && sensor <= 8) {
          const auto disks = disk_models();
          const int index = sensor - 6;
          if (index >= static_cast<int>(disks.size())) return unavailable("the requested physical disk is not present");
          return SetPluginValue(measure, 0.0, disks[index], true);
        }
        return unavailable("the requested HWiNFO entry name has no Linux mapping");
      }
      if (kind != "value") return unavailable("unsupported HWiNFOType=" + kind);
      if (entry == 7030 || (entry == 7000 && !gpu_sensor)) {
        std::ifstream stat("/proc/stat");
        std::string cpu;
        uint64_t user = 0, nice = 0, system = 0, idle = 0, wait = 0, irq = 0, softirq = 0, steal = 0;
        stat >> cpu >> user >> nice >> system >> idle >> wait >> irq >> softirq >> steal;
        const uint64_t idle_total = idle + wait;
        const uint64_t total = user + nice + system + idle + wait + irq + softirq + steal;
        double value = measure.value;
        if (measure.previous_total && total > measure.previous_total) {
          value = 100.0 * (1.0 - static_cast<double>(idle_total - measure.previous_idle) /
                                     static_cast<double>(total - measure.previous_total));
        }
        measure.previous_total = total;
        measure.previous_idle = idle_total;
        measure.maximum = 100.0;
        return SetPluginValue(measure, std::clamp(value, 0.0, 100.0));
      }
      if ((entry == 6000 || entry == 6001) && !gpu_sensor) {
        double mhz = ToNumber(ReadText("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq").value_or("0")) / 1000.0;
        if (mhz <= 0.0) {
          std::ifstream cpuinfo("/proc/cpuinfo");
          std::string line;
          while (std::getline(cpuinfo, line)) if (Lower(line).rfind("cpu mhz", 0) == 0) {
            mhz = ToNumber(Trim(line.substr(line.find(':') + 1)));
            break;
          }
        }
        if (mhz <= 0.0) return unavailable("CPU frequency is not exposed by the kernel");
        return SetPluginValue(measure, mhz);
      }
      if (gpu_sensor) {
        if (!gpu) return unavailable("the requested GPU is not exposed by nvidia-smi");
        if (entry == 7000) { measure.maximum = 100.0; return SetPluginValue(measure, gpu->utilization); }
        if (entry == 7008) { measure.maximum = 100.0; return SetPluginValue(measure, gpu->memory_utilization); }
        if (entry == 6000 || entry == 6001) return SetPluginValue(measure, gpu->clock);
        if (entry == 1003) return SetPluginValue(measure, gpu->temperature);
        if (entry == 3000 || entry == 3001) return SetPluginValue(measure, gpu->fan);
        if (entry == 2000 || entry == 2001) return unavailable("GPU voltage is not exposed by nvidia-smi");
        if (entry == 1000) return unavailable("GPU VRM temperature is not exposed by nvidia-smi");
      }
      if (sensor >= 6 && sensor <= 8 && entry == 1000) {
        const auto temperatures = hardware_values("nvme", "temp", 1000.0);
        const int index = sensor - 6;
        if (index >= static_cast<int>(temperatures.size())) {
          return unavailable("the requested disk temperature is not exposed by hwmon");
        }
        return SetPluginValue(measure, temperatures[index]);
      }
      if (sensor == 5 && entry == 1000) {
        auto temperatures = hardware_values("asus", "temp", 1000.0);
        if (temperatures.empty()) temperatures = hardware_values("acpitz", "temp", 1000.0);
        if (temperatures.empty()) return unavailable("motherboard temperature is not exposed by hwmon");
        return SetPluginValue(measure, temperatures.front());
      }
      if (sensor == 2 || entry == 1000 || entry == 1003) {
        auto temperatures = hardware_values(sensor == 2 ? "coretemp" : "", "temp", 1000.0);
        if (temperatures.empty()) return unavailable("the requested temperature sensor is not exposed by hwmon");
        return SetPluginValue(measure, *std::max_element(temperatures.begin(), temperatures.end()));
      }
      if (entry == 3000 || entry == 3001) {
        const auto fans = hardware_values("", "fan");
        if (fans.empty()) return unavailable("the requested fan sensor is not exposed by hwmon");
        return SetPluginValue(measure, fans[std::min<size_t>(entry - 3000, fans.size() - 1)]);
      }
      if (entry == 8000) return unavailable("SMART failure state requires a privileged native storage provider");
      return unavailable("the requested HWiNFO sensor ID has no Linux mapping");
    }

    if (measure.plugin == "topprocesses") {
      struct ProcessValue { std::string pid, name; uint64_t ticks = 0, bytes = 0; double cpu = 0.0; };
      std::vector<ProcessValue> processes;
      std::unordered_map<std::string, uint64_t> current;
      std::set<std::string> ignored;
      std::string ignored_text = option("IgnoredProcesses", option("GlobalIgnoredProcesses"));
      std::replace(ignored_text.begin(), ignored_text.end(), '|', ';');
      std::istringstream ignored_stream(ignored_text);
      std::string ignored_name;
      while (std::getline(ignored_stream, ignored_name, ';')) ignored.insert(Lower(Trim(ignored_name)));
      const long page_size = std::max<long>(1, sysconf(_SC_PAGESIZE));
      std::error_code error;
      for (const auto& entry_path : std::filesystem::directory_iterator("/proc", error)) {
        const auto pid = entry_path.path().filename().string();
        if (!entry_path.is_directory() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        const auto stat = ReadText(entry_path.path() / "stat");
        if (!stat) continue;
        const auto left = stat->find('('), right = stat->rfind(')');
        if (left == std::string::npos || right == std::string::npos || right <= left) continue;
        ProcessValue process;
        process.pid = pid;
        process.name = stat->substr(left + 1, right - left - 1);
        if (ignored.count(Lower(process.name))) continue;
        std::istringstream stat_fields(stat->substr(right + 2));
        std::vector<std::string> values;
        std::string value;
        while (stat_fields >> value) values.push_back(value);
        if (values.size() <= 21) continue;
        process.ticks = std::strtoull(values[11].c_str(), nullptr, 10) +
                        std::strtoull(values[12].c_str(), nullptr, 10);
        current[pid] = process.ticks;
        const auto previous = measure.previous_process_cpu.find(pid);
        const uint64_t delta = previous != measure.previous_process_cpu.end() && process.ticks >= previous->second ?
            process.ticks - previous->second : 0;
        process.cpu = update_interval > 0 ? delta * 100000.0 /
            (std::max<long>(1, sysconf(_SC_CLK_TCK)) * update_interval) : 0.0;
        std::ifstream statm(entry_path.path() / "statm");
        uint64_t size = 0, resident = 0;
        statm >> size >> resident;
        process.bytes = resident * static_cast<uint64_t>(page_size);
        processes.push_back(std::move(process));
      }
      measure.previous_process_cpu = std::move(current);
      const bool memory = Lower(option("MetricType", "cpu")) == "memory";
      std::sort(processes.begin(), processes.end(), [&](const auto& left, const auto& right) {
        return memory ? left.bytes > right.bytes : left.cpu > right.cpu;
      });
      int first = 0, last = 0;
      const auto range = option("ProcNums", "0");
      const auto dash = range.find('-');
      first = std::max(0, Integer(range.substr(0, dash)));
      last = dash == std::string::npos ? first : std::max(first, Integer(range.substr(dash + 1)));
      auto replace_all = [](std::string& text, const std::string& from, const std::string& to) {
        for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size()) text.replace(at, from.size(), to);
      };
      std::ostringstream output;
      for (int index = first; index <= last && index < static_cast<int>(processes.size()); ++index) {
        if (index > first) output << '\n';
        auto line = option("Format", "%CPU%: %pName");
        std::ostringstream cpu;
        cpu << std::fixed << std::setprecision(1) << processes[index].cpu;
        std::ostringstream memory_value;
        memory_value << std::fixed << std::setprecision(1) << processes[index].bytes / 1048576.0 << " MB";
        replace_all(line, "%pName", processes[index].name);
        replace_all(line, "%pID", processes[index].pid);
        replace_all(line, "%CPU", cpu.str());
        replace_all(line, "%Memory", memory_value.str());
        output << line;
      }
      return SetPluginValue(measure, 0.0, output.str(), true);
    }

    if (measure.plugin == "process") {
      const auto wanted = Lower(option("ProcessName"));
      uint64_t matches = 0;
      std::error_code error;
      for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        if (!entry.is_directory() || entry.path().filename().string().find_first_not_of("0123456789") != std::string::npos) continue;
        const auto name = Lower(ReadText(entry.path() / "comm").value_or(""));
        if (name == wanted || name == wanted + ".exe") ++matches;
      }
      return SetPluginValue(measure, static_cast<double>(matches));
    }

    if (measure.plugin == "recyclemanager") {
      const char* data = std::getenv("XDG_DATA_HOME");
      const char* user = std::getenv("HOME");
      const auto trash = data ? std::filesystem::path(data) / "Trash/files" :
          user ? std::filesystem::path(user) / ".local/share/Trash/files" : std::filesystem::path{};
      if (trash.empty() || !std::filesystem::is_directory(trash)) return SetPluginValue(measure, 0.0);
      uint64_t count = 0, bytes = 0;
      std::error_code error;
      for (const auto& entry : std::filesystem::recursive_directory_iterator(trash,
               std::filesystem::directory_options::skip_permission_denied, error)) {
        if (entry.is_regular_file(error)) { ++count; bytes += entry.file_size(error); }
      }
      return SetPluginValue(measure, Lower(option("RecycleType", "count")) == "size" ? bytes : count);
    }

    if (measure.plugin == "sysinfo") {
      struct utsname host {};
      uname(&host);
      const auto type = Lower(option("SysInfoType"));
      if (type == "lan_connectivity" || type == "internet_connectivity") {
        bool connected = false;
        std::error_code error;
        for (const auto& interface : std::filesystem::directory_iterator("/sys/class/net", error)) {
          if (interface.path().filename() == "lo") continue;
          if (Trim(ReadText(interface.path() / "operstate").value_or("")) == "up") {
            connected = true;
            break;
          }
        }
        if (type == "internet_connectivity" && connected) {
          connected = false;
          std::ifstream routes("/proc/net/route");
          std::string route;
          std::getline(routes, route);
          while (std::getline(routes, route)) {
            std::istringstream fields(route);
            std::string interface, destination, gateway, flags;
            fields >> interface >> destination >> gateway >> flags;
            if (destination == "00000000" && (std::strtoul(flags.c_str(), nullptr, 16) & 1U)) {
              connected = true;
              break;
            }
          }
        }
        return SetPluginValue(measure, connected ? 1.0 : -1.0);
      }
      std::string value = type == "computer_name" || type == "host_name" ? host.nodename :
          type == "user_name" ? (std::getenv("USER") ? std::getenv("USER") : "") :
          type == "os_version" ? std::string(host.sysname) + " " + host.release :
          type == "os_bits" ? (sizeof(void*) == 8 ? "64" : "32") : host.machine;
      return SetPluginValue(measure, 0.0, value, true);
    }

    if (measure.plugin == "syscolor") {
      return SetPluginValue(measure, static_cast<double>(update_counter));
    }

    if (measure.plugin == "chameleon") {
      const auto requested = Lower(option("Color", option("Type", "foreground1")));
      const std::string color = requested.find("foreground2") != std::string::npos ? "53,155,209" :
          requested.find("foreground") != std::string::npos ? "228,231,225" :
          requested.find("background2") != std::string::npos ? "33,35,27" : "33,35,27";
      return SetPluginValue(measure, 0.0, color, true);
    }

    if (measure.plugin == "wifistatus") {
      std::ifstream wireless("/proc/net/wireless");
      std::string line;
      while (std::getline(wireless, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::istringstream fields(line.substr(colon + 1));
        double status = 0, quality = 0, level = 0;
        fields >> status >> quality >> level;
        const auto type = Lower(option("WiFiInfoType", "quality"));
        return SetPluginValue(measure, type == "rssi" ? level : std::clamp(quality / 70.0 * 100.0, 0.0, 100.0));
      }
      return unavailable("no wireless interface is exposed by /proc/net/wireless");
    }

    static const std::unordered_map<std::string, std::string> limitations = {
        {"windowmessage", "Win32 HWND messages have no general Linux equivalent"},
        };
    if (const auto found = limitations.find(measure.plugin); found != limitations.end()) return unavailable(found->second);
    unavailable(KnownPlugin(measure.plugin) ? "this option set has no Linux mapping" : "unknown third-party Windows DLL");
  }

  double EvaluateFormula(const std::string& expression) {
    const auto wide = std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(expression);
    double result = 0.0;
    auto resolve = [](const wchar_t* text, int length, double* value, void* context) {
      auto* runtime = static_cast<Impl*>(context);
      const std::wstring key(text, static_cast<size_t>(length));
      const auto utf8 = std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(key);
      if (Lower(utf8) == "counter") {
        *value = static_cast<double>(runtime->update_counter);
        return true;
      }
      const auto* dependency = runtime->FindMeasure(utf8);
      if (!dependency) return false;
      *value = dependency->value;
      return true;
    };
    if (const wchar_t* error = MathParser::Parse(wide.c_str(), &result, resolve, this)) {
      throw std::runtime_error(std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(error));
    }
    return result;
  }

  void EvaluateMeasureActions(Measure& measure) {
    for (int index = 1; index <= 10; ++index) {
      const auto suffix = index == 1 ? std::string{} : std::to_string(index);
      const auto expression = config.Get(measure.name, "IfCondition" + suffix);
      if (expression.empty()) continue;
      try {
        const bool result = EvaluateFormula(ExpandMeasures(expression)) != 0.0;
        const int mode = Integer(config.Get(measure.name, "IfConditionMode", "0"));
        const auto previous = measure.condition_states.find(index);
        if (mode != 0 || previous == measure.condition_states.end() || previous->second != result) {
          const auto action = config.Get(measure.name,
              (result ? "IfTrueAction" : "IfFalseAction") + suffix);
          if (!action.empty()) Execute(action, measure.name);
        }
        measure.condition_states[index] = result;
      } catch (const std::exception& error) {
        EventLog("diagnostic", "IfCondition" + suffix + " in [" + measure.name + "]: " + error.what());
      }
    }
    struct LegacyCondition { const char* value; const char* action; int key; int relation; };
    static const LegacyCondition legacy[] = {
        {"IfAboveValue", "IfAboveAction", 101, 1},
        {"IfBelowValue", "IfBelowAction", 102, -1},
        {"IfEqualValue", "IfEqualAction", 103, 0}};
    for (const auto& item : legacy) {
      const auto threshold_text = config.Get(measure.name, item.value);
      if (threshold_text.empty()) continue;
      try {
        const double threshold = EvaluateFormula(ExpandMeasures(threshold_text));
        const bool result = item.relation > 0 ? measure.value > threshold :
            item.relation < 0 ? measure.value < threshold : measure.value == threshold;
        const auto previous = measure.condition_states.find(item.key);
        if (result && (previous == measure.condition_states.end() || !previous->second)) {
          const auto action = config.Get(measure.name, item.action);
          if (!action.empty()) Execute(action, measure.name);
        }
        measure.condition_states[item.key] = result;
      } catch (const std::exception& error) {
        EventLog("diagnostic", std::string(item.value) + " in [" + measure.name + "]: " + error.what());
      }
    }
  }

  void UpdateMeasure(const std::string& name) {
    auto* measure = FindMeasure(name);
    if (!measure) throw std::runtime_error("Unknown measure: " + name);
    config.SetBuiltin("CURRENTSECTION", measure->name);
    if (Lower(measure->type) == "calc") {
      measure->formula = config.Get(measure->name, "Formula", measure->formula);
      const auto expression = ExpandMeasures(measure->formula);
      const auto wide = std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(expression);
      auto resolve = [](const wchar_t* text, int length, double* value, void* context) {
        auto* runtime = static_cast<Impl*>(context);
        const std::wstring key(text, static_cast<size_t>(length));
        const auto utf8 = std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(key);
        if (Lower(utf8) == "counter") {
          *value = static_cast<double>(runtime->update_counter);
          return true;
        }
        const auto* dependency = runtime->FindMeasure(utf8);
        if (!dependency) return false;
        *value = dependency->value;
        return true;
      };
      if (const wchar_t* parse_error = MathParser::Parse(wide.c_str(), &measure->value, resolve, this)) {
        EventLog("diagnostic", "Formula error in [" + measure->name + "] for " + expression + ": " +
            std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(parse_error));
        measure->value = 0.0;
      }
      measure->string_value = Number(measure->value);
    } else if (Lower(measure->type) == "loop") {
      const int64_t start = static_cast<int64_t>(ReadNumber(measure->name, "StartValue", 1.0));
      const int64_t end = static_cast<int64_t>(ReadNumber(measure->name, "EndValue", 100.0));
      int64_t increment = static_cast<int64_t>(ReadNumber(measure->name, "Increment", 1.0));
      const int64_t count = std::max<int64_t>(0,
          static_cast<int64_t>(ReadNumber(measure->name, "LoopCount", 0.0)));
      const bool invert = ReadInteger(measure->name, "InvertMeasure", 0) != 0;
      if (increment == 0) increment = end >= start ? 1 : -1;
      if (start != measure->loop_start || end != measure->loop_end ||
          increment != measure->loop_increment || count != measure->loop_count || invert != measure->invert) {
        measure->loop_start = start;
        measure->loop_end = end;
        measure->loop_increment = increment;
        measure->loop_count = count;
        measure->invert = invert;
        measure->loops_completed = 0;
        measure->loop_started = false;
        measure->loop_at_end = false;
      }
      measure->minimum = static_cast<double>(std::min(start, end));
      measure->maximum = static_cast<double>(std::max(start, end));
      int64_t raw = static_cast<int64_t>(measure->value);
      if (!measure->loop_started) {
        raw = start;
        measure->loop_started = true;
      } else if (measure->loop_at_end) {
        if (count != 0 && measure->loops_completed >= count) {
          raw = end;
        } else {
          raw = start;
          measure->loop_at_end = false;
        }
      } else {
        raw += increment;
        if ((increment > 0 && raw >= end) || (increment < 0 && raw <= end)) {
          raw = end;
          measure->loop_at_end = true;
          ++measure->loops_completed;
        }
      }
      measure->value = static_cast<double>(invert ? start + end - raw : raw);
      measure->string_value = Number(measure->value);
    } else if (Lower(measure->type) == "time") {
      measure->formula = config.Get(measure->name, "Format", measure->formula);
      const auto now = std::time(nullptr);
      std::tm local{};
      localtime_r(&now, &local);
      char buffer[1024]{};
      if (std::strftime(buffer, sizeof(buffer), measure->formula.c_str(), &local) == 0) {
        throw std::runtime_error("Time format output too long in [" + measure->name + "]");
      }
      measure->value = static_cast<double>(now);
      measure->string_value = buffer;
    } else if (Lower(measure->type) == "registry") {
      const auto key = Lower(config.Get(measure->name, "RegKey"));
      const auto value = Lower(config.Get(measure->name, "RegValue"));
      if (key.find("centralprocessor") != std::string::npos && value == "processornamestring") {
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuinfo, line)) {
          if (Lower(line).rfind("model name", 0) != 0) continue;
          const auto colon = line.find(':');
          measure->string_value = colon == std::string::npos ? line : Trim(line.substr(colon + 1));
          break;
        }
      } else if (key.find("centralprocessor") != std::string::npos && value == "vendoridentifier") {
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuinfo, line)) {
          if (Lower(line).rfind("vendor_id", 0) != 0) continue;
          const auto colon = line.find(':');
          measure->string_value = colon == std::string::npos ? line : Trim(line.substr(colon + 1));
          break;
        }
      } else if (key.find("centralprocessor") != std::string::npos && value == "~mhz") {
        double mhz = ToNumber(ReadText("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq").value_or("0")) / 1000.0;
        if (mhz <= 0.0) {
          std::ifstream cpuinfo("/proc/cpuinfo");
          std::string line;
          while (std::getline(cpuinfo, line)) {
            if (Lower(line).rfind("cpu mhz", 0) != 0) continue;
            const auto colon = line.find(':');
            mhz = ToNumber(colon == std::string::npos ? line : Trim(line.substr(colon + 1)));
            break;
          }
        }
        measure->string_value = Number(mhz);
      } else if (key.find("system\\bios") != std::string::npos &&
                 (value == "baseboardmanufacturer" || value == "baseboardproduct")) {
        const auto path = value == "baseboardmanufacturer" ?
            "/sys/class/dmi/id/board_vendor" : "/sys/class/dmi/id/board_name";
        measure->string_value = Trim(ReadText(path).value_or(""));
      } else {
        EventLog("diagnostic", "Windows Registry value has no Linux mapping in [" + measure->name + "]");
        measure->string_value.clear();
      }
      measure->value = ToNumber(measure->string_value);
    } else if (Lower(measure->type) == "string") {
      measure->formula = config.Get(measure->name, "String", measure->formula);
      measure->string_value = ExpandMeasures(measure->formula);
      measure->value = ToNumber(measure->string_value);
    } else if (Lower(measure->type) == "script") {
      if (InitializeLua(*measure)) {
        lua_getglobal(measure->lua, "Update");
        if (lua_isfunction(measure->lua, -1)) {
          if (lua_pcall(measure->lua, 0, 1, 0) != 0) {
            LuaError(*measure, "Update");
          } else {
            if (lua_isnumber(measure->lua, -1)) {
              measure->value = lua_tonumber(measure->lua, -1);
              measure->string_value = Number(measure->value);
            } else if (lua_isstring(measure->lua, -1)) {
              measure->string_value = lua_tostring(measure->lua, -1);
              measure->value = ToNumber(measure->string_value);
            }
            lua_pop(measure->lua, 1);
          }
        } else {
          lua_pop(measure->lua, 1);
        }
      }
    } else if (Lower(measure->type) == "cpu") {
      if (measure->dynamic) measure->processor = ReadInteger(measure->name, "Processor", measure->processor);
      std::ifstream stat("/proc/stat");
      const std::string wanted = measure->processor > 0 ? "cpu" + std::to_string(measure->processor - 1) : "cpu";
      std::string line;
      while (std::getline(stat, line)) {
        std::istringstream values(line);
        std::string name;
        values >> name;
        if (name != wanted) continue;
        uint64_t user = 0, nice = 0, system = 0, idle = 0, wait = 0, irq = 0, softirq = 0, steal = 0;
        values >> user >> nice >> system >> idle >> wait >> irq >> softirq >> steal;
        const uint64_t idle_total = idle + wait;
        const uint64_t total = user + nice + system + idle + wait + irq + softirq + steal;
        if (measure->previous_total && total > measure->previous_total) {
          measure->value = 100.0 * (1.0 - static_cast<double>(idle_total - measure->previous_idle) /
                                             static_cast<double>(total - measure->previous_total));
        }
        measure->previous_total = total;
        measure->previous_idle = idle_total;
        break;
      }
      measure->value = std::clamp(measure->value, 0.0, 100.0);
      measure->string_value = Number(measure->value);
    } else if (Lower(measure->type) == "physicalmemory" || Lower(measure->type) == "swapmemory") {
      std::ifstream meminfo("/proc/meminfo");
      std::unordered_map<std::string, double> values;
      std::string memory_line;
      while (std::getline(meminfo, memory_line)) {
        std::istringstream fields(memory_line);
        std::string key;
        double kibibytes = 0.0;
        fields >> key >> kibibytes;
        if (!key.empty() && key.back() == ':') key.pop_back();
        values[key] = kibibytes * 1024.0;
      }
      const bool swap = Lower(measure->type) == "swapmemory";
      const double total = values[swap ? "SwapTotal" : "MemTotal"];
      const double free = values[swap ? "SwapFree" :
          (values.count("MemAvailable") ? "MemAvailable" : "MemFree")];
      measure->maximum = total;
      measure->value = measure->total ? total : (measure->invert ? free : total - free);
      measure->string_value = Number(measure->value);
    } else if (Lower(measure->type) == "freediskspace") {
      std::string host_path = measure->drive;
      if (host_path.size() >= 2 && std::isalpha(static_cast<unsigned char>(host_path[0])) &&
          host_path[1] == ':') host_path = "/";
      struct statvfs info {};
      if (statvfs(host_path.c_str(), &info) != 0) {
        EventLog("diagnostic", "unable to query disk path " + host_path + " for [" + measure->name + "]");
      } else {
        const double total = static_cast<double>(info.f_blocks) * info.f_frsize;
        const double free = static_cast<double>(info.f_bavail) * info.f_frsize;
        measure->maximum = total;
        measure->value = measure->total ? total : (measure->invert ? total - free : free);
      }
      measure->string_value = Number(measure->value);
    } else if (Lower(measure->type) == "netin" || Lower(measure->type) == "netout") {
      std::ifstream devices("/proc/net/dev");
      std::string line;
      uint64_t counter = 0;
      while (std::getline(devices, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto interface = Trim(line.substr(0, colon));
        if (interface == "lo") continue;
        std::istringstream values(line.substr(colon + 1));
        uint64_t receive = 0, transmit = 0, ignored = 0;
        values >> receive;
        for (int field = 0; field < 7; ++field) values >> ignored;
        values >> transmit;
        counter += Lower(measure->type) == "netin" ? receive : transmit;
      }
      if (measure->total) {
        measure->value = static_cast<double>(counter);
      } else if (measure->previous_counter && counter >= measure->previous_counter && update_interval > 0) {
        measure->value = static_cast<double>(counter - measure->previous_counter) * 1000.0 / update_interval;
      }
      measure->previous_counter = counter;
      measure->string_value = Number(measure->value);
    } else if (Lower(measure->type) == "uptime") {
      struct sysinfo info {};
      if (sysinfo(&info) != 0) throw std::runtime_error("Unable to query Linux uptime");
      measure->value = static_cast<double>(info.uptime);
      measure->string_value = Number(measure->value);
    } else if (Lower(measure->type) == "plugin") {
      UpdatePlugin(*measure);
    }
    measure->string_value = Substitute(measure->string_value, config.Get(measure->name, "Substitute"));
    EventLog("UpdateMeasure", measure->name + "=" + measure->string_value);
    EvaluateMeasureActions(*measure);
    const auto on_update = config.Get(measure->name, "OnUpdateAction");
    if (!on_update.empty()) {
      try {
        Execute(on_update, measure->name);
      } catch (const std::exception& error) {
        EventLog("diagnostic", "OnUpdateAction in [" + measure->name + "]: " + error.what());
      }
    }
  }

  void UpdateShape(Meter& meter) {
    const auto expression = ExpandMeasures(config.Get(meter.name, "Shape", meter.shape));
    if (expression.empty()) return;
    meter.shape = expression;
    meter.shape_kind = Meter::ShapeKind::Rectangle;
    meter.shape_gradient.clear();
    meter.shape_gradient_geometry.clear();
    meter.shape_radial_gradient = false;
    meter.shape_corner_radius = 0.0;
    meter.shape_dashes.clear();
    meter.shape_line_cap = CAIRO_LINE_CAP_BUTT;
    meter.shape_scale_x = meter.shape_scale_y = 1.0;
    meter.shape_pivot_x = meter.shape_pivot_y = 0.0;
    meter.shape_rotation = 0.0;
    auto read_gradient = [&](const std::string& name, bool radial) {
      const auto definition = ExpandMeasures(config.Get(meter.name, name));
      std::istringstream entries(definition);
      std::string entry;
      bool header = true;
      while (std::getline(entries, entry, '|')) {
        entry = Trim(entry);
        if (header) {
          header = false;
          for (const auto& value : SplitTopLevel(entry, ',')) {
            try { meter.shape_gradient_geometry.push_back(Formula(Trim(value))); } catch (...) { break; }
          }
          continue;
        }
        const auto semicolon = entry.rfind(';');
        if (semicolon == std::string::npos) continue;
        try {
          const auto color = ParseColor(Trim(entry.substr(0, semicolon)), meter.shape_fill);
          const auto stop = std::clamp(Formula(Trim(entry.substr(semicolon + 1))), 0.0, 1.0);
          meter.shape_gradient.emplace_back(stop, color);
        } catch (...) {
        }
      }
      std::sort(meter.shape_gradient.begin(), meter.shape_gradient.end(),
                [](const auto& left, const auto& right) { return left.first < right.first; });
      meter.shape_radial_gradient = radial && !meter.shape_gradient.empty();
    };
    std::vector<std::string> expanded_parts;
    std::istringstream source_parts(expression);
    std::string part;
    while (std::getline(source_parts, part, '|')) {
      part = Trim(part);
      const auto lower = Lower(part);
      if (lower.rfind("extend ", 0) == 0) {
        const auto extension = ExpandMeasures(config.Get(meter.name, Trim(part.substr(7))));
        std::istringstream extension_parts(extension);
        std::string extension_part;
        while (std::getline(extension_parts, extension_part, '|')) {
          expanded_parts.push_back(Trim(extension_part));
        }
      } else {
        expanded_parts.push_back(part);
      }
    }
    for (auto& part : expanded_parts) {
      part = Trim(part);
      const auto lower = Lower(part);
      if (lower.rfind("rectangle ", 0) == 0) {
        std::vector<double> rectangle;
        for (const auto& value : SplitTopLevel(part.substr(10), ',')) {
          try { rectangle.push_back(Formula(Trim(value))); } catch (...) { break; }
        }
        if (rectangle.size() >= 4) {
          meter.shape_rect = {static_cast<int>(rectangle[0]), static_cast<int>(rectangle[1]),
                              static_cast<int>(rectangle[2]), static_cast<int>(rectangle[3])};
          if (rectangle.size() >= 5) meter.shape_corner_radius = std::max(0.0, rectangle[4]);
          if (!meter.width_defined) meter.rect.width = std::max(0, meter.shape_rect.x + meter.shape_rect.width);
          if (!meter.height_defined) meter.rect.height = std::max(0, meter.shape_rect.y + meter.shape_rect.height);
        }
      } else if (lower.rfind("ellipse ", 0) == 0) {
        std::vector<double> ellipse;
        for (const auto& value : SplitTopLevel(part.substr(8), ',')) {
          try { ellipse.push_back(Formula(Trim(value))); } catch (...) { break; }
        }
        if (ellipse.size() >= 3) {
          const double ry = ellipse.size() >= 4 ? ellipse[3] : ellipse[2];
          meter.shape_kind = Meter::ShapeKind::Ellipse;
          meter.shape_rect = {static_cast<int>(ellipse[0] - ellipse[2]),
                              static_cast<int>(ellipse[1] - ry),
                              static_cast<int>(ellipse[2] * 2.0),
                              static_cast<int>(ry * 2.0)};
          if (!meter.width_defined) meter.rect.width = std::max(0, meter.shape_rect.x + meter.shape_rect.width);
          if (!meter.height_defined) meter.rect.height = std::max(0, meter.shape_rect.y + meter.shape_rect.height);
        }
      } else if (lower.rfind("fill color ", 0) == 0) {
        meter.shape_fill = ParseColor(Trim(part.substr(11)), meter.shape_fill);
      } else if (lower.rfind("fill radialgradient", 0) == 0) {
        size_t start = std::string("fill radialgradient").size();
        while (start < part.size() && (std::isdigit(static_cast<unsigned char>(part[start])) ||
                                      std::isspace(static_cast<unsigned char>(part[start])))) ++start;
        read_gradient(Trim(part.substr(start)), true);
      } else if (lower.rfind("fill lineargradient", 0) == 0) {
        size_t start = std::string("fill lineargradient").size();
        while (start < part.size() && (std::isdigit(static_cast<unsigned char>(part[start])) ||
                                      std::isspace(static_cast<unsigned char>(part[start])))) ++start;
        read_gradient(Trim(part.substr(start)), false);
      } else if (lower.rfind("stroke color ", 0) == 0 || lower.rfind("strokecolor ", 0) == 0) {
        const auto space = part.find(' ');
        meter.shape_stroke = ParseColor(Trim(part.substr(space + 1)), meter.shape_stroke);
      } else if (lower.rfind("strokewidth ", 0) == 0) {
        try { meter.shape_stroke_width = Formula(Trim(part.substr(12))); } catch (...) {}
      } else if (lower.rfind("strokedashes ", 0) == 0 || lower.rfind("stroke dashes ", 0) == 0) {
        const auto space = part.find(' ');
        for (const auto& value : SplitTopLevel(part.substr(space + 1), ',')) {
          try { meter.shape_dashes.push_back(std::max(0.0, Formula(Trim(value)))); } catch (...) { break; }
        }
      } else if (lower.rfind("strokedashcap ", 0) == 0 || lower.rfind("stroke dashcap ", 0) == 0) {
        if (lower.find("round") != std::string::npos) meter.shape_line_cap = CAIRO_LINE_CAP_ROUND;
        else if (lower.find("square") != std::string::npos) meter.shape_line_cap = CAIRO_LINE_CAP_SQUARE;
      } else if (lower.rfind("scale ", 0) == 0) {
        std::istringstream values(part.substr(6));
        std::string value;
        std::array<double, 4> scale{1.0, 1.0, 0.0, 0.0};
        size_t index = 0;
        while (index < scale.size() && std::getline(values, value, ',')) {
          try { scale[index++] = Formula(Trim(value)); } catch (...) { break; }
        }
        if (index >= 2) {
          meter.shape_scale_x = scale[0];
          meter.shape_scale_y = scale[1];
          if (index >= 4) {
            meter.shape_pivot_x = scale[2];
            meter.shape_pivot_y = scale[3];
          }
        }
      } else if (lower.rfind("rotate ", 0) == 0) {
        const auto values = SplitTopLevel(part.substr(7), ',');
        try {
          meter.shape_rotation = Formula(Trim(values[0])) * 3.14159265358979323846 / 180.0;
          if (values.size() >= 3) {
            meter.shape_pivot_x = Formula(Trim(values[1]));
            meter.shape_pivot_y = Formula(Trim(values[2]));
          }
        } catch (...) {
        }
      }
    }
  }

  void UpdateMeter(const std::string& name, size_t history_steps = 1,
                   double elapsed_seconds = 0.0) {
    auto* meter = FindMeter(name);
    if (!meter) throw std::runtime_error("Unknown meter: " + name);
    config.SetBuiltin("CURRENTSECTION", meter->name);
    if (meter->dynamic) {
      static constexpr const char* options[] = {
          "X", "Y", "W", "H", "Hidden", "Group", "SolidColor", "SolidColor2",
          "GradientAngle", "TransformationMatrix"};
      for (const char* option : options) {
        const auto value = config.Get(meter->name, option);
        if (!value.empty()) ApplyMeterOption(*meter, option, value);
      }
    }
    const auto* measure = meter->measure.empty() ? nullptr : FindMeasure(meter->measure);
    if (!meter->measure.empty() && !measure) {
      EventLog("diagnostic", "invalid MeasureName " + meter->measure + " in [" + meter->name + "]");
    }
    if (Lower(meter->type) == "shape") {
      UpdateShape(*meter);
    } else if (Lower(meter->type) == "bitmap") {
      if (measure) {
        const int offset = meter->bitmap_zero_frame ? 0 : -1;
        const int frame = static_cast<int>(std::floor(measure->value)) + offset;
        meter->normalized = static_cast<double>(std::clamp(frame, 0, meter->bitmap_frames - 1));
      }
    } else if (Lower(meter->type) == "bar") {
      if (!measure) {
        EventLog("diagnostic", "Missing MeasureName in [" + meter->name + "]");
        return;
      }
      const double range = measure->maximum - measure->minimum;
      meter->normalized = range == 0.0 ? 1.0 :
          std::clamp((measure->value - measure->minimum) / range, 0.0, 1.0);
    } else if (Lower(meter->type) == "roundline" || Lower(meter->type) == "rotator") {
      if (measure) {
        if (meter->value_remainder > 0) {
          meter->normalized = static_cast<double>(static_cast<int64_t>(measure->value) % meter->value_remainder) /
                              meter->value_remainder;
        } else {
          const double range = measure->maximum - measure->minimum;
          meter->normalized = range == 0.0 ? 0.0 :
              std::clamp((measure->value - measure->minimum) / range, 0.0, 1.0);
        }
      } else {
        meter->normalized = 1.0;
      }
    } else if (Lower(meter->type) == "line" || Lower(meter->type) == "histogram") {
      if (measure && !meter->history.empty()) {
        if (!meter->graph_value_ready || smoothing_ms <= 0.0 || elapsed_seconds <= 0.0) {
          meter->graph_value = measure->value;
          meter->graph_value_ready = true;
        } else {
          const double alpha = 1.0 - std::exp(-elapsed_seconds * 1000.0 / smoothing_ms);
          meter->graph_value += (measure->value - meter->graph_value) * alpha;
        }
      }
      if (measure && !meter->history.empty() && history_steps > 0) {
        const double previous = meter->history.back();
        const auto interpolated = [&](size_t step) {
          const double ratio = static_cast<double>(step) / static_cast<double>(history_steps);
          return previous + (meter->graph_value - previous) * ratio;
        };
        if (history_steps >= meter->history.size()) {
          const size_t first = history_steps - meter->history.size() + 1;
          for (size_t index = 0; index < meter->history.size(); ++index) {
            meter->history[index] = interpolated(first + index);
          }
        } else {
          for (size_t step = 1; step <= history_steps; ++step) {
            meter->history.pop_front();
            meter->history.push_back(interpolated(step));
          }
        }
      }
    } else if (Lower(meter->type) == "image" || Lower(meter->type) == "button" ||
               Lower(meter->type) == "rotator" || Lower(meter->type) == "bitmap") {
      const auto meter_type = Lower(meter->type);
      const std::string image_option = meter_type == "button" ? "ButtonImage" :
          meter_type == "bitmap" ? "BitmapImage" : "ImageName";
      const auto raw_image_name = config.Get(meter->name, image_option, meter->image_name);
      auto image_name = ExpandMeasures(raw_image_name);
      if (image_name.empty() && measure && measure->plugin_string) image_name = measure->string_value;
      if (measure) {
        for (size_t position = 0; (position = image_name.find("%1", position)) != std::string::npos;) {
          image_name.replace(position, 2, measure->string_value);
          position += measure->string_value.size();
        }
      }
      if (image_name.rfind("file://", 0) == 0) {
        char* decoded = g_uri_unescape_string(image_name.c_str() + 7, nullptr);
        if (decoded) {
          image_name = decoded;
          g_free(decoded);
        }
      }
      char* numeric_end = nullptr;
      std::strtod(image_name.c_str(), &numeric_end);
      if (raw_image_name.find('[') != std::string::npos && numeric_end != image_name.c_str() &&
          numeric_end && *numeric_end == '\0') {
        image_name.clear();
      }
      if (!image_name.empty() && image_name.find('[') == std::string::npos &&
          image_name != meter->image_name) {
        try {
          const auto image_path = config.ResolveImagePath(image_name);
          std::string image_error;
          cairo_surface_t* replacement = LoadImage(image_path, image_error,
                                                    meter->image_tint, meter->image_opacity);
          if (replacement) {
            if (meter->image) cairo_surface_destroy(meter->image);
            meter->image = replacement;
            meter->image_name = image_name;
            meter->image_source_width = cairo_image_surface_get_width(replacement);
            meter->image_source_height = cairo_image_surface_get_height(replacement);
            if (meter_type == "button") {
              if (meter->image_source_height > meter->image_source_width) meter->image_source_height /= 3.0;
              else meter->image_source_width /= 3.0;
            } else if (meter_type == "bitmap") {
              if (meter->image_source_height >= meter->image_source_width) meter->image_source_height /= meter->bitmap_frames;
              else meter->image_source_width /= meter->bitmap_frames;
            }
            if (meter_type != "rotator" && !meter->width_defined && !meter->height_defined) {
              meter->rect.width = static_cast<int>(meter->image_source_width);
              meter->rect.height = static_cast<int>(meter->image_source_height);
            } else if (meter_type != "rotator" && !meter->width_defined && meter->image_source_height > 0.0) {
              meter->rect.width = static_cast<int>(meter->rect.height *
                  meter->image_source_width / meter->image_source_height);
            } else if (meter_type != "rotator" && !meter->height_defined && meter->image_source_width > 0.0) {
              meter->rect.height = static_cast<int>(meter->rect.width *
                  meter->image_source_height / meter->image_source_width);
            }
          } else {
            EventLog("diagnostic", "unable to reload " + image_option + " in [" + meter->name + "]: " + image_error);
          }
        } catch (const std::exception& exception) {
          EventLog("diagnostic", "unable to reload " + image_option + " in [" + meter->name + "]: " + exception.what());
        }
      }
    } else if (Lower(meter->type) == "string") {
      auto text = config.Get(meter->name, "Text");
      if (text.empty() && measure) text = "%1";
      for (size_t index = 0; index < meter->measure_names.size(); ++index) {
        const auto* bound = FindMeasure(meter->measure_names[index]);
        if (!bound) continue;
        std::string formatted = bound->string_value;
        if (Lower(bound->type) != "time" && Lower(bound->type) != "string" &&
            !bound->plugin_string) {
          double value = bound->value;
          if (meter->percentual) {
            const double range = bound->maximum - bound->minimum;
            value = range == 0.0 ? 0.0 : (value - bound->minimum) * 100.0 / range;
          }
          if (meter->scale != 0.0) value /= meter->scale;
          std::string unit;
          if (meter->autoscale) {
            static const char* units[] = {"", "k", "M", "G", "T", "P"};
            size_t unit_index = 0;
            while (std::abs(value) >= 1024.0 && unit_index + 1 < std::size(units)) {
              value /= 1024.0;
              ++unit_index;
            }
            unit = units[unit_index];
          }
          if (meter->decimals >= 0) {
            std::ostringstream output;
            output << std::fixed << std::setprecision(meter->decimals) << value;
            formatted = output.str();
          } else {
            formatted = Number(value);
          }
          formatted += unit;
          formatted = Substitute(formatted, config.Get(bound->name, "Substitute"));
        }
        formatted = meter->prefix + formatted + meter->postfix;
        const auto marker = "%" + std::to_string(index + 1);
        for (size_t pos = 0; (pos = text.find(marker, pos)) != std::string::npos;) {
          text.replace(pos, marker.size(), formatted);
          pos += formatted.size();
        }
      }
      if (meter->string_case == "upper") {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return std::toupper(ch); });
      } else if (meter->string_case == "lower") {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return std::tolower(ch); });
      }
      meter->text = text;
      PangoContext* context = pango_font_map_create_context(pango_cairo_font_map_get_default());
      PangoLayout* layout = pango_layout_new(context);
      PangoFontDescription* font = pango_font_description_new();
      pango_font_description_set_family(font, meter->font_face.c_str());
      pango_font_description_set_size(font, static_cast<int>(meter->font_size * PANGO_SCALE));
      if (meter->string_style.find("bold") != std::string::npos) {
        pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
      }
      if (meter->string_style.find("italic") != std::string::npos) {
        pango_font_description_set_style(font, PANGO_STYLE_ITALIC);
      }
      pango_layout_set_font_description(layout, font);
      pango_layout_set_text(layout, meter->text.c_str(), -1);
      int text_width = 0;
      int text_height = 0;
      pango_layout_get_pixel_size(layout, &text_width, &text_height);
      if (!meter->width_defined) meter->rect.width = text_width;
      if (!meter->height_defined) meter->rect.height = text_height;
      meter->rect.x = meter->anchor_x;
      if (meter->align.rfind("center", 0) == 0) meter->rect.x -= meter->rect.width / 2;
      if (meter->align.rfind("right", 0) == 0) meter->rect.x -= meter->rect.width;
      pango_font_description_free(font);
      g_object_unref(layout);
      g_object_unref(context);
    }
    EventLog("UpdateMeter", meter->name + "=" +
        (Lower(meter->type) == "string" ? meter->text : std::to_string(meter->normalized)));
  }

  static bool InGroup(const std::string& groups, const std::string& wanted) {
    std::istringstream input(groups);
    std::string group;
    while (std::getline(input, group, '|')) if (Lower(Trim(group)) == Lower(wanted)) return true;
    return false;
  }

  void ApplyMeterOption(Meter& meter, const std::string& option, const std::string& value) {
    const auto name = Lower(option);
    try {
      auto position = [&](bool horizontal) {
        auto expanded = ExpandMeasures(config.Expand(value));
        char relative = '\0';
        if (!expanded.empty() && (expanded.back() == 'r' || expanded.back() == 'R')) {
          relative = expanded.back();
          expanded.pop_back();
        }
        int result = Integer(expanded);
        if (relative) {
          const auto current = std::find_if(meters.begin(), meters.end(),
              [&](const Meter& candidate) { return &candidate == &meter; });
          if (current != meters.begin() && current != meters.end()) {
            const auto& previous = *std::prev(current);
            result += horizontal ? previous.anchor_x : previous.anchor_y;
            if (relative == 'R') result += horizontal ? previous.rect.width : previous.rect.height;
          }
        }
        return result;
      };
      if (name == "x") meter.rect.x = meter.anchor_x = position(true);
      else if (name == "y") meter.rect.y = meter.anchor_y = position(false);
      else if (name == "w") meter.rect.width = Integer(ExpandMeasures(config.Expand(value)));
      else if (name == "h") meter.rect.height = Integer(ExpandMeasures(config.Expand(value)));
      else if (name == "solidcolor") {
        meter.solid_color = ParseColor(ExpandMeasures(config.Expand(value)), meter.solid_color);
        if (!meter.solid_color2_defined) meter.solid_color2 = meter.solid_color;
      } else if (name == "solidcolor2") {
        const auto expanded = ExpandMeasures(config.Expand(value));
        meter.solid_color2_defined = !expanded.empty();
        meter.solid_color2 = meter.solid_color2_defined ?
            ParseColor(expanded, meter.solid_color) : meter.solid_color;
      } else if (name == "gradientangle") {
        meter.gradient_angle = Formula(ExpandMeasures(config.Expand(value)));
      } else if (name == "transformationmatrix") {
        const auto expanded = ExpandMeasures(config.Expand(value));
        if (!expanded.empty()) {
          std::istringstream input(expanded);
          std::string component;
          std::array<double, 6> parsed{};
          size_t index = 0;
          while (index < parsed.size() && std::getline(input, component, ';')) {
            parsed[index++] = Formula(Trim(component));
          }
          if (index == parsed.size() && !std::getline(input, component, ';')) {
            meter.transformation = parsed;
            meter.transformed = true;
          } else {
            EventLog("diagnostic", "TransformationMatrix requires six values in [" + meter.name + "]");
          }
        }
      } else if (name == "group") {
        meter.group = config.Expand(value);
      } else if (name == "hidden") {
        meter.hidden = Integer(ExpandMeasures(config.Expand(value))) != 0;
      }
    } catch (const std::exception& error) {
      EventLog("diagnostic", "unable to apply " + option + " to [" + meter.name + "]: " + error.what());
    }
  }

  void CommandPlugin(Measure& measure, const std::string& command) {
    const auto lower = Lower(command);
    if (measure.plugin == "nowplaying" || measure.plugin == "itunes" || measure.plugin == "mediakey") {
      static const std::unordered_map<std::string, std::string> methods = {
          {"play", "Play"}, {"pause", "Pause"}, {"playpause", "PlayPause"}, {"stop", "Stop"},
          {"next", "Next"}, {"nexttrack", "Next"}, {"previous", "Previous"},
          {"previoustrack", "Previous"}};
      const auto method = methods.find(lower);
      if (method == methods.end()) return PluginUnavailable(measure, "unsupported media command " + command);
      GError* error = nullptr;
      GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
      if (!bus) {
        const std::string message = error ? error->message : "session D-Bus unavailable";
        if (error) g_error_free(error);
        return PluginUnavailable(measure, message);
      }
      GVariant* names_reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
          "org.freedesktop.DBus", "ListNames", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE,
          1000, nullptr, &error);
      std::string player;
      if (names_reply) {
        GVariant* names = g_variant_get_child_value(names_reply, 0);
        GVariantIter iterator;
        const gchar* name = nullptr;
        g_variant_iter_init(&iterator, names);
        while (g_variant_iter_next(&iterator, "&s", &name)) {
          if (std::string(name).rfind("org.mpris.MediaPlayer2.", 0) == 0) { player = name; break; }
        }
        g_variant_unref(names);
        g_variant_unref(names_reply);
      }
      if (player.empty()) {
        g_object_unref(bus);
        return PluginUnavailable(measure, "no MPRIS media player is running");
      }
      GVariant* reply = g_dbus_connection_call_sync(bus, player.c_str(), "/org/mpris/MediaPlayer2",
          "org.mpris.MediaPlayer2.Player", method->second.c_str(), nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE,
          1000, nullptr, &error);
      g_object_unref(bus);
      if (!reply) {
        const std::string message = error ? error->message : "MPRIS command failed";
        if (error) g_error_free(error);
        return PluginUnavailable(measure, message);
      }
      g_variant_unref(reply);
      EventLog("CommandMeasure", measure.name + " " + command);
      return;
    }
    if (measure.plugin == "actiontimer" && lower.rfind("execute", 0) == 0) {
      std::string suffix = Trim(command.substr(7));
      if (suffix.empty()) suffix = "1";
      const auto list = config.Get(measure.name, "ActionList" + suffix);
      if (list.empty()) return PluginUnavailable(measure, "ActionList" + suffix + " is empty");
      auto due = std::chrono::steady_clock::now();
      std::istringstream stream(list);
      std::string item;
      while (std::getline(stream, item, '|')) {
        item = Trim(item);
        if (Lower(item).rfind("wait ", 0) == 0) {
          due += std::chrono::milliseconds(std::max(0, Integer(Trim(item.substr(5)))));
          continue;
        }
        const auto action = config.Get(measure.name, item, item);
        if (!action.empty()) scheduled_actions.push_back({due, action});
      }
      EventLog("CommandMeasure", measure.name + " " + command);
      return;
    }
    if (measure.plugin == "inputtext" && lower.rfind("executebatch", 0) == 0) {
      std::string suffix = Trim(command.substr(12));
      if (suffix.empty()) suffix = "1";
      const auto input = RunCapture({"zenity", "--entry", "--title=" + config.Get(measure.name, "InputTitle", "Rainmeter"),
                                     "--text=" + config.Get(measure.name, "Prompt", ""),
                                     "--entry-text=" + config.Get(measure.name, "DefaultValue", "")}, 300000);
      if (input.status != 0) return PluginUnavailable(measure, input.timed_out ? "input timed out" : "input cancelled");
      const auto value = Trim(input.output);
      SetPluginValue(measure, ToNumber(value), value, true);
      std::string action = config.Get(measure.name, "Command" + suffix);
      for (size_t position = 0; (position = action.find("$UserInput$", position)) != std::string::npos;) {
        action.replace(position, 11, value);
        position += value.size();
      }
      if (!action.empty()) Execute(action);
      EventLog("CommandMeasure", measure.name + " " + command);
      return;
    }
    if (measure.plugin == "win7audio") {
      std::vector<std::string> arguments{"pactl"};
      if (lower == "togglemute" || lower == "toggle mute") {
        arguments.insert(arguments.end(), {"set-sink-mute", "@DEFAULT_SINK@", "toggle"});
      } else if (lower.rfind("changevolume ", 0) == 0 || lower.rfind("setvolume ", 0) == 0) {
        const bool relative = lower.rfind("changevolume ", 0) == 0;
        std::string value = Trim(command.substr(command.find(' ') + 1));
        if (relative && !value.empty() && value.front() != '+' && value.front() != '-') value.insert(0, "+");
        if (value.find('%') == std::string::npos) value += '%';
        arguments.insert(arguments.end(), {"set-sink-volume", "@DEFAULT_SINK@", value});
      } else {
        return PluginUnavailable(measure, "unsupported audio command " + command);
      }
      const auto result = RunCapture(arguments, 2000);
      if (result.status != 0) return PluginUnavailable(measure, Trim(result.output));
      EventLog("CommandMeasure", measure.name + " " + command);
      return;
    }
    if (measure.plugin == "runcommand" && lower == "run") {
      if (!std::getenv("RM_ALLOW_COMMANDS") || std::string(std::getenv("RM_ALLOW_COMMANDS")) != "1") {
        return PluginUnavailable(measure, "external commands require RM_ALLOW_COMMANDS=1");
      }
      const auto program = config.Get(measure.name, "Program");
      const auto parameters = config.Get(measure.name, "Parameter");
      if (program.empty()) return PluginUnavailable(measure, "Program is empty");
      const int timeout = std::max(1, Integer(config.Get(measure.name, "Timeout", "10000")));
      const auto result = RunCapture({"/bin/sh", "-c", program + (parameters.empty() ? "" : " " + parameters)}, timeout);
      SetPluginValue(measure, result.status, Trim(result.output), true);
      EventLog("CommandMeasure", measure.name + " Run status=" + std::to_string(result.status));
      return;
    }
    PluginUnavailable(measure, "plugin command is not implemented: " + command);
  }

  void Execute(std::string action, const std::string& current_section = {}) {
    for (const auto& raw : SplitCommands(action)) {
      config.SetBuiltin("CURRENTSECTION", current_section);
      auto command = ExpandMeasures(config.Expand(raw));
      auto tokens = Tokens(command);
      if (tokens.empty()) continue;
      auto bang = tokens.front();
      if (!bang.empty() && bang.front() == '!') bang.erase(0, 1);
      auto name = Lower(bang);
      if (name.rfind("rainmeter", 0) == 0) name.erase(0, 9);
      if (name == "setoption" && tokens.size() >= 4) {
        config.Set(tokens[1], tokens[2], tokens[3]);
        if (auto* meter = FindMeter(tokens[1])) ApplyMeterOption(*meter, tokens[2], tokens[3]);
        EventLog("SetOption", tokens[1] + "." + tokens[2] + "=" + tokens[3]);
      } else if (name == "setoptiongroup" && tokens.size() >= 4) {
        for (auto& measure : measures) {
          if (InGroup(measure.group, tokens[1])) config.Set(measure.name, tokens[2], tokens[3]);
        }
        for (auto& meter : meters) {
          if (!InGroup(meter.group, tokens[1])) continue;
          config.Set(meter.name, tokens[2], tokens[3]);
          ApplyMeterOption(meter, tokens[2], tokens[3]);
        }
        EventLog("SetOptionGroup", tokens[1] + "." + tokens[2] + "=" + tokens[3]);
      } else if (name == "updatemeasure" && tokens.size() >= 2) {
        if (tokens[1] == "*") {
          for (auto& measure : measures) if (!measure.disabled) UpdateMeasure(measure.name);
        } else {
          UpdateMeasure(tokens[1]);
        }
      } else if (name == "updatemeasuregroup" && tokens.size() >= 2) {
        for (auto& measure : measures) if (InGroup(measure.group, tokens[1]) && !measure.disabled) {
          UpdateMeasure(measure.name);
        }
      } else if (name == "updatemeter" && tokens.size() >= 2) {
        if (tokens[1] == "*") {
          for (auto& meter : meters) UpdateMeter(meter.name);
        } else {
          UpdateMeter(tokens[1]);
        }
      } else if (name == "updatemetergroup" && tokens.size() >= 2) {
        for (auto& meter : meters) if (InGroup(meter.group, tokens[1])) UpdateMeter(meter.name);
      } else if ((name == "enablemeasure" || name == "disablemeasure") && tokens.size() >= 2) {
        if (auto* measure = FindMeasure(tokens[1])) measure->disabled = name == "disablemeasure";
      } else if ((name == "enablemeasuregroup" || name == "disablemeasuregroup") && tokens.size() >= 2) {
        for (auto& measure : measures) if (InGroup(measure.group, tokens[1])) {
          measure.disabled = name == "disablemeasuregroup";
        }
      } else if (name == "commandmeasure" && tokens.size() >= 3) {
        if (auto* measure = FindMeasure(tokens[1]); measure && Lower(measure->type) == "plugin") {
          CommandPlugin(*measure, tokens[2]);
        } else if (auto* measure = FindMeasure(tokens[1]); measure && Lower(measure->type) == "loop" &&
                   Lower(tokens[2]) == "reset") {
          measure->loop_started = false;
          measure->loop_at_end = false;
          measure->loops_completed = 0;
          EventLog("CommandMeasure", measure->name + " Reset");
        } else {
          EventLog("diagnostic", "CommandMeasure target does not support commands: " + tokens[1]);
        }
      } else if (name == "pluginbang" && tokens.size() >= 2) {
        auto parameters = Tokens(tokens[1]);
        if (parameters.size() < 2 && tokens.size() >= 3) parameters = {tokens[1], tokens[2]};
        if (parameters.size() >= 2) {
          if (auto* measure = FindMeasure(parameters[0]); measure && Lower(measure->type) == "plugin") {
            CommandPlugin(*measure, parameters[1]);
          } else {
            EventLog("diagnostic", "PluginBang target is not a plugin measure: " + parameters[0]);
          }
        }
      } else if (name == "redraw") {
        EventLog("Redraw", "requested");
      } else if ((name == "showmeter" || name == "rainmetershowmeter") && tokens.size() >= 2) {
        if (auto* meter = FindMeter(tokens[1])) {
          meter->hidden = false;
          EventLog("ShowMeter", meter->name);
        } else {
          EventLog("diagnostic", "unknown meter " + tokens[1]);
        }
      } else if ((name == "hidemeter" || name == "rainmeterhidemeter") && tokens.size() >= 2) {
        if (auto* meter = FindMeter(tokens[1])) {
          meter->hidden = true;
          EventLog("HideMeter", meter->name);
        } else {
          EventLog("diagnostic", "unknown meter " + tokens[1]);
        }
      } else if (name == "togglemeter" && tokens.size() >= 2) {
        if (auto* meter = FindMeter(tokens[1])) {
          meter->hidden = !meter->hidden;
          EventLog("ToggleMeter", meter->name);
        } else {
          EventLog("diagnostic", "unknown meter " + tokens[1]);
        }
      } else if ((name == "showmetergroup" || name == "hidemetergroup" ||
                  name == "togglemetergroup") && tokens.size() >= 2) {
        for (auto& meter : meters) {
          if (!InGroup(meter.group, tokens[1])) continue;
          if (name == "showmetergroup") meter.hidden = false;
          else if (name == "hidemetergroup") meter.hidden = true;
          else meter.hidden = !meter.hidden;
        }
        EventLog(name, tokens[1]);
      } else if (name == "setvariable" && tokens.size() >= 3) {
        config.SetVariable(tokens[1], tokens[2]);
        EventLog("SetVariable", tokens[1] + "=" + tokens[2]);
      } else if ((name == "activateconfig" || name == "load") && tokens.size() >= 2) {
        host_actions.push_back({"activate", tokens[1], tokens.size() >= 3 ? tokens[2] : ""});
        EventLog("HostAction", "activate " + tokens[1] +
            (tokens.size() >= 3 ? " " + tokens[2] : ""));
      } else if (name == "toggleconfig" && tokens.size() >= 2) {
        host_actions.push_back({"toggle", tokens[1], tokens.size() >= 3 ? tokens[2] : ""});
        EventLog("HostAction", "toggle " + tokens[1]);
      } else if (name == "deactivateconfig" || name == "unload") {
        host_actions.push_back({tokens.size() >= 2 ? "deactivate" : "deactivate-current",
                                tokens.size() >= 2 ? tokens[1] : "", ""});
        EventLog("HostAction", tokens.size() >= 2 ? "deactivate " + tokens[1] : "deactivate-current");
      } else if (name == "refresh") {
        host_actions.push_back({tokens.size() >= 2 ? "refresh" : "refresh-current",
                                tokens.size() >= 2 ? tokens[1] : "", ""});
        EventLog("HostAction", tokens.size() >= 2 ? "refresh " + tokens[1] : "refresh-current");
      } else if (name == "skincustommenu") {
        host_actions.push_back({"custom-menu", "", ""});
        EventLog("HostAction", "custom-menu");
      } else if (name == "skinmenu") {
        host_actions.push_back({"skin-menu", "", ""});
        EventLog("HostAction", "skin-menu");
      } else if (name == "keeponscreen" || name == "draggable" || name == "clickthrough" ||
                 name == "snapedges" || name == "saveposition" || name == "zpos" ||
                 name == "settransparency") {
        EventLog("WindowBang", bang);
      } else if (name == "execute") {
        const auto nested = command.find('[');
        if (nested != std::string::npos) {
          Execute(command.substr(nested), current_section);
        } else {
          EventLog("diagnostic", "external !Execute blocked by unimplemented process service");
        }
      } else if (name == "log" && tokens.size() >= 2) {
        EventLog("Log", tokens[1]);
        std::cout << tokens[1] << '\n';
      } else {
        EventLog("diagnostic", "unsupported bang !" + bang);
      }
    }
  }

  void ProcessTimers() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> due;
    scheduled_actions.erase(std::remove_if(scheduled_actions.begin(), scheduled_actions.end(),
        [&](const ScheduledAction& scheduled) {
          if (scheduled.due > now) return false;
          due.push_back(scheduled.action);
          return true;
        }), scheduled_actions.end());
    for (const auto& action : due) Execute(action);
  }

  std::string Snapshot() const {
    std::ostringstream out;
    out << "{\"measures\":{";
    for (size_t i = 0; i < measures.size(); ++i) {
      if (i) out << ',';
      out << Json(measures[i].name) << ":{\"number\":" << std::setprecision(15) << measures[i].value
          << ",\"string\":" << Json(measures[i].string_value) << '}';
    }
    out << "},\"meters\":{";
    for (size_t i = 0; i < meters.size(); ++i) {
      if (i) out << ',';
      const auto& meter = meters[i];
      out << Json(meter.name) << ":{\"rect\":[" << meter.rect.x << ',' << meter.rect.y << ','
          << meter.rect.width << ',' << meter.rect.height << "],\"normalizedValue\":"
          << std::setprecision(15) << meter.normalized << ",\"hidden\":"
          << (meter.hidden ? "true" : "false");
      if (Lower(meter.type) == "string") out << ",\"text\":" << Json(meter.text);
      out << '}';
    }
    out << "}}";
    return out.str();
  }
};

Runtime::Runtime(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Runtime::~Runtime() = default;

std::unique_ptr<Runtime> Runtime::Load(const std::filesystem::path& skin, std::string& error) {
  try {
    if (!FcInit()) throw std::runtime_error("Unable to initialize Fontconfig");
    auto impl = std::make_unique<Impl>(skin);
    impl->Load();
    impl->initial_snapshot = impl->Snapshot();
    return std::unique_ptr<Runtime>(new Runtime(std::move(impl)));
  } catch (const std::exception& exception) {
    error = exception.what();
    return nullptr;
  }
}

double Runtime::MeasureValue(const std::string& name) const {
  const auto* measure = impl_->FindMeasure(name);
  if (!measure) throw std::runtime_error("Unknown measure: " + name);
  return measure->value;
}

std::string Runtime::MeasureString(const std::string& name) const {
  const auto* measure = impl_->FindMeasure(name);
  if (!measure) throw std::runtime_error("Unknown measure: " + name);
  return measure->string_value;
}

double Runtime::MeterNormalizedValue(const std::string& name) const {
  const auto* meter = impl_->FindMeter(name);
  if (!meter) throw std::runtime_error("Unknown meter: " + name);
  return meter->normalized;
}

Rect Runtime::MeterRect(const std::string& name) const {
  const auto* meter = impl_->FindMeter(name);
  if (!meter) throw std::runtime_error("Unknown meter: " + name);
  return meter->rect;
}

bool Runtime::MeterVisible(const std::string& name) const {
  const auto* meter = impl_->FindMeter(name);
  if (!meter) throw std::runtime_error("Unknown meter: " + name);
  return !meter->hidden;
}

int Runtime::Width() const {
  int width = std::max(1, impl_->background_margins.x);
  for (const auto& meter : impl_->meters) width = std::max(width, meter.rect.x + meter.rect.width);
  width += impl_->background_margins.width;
  if (impl_->background) {
    const int image_width = cairo_image_surface_get_width(impl_->background);
    if (impl_->background_mode == 0) return image_width;
    width = std::max(width, image_width);
  }
  return width;
}

int Runtime::Height() const {
  int height = std::max(1, impl_->background_margins.y);
  for (const auto& meter : impl_->meters) height = std::max(height, meter.rect.y + meter.rect.height);
  height += impl_->background_margins.height;
  if (impl_->background) {
    const int image_height = cairo_image_surface_get_height(impl_->background);
    if (impl_->background_mode == 0) return image_height;
    height = std::max(height, image_height);
  }
  return height;
}

int Runtime::UpdateInterval() const { return impl_->update_interval; }

double Runtime::FrameRate() const { return impl_->frame_rate; }

void Runtime::Update() {
  const auto now = std::chrono::steady_clock::now();
  const double elapsed_seconds = std::max(0.0,
      std::chrono::duration<double>(now - impl_->scroll_updated_at).count());
  impl_->scroll_updated_at = now;
  size_t history_steps = 1;
  if (impl_->scroll_speed > 0.0) {
    const double pending = impl_->scroll_remainder +
        elapsed_seconds * impl_->scroll_speed;
    history_steps = static_cast<size_t>(std::max(0.0, std::floor(pending)));
    impl_->scroll_remainder = pending - static_cast<double>(history_steps);
  }
  impl_->ProcessTimers();
  impl_->web_cycle_cache.clear();
  impl_->web_cycle_errors.clear();
  impl_->audio_capture_ready = false;
  impl_->audio_analysis_parent.clear();
  ++impl_->update_counter;
  for (auto& measure : impl_->measures) {
    const int divider = Integer(impl_->config.Get(measure.name, "UpdateDivider",
                                std::to_string(impl_->default_update_divider)),
                                impl_->default_update_divider);
    if (!measure.disabled && divider > 0 && impl_->update_counter % divider == 0) {
      impl_->UpdateMeasure(measure.name);
    }
  }
  for (auto& meter : impl_->meters) {
    const int divider = Integer(impl_->config.Get(meter.name, "UpdateDivider",
                                std::to_string(impl_->default_update_divider)),
                                impl_->default_update_divider);
    if (divider > 0 && impl_->update_counter % divider == 0) {
      impl_->UpdateMeter(meter.name, history_steps, elapsed_seconds);
    }
  }
  impl_->EventLog("lifecycle", "update complete");
  if (!impl_->on_update_action.empty()) impl_->Execute(impl_->on_update_action);
}

void Runtime::Tick() { impl_->ProcessTimers(); }

void Runtime::Click(int x, int y) {
  impl_->EventLog("input", "left-up " + std::to_string(x) + "," + std::to_string(y));
  for (auto meter = impl_->meters.rbegin(); meter != impl_->meters.rend(); ++meter) {
    if (x >= meter->rect.x && x < meter->rect.x + meter->rect.width && y >= meter->rect.y &&
        y < meter->rect.y + meter->rect.height && !meter->left_up_action.empty()) {
      impl_->Execute(meter->left_up_action, meter->name);
      impl_->post_snapshot = impl_->Snapshot();
      return;
    }
  }
}

bool Runtime::RightClick(int x, int y) {
  impl_->EventLog("input", "right-up " + std::to_string(x) + "," + std::to_string(y));
  if (impl_->right_mouse_up_action.empty()) return false;
  impl_->Execute(impl_->right_mouse_up_action);
  return true;
}

bool Runtime::DoubleClick(int x, int y) {
  impl_->EventLog("input", "left-double " + std::to_string(x) + "," + std::to_string(y));
  if (impl_->left_mouse_double_click_action.empty()) return false;
  impl_->Execute(impl_->left_mouse_double_click_action);
  return true;
}

void Runtime::ExecuteAction(const std::string& action) { impl_->Execute(action); }

std::vector<ContextItem> Runtime::ContextItems() const {
  std::vector<ContextItem> result;
  for (int index = 1; index <= 25; ++index) {
    const auto suffix = index == 1 ? std::string{} : std::to_string(index);
    const auto title = impl_->config.Get("Rainmeter", "ContextTitle" + suffix);
    if (title.empty()) continue;
    result.push_back({title, impl_->config.Get("Rainmeter", "ContextAction" + suffix)});
  }
  return result;
}

std::vector<HostAction> Runtime::TakeHostActions() {
  std::vector<HostAction> actions;
  actions.swap(impl_->host_actions);
  return actions;
}

void Runtime::Render(cairo_t* cr) const {
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  if (impl_->background) {
    if (impl_->background_mode == 3) {
      const double source_width = cairo_image_surface_get_width(impl_->background);
      const double source_height = cairo_image_surface_get_height(impl_->background);
      const double destination_width = Width();
      const double destination_height = Height();
      const double left = impl_->background_margins.x;
      const double top = impl_->background_margins.y;
      const double right = impl_->background_margins.width;
      const double bottom = impl_->background_margins.height;
      const double source_middle_width = source_width - left - right;
      const double source_middle_height = source_height - top - bottom;
      const double destination_middle_width = destination_width - left - right;
      const double destination_middle_height = destination_height - top - bottom;
      const double source_x[] = {0, left, source_width - right};
      const double source_y[] = {0, top, source_height - bottom};
      const double source_w[] = {left, source_middle_width, right};
      const double source_h[] = {top, source_middle_height, bottom};
      const double destination_x[] = {0, left, destination_width - right};
      const double destination_y[] = {0, top, destination_height - bottom};
      const double destination_w[] = {left, destination_middle_width, right};
      const double destination_h[] = {top, destination_middle_height, bottom};
      for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
          PaintImageSlice(cr, impl_->background, source_x[column], source_y[row],
                          source_w[column], source_h[row], destination_x[column], destination_y[row],
                          destination_w[column], destination_h[row]);
        }
      }
    } else {
      cairo_set_source_surface(cr, impl_->background, 0, 0);
      if (impl_->background_mode == 4) {
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
      }
      cairo_paint(cr);
    }
  }
  for (const auto& meter : impl_->meters) {
    if (meter.hidden) continue;
    cairo_save(cr);
    if (meter.transformed) {
      const cairo_matrix_t matrix{meter.transformation[0], meter.transformation[1],
                                  meter.transformation[2], meter.transformation[3],
                                  meter.transformation[4], meter.transformation[5]};
      cairo_transform(cr, &matrix);
    }
    const auto type = Lower(meter.type);
    cairo_pattern_t* gradient = nullptr;
    if (meter.solid_color.r != meter.solid_color2.r ||
        meter.solid_color.g != meter.solid_color2.g ||
        meter.solid_color.b != meter.solid_color2.b ||
        meter.solid_color.a != meter.solid_color2.a) {
      const auto start = GradientEdgePoint(meter.gradient_angle + 180.0, meter.rect);
      const auto end = GradientEdgePoint(meter.gradient_angle, meter.rect);
      gradient = cairo_pattern_create_linear(start.first, start.second, end.first, end.second);
      cairo_pattern_add_color_stop_rgba(gradient, 0.0, meter.solid_color.r, meter.solid_color.g,
                                        meter.solid_color.b, meter.solid_color.a);
      cairo_pattern_add_color_stop_rgba(gradient, 1.0, meter.solid_color2.r, meter.solid_color2.g,
                                        meter.solid_color2.b, meter.solid_color2.a);
      cairo_set_source(cr, gradient);
    } else {
      cairo_set_source_rgba(cr, meter.solid_color.r, meter.solid_color.g, meter.solid_color.b,
                            meter.solid_color.a);
    }
    cairo_rectangle(cr, meter.rect.x, meter.rect.y, meter.rect.width, meter.rect.height);
    cairo_fill(cr);
    if (gradient) cairo_pattern_destroy(gradient);
    if ((type == "image" || type == "button") && meter.image) {
      PaintImageSlice(cr, meter.image, 0, 0, meter.image_source_width, meter.image_source_height,
                      meter.rect.x, meter.rect.y, meter.rect.width, meter.rect.height);
    } else if (type == "bitmap" && meter.image) {
      const double frame = meter.normalized;
      const bool vertical = cairo_image_surface_get_height(meter.image) >=
                            cairo_image_surface_get_width(meter.image);
      PaintImageSlice(cr, meter.image,
                      vertical ? 0.0 : frame * meter.image_source_width,
                      vertical ? frame * meter.image_source_height : 0.0,
                      meter.image_source_width, meter.image_source_height,
                      meter.rect.x, meter.rect.y, meter.rect.width, meter.rect.height);
    } else if (type == "shape") {
      cairo_save(cr);
      cairo_translate(cr, meter.rect.x + meter.shape_pivot_x, meter.rect.y + meter.shape_pivot_y);
      cairo_rotate(cr, meter.shape_rotation);
      cairo_scale(cr, meter.shape_scale_x, meter.shape_scale_y);
      cairo_translate(cr, -meter.shape_pivot_x, -meter.shape_pivot_y);
      if (meter.shape_kind == Impl::Meter::ShapeKind::Ellipse) {
        const double center_x = meter.shape_rect.x + meter.shape_rect.width / 2.0;
        const double center_y = meter.shape_rect.y + meter.shape_rect.height / 2.0;
        cairo_save(cr);
        cairo_translate(cr, center_x, center_y);
        cairo_scale(cr, std::max(1, meter.shape_rect.width) / 2.0,
                       std::max(1, meter.shape_rect.height) / 2.0);
        cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * 3.14159265358979323846);
        cairo_restore(cr);
      } else {
        const double radius = std::min(meter.shape_corner_radius,
            std::min(meter.shape_rect.width, meter.shape_rect.height) / 2.0);
        if (radius > 0.0) {
          const double x = meter.shape_rect.x;
          const double y = meter.shape_rect.y;
          const double right = x + meter.shape_rect.width;
          const double bottom = y + meter.shape_rect.height;
          cairo_new_sub_path(cr);
          cairo_arc(cr, right - radius, y + radius, radius, -3.14159265358979323846 / 2.0, 0.0);
          cairo_arc(cr, right - radius, bottom - radius, radius, 0.0, 3.14159265358979323846 / 2.0);
          cairo_arc(cr, x + radius, bottom - radius, radius, 3.14159265358979323846 / 2.0,
                    3.14159265358979323846);
          cairo_arc(cr, x + radius, y + radius, radius, 3.14159265358979323846,
                    3.0 * 3.14159265358979323846 / 2.0);
          cairo_close_path(cr);
        } else {
          cairo_rectangle(cr, meter.shape_rect.x, meter.shape_rect.y,
                          meter.shape_rect.width, meter.shape_rect.height);
        }
      }
      cairo_pattern_t* shape_gradient = nullptr;
      if (!meter.shape_gradient.empty()) {
        const double center_x = meter.shape_rect.x + meter.shape_rect.width / 2.0;
        const double center_y = meter.shape_rect.y + meter.shape_rect.height / 2.0;
        if (meter.shape_radial_gradient && meter.shape_gradient_geometry.size() >= 6) {
          const auto& geometry = meter.shape_gradient_geometry;
          shape_gradient = cairo_pattern_create_radial(geometry[0], geometry[1], geometry[2],
                                                        geometry[3], geometry[4], geometry[5]);
        } else if (!meter.shape_radial_gradient && meter.shape_gradient_geometry.size() >= 4) {
          const auto& geometry = meter.shape_gradient_geometry;
          shape_gradient = cairo_pattern_create_linear(geometry[0], geometry[1],
                                                        geometry[2], geometry[3]);
        } else {
          shape_gradient = meter.shape_radial_gradient ?
              cairo_pattern_create_radial(center_x, center_y, 0.0, center_x, center_y,
                                          std::max(meter.shape_rect.width, meter.shape_rect.height) / 2.0) :
              cairo_pattern_create_linear(meter.shape_rect.x, meter.shape_rect.y,
                                          meter.shape_rect.x + meter.shape_rect.width,
                                          meter.shape_rect.y + meter.shape_rect.height);
        }
        for (const auto& stop : meter.shape_gradient) {
          cairo_pattern_add_color_stop_rgba(shape_gradient, stop.first, stop.second.r, stop.second.g,
                                            stop.second.b, stop.second.a);
        }
        cairo_set_source(cr, shape_gradient);
      } else {
        cairo_set_source_rgba(cr, meter.shape_fill.r, meter.shape_fill.g,
                              meter.shape_fill.b, meter.shape_fill.a);
      }
      cairo_fill_preserve(cr);
      if (shape_gradient) cairo_pattern_destroy(shape_gradient);
      if (meter.shape_stroke.a > 0.0) {
        cairo_set_source_rgba(cr, meter.shape_stroke.r, meter.shape_stroke.g,
                              meter.shape_stroke.b, meter.shape_stroke.a);
        cairo_set_line_width(cr, meter.shape_stroke_width);
        cairo_set_line_cap(cr, meter.shape_line_cap);
        if (!meter.shape_dashes.empty()) {
          cairo_set_dash(cr, meter.shape_dashes.data(),
                         static_cast<int>(meter.shape_dashes.size()), 0.0);
        }
        cairo_stroke(cr);
      } else {
        cairo_new_path(cr);
      }
      cairo_restore(cr);
    } else if (type == "rotator" && meter.image) {
      cairo_save(cr);
      const double center_x = meter.rect.x + meter.rect.width / 2.0;
      const double center_y = meter.rect.y + meter.rect.height / 2.0;
      cairo_translate(cr, center_x, center_y);
      cairo_rotate(cr, meter.rotation_angle * meter.normalized + meter.start_angle);
      cairo_set_source_surface(cr, meter.image, -meter.offset_x, -meter.offset_y);
      cairo_paint(cr);
      cairo_restore(cr);
    } else if (type == "roundline") {
      const double center_x = meter.rect.x + meter.rect.width / 2.0;
      const double center_y = meter.rect.y + meter.rect.height / 2.0;
      const double end_angle = meter.start_angle + meter.rotation_angle * meter.normalized;
      cairo_set_source_rgba(cr, meter.line_color.r, meter.line_color.g, meter.line_color.b,
                            meter.line_color.a);
      if (meter.solid) {
        cairo_new_path(cr);
        cairo_arc(cr, center_x, center_y, meter.line_length, meter.start_angle, end_angle);
        cairo_arc_negative(cr, center_x, center_y, std::max(0.0, meter.line_start), end_angle,
                           meter.start_angle);
        cairo_close_path(cr);
        cairo_fill(cr);
      } else {
        cairo_set_line_width(cr, meter.line_width);
        cairo_move_to(cr, center_x + std::cos(end_angle) * meter.line_start,
                      center_y + std::sin(end_angle) * meter.line_start);
        cairo_line_to(cr, center_x + std::cos(end_angle) * meter.line_length,
                      center_y + std::sin(end_angle) * meter.line_length);
        cairo_stroke(cr);
      }
    } else if ((type == "line" || type == "histogram") && !meter.history.empty()) {
      const auto* graph_measure = meter.measure.empty() ? nullptr : impl_->FindMeasure(meter.measure);
      const double scroll_offset = impl_->scroll_speed > 0.0 ? impl_->scroll_remainder : 0.0;
      double maximum = 1.0;
      if (meter.graph_autoscale) {
        maximum = *std::max_element(meter.history.begin(), meter.history.end());
        if (graph_measure) maximum = std::max(maximum, meter.graph_value);
        if (maximum <= 0.0) maximum = 1.0;
      }
      cairo_rectangle(cr, meter.rect.x, meter.rect.y, meter.rect.width, meter.rect.height);
      cairo_clip(cr);
      cairo_set_source_rgba(cr, meter.line_color.r, meter.line_color.g, meter.line_color.b,
                            meter.line_color.a);
      if (type == "line") {
        cairo_set_line_width(cr, std::max(1.0, meter.line_width));
        for (size_t index = 0; index < meter.history.size(); ++index) {
          const double relative = std::clamp(meter.history[index] / maximum, 0.0, 1.0);
          const double y = meter.graph_flip ? meter.rect.y + relative * meter.rect.height :
                                              meter.rect.y + (1.0 - relative) * meter.rect.height;
          const double x = meter.rect.x + static_cast<double>(index) - scroll_offset;
          if (index == 0) cairo_move_to(cr, x, y);
          else cairo_line_to(cr, x, y);
        }
        if (graph_measure && scroll_offset > 0.0) {
          const double relative = std::clamp(meter.graph_value / maximum, 0.0, 1.0);
          const double y = meter.graph_flip ? meter.rect.y + relative * meter.rect.height :
                                              meter.rect.y + (1.0 - relative) * meter.rect.height;
          cairo_line_to(cr, meter.rect.x + meter.rect.width - 1.0, y);
        }
        cairo_stroke(cr);
      } else {
        for (size_t index = 0; index < meter.history.size(); ++index) {
          const double relative = std::clamp(meter.history[index] / maximum, 0.0, 1.0);
          const double height = relative * meter.rect.height;
          const double y = meter.graph_flip ? meter.rect.y : meter.rect.y + meter.rect.height - height;
          cairo_rectangle(cr, meter.rect.x + static_cast<double>(index) - scroll_offset,
                          y, 1.0, height);
        }
        if (graph_measure && scroll_offset > 0.0) {
          const double relative = std::clamp(meter.graph_value / maximum, 0.0, 1.0);
          const double height = relative * meter.rect.height;
          const double y = meter.graph_flip ? meter.rect.y : meter.rect.y + meter.rect.height - height;
          cairo_rectangle(cr, meter.rect.x + meter.rect.width - scroll_offset,
                          y, scroll_offset, height);
        }
        cairo_fill(cr);
      }
    } else if (type == "bar") {
      if (meter.image) {
        if (meter.horizontal) {
          const double width = meter.image_source_width * meter.normalized;
          PaintImageSlice(cr, meter.image, 0.0, 0.0, width, meter.image_source_height,
                          meter.rect.x, meter.rect.y, meter.rect.width * meter.normalized,
                          meter.rect.height);
        } else {
          const double source_y = meter.image_source_height * (1.0 - meter.normalized);
          const double height = meter.image_source_height - source_y;
          PaintImageSlice(cr, meter.image, 0.0, source_y, meter.image_source_width, height,
                          meter.rect.x, meter.rect.y + meter.rect.height * (1.0 - meter.normalized),
                          meter.rect.width, meter.rect.height * meter.normalized);
        }
      } else {
        cairo_set_source_rgba(cr, meter.bar_color.r, meter.bar_color.g, meter.bar_color.b,
                              meter.bar_color.a);
        if (meter.horizontal) {
          cairo_rectangle(cr, meter.rect.x, meter.rect.y, meter.rect.width * meter.normalized,
                          meter.rect.height);
        } else {
          const double height = meter.rect.height * meter.normalized;
          cairo_rectangle(cr, meter.rect.x, meter.rect.y + meter.rect.height - height,
                          meter.rect.width, height);
        }
        cairo_fill(cr);
      }
    } else if (type == "string") {
      PangoLayout* layout = pango_cairo_create_layout(cr);
      PangoFontDescription* font = pango_font_description_new();
      pango_font_description_set_family(font, meter.font_face.c_str());
      pango_font_description_set_size(font, static_cast<int>(meter.font_size * PANGO_SCALE));
      if (meter.string_style.find("bold") != std::string::npos) {
        pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
      }
      if (meter.string_style.find("italic") != std::string::npos) {
        pango_font_description_set_style(font, PANGO_STYLE_ITALIC);
      }
      pango_layout_set_font_description(layout, font);
      pango_layout_set_text(layout, meter.text.c_str(), -1);
      if (meter.width_defined) pango_layout_set_width(layout, meter.rect.width * PANGO_SCALE);
      if (meter.height_defined) pango_layout_set_height(layout, meter.rect.height * PANGO_SCALE);
      if (meter.align.rfind("center", 0) == 0) pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
      if (meter.align.rfind("right", 0) == 0) pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
      if (meter.clip) pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
      if (meter.string_effect == "shadow") {
        cairo_move_to(cr, meter.rect.x + 1, meter.rect.y + 1);
        cairo_set_source_rgba(cr, meter.effect_color.r, meter.effect_color.g, meter.effect_color.b,
                              meter.effect_color.a);
        pango_cairo_show_layout(cr, layout);
      }
      cairo_move_to(cr, meter.rect.x, meter.rect.y);
      cairo_set_source_rgba(cr, meter.font_color.r, meter.font_color.g, meter.font_color.b,
                            meter.font_color.a);
      pango_cairo_show_layout(cr, layout);
      pango_font_description_free(font);
      g_object_unref(layout);
    }
    cairo_restore(cr);
  }
  cairo_restore(cr);
}

bool Runtime::WritePng(const std::filesystem::path& path, std::string& error) const {
  auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, Width(), Height());
  auto* cr = cairo_create(surface);
  Render(cr);
  cairo_destroy(cr);
  const auto status = cairo_surface_write_to_png(surface, path.c_str());
  cairo_surface_destroy(surface);
  if (status != CAIRO_STATUS_SUCCESS) {
    error = cairo_status_to_string(status);
    return false;
  }
  return true;
}

bool Runtime::WriteArtifacts(const std::filesystem::path& directory, bool click, std::string& error) {
  try {
    std::filesystem::create_directories(directory);
    if (!WritePng(directory / "frame-initial.png", error)) return false;
    if (click) {
      Click(50, 5);
      if (!WritePng(directory / "frame-post.png", error)) return false;
    }
    std::ofstream observations(directory / "observations.json");
    observations << "{\n  \"schemaVersion\": 1,\n  \"stages\": {\n    \"initial\": "
                 << impl_->initial_snapshot;
    if (!impl_->post_snapshot.empty()) observations << ",\n    \"left-up\": " << impl_->post_snapshot;
    observations << "\n  },\n  \"events\": [\n";
    for (size_t i = 0; i < impl_->events.size(); ++i) {
      const auto& event = impl_->events[i];
      observations << "    {\"sequence\":" << event.sequence << ",\"type\":" << Json(event.type)
                   << ",\"detail\":" << Json(event.detail) << '}';
      observations << (i + 1 == impl_->events.size() ? "\n" : ",\n");
    }
    observations << "  ]\n}\n";
    std::ofstream raw_log(directory / "raw.log");
    for (const auto& event : impl_->events) {
      raw_log << event.sequence << '\t' << event.type << '\t' << event.detail << '\n';
    }
    std::ofstream environment(directory / "environment.json");
    environment << "{\"runtime\":\"xrainmeter\",\"version\":\"0.1.0\","
                   "\"renderer\":\"cairo-provisional\"}\n";
    if (!observations.good() || !raw_log.good() || !environment.good()) {
      error = "Unable to write one or more conformance artifacts to " + directory.string();
      return false;
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

}
