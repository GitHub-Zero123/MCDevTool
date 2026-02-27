/**
 * @file mcp_logger.h
 * @brief Simple logger
 */

#ifndef MCP_LOGGER_H
#define MCP_LOGGER_H

#include <iostream>
#include <sstream>
#include <string>
#include <mutex>
#include <chrono>
#include <iomanip>

namespace mcp {

    enum class log_level { debug, info, warning, error };

    class logger {
    public:
        static logger& instance() {
            static logger instance;
            return instance;
        }

        void set_level(log_level level) {
            std::lock_guard<std::mutex> lock(mutex_);
            level_ = level;
        }

        template <typename... Args>
        void debug(Args&&... args) {
            log(log_level::debug, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void info(Args&&... args) {
            log(log_level::info, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void warning(Args&&... args) {
            log(log_level::warning, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void error(Args&&... args) {
            log(log_level::error, std::forward<Args>(args)...);
        }

    private:
        logger() : level_(log_level::info) {}

        template <typename T>
        void log_impl(std::stringstream& ss, T&& arg) {
            ss << std::forward<T>(arg);
        }

        template <typename T, typename... Args>
        void log_impl(std::stringstream& ss, T&& arg, Args&&... args) {
            ss << std::forward<T>(arg);
            log_impl(ss, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void log(log_level level, Args&&... args) {
            if (level < level_) {
                return;
            }

            std::stringstream ss;

            // Add timestamp
            auto now    = std::chrono::system_clock::now();
            auto now_c  = std::chrono::system_clock::to_time_t(now);
            auto now_tm = std::localtime(&now_c);

            // Format: [MCP] [TYPE] timestamp content
            // MCP tag and type tag share the same color, timestamp and content in dark gray
            const char* color = "";
            const char* label = "";
            switch (level) {
            case log_level::debug:
                color = "\033[36m";
                label = "DEBUG";
                break; // Cyan
            case log_level::info:
                color = "\033[32m";
                label = "INFO";
                break; // Green
            case log_level::warning:
                color = "\033[33m";
                label = "WARNING";
                break; // Yellow
            case log_level::error:
                color = "\033[31m";
                label = "ERROR";
                break; // Red
            }

            // INFO: gray text; others: text color matches type color
            const char* text_color = (level == log_level::info) ? "\033[90m" : color;

            ss << color << "[MCP] [" << label << "]\033[0m " << text_color << std::put_time(now_tm, "%Y-%m-%d %H:%M:%S")
               << " ";

            log_impl(ss, std::forward<Args>(args)...);
            ss << "\033[0m";

            // Output log
            std::lock_guard<std::mutex> lock(mutex_);
            std::cerr << ss.str() << std::endl;
        }

        log_level  level_;
        std::mutex mutex_;
    };

#define LOG_DEBUG(...)   mcp::logger::instance().debug(__VA_ARGS__)
#define LOG_INFO(...)    mcp::logger::instance().info(__VA_ARGS__)
#define LOG_WARNING(...) mcp::logger::instance().warning(__VA_ARGS__)
#define LOG_ERROR(...)   mcp::logger::instance().error(__VA_ARGS__)

    inline void set_log_level(log_level level) { mcp::logger::instance().set_level(level); }

} // namespace mcp

#endif // MCP_LOGGER_H