// Copyright © 2025 GlacieTeam. All rights reserved.
//
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once
#include <nbt-c/Macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nbtio_buffer {
    uint8_t* data;
    size_t   size;
};
NBT_API void nbtio_buffer_destroy(nbtio_buffer* buffer);

enum TagType {
    Tag_End       = 0,
    Tag_Byte      = 1,
    Tag_Short     = 2,
    Tag_Int       = 3,
    Tag_Long      = 4,
    Tag_Float     = 5,
    Tag_Double    = 6,
    Tag_ByteArray = 7,
    Tag_String    = 8,
    Tag_List      = 9,
    Tag_Compound  = 10,
    Tag_IntArray  = 11,
    Tag_LongArray = 12,
};

enum Snbt_Format {
    Snbt_Minimize                  = 0,
    Snbt_CompoundLineFeed          = 1,
    Snbt_ListArrayLineFeed         = 2,
    Snbt_PrettyFilePrint           = 3,
    Snbt_BinaryArrayLineFeed       = 4,
    Snbt_ArrayLineFeed             = 6,
    Snbt_AlwaysLineFeed            = 7,
    Snbt_ForceLineFeedIgnoreIndent = 8,
    Snbt_ForceAscii                = 16,
    Snbt_ForceQuote                = 32,
    Snbt_Classic                   = 35,
    Snbt_ForceUppercase            = 64,
    Snbt_MarkIntTag                = 128,
    Snbt_MarkDoubleTag             = 256,
    Snbt_MarkExtra                 = 384,
    Snbt_CommentMarks              = 512,
    Snbt_Jsonify                   = 551,
};

enum NBT_FileFormat {
    NBT_Format_Invalid                      = -1,
    NBT_Format_LittleEndianBinary           = 0,
    NBT_Format_LittleEndianBinaryWithHeader = 1,
    NBT_Format_BigEndianBinary              = 2,
    NBT_Format_BigEndianBinaryWithHeader    = 3,
    NBT_Format_BedrockNetwork               = 4,
};

enum NBT_CompressionType {
    NBT_Compression_None = 0,
    NBT_Compression_Gzip = 1,
    NBT_Compression_Zlib = 2,
};

// Any Tag
NBT_API TagType nbt_any_tag_get_type(void* handle);
NBT_API bool    nbt_any_tag_equals(void* handle, void* other);
NBT_API void*   nbt_any_tag_copy(void* handle);
NBT_API size_t  nbt_any_tag_hash(void* handle);
NBT_API void    nbt_any_tag_write(void* handle, void* stream);
NBT_API void    nbt_any_tag_load(void* handle, void* stream);
NBT_API void    nbt_any_tag_destroy(void* handle);

NBT_API nbtio_buffer* nbt_any_tag_to_snbt(void* handle, Snbt_Format format, uint8_t indent);
NBT_API nbtio_buffer* nbt_any_tag_to_json(void* handle, uint8_t indent);

// EndTag
NBT_API void* nbt_end_tag_create();

// ByteTag
NBT_API void*   nbt_byte_tag_create(uint8_t value);
NBT_API void    nbt_byte_tag_set_value(void* handle, uint8_t value);
NBT_API uint8_t nbt_byte_tag_get_value(void* handle);

// ShortTag
NBT_API void* nbt_short_tag_create(short value);
NBT_API void  nbt_short_tag_set_value(void* handle, short value);
NBT_API short nbt_short_tag_get_value(void* handle);

// IntTag
NBT_API void* nbt_int_tag_create(int value);
NBT_API void  nbt_int_tag_set_value(void* handle, int value);
NBT_API int   nbt_int_tag_get_value(void* handle);

// LongTag
NBT_API void*   nbt_long_tag_create(int64_t value);
NBT_API void    nbt_long_tag_set_value(void* handle, int64_t value);
NBT_API int64_t nbt_long_tag_get_value(void* handle);

// FloatTag
NBT_API void* nbt_float_tag_create(float value);
NBT_API void  nbt_float_tag_set_value(void* handle, float value);
NBT_API float nbt_float_tag_get_value(void* handle);

// DoubleTag
NBT_API void*  nbt_double_tag_create(double value);
NBT_API void   nbt_double_tag_set_value(void* handle, double value);
NBT_API double nbt_double_tag_get_value(void* handle);

// ByteArrayTag
NBT_API void*         nbt_byte_array_tag_create(const uint8_t* data, size_t size);
NBT_API void          nbt_byte_array_tag_set_value(void* handle, const uint8_t* data, size_t size);
NBT_API nbtio_buffer* nbt_byte_array_tag_get_value(void* handle);

// StringTag
NBT_API void*         nbt_string_tag_create(const char* data, size_t size);
NBT_API void          nbt_string_tag_set_value(void* handle, const char* data, size_t size);
NBT_API nbtio_buffer* nbt_string_tag_get_value(void* handle);

// ListTag
NBT_API void*  nbt_list_tag_create();
NBT_API size_t nbt_list_tag_size(void* handle);
NBT_API void   nbt_list_tag_add_tag(void* handle, void* tag);
NBT_API void*  nbt_list_tag_get_tag(void* handle, size_t index);
NBT_API bool   nbt_list_tag_remove_tag(void* handle, size_t index);
NBT_API void   nbt_list_tag_clear(void* handle);
NBT_API bool   nbt_list_tag_set_tag(void* handle, size_t index, void* tag);

// CompoundTag
NBT_API void*         nbt_compound_tag_create();
NBT_API size_t        nbt_compound_tag_size(void* handle);
NBT_API void          nbt_compound_tag_set_tag(void* handle, const char* key, void* tag);
NBT_API void*         nbt_compound_tag_get_tag(void* handle, const char* key);
NBT_API nbtio_buffer* nbt_compound_tag_get_key_index(void* handle, size_t index);
NBT_API void*         nbt_compound_tag_get_tag_index(void* handle, size_t index);
NBT_API bool          nbt_compound_tag_has_tag(void* handle, const char* key);
NBT_API bool          nbt_compound_tag_remove_tag(void* handle, const char* key);
NBT_API void          nbt_compound_tag_clear(void* handle);

NBT_API nbtio_buffer* nbt_compound_tag_to_binary_nbt(void* handle, bool little_endian, bool write_header);
NBT_API nbtio_buffer* nbt_compound_tag_to_network_nbt(void* handle);

NBT_API void* nbt_compound_tag_from_binary_nbt(const uint8_t* data, size_t size, bool little_endian, bool read_header);
NBT_API void* nbt_compound_tag_from_network_nbt(const uint8_t* data, size_t size);
NBT_API void* nbt_compound_tag_from_snbt(const uint8_t* data, size_t size);

// IntArrayTag
NBT_API void*  nbt_int_array_tag_create();
NBT_API size_t nbt_int_array_tag_size(void* handle);
NBT_API void   nbt_int_array_tag_add_value(void* handle, int value);
NBT_API int    nbt_int_array_tag_get_value(void* handle, size_t index);
NBT_API bool   nbt_int_array_tag_remove_value(void* handle, size_t index);
NBT_API void   nbt_int_array_tag_clear(void* handle);
NBT_API bool   nbt_int_array_tag_set_value(void* handle, size_t index, int value);

// LongArrayTag
NBT_API void*   nbt_long_array_tag_create();
NBT_API size_t  nbt_long_array_tag_size(void* handle);
NBT_API void    nbt_long_array_tag_add_value(void* handle, int64_t value);
NBT_API int64_t nbt_long_array_tag_get_value(void* handle, size_t index);
NBT_API bool    nbt_long_array_tag_remove_value(void* handle, size_t index);
NBT_API void    nbt_long_array_tag_clear(void* handle);
NBT_API bool    nbt_long_array_tag_set_value(void* handle, size_t index, int64_t value);

// FileIO
NBT_API void* nbt_parse_from_file(const char* path, NBT_FileFormat format);
NBT_API bool  nbt_save_to_file(
     void*               handle,
     const char*         path,
     NBT_FileFormat      format,
     NBT_CompressionType compressionType,
     int                 compressionLevel
 );
NBT_API void* nbt_parse_snbt_from_file(const char* path);
NBT_API bool  nbt_save_snbt_to_file(void* handle, const char* path, Snbt_Format format, uint8_t indent);

NBT_API bool nbt_validate_file(const char* path, NBT_FileFormat format, bool fmmap, bool strictMatchSize);
NBT_API bool nbt_validate_content(const uint8_t* data, size_t size, NBT_FileFormat format, bool strictMatchSize);

NBT_API NBT_FileFormat nbt_detect_file_format(const char* path, bool fmmap);
NBT_API NBT_FileFormat nbt_detect_content_format(const uint8_t* data, size_t size);

NBT_API NBT_CompressionType nbt_detect_file_compression_type(const char* path, bool fmmap);
NBT_API NBT_CompressionType nbt_detect_content_compression_type(const uint8_t* data, size_t size);

#ifdef __cplusplus
}
#endif