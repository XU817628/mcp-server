#include "httplib.h"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct UrlParts {
    std::string scheme;
    std::string host;
    int port = 80;
    std::string path;
};

struct HttpExchange {
    int status = 0;
    std::string body;
    httplib::Headers headers;
};

struct PendingResponse {
    std::promise<json> promise;
};

struct SseEvent {
    std::string event = "message";
    std::string data;
    std::string raw;
};

std::string trim(const std::string &value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string now_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) %
                    1000;
    std::tm local_tm{};
    localtime_r(&time, &local_tm);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

std::string today_date() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_r(&time, &local_tm);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d");
    return oss.str();
}

UrlParts parse_url(const std::string &url) {
    static const std::regex pattern(R"(^(http)://([^/:]+)(?::(\d+))?(\/.*)?$)");
    std::smatch match;
    if (!std::regex_match(url, match, pattern)) {
        throw std::runtime_error("Unsupported URL: " + url);
    }

    UrlParts parts;
    parts.scheme = match[1].str();
    parts.host = match[2].str();
    parts.port = match[3].matched ? std::stoi(match[3].str()) : 80;
    parts.path = match[4].matched ? match[4].str() : "/";
    return parts;
}

std::string httplib_error_to_string(httplib::Error error) {
    switch (error) {
    case httplib::Error::Success:
        return "Success";
    case httplib::Error::Unknown:
        return "Unknown";
    case httplib::Error::Connection:
        return "Connection";
    case httplib::Error::BindIPAddress:
        return "BindIPAddress";
    case httplib::Error::Read:
        return "Read";
    case httplib::Error::Write:
        return "Write";
    case httplib::Error::ExceedRedirectCount:
        return "ExceedRedirectCount";
    case httplib::Error::Canceled:
        return "Canceled";
    case httplib::Error::SSLConnection:
        return "SSLConnection";
    case httplib::Error::SSLLoadingCerts:
        return "SSLLoadingCerts";
    case httplib::Error::SSLServerVerification:
        return "SSLServerVerification";
    case httplib::Error::SSLServerHostnameVerification:
        return "SSLServerHostnameVerification";
    case httplib::Error::UnsupportedMultipartBoundaryChars:
        return "UnsupportedMultipartBoundaryChars";
    case httplib::Error::Compression:
        return "Compression";
    case httplib::Error::ConnectionTimeout:
        return "ConnectionTimeout";
    case httplib::Error::ProxyConnection:
        return "ProxyConnection";
    default:
        return "Other";
    }
}

class DailyLogger {
  public:
    explicit DailyLogger(std::string log_dir) : log_dir_(std::move(log_dir)) {
        fs::create_directories(log_dir_);
    }

    void log(const std::string &direction, const std::string &message) {
        std::lock_guard<std::mutex> lock(mutex_);
        rotate_if_needed();
        const std::string line =
            "[" + now_timestamp() + "] [" + direction + "] " + message;
        std::cout << line << std::endl;
        if (file_.is_open()) {
            file_ << line << '\n';
            file_.flush();
        }
    }

    void info(const std::string &message) { log("INFO", message); }
    void send(const std::string &message) { log("SEND", message); }
    void recv(const std::string &message) { log("RECV", message); }
    void recv_sse(const std::string &message) { log("RECV-SSE", message); }

  private:
    void rotate_if_needed() {
        const auto current = today_date();
        if (current == current_date_ && file_.is_open()) {
            return;
        }

        current_date_ = current;
        if (file_.is_open()) {
            file_.close();
        }
        const fs::path file_path = fs::path(log_dir_) /
                                   ("mcp-client-" + current_date_ + ".log");
        file_.open(file_path, std::ios::app);
        if (!file_) {
            throw std::runtime_error("Unable to open log file: " +
                                     file_path.string());
        }
    }

    std::string log_dir_;
    std::string current_date_;
    std::ofstream file_;
    std::mutex mutex_;
};

class SseParser {
  public:
    std::vector<SseEvent> append(const char *data, size_t len) {
        buffer_.append(data, len);
        std::vector<SseEvent> events;

        while (true) {
            const auto pos = buffer_.find('\n');
            if (pos == std::string::npos) {
                break;
            }

            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            process_line(line, events);
        }

        return events;
    }

  private:
    void process_line(const std::string &line, std::vector<SseEvent> &events) {
        if (line.empty()) {
            if (!event_name_.empty() || !data_.empty()) {
                SseEvent event;
                event.event = event_name_.empty() ? "message" : event_name_;
                event.data = data_;
                event.raw =
                    "event: " + event.event + "\ndata: " + event.data + "\n\n";
                events.push_back(std::move(event));
            }
            event_name_.clear();
            data_.clear();
            return;
        }

        if (line[0] == ':') {
            return;
        }

        if (line.rfind("event:", 0) == 0) {
            event_name_ = trim(line.substr(6));
            return;
        }

        if (line.rfind("data:", 0) == 0) {
            if (!data_.empty()) {
                data_ += '\n';
            }
            data_ += trim(line.substr(5));
        }
    }

    std::string buffer_;
    std::string event_name_;
    std::string data_;
};

class McpClient {
  public:
    enum class TransportMode { HttpStream, LegacySse };

    McpClient(UrlParts server_url, TransportMode mode, std::string log_dir)
        : server_url_(std::move(server_url)), mode_(mode),
          logger_(std::move(log_dir)) {
        post_path_ = server_url_.path;
        if (mode_ == TransportMode::LegacySse) {
            sse_path_ = server_url_.path;
        } else {
            sse_path_ = server_url_.path;
        }
        logger_.info("Client configured for " + server_url_.host + ":" +
                     std::to_string(server_url_.port) + server_url_.path);
    }

    ~McpClient() { shutdown(); }

    json initialize(const json &params) {
        if (mode_ == TransportMode::LegacySse) {
            start_event_stream_if_needed();
            wait_for_legacy_endpoint();
        }

        json response = request("initialize", params);
        initialized_ = true;

        if (mode_ == TransportMode::HttpStream) {
            if (session_id_.empty()) {
                throw std::runtime_error(
                    "HTTP Stream initialize did not return Mcp-Session-Id");
            }
            start_event_stream_if_needed();
        }

        return response;
    }

    json ping() { return request("ping", json::object()); }
    json list_tools() { return request("tools/list", json::object()); }
    json list_resources() { return request("resources/list", json::object()); }
    json list_prompts() { return request("prompts/list", json::object()); }

    json call_tool(const std::string &name, const json &arguments,
                   const json &meta = json()) {
        json params = {{"name", name}, {"arguments", arguments}};
        if (!meta.is_null() && !meta.empty()) {
            params["_meta"] = meta;
        }
        return request("tools/call", params);
    }

    json read_resource(const std::string &uri) {
        return request("resources/read", {{"uri", uri}});
    }

    json subscribe_resource(const std::string &uri) {
        return request("resources/subscribe", {{"uri", uri}});
    }

    json unsubscribe_resource(const std::string &uri) {
        return request("resources/unsubscribe", {{"uri", uri}});
    }

    json get_prompt(const std::string &name, const json &arguments) {
        return request("prompts/get", {{"name", name}, {"arguments", arguments}});
    }

    json logging_set_level(const std::string &level) {
        return request("logging/setLevel", {{"level", level}});
    }

    json completion_complete(const json &ref, const json &argument) {
        return request("completion/complete", {{"ref", ref}, {"argument", argument}});
    }

    json roots_list() { return request("roots/list", json::object()); }

    HttpExchange notify_initialized() {
        return notify("notifications/initialized", json::object());
    }

    HttpExchange notify_cancelled(const std::string &request_id,
                                  const std::string &reason) {
        return notify("notifications/cancelled",
                      {{"requestId", request_id}, {"reason", reason}});
    }

    HttpExchange notify_progress(const std::string &token, int progress,
                                 int total, const std::string &message) {
        return notify("notifications/progress",
                      {{"progressToken", token},
                       {"progress", progress},
                       {"total", total},
                       {"message", message}});
    }

    HttpExchange notify_roots_list_changed() {
        return notify("notifications/roots/list_changed", json::object());
    }

    HttpExchange notify_resources_list_changed() {
        return notify("notifications/resources/list_changed", json::object());
    }

    HttpExchange notify_resources_updated(const std::string &uri) {
        return notify("notifications/resources/updated", {{"uri", uri}});
    }

    HttpExchange notify_prompts_list_changed() {
        return notify("notifications/prompts/list_changed", json::object());
    }

    HttpExchange notify_tools_list_changed() {
        return notify("notifications/tools/list_changed", json::object());
    }

    HttpExchange notify_message(const std::string &level,
                                const std::string &data) {
        return notify("notifications/message", {{"level", level}, {"data", data}});
    }

    json wait_for_notification(const std::string &method,
                               std::chrono::milliseconds timeout,
                               std::function<bool(const json &)> predicate =
                                   [](const json &) { return true; }) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(notification_mutex_);
        while (true) {
            auto it = std::find_if(
                notifications_.begin(), notifications_.end(),
                [&](const json &message) {
                    return message.contains("method") &&
                           message["method"] == method && predicate(message);
                });
            if (it != notifications_.end()) {
                json found = *it;
                notifications_.erase(it);
                return found;
            }

            if (notification_cv_.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
                throw std::runtime_error("Timed out waiting for notification: " +
                                         method);
            }
        }
    }

    void shutdown() {
        bool expected = false;
        if (!shutdown_started_.compare_exchange_strong(expected, true)) {
            return;
        }

        sse_running_.store(false);
        {
            std::lock_guard<std::mutex> lock(endpoint_mutex_);
            endpoint_ready_ = true;
        }
        endpoint_cv_.notify_all();

        std::shared_ptr<httplib::Client> local_client;
        {
            std::lock_guard<std::mutex> lock(sse_client_mutex_);
            local_client = sse_client_;
        }
        if (local_client) {
            local_client->stop();
        }

        if (mode_ == TransportMode::HttpStream && !session_id_.empty()) {
            try {
                httplib::Client client(server_url_.host, server_url_.port);
                client.set_connection_timeout(5);
                client.set_read_timeout(5);
                client.set_keep_alive(true);
                auto result = client.Delete(server_url_.path, make_headers(false));
                if (result) {
                    logger_.recv("HTTP DELETE status=" +
                                 std::to_string(result->status) +
                                 " body=" + result->body);
                }
            } catch (...) {
            }
        }

        if (sse_thread_.joinable()) {
            sse_thread_.join();
        }

        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto &[id, pending] : pending_responses_) {
            try {
                pending->promise.set_exception(std::make_exception_ptr(
                    std::runtime_error("Client shutting down")));
            } catch (...) {
            }
        }
        pending_responses_.clear();
    }

  private:
    json request(const std::string &method, const json &params) {
        json payload = {{"jsonrpc", "2.0"},
                        {"id", next_id()},
                        {"method", method},
                        {"params", params}};
        return send_json(payload, true).value.value();
    }

    HttpExchange notify(const std::string &method, const json &params) {
        json payload = {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
        return send_json(payload, false).exchange;
    }

    struct SendResult {
        HttpExchange exchange;
        std::optional<json> value;
    };

    SendResult send_json(const json &payload, bool expect_response) {
        const std::string body = payload.dump();
        logger_.send(body);

        if (mode_ == TransportMode::LegacySse) {
            return send_via_legacy_sse(payload, body, expect_response);
        }
        return send_via_http_stream(payload, body, expect_response);
    }

    SendResult send_via_http_stream(const json &payload, const std::string &body,
                                    bool expect_response) {
        httplib::Client client(server_url_.host, server_url_.port);
        client.set_connection_timeout(5);
        client.set_read_timeout(35);
        client.set_keep_alive(true);

        auto result =
            client.Post(post_path_, make_headers(payload["method"] == "initialize"),
                        body, "application/json");
        if (!result) {
            throw std::runtime_error(
                "HTTP POST failed: " + httplib_error_to_string(result.error()));
        }

        HttpExchange exchange;
        exchange.status = result->status;
        exchange.body = result->body;
        exchange.headers = result->headers;
        if (!exchange.body.empty()) {
            logger_.recv(exchange.body);
        } else {
            logger_.recv("HTTP status=" + std::to_string(exchange.status) +
                         " body=<empty>");
        }

        if (payload["method"] == "initialize") {
            auto session = result->get_header_value("Mcp-Session-Id");
            if (!session.empty()) {
                session_id_ = session;
                logger_.info("Captured Mcp-Session-Id=" + session_id_);
            }
        }

        if (!expect_response) {
            if (exchange.status != 200 && exchange.status != 202) {
                throw std::runtime_error("Notification failed with HTTP status " +
                                         std::to_string(exchange.status));
            }
            return {exchange, std::nullopt};
        }

        if (exchange.status != 200) {
            throw std::runtime_error("Request failed with HTTP status " +
                                     std::to_string(exchange.status));
        }
        return {exchange, json::parse(exchange.body)};
    }

    SendResult send_via_legacy_sse(const json &payload, const std::string &body,
                                   bool expect_response) {
        wait_for_legacy_endpoint();

        std::shared_ptr<PendingResponse> pending;
        std::future<json> future;
        std::string request_id;
        if (expect_response) {
            request_id = payload["id"].get<std::string>();
            pending = std::make_shared<PendingResponse>();
            future = pending->promise.get_future();
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_responses_[request_id] = pending;
        }

        httplib::Client client(server_url_.host, server_url_.port);
        client.set_connection_timeout(5);
        client.set_read_timeout(35);
        client.set_keep_alive(true);

        auto result = client.Post(post_path_, make_headers(false), body,
                                  "application/json");
        if (!result) {
            remove_pending(request_id);
            throw std::runtime_error(
                "Legacy SSE POST failed: " +
                httplib_error_to_string(result.error()));
        }

        HttpExchange exchange;
        exchange.status = result->status;
        exchange.body = result->body;
        exchange.headers = result->headers;
        if (!exchange.body.empty()) {
            logger_.recv(exchange.body);
        } else {
            logger_.recv("HTTP status=" + std::to_string(exchange.status) +
                         " body=<empty>");
        }

        if (!expect_response) {
            if (exchange.status != 200 && exchange.status != 202) {
                throw std::runtime_error("Legacy SSE notification failed with " +
                                         std::to_string(exchange.status));
            }
            return {exchange, std::nullopt};
        }

        if (exchange.status != 200 && exchange.status != 202) {
            remove_pending(request_id);
            throw std::runtime_error("Legacy SSE request failed with HTTP status " +
                                     std::to_string(exchange.status));
        }

        if (future.wait_for(std::chrono::seconds(40)) !=
            std::future_status::ready) {
            remove_pending(request_id);
            throw std::runtime_error("Timed out waiting for SSE response id=" +
                                     request_id);
        }
        return {exchange, future.get()};
    }

    void remove_pending(const std::string &request_id) {
        if (request_id.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_responses_.erase(request_id);
    }

    std::string next_id() { return std::to_string(++next_id_); }

    httplib::Headers make_headers(bool initialize_request) const {
        httplib::Headers headers = {{"Content-Type", "application/json"},
                                    {"Accept",
                                     "application/json, text/event-stream"}};
        if (!initialize_request && !session_id_.empty()) {
            headers.emplace("Mcp-Session-Id", session_id_);
        }
        return headers;
    }

    void start_event_stream_if_needed() {
        bool expected = false;
        if (!sse_started_.compare_exchange_strong(expected, true)) {
            return;
        }

        sse_running_.store(true);
        sse_thread_ = std::thread([this]() { event_stream_loop(); });
    }

    void wait_for_legacy_endpoint() {
        std::unique_lock<std::mutex> lock(endpoint_mutex_);
        if (endpoint_ready_) {
            return;
        }
        if (!endpoint_cv_.wait_for(lock, std::chrono::seconds(10),
                                   [this]() { return endpoint_ready_; })) {
            throw std::runtime_error("Timed out waiting for SSE endpoint event");
        }
    }

    void event_stream_loop() {
        logger_.info("Starting SSE listener thread");
        while (sse_running_.load()) {
            try {
                auto client = std::make_shared<httplib::Client>(server_url_.host,
                                                                server_url_.port);
                client->set_connection_timeout(5);
                client->set_read_timeout(35);
                client->set_keep_alive(true);

                {
                    std::lock_guard<std::mutex> lock(sse_client_mutex_);
                    sse_client_ = client;
                }

                SseParser parser;
                auto response_handler = [&](const httplib::Response &response) {
                    logger_.info("SSE connected with status " +
                                 std::to_string(response.status));
                    if (response.status != 200) {
                        return false;
                    }
                    return true;
                };

                auto content_receiver = [&](const char *data, size_t len) {
                    for (auto &event : parser.append(data, len)) {
                        handle_sse_event(event);
                    }
                    return sse_running_.load();
                };

                auto result = client->Get(stream_path(), make_headers(false),
                                          response_handler, content_receiver);
                if (!sse_running_.load()) {
                    break;
                }

                if (!result) {
                    logger_.info("SSE stream disconnected: " +
                                 httplib_error_to_string(result.error()));
                } else {
                    logger_.info("SSE stream finished with status " +
                                 std::to_string(result->status));
                }
            } catch (const std::exception &ex) {
                logger_.info(std::string("SSE listener exception: ") + ex.what());
            }

            {
                std::lock_guard<std::mutex> lock(sse_client_mutex_);
                sse_client_.reset();
            }

            if (!sse_running_.load()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        logger_.info("SSE listener thread stopped");
    }

    std::string stream_path() const { return sse_path_; }

    void handle_sse_event(const SseEvent &event) {
        logger_.recv_sse(event.raw);

        if (event.event == "endpoint") {
            std::lock_guard<std::mutex> lock(endpoint_mutex_);
            post_path_ = trim(event.data);
            endpoint_ready_ = true;
            endpoint_cv_.notify_all();
            logger_.info("Legacy SSE endpoint discovered: " + post_path_);
            return;
        }

        if (event.data.empty()) {
            return;
        }

        json message;
        try {
            message = json::parse(event.data);
        } catch (const std::exception &) {
            return;
        }

        if (message.contains("id") &&
            (message.contains("result") || message.contains("error"))) {
            const std::string response_id =
                message["id"].is_number()
                    ? std::to_string(message["id"].get<int64_t>())
                    : message["id"].get<std::string>();
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_responses_.find(response_id);
            if (it != pending_responses_.end()) {
                try {
                    it->second->promise.set_value(message);
                } catch (...) {
                }
                pending_responses_.erase(it);
                return;
            }
        }

        std::lock_guard<std::mutex> lock(notification_mutex_);
        notifications_.push_back(message);
        notification_cv_.notify_all();
    }

    UrlParts server_url_;
    TransportMode mode_;
    DailyLogger logger_;

    std::string session_id_;
    std::string post_path_;
    std::string sse_path_;
    std::atomic<uint64_t> next_id_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> sse_started_{false};
    std::atomic<bool> sse_running_{false};
    std::atomic<bool> shutdown_started_{false};
    std::thread sse_thread_;

    std::mutex sse_client_mutex_;
    std::shared_ptr<httplib::Client> sse_client_;

    std::mutex endpoint_mutex_;
    std::condition_variable endpoint_cv_;
    bool endpoint_ready_{false};

    std::mutex pending_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingResponse>>
        pending_responses_;

    std::mutex notification_mutex_;
    std::condition_variable notification_cv_;
    std::deque<json> notifications_;
};

class TestRunner {
  public:
    explicit TestRunner(McpClient &client) : client_(client) {}

    void run_all() {
        test_initialize();
        test_client_notifications();
        test_basic_requests();
        test_resources_and_prompts();
        test_tool_calls();
        test_protocol_error_methods();
        test_server_notifications();
        print_summary();
    }

  private:
    void check(bool condition, const std::string &name) {
        if (!condition) {
            throw std::runtime_error("Test failed: " + name);
        }
        results_.push_back("[PASS] " + name);
    }

    void test_initialize() {
        json params = {
            {"protocolVersion", "2025-03-26"},
            {"capabilities",
             {{"roots", {{"listChanged", true}}},
              {"sampling", json::object()},
              {"experimental", json::object()}}},
            {"clientInfo",
             {{"name", "cpp-mcp-client"}, {"version", "1.0.0"}}}};
        const json response = client_.initialize(params);
        check(response["jsonrpc"] == "2.0", "initialize returns jsonrpc 2.0");
        check(response.contains("result"), "initialize returns result");
        check(response["result"]["serverInfo"]["name"].is_string(),
              "initialize returns serverInfo.name");
        check(response["result"]["capabilities"].contains("tools"),
              "initialize exposes tools capability");
    }

    void test_client_notifications() {
        auto response = client_.notify_initialized();
        check(response.status == 200 || response.status == 202,
              "notifications/initialized accepted");

        response = client_.notify_message("info", "cpp client smoke message");
        check(response.status == 200 || response.status == 202,
              "notifications/message accepted");

        response =
            client_.notify_progress("client-progress", 25, 100, "warming up");
        check(response.status == 200 || response.status == 202,
              "notifications/progress accepted");

        response = client_.notify_cancelled("local-req", "test cancellation");
        check(response.status == 200 || response.status == 202,
              "notifications/cancelled accepted");

        response = client_.notify_roots_list_changed();
        check(response.status == 200 || response.status == 202,
              "notifications/roots/list_changed accepted");

        response = client_.notify_resources_list_changed();
        check(response.status == 200 || response.status == 202,
              "notifications/resources/list_changed accepted");

        response = client_.notify_resources_updated("bacio:///quote");
        check(response.status == 200 || response.status == 202,
              "notifications/resources/updated accepted");

        response = client_.notify_prompts_list_changed();
        check(response.status == 200 || response.status == 202,
              "notifications/prompts/list_changed accepted");

        response = client_.notify_tools_list_changed();
        check(response.status == 200 || response.status == 202,
              "notifications/tools/list_changed accepted");
    }

    void test_basic_requests() {
        const json ping = client_.ping();
        check(ping.contains("result"), "ping returns result");
        check(ping["result"].is_object(), "ping result is object");

        const json tools = client_.list_tools();
        std::vector<std::string> tool_names;
        for (const auto &tool : tools["result"]["tools"]) {
            tool_names.push_back(tool["name"].get<std::string>());
        }
        check(std::find(tool_names.begin(), tool_names.end(), "sleep") !=
                  tool_names.end(),
              "tools/list includes sleep");
        check(std::find(tool_names.begin(), tool_names.end(), "get_weather") !=
                  tool_names.end(),
              "tools/list includes get_weather");
        check(std::find(tool_names.begin(), tool_names.end(), "logging_test") !=
                  tool_names.end(),
              "tools/list includes logging_test");
        check(std::find(tool_names.begin(), tool_names.end(), "progress_test") !=
                  tool_names.end(),
              "tools/list includes progress_test");
    }

    void test_resources_and_prompts() {
        const json resources = client_.list_resources();
        check(resources["result"]["resources"].is_array(),
              "resources/list returns array");
        check(!resources["result"]["resources"].empty(),
              "resources/list returns at least one resource");
        check(resources["result"]["resources"][0]["uri"] == "bacio:///quote",
              "resources/list returns expected URI");

        const json read = client_.read_resource("bacio:///quote");
        check(read["result"]["contents"].is_array(),
              "resources/read returns contents");
        check(!read["result"]["contents"].empty(),
              "resources/read returns at least one content item");

        const json prompts = client_.list_prompts();
        check(prompts["result"]["prompts"].is_array(),
              "prompts/list returns prompts array");
        check(!prompts["result"]["prompts"].empty(),
              "prompts/list returns at least one prompt");
        check(prompts["result"]["prompts"][0]["name"] == "code-review",
              "prompts/list returns code-review prompt");

        const json prompt = client_.get_prompt("code-review",
                                               {{"language", "C++"}});
        check(prompt["result"]["messages"].is_array(),
              "prompts/get returns messages");
        check(!prompt["result"]["messages"].empty(),
              "prompts/get returns at least one message");
    }

    void test_tool_calls() {
        const json sleep =
            client_.call_tool("sleep", {{"milliseconds", 100}});
        check(sleep["result"]["isError"] == false,
              "tools/call sleep succeeds");
        check(sleep["result"]["content"][0]["text"]
                  .get<std::string>()
                  .find("Waited for 100 milliseconds") != std::string::npos,
              "tools/call sleep returns expected text");

        const json weather =
            client_.call_tool("get_weather",
                              {{"city", "milano"},
                               {"latitude", "45.464664"},
                               {"longitude", "9.188540"}});
        check(weather["result"]["isError"] == false,
              "tools/call get_weather succeeds");
        check(weather["result"]["content"].is_array(),
              "tools/call get_weather returns content array");
    }

    void test_protocol_error_methods() {
        const json subscribe = client_.subscribe_resource("bacio:///quote");
        expect_method_not_found(subscribe, "resources/subscribe returns error");

        const json unsubscribe =
            client_.unsubscribe_resource("bacio:///quote");
        expect_method_not_found(unsubscribe,
                                "resources/unsubscribe returns error");

        const json set_level = client_.logging_set_level("debug");
        expect_method_not_found(set_level, "logging/setLevel returns error");

        const json completion =
            client_.completion_complete({{"type", "ref/prompt"}, {"name", "x"}},
                                        {{"name", "language"}, {"value", "C++"}});
        expect_method_not_found(completion,
                                "completion/complete returns error");

        const json roots = client_.roots_list();
        expect_method_not_found(roots, "roots/list returns error");
    }

    void test_server_notifications() {
        const json logging_test = client_.call_tool("logging_test", json::object());
        check(logging_test["result"]["isError"] == false,
              "tools/call logging_test succeeds");
        const json log_notification = client_.wait_for_notification(
            "notifications/message", std::chrono::seconds(5),
            [](const json &message) {
                return message.contains("params") &&
                       message["params"].contains("data");
            });
        check(log_notification["params"]["level"] == "notice",
              "logging_test emits notifications/message");

        const json progress_call = client_.call_tool(
            "progress_test", json::object(), {{"progressToken", "cpp-progress"}});
        check(progress_call["result"]["isError"] == false,
              "tools/call progress_test succeeds");

        json final_progress = client_.wait_for_notification(
            "notifications/progress", std::chrono::seconds(15),
            [](const json &message) {
                return message.contains("params") &&
                       message["params"].value("progressToken", "") ==
                           "cpp-progress" &&
                       message["params"].value("progress", 0) == 100;
            });
        check(final_progress["params"]["total"] == 100,
              "progress_test reaches 100 percent");
    }

    void expect_method_not_found(const json &response, const std::string &name) {
        check(response.contains("error"), name);
        check(response["error"]["code"] == -32601, name + " code is -32601");
    }

    void print_summary() const {
        std::cout << "========== TEST SUMMARY ==========" << std::endl;
        for (const auto &line : results_) {
            std::cout << line << std::endl;
        }
        std::cout << "==================================" << std::endl;
    }

    McpClient &client_;
    std::vector<std::string> results_;
};

struct Options {
    std::string url = "http://127.0.0.1:8080/mcp";
    std::string transport = "httpstream";
    std::string log_dir = "./logs";
    bool run_tests = false;
};

Options parse_args(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--url" && i + 1 < argc) {
            options.url = argv[++i];
        } else if (arg == "--transport" && i + 1 < argc) {
            options.transport = argv[++i];
        } else if (arg == "--log-dir" && i + 1 < argc) {
            options.log_dir = argv[++i];
        } else if (arg == "--run-tests") {
            options.run_tests = true;
        } else if (arg == "--help") {
            std::cout
                << "Usage: mcp_cpp_client [--url <url>] [--transport "
                   "httpstream|sse] [--log-dir <dir>] [--run-tests]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    return options;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parse_args(argc, argv);
        const UrlParts url = parse_url(options.url);
        const auto mode =
            options.transport == "sse" ? McpClient::TransportMode::LegacySse
                                        : McpClient::TransportMode::HttpStream;

        McpClient client(url, mode, options.log_dir);
        if (!options.run_tests) {
            std::cout << "Client initialized. Use --run-tests to execute MCP "
                         "end-to-end validation."
                      << std::endl;
            return 0;
        }

        TestRunner runner(client);
        runner.run_all();
        client.shutdown();
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }
}
