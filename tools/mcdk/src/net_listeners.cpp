#include <mcdk/net_listeners.hpp>

#include <algorithm>
#include <array>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#endif

namespace mcdk::net {

#ifdef _WIN32

    namespace {

        // GetExtendedTcpTable 需要"先问长度再取数据"，两个地址族的取法完全一致。
        template <typename TableType>
        bool readTcpTable(ULONG family, TCP_TABLE_CLASS tableClass, std::vector<std::byte>& buffer, TableType*& table) {
            ULONG      size  = 0;
            const auto first = GetExtendedTcpTable(nullptr, &size, FALSE, family, tableClass, 0);
            if (first != ERROR_INSUFFICIENT_BUFFER) {
                return false;
            }
            buffer.assign(size, std::byte{});
            table = reinterpret_cast<TableType*>(buffer.data());
            return GetExtendedTcpTable(table, &size, FALSE, family, tableClass, 0) == NO_ERROR;
        }

        void appendIpv4Listeners(std::vector<TcpListener>& listeners) {
            std::vector<std::byte>  buffer;
            MIB_TCPTABLE_OWNER_PID* table = nullptr;
            if (!readTcpTable(AF_INET, TCP_TABLE_OWNER_PID_LISTENER, buffer, table)) {
                return;
            }
            for (DWORD index = 0; index < table->dwNumEntries; ++index) {
                const auto& row     = table->table[index];
                const auto  address = ntohl(row.dwLocalAddr);

                TcpListener listener;
                listener.port       = ntohs(static_cast<u_short>(row.dwLocalPort));
                listener.pid        = row.dwOwningPid;
                listener.isIpv6     = false;
                listener.loopback   = (address >> 24) == 127;
                listener.anyAddress = address == 0;
                listeners.push_back(listener);
            }
        }

        void appendIpv6Listeners(std::vector<TcpListener>& listeners) {
            std::vector<std::byte>   buffer;
            MIB_TCP6TABLE_OWNER_PID* table = nullptr;
            if (!readTcpTable(AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, buffer, table)) {
                return;
            }
            for (DWORD index = 0; index < table->dwNumEntries; ++index) {
                const auto& row = table->table[index];

                const bool allZero =
                    std::all_of(std::begin(row.ucLocalAddr), std::end(row.ucLocalAddr), [](UCHAR byte) {
                        return byte == 0;
                    });
                const bool isLoopback =
                    !allZero
                    && std::all_of(std::begin(row.ucLocalAddr), std::end(row.ucLocalAddr) - 1, [](UCHAR byte) {
                           return byte == 0;
                       })
                    && row.ucLocalAddr[15] == 1;

                TcpListener listener;
                listener.port       = ntohs(static_cast<u_short>(row.dwLocalPort));
                listener.pid        = row.dwOwningPid;
                listener.isIpv6     = true;
                listener.loopback   = isLoopback;
                listener.anyAddress = allZero;
                listeners.push_back(listener);
            }
        }

    } // namespace

    bool listenerEnumerationSupported() noexcept { return true; }

    std::vector<TcpListener> listListeners() {
        std::vector<TcpListener> listeners;
        appendIpv4Listeners(listeners);
        appendIpv6Listeners(listeners);
        return listeners;
    }

    std::string processImageName(std::uint32_t pid) {
        // PROCESS_QUERY_LIMITED_INFORMATION 足以读取同用户进程的映像路径，无需提权
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr) {
            return {};
        }
        wchar_t    path[MAX_PATH]{};
        DWORD      length = MAX_PATH;
        const bool ok     = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
        CloseHandle(process);
        if (!ok || length == 0) {
            return {};
        }

        std::wstring_view full(path, length);
        if (const auto separator = full.find_last_of(L"\\/"); separator != std::wstring_view::npos) {
            full.remove_prefix(separator + 1);
        }

        const int bytes =
            WideCharToMultiByte(CP_UTF8, 0, full.data(), static_cast<int>(full.size()), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) {
            return {};
        }
        std::string name(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, full.data(), static_cast<int>(full.size()), name.data(), bytes, nullptr, nullptr
        );
        return name;
    }

    bool isProcessAlive(std::uint32_t pid) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (process == nullptr) {
            return false;
        }
        const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        return alive;
    }

#else

    bool listenerEnumerationSupported() noexcept { return false; }

    std::vector<TcpListener> listListeners() { return {}; }

    std::string processImageName(std::uint32_t) { return {}; }

    bool isProcessAlive(std::uint32_t) { return false; }

#endif

    bool isPortListening(int port) {
        const auto listeners = listListeners();
        return std::any_of(listeners.begin(), listeners.end(), [port](const TcpListener& listener) {
            return listener.port == port;
        });
    }

} // namespace mcdk::net
