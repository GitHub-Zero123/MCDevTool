/**
 * @file mcp_server.cpp
 * @brief Implementation of the MCP server
 *
 * This file implements the server-side functionality for the Model Context Protocol.
 * Supports 2025-03-26 Streamable HTTP transport with backward compatibility for 2024-11-05 SSE.
 */

#include "mcp_server.h"

namespace mcp {


    server::server(const server::configuration& conf)
    : host_(conf.host),
      port_(conf.port),
      name_(conf.name),
      version_(conf.version),
      sse_endpoint_(conf.sse_endpoint),
      msg_endpoint_(conf.msg_endpoint),
      streamable_endpoint_(conf.streamable_endpoint),
      enable_legacy_sse_(conf.enable_legacy_sse),
      thread_pool_(conf.threadpool_size) {
#ifdef MCP_SSL
        if (conf.ssl.server_cert_path && conf.ssl.server_private_key_path) {
            if (!std::filesystem::exists(*conf.ssl.server_cert_path)) {
                LOG_ERROR("SSL certificate file '", *conf.ssl.server_cert_path, "' not found");
            }

            if (!std::filesystem::exists(*conf.ssl.server_private_key_path)) {
                LOG_ERROR("SSL key file '", *conf.ssl.server_private_key_path, "' not found");
            }

            http_server_ = std::make_unique<httplib::SSLServer>(
                conf.ssl.server_cert_path->c_str(),
                conf.ssl.server_private_key_path->c_str()
            );
        } else {
            http_server_ = std::make_unique<httplib::Server>();
        }
#else
        http_server_ = std::make_unique<httplib::Server>();
#endif
    }

    server::~server() { stop(); }


    bool server::start(bool blocking) {
        if (running_) {
            return true; // Already running
        }

        LOG_INFO("Starting MCP server on ", host_, ":", port_);

        // Register default handlers for standard MCP methods (return empty results if not overridden)
        auto register_default = [this](const std::string& method, method_handler handler) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (method_handlers_.find(method) == method_handlers_.end()) {
                method_handlers_[method] = handler;
            }
        };

        register_default("resources/list", [](const json&, const std::string&) -> json {
            return json{{"resources", json::array()}};
        });
        register_default("resources/templates/list", [](const json&, const std::string&) -> json {
            return json{{"resourceTemplates", json::array()}};
        });
        register_default("prompts/list", [](const json&, const std::string&) -> json {
            return json{{"prompts", json::array()}};
        });
        register_default("tools/list", [](const json&, const std::string&) -> json {
            return json{{"tools", json::array()}};
        });

        // Setup CORS handling
        http_server_->Options(".*", [](const httplib::Request& req, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type, Mcp-Session-Id, Last-Event-ID");
            res.status = 204; // No Content
        });

        // Setup Streamable HTTP endpoint (2025-03-26)
        http_server_->Post(streamable_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
            this->handle_streamable_post(req, res);
            LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"POST ", req.path, " HTTP/1.1\" ", res.status);
        });

        http_server_->Get(streamable_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
            this->handle_streamable_get(req, res);
            LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"GET ", req.path, " HTTP/1.1\" ", res.status);
        });

        http_server_->Delete(streamable_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
            this->handle_streamable_delete(req, res);
            LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"DELETE ", req.path, " HTTP/1.1\" ", res.status);
        });

        // Setup legacy SSE transport (2024-11-05 backward compatibility)
        if (enable_legacy_sse_) {
            http_server_->Post(msg_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
                this->handle_jsonrpc(req, res);
                LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"POST ", req.path, " HTTP/1.1\" ", res.status);
            });

            http_server_->Get(sse_endpoint_.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
                this->handle_sse(req, res);
                LOG_INFO(req.remote_addr, ":", req.remote_port, " - \"GET ", req.path, " HTTP/1.1\" ", res.status);
            });
        }

        // Start resource check thread (only start in non-blocking mode)
        if (!blocking) {
            maintenance_thread_run_ = true;
            maintenance_thread_     = std::make_unique<std::thread>([this]() {
                while (true) {
                    // Check inactive sessions every 60 seconds
                    std::unique_lock<std::mutex> lock(maintenance_mutex_);
                    auto should_exit = maintenance_cond_.wait_for(lock, std::chrono::seconds(60), [this] {
                        return !maintenance_thread_run_;
                    });
                    if (should_exit) {
                        LOG_INFO("Maintenance thread exiting");
                        return;
                    }
                    lock.unlock();

                    try {
                        check_inactive_sessions();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Exception in maintenance thread: ", e.what());
                    } catch (...) {
                        LOG_ERROR("Unknown exception in maintenance thread");
                    }
                }
            });
        }

        // Start server
        if (blocking) {
            running_ = true;
            LOG_INFO("Starting server in blocking mode");
            if (!http_server_->listen(host_.c_str(), port_)) {
                running_ = false;
                LOG_ERROR("Failed to start server on ", host_, ":", port_);
                return false;
            }
            return true;
        } else {
            // Start server in a separate thread
            server_thread_ = std::make_unique<std::thread>([this]() {
                LOG_INFO("Starting server in separate thread");
                if (!http_server_->listen(host_.c_str(), port_)) {
                    LOG_ERROR("Failed to start server on ", host_, ":", port_);
                    running_ = false;
                    return;
                }
            });
            running_       = true;
            return true;
        }
    }

    void server::stop() {
        if (!running_) {
            return;
        }

        LOG_INFO("Stopping MCP server on ", host_, ":", port_);
        running_ = false;

        // Close maintenance thread
        if (maintenance_thread_ && maintenance_thread_->joinable()) {
            {
                std::unique_lock<std::mutex> lock(maintenance_mutex_);
                maintenance_thread_run_ = false;
            }

            maintenance_cond_.notify_one();

            try {
                maintenance_thread_->join();
            } catch (...) {
                maintenance_thread_->detach();
            }
        }

        // Copy all dispatchers and threads to avoid holding the lock for too long
        std::vector<std::shared_ptr<event_dispatcher>> dispatchers_to_close;
        std::vector<std::unique_ptr<std::thread>>      threads_to_join;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Copy all dispatchers
            dispatchers_to_close.reserve(session_dispatchers_.size());
            for (const auto& [_, dispatcher] : session_dispatchers_) {
                dispatchers_to_close.push_back(dispatcher);
            }

            // Copy all threads
            threads_to_join.reserve(sse_threads_.size());
            for (auto& [_, thread] : sse_threads_) {
                if (thread && thread->joinable()) {
                    threads_to_join.push_back(std::move(thread));
                }
            }

            // Clear the maps
            session_dispatchers_.clear();
            sse_threads_.clear();
            session_initialized_.clear();
            session_protocol_version_.clear();
        }

        // Close all sessions
        for (const auto& [session_id, _] : session_dispatchers_) {
            close_session(session_id);
        }

        // Give threads some time to handle close events
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Wait for threads to finish outside the lock (with timeout limit)
        const auto timeout_point = std::chrono::steady_clock::now() + std::chrono::seconds(2);

        for (auto& thread : threads_to_join) {
            if (!thread || !thread->joinable()) {
                continue;
            }

            if (std::chrono::steady_clock::now() >= timeout_point) {
                // If timeout reached, detach remaining threads
                LOG_WARNING("Thread join timeout reached, detaching remaining threads");
                thread->detach();
                continue;
            }

            // Try using timeout join
            bool joined = false;
            try {
                // Create future and promise for timeout join
                std::promise<void> thread_done;
                auto               future = thread_done.get_future();

                // Try join in another thread
                std::thread join_helper([&thread, &thread_done]() {
                    try {
                        thread->join();
                        thread_done.set_value();
                    } catch (...) {
                        try {
                            thread_done.set_exception(std::current_exception());
                        } catch (...) {}
                    }
                });

                // Wait for join to complete or timeout
                if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
                    future.get(); // Get possible exception
                    joined = true;
                }

                // Process join_helper thread
                if (join_helper.joinable()) {
                    if (joined) {
                        join_helper.join();
                    } else {
                        join_helper.detach();
                    }
                }
            } catch (...) {
                joined = false;
            }

            // If join fails, then detach
            if (!joined) {
                try {
                    thread->detach();
                } catch (...) {
                    // Ignore exceptions
                }
            }
        }

        if (server_thread_ && server_thread_->joinable()) {
            http_server_->stop();
            try {
                server_thread_->join();
            } catch (...) {
                server_thread_->detach();
            }
        } else {
            http_server_->stop();
        }

        LOG_INFO("MCP server stopped");
    }

    bool server::is_running() const { return running_; }

    void server::set_server_info(const std::string& name, const std::string& version) {
        std::lock_guard<std::mutex> lock(mutex_);
        name_    = name;
        version_ = version;
    }

    void server::set_capabilities(const json& capabilities) {
        std::lock_guard<std::mutex> lock(mutex_);
        capabilities_ = capabilities;
    }

    void server::register_method(const std::string& method, method_handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        method_handlers_[method] = handler;
    }

    void server::register_notification(const std::string& method, notification_handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        notification_handlers_[method] = handler;
    }

    void server::register_resource(const std::string& path, std::shared_ptr<resource> resource) {
        std::lock_guard<std::mutex> lock(mutex_);
        resources_[path] = resource;

        // Auto-set resources capability
        if (!capabilities_.contains("resources")) {
            capabilities_["resources"] = json::object({{"subscribe", true}});
        }

        // Register methods for resource access
        if (method_handlers_.find("resources/read") == method_handlers_.end()) {
            method_handlers_["resources/read"] = [this](const json& params, const std::string& session_id) -> json {
                if (!params.contains("uri")) {
                    throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
                }

                std::string uri = params["uri"];
                auto        it  = resources_.find(uri);
                if (it == resources_.end()) {
                    throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
                }

                json contents = json::array();
                contents.push_back(it->second->read());

                return json{{"contents", contents}};
            };
        }

        if (method_handlers_.find("resources/list") == method_handlers_.end()) {
            method_handlers_["resources/list"] = [this](const json& params, const std::string& session_id) -> json {
                json resources = json::array();

                for (const auto& [uri, res] : resources_) {
                    resources.push_back(res->get_metadata());
                }

                json result = {{"resources", resources}};

                if (params.contains("cursor")) {
                    result["nextCursor"] = "";
                }

                return result;
            };
        }

        if (method_handlers_.find("resources/subscribe") == method_handlers_.end()) {
            method_handlers_["resources/subscribe"] =
                [this](const json& params, const std::string& session_id) -> json {
                if (!params.contains("uri")) {
                    throw mcp_exception(error_code::invalid_params, "Missing 'uri' parameter");
                }

                std::string uri = params["uri"];
                auto        it  = resources_.find(uri);
                if (it == resources_.end()) {
                    throw mcp_exception(error_code::invalid_params, "Resource not found: " + uri);
                }

                return json::object();
            };
        }

        if (method_handlers_.find("resources/templates/list") == method_handlers_.end()) {
            method_handlers_["resources/templates/list"] =
                [this](const json& params, const std::string& session_id) -> json { return json::array(); };
        }
    }

    void server::register_tool(const tool& tool, tool_handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        tools_[tool.name] = std::make_pair(tool, handler);

        // Auto-set tools capability so clients know we support tools
        if (!capabilities_.contains("tools")) {
            capabilities_["tools"] = json::object();
        }

        // Register methods for tool listing and calling
        if (method_handlers_.find("tools/list") == method_handlers_.end()) {
            method_handlers_["tools/list"] = [this](const json& params, const std::string& session_id) -> json {
                json tools_json = json::array();
                for (const auto& [name, tool_pair] : tools_) {
                    tools_json.push_back(tool_pair.first.to_json());
                }
                return json{{"tools", tools_json}};
            };
        }

        if (method_handlers_.find("tools/call") == method_handlers_.end()) {
            method_handlers_["tools/call"] = [this](const json& params, const std::string& session_id) -> json {
                if (!params.contains("name")) {
                    throw mcp_exception(error_code::invalid_params, "Missing 'name' parameter");
                }

                std::string tool_name = params["name"];
                auto        it        = tools_.find(tool_name);
                if (it == tools_.end()) {
                    throw mcp_exception(error_code::invalid_params, "Tool not found: " + tool_name);
                }

                json tool_args = params.contains("arguments") ? params["arguments"] : json::array();

                if (tool_args.is_string()) {
                    try {
                        tool_args = json::parse(tool_args.get<std::string>());
                    } catch (const json::exception& e) {
                        throw mcp_exception(
                            error_code::invalid_params,
                            "Invalid JSON arguments: " + std::string(e.what())
                        );
                    }
                }

                json tool_result = {{"isError", false}};

                try {
                    json raw_result = it->second.second(tool_args, session_id);

                    // Support structured output (2025-03-26):
                    // If handler returns object with "content" key, extract it
                    // If it also has "structuredContent", include that too
                    if (raw_result.is_object() && raw_result.contains("content")) {
                        tool_result["content"] = raw_result["content"];
                        if (raw_result.contains("isError")) {
                            tool_result["isError"] = raw_result["isError"];
                        }
                        if (raw_result.contains("structuredContent")) {
                            tool_result["structuredContent"] = raw_result["structuredContent"];
                        }
                    } else {
                        // Legacy: entire return value is the content array
                        tool_result["content"] = raw_result;
                    }
                } catch (const std::exception& e) {
                    tool_result["isError"] = true;
                    tool_result["content"] = json::array({{{"type", "text"}, {"text", e.what()}}});
                }

                return tool_result;
            };
        }
    }

    void server::register_session_cleanup(const std::string& key, session_cleanup_handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        session_cleanup_handler_[key] = handler;
    }

    std::vector<tool> server::get_tools() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<tool>           tools;

        for (const auto& [name, tool_pair] : tools_) {
            tools.push_back(tool_pair.first);
        }

        return tools;
    }

    void server::set_auth_handler(auth_handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        auth_handler_ = handler;
    }

    void server::handle_sse(const httplib::Request& req, httplib::Response& res) {
        std::string session_id  = generate_session_id();
        std::string session_uri = msg_endpoint_ + "?session_id=" + session_id;

        // Setup SSE response headers
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("Access-Control-Allow-Origin", "*");

        // Create session-specific event dispatcher
        auto session_dispatcher = std::make_shared<event_dispatcher>();

        // Initialize activity time
        session_dispatcher->update_activity();

        // Add session dispatcher to mapping table
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_dispatchers_[session_id] = session_dispatcher;
        }

        // Create session thread
        auto thread = std::make_unique<std::thread>([this, res, session_id, session_uri, session_dispatcher]() {
            try {
                // Send initial session URI
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::stringstream ss;
                ss << "event: endpoint\r\ndata: " << session_uri << "\r\n\r\n";
                session_dispatcher->send_event(ss.str());

                // Update activity time (after sending message)
                session_dispatcher->update_activity();

                // Send periodic heartbeats to detect connection status
                int heartbeat_count = 0;
                while (running_ && !session_dispatcher->is_closed()) {
                    std::this_thread::sleep_for(
                        std::chrono::seconds(5) + std::chrono::milliseconds(rand() % 500)
                    ); // NOTE: DO NOT set it the same as the timeout of wait_event

                    if (session_dispatcher->is_closed() || !running_) {
                        break;
                    }

                    std::stringstream heartbeat;
                    heartbeat << "event: heartbeat\r\ndata: " << heartbeat_count++ << "\r\n\r\n";

                    try {
                        bool sent = session_dispatcher->send_event(heartbeat.str());
                        if (!sent) {
                            LOG_WARNING("Failed to send heartbeat, client may have closed connection: ", session_id);
                            break;
                        }

                        // Update activity time (heartbeat successful)
                        session_dispatcher->update_activity();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Failed to send heartbeat: ", e.what());
                        break;
                    }
                }
            } catch (const std::exception& e) {
                LOG_ERROR("SSE session thread exception: ", session_id, ", ", e.what());
            }

            close_session(session_id);
        });

        // Store thread
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sse_threads_[session_id] = std::move(thread);
        }

        // Setup chunked content provider
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, session_id, session_dispatcher](size_t /* offset */, httplib::DataSink& sink) {
                try {
                    // Check if session is closed - directly get status from dispatcher, reduce lock contention
                    if (session_dispatcher->is_closed()) {
                        return false;
                    }

                    // Update activity time (received request)
                    session_dispatcher->update_activity();

                    // Wait for event (timeout is normal, just retry)
                    bool result = session_dispatcher->wait_event(&sink);
                    if (!result) {
                        // Check if dispatcher was actually closed vs just timed out
                        if (session_dispatcher->is_closed() || !running_) {
                            LOG_DEBUG("SSE connection closed: ", session_id);
                            close_session(session_id);
                            return false;
                        }
                        // Timeout - just continue waiting
                        return true;
                    }

                    // Update activity time (successfully received message)
                    session_dispatcher->update_activity();

                    return true;
                } catch (const std::exception& e) {
                    LOG_ERROR("SSE content provider exception: ", e.what());

                    close_session(session_id);

                    return false;
                }
            }
        );
    }

    void server::handle_jsonrpc(const httplib::Request& req, httplib::Response& res) {
        // Setup response headers
        res.set_header("Content-Type", "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");

        // Handle OPTIONS request (CORS pre-flight)
        if (req.method == "OPTIONS") {
            res.status = 204; // No Content
            return;
        }

        // Get session ID
        auto        it         = req.params.find("session_id");
        std::string session_id = it != req.params.end() ? it->second : "";

        // Update session activity time
        if (!session_id.empty()) {
            std::shared_ptr<event_dispatcher> dispatcher;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto                        disp_it = session_dispatchers_.find(session_id);
                if (disp_it != session_dispatchers_.end()) {
                    dispatcher = disp_it->second;
                }
            }

            if (dispatcher) {
                dispatcher->update_activity();
            }
        }

        // Parse request
        json req_json;
        try {
            req_json = json::parse(req.body);
        } catch (const json::exception& e) {
            LOG_ERROR("Failed to parse JSON request: ", e.what());
            res.status = 400;
            res.set_content("{\"error\":\"Invalid JSON\"}", "application/json");
            return;
        }

        // Check if session exists
        std::shared_ptr<event_dispatcher> dispatcher;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto                        disp_it = session_dispatchers_.find(session_id);
            if (disp_it == session_dispatchers_.end()) {
                // Handle ping request
                if (req_json["method"] == "ping") {
                    res.status = 202;
                    res.set_content("Accepted", "text/plain");
                    return;
                }
                LOG_ERROR("Session not found: ", session_id);
                res.status = 404;
                res.set_content("{\"error\":\"Session not found\"}", "application/json");
                return;
            }
            dispatcher = disp_it->second;
        }

        // Create request object
        request mcp_req;
        try {
            mcp_req.jsonrpc = req_json["jsonrpc"].get<std::string>();
            if (req_json.contains("id") && !req_json["id"].is_null()) {
                mcp_req.id = req_json["id"];
            }
            mcp_req.method = req_json["method"].get<std::string>();
            if (req_json.contains("params")) {
                mcp_req.params = req_json["params"];
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to create request object: ", e.what());
            res.status = 400;
            res.set_content("{\"error\":\"Invalid request format\"}", "application/json");
            return;
        }

        // If it is a notification (no ID), process it directly and return 202 status code
        if (mcp_req.is_notification()) {
            // Process it asynchronously in the thread pool
            thread_pool_.enqueue([this, mcp_req, session_id]() { process_request(mcp_req, session_id); });

            // Return 202 Accepted
            res.status = 202;
            res.set_content("Accepted", "text/plain");
            return;
        }

        // For requests with ID, process it asynchronously in the thread pool and return the result via SSE
        thread_pool_.enqueue([this, mcp_req, session_id, dispatcher]() {
            // Process the request
            json response_json = process_request(mcp_req, session_id);

            // Send response via SSE
            std::stringstream ss;
            ss << "event: message\r\ndata: " << response_json.dump() << "\r\n\r\n";
            bool result = dispatcher->send_event(ss.str());

            if (!result) {
                LOG_ERROR("Failed to send response via SSE: session_id=", session_id);
            }
        });

        // Return 202 Accepted
        res.status = 202;
        res.set_content("Accepted", "text/plain");
    }

    json server::process_request(const request& req, const std::string& session_id) {
        // Check if it is a notification
        if (req.is_notification()) {
            if (req.method == "notifications/initialized") {
                set_session_initialized(session_id, true);
            }
            return json::object();
        }

        // Process method call
        try {
            LOG_INFO("Processing method call: ", req.method);

            // Special case: initialization
            if (req.method == "initialize") {
                return handle_initialize(req, session_id);
            } else if (req.method == "ping") {
                return response::create_success(req.id, json::object()).to_json();
            }

            if (!is_session_initialized(session_id)) {
                LOG_WARNING("Session not initialized: ", session_id);
                return response::create_error(req.id, error_code::invalid_request, "Session not initialized").to_json();
            }

            // Find registered method handler
            method_handler handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto                        it = method_handlers_.find(req.method);
                if (it != method_handlers_.end()) {
                    handler = it->second;
                }
            }

            if (handler) {
                // Call handler
                LOG_INFO("Calling method handler: ", req.method);
                json result = handler(req.params, session_id);

                // Create success response
                LOG_INFO("Method call successful: ", req.method);
                return response::create_success(req.id, result).to_json();
            }

            // Method not found
            LOG_WARNING("Method not found: ", req.method);
            return response::create_error(req.id, error_code::method_not_found, "Method not found: " + req.method)
                .to_json();
        } catch (const mcp_exception& e) {
            // MCP exception
            LOG_ERROR("MCP exception: ", e.what(), ", code: ", static_cast<int>(e.code()));
            return response::create_error(req.id, e.code(), e.what()).to_json();
        } catch (const std::exception& e) {
            // Other exceptions
            LOG_ERROR("Exception while processing request: ", e.what());
            return response::create_error(
                       req.id,
                       error_code::internal_error,
                       "Internal error: " + std::string(e.what())
            )
                .to_json();
        } catch (...) {
            // Unknown exception
            LOG_ERROR("Unknown exception while processing request");
            return response::create_error(req.id, error_code::internal_error, "Unknown internal error").to_json();
        }
    }

    json server::handle_initialize(const request& req, const std::string& session_id) {
        const json& params = req.params;

        // Version negotiation (2025-03-26: support multiple versions)
        if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string()) {
            LOG_ERROR("Missing or invalid protocolVersion parameter");
            return response::create_error(
                       req.id,
                       error_code::invalid_params,
                       "Expected string for 'protocolVersion' parameter"
            )
                .to_json();
        }

        std::string requested_version = params["protocolVersion"].get<std::string>();
        LOG_INFO("Client requested protocol version: ", requested_version);

        // Negotiate version - accept any supported version, fallback to latest
        std::string negotiated_version = negotiate_version(requested_version);

        if (!is_version_supported(requested_version)) {
            LOG_WARNING(
                "Client requested unsupported version: ",
                requested_version,
                ", negotiating to: ",
                negotiated_version
            );
        }

        // Store negotiated version for this session
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_protocol_version_[session_id] = negotiated_version;
        }

        // Extract client info
        std::string client_name    = "UnknownClient";
        std::string client_version = "UnknownVersion";

        if (params.contains("clientInfo")) {
            if (params["clientInfo"].contains("name")) {
                client_name = params["clientInfo"]["name"];
            }
            if (params["clientInfo"].contains("version")) {
                client_version = params["clientInfo"]["version"];
            }
        }

        // Log connection
        LOG_INFO("Client connected: ", client_name, " ", client_version, " (protocol: ", negotiated_version, ")");

        // Return server info and capabilities
        json server_info = {{"name", name_}, {"version", version_}};

        json result =
            {{"protocolVersion", negotiated_version}, {"capabilities", capabilities_}, {"serverInfo", server_info}};

        LOG_INFO("Initialization successful, waiting for notifications/initialized notification");

        return response::create_success(req.id, result).to_json();
    }

    void server::send_jsonrpc(const std::string& session_id, const json& message) {
        // Check if session ID is valid
        if (session_id.empty()) {
            LOG_WARNING("Cannot send message to empty session_id");
            return;
        }

        // Get session dispatcher
        std::shared_ptr<event_dispatcher> dispatcher;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto                        it = session_dispatchers_.find(session_id);
            if (it == session_dispatchers_.end()) {
                LOG_ERROR("Session not found: ", session_id);
                return;
            }
            dispatcher = it->second;
        }

        // Confirm dispatcher is still valid
        if (!dispatcher || dispatcher->is_closed()) {
            LOG_WARNING("Cannot send to closed session: ", session_id);
            return;
        }

        // Send message
        std::stringstream ss;
        ss << "event: message\r\ndata: " << message.dump() << "\r\n\r\n";
        bool result = dispatcher->send_event(ss.str());

        if (!result) {
            LOG_ERROR("Failed to send message to session: ", session_id);
        }
    }

    void server::send_request(const std::string& session_id, const request& req) {
        send_jsonrpc(session_id, req.to_json());
    }

    bool server::is_session_initialized(const std::string& session_id) const {
        // Check if session ID is valid
        if (session_id.empty()) {
            return false;
        }

        try {
            std::lock_guard<std::mutex> lock(mutex_);
            auto                        it = session_initialized_.find(session_id);
            return (it != session_initialized_.end() && it->second);
        } catch (const std::exception& e) {
            LOG_ERROR("Exception checking if session is initialized: ", e.what());
            return false;
        }
    }

    void server::set_session_initialized(const std::string& session_id, bool initialized) {
        // Check if session ID is valid
        if (session_id.empty()) {
            LOG_WARNING("Cannot set initialization state for empty session_id");
            return;
        }

        try {
            std::lock_guard<std::mutex> lock(mutex_);
            // Check if session still exists
            auto it = session_dispatchers_.find(session_id);
            if (it == session_dispatchers_.end()) {
                LOG_WARNING("Cannot set initialization state for non-existent session: ", session_id);
                return;
            }
            session_initialized_[session_id] = initialized;
        } catch (const std::exception& e) {
            LOG_ERROR("Exception setting session initialization state: ", e.what());
        }
    }

    std::string server::generate_session_id() const {
        std::random_device              rd;
        std::mt19937                    gen(rd());
        std::uniform_int_distribution<> dis(0, 15);

        std::stringstream ss;
        ss << std::hex;

        // UUID format: 8-4-4-4-12 hexadecimal digits
        for (int i = 0; i < 8; ++i) {
            ss << dis(gen);
        }
        ss << "-";

        for (int i = 0; i < 4; ++i) {
            ss << dis(gen);
        }
        ss << "-";

        for (int i = 0; i < 4; ++i) {
            ss << dis(gen);
        }
        ss << "-";

        for (int i = 0; i < 4; ++i) {
            ss << dis(gen);
        }
        ss << "-";

        for (int i = 0; i < 12; ++i) {
            ss << dis(gen);
        }

        return ss.str();
    }

    void server::check_inactive_sessions() {
        if (!running_) return;

        const auto now     = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::minutes(60); // 1 hour inactive then close

        std::vector<std::string> sessions_to_close;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [session_id, dispatcher] : session_dispatchers_) {
                if (now - dispatcher->last_activity() > timeout) {
                    // Exceeded idle time limit
                    sessions_to_close.push_back(session_id);
                }
            }
        }

        // Close inactive sessions
        for (const auto& session_id : sessions_to_close) {
            LOG_INFO("Closing inactive session: ", session_id);

            close_session(session_id);
        }
    }

    bool server::set_mount_point(const std::string& mount_point, const std::string& dir, httplib::Headers headers) {
        return http_server_->set_mount_point(mount_point, dir, headers);
    }

    // ========================================================================
    // Streamable HTTP Transport (2025-03-26)
    // ========================================================================

    void server::handle_streamable_post(const httplib::Request& req, httplib::Response& res) {
        // CORS and content type headers
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Mcp-Session-Id, Last-Event-ID");

        // Parse JSON body
        json req_json;
        try {
            req_json = json::parse(req.body);
        } catch (const json::exception& e) {
            LOG_ERROR("Failed to parse JSON request: ", e.what());
            res.status = 400;
            res.set_content(
                "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}",
                "application/json"
            );
            return;
        }

        // Handle JSON-RPC batch request (2025-03-26)
        if (req_json.is_array()) {
            std::string session_id;
            auto        hdr = req.headers.find("Mcp-Session-Id");
            if (hdr != req.headers.end()) session_id = hdr->second;

            if (session_id.empty()) {
                res.status = 400;
                res.set_content(
                    "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Missing Mcp-Session-Id for batch "
                    "request\"}}",
                    "application/json"
                );
                return;
            }

            // Verify session exists
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (session_dispatchers_.find(session_id) == session_dispatchers_.end()) {
                    res.status = 404;
                    res.set_content(
                        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Session not found\"}}",
                        "application/json"
                    );
                    return;
                }
            }

            json batch_result = process_batch_request(req_json, session_id);
            if (batch_result.empty() || (batch_result.is_array() && batch_result.size() == 0)) {
                // All were notifications
                res.status = 202;
                return;
            }
            res.status = 200;
            res.set_content(batch_result.dump(), "application/json");
            return;
        }

        // Single request
        request mcp_req;
        try {
            mcp_req.jsonrpc = req_json.value("jsonrpc", "2.0");
            if (req_json.contains("id") && !req_json["id"].is_null()) {
                mcp_req.id = req_json["id"];
            }
            mcp_req.method = req_json.value("method", "");
            if (req_json.contains("params")) {
                mcp_req.params = req_json["params"];
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to create request object: ", e.what());
            res.status = 400;
            res.set_content(
                "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Invalid request format\"}}",
                "application/json"
            );
            return;
        }

        // Get session ID from Mcp-Session-Id header
        std::string session_id;
        auto        hdr = req.headers.find("Mcp-Session-Id");
        if (hdr != req.headers.end()) session_id = hdr->second;

        // For initialize, create a new session
        if (mcp_req.method == "initialize") {
            session_id      = generate_session_id();
            auto dispatcher = std::make_shared<event_dispatcher>();
            dispatcher->update_activity();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                session_dispatchers_[session_id] = dispatcher;
            }
            LOG_INFO("Streamable HTTP: created session ", session_id);
        } else if (mcp_req.method != "ping") {
            // Validate session for non-initialize, non-ping requests
            if (session_id.empty()) {
                res.status = 400;
                json err =
                    response::create_error(mcp_req.id, error_code::invalid_request, "Missing Mcp-Session-Id header")
                        .to_json();
                res.set_content(err.dump(), "application/json");
                return;
            }

            // Verify session exists and update activity
            std::shared_ptr<event_dispatcher> dispatcher;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto                        it = session_dispatchers_.find(session_id);
                if (it == session_dispatchers_.end()) {
                    res.status = 404;
                    json err =
                        response::create_error(mcp_req.id, error_code::invalid_request, "Session not found").to_json();
                    res.set_content(err.dump(), "application/json");
                    return;
                }
                dispatcher = it->second;
            }
            if (dispatcher) dispatcher->update_activity();
        }

        // Handle notification (no response needed)
        if (mcp_req.is_notification()) {
            thread_pool_.enqueue([this, mcp_req, session_id]() { process_request(mcp_req, session_id); });
            res.status = 202;
            return;
        }

        // Process request synchronously and return response directly
        json response_json = process_request(mcp_req, session_id);

        // Set Mcp-Session-Id header for initialize response
        if (mcp_req.method == "initialize") {
            res.set_header("Mcp-Session-Id", session_id);
        }

        res.status = 200;
        res.set_content(response_json.dump(), "application/json");
    }

    void server::handle_streamable_get(const httplib::Request& req, httplib::Response& res) {
        // Get session ID from Mcp-Session-Id header
        auto hdr = req.headers.find("Mcp-Session-Id");
        if (hdr == req.headers.end()) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing Mcp-Session-Id header\"}", "application/json");
            return;
        }

        std::string session_id = hdr->second;

        // Find session dispatcher
        std::shared_ptr<event_dispatcher> dispatcher;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto                        it = session_dispatchers_.find(session_id);
            if (it == session_dispatchers_.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"Session not found\"}", "application/json");
                return;
            }
            dispatcher = it->second;
        }

        // Setup SSE response for server-initiated messages
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("Access-Control-Allow-Origin", "*");

        LOG_INFO("Streamable HTTP: SSE stream opened for session ", session_id);

        res.set_chunked_content_provider(
            "text/event-stream",
            [this, session_id, dispatcher](size_t /* offset */, httplib::DataSink& sink) {
                try {
                    if (dispatcher->is_closed() || !running_) {
                        return false;
                    }

                    dispatcher->update_activity();

                    bool result = dispatcher->wait_event(&sink);
                    if (!result) {
                        LOG_WARNING("Streamable HTTP: SSE stream closed for session ", session_id);
                        return false;
                    }

                    dispatcher->update_activity();
                    return true;
                } catch (const std::exception& e) {
                    LOG_ERROR("Streamable HTTP: SSE exception: ", e.what());
                    return false;
                }
            }
        );
    }

    void server::handle_streamable_delete(const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");

        // Get session ID from Mcp-Session-Id header
        auto hdr = req.headers.find("Mcp-Session-Id");
        if (hdr == req.headers.end()) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing Mcp-Session-Id header\"}", "application/json");
            return;
        }

        std::string session_id = hdr->second;

        // Check if session exists
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_dispatchers_.find(session_id) == session_dispatchers_.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"Session not found\"}", "application/json");
                return;
            }
        }

        LOG_INFO("Streamable HTTP: session terminated by client: ", session_id);
        close_session(session_id);
        res.status = 200;
    }

    json server::process_batch_request(const json& batch, const std::string& session_id) {
        json results = json::array();

        for (const auto& item : batch) {
            request mcp_req;
            try {
                mcp_req.jsonrpc = item.value("jsonrpc", "2.0");
                if (item.contains("id") && !item["id"].is_null()) {
                    mcp_req.id = item["id"];
                }
                mcp_req.method = item.value("method", "");
                if (item.contains("params")) {
                    mcp_req.params = item["params"];
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Failed to parse batch item: ", e.what());
                results.push_back(
                    response::create_error(nullptr, error_code::invalid_request, "Invalid request in batch").to_json()
                );
                continue;
            }

            // Notifications don't produce responses
            if (mcp_req.is_notification()) {
                process_request(mcp_req, session_id);
                continue;
            }

            json result = process_request(mcp_req, session_id);
            results.push_back(result);
        }

        return results;
    }

    void server::close_session(const std::string& session_id) {
        // Clean up resources safely
        try {
            for (const auto& [key, handler] : session_cleanup_handler_) {
                handler(key);
            }

            // Copy resources to be processed
            std::shared_ptr<event_dispatcher> dispatcher_to_close;
            std::unique_ptr<std::thread>      thread_to_release;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                // Get dispatcher pointer
                auto dispatcher_it = session_dispatchers_.find(session_id);
                if (dispatcher_it != session_dispatchers_.end()) {
                    dispatcher_to_close = dispatcher_it->second;
                    session_dispatchers_.erase(dispatcher_it);
                }

                // Get thread pointer
                auto thread_it = sse_threads_.find(session_id);
                if (thread_it != sse_threads_.end()) {
                    thread_to_release = std::move(thread_it->second);
                    sse_threads_.erase(thread_it);
                }

                // Clean up initialization status
                session_initialized_.erase(session_id);

                // Clean up protocol version
                session_protocol_version_.erase(session_id);
            }

            // Close dispatcher outside the lock
            if (dispatcher_to_close && !dispatcher_to_close->is_closed()) {
                dispatcher_to_close->close();
            }

            // Release thread resources
            if (thread_to_release) {
                thread_to_release.release();
            }
        } catch (const std::exception& e) {
            LOG_WARNING("Exception while cleaning up session resources: ", session_id, ", ", e.what());
        } catch (...) {
            LOG_WARNING("Unknown exception while cleaning up session resources: ", session_id);
        }
    }

} // namespace mcp
