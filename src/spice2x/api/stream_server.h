#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <winsock2.h>

namespace api {

    struct HttpRequest {
        std::string method;
        std::string path;
        std::string version;
        std::map<std::string, std::string> query;
        std::map<std::string, std::string> headers;
    };

    // Reads exactly through the HTTP header terminator, leaving any following bytes untouched.
    bool read_http_request(SOCKET socket, size_t size_limit, HttpRequest &request);
    bool websocket_upgrade_requested(const HttpRequest &request);
    void send_http_error(SOCKET socket, const char *status, const char *allow = nullptr);
    void handle_http_request(SOCKET socket, const HttpRequest &request,
            std::atomic_bool &running, const std::string &address);

    // 0 while no stream server is listening, so the API can tell clients not to look for one
    unsigned short stream_server_port();

    class StreamServer {
    public:

        explicit StreamServer(unsigned short port);
        ~StreamServer();

        StreamServer(const StreamServer &) = delete;
        StreamServer &operator=(const StreamServer &) = delete;

    private:

        // configuration
        static constexpr int server_backlog = 4;
        static constexpr int client_limit = 4;
        static constexpr int request_size_limit = 8 * 1024;
        static constexpr int request_timeout_ms = 5000;
        static constexpr int send_timeout_ms = 5000;

        struct Client {
            std::thread thread;
            SOCKET socket = INVALID_SOCKET;
            bool active = false;
        };

        void accept_worker();
        bool open_listener();
        void client_worker(int slot, SOCKET socket, std::string address);

        unsigned short port;
        SOCKET listener = INVALID_SOCKET;
        bool wsa_started = false;
        std::atomic_bool running { false };
        std::thread acceptor;
        std::mutex clients_m;
        // socket and active are guarded by clients_m; only the acceptor touches thread
        std::array<Client, client_limit> clients;
    };
}
