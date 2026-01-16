# CLI Log Level Command - Complete! ✅

## Overview

Added a new command to the Node.js CLI (`flapii`) to manage the backend's runtime log level via the config service API.

## What Was Implemented

### New CLI Command: `config log-level`

**Location:** `cli/src/commands/config/log-level.ts`

**Subcommands:**
1. **`list`** - List all valid log levels with descriptions
2. **`get`** - Get the current backend log level
3. **`set <level>`** - Set the backend log level (debug, info, warning, error)

### Features

✅ **Three subcommands** for complete log level management
✅ **Authentication handling** - Clear error messages when token is missing
✅ **Input validation** - Validates log level before sending to backend
✅ **Environment variable support** - Can use `FLAPI_CONFIG_SERVICE_TOKEN`
✅ **Beautiful output** - Colored, formatted output with emoji indicators
✅ **Error handling** - Handles 401, 400, and network errors gracefully
✅ **Help text** - Full documentation in `--help` output

## Usage

### List Valid Log Levels
```bash
flapii config log-level list
```

**Output:**
```
📋 Valid Log Levels
════════════════════════════════════════════════════════════
  debug      Verbose output for debugging
  info       General informational messages
  warning    Warning messages only
  error      Error messages only

Usage: flapii config log-level set <level>
```

### Get Current Log Level
```bash
# Using CLI flags
flapii config log-level get \
  --base-url http://localhost:8080 \
  --config-service-token YOUR_TOKEN

# Using environment variables
export FLAPI_BASE_URL=http://localhost:8080
export FLAPI_CONFIG_SERVICE_TOKEN=YOUR_TOKEN
flapii config log-level get
```

**Output:**
```
📊 Current Log Level
════════════════════════════════════════════════════════════
Level: info
```

### Set Log Level
```bash
# Set to debug for verbose logging
flapii config log-level set debug \
  --base-url http://localhost:8080 \
  --config-service-token YOUR_TOKEN

# Set back to info
flapii config log-level set info \
  --base-url http://localhost:8080 \
  --config-service-token YOUR_TOKEN
```

**Output:**
```
✓ Log level updated
════════════════════════════════════════════════════════════
New level: debug
Message: Log level updated successfully

Note: Log level will reset on server restart
```

## Error Handling

### Missing Authentication
```bash
$ flapii config log-level get
✘ Authentication failed. Config service token required.
Set token with --config-service-token or FLAPI_CONFIG_SERVICE_TOKEN env var
```

### Invalid Log Level
```bash
$ flapii config log-level set invalid
✘ Invalid log level: invalid
Valid levels: debug, info, warning, error
```

### Backend Errors
- **401 Unauthorized** - Shows authentication error with helpful hint
- **400 Bad Request** - Shows validation error from backend
- **Network errors** - Shows connection error message

## Files Created/Modified

### New Files
- **`cli/src/commands/config/log-level.ts`** - Log level command implementation

### Modified Files
- **`cli/src/commands/config/index.ts`** - Register log-level commands
- **`cli/Readme.md`** - Documentation for new command

## Implementation Details

### Authentication
Uses the config service token from:
1. `--config-service-token` CLI flag
2. `FLAPI_CONFIG_SERVICE_TOKEN` environment variable

The token is automatically added to requests via the existing `createApiClient` logic.

### API Endpoints Used
- `GET /api/v1/_config/log-level` - Get current level
- `PUT /api/v1/_config/log-level` - Set new level (with JSON body: `{"level": "debug"}`)

### TypeScript Types
```typescript
const VALID_LOG_LEVELS = ['debug', 'info', 'warning', 'error'] as const;
type LogLevel = typeof VALID_LOG_LEVELS[number];
```

### Console Output
Uses the existing `Console` helper for consistent formatting:
- `Console.info()` - Blue/cyan informational messages
- `Console.success()` - Green success messages  
- `Console.error()` - Red error messages
- `chalk` for colors and styling

## Testing

All commands tested successfully:

### ✅ List Command
```bash
$ node dist/index.js config log-level list
# Output: Lists all valid levels with descriptions
```

### ✅ Get Command
```bash
$ node dist/index.js --config-service-token test1234567890123456789012345678 \
    --base-url http://localhost:8080 config log-level get
# Output: Current log level (info)
```

### ✅ Set Command
```bash
$ node dist/index.js --config-service-token test1234567890123456789012345678 \
    --base-url http://localhost:8080 config log-level set debug
# Output: Success message with new level
```

### ✅ Environment Variables
```bash
$ FLAPI_CONFIG_SERVICE_TOKEN=test1234567890123456789012345678 \
  FLAPI_BASE_URL=http://localhost:8080 \
  node dist/index.js config log-level get
# Works correctly
```

### ✅ Error Cases
- Invalid log level → Shows error and valid options
- Missing authentication → Shows clear error with hint
- Backend unavailable → Shows connection error

## Integration with Backend

Works seamlessly with the backend endpoints implemented earlier:
- Backend: `GET/PUT /api/v1/_config/log-level` (C++)
- CLI: `config log-level get/set` (TypeScript)

Both use the same authentication mechanism (Bearer token via `Authorization` header).

## Benefits

### For Developers
- ✅ **Debug production issues** without restarting the server
- ✅ **Reduce log noise** in stable environments
- ✅ **Temporarily increase verbosity** for troubleshooting
- ✅ **Scriptable** for automation and CI/CD

### For Operations
- ✅ **Zero downtime** log level changes
- ✅ **Immediate effect** - no restart required
- ✅ **Safe** - changes don't persist (reset on restart)
- ✅ **Auditable** - all changes via authenticated API

### For Teams
- ✅ **Consistent interface** with other CLI commands
- ✅ **Well documented** in README
- ✅ **Easy to remember** command structure
- ✅ **Helpful error messages** for common mistakes

## Use Cases

### Debugging a Live Issue
```bash
# Increase verbosity to debug
flapii config log-level set debug

# Monitor logs...
tail -f /tmp/flapi.log

# Reduce back to normal
flapii config log-level set info
```

### Automated Testing
```bash
#!/bin/bash
# Enable debug logging for test run
flapii config log-level set debug

# Run tests
./run_tests.sh

# Restore original level
flapii config log-level set info
```

### Production Monitoring
```bash
# Check current level
CURRENT=$(flapii config log-level get --output json | jq -r '.level')
echo "Backend is running at log level: $CURRENT"
```

## Future Enhancements

Possible future improvements:
- [ ] Add `--duration` flag to temporarily change level (auto-revert)
- [ ] Add log level history/audit trail
- [ ] Support wildcards for selective module logging
- [ ] Add `config log-level watch` to monitor changes
- [ ] Integration with monitoring tools (Prometheus metrics)

## Summary

The CLI log level command provides a complete, production-ready solution for managing backend log verbosity:

- ✅ **Implemented** - All subcommands working
- ✅ **Tested** - All scenarios verified
- ✅ **Documented** - README updated
- ✅ **Integrated** - Works with backend API
- ✅ **User-friendly** - Clear output and error messages

**Status: READY TO USE! 🚀**

Simply build the CLI and start using:
```bash
cd cli
npm run build
node dist/index.js config log-level --help
```

