#include "archive_io.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <array>
#include <cstring>
#include <ctime>
#include <utility>

namespace flapi {

namespace {

struct WriteSink {
    std::vector<std::uint8_t>* out;
};

la_ssize_t WriteCallback(archive*, void* client_data, const void* buffer, std::size_t length) {
    auto* sink = static_cast<WriteSink*>(client_data);
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    sink->out->insert(sink->out->end(), bytes, bytes + length);
    return static_cast<la_ssize_t>(length);
}

std::string ArchiveErrorMessage(archive* a, const char* prefix) {
    const char* err = archive_error_string(a);
    std::string msg = prefix;
    msg += ": ";
    msg += err ? err : "unknown libarchive error";
    return msg;
}

class ScopedWriter {
public:
    ScopedWriter() : a_(archive_write_new()) {}
    ~ScopedWriter() {
        if (a_ != nullptr) {
            archive_write_free(a_);
        }
    }
    ScopedWriter(const ScopedWriter&) = delete;
    ScopedWriter& operator=(const ScopedWriter&) = delete;

    archive* get() const { return a_; }

private:
    archive* a_;
};

class ScopedReader {
public:
    ScopedReader() : a_(archive_read_new()) {}
    ~ScopedReader() {
        if (a_ != nullptr) {
            archive_read_free(a_);
        }
    }
    ScopedReader(const ScopedReader&) = delete;
    ScopedReader& operator=(const ScopedReader&) = delete;

    archive* get() const { return a_; }

private:
    archive* a_;
};

class ScopedEntry {
public:
    ScopedEntry() : e_(archive_entry_new()) {}
    ~ScopedEntry() {
        if (e_ != nullptr) {
            archive_entry_free(e_);
        }
    }
    ScopedEntry(const ScopedEntry&) = delete;
    ScopedEntry& operator=(const ScopedEntry&) = delete;

    archive_entry* get() const { return e_; }

private:
    archive_entry* e_;
};

}  // namespace

std::vector<std::uint8_t> WriteArchive(const ArchiveEntries& entries,
                                       const ArchiveWriteOptions& options) {
    std::vector<std::uint8_t> out;
    WriteSink sink{&out};

    ScopedWriter writer;
    if (writer.get() == nullptr) {
        throw ArchiveIOError("archive_write_new returned null");
    }
    archive* a = writer.get();

    if (archive_write_set_format_zip(a) != ARCHIVE_OK) {
        throw ArchiveIOError(ArchiveErrorMessage(a, "archive_write_set_format_zip"));
    }

    // Critical: keep the spike-found "no tar-block padding" property so a
    // later EOCD reverse scan finds the record at file EOF (or close to it)
    // instead of after a chunk of trailing zeros.
    if (archive_write_set_bytes_in_last_block(a, 1) != ARCHIVE_OK) {
        throw ArchiveIOError(ArchiveErrorMessage(a, "archive_write_set_bytes_in_last_block"));
    }

    if (archive_write_open(a, &sink, /*open=*/nullptr, WriteCallback, /*close=*/nullptr) != ARCHIVE_OK) {
        throw ArchiveIOError(ArchiveErrorMessage(a, "archive_write_open"));
    }

    const std::time_t mtime = static_cast<std::time_t>(options.source_date_epoch.value_or(0));

    for (const auto& [name, data] : entries) {
        ScopedEntry entry;
        if (entry.get() == nullptr) {
            throw ArchiveIOError("archive_entry_new returned null");
        }
        archive_entry_set_pathname(entry.get(), name.c_str());
        archive_entry_set_size(entry.get(), static_cast<la_int64_t>(data.size()));
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_mtime(entry.get(), mtime, 0);
        archive_entry_set_atime(entry.get(), mtime, 0);
        archive_entry_set_ctime(entry.get(), mtime, 0);

        if (archive_write_header(a, entry.get()) != ARCHIVE_OK) {
            throw ArchiveIOError(ArchiveErrorMessage(a, ("archive_write_header(" + name + ")").c_str()));
        }

        if (!data.empty()) {
            const la_ssize_t written = archive_write_data(a, data.data(), data.size());
            if (written < 0 || static_cast<std::size_t>(written) != data.size()) {
                throw ArchiveIOError(ArchiveErrorMessage(a, ("archive_write_data(" + name + ")").c_str()));
            }
        }
    }

    if (archive_write_close(a) != ARCHIVE_OK) {
        throw ArchiveIOError(ArchiveErrorMessage(a, "archive_write_close"));
    }

    return out;
}

ArchiveEntries ReadArchive(const std::vector<std::uint8_t>& buffer) {
    if (buffer.empty()) {
        throw ArchiveIOError("archive buffer is empty");
    }

    ScopedReader reader;
    if (reader.get() == nullptr) {
        throw ArchiveIOError("archive_read_new returned null");
    }
    archive* a = reader.get();

    if (archive_read_support_format_zip(a) != ARCHIVE_OK) {
        throw ArchiveIOError(ArchiveErrorMessage(a, "archive_read_support_format_zip"));
    }

    if (archive_read_open_memory(a, buffer.data(), buffer.size()) != ARCHIVE_OK) {
        throw ArchiveIOError(ArchiveErrorMessage(a, "archive_read_open_memory"));
    }

    ArchiveEntries result;
    archive_entry* entry = nullptr;

    while (true) {
        const int rc = archive_read_next_header(a, &entry);
        if (rc == ARCHIVE_EOF) {
            break;
        }
        if (rc != ARCHIVE_OK) {
            throw ArchiveIOError(ArchiveErrorMessage(a, "archive_read_next_header"));
        }

        const char* path_cstr = archive_entry_pathname(entry);
        std::string name = path_cstr ? path_cstr : "";

        std::vector<std::uint8_t> data;
        std::array<std::uint8_t, 8192> chunk{};
        while (true) {
            const la_ssize_t n = archive_read_data(a, chunk.data(), chunk.size());
            if (n < 0) {
                throw ArchiveIOError(ArchiveErrorMessage(a, ("archive_read_data(" + name + ")").c_str()));
            }
            if (n == 0) {
                break;
            }
            data.insert(data.end(), chunk.begin(), chunk.begin() + n);
        }

        result.emplace(std::move(name), std::move(data));
    }

    return result;
}

}  // namespace flapi
