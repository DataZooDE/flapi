# AI-Assisted Endpoint Wizard Guide

The endpoint wizard provides two ways to create flAPI endpoints: **AI-assisted** (using Google Gemini) or **manual** (interactive step-by-step).

## Quick Start

### AI-Assisted Mode (Recommended)

```bash
# Start the AI wizard
flapii endpoints wizard --ai

# Or let the wizard ask you:
flapii endpoints wizard
# Choose: "AI-Assisted (using Google Gemini)"
```

### Manual Mode

```bash
flapii endpoints wizard
# Choose: "Manual (step-by-step)"
```

## AI-Assisted Endpoint Creation

### Setting Up Authentication

The first time you use AI features, you'll be prompted for authentication:

```bash
$ flapii endpoints wizard --ai

To use AI-assisted endpoint creation, you need a Google Gemini API key.

? How would you like to authenticate with Gemini?
  ❯ API Key
    OAuth (Google Account)
```

#### Option 1: API Key (Recommended)

1. Get a free API key: https://aistudio.google.com/app/apikey
2. When prompted, paste your API key
3. The key is stored securely in `~/.flapi/config.json` (readable only by your user)

```bash
? Enter your Google Gemini API key: ••••••••••••••••••••
✓ Configuration saved
```

#### Option 2: Environment Variable

Set the API key as an environment variable (no prompt):

```bash
export FLAPI_GEMINI_KEY="your-api-key-here"
flapii endpoints wizard --ai
```

#### Option 3: OAuth (Future)

OAuth support allows browser-based authentication without storing credentials locally.

### Natural Language Description

When using AI mode, describe your endpoint in natural language:

```
Describe the endpoint you want to create:
(Press Enter twice to finish)

> I need a GET endpoint to fetch orders by customer ID.
> It should support optional filtering by date range.
> Include pagination with limit/offset parameters.
> Cache results for 10 minutes.
```

**Good descriptions include:**
- **What data**: "customer orders", "product inventory", "sales transactions"
- **How to filter**: "by customer ID", "by date range", "by status"
- **Parameters**: "limit", "offset", "sort_by"
- **Caching**: "cache for 5 minutes", "no caching"
- **Access pattern**: "read-only", "supports create/update"

**Examples:**

```
Create a GET endpoint for the customers table.
Include parameters for filtering by industry and region.
Cache for 1 hour.
```

```
Build a POST endpoint to insert new orders.
Parameters: customer_id (path), order_date (body), items array (body).
Validate that order_date is not in the past.
```

```
I need a DELETE endpoint for removing old cache entries.
Path: /cache/{id}
Require admin authentication.
```

### Review and Edit

After AI generates the configuration, you'll see a summary:

```
┌─────────────────────────────────────────┐
│ Generated Endpoint Configuration        │
├─────────────────────────────────────────┤
│ Endpoint Name: get_customer_orders      │
│ URL Path: /customers/{customer_id}/orders
│ HTTP Method: GET                        │
│ Connection: customer_db                 │
│ Table: orders                           │
│ Parameters: 3                           │
│ Cache: Enabled (600s)                   │
│ Description: Fetch orders by customer   │
└─────────────────────────────────────────┘

AI Reasoning:
- Path parameters for customer_id enable RESTful design
- Query parameters for filtering maintain flexibility
- Cache recommended due to potentially expensive joins

? What would you like to do?
  ❯ Accept and save
    Edit configuration
    Regenerate with different description
    Switch to manual mode
    Cancel
```

**Available actions:**

- **Accept and save**: Creates endpoint immediately
- **Edit configuration**: Change name, path, parameters, cache settings
- **Regenerate**: Try again with a refined description
- **Switch to manual mode**: Create endpoint manually instead
- **Cancel**: Exit without saving

### Editing Generated Configurations

Select "Edit configuration" to modify any aspect:

```
? What would you like to edit?
  ❯ Endpoint name
    URL path
    Description
    HTTP method
    Parameters
    Cache settings
    Done editing
```

#### Edit Parameters

Add, remove, or modify endpoint parameters:

```
? What would you like to do?
  ❯ Add parameter
    Remove parameter
    Done

? Parameter name: start_date
? Parameter type: string
? Is required? No
? Parameter location: query
? Description (optional): Start date for filtering
```

#### Edit Cache Settings

Enable/disable caching and set TTL:

```
? Enable caching? Yes
? Cache TTL in seconds: 600
```

## Manual Mode

When using manual mode, the wizard guides you step-by-step:

### 1. Endpoint Metadata

```
? Endpoint name (e.g., get_customers): fetch_active_orders
? URL path (e.g., /customers): /orders/active
? Description (optional): Get all active customer orders
? Select connection: [customer_db, analytics_db]
? Table name: orders
```

### 2. HTTP Method

```
? HTTP method
  ❯ GET (read)
    POST (create)
    PUT (update)
    DELETE (delete)
```

### 3. Parameters

```
Configure parameters (or skip if none):

? Parameter name: customer_id
? Parameter type: string
? Is required? Yes
? Parameter location: path
? Description (optional): Customer ID filter
? Add another parameter? Yes

? Parameter name: limit
? Parameter type: integer
? Is required? No
? Parameter location: query
? Description (optional): Maximum results to return
? Add another parameter? No
```

### 4. Cache Configuration

```
? Enable caching? Yes
? Cache TTL in seconds: 300
```

### 5. Confirmation

```
? Save endpoint configuration? Yes
✓ Endpoint fetch_active_orders created
```

## Advanced Options

### Save to File Instead of Creating

Useful for version control and review:

```bash
# Generate configuration but don't create endpoint
flapii endpoints wizard --output-file my-endpoint.yaml

# Review the YAML
cat my-endpoint.yaml

# Create endpoint later
flapii endpoints create --file my-endpoint.yaml
```

### Dry Run (Preview)

See what would be created without making changes:

```bash
flapii endpoints wizard --dry-run

# Shows:
# - Generated configuration summary
# - What would be created
# - No actual changes made
```

### Skip Validation

For batch processing or trusted configurations:

```bash
flapii endpoints wizard --skip-validation
# Skips server-side validation checks
```

### Combine Flags

```bash
# AI mode, save to file, preview first
flapii endpoints wizard --ai --output-file config.yaml --dry-run
```

## AI Best Practices

### Be Specific

```bash
# ✓ Good - specific and detailed
"Create a GET endpoint for customer accounts.
Path: /accounts/{account_id}
Parameters: account_id (path), include_transactions (query bool)
Connection: postgres-db
Cache: 15 minutes"

# ✗ Avoid - too vague
"Create an endpoint for getting data"
```

### Include RESTful Path Parameters

```bash
# ✓ Good - RESTful design
"/customers/{customer_id}/orders"

# ✗ Avoid - non-standard
"/get-customer-orders?customer_id=123"
```

### Specify Parameter Locations

```bash
# ✓ Good - clear about parameter usage
"customer_id as path parameter, sort_by as query parameter,
filters as request body"

# ✗ Vague
"customers, sort, filters as parameters"
```

### Mention Cache Constraints

```bash
# ✓ Good - realistic caching
"Cache for 5 minutes (data updates hourly)"

# ✗ Unrealistic
"Cache for 1 day (for real-time transaction data)"
```

## Troubleshooting

### "API Key Invalid"

```bash
# Verify your API key works
curl -X POST "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash-exp:generateContent" \
  -H "x-goog-api-key: YOUR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "contents": [{
      "parts": [{"text": "Say hello"}]
    }]
  }'

# Reset stored credentials
rm ~/.flapi/config.json
flapii endpoints wizard --ai
```

### "Quota Exceeded"

Google Gemini has usage limits on free tier. Check your usage:

```bash
# Visit: https://aistudio.google.com/app/apikey
# Check: Usage and billing settings
```

**Solutions:**
- Wait for quota to reset (usually daily)
- Upgrade to paid plan
- Use manual mode instead

### Generated Config Looks Wrong

Try these strategies:

1. **Be more specific in description**:
   ```
   Before: "Create an endpoint for customers"
   After: "GET endpoint at /customers/{id} with query params for status and region"
   ```

2. **Review and edit**:
   - Select "Edit configuration" after generation
   - Manually fix incorrect parameters or paths
   - Click "Accept and save"

3. **Regenerate with feedback**:
   - Select "Regenerate with different description"
   - Refine your description based on what went wrong
   - Be more explicit about parameter names and types

4. **Fall back to manual mode**:
   - Select "Switch to manual mode"
   - Create endpoint step-by-step

### AI Generated Invalid SQL

If the created endpoint returns SQL errors, the AI may have misunderstood your table schema:

```bash
# Check available tables
flapii schema connections
flapii schema refresh --connection connection-name

# List tables in connection
flapii schema tables --connection connection-name

# Check table structure
flapii schema describe table_name --connection connection-name
```

**Fix by:**
1. Regenerating with more specific table/column names
2. Editing the endpoint configuration manually
3. Checking the connection has access to the table

## Configuration Format

The wizard generates endpoint configurations in flAPI format:

```yaml
endpoint_name: get_customer_orders
url_path: /customers/{customer_id}/orders
method: GET
connection: customer_db
table: orders
description: Fetch orders for a specific customer
parameters:
  - name: customer_id
    type: string
    required: true
    location: path
    description: Customer identifier
  - name: start_date
    type: string
    required: false
    location: query
    description: Filter by start date
cache:
  enabled: true
  ttl: 600
```

## Programmatic Usage

### Creating Endpoints with Generated Configs

```bash
# Generate and save
flapii endpoints wizard --ai --output-file endpoint.yaml

# Apply to server
flapii endpoints create --file endpoint.yaml

# Verify it was created
flapii endpoints get /customers/{customer_id}/orders
```

### Batch Processing

```bash
# Create from stored configuration file
for file in configs/*.yaml; do
  flapii endpoints create --file "$file" --skip-validation
done
```

## Performance Considerations

### API Latency

The AI generation process takes 5-10 seconds:
- ~1s: Connect to Gemini API
- ~3-5s: AI generates configuration
- ~1s: Parse and validate response

This is one-time cost per endpoint creation.

### Cost

Google Gemini free tier includes:
- **60 requests per minute**
- **1,500 requests per day**

For typical usage (1-2 endpoints per day), free tier is sufficient.

## Security

### Credential Storage

- API keys stored in `~/.flapi/config.json` with `0600` permissions
- Only readable by your user account
- Consider key rotation periodically
- Never commit API keys to version control

### Data Privacy

When using AI generation:
- Your endpoint description is sent to Google's API
- Connection names are sent for context
- No actual data or passwords are sent
- Complies with Google's privacy policy

### Recommendation

Use environment variable for CI/CD:

```bash
# GitHub Actions example
env:
  FLAPI_GEMINI_KEY: ${{ secrets.GEMINI_API_KEY }}
```

## Examples

### Example 1: E-commerce Product Search

```bash
$ flapii endpoints wizard --ai

? How would you like to create the endpoint?
  ❯ AI-Assisted (using Google Gemini)

Describe the endpoint you want to create:
> Create a GET endpoint to search for products.
> Parameters: search_query (query string), category (query, optional),
> sort_by (query, optional: name/price/rating), limit (query int, default 20)
> Cache for 30 minutes.

⠋ Analyzing with Gemini...
✓ Configuration generated!

┌─────────────────────────────────────────┐
│ Generated Endpoint Configuration        │
├─────────────────────────────────────────┤
│ Endpoint Name: search_products          │
│ URL Path: /products/search              │
│ HTTP Method: GET                        │
│ Connection: ecommerce_db                │
│ Table: products                         │
│ Parameters: 4                           │
│ Cache: Enabled (1800s)                  │
└─────────────────────────────────────────┘

? What would you like to do?
  ❯ Accept and save
    Edit configuration
    Regenerate with different description
    Switch to manual mode
    Cancel

✓ Endpoint search_products created
```

### Example 2: Admin Data Export

```bash
$ flapii endpoints wizard

? How would you like to create the endpoint?
  ❯ Manual (step-by-step)

? Endpoint name: export_audit_log
? URL path: /admin/audit-log/export
? Description: Export audit log for compliance
? Select connection: audit_db
? Table name: audit_log
? HTTP method: GET
? Parameter name: start_date
? Parameter type: string
? Is required? Yes
? Parameter location: query
? Add another parameter? Yes

? Parameter name: end_date
? Parameter type: string
? Is required? Yes
? Parameter location: query
? Add another parameter? No

? Enable caching? No
? Save endpoint configuration? Yes

✓ Endpoint export_audit_log created
```

## Getting Help

- **API Key issues**: https://aistudio.google.com/app/apikey
- **Configuration format**: See `docs/config.md`
- **Endpoint examples**: See `examples/` directory
- **Bug reports**: GitHub issues

