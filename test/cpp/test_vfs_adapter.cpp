#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "vfs_adapter.hpp"
#include <filesystem>
#include <fstream>

using namespace flapi;

// Helper to create temporary test files
class TempTestFile {
public:
    explicit TempTestFile(const std::string& content = "",
                          const std::string& extension = ".txt") {
        path_ = std::filesystem::temp_directory_path() /
                ("vfs_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + extension);
        // Always create the file, even if content is empty
        std::ofstream file(path_);
        file << content;
    }

    ~TempTestFile() {
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
        }
    }

    std::filesystem::path path() const { return path_; }
    std::string pathString() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// Helper to create temporary test directories
class TempTestDir {
public:
    TempTestDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("vfs_test_dir_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }

    ~TempTestDir() {
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove_all(path_);
        }
    }

    std::filesystem::path path() const { return path_; }
    std::string pathString() const { return path_.string(); }

    void createFile(const std::string& name, const std::string& content = "") {
        std::ofstream file(path_ / name);
        file << content;
    }

private:
    std::filesystem::path path_;
};

// ============================================================================
// PathSchemeUtils Tests
// ============================================================================

TEST_CASE("PathSchemeUtils::IsRemotePath", "[vfs][scheme]") {
    SECTION("S3 paths are remote") {
        REQUIRE(PathSchemeUtils::IsRemotePath("s3://bucket/key"));
        REQUIRE(PathSchemeUtils::IsRemotePath("s3://my-bucket/path/to/file.yaml"));
    }

    SECTION("GCS paths are remote") {
        REQUIRE(PathSchemeUtils::IsRemotePath("gs://bucket/key"));
        REQUIRE(PathSchemeUtils::IsRemotePath("gs://my-bucket/path/to/file.yaml"));
    }

    SECTION("Azure paths are remote") {
        REQUIRE(PathSchemeUtils::IsRemotePath("az://container/blob"));
        REQUIRE(PathSchemeUtils::IsRemotePath("azure://container/blob"));
    }

    SECTION("HTTP/HTTPS paths are remote") {
        REQUIRE(PathSchemeUtils::IsRemotePath("http://example.com/file.yaml"));
        REQUIRE(PathSchemeUtils::IsRemotePath("https://example.com/file.yaml"));
    }

    SECTION("Local paths are not remote") {
        REQUIRE_FALSE(PathSchemeUtils::IsRemotePath("/local/path/file.yaml"));
        REQUIRE_FALSE(PathSchemeUtils::IsRemotePath("./relative/path.yaml"));
        REQUIRE_FALSE(PathSchemeUtils::IsRemotePath("relative/path.yaml"));
        REQUIRE_FALSE(PathSchemeUtils::IsRemotePath("file.yaml"));
    }

    SECTION("file:// scheme is not remote") {
        REQUIRE_FALSE(PathSchemeUtils::IsRemotePath("file:///local/path/file.yaml"));
    }

    SECTION("Empty path is not remote") {
        REQUIRE_FALSE(PathSchemeUtils::IsRemotePath(""));
    }
}

TEST_CASE("PathSchemeUtils::IsS3Path", "[vfs][scheme]") {
    REQUIRE(PathSchemeUtils::IsS3Path("s3://bucket/key"));
    REQUIRE(PathSchemeUtils::IsS3Path("s3://"));
    REQUIRE(PathSchemeUtils::IsS3Path("S3://bucket/key")); // Case insensitive
    REQUIRE(PathSchemeUtils::IsS3Path("S3://BUCKET/KEY")); // Mixed case
    REQUIRE_FALSE(PathSchemeUtils::IsS3Path("gs://bucket/key"));
    REQUIRE_FALSE(PathSchemeUtils::IsS3Path("/local/path"));
}

TEST_CASE("PathSchemeUtils::IsGCSPath", "[vfs][scheme]") {
    REQUIRE(PathSchemeUtils::IsGCSPath("gs://bucket/key"));
    REQUIRE(PathSchemeUtils::IsGCSPath("gs://"));
    REQUIRE(PathSchemeUtils::IsGCSPath("GS://bucket/key")); // Case insensitive
    REQUIRE(PathSchemeUtils::IsGCSPath("Gs://BUCKET/key")); // Mixed case
    REQUIRE_FALSE(PathSchemeUtils::IsGCSPath("s3://bucket/key"));
    REQUIRE_FALSE(PathSchemeUtils::IsGCSPath("/local/path"));
}

TEST_CASE("PathSchemeUtils::IsAzurePath", "[vfs][scheme]") {
    REQUIRE(PathSchemeUtils::IsAzurePath("az://container/blob"));
    REQUIRE(PathSchemeUtils::IsAzurePath("azure://container/blob"));
    REQUIRE(PathSchemeUtils::IsAzurePath("AZ://container/blob")); // Case insensitive
    REQUIRE(PathSchemeUtils::IsAzurePath("AZURE://container/blob")); // Case insensitive
    REQUIRE(PathSchemeUtils::IsAzurePath("Azure://Container/Blob")); // Mixed case
    REQUIRE_FALSE(PathSchemeUtils::IsAzurePath("s3://bucket/key"));
    REQUIRE_FALSE(PathSchemeUtils::IsAzurePath("/local/path"));
}

TEST_CASE("PathSchemeUtils::IsHttpPath", "[vfs][scheme]") {
    REQUIRE(PathSchemeUtils::IsHttpPath("http://example.com/file"));
    REQUIRE(PathSchemeUtils::IsHttpPath("https://example.com/file"));
    REQUIRE(PathSchemeUtils::IsHttpPath("HTTP://example.com")); // Case insensitive
    REQUIRE(PathSchemeUtils::IsHttpPath("HTTPS://example.com")); // Case insensitive
    REQUIRE(PathSchemeUtils::IsHttpPath("Http://Example.COM/file")); // Mixed case
    REQUIRE_FALSE(PathSchemeUtils::IsHttpPath("ftp://example.com/file"));
    REQUIRE_FALSE(PathSchemeUtils::IsHttpPath("/local/path"));
}

TEST_CASE("PathSchemeUtils::IsFilePath", "[vfs][scheme]") {
    REQUIRE(PathSchemeUtils::IsFilePath("file:///local/path"));
    REQUIRE(PathSchemeUtils::IsFilePath("file://relative/path"));
    REQUIRE(PathSchemeUtils::IsFilePath("FILE:///local/path")); // Case insensitive
    REQUIRE(PathSchemeUtils::IsFilePath("File:///local/path")); // Mixed case
    REQUIRE_FALSE(PathSchemeUtils::IsFilePath("/local/path"));
    REQUIRE_FALSE(PathSchemeUtils::IsFilePath("s3://bucket/key"));
}

TEST_CASE("PathSchemeUtils::GetScheme", "[vfs][scheme]") {
    SECTION("Returns correct scheme for each type") {
        REQUIRE(PathSchemeUtils::GetScheme("s3://bucket/key") == "s3://");
        REQUIRE(PathSchemeUtils::GetScheme("gs://bucket/key") == "gs://");
        REQUIRE(PathSchemeUtils::GetScheme("az://container/blob") == "az://");
        REQUIRE(PathSchemeUtils::GetScheme("azure://container/blob") == "azure://");
        REQUIRE(PathSchemeUtils::GetScheme("http://example.com") == "http://");
        REQUIRE(PathSchemeUtils::GetScheme("https://example.com") == "https://");
        REQUIRE(PathSchemeUtils::GetScheme("file:///local/path") == "file://");
    }

    SECTION("Returns empty string for local paths") {
        REQUIRE(PathSchemeUtils::GetScheme("/local/path") == "");
        REQUIRE(PathSchemeUtils::GetScheme("relative/path") == "");
        REQUIRE(PathSchemeUtils::GetScheme("") == "");
    }
}

TEST_CASE("PathSchemeUtils::StripFileScheme", "[vfs][scheme]") {
    SECTION("Strips file:// prefix") {
        REQUIRE(PathSchemeUtils::StripFileScheme("file:///local/path") == "/local/path");
        REQUIRE(PathSchemeUtils::StripFileScheme("file://relative") == "relative");
    }

    SECTION("Returns unchanged for non-file paths") {
        REQUIRE(PathSchemeUtils::StripFileScheme("/local/path") == "/local/path");
        REQUIRE(PathSchemeUtils::StripFileScheme("s3://bucket/key") == "s3://bucket/key");
        REQUIRE(PathSchemeUtils::StripFileScheme("relative") == "relative");
    }
}

// ============================================================================
// LocalFileProvider Tests
// ============================================================================

TEST_CASE("LocalFileProvider::GetProviderName", "[vfs][local]") {
    LocalFileProvider provider;
    REQUIRE(provider.GetProviderName() == "local");
}

TEST_CASE("LocalFileProvider::IsRemotePath", "[vfs][local]") {
    LocalFileProvider provider;

    SECTION("Local paths are not remote") {
        REQUIRE_FALSE(provider.IsRemotePath("/local/path"));
        REQUIRE_FALSE(provider.IsRemotePath("./relative/path"));
        REQUIRE_FALSE(provider.IsRemotePath("file.txt"));
    }

    SECTION("Remote paths are identified") {
        REQUIRE(provider.IsRemotePath("s3://bucket/key"));
        REQUIRE(provider.IsRemotePath("https://example.com/file"));
    }
}

TEST_CASE("LocalFileProvider::ReadFile", "[vfs][local]") {
    LocalFileProvider provider;

    SECTION("Read existing file") {
        TempTestFile file("Hello, World!");
        std::string content = provider.ReadFile(file.pathString());
        REQUIRE(content == "Hello, World!");
    }

    SECTION("Read file with multiple lines") {
        TempTestFile file("Line 1\nLine 2\nLine 3");
        std::string content = provider.ReadFile(file.pathString());
        REQUIRE(content == "Line 1\nLine 2\nLine 3");
    }

    SECTION("Read empty file") {
        TempTestFile file("");
        std::string content = provider.ReadFile(file.pathString());
        REQUIRE(content.empty());
    }

    SECTION("Read YAML content") {
        std::string yaml = "project-name: test\nversion: 1.0.0\n";
        TempTestFile file(yaml, ".yaml");
        std::string content = provider.ReadFile(file.pathString());
        REQUIRE(content == yaml);
    }

    SECTION("Read file with file:// scheme") {
        TempTestFile file("Content with file scheme");
        std::string file_uri = "file://" + file.pathString();
        std::string content = provider.ReadFile(file_uri);
        REQUIRE(content == "Content with file scheme");
    }

    SECTION("Throw on non-existent file") {
        REQUIRE_THROWS_AS(
            provider.ReadFile("/nonexistent/path/file.txt"),
            FileOperationError
        );
    }

    SECTION("Error message includes file path") {
        try {
            provider.ReadFile("/nonexistent/path/file.txt");
            REQUIRE(false); // Should not reach here
        } catch (const FileOperationError& e) {
            REQUIRE(std::string(e.what()).find("nonexistent") != std::string::npos);
        }
    }
}

TEST_CASE("LocalFileProvider::FileExists", "[vfs][local]") {
    LocalFileProvider provider;

    SECTION("Returns true for existing file") {
        TempTestFile file("test content");
        REQUIRE(provider.FileExists(file.pathString()));
    }

    SECTION("Returns false for non-existent file") {
        REQUIRE_FALSE(provider.FileExists("/nonexistent/path/file.txt"));
    }

    SECTION("Returns false for directory") {
        TempTestDir dir;
        REQUIRE_FALSE(provider.FileExists(dir.pathString()));
    }

    SECTION("Returns false for remote paths") {
        REQUIRE_FALSE(provider.FileExists("s3://bucket/key"));
        REQUIRE_FALSE(provider.FileExists("https://example.com/file"));
    }

    SECTION("Handles file:// scheme") {
        TempTestFile file("test content");
        std::string file_uri = "file://" + file.pathString();
        REQUIRE(provider.FileExists(file_uri));
    }
}

TEST_CASE("LocalFileProvider::ListFiles", "[vfs][local]") {
    LocalFileProvider provider;

    SECTION("List files matching pattern") {
        TempTestDir dir;
        dir.createFile("file1.yaml", "content1");
        dir.createFile("file2.yaml", "content2");
        dir.createFile("file3.txt", "content3");

        auto yaml_files = provider.ListFiles(dir.pathString(), "*.yaml");
        REQUIRE(yaml_files.size() == 2);

        // Check that both yaml files are found
        bool found_file1 = false, found_file2 = false;
        for (const auto& f : yaml_files) {
            if (f.find("file1.yaml") != std::string::npos) {
                found_file1 = true;
            }
            if (f.find("file2.yaml") != std::string::npos) {
                found_file2 = true;
            }
        }
        REQUIRE(found_file1);
        REQUIRE(found_file2);
    }

    SECTION("List all files with wildcard") {
        TempTestDir dir;
        dir.createFile("file1.yaml", "content1");
        dir.createFile("file2.txt", "content2");
        dir.createFile("file3.sql", "content3");

        auto all_files = provider.ListFiles(dir.pathString(), "*");
        REQUIRE(all_files.size() == 3);
    }

    SECTION("Empty directory returns empty list") {
        TempTestDir dir;
        auto files = provider.ListFiles(dir.pathString(), "*.yaml");
        REQUIRE(files.empty());
    }

    SECTION("No matching files returns empty list") {
        TempTestDir dir;
        dir.createFile("file.txt", "content");

        auto files = provider.ListFiles(dir.pathString(), "*.yaml");
        REQUIRE(files.empty());
    }

    SECTION("Throw on non-existent directory") {
        REQUIRE_THROWS_AS(
            provider.ListFiles("/nonexistent/directory", "*"),
            FileOperationError
        );
    }

    SECTION("Pattern with question mark matches single character") {
        TempTestDir dir;
        dir.createFile("file1.txt", "");
        dir.createFile("file2.txt", "");
        dir.createFile("file10.txt", "");

        auto files = provider.ListFiles(dir.pathString(), "file?.txt");
        REQUIRE(files.size() == 2); // file1.txt and file2.txt, not file10.txt
    }

    SECTION("Results are sorted") {
        TempTestDir dir;
        dir.createFile("c.txt", "");
        dir.createFile("a.txt", "");
        dir.createFile("b.txt", "");

        auto files = provider.ListFiles(dir.pathString(), "*.txt");
        REQUIRE(files.size() == 3);

        // Check sorted order
        REQUIRE(files[0].find("a.txt") != std::string::npos);
        REQUIRE(files[1].find("b.txt") != std::string::npos);
        REQUIRE(files[2].find("c.txt") != std::string::npos);
    }

    SECTION("Handles file:// scheme for directory") {
        TempTestDir dir;
        dir.createFile("test.yaml", "content");

        std::string dir_uri = "file://" + dir.pathString();
        auto files = provider.ListFiles(dir_uri, "*.yaml");
        REQUIRE(files.size() == 1);
    }
}

// ============================================================================
// FileProviderFactory Tests
// ============================================================================

TEST_CASE("FileProviderFactory::CreateLocalProvider", "[vfs][factory]") {
    auto provider = FileProviderFactory::CreateLocalProvider();
    REQUIRE(provider != nullptr);
    REQUIRE(provider->GetProviderName() == "local");
}

TEST_CASE("FileProviderFactory::CreateProvider", "[vfs][factory]") {
    SECTION("Returns local provider for local paths") {
        auto provider = FileProviderFactory::CreateProvider("/local/path/file.yaml");
        REQUIRE(provider != nullptr);
        REQUIRE(provider->GetProviderName() == "local");
    }

    SECTION("Returns local provider for relative paths") {
        auto provider = FileProviderFactory::CreateProvider("relative/path.yaml");
        REQUIRE(provider != nullptr);
        REQUIRE(provider->GetProviderName() == "local");
    }

    SECTION("Returns local provider for file:// paths") {
        auto provider = FileProviderFactory::CreateProvider("file:///local/path");
        REQUIRE(provider != nullptr);
        REQUIRE(provider->GetProviderName() == "local");
    }

    SECTION("Returns DuckDBVFSProvider for S3 paths (or throws if DB not initialized)") {
        // DuckDBVFSProvider requires DatabaseManager to be initialized
        // If DB is initialized (e.g., by earlier tests), it returns a provider
        // If not, it throws FileOperationError
        try {
            auto provider = FileProviderFactory::CreateProvider("s3://bucket/key");
            // If we get here, DatabaseManager was initialized by earlier tests
            REQUIRE(provider != nullptr);
            REQUIRE(provider->GetProviderName() == "duckdb-vfs");
        } catch (const FileOperationError& e) {
            // Expected when DatabaseManager not initialized
            std::string msg(e.what());
            bool mentions_init = msg.find("Database") != std::string::npos ||
                                msg.find("database") != std::string::npos ||
                                msg.find("initialized") != std::string::npos;
            REQUIRE(mentions_init);
        }
    }

    SECTION("Returns DuckDBVFSProvider for HTTPS paths (or throws if DB not initialized)") {
        try {
            auto provider = FileProviderFactory::CreateProvider("https://example.com/file");
            REQUIRE(provider != nullptr);
            REQUIRE(provider->GetProviderName() == "duckdb-vfs");
        } catch (const FileOperationError& e) {
            std::string msg(e.what());
            bool mentions_init = msg.find("Database") != std::string::npos ||
                                msg.find("database") != std::string::npos ||
                                msg.find("initialized") != std::string::npos;
            REQUIRE(mentions_init);
        }
    }
}

// ============================================================================
// DuckDBVFSProvider Tests
// ============================================================================

TEST_CASE("DuckDBVFSProvider construction", "[vfs][duckdb]") {
    SECTION("Requires DatabaseManager (throws or succeeds based on state)") {
        // If DatabaseManager is initialized (by earlier tests), this succeeds
        // If not, it throws FileOperationError with a helpful message
        try {
            DuckDBVFSProvider provider;
            // If we get here, DatabaseManager was initialized
            REQUIRE(provider.GetProviderName() == "duckdb-vfs");
        } catch (const FileOperationError& e) {
            std::string msg(e.what());
            // Should mention what's needed
            bool helpful = msg.find("DatabaseManager") != std::string::npos ||
                          msg.find("database") != std::string::npos ||
                          msg.find("Database") != std::string::npos ||
                          msg.find("initialized") != std::string::npos;
            REQUIRE(helpful);
        }
    }
}

TEST_CASE("FileProviderFactory::CreateDuckDBProvider", "[vfs][factory]") {
    SECTION("Creates DuckDBVFSProvider (or throws if DB not initialized)") {
        try {
            auto provider = FileProviderFactory::CreateDuckDBProvider();
            REQUIRE(provider != nullptr);
            REQUIRE(provider->GetProviderName() == "duckdb-vfs");
        } catch (const FileOperationError& e) {
            std::string msg(e.what());
            bool mentions_init = msg.find("Database") != std::string::npos ||
                                msg.find("database") != std::string::npos ||
                                msg.find("initialized") != std::string::npos;
            REQUIRE(mentions_init);
        }
    }
}

// ============================================================================
// IFileProvider Interface Contract Tests
// ============================================================================

TEST_CASE("IFileProvider interface contract", "[vfs][interface]") {
    // This test verifies that LocalFileProvider correctly implements IFileProvider

    std::shared_ptr<IFileProvider> provider = std::make_shared<LocalFileProvider>();

    SECTION("Can be used through interface pointer") {
        TempTestFile file("Interface test content");

        std::string content = provider->ReadFile(file.pathString());
        REQUIRE(content == "Interface test content");

        REQUIRE(provider->FileExists(file.pathString()));
        REQUIRE_FALSE(provider->FileExists("/nonexistent"));

        REQUIRE_FALSE(provider->IsRemotePath(file.pathString()));
        REQUIRE(provider->IsRemotePath("s3://bucket/key"));
    }

    SECTION("ListFiles through interface") {
        TempTestDir dir;
        dir.createFile("test.yaml", "content");

        auto files = provider->ListFiles(dir.pathString(), "*.yaml");
        REQUIRE(files.size() == 1);
    }
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_CASE("VFS edge cases", "[vfs][edge]") {
    LocalFileProvider provider;

    SECTION("Handle paths with spaces") {
        // Create temp directory with space in name
        auto temp_dir = std::filesystem::temp_directory_path() / "vfs test dir";
        std::filesystem::create_directories(temp_dir);
        auto file_path = temp_dir / "file with spaces.txt";
        std::ofstream(file_path) << "content with spaces";

        REQUIRE(provider.FileExists(file_path.string()));
        REQUIRE(provider.ReadFile(file_path.string()) == "content with spaces");

        std::filesystem::remove_all(temp_dir);
    }

    SECTION("Handle special characters in content") {
        std::string special_content = "SELECT * FROM table WHERE name = 'O''Brien' AND value > 0;";
        TempTestFile file(special_content);

        std::string content = provider.ReadFile(file.pathString());
        REQUIRE(content == special_content);
    }

    SECTION("Handle binary-like content") {
        std::string binary_like = std::string("line1\0line2", 11);
        TempTestFile file(binary_like);

        std::string content = provider.ReadFile(file.pathString());
        REQUIRE(content == binary_like);
    }

    SECTION("Handle unicode content") {
        std::string unicode_content = "Hello 世界 🌍 مرحبا";
        TempTestFile file(unicode_content);

        std::string content = provider.ReadFile(file.pathString());
        REQUIRE(content == unicode_content);
    }
}
