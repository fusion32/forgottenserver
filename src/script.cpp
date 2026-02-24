// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "script.h"

#include "configmanager.h"

extern LuaEnvironment g_luaEnvironment;

Scripts::Scripts() : scriptInterface("Scripts Interface") { scriptInterface.initState(); }

Scripts::~Scripts() { scriptInterface.reInitState(); }

bool Scripts::loadScripts(std::string folderName, bool isLib, bool reload)
{
	namespace fs = std::filesystem;

	const auto dir = fs::current_path() / "data" / folderName;
	if (!fs::exists(dir) || !fs::is_directory(dir)) {
		LOG_ERR("unable to load folder '{}'", folderName);
		return false;
	}

	fs::recursive_directory_iterator endit;
	std::vector<fs::path> v;
	std::string disable = ("#");
	for (fs::recursive_directory_iterator it(dir); it != endit; ++it) {
		auto fn = it->path().parent_path().filename();
		if (fn == "lib" && !isLib) {
			continue;
		}
		if (fs::is_regular_file(*it) && it->path().extension() == ".lua") {
			size_t found = it->path().filename().string().find(disable);
			if (found != std::string::npos) {
				if (getBoolean(ConfigManager::SCRIPTS_CONSOLE_LOGS)) {
					LOG("{} [disabled]", it->path().filename().string());
				}
				continue;
			}
			v.push_back(it->path());
		}
	}
	sort(v.begin(), v.end());
	std::string redir;
	for (auto it = v.begin(); it != v.end(); ++it) {
		const std::string scriptFile = it->string();
		if (!isLib) {
			if (redir.empty() || redir != it->parent_path().string()) {
				auto p = fs::path(it->relative_path());
				if (getBoolean(ConfigManager::SCRIPTS_CONSOLE_LOGS)) {
					LOG("[{}]", p.parent_path().filename().string());
				}
				redir = it->parent_path().string();
			}
		}

		if (scriptInterface.loadFile(scriptFile) != -1) {
			if (getBoolean(ConfigManager::SCRIPTS_CONSOLE_LOGS)) {
				if (!reload) {
					LOG("{} [loaded]", it->filename().string());
				} else {
					LOG("{} [reloaded]", it->filename().string());
				}
			}
		}else{
			LOG_ERR("unable to load script {}: {}",
					it->filename().string(),
					scriptInterface.getLastLuaError());
		}
	}

	return true;
}
