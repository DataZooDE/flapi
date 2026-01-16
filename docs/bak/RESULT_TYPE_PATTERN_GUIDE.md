# Result<T> Type Pattern Implementation Guide

## Overview

The `Result<T>` type is a type-safe error handling pattern that replaces exceptions, optional returns, and bool-based error indication with explicit, compile-time-checked error propagation.

**Definition** (from `error.hpp`):
```cpp
template<typename T>
using Result = Expected<T, Error>;
```

## Quick Start

### Basic Usage

```cpp
// Old style - what we're moving away from
bool operationFailed = false;
int result = risky_operation(&operationFailed);
if (operationFailed) {
    // Handle error... but what error?
}

// New style - with Result<T>
Result<int> result = risky_operation();
if (result.has_value()) {
    int value = result.value();
    // Use value...
} else {
    Error err = result.error();
    // Handle specific error
}
```

### Creating Results

**Success case:**
```cpp
Result<std::string> get_user_name(int id) {
    // ...processing...
    return "John Doe";  // Implicit conversion to Result
}
```

**Error case:**
```cpp
Result<std::string> get_user_name(int id) {
    if (id < 0) {
        return Error::Validation("User ID must be positive", "Got: " + std::to_string(id));
    }
    // ...processing...
}
```

## Error Categories

The `Error` class provides type-safe error categories with automatic HTTP status code mapping:

| Category | HTTP Status | Use Case |
|----------|------------|----------|
| `Configuration` | 500 | Config file/structure issues |
| `Database` | 500 | Database/query execution errors |
| `Validation` | 400 | Input validation failures |
| `Authentication` | 401 | Auth/authorization failures |
| `NotFound` | 404 | Resource not found |
| `Internal` | 500 | Internal/programming errors |

### Factory Methods

```cpp
// Validation error (400)
return Error::Validation("Invalid input", "Must be positive integer");

// Database error (500)
return Error::Database("Query failed", "Table not found");

// Authentication error (401)
return Error::Auth("Invalid token", "Token expired");

// Not found error (404)
return Error::NotFound("User not found");

// Internal error (500)
return Error::Internal("Unexpected error");

// Configuration error (500)
return Error::Config("Missing required field");
```

## Refactoring Strategies

### Strategy 1: Add Result-based Versions Alongside Existing Code

**Best for gradual migration without breaking changes.**

```cpp
// In header file (.hpp)
class DatabaseManager {
public:
    // OLD: returns bool, throws exceptions, or returns sentinel values
    bool tableExists(const std::string& schema, const std::string& table);

    // NEW: returns Result<bool> - explicit error handling
    Result<bool> tableExistsAsync(const std::string& schema, const std::string& table);
};
```

**Implementation:**
```cpp
// In implementation file (.cpp)
Result<bool> DatabaseManager::tableExistsAsync(const std::string& schema, const std::string& table) {
    try {
        // Wrap old method or reimplement with error handling
        bool exists = tableExists(schema, table);
        return exists;
    } catch (const std::exception& e) {
        return Error::Database("Failed to check if table exists", e.what());
    }
}
```

### Strategy 2: Direct Refactoring (Break-Fix)

**Best for new code or when breaking changes are acceptable.**

Change method signature directly:

```cpp
// Before
bool tableExists(const std::string& schema, const std::string& table);

// After
Result<bool> tableExists(const std::string& schema, const std::string& table);
```

Then update all call sites.

### Strategy 3: Wrapper Functions

**Best for small utility functions or when the old code is hard to modify.**

```cpp
// Keep existing function
bool old_operation() {
    // ...existing implementation...
}

// Add wrapper that uses Result<T>
Result<void> safe_operation() {
    try {
        if (!old_operation()) {
            return Error::Internal("Operation failed");
        }
        return {};  // Success, no value
    } catch (const std::exception& e) {
        return Error::Internal("Operation failed", e.what());
    }
}
```

## Common Patterns

### Pattern 1: Simple Validation

```cpp
Result<int> parse_age(const std::string& input) {
    if (input.empty()) {
        return Error::Validation("Age cannot be empty");
    }

    try {
        int age = std::stoi(input);
        if (age < 0 || age > 150) {
            return Error::Validation("Age out of range", "Must be 0-150, got: " + input);
        }
        return age;
    } catch (const std::exception& e) {
        return Error::Validation("Invalid integer format", input);
    }
}
```

### Pattern 2: Propagating Errors from Callees

```cpp
Result<UserData> get_user_with_profile(int user_id) {
    // Call function that returns Result
    auto user_result = get_user(user_id);

    // Check if it succeeded
    if (!user_result.has_value()) {
        // Propagate the error
        return user_result.error();
    }

    // Use the value
    UserData user = user_result.value();

    // Call another function
    auto profile_result = get_profile(user.id);
    if (!profile_result.has_value()) {
        return profile_result.error();
    }

    user.profile = profile_result.value();
    return user;
}
```

### Pattern 3: Mapping Errors

```cpp
Result<std::vector<std::string>> get_table_names(const std::string& schema) {
    if (schema.empty()) {
        return Error::Validation("Schema name cannot be empty");
    }

    try {
        // Call underlying implementation
        std::vector<std::string> names = impl_get_table_names(schema);
        return names;
    } catch (const DatabaseException& e) {
        return Error::Database("Failed to list tables", e.what());
    } catch (const std::exception& e) {
        return Error::Internal("Unexpected error listing tables", e.what());
    }
}
```

### Pattern 4: Optional Values in Results

```cpp
// Result with optional value (e.g., may-or-may-not-exist case)
Result<std::optional<UserData>> find_user_by_email(const std::string& email) {
    if (email.empty()) {
        return Error::Validation("Email cannot be empty");
    }

    try {
        auto user = db.query<UserData>("SELECT * FROM users WHERE email = ?", email);
        return user;  // Returns Result<std::optional<UserData>>
    } catch (const std::exception& e) {
        return Error::Database("Database query failed", e.what());
    }
}

// Usage:
auto result = find_user_by_email(email);
if (!result.has_value()) {
    // Error occurred
    log_error(result.error().message);
    return send_error_response(result.error());
}

if (result.value().has_value()) {
    // User found
    UserData user = result.value().value();
} else {
    // User doesn't exist (not an error)
    return send_not_found_response();
}
```

## Integration with HTTP Layer

### Converting Result to HTTP Response

```cpp
// In your request handler
crow::response handle_get_user(int user_id) {
    auto result = user_service.get_user(user_id);

    // Result has automatic conversion to HTTP response
    if (!result.has_value()) {
        return result.error().toHttpResponse();
    }

    // Build success response
    crow::json::wvalue response;
    response["success"] = true;
    response["data"] = serialize_user(result.value());
    return crow::response(200, response);
}
```

### Error Response Format

All error responses follow a consistent JSON format:

```json
{
  "success": false,
  "error": {
    "category": "Validation",
    "message": "Invalid input",
    "details": "Must be positive integer"
  }
}
```

HTTP status codes are automatically set based on error category:
- `Validation` → 400 Bad Request
- `Authentication` → 401 Unauthorized
- `NotFound` → 404 Not Found
- `Database`, `Configuration`, `Internal` → 500 Internal Server Error

## Testing

### Unit Testing Result Types

```cpp
TEST_CASE("Result<T> usage", "[result]") {
    SECTION("Success case") {
        Result<int> r = parse_age("25");
        REQUIRE(r.has_value());
        REQUIRE(r.value() == 25);
    }

    SECTION("Error case") {
        Result<int> r = parse_age("-5");
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().category == ErrorCategory::Validation);
    }

    SECTION("Error propagation") {
        auto result = get_user_with_profile(invalid_id);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().category == ErrorCategory::NotFound);
    }
}
```

## Migration Roadmap

### Phase 1: Foundation (✅ Complete)
- Create Error class with categories
- Implement Expected<T, E> template
- Write comprehensive unit tests
- Document pattern

### Phase 2: Key Components (Current)
- Refactor DatabaseManager methods
- Refactor QueryExecutor
- Refactor RequestHandler
- Update tests for each component

### Phase 3: Integration
- Update HTTP layer to use Result types
- Consistent error responses across all endpoints
- Full integration tests

### Phase 4: Cleanup (Future)
- Remove old error handling patterns
- Deprecate exception-based error handling
- Complete migration to Result<T> throughout codebase

## Performance Considerations

### Memory
- Result<T> is stack-allocated (no heap allocation)
- Uses a union internally for space efficiency
- Move semantics prevent unnecessary copying

### Speed
- Zero-cost abstraction - compiles down to direct execution
- No heap allocation overhead
- No exception handling overhead

## Best Practices

### Do ✅

1. **Return error context, not just failure:**
   ```cpp
   // Good
   return Error::Validation("Invalid user ID", "Must be positive, got: " + id_str);

   // Bad
   return Error::Validation("Invalid user ID");
   ```

2. **Use appropriate error categories:**
   ```cpp
   // Good - tells caller what went wrong
   if (!database.is_open()) {
       return Error::Database("Database not initialized");
   }

   // Bad - vague error
   return Error::Internal("Something failed");
   ```

3. **Check errors at system boundaries:**
   ```cpp
   // Good - validate at entry points
   Result<User> user_data = parse_user_input(request.body);
   if (!user_data.has_value()) {
       return user_data.error().toHttpResponse();
   }
   ```

4. **Propagate errors up the call stack:**
   ```cpp
   // Good - let caller handle the error type
   Result<User> user = get_user(id);
   if (!user.has_value()) {
       return user.error();
   }
   ```

### Don't ❌

1. **Don't ignore errors:**
   ```cpp
   // Bad - error is lost
   auto r = risky_operation();
   if (r.has_value()) {
       process(r.value());
   }
   // What if there's an error? Silently continuing is wrong

   // Good
   auto r = risky_operation();
   if (!r.has_value()) {
       return r.error();
   }
   process(r.value());
   ```

2. **Don't convert Result to bool in error paths:**
   ```cpp
   // Bad
   if (operation()) {  // What does true mean?
       // ...
   }

   // Good
   auto result = operation();
   if (!result.has_value()) {
       log_error(result.error());
   }
   ```

3. **Don't mix Result and exceptions:**
   ```cpp
   // Bad - inconsistent error handling
   Result<int> func1() { return value; }
   int func2() { throw std::runtime_error("error"); }

   // Good - consistent pattern
   Result<int> func1() { return value; }
   Result<int> func2() { return error; }
   ```

## References

- **Files**: `src/include/error.hpp`, `src/error.cpp`
- **Tests**: `test/cpp/test_error.cpp`
- **Examples**: Various methods in DatabaseManager, QueryExecutor
- **C++ Standard**: Follows Rust-inspired Result<T> pattern from RFC

## Questions & Support

For questions about the Result<T> pattern:
1. Check test cases in `test_error.cpp` for usage examples
2. Review implementation in `error.hpp`
3. See integration examples in refactored components
