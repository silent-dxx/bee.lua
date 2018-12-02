#pragma once

#include <bee/config.h>
#include <filesystem>
#include <Windows.h>
#include <bee/nonstd/expected.h>

namespace bee::path {
	namespace fs = std::filesystem;
	_BEE_API auto exe_path()->nonstd::expected<fs::path, std::exception>;
	_BEE_API auto dll_path()->nonstd::expected<fs::path, std::exception>;
	_BEE_API auto dll_path(HMODULE module_handle)->nonstd::expected<fs::path, std::exception>;
	_BEE_API bool equal(fs::path const& lhs, fs::path const& rhs);
}
