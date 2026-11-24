// Convertible desktop - in-house file browser (open files / save / choose
// folder), drawn by the toolkit. Used where the OS offers no native dialog
// API without extra libraries (X11), and testable everywhere.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "platform/fileio.hpp"
#include "ui/ui.hpp"

namespace cvd::app {

class FileBrowser {
 public:
  enum class Mode { OpenFiles, SaveFile, ChooseFolder };
  using Done = std::function<void(std::vector<std::string>)>;  // empty = cancelled

  FileBrowser(Mode mode, std::string title, const std::string& start_dir, std::string suggested_name, Done done);

  /// Builds the modal sheet into the Modal layer. Returns false once it closed.
  bool build(ui::Context& ctx, Vec2 size);

  // Logic (unit-tested).
  void navigate(const std::string& dir);
  void go_up();
  void toggle(std::size_t index, bool extend);
  void activate(std::size_t index);  // double click / Enter
  void accept();
  void cancel();
  [[nodiscard]] bool closed() const { return closed_; }
  [[nodiscard]] const std::string& directory() const { return dir_; }
  [[nodiscard]] const std::vector<fileio::DirEntry>& entries() const { return entries_; }
  [[nodiscard]] std::vector<std::string> selection() const;
  [[nodiscard]] Mode mode() const { return mode_; }
  std::string file_name;  // save mode
  bool show_hidden = false;

 private:
  void refresh();
  void finish(std::vector<std::string> result);

  Mode mode_;
  std::string title_;
  std::string dir_;
  std::vector<fileio::DirEntry> entries_;
  std::vector<bool> selected_;
  std::size_t cursor_ = 0;
  bool closed_ = false;
  Done done_;
  std::string path_field_;
};

}  // namespace cvd::app
