#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdlib> // getenv

#include <iostream>
#ifdef WIN32
#define NOMINMAX
#include "Windows.h"
#endif

// Portable solution probably doesn't exist, sucks to suck
static std::string locate_file_dialog(const char* hint, const char* filter, const char* title) {
#ifdef WIN32
	char buf[2048] = {};
	OPENFILENAMEA fn = {};
	fn.lStructSize = sizeof(OPENFILENAMEA);
	fn.lpstrFilter = (LPSTR)filter;
	fn.lpstrFile = (LPSTR)buf;
	fn.nMaxFile = 2048;
	fn.lpstrTitle = (LPSTR)title;
	fn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	GetOpenFileNameA(&fn);
	return std::string(buf);
#else
	// yep, it's terrible. Linux people, make this better if possible maybe
	char buf[1024];
	printf("%s\nThis would be a dialog box, but you're on the wrong operating system. Enter the path here:\n>", title);
	std::cin.getline(buf, 1024);
	return buf;
#endif
}


std::unordered_map<std::string, std::string> globalWakContents;

template <typename T>
static T read_le(std::istream&);
template <>
std::uint8_t read_le(std::istream& s) {
	uint8_t val;
	s.read((char*)&val, sizeof(val));
	return val;
}
template <>
std::uint32_t read_le(std::istream& s) {
	uint32_t val;
	auto it = (uint8_t*)&val;
	for (int i = 0; i < 4; i++)
		it[i] = read_le<uint8_t>(s);
	return val;
}
template <>
std::string read_le(std::istream& s) {
	std::uint32_t size = read_le<std::uint32_t>(s);
	std::string str;
	str.resize(size);
	s.read((char*)str.data(), size);
	return str;
}

static std::string read_file(const char* path) {
	std::string out;
	std::ifstream stream(path, std::ios::binary);
	if (stream.fail()) {
		fprintf(stderr, "Internal error: [%s] does not exist.\n", path);
		exit(-1);
	}
	while (stream) {
		char buffer[1024];
		stream.read(buffer, sizeof(buffer));
		out.append(buffer, stream.gcount());
	}

	return out;
}
static void write_file(const char* path, const std::string& in) {
	std::ofstream stream(path, std::ios::binary);
	stream.write(in.c_str(), in.length());
}

void read_wak(const char* wak_path) {
	globalWakContents.clear();
	std::string contents = read_file(wak_path);
	std::istringstream data(contents);

	uint32_t z1 = read_le<std::uint32_t>(data);
	uint32_t fileCount = read_le<std::uint32_t>(data);
	uint32_t dataStart = read_le<std::uint32_t>(data);
	uint32_t z2 = read_le<std::uint32_t>(data);

	for (uint32_t i = 0; i < fileCount; i++) {
		uint32_t offset = read_le<std::uint32_t>(data);
		uint32_t size = read_le<std::uint32_t>(data);
		std::string name = read_le<std::string>(data);
		globalWakContents.emplace(name, std::string(contents.c_str() + offset, size));
	}
}

std::string& get_wak_file(const std::string& path) {
	if (globalWakContents.find(path) != globalWakContents.end()) {
		return globalWakContents.at(path);
	} else {
		fprintf(stderr, "Internal error: [%s] does not exist.\n", path.c_str());
		exit(-1);
	}
}

std::string find_wak() {
#ifndef __CUDA_ARCH__
	const char* wakpath = ".wakpath";
	if (std::filesystem::exists(wakpath)) {
		std::filesystem::path p = read_file(wakpath);
		if (p.filename().string() != "data.wak") {
			printf(".wakpath didn't contain a valid path!\n");
			exit(-1);
		}
		return p.string();
	}

	std::filesystem::path current = std::filesystem::current_path();
	std::filesystem::path dialog_ret = locate_file_dialog(
		"", "data.wak\0data.wak\0", "Locate your install's data.wak, in the steam install directory");
	std::filesystem::current_path(current);
	write_file(wakpath, dialog_ret.string().c_str());

	if (dialog_ret.filename().string() != "data.wak") {
		printf("File dialog selection didn't result in a valid path!\n");
		exit(-1);
	}

	return dialog_ret.string();
#else
	return "";
#endif
}

// -----------------------------------------------------------------------------
// find_flags_dir
// -----------------------------------------------------------------------------
// Locates the Noita persistent flags directory, caching the result in .flagspath.
// Mirrors find_wak() behavior: check cache, try auto-detection, then show a file
// dialog (pick any 'card_unlocked_*' file) and store the parent folder.
std::string find_flags_dir() {
#ifndef __CUDA_ARCH__
	const char* flagspath_cache = ".flagspath";

	// 1) Cache: if .flagspath exists, validate it's a directory (ideally named 'flags').
	if (std::filesystem::exists(flagspath_cache)) {
		std::filesystem::path p = read_file(flagspath_cache);
		if (!std::filesystem::is_directory(p)) {
			printf(".flagspath didn't contain a valid directory!\n");
			exit(-1);
		}
		// Optional sanity: require directory name to be 'flags' to reduce mistakes.
		if (p.filename().string() != "flags") {
			printf(".flagspath points to '%s' which is not the expected 'flags' directory.\n",
			       p.string().c_str());
			exit(-1);
		}
		return p.string();
	}

// 2) Auto-detect (Windows): %USERPROFILE%\\AppData\\LocalLow\\Nolla_Games_Noita\\save00\\persistent\\flags
#ifdef WIN32
	if (const char* up = std::getenv("USERPROFILE")) {
		std::filesystem::path cand = std::filesystem::path(up) / "AppData/LocalLow/Nolla_Games_Noita/save00/persistent/flags";
		if (std::filesystem::exists(cand) && std::filesystem::is_directory(cand)) {
			// Accept even if the folder is empty (player may not have unlocked anything yet)
			write_file(flagspath_cache, cand.string());
			return cand.string();
		}
	}
#endif

// 3) Fallback: prompt user to paste/type the 'flags' folder path directly.
	std::filesystem::path current = std::filesystem::current_path();
	char buf[2048] = {};
	printf("Locate your Noita persistent flags directory (paste the path to '.../save00/persistent/flags')\n>");
	std::cin.getline(buf, sizeof(buf));
	std::filesystem::current_path(current);

	std::filesystem::path folder = std::filesystem::path(buf);
	if (folder.empty()) {
		printf("No folder provided; cannot resolve flags directory.\n");
		exit(-1);
	}
	if (!std::filesystem::exists(folder) || !std::filesystem::is_directory(folder)) {
		printf("Provided path is not a valid folder.\n");
		exit(-1);
	}
	if (folder.filename().string() != "flags") {
		printf("Provided folder is not named 'flags'. Provided: %s\n", folder.string().c_str());
		exit(-1);
	}

	// Cache for future runs and return.
	write_file(flagspath_cache, folder.string());
	return folder.string();
#else
	return "";
#endif
}
