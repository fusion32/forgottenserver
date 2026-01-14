// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "configmanager.h"
#include "databasemanager.h"
#include "databasetasks.h"
#include "game.h"
#include "iomarket.h"
#include "monsters.h"
#include "outfit.h"
#include "rsa.h"
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

#if defined(__amd64__) || defined(_M_X64)
#	define BUILD_ARCH "x64"
#elif defined(__i386__) || defined(_M_IX86) || defined(_X86_)
#	define BUILD_ARCH "x86"
#elif defined(__arm__)
#	define BUILD_ARCH "ARM";
#else
#	define BUILD_ARCH "unknown"
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

template<typename ...Args>
void PrintError(fmt::format_string<Args...> fmt, Args &&...args){
	fmt::print(fg(fmt::color::crimson) | fmt::emphasis::bold,
			fmt, std::forward<Args>(args)...);
}

void ServerStop(void){
	g_ioContext.stop();
}

int main(int argc, const char **argv){
	(void)argc;
	(void)argv;

	std::set_new_handler([]{
			puts("OUT OF MEMORY");
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
		PrintError("Running the server as root is unsafe and may compromise the"
				" whole system in case of unknown vunerabilities. Please setup"
				" and use a regular user instead.\n");
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
	fmt::print("{} - Version {}\n", STATUS_SERVER_NAME, GIT_DESCRIBE);
	fmt::print("Git SHA1 {} dated {}\n", GIT_SHORT_SHA1, GIT_COMMIT_DATE_ISO8601);
#if GIT_IS_DIRTY
	fmt::print("*** DIRTY - NOT OFFICIAL RELEASE ***\n");
#endif
#else
	fmt::print("{} - Version {}\n", STATUS_SERVER_NAME, STATUS_SERVER_VERSION);
#endif

	fmt::print("Compiled with {} ({}) on {} {}\n", BOOST_COMPILER, BUILD_ARCH, __DATE__, __TIME__);

#if defined(LUAJIT_VERSION)
	fmt::print("Linked with {}\n", LUAJIT_VERSION);
#else
	fmt::print("Linked with {}\n", LUA_RELEASE);
#endif

	fmt::print("\n");
	fmt::print("A server developed by {}\n", STATUS_SERVER_DEVELOPERS);
	fmt::print("Visit our forum for updates, support, and resources: https://otland.net/.\n");
	fmt::print("\n");


	g_game.setGameState(GAME_STATE_STARTUP);

	fmt::print(">> Loading config\n");
	if (!ConfigManager::load()) {
		PrintError("Unable to load {}!\n", getString(ConfigManager::CONFIG_FILE));
		return EXIT_FAILURE;
	}

	fmt::print(">> Establishing database connection...");
	if (!Database::getInstance().connect()) {
		PrintError("Failed to connect to database.\n");
		return EXIT_FAILURE;
	}

	fmt::print(" MySQL {}\n", Database::getClientVersion());
	fmt::print(">> Running database manager\n");
	if (!DatabaseManager::isDatabaseSetup()) {
		PrintError("The database you have specified in config.lua is empty,"
				" please import the schema.sql to your database.\n");
		return EXIT_FAILURE;
	}

	DatabaseManager::updateDatabase();
	if (getBoolean(ConfigManager::OPTIMIZE_DATABASE) && !DatabaseManager::optimizeTables()) {
		fmt::print("> No tables were optimized.\n");
	}

	fmt::print(">> Loading vocations\n");
	if (!g_vocations.loadFromXml()) {
		PrintError("Unable to load vocations!\n");
		return EXIT_FAILURE;
	}

	fmt::print(">> Loading items...");
	if (!Item::items.loadFromOtb()) {
		fmt::print("\n");
		PrintError("Unable to load items (OTB)!\n");
		return EXIT_FAILURE;
	}

	fmt::print(" OTB v{:d}.{:d}.{:d}\n",
			Item::items.majorVersion,
			Item::items.minorVersion,
			Item::items.buildNumber);

	if (!Item::items.loadFromXml()) {
		PrintError("Unable to load items (XML)!\n");
		return EXIT_FAILURE;
	}

	fmt::print(">> Loading script systems\n");
	if (!ScriptingManager::getInstance().loadScriptSystems()) {
		PrintError("Failed to load script systems\n");
		return EXIT_FAILURE;
	}

	fmt::print(">> Loading lua scripts\n");
	if (!g_scripts->loadScripts("scripts", false, false)) {
		PrintError("Failed to load lua scripts\n");
		return EXIT_FAILURE;
	}

	fmt::print(">> Loading monsters\n");
	if (!g_monsters.loadFromXml()) {
		PrintError("Unable to load monsters!\n");
		return EXIT_FAILURE;
	}

	fmt::print(">> Loading lua monsters\n");
	if (!g_scripts->loadScripts("monster", false, false)) {
		PrintError("Failed to load lua monsters\n");
		return EXIT_FAILURE;
	}

	fmt::print(">> Loading outfits\n");
	if (!Outfits::getInstance().loadFromXml()) {
		PrintError("Unable to load outfits!\n");
		return EXIT_FAILURE;
	}

	fmt::print(">> Checking world type...");
	std::string worldType = boost::algorithm::to_lower_copy(getString(ConfigManager::WORLD_TYPE));
	if (worldType == "pvp") {
		g_game.setWorldType(WORLD_TYPE_PVP);
	} else if (worldType == "no-pvp") {
		g_game.setWorldType(WORLD_TYPE_NO_PVP);
	} else if (worldType == "pvp-enforced") {
		g_game.setWorldType(WORLD_TYPE_PVP_ENFORCED);
	} else {
		fmt::print("\n");
		PrintError("Unknown world type {}, valid world types are: pvp, no-pvp and pvp-enforced.\n",
				getString(ConfigManager::WORLD_TYPE));
		return EXIT_FAILURE;
	}
	fmt::print(" {}\n", boost::algorithm::to_upper_copy(worldType));

	fmt::print(">> Loading map\n");
	if (!g_game.loadMainMap(getString(ConfigManager::MAP_NAME))) {
		PrintError("Failed to load map\n");
		return EXIT_FAILURE;
	}

	fmt::print(">> Initializing gamestate\n");
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

	fmt::print(">> Loaded all modules, server starting up...\n");
	g_game.setGameState(GAME_STATE_NORMAL);

	// TODO(fusion): Simplify threads?
	g_dispatcher.start();
	g_scheduler.start();
	g_databaseTasks.start();

	//===================================================================================
	//===================================================================================
	//===================================================================================
	//===================================================================================


	// SERVICE BIND ADDRESS
	asio::ip::address bindAddress = getBoolean(ConfigManager::BIND_ONLY_GLOBAL_ADDRESS)
			? asio::ip::make_address(getString(ConfigManager::IP))
			: asio::ip::address_v6::any();

	{ // GAME SERVICE
		asio::ip::tcp::endpoint endpoint(bindAddress, getNumber(ConfigManager::GAME_PORT));
		asio::co_spawn(g_ioContext,
				GameService(endpoint),
				std::rethrow_exception);
	}

	{ // STATUS SERVICE
		auto minRequestInterval = chrono::milliseconds(getNumber(ConfigManager::STATUS_MIN_REQUEST_INTERVAL));
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

	fmt::print(">> {} Online!\n", getString(ConfigManager::SERVER_NAME));
	fflush(stdout);

	try{
		g_ioContext.run();
	}catch(const std::exception &e){
		PrintError("Server error: {}\n", e.what());
	}

	fmt::print(">> Shutting down...\n");

	g_scheduler.shutdown();
	g_databaseTasks.shutdown();
	g_dispatcher.shutdown();

	g_scheduler.join();
	g_databaseTasks.join();
	g_dispatcher.join();
}

