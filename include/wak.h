#pragma once
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Reads a Noita .wak archive into memory for later file access (get_wak_file)
void read_wak(const char* wak_path);
// Retrieves a file's content (by virtual path) from the most recently read .wak archive
std::string& get_wak_file(const std::string& path);
// Locates and caches the path to Noita's data.wak (uses .wakpath cache or a file dialog)
std::string find_wak();

// Locates and caches the path to the player's persistent flags directory.
// Behavior mirrors find_wak():
// - If a cached path exists in .flagspath, validate and return it.
// - Otherwise, attempt platform-specific auto-detection (Windows: AppData\LocalLow\Nolla_Games_Noita\save00\persistent\flags).
// - If auto-detection fails, show a folder picker (Windows) to select the 'flags' directory directly; on other OSes, prompt for a path.
// - The directory may be empty (no card_unlocked_* files) and will still be accepted.
// - The resolved path is cached in .flagspath for future runs.
std::string find_flags_dir();
