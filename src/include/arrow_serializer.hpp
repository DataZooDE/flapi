#pragma once

#include <duckdb.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>
// Use DuckDB's bundled ZSTD (in duckdb_zstd namespace)
// Include path has duckdb/third_party/zstd/include so we use <zstd.h>
#include <zstd.h>
// Use vcpkg's LZ4 (in global namespace)
#include <lz4frame.h>
#include <string>
#include <vector>
#include <cstring>
#include <memory>
#include <algorithm>
#include <cctype>

namespace flapi {

/**
 * Configuration for Arrow serialization.
 */
/**
 * Configuration for Arrow serialization.
 *
 * Note: batchSize is currently advisory - actual batching is controlled
 * by DuckDB's chunk fetching mechanism. Full batch size control requires
 * either splitting large chunks or combining small ones, which is
 * planned for a future enhancement.
 */
struct ArrowSerializerConfig {
    size_t batchSize = 8192;          // Advisory rows per batch
    std::string codec;                 // Compression: "", "lz4", "zstd"
    int compressionLevel = 0;          // Compression level (0 = default, 1-22 for zstd)
    size_t maxMemoryBytes = 256 * 1024 * 1024;  // 256 MB default
};

/**
 * Normalize codec name to lowercase.
 */
inline std::string normalizeCodecName(const std::string& codec) {
    std::string normalized = codec;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return normalized;
}

/**
 * Check if a codec name is valid.
 */
inline bool isValidCodec(const std::string& codec) {
    std::string normalized = normalizeCodecName(codec);
    return normalized.empty() || normalized == "zstd" || normalized == "lz4";
}

/**
 * Compress data using ZSTD.
 * Returns empty vector on failure.
 */
inline std::vector<uint8_t> compressZstd(const std::vector<uint8_t>& input, int level = 3) {
    if (input.empty()) {
        return {};
    }

    // Clamp level to valid range (1-22, default 3)
    if (level <= 0) {
        level = 3;
    } else if (level > 22) {
        level = 22;
    }

    size_t compressBound = duckdb_zstd::ZSTD_compressBound(input.size());
    std::vector<uint8_t> output(compressBound);

    size_t compressedSize = duckdb_zstd::ZSTD_compress(
        output.data(), output.size(),
        input.data(), input.size(),
        level
    );

    if (duckdb_zstd::ZSTD_isError(compressedSize)) {
        return {};
    }

    output.resize(compressedSize);
    return output;
}

/**
 * Compress data using LZ4 frame format.
 * Returns empty vector on failure.
 */
inline std::vector<uint8_t> compressLz4(const std::vector<uint8_t>& input) {
    if (input.empty()) {
        return {};
    }

    // Create LZ4 frame preferences
    LZ4F_preferences_t prefs;
    std::memset(&prefs, 0, sizeof(prefs));
    prefs.frameInfo.contentSize = input.size();
    prefs.compressionLevel = 0;  // Default compression level

    size_t compressBound = LZ4F_compressFrameBound(input.size(), &prefs);
    std::vector<uint8_t> output(compressBound);

    size_t compressedSize = LZ4F_compressFrame(
        output.data(), output.size(),
        input.data(), input.size(),
        &prefs
    );

    if (LZ4F_isError(compressedSize)) {
        return {};
    }

    output.resize(compressedSize);
    return output;
}

/**
 * Result of Arrow serialization operation.
 */
struct ArrowSerializationResult {
    bool success = false;
    std::string errorMessage;
    std::vector<uint8_t> data;        // IPC stream bytes
    size_t rowCount = 0;
    size_t batchCount = 0;
    size_t bytesWritten = 0;
};

/**
 * Check if a DuckDB type is supported for Arrow conversion.
 */
inline bool isDuckDBTypeSupported(duckdb_type type) {
    switch (type) {
        case DUCKDB_TYPE_BOOLEAN:
        case DUCKDB_TYPE_TINYINT:
        case DUCKDB_TYPE_SMALLINT:
        case DUCKDB_TYPE_INTEGER:
        case DUCKDB_TYPE_BIGINT:
        case DUCKDB_TYPE_UTINYINT:
        case DUCKDB_TYPE_USMALLINT:
        case DUCKDB_TYPE_UINTEGER:
        case DUCKDB_TYPE_UBIGINT:
        case DUCKDB_TYPE_FLOAT:
        case DUCKDB_TYPE_DOUBLE:
        case DUCKDB_TYPE_VARCHAR:
        case DUCKDB_TYPE_BLOB:
        case DUCKDB_TYPE_DATE:
        case DUCKDB_TYPE_TIME:
        case DUCKDB_TYPE_TIMESTAMP:
        case DUCKDB_TYPE_TIMESTAMP_S:
        case DUCKDB_TYPE_TIMESTAMP_MS:
        case DUCKDB_TYPE_TIMESTAMP_NS:
        case DUCKDB_TYPE_INTERVAL:
        case DUCKDB_TYPE_HUGEINT:
        case DUCKDB_TYPE_DECIMAL:
        case DUCKDB_TYPE_UUID:
            return true;
        default:
            return false;
    }
}

/**
 * Get the Arrow type format string for a DuckDB type.
 * See https://arrow.apache.org/docs/format/CDataInterface.html#data-type-description-format-strings
 */
inline const char* duckdbTypeToArrowFormat(duckdb_type type) {
    switch (type) {
        case DUCKDB_TYPE_BOOLEAN:    return "b";      // boolean
        case DUCKDB_TYPE_TINYINT:    return "c";      // int8
        case DUCKDB_TYPE_SMALLINT:   return "s";      // int16
        case DUCKDB_TYPE_INTEGER:    return "i";      // int32
        case DUCKDB_TYPE_BIGINT:     return "l";      // int64
        case DUCKDB_TYPE_UTINYINT:   return "C";      // uint8
        case DUCKDB_TYPE_USMALLINT:  return "S";      // uint16
        case DUCKDB_TYPE_UINTEGER:   return "I";      // uint32
        case DUCKDB_TYPE_UBIGINT:    return "L";      // uint64
        case DUCKDB_TYPE_FLOAT:      return "f";      // float32
        case DUCKDB_TYPE_DOUBLE:     return "g";      // float64
        case DUCKDB_TYPE_VARCHAR:    return "u";      // utf8 string
        case DUCKDB_TYPE_BLOB:       return "z";      // binary
        case DUCKDB_TYPE_DATE:       return "tdD";    // date32[days]
        case DUCKDB_TYPE_TIME:       return "ttu";    // time64[microseconds]
        case DUCKDB_TYPE_TIMESTAMP:  return "tsu:";   // timestamp[microseconds]
        case DUCKDB_TYPE_TIMESTAMP_S:  return "tss:"; // timestamp[seconds]
        case DUCKDB_TYPE_TIMESTAMP_MS: return "tsm:"; // timestamp[milliseconds]
        case DUCKDB_TYPE_TIMESTAMP_NS: return "tsn:"; // timestamp[nanoseconds]
        case DUCKDB_TYPE_INTERVAL:   return "tiM";    // interval[months]
        case DUCKDB_TYPE_HUGEINT:    return "u";      // Convert to string (128-bit)
        case DUCKDB_TYPE_UUID:       return "u";      // Convert to string (UUID)
        default:                     return "u";      // Fallback to string
    }
}

/**
 * Extract Arrow schema from DuckDB result.
 * Maps DuckDB types to Arrow types.
 */
inline ArrowErrorCode extractSchemaFromDuckDB(
    const duckdb_result& result,
    ArrowSchema* schema,
    ArrowError* error
) {
    idx_t col_count = duckdb_column_count(const_cast<duckdb_result*>(&result));

    // Initialize struct schema for the record batch
    ArrowSchemaInit(schema);
    NANOARROW_RETURN_NOT_OK(ArrowSchemaSetTypeStruct(schema, col_count));

    for (idx_t i = 0; i < col_count; i++) {
        const char* name = duckdb_column_name(const_cast<duckdb_result*>(&result), i);
        duckdb_type type = duckdb_column_type(const_cast<duckdb_result*>(&result), i);

        const char* format = duckdbTypeToArrowFormat(type);

        NANOARROW_RETURN_NOT_OK(ArrowSchemaSetType(schema->children[i], NANOARROW_TYPE_UNINITIALIZED));
        NANOARROW_RETURN_NOT_OK(ArrowSchemaSetFormat(schema->children[i], format));
        NANOARROW_RETURN_NOT_OK(ArrowSchemaSetName(schema->children[i], name));

        // All columns are nullable by default
        schema->children[i]->flags = ARROW_FLAG_NULLABLE;
    }

    return NANOARROW_OK;
}

namespace detail {

// Helper to copy validity bitmap
inline void copyValidityBitmap(duckdb_vector vec, ArrowArray* array, idx_t count) {
    uint64_t* validity = duckdb_vector_get_validity(vec);
    if (validity == nullptr) {
        // All values are valid, no need for validity bitmap
        array->null_count = 0;
        return;
    }

    // Count nulls and set bitmap
    int64_t null_count = 0;
    auto* bitmap = static_cast<uint8_t*>(const_cast<void*>(array->buffers[0]));

    for (idx_t i = 0; i < count; i++) {
        bool is_valid = duckdb_validity_row_is_valid(validity, i);
        if (!is_valid) {
            null_count++;
            // Clear bit (nanoarrow uses inverted validity)
            bitmap[i / 8] &= ~(1 << (i % 8));
        } else {
            // Set bit
            bitmap[i / 8] |= (1 << (i % 8));
        }
    }
    array->null_count = null_count;
}

// Helper to convert primitive types
template<typename T>
inline ArrowErrorCode convertPrimitiveColumn(
    duckdb_vector vec,
    ArrowArray* array,
    idx_t count
) {
    T* src = static_cast<T*>(duckdb_vector_get_data(vec));
    T* dst = static_cast<T*>(const_cast<void*>(array->buffers[1]));
    std::memcpy(dst, src, count * sizeof(T));
    copyValidityBitmap(vec, array, count);
    return NANOARROW_OK;
}

// Helper to convert string column
inline ArrowErrorCode convertStringColumn(
    duckdb_vector vec,
    ArrowArray* array,
    ArrowBuffer* data_buffer,
    idx_t count,
    ArrowError* error
) {
    auto* validity = duckdb_vector_get_validity(vec);
    auto* offsets = static_cast<int32_t*>(const_cast<void*>(array->buffers[1]));
    auto* strings = static_cast<duckdb_string_t*>(duckdb_vector_get_data(vec));

    int32_t current_offset = 0;
    offsets[0] = 0;

    for (idx_t i = 0; i < count; i++) {
        bool is_valid = validity == nullptr || duckdb_validity_row_is_valid(validity, i);

        if (is_valid) {
            duckdb_string_t str = strings[i];
            uint32_t len = duckdb_string_t_length(str);
            const char* data = duckdb_string_t_data(&str);

            NANOARROW_RETURN_NOT_OK(ArrowBufferAppend(data_buffer, data, len));
            current_offset += len;
        }

        offsets[i + 1] = current_offset;
    }

    copyValidityBitmap(vec, array, count);
    return NANOARROW_OK;
}

} // namespace detail

/**
 * Convert a DuckDB data chunk to an Arrow array.
 * Returns a struct array containing child arrays for each column.
 */
inline ArrowErrorCode convertChunkToArrow(
    duckdb_data_chunk chunk,
    const ArrowSchema* schema,
    ArrowArray* array,
    ArrowError* error
) {
    idx_t col_count = duckdb_data_chunk_get_column_count(chunk);
    idx_t row_count = duckdb_data_chunk_get_size(chunk);

    // Initialize struct array manually (don't use ArrowArrayInitFromSchema
    // since we need to build children differently based on DuckDB types)
    NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(array, NANOARROW_TYPE_STRUCT));
    NANOARROW_RETURN_NOT_OK(ArrowArrayAllocateChildren(array, col_count));

    array->length = row_count;
    array->null_count = 0;

    for (idx_t col = 0; col < col_count; col++) {
        duckdb_vector vec = duckdb_data_chunk_get_vector(chunk, col);
        duckdb_logical_type logical_type = duckdb_vector_get_column_type(vec);
        duckdb_type type = duckdb_get_type_id(logical_type);
        duckdb_destroy_logical_type(&logical_type);

        ArrowArray* child = array->children[col];
        auto* validity = duckdb_vector_get_validity(vec);

        switch (type) {
            case DUCKDB_TYPE_BOOLEAN: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_BOOL));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));

                auto* src = static_cast<bool*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(child, src[i] ? 1 : 0));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_TINYINT: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_INT8));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* data = static_cast<int8_t*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(child, data[i]));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_SMALLINT: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_INT16));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* data = static_cast<int16_t*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(child, data[i]));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_INTEGER:
            case DUCKDB_TYPE_DATE: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_INT32));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* data = static_cast<int32_t*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(child, data[i]));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_BIGINT:
            case DUCKDB_TYPE_TIME:
            case DUCKDB_TYPE_TIMESTAMP:
            case DUCKDB_TYPE_TIMESTAMP_S:
            case DUCKDB_TYPE_TIMESTAMP_MS:
            case DUCKDB_TYPE_TIMESTAMP_NS: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_INT64));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* data = static_cast<int64_t*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendInt(child, data[i]));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_UTINYINT: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_UINT8));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* data = static_cast<uint8_t*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendUInt(child, data[i]));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_USMALLINT: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_UINT16));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* data = static_cast<uint16_t*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendUInt(child, data[i]));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_UINTEGER: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_UINT32));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* data = static_cast<uint32_t*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendUInt(child, data[i]));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_UBIGINT: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_UINT64));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* data = static_cast<uint64_t*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendUInt(child, data[i]));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_FLOAT: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_FLOAT));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* data = static_cast<float*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendDouble(child, data[i]));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_DOUBLE: {
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_DOUBLE));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* data = static_cast<double*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendDouble(child, data[i]));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            case DUCKDB_TYPE_VARCHAR:
            case DUCKDB_TYPE_BLOB: {
                // String/binary types
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_STRING));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                auto* strings = static_cast<duckdb_string_t*>(duckdb_vector_get_data(vec));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        duckdb_string_t str = strings[i];
                        uint32_t len = duckdb_string_t_length(str);
                        const char* data = duckdb_string_t_data(&str);
                        ArrowStringView view = {data, static_cast<int64_t>(len)};
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendString(child, view));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }

            default: {
                // Unsupported type - add empty string column as placeholder
                // This is safer than trying to interpret unknown binary data
                NANOARROW_RETURN_NOT_OK(ArrowArrayInitFromType(child, NANOARROW_TYPE_STRING));
                NANOARROW_RETURN_NOT_OK(ArrowArrayStartAppending(child));
                for (idx_t i = 0; i < row_count; i++) {
                    if (validity && !duckdb_validity_row_is_valid(validity, i)) {
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendNull(child, 1));
                    } else {
                        // Empty string for unsupported types
                        ArrowStringView view = {"", 0};
                        NANOARROW_RETURN_NOT_OK(ArrowArrayAppendString(child, view));
                    }
                }
                NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(child, error));
                break;
            }
        }
    }

    NANOARROW_RETURN_NOT_OK(ArrowArrayFinishBuildingDefault(array, error));
    return NANOARROW_OK;
}

/**
 * Serialize a DuckDB result to Arrow IPC stream format.
 */
inline ArrowSerializationResult serializeToArrowIPC(
    duckdb_result& result,
    const ArrowSerializerConfig& config
) {
    ArrowSerializationResult output;
    ArrowError error;

    // Extract schema
    ArrowSchema schema;
    if (extractSchemaFromDuckDB(result, &schema, &error) != NANOARROW_OK) {
        output.errorMessage = "Failed to extract schema: " + std::string(error.message);
        return output;
    }

    // Create output buffer
    ArrowBuffer outputBuffer;
    ArrowBufferInit(&outputBuffer);

    // Create output stream
    ArrowIpcOutputStream stream;
    if (ArrowIpcOutputStreamInitBuffer(&stream, &outputBuffer) != NANOARROW_OK) {
        ArrowSchemaRelease(&schema);
        output.errorMessage = "Failed to initialize output stream";
        return output;
    }

    // Create writer
    ArrowIpcWriter writer;
    if (ArrowIpcWriterInit(&writer, &stream) != NANOARROW_OK) {
        ArrowSchemaRelease(&schema);
        ArrowBufferReset(&outputBuffer);
        output.errorMessage = "Failed to initialize IPC writer";
        return output;
    }

    // Write schema
    if (ArrowIpcWriterWriteSchema(&writer, &schema, &error) != NANOARROW_OK) {
        ArrowIpcWriterReset(&writer);
        ArrowSchemaRelease(&schema);
        ArrowBufferReset(&outputBuffer);
        output.errorMessage = "Failed to write schema: " + std::string(error.message);
        return output;
    }

    // Process data chunks
    size_t totalRows = 0;
    size_t batchCount = 0;
    size_t memoryUsed = 0;

    duckdb_data_chunk chunk;
    while ((chunk = duckdb_fetch_chunk(result)) != nullptr) {
        idx_t chunk_size = duckdb_data_chunk_get_size(chunk);
        if (chunk_size == 0) {
            duckdb_destroy_data_chunk(&chunk);
            break;
        }

        // Check memory limit
        memoryUsed += chunk_size * 100;  // Rough estimate
        if (config.maxMemoryBytes > 0 && memoryUsed > config.maxMemoryBytes) {
            duckdb_destroy_data_chunk(&chunk);
            ArrowIpcWriterReset(&writer);
            ArrowSchemaRelease(&schema);
            ArrowBufferReset(&outputBuffer);
            output.errorMessage = "Exceeded memory limit";
            return output;
        }

        // Convert chunk to Arrow
        ArrowArray array;
        if (convertChunkToArrow(chunk, &schema, &array, &error) != NANOARROW_OK) {
            duckdb_destroy_data_chunk(&chunk);
            ArrowIpcWriterReset(&writer);
            ArrowSchemaRelease(&schema);
            ArrowBufferReset(&outputBuffer);
            output.errorMessage = "Failed to convert chunk: " + std::string(error.message);
            return output;
        }

        // Create array view for writing
        ArrowArrayView arrayView;
        if (ArrowArrayViewInitFromSchema(&arrayView, &schema, &error) != NANOARROW_OK) {
            ArrowArrayRelease(&array);
            duckdb_destroy_data_chunk(&chunk);
            ArrowIpcWriterReset(&writer);
            ArrowSchemaRelease(&schema);
            ArrowBufferReset(&outputBuffer);
            output.errorMessage = "Failed to create array view";
            return output;
        }

        if (ArrowArrayViewSetArray(&arrayView, &array, &error) != NANOARROW_OK) {
            ArrowArrayViewReset(&arrayView);
            ArrowArrayRelease(&array);
            duckdb_destroy_data_chunk(&chunk);
            ArrowIpcWriterReset(&writer);
            ArrowSchemaRelease(&schema);
            ArrowBufferReset(&outputBuffer);
            output.errorMessage = "Failed to set array view";
            return output;
        }

        // Write batch
        if (ArrowIpcWriterWriteArrayView(&writer, &arrayView, &error) != NANOARROW_OK) {
            ArrowArrayViewReset(&arrayView);
            ArrowArrayRelease(&array);
            duckdb_destroy_data_chunk(&chunk);
            ArrowIpcWriterReset(&writer);
            ArrowSchemaRelease(&schema);
            ArrowBufferReset(&outputBuffer);
            output.errorMessage = "Failed to write batch: " + std::string(error.message);
            return output;
        }

        ArrowArrayViewReset(&arrayView);
        ArrowArrayRelease(&array);
        duckdb_destroy_data_chunk(&chunk);

        totalRows += chunk_size;
        batchCount++;
    }

    // Finalize stream (write EOS marker)
    ArrowIpcWriterReset(&writer);
    ArrowSchemaRelease(&schema);

    // Copy uncompressed output to vector
    std::vector<uint8_t> rawData(outputBuffer.size_bytes);
    std::memcpy(rawData.data(), outputBuffer.data, outputBuffer.size_bytes);
    ArrowBufferReset(&outputBuffer);

    // Apply compression if codec specified
    std::string codecNormalized = normalizeCodecName(config.codec);
    if (!codecNormalized.empty()) {
        std::vector<uint8_t> compressedData;

        if (codecNormalized == "zstd") {
            compressedData = compressZstd(rawData, config.compressionLevel);
        } else if (codecNormalized == "lz4") {
            compressedData = compressLz4(rawData);
        } else {
            // Invalid codec - return error or fallback to uncompressed
            output.errorMessage = "Unsupported compression codec: " + config.codec;
            return output;
        }

        if (compressedData.empty() && !rawData.empty()) {
            // Compression failed - fallback to uncompressed
            output.success = true;
            output.rowCount = totalRows;
            output.batchCount = batchCount;
            output.bytesWritten = rawData.size();
            output.data = std::move(rawData);
            return output;
        }

        // Use compressed data
        output.success = true;
        output.rowCount = totalRows;
        output.batchCount = batchCount;
        output.bytesWritten = compressedData.size();
        output.data = std::move(compressedData);
        return output;
    }

    // No compression - use raw data
    output.success = true;
    output.rowCount = totalRows;
    output.batchCount = batchCount;
    output.bytesWritten = rawData.size();
    output.data = std::move(rawData);
    return output;
}

} // namespace flapi
