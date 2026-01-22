# Cloud Storage Guide

This guide explains how to use flAPI with cloud storage backends (S3, GCS, Azure, HTTPS) for configuration files and SQL templates.

## Overview

flAPI supports loading configuration and SQL template files from remote storage via DuckDB's Virtual File System (VFS). This enables:

- **Serverless deployments**: Run flAPI on AWS Lambda, Google Cloud Run, or Azure Functions without volume mounts
- **Centralized configuration**: Store all endpoint definitions in a single S3 bucket or GCS bucket
- **GitOps workflows**: Deploy configuration changes by updating files in cloud storage
- **Multi-environment setups**: Use different storage paths for dev/staging/prod

## Quick Start

### Loading Config from HTTPS

The simplest way to use remote configuration is via HTTPS:

```bash
flapi --config https://raw.githubusercontent.com/myorg/configs/main/flapi.yaml
```

### Loading Config from S3

For S3, set AWS credentials and use an S3 URL:

```bash
export AWS_ACCESS_KEY_ID=your-access-key
export AWS_SECRET_ACCESS_KEY=your-secret-key
export AWS_REGION=us-east-1

flapi --config s3://my-bucket/configs/flapi.yaml
```

### Loading Config from GCS

For Google Cloud Storage:

```bash
export GOOGLE_APPLICATION_CREDENTIALS=/path/to/service-account.json

flapi --config gs://my-bucket/configs/flapi.yaml
```

## Supported Storage Backends

| Backend | URL Scheme | Authentication |
|---------|------------|----------------|
| Amazon S3 | `s3://` | AWS credentials (env vars or IAM role) |
| S3-compatible | `s3://`, `s3a://`, `s3n://` | Same as S3 |
| Google Cloud Storage | `gs://` | Service account or ADC |
| Azure Blob Storage | `az://`, `abfs://` | Storage account key or managed identity |
| HTTPS | `https://` | None (public) or HTTP auth headers |
| HTTP | `http://` | None (not recommended for production) |
| Local filesystem | `file://` or plain path | Filesystem permissions |

## Configuration Examples

### Example 1: HTTPS Templates

Load templates from a web server or GitHub raw URLs:

```yaml
# flapi.yaml
project-name: HTTPS Example
project-description: Templates served via HTTPS

template:
  path: https://raw.githubusercontent.com/myorg/templates/main/sqls/

connections:
  local-data:
    properties:
      path: ./data/customers.parquet
```

### Example 2: S3 Configuration

Full configuration with S3 for both config and templates:

```yaml
# flapi.yaml (can be hosted at s3://my-bucket/configs/flapi.yaml)
project-name: S3 Cloud API
project-description: Fully cloud-native configuration

template:
  path: s3://my-bucket/templates/

connections:
  s3-data:
    init: |
      INSTALL httpfs;
      LOAD httpfs;
      SET s3_region='us-east-1';
    properties:
      bucket: my-data-bucket
      base_path: s3://my-data-bucket/data/

duckdb:
  access_mode: READ_ONLY
```

### Example 3: Mixed Local/Remote

Use local config with remote templates:

```yaml
# flapi.yaml (local)
project-name: Mixed Storage API
project-description: Local config, remote templates

template:
  path: https://templates.example.com/sqls/

connections:
  local-parquet:
    properties:
      path: ./data/customers.parquet
```

### Example 4: Remote Template in Endpoint

Specify remote templates per-endpoint:

```yaml
# sqls/customers.yaml
url-path: /customers
method: GET

# Remote template - full URL overrides template.path
template-source: https://templates.example.com/customers.sql

connection:
  - local-data
```

## Authentication

### Amazon S3

**Option 1: Environment Variables**

```bash
export AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE
export AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY
export AWS_REGION=us-east-1

# Optional: for temporary credentials
export AWS_SESSION_TOKEN=your-session-token
```

**Option 2: IAM Role (EC2/ECS/Lambda)**

When running on AWS infrastructure, flAPI automatically uses the instance's IAM role. No environment variables needed.

**Option 3: AWS Profile**

```bash
export AWS_PROFILE=my-profile
```

### Google Cloud Storage

**Option 1: Service Account Key**

```bash
export GOOGLE_APPLICATION_CREDENTIALS=/path/to/service-account.json
```

**Option 2: Application Default Credentials**

```bash
gcloud auth application-default login
```

**Option 3: Compute Engine/Cloud Run**

Uses the attached service account automatically.

### Azure Blob Storage

**Option 1: Storage Account Key**

```bash
export AZURE_STORAGE_ACCOUNT=mystorageaccount
export AZURE_STORAGE_KEY=your-storage-key
```

**Option 2: Connection String**

```bash
export AZURE_STORAGE_CONNECTION_STRING="DefaultEndpointsProtocol=https;..."
```

**Option 3: Managed Identity**

When running on Azure infrastructure, uses the managed identity automatically.

## Serverless Deployment

### AWS Lambda

```dockerfile
FROM public.ecr.aws/lambda/provided:al2

COPY flapi /var/task/
COPY bootstrap /var/task/

# Config loaded from S3 at runtime
ENV FLAPI_CONFIG=s3://my-bucket/configs/flapi.yaml
ENV AWS_REGION=us-east-1

CMD ["flapi"]
```

```bash
# bootstrap script
#!/bin/sh
exec /var/task/flapi --config "$FLAPI_CONFIG"
```

### Google Cloud Run

```dockerfile
FROM gcr.io/distroless/cc

COPY flapi /flapi

# Config loaded from GCS at runtime
ENV FLAPI_CONFIG=gs://my-bucket/configs/flapi.yaml

ENTRYPOINT ["/flapi", "--config", "${FLAPI_CONFIG}"]
```

### Azure Container Apps

```yaml
# container-app.yaml
properties:
  configuration:
    secrets:
      - name: azure-storage-key
        value: your-storage-key
  template:
    containers:
      - name: flapi
        image: myregistry.azurecr.io/flapi:latest
        env:
          - name: FLAPI_CONFIG
            value: az://my-container/configs/flapi.yaml
          - name: AZURE_STORAGE_ACCOUNT
            value: mystorageaccount
          - name: AZURE_STORAGE_KEY
            secretRef: azure-storage-key
```

## Security Best Practices

### 1. Use HTTPS, Not HTTP

Always use `https://` for remote configuration in production. HTTP URLs are supported but transmit configuration (potentially containing secrets) in plain text.

### 2. Restrict Bucket Permissions

Use least-privilege IAM policies:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": ["s3:GetObject"],
      "Resource": ["arn:aws:s3:::my-bucket/configs/*", "arn:aws:s3:::my-bucket/templates/*"]
    }
  ]
}
```

### 3. Avoid Secrets in Configuration Files

Use environment variables for sensitive values:

```yaml
# Good - secrets from environment
auth:
  jwt-secret: '${JWT_SECRET}'

connections:
  postgres:
    properties:
      password: '${DB_PASSWORD}'
```

### 4. Enable Server-Side Encryption

For S3, enable SSE:

```bash
aws s3 cp flapi.yaml s3://my-bucket/configs/ --sse AES256
```

### 5. Use VPC Endpoints for Private Access

For AWS, configure VPC endpoints to access S3 without public internet:

```bash
aws ec2 create-vpc-endpoint \
  --vpc-id vpc-xxx \
  --service-name com.amazonaws.us-east-1.s3 \
  --route-table-ids rtb-xxx
```

## Troubleshooting

### "Access Denied" Errors

1. Verify credentials are set correctly:
   ```bash
   echo $AWS_ACCESS_KEY_ID
   echo $AWS_REGION
   ```

2. Test access with AWS CLI:
   ```bash
   aws s3 ls s3://my-bucket/configs/
   ```

3. Check IAM permissions allow `s3:GetObject`

### "File Not Found" Errors

1. Verify the full URL is correct:
   ```bash
   curl -I https://example.com/templates/customers.sql
   ```

2. Check for typos in bucket names or paths

3. Ensure the file exists in the remote location

### Slow Startup Times

Remote configuration loading adds network latency to startup. To minimize impact:

1. Use regional endpoints (same region as your compute)
2. Keep configuration files small
3. Consider caching (future feature)

### Template Path Resolution

When using remote `template.path`, relative paths in `template-source` are resolved against it:

```yaml
# flapi.yaml
template:
  path: s3://my-bucket/templates/

# sqls/customers.yaml
template-source: customers.sql
# Resolves to: s3://my-bucket/templates/customers.sql
```

To use a different location, specify a full URL:

```yaml
template-source: https://other-server.com/templates/customers.sql
```

## Related Documentation

- [CONFIG_REFERENCE.md](./CONFIG_REFERENCE.md) - Full configuration reference
- [CLI_REFERENCE.md](./CLI_REFERENCE.md) - Command-line options
- [features/flapi-10-fs-abstraction.md](./features/flapi-10-fs-abstraction.md) - Technical design document
