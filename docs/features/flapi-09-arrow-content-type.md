# flapi Arrow IPC Streaming

## Architecture and Design Document

---

**Document Information**

| Field | Value |
|-------|-------|
| Feature | Apache Arrow IPC Streaming over HTTP |
| Reference | GitHub Issue #9 |
| Status | Proposal |
| Version | 1.0 |
| Date | January 2026 |

---

## Table of Contents

1. Executive Summary
2. Problem Analysis
3. Design Principles
4. System Architecture
5. Component Design
6. Integration Strategy
7. Edge Cases and Failure Modes
8. Security Architecture
9. Performance Engineering
10. Batching Model: Paradigm Shift from JSON Pagination
11. Configuration Schema
12. Observability
13. Testing Strategy
14. Implementation Phases
15. Risk Assessment
16. Open Decisions

---

## 1. Executive Summary

This document specifies the architecture for adding Apache Arrow IPC streaming as an alternative response format in flapi. The feature addresses the performance limitations of JSON serialization for analytical workloads, enabling near-zero-copy data transfer to modern data science clients.

The design philosophy positions flapi as orchestration glue between DuckDB's native Arrow capabilities and Crow's HTTP streaming infrastructure. Minimal custom serialization logic is introduced; instead, the implementation leverages battle-tested components from both ecosystems.

Key architectural decisions:

- Content negotiation via standard HTTP Accept headers with query parameter fallback
- Streaming response model using HTTP/1.1 chunked transfer encoding
- Memory-bounded operation via configurable limits and backpressure mechanisms
- Fail-fast error handling with graceful degradation to JSON on Arrow failures
- Security hardening through schema validation and resource limits

The expected outcome is 10-30x improvement in data transfer performance for analytical clients while maintaining full backward compatibility with existing JSON consumers.

---

## 2. Problem Analysis

### 2.1 Current State

flapi currently serializes all query results to JSON, regardless of client capabilities. This approach introduces three performance penalties:

**Serialization Overhead**: DuckDB operates on columnar data internally. Converting to JSON requires row-by-row iteration, type conversion to strings, and construction of nested JSON structures. For a 1M row result set with 10 columns, this involves approximately 10M type conversions and string allocations.

**Wire Format Inefficiency**: JSON is a text-based format with significant syntactic overhead. Field names repeat for every row. Numeric values are encoded as decimal strings. The resulting payload size is typically 3-10x larger than binary alternatives.

**Client-Side Parsing**: Consumers must parse the JSON text, reconstruct type information, and convert to their internal representation. For analytical clients (Polars, PyArrow, Pandas), this ultimately means converting back to columnar format—the inverse of the server-side serialization.

### 2.2 Proposed Solution

Apache Arrow IPC streaming eliminates these penalties by maintaining columnar format throughout the pipeline:

**Server Side**: DuckDB query results are already Arrow-compatible internally. The Arrow C Data Interface provides zero-copy access to result buffers. IPC serialization adds minimal framing overhead.

**Wire Format**: Arrow IPC is a binary format where data buffers are transmitted in their native columnar layout. Typical compression ratios of 3-10x are achievable with standard codecs.

**Client Side**: Arrow-native clients can memory-map or directly reference received buffers without parsing or conversion. The data arrives in the exact format required for analytical operations.

### 2.3 Scope Boundaries

**In Scope**:
- Arrow IPC streaming format for query responses
- Content negotiation mechanism
- Configuration at global and endpoint levels
- Compression support (LZ4, ZSTD)
- Integration with existing authentication and rate limiting

**Out of Scope**:
- Arrow Flight protocol (gRPC-based, different transport)
- Bidirectional Arrow streaming (request bodies)
- Arrow IPC file format with footer (streaming only)
- Schema-on-read or schema evolution

---

## 3. Design Principles

### 3.1 Minimal Glue Philosophy

flapi's role is coordination, not reimplementation. The design delegates serialization to DuckDB's Arrow interface and transport to Crow's HTTP layer. Custom logic is limited to:

- Format selection based on content negotiation
- Configuration management and validation
- Error translation between subsystems
- Resource lifecycle management

This principle minimizes maintenance burden and ensures compatibility with upstream improvements in both DuckDB and Crow.

### 3.2 Progressive Enhancement

Arrow support is additive. The feature introduces no changes to existing JSON response paths. Clients that do not request Arrow format receive identical behavior to current flapi versions. This enables gradual adoption and rollback without service disruption.

### 3.3 Fail-Safe Degradation

When Arrow serialization encounters errors (unsupported types, memory pressure, client disconnection), the system degrades gracefully. Configuration options allow operators to choose between:

- Error response with appropriate HTTP status
- Automatic fallback to JSON with warning header
- Partial stream with error termination marker

### 3.4 Resource Boundedness

All Arrow operations execute within configurable resource limits. Memory pools have maximum allocation sizes. Batch counts have upper bounds. Timeouts apply at connection, query, and batch levels. No single request can monopolize server resources.

### 3.5 Security by Default

Untrusted input (malicious schemas, oversized batches) cannot cause unbounded resource consumption or code execution. Validation occurs before allocation. Extension type loading is disabled. Schema depth and complexity limits are enforced.

---

## 4. System Architecture

### 4.1 High-Level Data Flow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              flapi Process                               │
│                                                                         │
│  ┌─────────────┐    ┌──────────────┐    ┌────────────────────────────┐ │
│  │   Crow      │    │   Request    │    │      Response Path         │ │
│  │   HTTP      │───▶│   Router     │───▶│   Selection                │ │
│  │   Server    │    │              │    │                            │ │
│  └─────────────┘    └──────────────┘    └────────────┬───────────────┘ │
│                                                       │                 │
│                           ┌───────────────────────────┼─────────────┐   │
│                           │                           │             │   │
│                           ▼                           ▼             │   │
│                    ┌─────────────┐           ┌─────────────┐        │   │
│                    │    JSON     │           │   Arrow     │        │   │
│                    │  Serializer │           │  Serializer │        │   │
│                    │  (existing) │           │   (new)     │        │   │
│                    └──────┬──────┘           └──────┬──────┘        │   │
│                           │                         │               │   │
│                           └────────────┬────────────┘               │   │
│                                        │                            │   │
│                                        ▼                            │   │
│                              ┌─────────────────┐                    │   │
│                              │   DuckDB        │                    │   │
│                              │   Query Engine  │                    │   │
│                              └─────────────────┘                    │   │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 4.2 Component Responsibilities

**HTTP Layer (Crow)**
- Connection management and HTTP parsing
- Content negotiation header extraction
- Chunked transfer encoding for streaming responses
- Connection timeout enforcement
- TLS termination (when configured)

**Request Router**
- URL pattern matching to endpoint configurations
- Parameter extraction and validation
- Authentication and authorization checks
- Rate limit enforcement
- Format selection delegation

**Response Path Selection**
- Accept header parsing and quality value handling
- Query parameter override processing
- Endpoint configuration lookup
- Format capability validation
- Serializer instantiation

**JSON Serializer (Existing)**
- Row-by-row result iteration
- Type conversion to JSON primitives
- Pagination and envelope construction
- No modifications required

**Arrow Serializer (New)**
- Schema extraction from query result
- Record batch iteration via streaming fetch
- IPC message construction
- Compression application
- Chunked response writing

**DuckDB Query Engine**
- SQL template execution
- Result streaming via fetch interface
- Arrow C Data Interface exposure
- Memory pool management

### 4.3 Threading Model

The Arrow serialization path respects flapi's existing threading model:

- Request handling occurs on Crow's thread pool
- DuckDB query execution uses its internal parallelism
- Arrow serialization is single-threaded per request
- No shared mutable state between concurrent requests

Each request receives an isolated serialization context with its own memory budget and batch iterator. Thread safety is achieved through isolation rather than synchronization.

---

## 5. Component Design

### 5.1 Content Negotiation Component

**Purpose**: Determine appropriate response format based on client capabilities and server configuration.

**Input Sources** (in priority order):
1. Query parameter override (e.g., `?format=arrow`)
2. HTTP Accept header with quality values
3. Endpoint-level default format configuration
4. Global default (JSON for backward compatibility)

**Accept Header Processing**:
- Parse comma-separated media types with quality values
- Recognize `application/vnd.apache.arrow.stream` as Arrow request
- Recognize `application/json` as JSON request
- Handle wildcards (`*/*`) according to server preference
- Extract codec preferences from media type parameters

**Output**: Format selection result containing:
- Selected format identifier (json, arrow)
- Compression codec (none, lz4, zstd)
- Quality value for response Content-Type construction

**Edge Cases**:
- Conflicting Accept and query parameter: Query parameter wins
- Unsupported format requested with no fallback: 406 Not Acceptable
- Arrow requested but endpoint disables it: 406 or JSON fallback (configurable)
- Malformed Accept header: Treat as `*/*`

### 5.2 Arrow Serialization Component

**Purpose**: Transform DuckDB query results into Arrow IPC stream bytes.

**Lifecycle**:
1. **Initialization**: Acquire memory pool allocation, validate schema compatibility
2. **Schema Phase**: Extract Arrow schema from result, serialize schema message
3. **Batch Phase**: Iterate result chunks, convert to Arrow arrays, serialize batch messages
4. **Termination**: Write EOS marker, release resources

**Schema Handling**:
- Extract schema via DuckDB's Arrow interface
- Validate all column types are Arrow-compatible
- Reject or convert unsupported types (configurable behavior)
- Set IPC format version based on configuration

**Batch Iteration**:
- Use DuckDB's streaming fetch to avoid materializing full result
- Convert each DataChunk to ArrowArray via C Data Interface
- Apply configured batch size limits (split large chunks if needed)
- Track row count for observability

**IPC Message Construction**:
- Delegate to nanoarrow for FlatBuffer metadata construction
- Apply compression codec to data buffers
- Compute and verify message alignment (8-byte boundaries)
- Emit complete IPC messages to output buffer

**Resource Management**:
- Memory pool with configurable maximum size
- Batch count limit to prevent infinite streams
- Per-batch timeout for detecting stalled queries
- Automatic cleanup on error or cancellation

### 5.3 Streaming Response Writer

**Purpose**: Transmit Arrow IPC bytes to client via HTTP chunked encoding.

**Integration with Crow**:
- Utilize Crow's response streaming interface
- Set appropriate headers before first write
- Write IPC messages as HTTP chunks
- Respect Crow's backpressure mechanisms

**Header Management**:
- Content-Type: `application/vnd.apache.arrow.stream` with codec parameter
- Transfer-Encoding: `chunked` (HTTP/1.1 only, implicit in HTTP/2)
- Cache-Control: `no-cache` (streaming responses should not be cached)
- X-Content-Type-Options: `nosniff` (prevent MIME type sniffing)

**Chunking Strategy**:
- One HTTP chunk per IPC message (schema, batch, EOS)
- Flush after each chunk to minimize client latency
- Respect Crow's buffer size limits
- Handle partial writes and retry

**Error During Streaming**:
- Connection closed by client: Abort gracefully, log incomplete transfer
- Serialization error: Write error marker (if protocol supports), close connection
- Timeout: Close connection, log timeout details

### 5.4 Memory Pool Component

**Purpose**: Provide bounded memory allocation for Arrow serialization.

**Design**:
- Wrapper around system allocator with tracking
- Configurable maximum allocation limit per request
- Configurable maximum total allocation across requests
- Allocation failure triggers graceful degradation

**Allocation Tracking**:
- Track current allocation per request
- Track peak allocation for capacity planning
- Expose metrics for monitoring systems

**Failure Handling**:
- Soft limit: Log warning, continue with degraded batch size
- Hard limit: Abort serialization, return error response
- System allocation failure: Abort, return 503 Service Unavailable

---

## 6. Integration Strategy

### 6.1 Integration with Existing Request Pipeline

The Arrow response path integrates at the response serialization stage, after query execution completes. No changes are required to:

- Endpoint configuration parsing (extended only)
- SQL template processing
- Parameter validation
- Authentication middleware
- Rate limiting
- Caching layer (DuckLake)

The format selection occurs after authentication and rate limiting, ensuring all security controls apply uniformly regardless of response format.

### 6.2 Integration with DuckDB

**Connection Management**:
- Use existing flapi connection pool
- No additional connection configuration required
- Arrow interface available on standard connections

**Query Execution**:
- Existing query execution path unchanged
- Same SQL templates serve both JSON and Arrow responses
- Result streaming via standard fetch interface

**Arrow Interface Usage**:
- Access Arrow schema via ToArrowSchema()
- Access data via ToArrowArray() on each chunk
- Release Arrow resources via standard release callbacks

**Extension Loading**:
- The `arrow` community extension provides file I/O (not required for HTTP streaming)
- Core Arrow C Data Interface is built into DuckDB
- nanoarrow dependency added to flapi for IPC serialization

### 6.3 Integration with Crow

**Response Object**:
- Use Crow's response streaming mode
- Set headers before body writes begin
- Binary body content (no character encoding issues)

**Streaming Mode Activation**:
- Crow automatically uses chunked encoding for streaming responses
- Leverage `stream_threshold` configuration for consistency
- Ensure response timeout disabled or extended for large streams

**Connection Lifecycle**:
- Crow manages connection keep-alive
- Arrow responses should complete and allow connection reuse
- Handle client disconnection via Crow's cancellation mechanism

### 6.4 Integration with MCP

The existing MCP (Model Context Protocol) support requires consideration:

- MCP tools returning large datasets could benefit from Arrow format
- MCP protocol currently specifies JSON-RPC transport
- Arrow integration for MCP deferred to future enhancement
- Current implementation: MCP responses remain JSON only

---

## 7. Edge Cases and Failure Modes

### 7.1 Stream Interruption Scenarios

**Client Disconnects Mid-Stream**:
- Detection: Crow signals write failure or connection close
- Response: Abort serialization, release resources, log partial transfer
- No retry or recovery (HTTP is stateless)

**Server Timeout During Streaming**:
- Detection: Batch fetch exceeds configured timeout
- Response: Attempt to write error marker, close connection
- Consideration: Long-running queries need appropriate timeout configuration

**Memory Exhaustion During Serialization**:
- Detection: Memory pool allocation fails
- Response: Abort current batch, write partial stream indicator if possible
- Recovery: Suggest client retry with smaller limit parameter

### 7.2 Data Compatibility Issues

**Unsupported DuckDB Types**:
- Detection: Schema extraction encounters unmappable type
- Response (configurable):
  - Option A: Reject entire request with 400 Bad Request
  - Option B: Omit unsupported columns, add warning header
  - Option C: Fall back to JSON with warning
- Types requiring attention: ENUM, UNION, MAP (version-dependent support)

**Schema Mismatch Between Batches**:
- Detection: Subsequent batch has different schema than initial
- Response: This indicates a DuckDB bug; abort with internal error
- Prevention: Schema is fixed at query compile time

**Empty Result Set**:
- Detection: First fetch returns zero rows
- Response: Write schema message followed by immediate EOS
- Client sees: Valid Arrow stream with schema but no data

### 7.3 Resource Exhaustion Scenarios

**Too Many Concurrent Arrow Requests**:
- Detection: Global memory pool limit reached
- Response: Queue or reject new Arrow requests, JSON unaffected
- Configuration: `arrow.max_concurrent_streams` limit

**Single Large Result Set**:
- Detection: Row count or batch count exceeds limits
- Response: Truncate stream at limit, write truncation marker
- Client notification: Custom metadata in final batch or HTTP trailer

**Slow Client (Backpressure)**:
- Detection: Write buffer full, Crow signals backpressure
- Response: Pause batch generation until buffer drains
- Timeout: If backpressure exceeds duration limit, abort stream

### 7.4 Protocol Edge Cases

**HTTP/2 Considerations**:
- Chunked transfer encoding is implicit (no header)
- DATA frames replace chunk boundaries
- Flow control handled by HTTP/2 layer
- Implementation: Detect protocol version, adjust header handling

**Proxy/CDN Buffering**:
- Issue: Intermediate proxies may buffer entire response
- Mitigation: Document requirement for streaming-compatible proxies
- Header hint: `X-Accel-Buffering: no` for Nginx

**Browser Clients**:
- Streams API support required for incremental processing
- Fallback: Full buffer before processing (memory constrained)
- CORS: Ensure Arrow content type in allowed headers

---

## 8. Security Architecture

### 8.1 Threat Model

**Attacker Profiles**:
- Unauthenticated external client attempting denial of service
- Authenticated client attempting resource exhaustion
- Malicious client sending crafted requests to trigger vulnerabilities

**Protected Assets**:
- Server availability (prevent DoS)
- Memory resources (prevent exhaustion)
- CPU resources (prevent computational DoS)
- Data confidentiality (existing auth model applies)

### 8.2 Input Validation

**Request Validation**:
- Accept header parsing with length limits
- Query parameter validation (format values allowlisted)
- Request size limits (existing Crow configuration)

**Output Safety**:
- Arrow schema generated from trusted DuckDB source
- No user-controlled data in schema metadata
- Extension types disabled (prevent code execution vectors)

### 8.3 Resource Limits

| Resource | Limit Type | Recommended Default |
|----------|------------|---------------------|
| Memory per request | Hard limit | 256 MB |
| Memory global pool | Hard limit | 2 GB |
| Batch count per stream | Hard limit | 10,000 batches |
| Row count per stream | Soft limit | 10,000,000 rows |
| Stream duration | Timeout | 10 minutes |
| Concurrent Arrow streams | Semaphore | 10 streams |

### 8.4 Security Considerations for IPC Parsing

While flapi primarily produces Arrow IPC (not consumes), future bidirectional support requires:

- Schema recursion depth limit (default: 64)
- Dictionary size limits before allocation
- Disable extension type registration
- Validate buffer sizes before memory mapping

### 8.5 Audit and Logging

Log the following for security monitoring:

- Arrow format requests (client IP, endpoint, auth identity)
- Resource limit triggers (which limit, request details)
- Serialization failures (type, error details)
- Abnormal stream terminations (reason, bytes sent)

---

## 9. Performance Engineering

### 9.1 Batch Size Optimization

Batch size affects latency, throughput, and memory usage:

| Batch Size | Characteristics | Recommended Use |
|------------|-----------------|-----------------|
| 1,024 rows | Low latency, high overhead | Real-time dashboards |
| 8,192 rows | Balanced | Interactive queries |
| 65,536 rows | High throughput | Bulk exports |
| 122,880 rows | DuckDB-aligned | Maximum efficiency |

Default: 122,880 rows (matches DuckDB's internal vector size for optimal conversion efficiency).

Configuration allows override at global, endpoint, and request levels.

### 9.2 Compression Strategy

**Codec Selection**:
- None: Lowest CPU, highest bandwidth; for fast networks
- LZ4: Fast compression/decompression; for balanced scenarios
- ZSTD (level 1-3): Best ratio with acceptable speed; for bandwidth-constrained

**Decision Factors**:
- Network bandwidth between server and typical clients
- Server CPU headroom
- Client decompression capability
- Data compressibility (varies by content)

**Recommendation**: Default to ZSTD level 1 for HTTP transport. Allow client preference via Accept header parameters.

### 9.3 Memory Efficiency

**Avoid Full Materialization**:
- Stream batches as they are produced
- Release each batch after serialization
- Never accumulate entire result in memory

**Buffer Management**:
- Reuse serialization buffers across batches
- Size output buffer to typical batch size
- Flush buffers promptly to limit memory footprint

**Memory Pool Sizing**:
- Per-request pool sized for expected batch size + overhead
- Global pool sized for expected concurrent streams
- Monitor and tune based on production metrics

### 9.4 Latency Optimization

**Time to First Byte**:
- Send schema message immediately after query starts returning
- Don't wait for first data batch before writing headers
- Stream each batch as soon as available

**Connection Efficiency**:
- Keep-alive for connection reuse
- HTTP/2 multiplexing when available
- Minimize header overhead

### 9.5 Benchmark Targets

Compared to JSON baseline for typical analytical result set (100K rows, 10 columns, mixed types):

| Metric | JSON Baseline | Arrow Target | Improvement |
|--------|---------------|--------------|-------------|
| Serialization time | 100ms | 10ms | 10x |
| Response size | 50MB | 5MB | 10x (with compression) |
| Client parse time | 200ms | ~0ms | ~200x (zero-copy) |
| Total transfer time (100Mbps) | 4.2s | 0.5s | 8x |

---

## 10. Batching Model: Paradigm Shift from JSON Pagination

### 10.1 Why JSON Uses Pagination

JSON responses in flapi are fully buffered — the server constructs the entire response before sending. Clients parse the entire JSON blob into memory. Without pagination, large results exhaust memory on both sides. This necessitates the `?limit=N&offset=M` pattern with multiple round-trips.

### 10.2 Arrow IPC Streaming Model

Arrow IPC fundamentally changes this model:

| Aspect | JSON (Current) | Arrow IPC (Proposed) |
|--------|----------------|----------------------|
| Transfer pattern | Multiple paginated requests | Single streaming request |
| Size control | `?limit=1000&offset=5000` | `batch_size` configuration |
| Client behavior | Requests each page explicitly | Consumes batches as they arrive |
| Round-trips | N requests for N pages | 1 request, N batches streamed |
| Memory model | Full page in memory | Process and release each batch |

RecordBatches flow incrementally — server sends a batch, releases its memory, moves to the next. Clients can process batch-by-batch without holding the full result. The `batch_size` parameter (default 122,880 rows) controls granularity within a single stream.

### 10.3 No Pagination Required for Arrow

For most use cases, Arrow streaming eliminates the need for pagination. The "out-of-core large exports" use case works precisely because neither side buffers the complete result.

**Optional safety limits** (not pagination):

| Parameter | Purpose |
|-----------|---------|
| `?max_rows=N` | Truncate stream after N rows (preview/sampling) |
| `?max_batches=N` | Limit batch count (safety cap) |

These are explicit bounds, not pagination. The client receives the (bounded) full result or explicitly requests a subset.

### 10.4 Memory-Bounded Streaming Requirement

For Arrow streaming to deliver on its promise, the server must operate with bounded memory regardless of result set size. The ideal data flow:

```
┌─────────┐     ┌───────────┐     ┌───────────┐     ┌────────┐
│ DuckDB  │────▶│ Arrow     │────▶│ Crow      │────▶│ TCP    │
│ Fetch() │     │ Serialize │     │ Write()   │     │ Socket │
└─────────┘     └───────────┘     └───────────┘     └────────┘
     │               │                  │
     ▼               ▼                  ▼
  ~120K rows      ~few MB           flush to
  then release   then release       network
```

Per-batch lifecycle: fetch → serialize → write → flush → **release memory** → next batch

Memory footprint remains bounded at approximately: `1 batch serialization buffer + HTTP write buffer`

### 10.5 Crow Streaming Behavior: Validation Required

Crow provides streaming primitives:
- `stream_threshold()` — responses above threshold use chunked encoding
- `response.write()` — incremental body writes  
- `response.end()` — finalize response

**Critical assumption**: `write()` flushes to the socket incrementally rather than accumulating in an internal buffer until `end()`.

**Risk**: If Crow buffers all `write()` calls internally, the memory advantage disappears — we're back to full materialization.

**Factors affecting actual behavior**:
- Crow's internal buffer sizing and flush policy
- TCP socket readiness (backpressure from slow clients)
- Intermediate proxy buffering (Nginx, CDN)

**Phase 1 validation checkpoint** (added to success criteria):
1. Test with multi-GB result set on memory-constrained server (e.g., 512MB limit)
2. Monitor flapi process RSS memory throughout streaming
3. Verify batches leave the process incrementally (tcpdump/Wireshark)
4. Test with slow client to verify backpressure handling

**Contingency if Crow buffers**: Direct socket writes via Boost.Asio (Crow's underlying I/O layer) would bypass Crow's response abstraction. This is invasive and deferred unless validation fails.

---

## 11. Configuration Schema

### 11.1 Global Configuration

```yaml
# flapi.yaml - global Arrow configuration
arrow:
  enabled: true                          # Master switch for Arrow support
  
  defaults:
    batch_size: 122880                   # Rows per Arrow RecordBatch
    compression: zstd                    # none | lz4 | zstd
    compression_level: 1                 # 1-22 for zstd, ignored for lz4
  
  limits:
    max_memory_per_request: 268435456    # 256 MB per request
    max_memory_global: 2147483648        # 2 GB total for Arrow
    max_concurrent_streams: 10           # Concurrent Arrow responses
    max_batches_per_stream: 10000        # Prevent infinite streams
    max_rows_per_stream: 10000000        # Soft limit with warning
    stream_timeout_seconds: 600          # 10 minute maximum
  
  fallback:
    on_unsupported_type: error           # error | omit_column | json
    on_memory_exhaustion: error          # error | json
  
  security:
    max_schema_depth: 64                 # Nested type limit
    allow_extension_types: false         # Disable for security
```

### 11.2 Endpoint Configuration

```yaml
# Endpoint-level overrides in sqls/customers.yaml
url-path: /customers/

response:
  formats:
    - json                               # Always available
    - arrow                              # Enable Arrow for this endpoint
  
  default_format: json                   # Default when Accept: */*
  
  arrow:
    enabled: true                        # Can disable per-endpoint
    batch_size: 8192                     # Override for this endpoint
    compression: lz4                     # Different compression preference

template-source: customers.sql
connection: [customers-parquet]
```

### 11.3 Request-Level Parameters

Clients can influence Arrow behavior via query parameters:

| Parameter | Values | Description |
|-----------|--------|-------------|
| `format` | `json`, `arrow` | Override Accept header |
| `arrow_batch_size` | Integer | Rows per batch (within limits) |
| `arrow_compression` | `none`, `lz4`, `zstd` | Compression override |

---

## 12. Observability

### 12.1 Metrics

**Counter Metrics**:
- `flapi_arrow_requests_total` (labels: endpoint, status)
- `flapi_arrow_batches_total` (labels: endpoint)
- `flapi_arrow_bytes_total` (labels: endpoint, compressed)
- `flapi_arrow_errors_total` (labels: endpoint, error_type)

**Gauge Metrics**:
- `flapi_arrow_active_streams` (labels: endpoint)
- `flapi_arrow_memory_pool_bytes` (labels: pool_type)

**Histogram Metrics**:
- `flapi_arrow_stream_duration_seconds` (labels: endpoint)
- `flapi_arrow_batch_size_rows` (labels: endpoint)
- `flapi_arrow_response_size_bytes` (labels: endpoint)

### 12.2 Logging

**Request Lifecycle Logging**:
- Arrow format selected: INFO level with endpoint, client, batch_size
- Stream started: DEBUG level with schema summary
- Stream completed: INFO level with row_count, batch_count, duration, bytes
- Stream aborted: WARN level with reason, partial counts

**Error Logging**:
- Unsupported type encountered: WARN with type details
- Resource limit reached: WARN with limit name and request context
- Serialization failure: ERROR with stack context

### 12.3 Health Checks

Extend existing health endpoint:

```json
{
  "status": "healthy",
  "components": {
    "arrow": {
      "enabled": true,
      "active_streams": 3,
      "memory_pool_used_bytes": 134217728,
      "memory_pool_max_bytes": 2147483648
    }
  }
}
```

---

## 13. Testing Strategy

### 13.1 Unit Testing

**Content Negotiation**:
- Accept header parsing with various formats
- Quality value ordering
- Codec parameter extraction
- Edge cases: malformed headers, missing headers

**Configuration Validation**:
- Valid configurations accepted
- Invalid configurations rejected with clear errors
- Default value application

**Memory Pool**:
- Allocation within limits succeeds
- Allocation exceeding limits fails gracefully
- Concurrent allocation tracking accuracy

### 13.2 Integration Testing

**End-to-End Arrow Response**:
- Request with Arrow Accept header returns Arrow content type
- Response is valid Arrow IPC stream (validate with pyarrow)
- Data matches equivalent JSON response

**Error Handling**:
- Unsupported type produces configured response
- Memory exhaustion produces configured response
- Client disconnect handled cleanly

**Performance Validation**:
- Response time within acceptable bounds
- Memory usage within configured limits
- Concurrent streams within limits

### 13.3 Compatibility Testing

**Client Library Validation**:
- PyArrow: `pyarrow.ipc.open_stream()` succeeds
- Polars: `pl.read_ipc_stream()` succeeds
- Arrow.js: Browser consumption succeeds
- R arrow: `read_ipc_stream()` succeeds

**Compression Compatibility**:
- Each codec variant readable by all target clients
- Codec negotiation produces compatible streams

### 13.4 Load Testing

**Throughput Testing**:
- Maximum requests per second with Arrow responses
- Comparison with JSON baseline
- Identification of bottlenecks

**Concurrency Testing**:
- Behavior at concurrent stream limit
- Memory pool behavior under load
- Graceful degradation verification

**Longevity Testing**:
- Extended operation under sustained load
- Memory leak detection
- Resource cleanup verification

---
## 15. Risk Assessment

### 15.1 Technical Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| nanoarrow integration complexity | Medium | Medium | Early prototyping, fallback to full Arrow C++ |
| DuckDB Arrow interface changes | Low | High | Pin DuckDB version, monitor changelogs |
| Crow buffers responses internally | Medium | High | Phase 1 validation (Section 10.5), contingency: direct Boost.Asio |
| Performance below expectations | Low | Medium | Early benchmarking, iterative optimization |

### 15.2 Operational Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Memory exhaustion in production | Medium | High | Conservative defaults, monitoring, alerts |
| Incompatibility with client versions | Medium | Medium | Compatibility testing matrix, documentation |
| Increased support burden | Medium | Low | Clear documentation, error messages |

### 15.3 Security Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Arrow parsing vulnerabilities | Low | Critical | Disable extension types, validate inputs |
| Resource exhaustion attacks | Medium | Medium | Strict limits, rate limiting |
| Data exposure via error messages | Low | Medium | Sanitize error responses |

---

## 16. Open Decisions

### 16.1 Requiring Resolution Before Implementation

**OD-1: Dependency Choice for IPC Serialization**

Options:
- A) nanoarrow (minimal dependency, sufficient functionality)
- B) Arrow C++ library (full features, larger footprint)
- C) Custom IPC writer (maximum control, maintenance burden)

Recommendation: nanoarrow (Option A) based on sufficient functionality and minimal footprint.

**OD-2: Default Fallback Behavior**

Options:
- A) Return error when Arrow requested but unavailable
- B) Silently fall back to JSON
- C) Fall back to JSON with warning header

Recommendation: Option C for graceful degradation with visibility.

**OD-3: Batch Size Default**

Options:
- A) 8,192 rows (network-optimized)
- B) 65,536 rows (balanced)
- C) 122,880 rows (DuckDB-aligned)

Recommendation: Option C for optimal DuckDB integration efficiency.

### 16.2 Deferred Decisions

**DD-1: HTTP/2 Server Push for Schema**
Consider proactively pushing schema to clients. Defer to Phase 4.

**DD-2: Arrow Flight Support**
Evaluate adding gRPC-based Arrow Flight protocol. Defer based on user demand.

**DD-3: Response Caching for Arrow**
Evaluate caching Arrow responses in DuckLake. Defer to Phase 4.

---

## Appendix A: Glossary

| Term | Definition |
|------|------------|
| Arrow C Data Interface | ABI-stable C interface for zero-copy Arrow data exchange |
| Arrow IPC | Arrow Interprocess Communication format for serializing Arrow data |
| Chunked Transfer Encoding | HTTP/1.1 mechanism for streaming response bodies |
| DataChunk | DuckDB's internal batch representation |
| nanoarrow | Minimal C library for Arrow format operations |
| RecordBatch | Arrow's unit of columnar data transfer |

## Appendix B: Reference Documents

- Apache Arrow IPC Format Specification
- DuckDB Arrow Integration Documentation
- Crow HTTP Server Documentation
- GitHub Issue #9: Feature Request Discussion
- Arrow Security Advisories (CVE-2023-47248, CVE-2024-52338)

---

