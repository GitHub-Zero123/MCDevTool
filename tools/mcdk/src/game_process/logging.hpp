#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <MCDevLink/Protocol/Safaia.hpp>
#include <MCDevLink/Runtime.hpp>

namespace mcdk {
    class LogBuffer;
}

namespace mcdk::detail {

    using LineHandler = std::function<void(std::string)>;

    struct GameLogHandlers {
        LineHandler output;
        LineHandler traceback;
    };

    [[nodiscard]] GameLogHandlers createGameLogHandlers(
        bool needLogBuffer,
        const std::shared_ptr<LogBuffer>& logBuffer,
        const std::shared_ptr<LogBuffer>& errorBuffer
    );

    class PipeReaderThreads {
    public:
        ~PipeReaderThreads();

        void start(
            HANDLE stdoutPipe,
            HANDLE stderrPipe,
            bool filterPython,
            const LineHandler& stdoutCallback,
            const LineHandler& stderrCallback
        );
        void join();

    private:
        static void cancelAndJoin(std::thread& thread) noexcept;

        std::thread mStdout;
        std::thread mStderr;
    };

    class SafaiaLogReceiver {
    public:
        explicit SafaiaLogReceiver(DWORD processId);

        void setLineHandlers(LineHandler outputHandler, LineHandler tracebackHandler);
        [[nodiscard]] std::error_code start();
        [[nodiscard]] MCDevLink::Endpoint localEndpoint() const;
        void poll();
        void stop();

    private:
        struct StreamState {
            std::string buffer;
            bool tracebackActive = false;
        };

        static MCDevLink::Protocol::SafaiaOptions createOptions(DWORD processId);
        void drain();
        static bool startsTraceback(std::string_view line);
        static bool isTracebackChainSeparator(std::string_view line);
        static bool isIdentifierStart(char value);
        static bool isIdentifierContinuation(char value);
        static bool isQualifiedIdentifier(std::string_view value);
        static bool hasKnownExceptionSuffix(std::string_view name);
        static bool isExceptionTerminator(std::string_view line);
        static bool isIndentedTracebackLine(std::string_view line);
        static bool startsMidTraceback(std::string_view line);
        void dispatchLine(StreamState& stream, std::string line);
        void flush(MCDevLink::SessionId sessionId);
        void flushAll();

        LineHandler mOutputHandler;
        LineHandler mTracebackHandler;
        std::unordered_map<MCDevLink::SessionId, StreamState> mStreams;
        MCDevLink::Runtime mRuntime;
        MCDevLink::Protocol::SafaiaService mService;
    };

} // namespace mcdk::detail
