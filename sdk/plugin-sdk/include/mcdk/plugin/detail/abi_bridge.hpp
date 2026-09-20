#pragma once
// C++ ↔ C ABI 的握手层。
// 这里是「消化」实际发生的地方：本文件之上（context.hpp / console.hpp / 用户代码）
#include <string>
#include <string_view>
#include <type_traits>

#include "../abi/core.h"

namespace mcdk::detail {
// 字符串
// std::string_view → mcdk_str。借用语义：被引用的存储必须在本次 ABI 调用
    [[nodiscard]] inline mcdk_str toAbi(std::string_view text) noexcept {
        mcdk_str out;
        out.ptr = text.data();
        out.len = text.size();
        return out;
    }

    // mcdk_str → string_view。零拷贝，寿命同样只到本次调用返回。
    [[nodiscard]] inline std::string_view toView(mcdk_str text) noexcept {
        if (text.ptr == nullptr || text.len == 0) {
            return {};
        }
        return std::string_view(text.ptr, text.len);
    }

    // mcdk_str → std::string。深拷贝。凡是需要跨越本次调用保留的内容都必须用它，
    // 这是 02-abi-contract.md §6「借用」规则在插件侧的落实。
    [[nodiscard]] inline std::string toString(mcdk_str text) { return std::string(toView(text)); }

    // ---------------------------------------------------------------
    // 接口表获取与能力探测
    // ---------------------------------------------------------------

    template <class Table>
    [[nodiscard]] const Table* getInterface(const mcdk_host_info& host, const char* name, uint32_t version) noexcept {
        if (host.get_interface == nullptr) {
            return nullptr;
        }
        const auto* table = static_cast<const Table*>(host.get_interface(name, version));
        if (table == nullptr) {
            return nullptr;
        }
        // struct_size 连首字段都放不下，说明宿主给了个不合法的表。
        if (table->struct_size < sizeof(uint32_t)) {
            return nullptr;
        }
        return table;
    }

    // 追加式演进的能力探测：宿主的表若比插件编译时的定义短，尾部字段就不存在。
    // 用法 ifaceHas(table, &mcdk_iface_console::log_colored)。
    template <class Table, class Field>
    [[nodiscard]] constexpr bool ifaceHas(const Table* table, Field Table::* member) noexcept {
        if (table == nullptr) {
            return false;
        }
        // 取成员相对表首的字节偏移。Table 是标准布局的 POD，该计算良定义。
        const auto* base   = reinterpret_cast<const unsigned char*>(table);
        const auto* field  = reinterpret_cast<const unsigned char*>(&(table->*member));
        const auto  offset = static_cast<std::size_t>(field - base);
        if (table->struct_size < offset + sizeof(Field)) {
            return false;
        }
        return table->*member != nullptr;
    }

} // namespace mcdk::detail
