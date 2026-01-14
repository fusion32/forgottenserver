#include "otpch.h"
#include "service_http.h"

#include "base64.h"
#include "database.h"
#include "game.h"
#include "tools.h"
#include "vocation.h"

#include <boost/beast.hpp>
#include <boost/json.hpp>

extern Vocations g_vocations;

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace json = boost::json;
namespace chrono = std::chrono;
using asio::use_awaitable;
using asio::ip::tcp;

// IMPORTANT(fusion): We're doing database access inline which is not optimal and
// will block the coroutine and the thread it's running upon. It is not like we
// were doing anything different before, this has always been a problem.

struct HttpResponse {
    beast::http::status status;
    json::value body;
};

static int GetPvpType(WorldType_t worldType){
    switch(worldType){
        default:
        case WORLD_TYPE_PVP:          return 0;
        case WORLD_TYPE_NO_PVP:       return 1;
        case WORLD_TYPE_PVP_ENFORCED: return 2;
    }
}

static HttpResponse HttpBadRequest(int code, std::string_view message){
    HttpResponse res;
    res.status = beast::http::status::bad_request;
    res.body   = json::object{
        {"errorCode",    code},
        {"errorMessage", message},
    };
    return res;
}

static HttpResponse HttpHandleLogin(const tcp::endpoint &endpoint, const json::object &req){
    DBResult_ptr result;
    auto &db = Database::getInstance();
    try {
        std::string_view email    = req.at("email").as_string();
        std::string_view password = req.at("password").as_string();
        result = db.storeQuery(fmt::format(
            "SELECT `id`, UNHEX(`password`) AS `password`, `secret`, `premium_ends_at`"
                    "FROM `accounts` WHERE `email` = {:s}", db.escapeString(email)));
        if(!result || result->getString("password") != transformToSHA1(password)){
            throw 0;
        }
    }catch(...){
        return HttpBadRequest(3, "Email address or password is not correct.");
    }

    int64_t currentTimestamp = chrono::duration_cast<chrono::seconds>(
            chrono::system_clock::now().time_since_epoch()).count();
    std::string_view secret = result->getString("secret");
    if(!secret.empty()){
        const json::value *tokenValue = req.if_contains("token");
        if(!tokenValue || !tokenValue->is_string()){
            return HttpBadRequest(6, "Two-factor token required for authentication.");
        }

        int64_t ticks = currentTimestamp / AUTHENTICATOR_PERIOD;
        std::string_view token = tokenValue->get_string();
        if(token != generateToken(secret, ticks)
                && token != generateToken(secret, ticks - 1)
                && token != generateToken(secret, ticks + 1)) {
            return HttpBadRequest(6, "Two-factor token required for authentication.");
        }
    }

    int64_t accountId = result->getNumber<int64_t>("id");
    int64_t premiumEnd = result->getNumber<int64_t>("premium_ends_at");
    std::string sessionKey = randomBytes(16);
    if (!db.executeQuery(fmt::format(
            "INSERT INTO `sessions` (`token`, `account_id`, `ip`)"
                " VALUES ({:s}, {:d}, INET6_ATON({:s}))",
            db.escapeString(sessionKey), accountId,
            db.escapeString(endpoint.address().to_string())))) {
        return HttpBadRequest(2, "Internal error.");
    }

    int64_t lastLogin = 0;
    json::array characters;
    result = db.storeQuery(fmt::format(
            "SELECT `id`, `name`, `level`, `vocation`, `lastlogin`, `sex`,"
                " `looktype`, `lookhead`, `lookbody`, `looklegs`, `lookfeet`,"
                " `lookaddons`"
            " FROM `players` WHERE `account_id` = {:d}", accountId));
    if(result){
        do {
            std::string_view vocationName = "none";
            int vocationId = result->getNumber<int>("vocation");
            if(Vocation *vocation = g_vocations.getVocation(vocationId)){
                vocationName = vocation->getVocName();
            }

            characters.push_back(json::object{
                {"worldid",             0}, // not implemented
                {"name",                result->getString("name")},
                {"level",               result->getNumber<uint32_t>("level")},
                {"vocation",            vocationName},
                {"lastlogin",           result->getNumber<uint64_t>("lastlogin")},
                {"ismale",              result->getNumber<uint16_t>("sex") == PLAYERSEX_MALE},
                {"ishidden",            false}, // not implemented
                {"ismaincharacter",     false}, // not implemented
                {"tutorial",            false}, // not implemented
                {"outfitid",            result->getNumber<uint32_t>("looktype")},
                {"headcolor",           result->getNumber<uint32_t>("lookhead")},
                {"torsocolor",          result->getNumber<uint32_t>("lookbody")},
                {"legscolor",           result->getNumber<uint32_t>("looklegs")},
                {"detailcolor",         result->getNumber<uint32_t>("lookfeet")},
                {"addonsflags",         result->getNumber<uint32_t>("lookaddons")},
                {"dailyrewardstate",    0}, // not implemented
            });

            lastLogin = std::max(lastLogin, result->getNumber<int64_t>("lastlogin"));
        } while (result->next());
    }

    // IMPORTANT(fusion): These config values must be non-reloadable to avoid
    // race conditions.
    json::array worlds{
        json::object{
            {"id",                          0}, // not implemented
            {"name",                        getString(ConfigManager::SERVER_NAME)},
            {"externaladdressprotected",    getString(ConfigManager::IP)},
            {"externalportprotected",       getNumber(ConfigManager::GAME_PORT)},
            {"externaladdressunprotected",  getString(ConfigManager::IP)},
            {"externalportunprotected",     getNumber(ConfigManager::GAME_PORT)},
            {"previewstate",                0}, // not implemented
            {"location",                    getString(ConfigManager::LOCATION)},
            {"anticheatprotection",         false}, // not implemented
            {"pvptype",                     GetPvpType(g_game.getWorldType())},
        },
    };

    HttpResponse res;
    res.status = beast::http::status::ok;
    res.body   = {
        {
            "session",
            json::object{
                {"sessionkey",              tfs::base64::encode(sessionKey)},
                {"lastlogintime",           lastLogin},
                {"ispremium",               premiumEnd >= currentTimestamp},
                {"premiumuntil",            premiumEnd},
                {"status",                  "active"}, // not implemented
                {"returnernotification",    false}, // not implemented
                {"showrewardnews",          true}, // not implemented
                {"isreturner",              true}, // not implemented
                {"recoverysetupcomplete",   true}, // not implemented
                {"fpstracking",             false}, // not implemented
                {"optiontracking",          false}, // not implemented
            },
        },
        {
            "playdata",
            json::object{
                {"worlds", worlds},
                {"characters", characters},
            },
        },
    };
    return res;
}

static HttpResponse HttpHandleCacheInfo(const tcp::endpoint &endpoint, const json::object &req){
    (void)endpoint;
    (void)req;

    auto& db = Database::getInstance();
    auto result = db.storeQuery("SELECT COUNT(*) AS `count` FROM `players_online`");
    if (!result) {
        return HttpBadRequest(2, "Internal error.");
    }

    HttpResponse res;
    res.status = beast::http::status::ok;
    res.body   = json::object{
        {"playersonline", result->getNumber<int>("count")},
    };
    return res;
}

static beast::http::message_generator HttpHandleRequest(const tcp::endpoint &endpoint,
                                beast::http::request<beast::http::string_body> &&req){
    HttpResponse res;

    // TODO(fusion): Should we even care about returning a sensible result if
    // the request is malformed?
    json::monotonic_resource mr;
    json::value reqBody = json::parse(req.body(), &mr);
    json::object reqBodyObj = reqBody.as_object();
    json::value *typeValue = reqBodyObj.if_contains("type");
    if(typeValue && typeValue->is_string()){
        std::string_view type = typeValue->get_string();
        if(type == "login"){
            res = HttpHandleLogin(endpoint, reqBodyObj);
        }else if(type == "cacheinfo"){
            res = HttpHandleCacheInfo(endpoint, reqBodyObj);
        }else{
            res = HttpBadRequest(2, "Invalid request type.");
        }
    }else{
        res = HttpBadRequest(2, "Invalid request.");
    }

    beast::http::response<beast::http::string_body> msg{beast::http::status::ok, req.version()};
    msg.body() = json::serialize(res.body);
    msg.keep_alive(req.keep_alive());
    msg.prepare_payload();
    return msg;
}

static asio::awaitable<void> HttpHandler(tcp::socket socket, tcp::endpoint endpoint){
    beast::tcp_stream stream(std::move(socket));
    beast::flat_buffer buffer;
    bool keepAlive = true;
    try{
        while(keepAlive){
            stream.expires_after(chrono::seconds(5));

            beast::http::request<beast::http::string_body> req;
            co_await beast::http::async_read(stream, buffer, req, use_awaitable);

            beast::http::message_generator msg = HttpHandleRequest(endpoint, std::move(req));
            keepAlive = msg.keep_alive();
            co_await beast::async_write(stream, std::move(msg), use_awaitable);
        }
    }catch(const boost::system::system_error &e){
        if(e.code() != beast::http::error::end_of_stream
                && e.code() != beast::error::timeout){
            std::cout << "HttpConnectionHandler: " << e.what() << std::endl;
        }
    }
}

asio::awaitable<void> HttpService(tcp::endpoint endpoint){
    // TODO(fusion): Same as `StatusService`.
    auto executor = co_await asio::this_coro::executor;
    try{
        tcp::acceptor acceptor(executor);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.set_option(asio::ip::tcp::no_delay(true));
        if(endpoint.address().is_v6()){
            acceptor.set_option(asio::ip::v6_only(false));
        }
        acceptor.bind(endpoint);
        acceptor.listen(1024);

        std::cout << ">> HTTP service listening on " << endpoint << std::endl;
        while(true){
            tcp::endpoint peer_endpoint;
            tcp::socket socket = co_await acceptor.async_accept(peer_endpoint, use_awaitable);
            asio::co_spawn(executor,
                    HttpHandler(std::move(socket), std::move(peer_endpoint)),
                    asio::detached);
        }
    }catch(const std::exception &e){
        std::cout << "Status service error: " << e.what() << std::endl;
        throw;
    }
}

