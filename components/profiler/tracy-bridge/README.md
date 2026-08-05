# MCDevTool Tracy Bridge

This Windows x64 DLL embeds the Tracy 0.11.1 server worker used by the game's Native profiler endpoint. It exposes a
narrow C ABI; no Tracy or STL C++ types cross the DLL boundary.

The root MCDevTool build owns normal integration:

```powershell
cmake --build <build-dir> --target mcdk
```

Building `mcdk` also builds `mcdev-tracy-bridge.dll` directly beside `mcdk.exe`:

```text
mcdk.exe
mcdev-tracy-bridge.dll
licenses/native-profiler/
```

`mcdk` loads only this fixed sibling DLL and validates its required exports, bridge API version, and Tracy protocol
version at runtime. The DLL and executable are released together; there is no separate component manifest.

Tracy and Capstone are immutable FetchContent archives with SHA-256 verification. Downloads are shared through
`MCDEV_DEPS_DOWNLOAD_CACHE` (or the environment variable with the same name); each top-level build retains its own
`_deps` source and binary trees. No repository Preset controls whether this runtime component is built or packaged.

The bridge still permits one active capture per process. Tracy 0.11.1 server code contains process-global accounting
and progress state, so concurrent workers require process isolation and additional stress testing.
