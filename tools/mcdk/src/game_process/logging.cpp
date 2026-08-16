#include "logging.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>
#include <vector>

#include <mcdk/console_output.hpp>
#include <mcdk/log_buffer.hpp>
#include <mcdk/utils.hpp>

namespace {
    template<typename ProcessLine>
    void processBufferAppend(
        std::string& lineBuffer,
        const char* buffer,
        std::size_t length,
        bool filterPython,
        ProcessLine&& processLine
    ) {
        lineBuffer.append(buffer, length);

        std::size_t consumed = 0;
        std::size_t position = 0;
        while ((position = lineBuffer.find('\n', consumed)) != std::string::npos) {
            std::size_t lineEnd = position;
            if (lineEnd != consumed && lineBuffer[lineEnd - 1] == '\r') {
                --lineEnd;
            }
            const std::string_view line(lineBuffer.data() + consumed, lineEnd - consumed);
            consumed = position + 1;
            if (filterPython && line.find("[Python] ") == std::string::npos) {
                continue;
            }
            processLine(std::string(line));
        }
        if (consumed != 0) {
            lineBuffer.erase(0, consumed);
        }
    }

    void readPipeThread(
        HANDLE pipe,
        bool filterPython,
        const mcdk::detail::LineHandler& processLine
    ) {
        constexpr DWORD bufferSize = 4096;
        std::string lineBuffer;
        std::vector<char> buffer(bufferSize);

        while (true) {
            DWORD bytesRead = 0;
            const BOOL ok = ReadFile(pipe, buffer.data(), bufferSize, &bytesRead, nullptr);
            if (!ok) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE) {
                    if (!lineBuffer.empty()) {
                        std::string lastLine = lineBuffer;
                        if (!lastLine.empty() && lastLine.back() == '\r') {
                            lastLine.pop_back();
                        }
                        if (!(filterPython && lastLine.find("[Python] ") == std::string::npos)) {
                            processLine(std::move(lastLine));
                        }
                        lineBuffer.clear();
                    }
                    break;
                }
                break;
            }

            if (bytesRead == 0) {
                if (!lineBuffer.empty()) {
                    std::string lastLine = lineBuffer;
                    if (!lastLine.empty() && lastLine.back() == '\r') {
                        lastLine.pop_back();
                    }
                    if (!(filterPython && lastLine.find("[Python] ") == std::string::npos)) {
                        processLine(std::move(lastLine));
                    }
                    lineBuffer.clear();
                }
                break;
            }

            processBufferAppend(lineBuffer, buffer.data(), bytesRead, filterPython, processLine);
        }
    }
}

namespace mcdk::detail {

    GameLogHandlers createGameLogHandlers(
        bool needLogBuffer,
        const std::shared_ptr<LogBuffer>& logBuffer,
        const std::shared_ptr<LogBuffer>& errorBuffer
    ) {
        GameLogHandlers handlers;
        handlers.output = [needLogBuffer, logBuffer](std::string line) {
            if (line.find(" [INFO][Engine] ") != std::string::npos) {
                return;
            }
            if (line.find("[INFO][Developer]") != std::string::npos) {
                printColoredAtomic(line, ConsoleColor::DarkGray);
                return;
            } else if (containsIgnoreCase(line, "SUC")) {
                printColoredAtomic(line, ConsoleColor::Green);
                return;
            } else if (containsIgnoreCase(line, "ERROR")) {
                printColoredAtomic(line, ConsoleColor::Red);
                return;
            } else if (containsIgnoreCase(line, "WARN")) {
                printColoredAtomic(line, ConsoleColor::Yellow);
                return;
            } else if (containsIgnoreCase(line, "DEBUG")) {
                printColoredAtomic(line, ConsoleColor::Cyan);
                return;
            }
            printColoredAtomic(line, ConsoleColor::Default);
            if (needLogBuffer) {
                logBuffer->add(std::move(line));
            }
        };

        handlers.traceback = [needLogBuffer, logBuffer, errorBuffer](std::string line) {
            constexpr std::string_view filePrefix = "File \"";
            constexpr std::string_view lineMarker = "\", line ";
            std::size_t searchFrom = 0;
            while (true) {
                const auto prefix = line.find(filePrefix, searchFrom);
                if (prefix == std::string::npos) {
                    break;
                }
                const auto pathStart = prefix + filePrefix.size();
                const auto pathEnd   = line.find(lineMarker, pathStart);
                if (pathEnd == std::string::npos) {
                    break;
                }

                const auto pathLength   = pathEnd - pathStart;
                const bool hasBackslash = line.find('\\', pathStart) < pathEnd;
                const bool hasSlash     = line.find('/', pathStart) < pathEnd;
                if (hasBackslash) {
                    std::replace(
                        line.begin() + static_cast<std::ptrdiff_t>(pathStart),
                        line.begin() + static_cast<std::ptrdiff_t>(pathEnd),
                        '\\',
                        '/'
                    );
                } else if (!hasSlash
                           && (pathLength < 3 || line.compare(pathEnd - 3, 3, ".py") != 0)) {
                    std::replace(
                        line.begin() + static_cast<std::ptrdiff_t>(pathStart),
                        line.begin() + static_cast<std::ptrdiff_t>(pathEnd),
                        '.',
                        '/'
                    );
                    line.insert(pathEnd, ".py");
                    searchFrom = pathEnd + 3 + lineMarker.size();
                    continue;
                }
                searchFrom = pathEnd + lineMarker.size();
            }

            printColoredAtomic(line, ConsoleColor::Red);
            if (needLogBuffer) {
                logBuffer->add(line);
                errorBuffer->add(std::move(line));
            }
        };
        return handlers;
    }

    PipeReaderThreads::~PipeReaderThreads() {
        cancelAndJoin(mStdout);
        cancelAndJoin(mStderr);
    }

    void PipeReaderThreads::start(
        HANDLE stdoutPipe,
        HANDLE stderrPipe,
        bool filterPython,
        const LineHandler& stdoutCallback,
        const LineHandler& stderrCallback
    ) {
        mStdout = std::thread(readPipeThread, stdoutPipe, filterPython, stdoutCallback);
        mStderr = std::thread(readPipeThread, stderrPipe, filterPython, stderrCallback);
    }

    void PipeReaderThreads::join() {
        if (mStdout.joinable()) {
            mStdout.join();
        }
        if (mStderr.joinable()) {
            mStderr.join();
        }
    }

    void PipeReaderThreads::cancelAndJoin(std::thread& thread) noexcept {
        if (!thread.joinable()) {
            return;
        }
        CancelSynchronousIo(thread.native_handle());
        try {
            thread.join();
        } catch (...) {
            thread.detach();
        }
    }

    SafaiaLogReceiver::SafaiaLogReceiver(DWORD processId)
        : mService(mRuntime, createOptions(processId)) {
        mService.setLogHandler([this](const MCDevLink::LogEvent& event) {
            auto& stream = mStreams[event.sessionId];
            processBufferAppend(
                stream.buffer,
                event.message.data(),
                event.message.size(),
                false,
                [this, &stream](std::string line) {
                    dispatchLine(stream, std::move(line));
                }
            );
        });
        mService.setSessionHandler([this](const MCDevLink::SessionEvent& event) {
            if (event.state == MCDevLink::SessionState::disconnected) {
                flush(event.sessionId);
            }
        });
        mService.setDiagnosticHandler([](const MCDevLink::DiagnosticEvent& event) {
            if (event.level == MCDevLink::DiagnosticLevel::info) {
                return;
            }
            const auto color = event.level == MCDevLink::DiagnosticLevel::warning
                                 ? ConsoleColor::Yellow
                                 : ConsoleColor::Red;
            printColoredAtomic(event.message, color);
        });
    }

    void SafaiaLogReceiver::setLineHandlers(LineHandler outputHandler, LineHandler tracebackHandler) {
        mOutputHandler    = std::move(outputHandler);
        mTracebackHandler = std::move(tracebackHandler);
    }

    std::error_code SafaiaLogReceiver::start() {
        return mService.start();
    }

    MCDevLink::Endpoint SafaiaLogReceiver::localEndpoint() const {
        return mService.localEndpoint();
    }

    void SafaiaLogReceiver::poll() {
        (void)mRuntime.poll({256, std::chrono::microseconds{1000}});
    }

    void SafaiaLogReceiver::stop() {
        mService.stop();
        drain();
        flushAll();
    }

    MCDevLink::Protocol::SafaiaOptions SafaiaLogReceiver::createOptions(DWORD processId) {
        MCDevLink::Protocol::SafaiaOptions options;
        options.targetProcessId = processId;
        return options;
    }

    void SafaiaLogReceiver::drain() {
        constexpr int maxDrainPolls = 16;
        for (int attempt = 0; attempt < maxDrainPolls; ++attempt) {
            const auto result = mRuntime.poll({256, std::chrono::microseconds{1000}});
            if (result.eventsProcessed == 0) {
                break;
            }
        }
    }

    std::string_view SafaiaLogReceiver::stripPythonPrefix(std::string_view line) {
        constexpr std::string_view prefix = "[Python] ";
        if (line.starts_with(prefix)) {
            line.remove_prefix(prefix.size());
        }
        return line;
    }

    bool SafaiaLogReceiver::startsTraceback(std::string_view line) {
        return stripPythonPrefix(line) == "Traceback (most recent call last):";
    }

    bool SafaiaLogReceiver::isTracebackChainSeparator(std::string_view line) {
        line = stripPythonPrefix(line);
        return line == "During handling of the above exception, another exception occurred:"
            || line == "The above exception was the direct cause of the following exception:";
    }

    bool SafaiaLogReceiver::isIdentifierStart(char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_';
    }

    bool SafaiaLogReceiver::isIdentifierContinuation(char value) {
        return isIdentifierStart(value) || (value >= '0' && value <= '9');
    }

    bool SafaiaLogReceiver::isQualifiedIdentifier(std::string_view value) {
        bool atSegmentStart = true;
        for (const char current : value) {
            if (atSegmentStart) {
                if (!isIdentifierStart(current)) {
                    return false;
                }
                atSegmentStart = false;
            } else if (current == '.') {
                atSegmentStart = true;
            } else if (!isIdentifierContinuation(current)) {
                return false;
            }
        }
        return !value.empty() && !atSegmentStart;
    }

    bool SafaiaLogReceiver::hasKnownExceptionSuffix(std::string_view name) {
        constexpr std::string_view suffixes[] = {
            "Error",
            "Exception",
            "Interrupt",
            "Exit",
            "StopIteration",
        };
        for (const auto suffix : suffixes) {
            if (name.ends_with(suffix)) {
                return true;
            }
        }
        return false;
    }

    bool SafaiaLogReceiver::isExceptionTerminator(std::string_view line) {
        line = stripPythonPrefix(line);
        if (line.empty() || !isIdentifierStart(line.front())) {
            return false;
        }
        const auto colon = line.find(':');
        const auto name  = line.substr(0, colon);
        if (!isQualifiedIdentifier(name)) {
            return false;
        }
        const auto classSeparator = name.rfind('.');
        const auto className = name.substr(classSeparator == std::string_view::npos ? 0 : classSeparator + 1);
        if (hasKnownExceptionSuffix(className)) {
            return true;
        }
        return colon != std::string_view::npos
            && !className.empty()
            && className.front() >= 'A'
            && className.front() <= 'Z';
    }

    bool SafaiaLogReceiver::isIndentedTracebackLine(std::string_view line) {
        line = stripPythonPrefix(line);
        return !line.empty() && (line.front() == ' ' || line.front() == '\t');
    }

    bool SafaiaLogReceiver::startsMidTraceback(std::string_view line) {
        line = stripPythonPrefix(line);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        return line.starts_with("File \"");
    }

    void SafaiaLogReceiver::dispatchLine(StreamState& stream, std::string line) {
        const bool header         = startsTraceback(line);
        const bool chainSeparator = isTracebackChainSeparator(line);
        if (header) {
            stream.tracebackActive = true;
        } else if (!stream.tracebackActive && startsMidTraceback(line)) {
            stream.tracebackActive = true;
        }

        const bool terminator = stream.tracebackActive && isExceptionTerminator(line);
        const bool tracebackLine = header || terminator || chainSeparator
                                || (stream.tracebackActive && isIndentedTracebackLine(line));
        auto& handler = tracebackLine ? mTracebackHandler : mOutputHandler;
        if (handler) {
            handler(std::move(line));
        }
        if (terminator) {
            stream.tracebackActive = false;
        }
    }

    void SafaiaLogReceiver::flush(MCDevLink::SessionId sessionId) {
        const auto found = mStreams.find(sessionId);
        if (found == mStreams.end()) {
            return;
        }
        if (!found->second.buffer.empty()) {
            auto line = std::move(found->second.buffer);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            dispatchLine(found->second, std::move(line));
        }
        mStreams.erase(found);
    }

    void SafaiaLogReceiver::flushAll() {
        while (!mStreams.empty()) {
            flush(mStreams.begin()->first);
        }
    }

} // namespace mcdk::detail
