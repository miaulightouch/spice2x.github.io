#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "stream_server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "build/resource.h"
#include "capture_pump.h"
#include "hooks/graphics/graphics.h"
#include "overlay/notifications.h"
#include "stream_format.h"
#include "util/logging.h"
#include "util/utils.h"

namespace api {

    namespace {

        bool send_all(SOCKET socket, const void *data, size_t size) {
            auto cursor = reinterpret_cast<const char *>(data);
            size_t remaining = size;

            while (remaining > 0) {
                const int sent = send(socket, cursor, static_cast<int>(remaining), 0);
                if (sent <= 0) {
                    return false;
                }
                cursor += sent;
                remaining -= static_cast<size_t>(sent);
            }

            return true;
        }

        bool send_all(SOCKET socket, const std::string &text) {
            return send_all(socket, text.data(), text.size());
        }

        // a viewer leaving is normally noticed by a failing send, so a stream with no frame
        // to push has to ask the socket instead
        bool client_gone(SOCKET socket) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(socket, &read_set);

            // the socket is blocking with a receive timeout, so poll before touching it
            timeval immediately {};
            const int ready = select(0, &read_set, nullptr, nullptr, &immediately);
            if (ready == 0) {
                return false;
            }
            if (ready < 0) {
                return true;
            }

            // consumed rather than peeked: a stray byte would otherwise sit in front of the
            // FIN and keep hiding it for as long as the stream runs
            char discard[256];
            return recv(socket, discard, sizeof(discard), 0) <= 0;
        }

        int hex_value(char c) {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        }

        bool decode_url(const std::string &input, bool plus_as_space, std::string &output) {
            output.clear();
            output.reserve(input.size());

            for (size_t i = 0; i < input.size(); i++) {
                if (input[i] == '%') {
                    if (i + 2 >= input.size()) {
                        return false;
                    }

                    const int high = hex_value(input[i + 1]);
                    const int low = hex_value(input[i + 2]);
                    if (high < 0 || low < 0) {
                        return false;
                    }

                    output.push_back(static_cast<char>((high << 4) | low));
                    i += 2;
                } else if (plus_as_space && input[i] == '+') {
                    output.push_back(' ');
                } else {
                    output.push_back(input[i]);
                }
            }

            return true;
        }

        bool parse_query(const std::string &query, HttpRequest &request) {
            size_t pos = 0;

            while (pos < query.size()) {
                auto end = query.find('&', pos);
                if (end == std::string::npos) {
                    end = query.size();
                }

                const auto pair = query.substr(pos, end - pos);
                const auto split = pair.find('=');
                if (split != std::string::npos && split > 0) {
                    std::string key;
                    std::string value;
                    if (!decode_url(pair.substr(0, split), true, key)
                            || !decode_url(pair.substr(split + 1), true, value)) {
                        return false;
                    }
                    request.query[key] = value;
                }

                pos = end + 1;
            }

            return true;
        }

        bool is_token_character(unsigned char c) {
            if (c < 0x21 || c > 0x7e) {
                return false;
            }

            switch (c) {
            case '(':
            case ')':
            case '<':
            case '>':
            case '@':
            case ',':
            case ';':
            case ':':
            case '\\':
            case '"':
            case '/':
            case '[':
            case ']':
            case '?':
            case '=':
            case '{':
            case '}':
            case ' ':
            case '\t':
                return false;
            default:
                return true;
            }
        }

        std::string trim_ows(const std::string &value) {
            const auto begin = value.find_first_not_of(" \t");
            if (begin == std::string::npos) {
                return "";
            }
            return value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
        }

        bool parse_http_head(const std::string &head, HttpRequest &request) {
            request = {};
            const size_t header_end = head.find("\r\n\r\n");
            const size_t request_line_end = head.find("\r\n");
            if (header_end == std::string::npos || request_line_end == std::string::npos
                    || request_line_end > header_end) {
                return false;
            }

            const std::string request_line = head.substr(0, request_line_end);
            const size_t method_end = request_line.find(' ');
            const size_t target_end = method_end == std::string::npos
                    ? std::string::npos : request_line.find(' ', method_end + 1);
            if (method_end == std::string::npos || target_end == std::string::npos
                    || method_end == 0 || target_end == method_end + 1
                    || target_end != request_line.rfind(' ')) {
                return false;
            }

            request.method = request_line.substr(0, method_end);
            request.version = request_line.substr(target_end + 1);
            if (request.version != "HTTP/1.0" && request.version != "HTTP/1.1") {
                return false;
            }
            for (const unsigned char c : request.method) {
                if (!is_token_character(c)) {
                    return false;
                }
            }

            std::string target = request_line.substr(method_end + 1,
                    target_end - method_end - 1);
            if (target.empty() || target[0] != '/' || target.find('#') != std::string::npos) {
                return false;
            }

            const size_t query_start = target.find('?');
            if (query_start != std::string::npos) {
                if (!parse_query(target.substr(query_start + 1), request)) {
                    return false;
                }
                target.resize(query_start);
            }

            if (!decode_url(target, false, request.path)) {
                return false;
            }
            for (const unsigned char c : request.path) {
                if (c < 0x20 || c == 0x7f) {
                    return false;
                }
            }

            size_t pos = request_line_end + 2;
            while (pos < header_end) {
                const size_t line_end = head.find("\r\n", pos);
                if (line_end == std::string::npos || line_end > header_end || line_end == pos) {
                    return false;
                }

                const std::string line = head.substr(pos, line_end - pos);
                const size_t colon = line.find(':');
                if (colon == std::string::npos || colon == 0) {
                    return false;
                }

                const std::string name = line.substr(0, colon);
                for (const unsigned char c : name) {
                    if (!is_token_character(c)) {
                        return false;
                    }
                }

                const std::string value = trim_ows(line.substr(colon + 1));
                for (const unsigned char c : value) {
                    if ((c < 0x20 && c != '\t') || c == 0x7f) {
                        return false;
                    }
                }

                std::string lower_name = name;
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const auto existing = request.headers.find(lower_name);
                if (existing == request.headers.end()) {
                    request.headers.emplace(std::move(lower_name), value);
                } else {
                    existing->second += "," + value;
                }

                pos = line_end + 2;
            }

            return true;
        }

        int query_int(const HttpRequest &request, const std::string &name, int fallback,
                int min, int max) {

            const auto pos = request.query.find(name);
            if (pos == request.query.end()) {
                return fallback;
            }

            try {
                return std::clamp(std::stoi(pos->second), min, max);
            } catch (const std::exception &) {
                return fallback;
            }
        }

        // an <img> can show a cross-origin stream without this, but a browser client that
        // decodes the bytes itself has to fetch() them, and fetch is subject to CORS. errors
        // carry it too, or the client sees an opaque failure instead of the status.
        constexpr const char *cors_header = "Access-Control-Allow-Origin: *\r\n";

        // the port is unauthenticated, so a scanner hammering a busy/missing screen could
        // otherwise flood the overlay; throttle failure toasts per distinct cause. kept under
        // a second so it only swallows that, not a legitimate reconnect - substream itself
        // switches screens with a 300ms gap, and only backs off to a full second once a
        // retry has actually failed
        constexpr double notification_throttle_seconds = 0.5;
        constexpr int stream_fps_limit = 60;

        std::atomic<unsigned short> LISTENING_PORT { 0 };
    }

    unsigned short stream_server_port() {
        return LISTENING_PORT.load();
    }

    bool read_http_request(SOCKET socket, size_t size_limit, HttpRequest &request) {
        std::string head;
        head.reserve(size_limit);

        // Read one byte at a time so a WebSocket frame coalesced with the handshake remains
        // in the socket for wslay.
        while (head.size() < size_limit) {
            char byte = 0;
            const int received = recv(socket, &byte, 1, 0);
            if (received <= 0) {
                return false;
            }

            head.push_back(byte);
            if (head.size() >= 4
                    && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) {
                return parse_http_head(head, request);
            }
        }

        return false;
    }

    namespace {

        std::string lower_copy(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        bool contains_http_token(const std::string &value, const std::string &expected) {
            size_t pos = 0;
            while (pos < value.size()) {
                const size_t end = value.find(',', pos);
                if (lower_copy(trim_ows(value.substr(pos, end - pos))) == expected) {
                    return true;
                }
                if (end == std::string::npos) {
                    break;
                }
                pos = end + 1;
            }
            return false;
        }

        bool send_html(SOCKET socket, bool head_only) {
            const HMODULE module = GetModuleHandle(nullptr);
            const HRSRC resource = FindResource(
                    module, MAKEINTRESOURCE(SPICE_CONTROLLER_HTML), RT_RCDATA);
            if (resource == nullptr) {
                return false;
            }

            const HGLOBAL loaded = LoadResource(module, resource);
            const DWORD size = SizeofResource(module, resource);
            const void *data = loaded == nullptr ? nullptr : LockResource(loaded);
            if (data == nullptr && size != 0) {
                return false;
            }

            const std::string header =
                    "HTTP/1.1 200 OK\r\n"
                    "Connection: close\r\n"
                    + std::string(cors_header) +
                    "Cache-Control: no-store\r\n"
                    "Content-Type: text/html; charset=utf-8\r\n"
                    "Content-Length: " + std::to_string(size) + "\r\n"
                    "\r\n";
            return send_all(socket, header)
                    && (head_only || send_all(socket, data, size));
        }

        bool is_stream_path(const std::string &path) {
            for (const auto &format : stream_formats()) {
                if (format.second == path) {
                    return true;
                }
            }
            return false;
        }

        void serve_stream(SOCKET socket, const HttpRequest &request,
                std::atomic_bool &running, const std::string &address) {
            // Keep a slow viewer from accumulating stale frames in the socket buffer.
            int send_buffer = 16 * 1024;
            setsockopt(socket, SOL_SOCKET, SO_SNDBUF,
                    reinterpret_cast<const char *>(&send_buffer), sizeof(send_buffer));
            const int fps = query_int(request, "fps", 30, 1, stream_fps_limit);
            const int quality = query_int(request, "q", 70, 1, 100);

            // The websocket port only mirrors streams while the explicitly requested stream
            // listener is alive; this never turns -apistream on implicitly.
            if (stream_server_port() == 0) {
                send_http_error(socket, "404 Not Found");
                return;
            }

            auto writer = make_stream_writer(request.path, quality, fps);
            if (!writer) {
                send_http_error(socket, "404 Not Found");
                return;
            }

            std::vector<int> screens;
            graphics_screens_get(screens);

            // registration takes a raw swapchain index and never bounds it, so the
            // capture range has to be enforced here rather than assumed
            const auto streamable = [&screens](int screen) {
                return screen < static_cast<int>(GRAPHICS_CAPTURE_SCREEN_NO)
                        && std::find(screens.begin(), screens.end(), screen) != screens.end();
            };

            // screen 1 is the subscreen in every game that has one; single-screen games only
            // ever register screen 0, so resolve the default against what exists.
            int screen = query_int(request, "screen", -1, 0,
                    std::numeric_limits<int>::max());
            if (screen < 0) {
                screen = streamable(1) ? 1 : 0;
            }

            if (!streamable(screen)) {
                log_warning("api::stream",
                        "screen {} is not available, refusing {}", screen, address);
                overlay::notifications::add_throttled(
                        overlay::notifications::Severity::Warning,
                        fmt::format("api::stream.screen_unavailable.{}", screen),
                        notification_throttle_seconds,
                        fmt::format("Video stream refused: screen {} not available ({})",
                                screen, address));
                send_http_error(socket, "404 Not Found");
                return;
            }

            if (!capture_pump::claim_screen(screen)) {
                log_warning("api::stream",
                        "screen {} is already being streamed, refusing {}", screen, address);
                overlay::notifications::add_throttled(
                        overlay::notifications::Severity::Warning,
                        fmt::format("api::stream.screen_claimed.{}", screen),
                        notification_throttle_seconds,
                        fmt::format("Video stream refused: screen {} already streaming ({})",
                                screen, address));
                send_http_error(socket, "503 Service Unavailable");
                return;
            }

            log_info("api::stream",
                    "client connected: {} ({}, screen={}, fps={}, quality={})",
                    address, request.path, screen, fps, quality);
            overlay::notifications::add(
                    overlay::notifications::Severity::Success,
                    fmt::format("Video stream client connected ({}, screen {})", address, screen));

            const std::string header =
                    "HTTP/1.0 200 OK\r\n"
                    "Connection: close\r\n"
                    + std::string(cors_header) +
                    "Cache-Control: no-store, no-cache, must-revalidate\r\n"
                    "Pragma: no-cache\r\n"
                    "Content-Type: " + writer->content_type() + "\r\n"
                    "\r\n";

            const StreamSend stream_send = [socket](const void *data, size_t size) {
                return send_all(socket, data, size);
            };

            if (send_all(socket, header) && writer->begin(stream_send)) {
                const auto interval = std::chrono::microseconds(1000000 / fps);

                while (running) {
                    const auto started = std::chrono::steady_clock::now();

                    capture_pump::Frame frame;
                    const bool ok = capture_pump::capture_direct(
                            screen, frame.pixels, 1,
                            &frame.timestamp, &frame.width, &frame.height);

                    if (ok && frame.pixels) {
                        if (!writer->write(stream_send, frame)) {
                            break;
                        }
                    } else if (client_gone(socket)) {
                        break;
                    }

                    // A failed capture still paces, or a stalled game spins this.
                    std::this_thread::sleep_until(started + interval);
                }
            }

            capture_pump::release_screen(screen);
            log_info("api::stream", "client disconnected: {}", address);
            overlay::notifications::add(
                    overlay::notifications::Severity::Info,
                    fmt::format("Video stream client disconnected ({})", address));
        }
    }

    bool websocket_upgrade_requested(const HttpRequest &request) {
        const auto upgrade = request.headers.find("upgrade");
        const auto connection = request.headers.find("connection");
        return (upgrade != request.headers.end()
                        && contains_http_token(upgrade->second, "websocket"))
                || (connection != request.headers.end()
                        && contains_http_token(connection->second, "upgrade"))
                || request.headers.find("sec-websocket-key") != request.headers.end();
    }

    void send_http_error(SOCKET socket, const char *status, const char *allow) {
        std::string response = std::string("HTTP/1.0 ") + status + "\r\n"
                + cors_header;
        if (allow != nullptr) {
            response += std::string("Allow: ") + allow + "\r\n";
        }
        response += "Content-Length: 0\r\n"
                    "Connection: close\r\n"
                    "\r\n";
        send_all(socket, response);
    }

    void handle_http_request(SOCKET socket, const HttpRequest &request,
            std::atomic_bool &running, const std::string &address) {
        const bool is_root = request.path == "/" || request.path == "/index.html";
        if (is_root) {
            if (request.method != "GET" && request.method != "HEAD") {
                send_http_error(socket, "405 Method Not Allowed", "GET, HEAD");
            } else if (!send_html(socket, request.method == "HEAD")) {
                send_http_error(socket, "500 Internal Server Error");
            }
            return;
        }

        if (is_stream_path(request.path)) {
            if (request.method != "GET") {
                send_http_error(socket, "405 Method Not Allowed", "GET");
            } else {
                serve_stream(socket, request, running, address);
            }
            return;
        }

        send_http_error(socket, "404 Not Found");
    }

    StreamServer::StreamServer(unsigned short port)
        : port(port)
    {
        // Builds without a video encoder have nothing to serve and every request would 404;
        // taking the port instead only invites confused clients
        if (stream_formats().empty()) {
            log_warning("api::stream",
                    "this build has no video encoders, the video stream is unavailable");
            return;
        }

        if (!this->open_listener()) {
            // the stream was asked for explicitly, so say plainly that it is not there
            log_warning("api::stream",
                    "the video stream is not available on port {}", this->port);
            return;
        }

        this->running = true;
        this->acceptor = std::thread([this] {
            this->accept_worker();
        });

        LISTENING_PORT = this->port;

        // deliberately not logging a full URL; local IPs would leak into shared logs
        log_info("api::stream", "video stream is listening on port: {}", this->port);
        log_warning("api::stream",
                "the video stream is unauthenticated - anyone who can reach port {} can watch "
                "the game screen", this->port);
    }

    bool StreamServer::open_listener() {
        WSADATA wsa_data;
        const int error = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        if (error != 0) {
            log_warning("api::stream", "WSAStartup() returned {}", error);
            return false;
        }
        this->wsa_started = true;

        this->listener = socket(AF_INET, SOCK_STREAM, 0);
        if (this->listener == INVALID_SOCKET) {
            log_warning("api::stream", "could not create listener socket: {}",
                    get_last_error_string());
            return false;
        }

        int opt_enable = 1;
        if (setsockopt(this->listener, SOL_SOCKET, SO_REUSEADDR,
                reinterpret_cast<const char *>(&opt_enable), sizeof(int)) == -1) {
            log_warning("api::stream", "could not set socket option SO_REUSEADDR: {}",
                    get_last_error_string());
        }

        sockaddr_in server_address {};
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(this->port);
        server_address.sin_addr.s_addr = INADDR_ANY;

        if (bind(this->listener, (sockaddr *) &server_address, sizeof(sockaddr)) == -1) {
            log_warning("api::stream", "could not bind socket on port {}: {}",
                    this->port, get_last_error_string());
            closesocket(this->listener);
            this->listener = INVALID_SOCKET;
            return false;
        }

        if (listen(this->listener, server_backlog) == -1) {
            log_warning("api::stream", "could not listen on port {}: {}",
                    this->port, get_last_error_string());
            closesocket(this->listener);
            this->listener = INVALID_SOCKET;
            return false;
        }

        return true;
    }

    StreamServer::~StreamServer() {

        this->running = false;
        LISTENING_PORT = 0;

        if (this->listener != INVALID_SOCKET) {
            closesocket(this->listener);
            this->listener = INVALID_SOCKET;
        }

        // drops the client threads out of their blocking send/recv
        {
            std::lock_guard<std::mutex> lock(this->clients_m);
            for (auto &client : this->clients) {
                if (client.socket != INVALID_SOCKET) {
                    ::shutdown(client.socket, SD_BOTH);
                }
            }
        }

        if (this->acceptor.joinable()) {
            this->acceptor.join();
        }

        // joining is what guarantees no client thread outlives this object
        for (auto &client : this->clients) {
            if (client.thread.joinable()) {
                client.thread.join();
            }
        }

        if (this->wsa_started) {
            WSACleanup();
        }
    }

    void StreamServer::accept_worker() {

        while (this->running) {
            sockaddr_in client_address {};
            int client_address_size = sizeof(sockaddr_in);

            const SOCKET client = accept(
                    this->listener, (sockaddr *) &client_address, &client_address_size);
            if (client == INVALID_SOCKET) {
                // on shutdown the listener is closed under us; otherwise do not spin
                if (this->running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                continue;
            }

            if (!this->running) {
                closesocket(client);
                break;
            }

            char address_data[INET_ADDRSTRLEN] {};
            inet_ntop(AF_INET, &client_address.sin_addr, address_data, INET_ADDRSTRLEN);
            std::string address(address_data);

            // every client costs an encode and real bandwidth, so the cap protects the game
            int slot = -1;
            {
                std::lock_guard<std::mutex> lock(this->clients_m);
                for (size_t i = 0; i < this->clients.size(); i++) {
                    if (!this->clients[i].active) {
                        this->clients[i].active = true;
                        this->clients[i].socket = client;
                        slot = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (slot < 0) {
                log_warning("api::stream", "client limit of {} hit", client_limit);
                overlay::notifications::add_throttled(
                        overlay::notifications::Severity::Warning,
                        "api::stream.client_limit",
                        notification_throttle_seconds,
                        fmt::format("Video stream refused: client limit reached ({})", address));
                send_http_error(client, "503 Service Unavailable");
                closesocket(client);
                continue;
            }

            // this thread is the only one that touches the thread objects, so the slot's
            // previous occupant gets reaped here rather than being detached
            if (this->clients[slot].thread.joinable()) {
                this->clients[slot].thread.join();
            }

            this->clients[slot].thread = std::thread([this, slot, client, address] {
                this->client_worker(slot, client, address);
            });
        }
    }

    void StreamServer::client_worker(int slot, SOCKET socket, std::string address) {

        DWORD timeout = request_timeout_ms;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char *>(&timeout), sizeof(timeout));

        timeout = send_timeout_ms;
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                reinterpret_cast<const char *>(&timeout), sizeof(timeout));

        int opt_enable = 1;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char *>(&opt_enable), sizeof(int));

        HttpRequest request;
        if (read_http_request(socket, request_size_limit, request)) {
            handle_http_request(socket, request, this->running, address);
        } else {
            send_http_error(socket, "400 Bad Request");
        }

        {
            std::lock_guard<std::mutex> lock(this->clients_m);
            this->clients[slot].socket = INVALID_SOCKET;
            this->clients[slot].active = false;
        }

        closesocket(socket);
    }
}
