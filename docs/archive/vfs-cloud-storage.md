# Cloud Storage Support

flAPI supports reading configuration files and SQL templates from cloud storage services via DuckDB's virtual file system (VFS). This enables storing your API definitions in S3, Google Cloud Storage, or Azure Blob Storage.

## Supported Cloud Providers

| Provider | URL Schemes | Environment Variables |
|----------|-------------|----------------------|
| **AWS S3** | `s3://` | `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_REGION` |
| **Google Cloud Storage** | `gs://` | `GOOGLE_APPLICATION_CREDENTIALS`, `GOOGLE_CLOUD_PROJECT` |
| **Azure Blob Storage** | `az://`, `azure://` | `AZURE_STORAGE_CONNECTION_STRING` or `AZURE_STORAGE_ACCOUNT` + `AZURE_STORAGE_KEY` |

## Quick Start

### S3 Configuration

```bash
# Set AWS credentials
export AWS_ACCESS_KEY_ID="your-access-key"
export AWS_SECRET_ACCESS_KEY="your-secret-key"
export AWS_REGION="us-east-1"

# Start flAPI with S3-hosted config
./flapi -c s3://my-bucket/path/to/flapi.yaml
```

### GCS Configuration

```bash
# Set GCP credentials
export GOOGLE_APPLICATION_CREDENTIALS="/path/to/service-account.json"
export GOOGLE_CLOUD_PROJECT="my-project"

# Start flAPI with GCS-hosted config
./flapi -c gs://my-bucket/path/to/flapi.yaml
```

### Azure Configuration

```bash
# Option 1: Connection string
export AZURE_STORAGE_CONNECTION_STRING="DefaultEndpointsProtocol=https;AccountName=..."

# Option 2: Account name + key
export AZURE_STORAGE_ACCOUNT="mystorageaccount"
export AZURE_STORAGE_KEY="base64key=="

# Start flAPI with Azure-hosted config
./flapi -c az://my-container/path/to/flapi.yaml
```

## Configuration Options

### Storage Section in flapi.yaml

```yaml
storage:
  # Enable caching for remote files (default: true)
  cache:
    enabled: true
    ttl: 300           # Cache TTL in seconds (default: 300)
    max_size: 50MB     # Maximum cache size (default: 50MB)

  # Credential configuration (optional - uses environment by default)
  credentials:
    s3:
      type: environment          # environment, secret, instance_profile
      region: us-east-1          # Override region
    gcs:
      type: environment          # environment, service_account
      key_file: '/secrets/gcs.json'  # Optional explicit key file
    azure:
      type: environment          # environment, connection_string, managed_identity
      account: mystorageaccount  # Optional explicit account
```

## File Caching

Remote files are automatically cached to improve performance and reduce cloud storage costs.

### Cache Behavior

- **Local files**: Never cached (always read fresh from disk)
- **Remote files**: Cached with configurable TTL
- **Cache eviction**: LRU (Least Recently Used) when max size exceeded
- **Thread safety**: Concurrent access is fully supported

### Cache Configuration

```yaml
storage:
  cache:
    enabled: true       # Enable/disable caching
    ttl: 300            # Time-to-live in seconds
    max_size: 50MB      # Maximum cache size
```

### Cache Invalidation

Caches are automatically invalidated when:
- TTL expires
- Server restarts
- Manual cache clear via health endpoint

## Health Checks

The health endpoint (`GET /api/v1/_config/health`) includes storage backend status:

```json
{
  "status": "healthy",
  "storage": {
    "status": "healthy",
    "backends": [
      {
        "name": "config",
        "path": "s3://my-bucket/config/flapi.yaml",
        "accessible": true,
        "latency_ms": 45,
        "scheme": "s3"
      },
      {
        "name": "templates",
        "path": "./sqls/",
        "accessible": true,
        "latency_ms": 2,
        "scheme": "local"
      }
    ],
    "total_latency_ms": 47
  },
  "credentials": {
    "s3_configured": true,
    "gcs_configured": false,
    "azure_configured": false
  }
}
```

## Credential Types

### S3 Credentials

| Type | Description | Environment Variables |
|------|-------------|----------------------|
| `environment` | Load from environment | `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_REGION` |
| `secret` | Use DuckDB secrets | Configured via DuckDB |
| `instance_profile` | AWS IAM role | Automatic on EC2/ECS/Lambda |

**Session tokens** for temporary credentials:
```bash
export AWS_SESSION_TOKEN="FwoGZXIvYXdzE..."
```

**Custom endpoints** for S3-compatible storage (MinIO, LocalStack):
```bash
export AWS_ENDPOINT_URL="http://localhost:9000"
```

### GCS Credentials

| Type | Description | Environment Variables |
|------|-------------|----------------------|
| `environment` | Load from environment | `GOOGLE_APPLICATION_CREDENTIALS` |
| `service_account` | Explicit key file | Path configured in yaml |

**Project ID** (optional, some operations require it):
```bash
export GOOGLE_CLOUD_PROJECT="my-project"
# Alternative:
export GCLOUD_PROJECT="my-project"
```

### Azure Credentials

| Type | Description | Environment Variables |
|------|-------------|----------------------|
| `environment` | Account name + key | `AZURE_STORAGE_ACCOUNT`, `AZURE_STORAGE_KEY` |
| `connection_string` | Full connection string | `AZURE_STORAGE_CONNECTION_STRING` |
| `managed_identity` | Azure Managed Identity | `AZURE_TENANT_ID`, `AZURE_CLIENT_ID` |

Connection string format:
```
DefaultEndpointsProtocol=https;AccountName=<name>;AccountKey=<key>;EndpointSuffix=core.windows.net
```

## URL Format Reference

### S3 URLs

```
s3://bucket-name/path/to/file.yaml
s3://bucket-name/prefix/
```

### GCS URLs

```
gs://bucket-name/path/to/file.yaml
gs://bucket-name/prefix/
```

### Azure URLs

```
az://container-name/path/to/file.yaml
azure://container-name/path/to/file.yaml
```

## Error Handling

### Common Errors

| Error | Cause | Solution |
|-------|-------|----------|
| `No S3 credentials configured` | Missing AWS credentials | Set `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` |
| `AccessDenied` | Invalid credentials or permissions | Check IAM policies |
| `NoSuchBucket` / `NoSuchKey` | Invalid path | Verify bucket/key exists |
| `Network error` | Connectivity issue | Check network/firewall |

### Startup Verification

flAPI verifies storage accessibility at startup. If storage is unreachable:
- Warning logged
- Server continues if local fallback available
- Server fails if no config accessible

## Best Practices

1. **Use environment variables** for credentials (not config files)
2. **Enable caching** for remote configs to reduce latency
3. **Set appropriate TTL** based on how often configs change
4. **Use IAM roles** on cloud platforms instead of static credentials
5. **Monitor health endpoint** for storage status
6. **Test locally** before deploying remote configs

## Example Configurations

See example files in the `examples/` directory:
- `flapi-s3.yaml` - S3 configuration
- `flapi-gcs.yaml` - GCS configuration
- `flapi-azure.yaml` - Azure Blob Storage configuration
