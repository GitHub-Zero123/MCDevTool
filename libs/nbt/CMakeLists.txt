cmake_minimum_required(VERSION 3.15)

project(NBT LANGUAGES CXX C)

# zlib library
set(ZLIB_SOURCES
    libs/zlib-1.3.1/adler32.c
    libs/zlib-1.3.1/compress.c
    libs/zlib-1.3.1/crc32.c
    libs/zlib-1.3.1/deflate.c
    libs/zlib-1.3.1/gzclose.c
    libs/zlib-1.3.1/gzlib.c
    libs/zlib-1.3.1/gzread.c
    libs/zlib-1.3.1/gzwrite.c
    libs/zlib-1.3.1/infback.c
    libs/zlib-1.3.1/inffast.c
    libs/zlib-1.3.1/inflate.c
    libs/zlib-1.3.1/inftrees.c
    libs/zlib-1.3.1/trees.c
    libs/zlib-1.3.1/uncompr.c
    libs/zlib-1.3.1/zutil.c
)

add_library(zlib_internal STATIC ${ZLIB_SOURCES})
target_include_directories(zlib_internal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/zlib-1.3.1)
if(MSVC)
    target_compile_definitions(zlib_internal PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# binarystream library
set(BINARYSTREAM_SOURCES
    libs/binarystream/src/binarystream/BinaryStream.cpp
    libs/binarystream/src/binarystream/ReadOnlyBinaryStream.cpp
)

add_library(binarystream STATIC ${BINARYSTREAM_SOURCES})
target_compile_features(binarystream PRIVATE cxx_std_23)
target_include_directories(binarystream PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libs/binarystream/include)

# NBT library
set(NBT_SOURCES
    src/nbt/NBT.cpp
    src/nbt/detail/CompressionUtils.cpp
    src/nbt/detail/FileUtils.cpp
    src/nbt/detail/SnbtDeserializer.cpp
    src/nbt/detail/SnbtSerializer.cpp
    src/nbt/detail/StringUtils.cpp
    src/nbt/detail/Validate.cpp
    src/nbt/io/BytesDataInput.cpp
    src/nbt/io/BytesDataOutput.cpp
    src/nbt/io/NBTIO.cpp
    src/nbt/types/ByteArrayTag.cpp
    src/nbt/types/ByteTag.cpp
    src/nbt/types/CompoundTag.cpp
    src/nbt/types/CompoundTagVariant.cpp
    src/nbt/types/DoubleTag.cpp
    src/nbt/types/EndTag.cpp
    src/nbt/types/FloatTag.cpp
    src/nbt/types/IntArrayTag.cpp
    src/nbt/types/IntTag.cpp
    src/nbt/types/ListTag.cpp
    src/nbt/types/LongArrayTag.cpp
    src/nbt/types/LongTag.cpp
    src/nbt/types/NbtContent.cpp
    src/nbt/types/NbtFile.cpp
    src/nbt/types/ShortTag.cpp
    src/nbt/types/StringTag.cpp
    src/nbt/types/Tag.cpp
    src/nbt/types/TypedNbt.cpp
)

add_library(NBT STATIC ${NBT_SOURCES})
target_compile_features(NBT PUBLIC cxx_std_23)
target_include_directories(NBT PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(NBT PUBLIC binarystream zlib_internal)

if(WIN32)
    target_compile_definitions(NBT PRIVATE NOMINMAX UNICODE)
endif()

if(MSVC)
    target_compile_options(NBT PRIVATE /EHsc /utf-8 /W4)
    target_compile_definitions(NBT PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(NBT PRIVATE -Wall -Wextra -Wconversion -pedantic -fexceptions)
endif()
