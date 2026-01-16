#pragma once

#include <duckdb.h>
#include <string>
#include <utility>

namespace flapi {

/**
 * RAII wrapper for DuckDB string pointers.
 * Automatically frees memory allocated by DuckDB when going out of scope.
 * Supports move semantics but not copy semantics.
 */
class DuckDBString {
public:
    /**
     * Construct from a DuckDB-allocated string pointer.
     * Takes ownership of the pointer and will free it on destruction.
     */
    explicit DuckDBString(char* ptr) noexcept : ptr_(ptr) {}

    /**
     * Destructor: automatically frees the pointer using duckdb_free().
     */
    ~DuckDBString() {
        if (ptr_) {
            duckdb_free(ptr_);
        }
    }

    // Delete copy constructor and assignment operator
    DuckDBString(const DuckDBString&) = delete;
    DuckDBString& operator=(const DuckDBString&) = delete;

    /**
     * Move constructor: transfers ownership to the new object.
     */
    DuckDBString(DuckDBString&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    /**
     * Move assignment operator: transfers ownership.
     */
    DuckDBString& operator=(DuckDBString&& other) noexcept {
        if (this != &other) {
            if (ptr_) {
                duckdb_free(ptr_);
            }
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    /**
     * Get the raw pointer (const).
     */
    const char* get() const noexcept { return ptr_; }

    /**
     * Convert to std::string.
     * Returns empty string if pointer is null.
     */
    std::string to_string() const {
        return ptr_ ? std::string(ptr_) : "";
    }

    /**
     * Check if pointer is null.
     */
    bool is_null() const noexcept { return ptr_ == nullptr; }

private:
    char* ptr_;
};

/**
 * RAII wrapper for DuckDB result structures.
 * Automatically destroys the result when going out of scope.
 * Supports move semantics but not copy semantics.
 */
class DuckDBResult {
public:
    /**
     * Default constructor: creates an uninitialized result.
     */
    DuckDBResult() noexcept : has_result_(false) {}

    /**
     * Destructor: automatically destroys the result if initialized.
     */
    ~DuckDBResult() {
        if (has_result_) {
            duckdb_destroy_result(&result_);
        }
    }

    // Delete copy constructor and assignment operator
    DuckDBResult(const DuckDBResult&) = delete;
    DuckDBResult& operator=(const DuckDBResult&) = delete;

    /**
     * Move constructor: transfers ownership.
     */
    DuckDBResult(DuckDBResult&& other) noexcept
        : has_result_(other.has_result_) {
        if (other.has_result_) {
            result_ = other.result_;
            other.has_result_ = false;
        }
    }

    /**
     * Move assignment operator: transfers ownership.
     */
    DuckDBResult& operator=(DuckDBResult&& other) noexcept {
        if (this != &other) {
            if (has_result_) {
                duckdb_destroy_result(&result_);
            }
            has_result_ = other.has_result_;
            if (other.has_result_) {
                result_ = other.result_;
                other.has_result_ = false;
            }
        }
        return *this;
    }

    /**
     * Get non-const pointer to the result structure.
     * Use this to pass to DuckDB functions that populate the result.
     */
    duckdb_result* get() noexcept { return &result_; }

    /**
     * Get const pointer to the result structure.
     */
    const duckdb_result* get() const noexcept { return &result_; }

    /**
     * Mark the result as initialized (should be called after successful duckdb_query).
     */
    void set_initialized() noexcept { has_result_ = true; }

    /**
     * Check if the result has been initialized and needs cleanup.
     */
    bool has_result() const noexcept { return has_result_; }

    /**
     * Reset the result state (manually destroy if initialized and mark as uninitialized).
     */
    void reset() {
        if (has_result_) {
            duckdb_destroy_result(&result_);
            has_result_ = false;
        }
    }

private:
    duckdb_result result_;
    bool has_result_;
};

} // namespace flapi
