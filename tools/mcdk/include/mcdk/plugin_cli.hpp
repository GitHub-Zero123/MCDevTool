#pragma once

//
// `mcdk plugin ...` 子命令的实现入口。规范见 docs/plugin-system/06-loading.md §5。
//

#include <string>

namespace mcdk {

    // 列出 .mcdev.json 中的声明及其解析结果。
    int pluginList();

    // 把匹配 key（id 或 path）的那条声明的 enable 置为 enabled。
    int pluginSetEnabled(const std::string& key, bool enabled);

    // 追加一条声明。写入前会打印清单的 id、version 与 permissions 供用户确认。
    int pluginAdd(const std::string& rawPath);

    // 移除匹配 key（id 或 path）的声明。
    int pluginRemove(const std::string& key);

} // namespace mcdk
