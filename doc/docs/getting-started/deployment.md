---
sidebar_position: 5
---

# Deployment

Flapi can be deployed in various ways depending on your needs. This guide covers deployment options from managed cloud services to self-hosted solutions.

## Cloud Platforms

### AWS App Runner

AWS App Runner provides a fully managed container service that makes it easy to deploy Flapi without managing infrastructure.

1. Create a new App Runner service in AWS Console
2. Choose "Container Registry" as source and enter:
```
ghcr.io/datazoode/flapi:latest
```
3. Configure the service:
   - Port: 8080
   - CPU: 1 vCPU (recommended minimum)
   - Memory: 2 GB (recommended minimum)
4. Set your environment variables for database connections and other configurations
5. Deploy and AWS will automatically provision HTTPS endpoints and handle scaling

### Google Cloud Run

Cloud Run offers serverless container deployment with automatic scaling to zero.

1. Pull and tag the Flapi image:
```bash
docker pull ghcr.io/datazoode/flapi:latest
docker tag ghcr.io/datazoode/flapi:latest gcr.io/[PROJECT_ID]/flapi
```

2. Push to Google Container Registry:
```bash
docker push gcr.io/[PROJECT_ID]/flapi
```

3. Deploy to Cloud Run:
```bash
gcloud run deploy flapi \
  --image gcr.io/[PROJECT_ID]/flapi \
  --platform managed \
  --port 8080 \
  --region [REGION] \
  --allow-unauthenticated
```

### Azure Container Apps

Azure Container Apps provides a managed Kubernetes-based environment.

1. Create a Container App:
```bash
az containerapp create \
  --name flapi \
  --resource-group myResourceGroup \
  --image ghcr.io/datazoode/flapi:latest \
  --target-port 8080 \
  --ingress external \
  --min-replicas 1 \
  --max-replicas 10
```

2. Configure scaling rules and environment variables through Azure Portal or CLI

## Hosting Providers

### Vultr

Vultr offers a one-click Docker deployment:

1. Create a new server with the "Docker" one-click app
2. SSH into your server
3. Run Flapi:
```bash
docker run -d \
  --name flapi \
  -p 8080:8080 \
  -v $(pwd)/config:/config \
  ghcr.io/datazoode/flapi:latest
```

### STACKIT Kubernetes Engine (SKE)

Deploy on STACKIT's managed Kubernetes service:

1. Create a Kubernetes deployment file `flapi.yaml`:
```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: flapi
spec:
  replicas: 2
  selector:
    matchLabels:
      app: flapi
  template:
    metadata:
      labels:
        app: flapi
    spec:
      containers:
      - name: flapi
        image: ghcr.io/datazoode/flapi:latest
        ports:
        - containerPort: 8080
---
apiVersion: v1
kind: Service
metadata:
  name: flapi
spec:
  type: LoadBalancer
  ports:
  - port: 80
    targetPort: 8080
  selector:
    app: flapi
```

2. Apply the configuration:
```bash
kubectl apply -f flapi.yaml
```

### Hetzner

Using Hetzner's Docker-CE app:

1. Create a new server with Docker-CE app
2. SSH into your server
3. Create a docker-compose.yml:
```yaml
version: '3'
services:
  flapi:
    image: ghcr.io/datazoode/flapi:latest
    ports:
      - "8080:8080"
    volumes:
      - ./config:/config
    restart: unless-stopped
```

4. Start the service:
```bash
docker-compose up -d
```

## Self-Hosted

### Systemd Service

For running Flapi as a systemd service on Linux:

1. Create a systemd service file `/etc/systemd/system/flapi.service`:
```ini
[Unit]
Description=Flapi REST API Service
After=network.target

[Service]
Type=simple
User=flapi
ExecStart=/usr/local/bin/flapi -c /etc/flapi/config.yaml
Restart=always
RestartSec=5
StartLimitInterval=0
WorkingDirectory=/etc/flapi

[Install]
WantedBy=multi-user.target
```

2. Create necessary directories and user:
```bash
sudo useradd -r -s /bin/false flapi
sudo mkdir -p /etc/flapi
sudo chown flapi:flapi /etc/flapi
```

3. Copy your configuration:
```bash
sudo cp config.yaml /etc/flapi/
sudo chown flapi:flapi /etc/flapi/config.yaml
```

4. Enable and start the service:
```bash
sudo systemctl daemon-reload
sudo systemctl enable flapi
sudo systemctl start flapi
```

5. Check service status:
```bash
sudo systemctl status flapi
```

For all deployment methods, remember to:
- Configure your database connections
- Set up proper monitoring
- Configure logging
- Set up health checks
- Implement proper backup strategies

# Features

Key features of Flapi include:

- Quick REST API creation from SQL queries
- Multiple database support
- Built-in authentication
- Caching capabilities
- Parameter validation 