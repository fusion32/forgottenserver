// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "configmanager.h"
#include "crypto.h"
#include "databasemanager.h"
#include "databasetasks.h"
#include "game.h"
#include "iomarket.h"
#include "monsters.h"
#include "outfit.h"
#include "scheduler.h"
#include "script.h"
#include "scriptmanager.h"

#include "service_game.h"
#include "service_status.h"
#ifdef HTTP
#	include "service_http.h"
#endif

#if __has_include("gitmetadata.h")
#include "gitmetadata.h"
#endif

namespace asio = boost::asio;
namespace chrono = std::chrono;
static boost::asio::io_context g_ioContext(1);

DatabaseTasks g_databaseTasks;
Dispatcher g_dispatcher;
Scheduler g_scheduler;

Game g_game;
Monsters g_monsters;
Vocations g_vocations;
extern Scripts* g_scripts;

void ServerStop(void){
	g_ioContext.stop();
}

int main(int argc, const char **argv){
	(void)argc;
	(void)argv;

	std::set_new_handler([]{
			LOG_ERR("OUT OF MEMORY");
			std::terminate();
		});

	// TODO(fusion): Support the other "utility" signals?
	asio::signal_set signals(g_ioContext, SIGINT, SIGTERM);
	signals.async_wait(
		[&](boost::system::error_code, int){
			g_dispatcher.addTask([]{ g_game.setGameState(GAME_STATE_SHUTDOWN); });
		});

#ifndef _WIN32
	if (getuid() == 0 || geteuid() == 0) {
		LOG_ERR("Running the server as root is unsafe and may compromise the"
				" whole system in case of unknown vunerabilities. Please setup"
				" and use a regular user instead.");
		return EXIT_FAILURE;
	}
#else
	SetConsoleTitle(STATUS_SERVER_NAME);

	// NOTE(fusion): Enable virtual terminal processing.
	{
		HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
		if(hStdout && hStdout != INVALID_HANDLE_VALUE){
			DWORD mode = 0;
			if(GetConsoleMode(hStdout, &mode)){
				mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
				SetConsoleMode(hStdout, mode);
			}
		}

		HANDLE hStderr = GetStdHandle(STD_ERROR_HANDLE);
		if(hStderr && hStderr != INVALID_HANDLE_VALUE){
			DWORD mode = 0;
			if(GetConsoleMode(hStderr, &mode)){
				mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
				SetConsoleMode(hStderr, mode);
			}
		}
	}

	// NOTE(fusion): Change process priority.
	{
		std::string_view defaultPriority = getString(ConfigManager::DEFAULT_PRIORITY);
		if (caseInsensitiveEqual(defaultPriority, "high")) {
			SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
		} else if (caseInsensitiveEqual(defaultPriority, "above-normal")) {
			SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
		}
	}
#endif

	srand(static_cast<unsigned int>(OTSYS_TIME()));

#if GIT_RETRIEVED_STATE
	LOG("{} - Version {}", STATUS_SERVER_NAME, GIT_DESCRIBE);
	LOG("Git SHA1 {} dated {}", GIT_SHORT_SHA1, GIT_COMMIT_DATE_ISO8601);
#if GIT_IS_DIRTY
	LOG("*** DIRTY - NOT OFFICIAL RELEASE ***");
#endif
#else
	LOG("{} - Version {}", STATUS_SERVER_NAME, STATUS_SERVER_VERSION);
#endif

	LOG("Compiled with {} ({}) on {} {}", BOOST_COMPILER, ARCH_NAME, __DATE__, __TIME__);

#if defined(LUAJIT_VERSION)
	LOG("Linked with {}", LUAJIT_VERSION);
#else
	LOG("Linked with {}", LUA_RELEASE);
#endif

	LOG("");
	LOG("A server developed by {}", STATUS_SERVER_DEVELOPERS);
	LOG("Visit our forum for updates, support, and resources: https://otland.net/.");
	LOG("");

	g_game.setGameState(GAME_STATE_STARTUP);

	LOG("Loading config...");
	if (!ConfigManager::load()) {
		LOG_ERR("unable to load {}", getString(ConfigManager::CONFIG_FILE));
		return EXIT_FAILURE;
	}

	LOG("Loading rsa private key...");
	if(!RsaLoadPrivateKey()){
		LOG_ERR("failed to load rsa private key");
		return EXIT_FAILURE;
	}

	LOG("Establishing database connection...");
	if (!Database::getInstance().connect()) {
		LOG_ERR("failed to connect to database");
		return EXIT_FAILURE;
	}

	LOG("MySQL: {}", Database::getClientVersion());
	LOG("Running database manager...");
	if (!DatabaseManager::isDatabaseSetup()) {
		LOG_ERR("the database you have specified in config.lua is empty,"
				" please import the schema.sql to your database");
		return EXIT_FAILURE;
	}

	DatabaseManager::updateDatabase();
	if (getBoolean(ConfigManager::OPTIMIZE_DATABASE) && !DatabaseManager::optimizeTables()) {
		LOG("No tables were optimized");
	}

	LOG("Loading vocations...");
	if (!g_vocations.loadFromXml()) {
		LOG_ERR("unable to load vocations");
		return EXIT_FAILURE;
	}

	LOG("Loading items...");
	if (!Item::items.loadFromOtb()) {
		LOG_ERR("unable to load items (OTB)");
		return EXIT_FAILURE;
	}

	LOG("OTB v{:d}.{:d}.{:d}",
			Item::items.majorVersion,
			Item::items.minorVersion,
			Item::items.buildNumber);

	if (!Item::items.loadFromXml()) {
		LOG_ERR("unable to load items (XML)");
		return EXIT_FAILURE;
	}

	LOG("Loading script systems...");
	if (!ScriptingManager::getInstance().loadScriptSystems()) {
		LOG_ERR("failed to load script systems");
		return EXIT_FAILURE;
	}

	LOG("Loading lua scripts...");
	if (!g_scripts->loadScripts("scripts", false, false)) {
		LOG_ERR("failed to load lua scripts");
		return EXIT_FAILURE;
	}

	LOG("Loading monsters...");
	if (!g_monsters.loadFromXml()) {
		LOG_ERR("unable to load monsters");
		return EXIT_FAILURE;
	}

	LOG("Loading lua monsters...");
	if (!g_scripts->loadScripts("monster", false, false)) {
		LOG_ERR("failed to load lua monsters");
		return EXIT_FAILURE;
	}

	LOG("Loading outfits...");
	if (!Outfits::getInstance().loadFromXml()) {
		LOG_ERR("unable to load outfits");
		return EXIT_FAILURE;
	}

	std::string worldType = boost::algorithm::to_upper_copy(getString(ConfigManager::WORLD_TYPE));
	LOG("Checking world type... {}", worldType);
	if (worldType == "PVP") {
		g_game.setWorldType(WORLD_TYPE_PVP);
	} else if (worldType == "NO-PVP") {
		g_game.setWorldType(WORLD_TYPE_NO_PVP);
	} else if (worldType == "PVP-ENFORCED") {
		g_game.setWorldType(WORLD_TYPE_PVP_ENFORCED);
	} else {
		LOG_ERR("unknown world type {}, valid world types are: pvp, no-pvp and pvp-enforced", worldType);
		return EXIT_FAILURE;
	}

	LOG("Loading map...");
	if (!g_game.loadMainMap(getString(ConfigManager::MAP_NAME))) {
		LOG_ERR("failed to load map");
		return EXIT_FAILURE;
	}

	LOG("Initializing game state...");
	g_game.setGameState(GAME_STATE_INIT);

	RentPeriod_t rentPeriod;
	std::string strRentPeriod = boost::algorithm::to_lower_copy(getString(ConfigManager::HOUSE_RENT_PERIOD));
	if (strRentPeriod == "yearly") {
		rentPeriod = RENTPERIOD_YEARLY;
	} else if (strRentPeriod == "weekly") {
		rentPeriod = RENTPERIOD_WEEKLY;
	} else if (strRentPeriod == "monthly") {
		rentPeriod = RENTPERIOD_MONTHLY;
	} else if (strRentPeriod == "daily") {
		rentPeriod = RENTPERIOD_DAILY;
	} else {
		rentPeriod = RENTPERIOD_NEVER;
	}

	g_game.map.houses.payHouses(rentPeriod);
	tfs::iomarket::checkExpiredOffers();
	tfs::iomarket::updateStatistics();

	LOG("Server starting up...");
	g_game.start();
	g_game.setGameState(GAME_STATE_NORMAL);

	// TODO(fusion): Simplify threads "API"?
	g_dispatcher.start();
	g_scheduler.start();
	g_databaseTasks.start();

	// SERVICE BIND ADDRESS
	// IMPORTANT(fusion): Using an IPv6 address here will cause the services to listen
	// to both IPv4 and IPv6. This is not usually a problem, but it depends on how the
	// world address is resolved by the client, which will depend on DNS settings and
	// the address format. If it resolves an IPv6 address, it'll connect with IPv6 but
	// if it resolves an IPv4 address, it will connect as IPv4.
	//  Now, game sessions are tied to specific remote addresses so it would be a problem
	// if the HTTP service records an IPv6 address within a session, but the game received
	// IPv4 connections, etc... Which is why defaulting to an IPv4 address may be a better
	// solution overall.
	//  And there is yet another detail. The client will put the world address directly
	// into an URL, but IPv6 addresses need to be enclosed in brackets to be properly
	// parsed there. If we sent "::1", it would try to connect to "tcp://::1:7171" which
	// is invalid. The better approach would be to always send a hostname instead but in
	// case there is only an IPv6 address, we'd need to make sure we add those brackets
	// to get a "tcp://[::1]:7171" result.
	// TODO(fusion): We might also want to resolve it, if it's a hostname.
	asio::ip::address bindAddress = asio::ip::make_address(getString(ConfigManager::IP));
	if(!getBoolean(ConfigManager::BIND_ONLY_GLOBAL_ADDRESS)){
		if(bindAddress.is_v4()){
			bindAddress = asio::ip::address_v4::any();
		}else{
			bindAddress = asio::ip::address_v6::any();
		}
	}

	{ // GAME SERVICE
		asio::ip::tcp::endpoint endpoint(bindAddress, getNumber(ConfigManager::GAME_PORT));
		asio::co_spawn(g_ioContext,
				GameService(endpoint),
				std::rethrow_exception);
	}

	{ // STATUS SERVICE
		auto minRequestInterval = chrono::seconds(getNumber(ConfigManager::STATUS_MIN_REQUEST_INTERVAL));
		asio::ip::tcp::endpoint endpoint(bindAddress, getNumber(ConfigManager::STATUS_PORT));
		asio::co_spawn(g_ioContext,
				StatusService(endpoint, minRequestInterval),
				std::rethrow_exception);
	}

#ifdef HTTP
	{ // HTTP SERVICE
		asio::ip::tcp::endpoint endpoint(bindAddress, getNumber(ConfigManager::HTTP_PORT));
		asio::co_spawn(g_ioContext,
				HttpService(endpoint),
				std::rethrow_exception);
	}
#endif

	LOG("{} online!", getString(ConfigManager::SERVER_NAME));
	try{
		g_ioContext.run();
	}catch(const std::exception &e){
		LOG_ERR("server error: {}", e.what());
	}

	LOG("Shutting down...");

	g_scheduler.shutdown();
	g_databaseTasks.shutdown();
	g_dispatcher.shutdown();

	g_scheduler.join();
	g_databaseTasks.join();
	g_dispatcher.join();
}

