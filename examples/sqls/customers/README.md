# Customer API Examples - Refactored with YAML Includes

This directory demonstrates FLAPI's unified configuration approach and advanced YAML include functionality through a comprehensive customer data API example.

## 🏗️ Architecture Overview

The refactored structure eliminates configuration duplication by using YAML includes to share common components across different endpoint types.

```
customers/
├── common/                          # 📁 Shared Configuration Components
│   ├── customer-common.yaml        # 🎯 CONSOLIDATED: All shared configurations
│   ├── customer-request-fields.yaml # [LEGACY] Common request parameters
│   ├── customer-auth.yaml          # [LEGACY] Production authentication  
│   ├── customer-auth-dev.yaml      # [LEGACY] Development authentication
│   ├── customer-auth-prod.yaml     # [LEGACY] Production JWT authentication
│   ├── customer-rate-limit.yaml    # [LEGACY] Rate limiting configuration
│   └── customer-connection.yaml    # [LEGACY] Connection and template configuration
├── 
├── customers-rest.yaml             # 🌐 Pure REST endpoint
├── customers-mcp-tool.yaml         # 🤖 Pure MCP tool
├── customers-mcp-resource.yaml     # 📋 MCP resource (schema)
├── customers-mcp-prompt.yaml       # 💭 MCP prompt (AI analysis)
├── 
├── customers-unified-advanced.yaml # 🔄 REST + MCP tool in one file
├── customers-environment-aware.yaml # 🌍 Environment-specific configuration
├── customers-complete-example.yaml # 📚 Documentation example
├── 
├── customers-rest-consolidated.yaml # 🎯 NEW: REST using consolidated config
├── customers-mcp-tool-consolidated.yaml # 🎯 NEW: MCP tool using consolidated config  
├── customers-environment-consolidated.yaml # 🎯 NEW: Environment-aware consolidated
├── 
├── README.md                       # 📖 This file
└── CONSOLIDATION-COMPARISON.md     # 📊 Comparison of approaches
```

## 🎯 Endpoint Types Demonstrated

### 1. REST Endpoint (`customers-rest.yaml`)
- **Purpose**: Traditional HTTP REST API
- **URL**: `GET /customers/`
- **Features**: Pagination, caching, full request validation
- **Use Case**: Web applications, mobile apps, direct API access

### 2. MCP Tool (`customers-mcp-tool.yaml`)
- **Purpose**: AI agent tool for programmatic data access
- **Name**: `customer_lookup`
- **Features**: Same data access as REST, optimized for AI usage
- **Use Case**: AI agents, chatbots, automated analysis

### 3. MCP Resource (`customers-mcp-resource.yaml`)
- **Purpose**: Provides database schema information
- **Name**: `customer_schema`
- **Features**: Static schema definition, lightweight access
- **Use Case**: AI agents understanding data structure

### 4. MCP Prompt (`customers-mcp-prompt.yaml`)
- **Purpose**: AI prompt template for customer analysis
- **Name**: `customer_data_analysis`
- **Features**: Structured analysis template with parameters
- **Use Case**: AI-powered customer insights and reporting

## 🎯 Configuration Approaches

### Approach 1: Multiple Files (Original)
Each configuration component in its own file:
```yaml
{{include:request from common/customer-request-fields.yaml}}
{{include:auth from common/customer-auth.yaml}}
{{include:rate-limit from common/customer-rate-limit.yaml}}
```

### Approach 2: Consolidated File (Recommended)
All configurations in a single file with section includes:
```yaml
{{include:request from common/customer-common.yaml}}
{{include:auth from common/customer-common.yaml}}
{{include:rate-limit from common/customer-common.yaml}}
```

**Benefits of Consolidated Approach:**
- ✅ **Fewer Files**: 1 file instead of 6+ files
- ✅ **Better Organization**: All related configs in one place
- ✅ **More Variants**: Multiple auth, rate-limit, cache options
- ✅ **Environment-Aware**: Built-in dev/staging/prod variants
- ✅ **Easier Maintenance**: Single file to update

See `CONSOLIDATION-COMPARISON.md` for detailed comparison.

## 🔧 YAML Include Features Demonstrated

### Basic Includes (Consolidated Approach)
```yaml
# Include sections from consolidated configuration file
{{include:request from common/customer-common.yaml}}
{{include:auth from common/customer-common.yaml}}
{{include:rate-limit from common/customer-common.yaml}}
```

### Configuration Variants (New Feature)
```yaml
# Select specific configuration variants
{{include:auth-dev from common/customer-common.yaml}}      # Development auth
{{include:auth-prod from common/customer-common.yaml}}     # Production JWT auth
{{include:rate-limit-high from common/customer-common.yaml}} # High-traffic limits
{{include:rate-limit-ai from common/customer-common.yaml}}   # AI tool limits
{{include:cache-performance from common/customer-common.yaml}} # High-perf cache
```

### Environment-Aware Includes
```yaml
# Different authentication based on environment
{{include:auth from common/customer-auth-dev.yaml if env.ENVIRONMENT}}
{{include:auth from common/customer-auth-prod.yaml if env.ENVIRONMENT}}
```

### Environment Variables in Configuration
```yaml
# Dynamic values from environment
cache-table-name: customers_{{env.ENVIRONMENT}}_cache
refresh-time: '{{env.CACHE_REFRESH_TIME}}'
```

## 🚀 Usage Examples

### 1. REST API Access
```bash
# Get all customers
curl "http://localhost:8080/customers/"

# Get customer by ID
curl "http://localhost:8080/customers/?id=123"

# Get customers by segment
curl "http://localhost:8080/customers/?segment=vip"
```

### 2. MCP Tool Access
```bash
# List available MCP tools
curl -X POST http://localhost:8081/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc": "2.0", "id": 1, "method": "tools/list"}'

# Call customer lookup tool
curl -X POST http://localhost:8081/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0", 
    "id": 2, 
    "method": "tools/call",
    "params": {
      "name": "customer_lookup",
      "arguments": {"id": "123"}
    }
  }'
```

### 3. MCP Resource Access
```bash
# Get customer schema
curl -X POST http://localhost:8081/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0", 
    "id": 3, 
    "method": "resources/read",
    "params": {
      "uri": "flapi://customer_schema"
    }
  }'
```

### 4. MCP Prompt Access
```bash
# Get customer analysis prompt
curl -X POST http://localhost:8081/mcp/jsonrpc \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0", 
    "id": 4, 
    "method": "prompts/get",
    "params": {
      "name": "customer_data_analysis",
      "arguments": {
        "customer_id": "123",
        "analysis_type": "behavioral",
        "time_period": "last_90_days"
      }
    }
  }'
```

## 🌍 Environment Configuration

Set these environment variables to customize behavior:

```bash
# Authentication
export CUSTOMER_API_ADMIN_USER="admin"
export CUSTOMER_API_ADMIN_PASSWORD="secure_admin_password"
export CUSTOMER_API_READ_USER="readonly"
export CUSTOMER_API_READ_PASSWORD="readonly_password"
export CUSTOMER_API_AI_USER="ai_agent"
export CUSTOMER_API_AI_PASSWORD="ai_agent_password"

# Environment-specific settings
export ENVIRONMENT="development"  # or "staging", "production"
export CACHE_REFRESH_TIME="5m"
export ENABLE_HEARTBEAT="true"
export DEBUG_MODE="true"

# Production JWT settings (if using JWT auth)
export CUSTOMER_API_JWT_SECRET="your-jwt-secret-key"
export CUSTOMER_API_JWT_ISSUER="your-api-issuer"
```

## 📊 Benefits of This Approach

### 1. **Elimination of Duplication**
- Request parameters defined once, used everywhere
- Authentication configuration shared across endpoints
- Rate limiting and connection settings reused

### 2. **Environment Flexibility**
- Same configuration adapts to dev/staging/production
- Environment-specific overrides without duplication
- Conditional features based on environment variables

### 3. **Unified Data Access**
- Same SQL template serves REST and MCP endpoints
- Consistent data format across access methods
- Shared caching benefits all access patterns

### 4. **AI-Ready Architecture**
- MCP tools provide programmatic access for AI agents
- MCP resources help AI understand data structure
- MCP prompts guide AI analysis and insights

### 5. **Maintainability**
- Single source of truth for common configurations
- Easy to update authentication, rate limits, etc.
- Clear separation of concerns

## 🔍 Configuration Analysis

| File | REST | MCP Tool | MCP Resource | MCP Prompt | Shared Components |
|------|------|----------|--------------|------------|-------------------|
| `customers-rest.yaml` | ✅ | ❌ | ❌ | ❌ | ✅ All |
| `customers-mcp-tool.yaml` | ❌ | ✅ | ❌ | ❌ | ✅ All |
| `customers-mcp-resource.yaml` | ❌ | ❌ | ✅ | ❌ | ✅ Connection only |
| `customers-mcp-prompt.yaml` | ❌ | ❌ | ❌ | ✅ | ❌ None |
| `customers-unified-advanced.yaml` | ✅ | ✅ | ❌ | ❌ | ✅ All |
| `customers-complete-example.yaml` | ✅ | ✅ | 📖 Ref | 📖 Ref | ✅ All |

## 🎓 Learning Path

1. **Start with**: `customers-rest.yaml` - Basic REST endpoint with includes
2. **Then explore**: `customers-mcp-tool.yaml` - Same data via MCP protocol  
3. **Add context**: `customers-mcp-resource.yaml` - Schema information for AI
4. **Enable AI**: `customers-mcp-prompt.yaml` - AI analysis capabilities
5. **Optimize**: `customers-unified-advanced.yaml` - Maximum reuse
6. **Scale**: `customers-environment-aware.yaml` - Multi-environment deployment

This example demonstrates FLAPI's power in creating maintainable, scalable, and AI-ready data APIs with minimal configuration duplication.
