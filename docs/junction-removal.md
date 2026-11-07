# 删除含目录联接的路径

mcdk 会在运行时包目录、存档目录和 Lane 目录里放置指向用户源码或真实游戏数据的目录联接（junction）。删除这些目录时，遇到联接只能摘掉链接本身，绝不能递归进入目标。所有这类删除都必须调用 `MCDevTool::removeAllNoFollow`（`include/mcdevtool/env.h`），判断某个条目是不是链接用 `MCDevTool::isLink`。

## 为什么不能直接用 `std::filesystem::remove_all`

C++ 标准只规定 `remove_all` 不跟随符号链接（symlink），没有规定 junction 怎么处理。junction 是否被当作链接，完全由标准库实现决定。

**MSVC STL 是安全的**，靠的是两层实现定义的行为，换标准库就必须重新验证：

- `remove_all` 会先尝试直接删除，只有收到 `ERROR_DIRECTORY_NOT_EMPTY` 时才递归。而 [`RemoveDirectoryW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-removedirectoryw) 删除 junction 时，不管目标是否为空都只删链接，所以第一步就会成功。
- 删除父目录时一定会走递归分支。MSVC 把 junction 归为 `file_type::junction`，不是 `directory`，所以递归不会进入 junction。

另外，MSVC 下 `is_symlink(junction)` 永远返回 `false`，所以不能用 `is_symlink` 判断条目是不是链接。

**其他标准库不保证这一点。** 例如 MinGW 使用的 libstdc++ 会把 junction 当作普通目录，`remove_all` 删除父目录时会进入 junction，把用户源码一起删掉。实际已经有人因为用 MinGW 构建而丢失了工程。

## 实现

`env.cpp` 用 `_MSVC_STL_VERSION` 判断当前用的是不是 MSVC STL。这里判断的是标准库而不是编译器：clang-cl 用的是 MSVC STL，可以走 MSVC 分支；用 libc++ 的 clang 不能。

- **MSVC STL：** 直接调用 `std::filesystem::remove_all`。
- **Windows 上的其他标准库：** 用 Win32 API 自己递归删除。
  - 打开条目时带 `FILE_FLAG_OPEN_REPARSE_POINT`，读到的是条目本身的属性和 reparse tag，不跟随链接。
  - 只有名称代理类（name surrogate）reparse point 才算链接，包括 junction 和符号链接。OneDrive 占位目录这类 reparse point 仍按普通目录递归删除。
  - 遇到链接时不递归，目录链接用 `RemoveDirectoryW` 删除，文件链接用 `DeleteFileW` 删除。
  - 目标已经不存在的悬空 junction 也能正常删掉。
- **POSIX：** 标准保证 `remove_all` 不跟随符号链接，所以直接调用。
