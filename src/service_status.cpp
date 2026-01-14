#include "otpch.h"
#include "service_status.h"

#include <iostream>
#include <mutex>

// NOTE(fusion): Although the feature for running coroutines in parallel is still
// in the experimental namespace, it has been implemented ever since ASIO 1.21
// which is already ~5 years old and presumably production-ready.
#include <boost/asio/experimental/awaitable_operators.hpp>
using namespace boost::asio::experimental::awaitable_operators;

namespace asio = boost::asio;
namespace chrono = std::chrono;
using asio::use_awaitable;
using asio::ip::tcp;
using chrono::steady_clock;

// Status String
//==============================================================================
static std::mutex g_statusMutex;
static std::string g_statusString;

void SetStatusString(std::string_view s){
    std::lock_guard lockGuard(g_statusMutex);
    g_statusString.assign(s);
}

size_t GetStatusString(uint8_t *buffer, size_t bufferCapacity){
    std::lock_guard lockGuard(g_statusMutex);
    size_t stringLen = g_statusString.size();
    if(stringLen == 0 || bufferCapacity < stringLen){
        return 0;
    }
    memcpy(buffer, g_statusString.data(), stringLen);
    return stringLen;
}

// Status Record
//==============================================================================
struct StatusRecord{
    asio::ip::address        address;
    steady_clock::time_point timepoint;
};

bool AllowStatusRequest(std::vector<StatusRecord> &records,
                        const asio::ip::address &address,
                        steady_clock::duration minRequestInterval){
    // NOTE(fusion): Retain only non-expired records to avoid growing the vector
    // indefinitely.
    size_t writeIdx = 0;
    bool recentRequest = false;
    auto now = steady_clock::now();
    auto cutoff = now - minRequestInterval;
    for(size_t readIdx = 0; readIdx < records.size(); readIdx += 1){
        if(records[readIdx].timepoint >= cutoff){
            if(records[readIdx].address == address){
                recentRequest = true;
            }

            if(readIdx != writeIdx){
                std::swap(records[writeIdx], records[readIdx]);
            }
            writeIdx += 1;
        }
    }
    records.resize(writeIdx);

    if(!recentRequest){
        records.push_back({address, now});
        return true;
    }else{
        return false;
    }
}

// Status Service
//==============================================================================
static asio::awaitable<void> StatusAlarm(void){
    auto executor = co_await asio::this_coro::executor;

    boost::system::error_code ec;
    asio::steady_timer timer(executor);
    timer.expires_at(steady_clock::now() + chrono::seconds(5));
    co_await timer.async_wait(asio::redirect_error(use_awaitable, ec));
    if(ec && ec != asio::error::operation_aborted){
        std::cout << "StatusAlarm: " << ec.message() << std::endl;
    }
}

static asio::awaitable<void> StatusProcess(tcp::socket &socket, const tcp::endpoint &endpoint){
    (void)endpoint;
    uint8_t buffer[1024];
    try{
        co_await asio::async_read(socket, asio::buffer(buffer, 2), use_awaitable);
        size_t requestLen = ((size_t)buffer[0]) | ((size_t)buffer[1] << 8);
        if(requestLen != 6){
            std::cout << "StatusProcess: invalid request length " << requestLen << std::endl;
            co_return;
        }

        co_await asio::async_read(socket, asio::buffer(buffer, requestLen), use_awaitable);
        if(buffer[0] != 255 || buffer[1] != 255){
            std::cout << "StatusProcess: expected status request type (255, 255), got ("
                        << buffer[0] << ", " << buffer[1] << ")" << std::endl;
            co_return;
        }

        std::string_view request{(const char*)&buffer[2], (const char*)&buffer[6]};
        if(request != "info"){
            std::cout << "StatusProcess: unknown status request \"" << request << "\"" << std::endl;
            co_return;
        }

        size_t resultLen = GetStatusString(buffer, sizeof(buffer));
        if(resultLen > 0){
            co_await asio::async_write(socket, asio::buffer(buffer, resultLen), use_awaitable);
        }
    }catch(const boost::system::system_error &e){
        std::cout << "StatusProcess: " << e.what() << std::endl;
    }
}

static asio::awaitable<void> StatusHandler(tcp::socket socket, tcp::endpoint endpoint){
    co_await (StatusAlarm() || StatusProcess(socket, endpoint));
}

asio::awaitable<void> StatusService(tcp::endpoint endpoint,
                                    steady_clock::duration minRequestInterval){
    // TODO(fusion): Find out which exceptions are thrown from `async_accept` and
    // whether they're recoverable, in which case we might want to recreate the
    // acceptor.
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

        std::vector<StatusRecord> statusRecords;
        std::cout << ">> Status service listening on " << endpoint << std::endl;
        while(true){
            tcp::endpoint peer_endpoint;
            tcp::socket socket = co_await acceptor.async_accept(peer_endpoint, use_awaitable);
            if(AllowStatusRequest(statusRecords, peer_endpoint.address(), minRequestInterval)){
                asio::co_spawn(executor,
                        StatusHandler(std::move(socket), std::move(peer_endpoint)),
                        asio::detached);
            }
        }
    }catch(const std::exception &e){
        std::cout << ">> Status service error: " << e.what() << std::endl;
        throw;
    }
}

