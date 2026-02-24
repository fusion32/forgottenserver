// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "baseevents.h"

#include "luascript.h"
#include "tools.h"

extern LuaEnvironment g_luaEnvironment;

bool BaseEvents::loadFromXml()
{
	if (loaded) {
		LOG_ERR("already loaded");
		return false;
	}

	std::string scriptsName{getScriptBaseName()};
	std::string basePath = "data/" + scriptsName + "/";
	if (getScriptInterface().loadFile(basePath + "lib/" + scriptsName + ".lua") == -1) {
		LOG_WARN("unable to load {}lib/{}.lua", basePath, scriptsName);
	}

	std::string filename = basePath + scriptsName + ".xml";

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(filename.c_str());
	if (!result) {
		printXMLError("BaseEvents::loadFromXml", filename, result);
		return false;
	}

	loaded = true;

	for (auto node : doc.child(scriptsName.c_str()).children()) {
		Event_ptr event = getEvent(node.name());
		if (!event) {
			continue;
		}

		if (!event->configureEvent(node)) {
			LOG_ERR("{}:{}: failed to configure event", filename, node.offset_debug());
			continue;
		}

		bool success;
		pugi::xml_attribute scriptAttribute = node.attribute("script");
		if (scriptAttribute) {
			std::string scriptFile = "scripts/" + std::string(scriptAttribute.as_string());
			success = event->checkScript(basePath, scriptsName, scriptFile)
					&& event->loadScript(basePath + scriptFile);
			if (node.attribute("function")) {
				event->loadFunction(node.attribute("function"), true);
			}
		} else {
			success = event->loadFunction(node.attribute("function"), false);
		}

		if (success) {
			registerEvent(std::move(event), node);
		}
	}
	return true;
}

bool BaseEvents::reload()
{
	loaded = false;
	clear(false);
	return loadFromXml();
}

void BaseEvents::reInitState(bool fromLua)
{
	if (!fromLua) {
		getScriptInterface().reInitState();
	}
}

Event::Event(LuaScriptInterface* interface) : scriptInterface(interface) {}

bool Event::checkScript(const std::string& basePath, const std::string& scriptsName,
                        const std::string& scriptFile) const
{
	LuaScriptInterface* testInterface = g_luaEnvironment.getTestInterface();
	testInterface->reInitState();

	if (testInterface->loadFile(std::string(basePath + "lib/" + scriptsName + ".lua")) == -1) {
		LOG_WARN("unable to load {}lib/{}.lua", basePath, scriptsName);
	}

	if (scriptId != 0) {
		LOG_ERR("script is already loaded (scriptId: {})", scriptId);
		return false;
	}

	if (testInterface->loadFile(basePath + scriptFile) == -1) {
		LOG_ERR("unable to load script {}: {}", scriptFile, testInterface->getLastLuaError());
		return false;
	}

	int32_t id = testInterface->getEvent(getScriptEventName());
	if (id == -1) {
		LOG_ERR("{}: event {} not found", scriptFile, getScriptEventName());
		return false;
	}
	return true;
}

bool Event::loadScript(const std::string& scriptFile)
{
	if (!scriptInterface) {
		LOG_ERR("no script interface");
		return false;
	}

	if (scriptId != 0) {
		LOG_ERR("script is already loaded (scriptId: {})", scriptId);
		return false;
	}

	if (scriptInterface->loadFile(scriptFile) == -1) {
		LOG_ERR("unable to load script {}: {}", scriptFile, scriptInterface->getLastLuaError());
		return false;
	}

	int32_t id = scriptInterface->getEvent(getScriptEventName());
	if (id == -1) {
		LOG_ERR("{}: event {} not found", scriptFile, getScriptEventName());
		return false;
	}

	scripted = true;
	scriptId = id;
	return true;
}

bool Event::loadCallback()
{
	if (!scriptInterface) {
		LOG_ERR("no script interface");
		return false;
	}

	if (scriptId != 0) {
		LOG_ERR("script is already loaded (scriptId: {})", scriptId);
		return false;
	}

	int32_t id = scriptInterface->getEvent();
	if (id == -1) {
		LOG_ERR("event {} not found", getScriptEventName());
		return false;
	}

	scripted = true;
	scriptId = id;
	return true;
}

bool CallBack::loadCallBack(LuaScriptInterface* interface, const std::string& name)
{
	if (!interface) {
		LOG_ERR("no script interface");
		return false;
	}

	scriptInterface = interface;

	int32_t id = scriptInterface->getEvent(name);
	if (id == -1) {
		LOG_ERR("event {} not found", name);
		return false;
	}

	scriptId = id;
	loaded = true;
	return true;
}
