set(_tracy_common_dir "${tracy_SOURCE_DIR}/public/common")
set(_tracy_server_dir "${tracy_SOURCE_DIR}/server")

set(_tracy_server_sources
    "${_tracy_common_dir}/tracy_lz4.cpp"
    "${_tracy_common_dir}/tracy_lz4hc.cpp"
    "${_tracy_common_dir}/TracySocket.cpp"
    "${_tracy_common_dir}/TracyStackFrames.cpp"
    "${_tracy_common_dir}/TracySystem.cpp"
    "${_tracy_server_dir}/TracyMemory.cpp"
    "${_tracy_server_dir}/TracyMmap.cpp"
    "${_tracy_server_dir}/TracyPrint.cpp"
    "${_tracy_server_dir}/TracySysUtil.cpp"
    "${_tracy_server_dir}/TracyTaskDispatch.cpp"
    "${_tracy_server_dir}/TracyTextureCompression.cpp"
    "${_tracy_server_dir}/TracyThreadCompress.cpp"
    "${_tracy_server_dir}/TracyWorker.cpp"
)

set(_tracy_zstd_dir "${tracy_SOURCE_DIR}/zstd")
set(_tracy_zstd_sources
    decompress/zstd_ddict.c
    decompress/zstd_decompress_block.c
    decompress/huf_decompress.c
    decompress/zstd_decompress.c
    common/zstd_common.c
    common/error_private.c
    common/xxhash.c
    common/entropy_common.c
    common/debug.c
    common/threading.c
    common/pool.c
    common/fse_decompress.c
    compress/zstd_ldm.c
    compress/zstd_compress_superblock.c
    compress/zstd_opt.c
    compress/zstd_compress_sequences.c
    compress/fse_compress.c
    compress/zstd_double_fast.c
    compress/zstd_compress.c
    compress/zstd_compress_literals.c
    compress/hist.c
    compress/zstdmt_compress.c
    compress/zstd_lazy.c
    compress/huf_compress.c
    compress/zstd_fast.c
    dictBuilder/zdict.c
    dictBuilder/cover.c
    dictBuilder/divsufsort.c
    dictBuilder/fastcover.c
)
list(TRANSFORM _tracy_zstd_sources PREPEND "${_tracy_zstd_dir}/")

add_library(mcdev-tracy-zstd STATIC ${_tracy_zstd_sources})
target_include_directories(mcdev-tracy-zstd PUBLIC "${_tracy_zstd_dir}")
target_compile_definitions(mcdev-tracy-zstd PRIVATE ZSTD_DISABLE_ASM)

add_library(mcdev-tracy-server STATIC ${_tracy_server_sources})
target_compile_features(mcdev-tracy-server PUBLIC cxx_std_20)
target_compile_definitions(mcdev-tracy-server PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN NO_PARALLEL_SORT)
target_include_directories(mcdev-tracy-server PUBLIC
    "${_tracy_common_dir}"
    "${_tracy_server_dir}"
    "${capstone_SOURCE_DIR}/include/capstone"
)
target_link_libraries(mcdev-tracy-server PUBLIC mcdev-tracy-zstd capstone ws2_32 dbghelp)

if(MSVC)
    target_compile_options(mcdev-tracy-server PRIVATE /W0 /utf-8)
    target_compile_options(mcdev-tracy-zstd PRIVATE /W0)
endif()
