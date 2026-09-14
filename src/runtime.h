// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cairo/cairo.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rm {

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct HostAction {
  std::string verb;
  std::string config;
  std::string variant;
};

struct ContextItem {
  std::string title;
  std::string action;
};

class Runtime {
 public:
  ~Runtime();
  static std::unique_ptr<Runtime> Load(const std::filesystem::path& skin, std::string& error);

  double MeasureValue(const std::string& name) const;
  std::string MeasureString(const std::string& name) const;
  double MeterNormalizedValue(const std::string& name) const;
  Rect MeterRect(const std::string& name) const;
  bool MeterVisible(const std::string& name) const;
  int Width() const;
  int Height() const;
  int UpdateInterval() const;
  double FrameRate() const;

  void Update();
  void Tick();
  void Click(int x, int y);
  bool RightClick(int x, int y);
  bool DoubleClick(int x, int y);
  void ExecuteAction(const std::string& action);
  std::vector<ContextItem> ContextItems() const;
  std::vector<HostAction> TakeHostActions();
  void Render(cairo_t* cr) const;
  bool WritePng(const std::filesystem::path& path, std::string& error) const;
  bool WriteArtifacts(const std::filesystem::path& directory, bool click, std::string& error);

 private:
  struct Impl;
  explicit Runtime(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}
