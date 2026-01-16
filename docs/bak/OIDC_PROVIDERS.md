# OIDC Provider Setup Guides

Provider-specific setup instructions for integrating your flAPI server with various OIDC providers.

## Table of Contents

- [Google Workspace](#google-workspace)
- [Microsoft Azure AD](#microsoft-azure-ad)
- [Keycloak](#keycloak)
- [Auth0](#auth0)
- [Okta](#okta)
- [GitHub OAuth](#github-oauth)
- [Generic OIDC](#generic-oidc)

---

## Google Workspace

### Provider Info

- **Type**: OIDC-compliant
- **Issuer**: https://accounts.google.com
- **JWKS**: https://www.googleapis.com/oauth2/v3/certs
- **Suitable for**: B2B SaaS with Google Workspace customers, consumer apps

### Setup Steps

#### 1. Create OAuth 2.0 Application

1. Open [Google Cloud Console](https://console.cloud.google.com)
2. Create new project or select existing
3. Enable Google+ API:
   - Search "Google+ API"
   - Click "Enable"
4. Go to Credentials (left sidebar)
5. Click "Create Credentials" → "OAuth 2.0 Client ID"
6. Select "Web application"
7. Authorized redirect URIs: (not needed for server-to-server)
   - `http://localhost:8080` (development)
   - `https://your-domain.com` (production)
8. Copy **Client ID** and **Client Secret**

#### 2. Configure flAPI

```yaml
# flapi.yaml
auth:
  type: oidc
  oidc:
    provider-type: google
    client-id: YOUR_CLIENT_ID.apps.googleusercontent.com
    client-secret: ${GOOGLE_CLIENT_SECRET}
    allowed-audiences:
      - YOUR_CLIENT_ID.apps.googleusercontent.com
    verify-expiration: true
    clock-skew-seconds: 300
```

#### 3. Environment Setup

```bash
# .env
export GOOGLE_CLIENT_SECRET="your-secret-key"
```

#### 4. Test Endpoint

```bash
# 1. Get token from Google (manual test)
# Visit in browser (opens login flow):
# https://accounts.google.com/o/oauth2/auth?
#   client_id=YOUR_CLIENT_ID&
#   response_type=code&
#   scope=openid%20email%20profile&
#   redirect_uri=http://localhost:8080/callback

# 2. Exchange code for token (using Google API docs)
# Then test endpoint:
curl -H "Authorization: Bearer <token>" \
  http://localhost:8080/customers
```

#### 5. Token Claims

Google tokens include:

```json
{
  "sub": "user@example.com",
  "iss": "https://accounts.google.com",
  "aud": "YOUR_CLIENT_ID.apps.googleusercontent.com",
  "email": "user@example.com",
  "email_verified": true,
  "name": "User Name",
  "picture": "https://...",
  "iat": 1234567890,
  "exp": 1234571490
}
```

### Advanced: Custom Claims

Google doesn't support custom claims in standard OIDC. For role-based access:

1. Use [Firebase Custom Claims](https://firebase.google.com/docs/auth/admin/custom-claims)
2. Add role via token endpoint
3. Access in flAPI: Configure `roles-claim: roles` if present

### Rate Limits

Google OAuth has generous rate limits for most applications.

---

## Microsoft Azure AD

### Provider Info

- **Type**: OIDC-compliant
- **Issuer**: https://login.microsoftonline.com/{tenant}/v2.0
- **JWKS**: Provided in discovery document
- **Suitable for**: Enterprise (Microsoft 365 users), B2B

### Setup Steps

#### 1. Create Application

1. Open [Azure Portal](https://portal.azure.com)
2. Search "App registrations" → New registration
3. Enter application name
4. Supported account types: "Accounts in any organizational directory" or "Any Azure AD directory + Microsoft account"
5. Redirect URI (if needed): `http://localhost/callback`
6. Click "Register"

#### 2. Obtain Credentials

1. Click "Certificates & secrets"
2. "New client secret"
3. Copy **Client ID** (from Overview tab)
4. Copy **Client Secret** value

#### 3. Get Tenant ID

From Azure AD properties:
- Tenant ID format: `12345678-1234-1234-1234-123456789012`

#### 4. Configure flAPI

```yaml
# flapi.yaml
auth:
  type: oidc
  oidc:
    provider-type: microsoft
    issuer-url: https://login.microsoftonline.com/TENANT_ID/v2.0
    client-id: APPLICATION_CLIENT_ID
    client-secret: ${AZURE_CLIENT_SECRET}
    allowed-audiences:
      - api://APPLICATION_CLIENT_ID  # Format for Azure AD
    username-claim: preferred_username
    roles-claim: roles
    verify-expiration: true
    clock-skew-seconds: 300
```

Replace:
- `TENANT_ID`: Your Azure AD tenant ID
- `APPLICATION_CLIENT_ID`: Application (client) ID

#### 5. Environment Setup

```bash
export AZURE_CLIENT_SECRET="your-secret-value"
```

#### 6. Configure App Roles (Optional)

1. In App registration, go to "Manifest"
2. Add app role definition:

```json
{
  "appRoles": [
    {
      "id": "admin-role-id",
      "displayName": "Admin",
      "value": "admin",
      "description": "Administrator role"
    },
    {
      "id": "user-role-id",
      "displayName": "User",
      "value": "user",
      "description": "Regular user role"
    }
  ]
}
```

3. Assign users/groups to roles in Enterprise Applications

#### 7. Token Claims

Azure AD tokens include:

```json
{
  "sub": "user-object-id",
  "iss": "https://login.microsoftonline.com/tenant-id/v2.0",
  "aud": "api://application-id",
  "email": "user@company.com",
  "preferred_username": "user@company.com",
  "name": "User Name",
  "roles": ["admin", "user"],
  "iat": 1234567890,
  "exp": 1234571490
}
```

### Common Issues

**Issue**: Token has `aud` claim as client ID instead of expected audience

**Solution**:
```yaml
auth:
  oidc:
    allowed-audiences:
      - application-client-id  # Add this too
      - api://application-client-id
```

---

## Keycloak

### Provider Info

- **Type**: OIDC-compliant (open source)
- **Issuer**: https://keycloak.example.com/realms/{realm}
- **Deployment**: Self-hosted (Docker, VM, etc.)
- **Suitable for**: Enterprise, open-source projects

### Setup Steps

#### 1. Start Keycloak

```bash
# Docker
docker run -p 8080:8080 \
  -e KEYCLOAK_ADMIN=admin \
  -e KEYCLOAK_ADMIN_PASSWORD=admin \
  quay.io/keycloak/keycloak:latest \
  start-dev

# Access: http://localhost:8080/admin
```

#### 2. Create Realm (Namespace)

1. Log in as admin
2. Click "Keycloak" logo → "Create realm"
3. Realm name: `flapi` (or your choice)
4. Enable realm
5. Click "Create"

#### 3. Create Client

1. Go to Clients (left sidebar)
2. "Create client"
3. Client ID: `flapi`
4. Client authentication: ON (for server apps)
5. Authorization: ON (optional)
6. Save

#### 4. Configure Client

1. Under "flapi" client, go to "Settings" tab
2. Access type: confidential
3. Valid redirect URIs: `http://localhost` (for development)
4. Scopes: openid, profile, email
5. Click "Save"

#### 5. Get Credentials

1. Go to "Credentials" tab
2. Copy **Client secret**

#### 6. Configure flAPI

```yaml
# flapi.yaml
auth:
  type: oidc
  oidc:
    provider-type: keycloak
    issuer-url: http://keycloak.example.com/realms/flapi
    client-id: flapi
    client-secret: ${KEYCLOAK_CLIENT_SECRET}
    allowed-audiences:
      - account
      - flapi
    username-claim: preferred_username
    role-claim-path: realm_access.roles  # Keycloak's nested roles
    roles-claim: roles  # Fallback if role-claim-path not found
    groups-claim: groups
    verify-expiration: true
    clock-skew-seconds: 300
```

#### 7. Environment Setup

```bash
export KEYCLOAK_CLIENT_SECRET="secret-from-credentials"
```

#### 8. Create Test User

1. Left sidebar → Users
2. "Create new user"
3. Username: `testuser`
4. First name, Last name, Email: fill in
5. Save
6. "Credentials" tab → "Set password"
7. Type password, temporary: OFF
8. Save

#### 9. Assign Roles

1. "Role mappings" tab
2. "Assign role"
3. Select roles (e.g., "admin", "user")
4. Click "Assign"

#### 10. Get Token (Test)

```bash
# Use password grant (test only, not for production UI)
curl http://keycloak.example.com/realms/flapi/protocol/openid-connect/token \
  -X POST \
  -d "client_id=flapi&client_secret=SECRET&username=testuser&password=PASSWORD&grant_type=password&scope=openid%20profile%20email" \
  | jq '.access_token'

# Test endpoint
curl -H "Authorization: Bearer <token>" \
  http://localhost:8080/customers
```

#### 11. Token Claims

Keycloak tokens include:

```json
{
  "sub": "user-id-uuid",
  "iss": "http://keycloak.example.com/realms/flapi",
  "aud": "account",
  "email": "user@example.com",
  "email_verified": false,
  "preferred_username": "testuser",
  "name": "Test User",
  "realm_access": {
    "roles": ["admin", "user", "offline_access"]
  },
  "client_access": {
    "account": {
      "roles": ["manage-account"]
    }
  },
  "iat": 1234567890,
  "exp": 1234571490
}
```

### Advanced: Client Credentials Flow

For service-to-service auth:

```bash
# Get token as service account
curl http://keycloak.example.com/realms/flapi/protocol/openid-connect/token \
  -X POST \
  -d "client_id=flapi&client_secret=SECRET&grant_type=client_credentials" \
  | jq '.access_token'
```

### Performance Notes

- **JWKS caching**: Default 24 hours (Keycloak keys stable)
- **Discovery caching**: 24 hours
- **For high-traffic**: Use persistent HTTP cache between flAPI and Keycloak

---

## Auth0

### Provider Info

- **Type**: OIDC-compliant (SaaS)
- **Issuer**: https://{domain}.auth0.com
- **JWKS**: Provided in discovery document
- **Suitable for**: SaaS platforms, managed authentication

### Setup Steps

#### 1. Create Auth0 Tenant

1. Sign up at [Auth0](https://auth0.com) (free tier available)
2. Create tenant: `your-company` → Domain will be `your-company.auth0.com`

#### 2. Create Application

1. Go to Applications → Applications
2. "Create application"
3. Name: `flAPI`
4. Application type: "Regular Web Application" or "Machine to Machine"
5. Click "Create"

#### 3. Get Credentials

1. Under "flAPI" application → Settings tab
2. Copy:
   - Client ID
   - Client Secret

#### 4. Configure flAPI

```yaml
# flapi.yaml
auth:
  type: oidc
  oidc:
    provider-type: auth0
    issuer-url: https://your-domain.auth0.com
    client-id: YOUR_CLIENT_ID
    client-secret: ${AUTH0_CLIENT_SECRET}
    allowed-audiences:
      - YOUR_CLIENT_ID  # Auth0 often uses client ID as audience
    username-claim: email
    verify-expiration: true
    clock-skew-seconds: 300
```

#### 5. Environment Setup

```bash
export AUTH0_CLIENT_SECRET="your-secret"
```

#### 6. Create Custom Claims

For roles in Auth0:

1. Go to Actions → Flows → Login
2. "Custom" → Create action
3. Code:

```javascript
exports.onExecutePostLogin = async (event, api) => {
  // Add custom role claim
  const namespace = 'https://your-api.example.com';
  api.idToken.setCustomClaim(`${namespace}/roles`, ['admin', 'user']);
  api.accessToken.setCustomClaim(`${namespace}/roles`, ['admin', 'user']);
};
```

4. Add to Login flow

#### 7. Configure Custom Claim in flAPI

```yaml
auth:
  oidc:
    role-claim-path: https://your-api.example.com/roles
```

#### 8. Get Token (Test)

```bash
# Using Auth0 test endpoint
curl https://your-domain.auth0.com/oauth/token \
  -X POST \
  -H "Content-Type: application/json" \
  -d '{
    "client_id": "CLIENT_ID",
    "client_secret": "CLIENT_SECRET",
    "audience": "CLIENT_ID",
    "grant_type": "client_credentials"
  }' | jq '.access_token'
```

#### 9. Token Claims

Auth0 tokens include:

```json
{
  "sub": "google-oauth2|google-id",
  "iss": "https://your-domain.auth0.com/",
  "aud": "CLIENT_ID",
  "email": "user@example.com",
  "email_verified": true,
  "name": "User Name",
  "https://your-api.example.com/roles": ["admin"],
  "iat": 1234567890,
  "exp": 1234571490
}
```

### Auth0 Pricing

- Free tier: 7,000 log-ins/month
- Pro tier: Usage-based pricing
- Good for evaluating OIDC integration

---

## Okta

### Provider Info

- **Type**: OIDC-compliant (SaaS)
- **Issuer**: https://{domain}.okta.com/oauth2/{auth-server-id}
- **Deployment**: Cloud SaaS
- **Suitable for**: Enterprise identity management

### Setup Steps

#### 1. Create Okta Organization

1. Sign up at [Okta](https://developer.okta.com/signup/) (free developer account)
2. Verify email and set up account
3. Okta organization URL: `https://dev-XXXXXXX.okta.com`

#### 2. Create Application

1. Go to Applications → Applications
2. "Create App Integration"
3. Sign-in method: "OIDC"
4. Application type: "Web Application"
5. App name: `flAPI`
6. Grant type: "Client Credentials" (for server)
7. Click "Save"

#### 3. Get Credentials

1. Under "flAPI" app → General tab
2. Copy:
   - Client ID
   - Client Secret

#### 4. Configure flAPI

```yaml
# flapi.yaml
auth:
  type: oidc
  oidc:
    provider-type: okta
    issuer-url: https://dev-XXXXXXX.okta.com/oauth2/default
    client-id: YOUR_CLIENT_ID
    client-secret: ${OKTA_CLIENT_SECRET}
    allowed-audiences:
      - api://default
    username-claim: preferred_username
    verify-expiration: true
    clock-skew-seconds: 300
```

#### 5. Environment Setup

```bash
export OKTA_CLIENT_SECRET="your-secret"
```

#### 6. Assign User to App

1. Go to Assignments tab
2. "Assign" → "Assign to People"
3. Select users/groups
4. Click "Assign"

#### 7. Get Token (Test)

```bash
# Using authorization code flow (requires user login)
# Or client credentials for M2M:
curl https://dev-XXXXXXX.okta.com/oauth2/default/v1/token \
  -X POST \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "client_id=CLIENT_ID&client_secret=CLIENT_SECRET&scope=okta.users.read&grant_type=client_credentials" \
  | jq '.access_token'
```

#### 8. Token Claims

Okta tokens include:

```json
{
  "sub": "user-id-uuid",
  "iss": "https://dev-XXXXXXX.okta.com/oauth2/default",
  "aud": "api://default",
  "email": "user@example.com",
  "email_verified": true,
  "preferred_username": "user@example.com",
  "name": "User Name",
  "groups": ["Everyone", "Admins"],
  "iat": 1234567890,
  "exp": 1234571490
}
```

---

## GitHub OAuth

### Provider Info

- **Type**: OAuth 2.0 (not full OIDC)
- **Endpoints**: https://api.github.com, https://github.com/login/oauth
- **Suitable for**: Developer tools, integrations with GitHub

### Important Notes

- GitHub does **not** provide standard OIDC
- Uses custom endpoints and claim extraction
- Still supported by flAPI with custom handling

### Setup Steps

#### 1. Create OAuth App

1. Go to GitHub Settings → Developer settings → OAuth Apps
2. "New OAuth App"
3. Fill in:
   - Application name: `flAPI`
   - Homepage URL: `http://localhost:8080`
   - Authorization callback URL: `http://localhost:8080/callback`
4. Click "Register application"

#### 2. Get Credentials

Copy:
- Client ID
- Client Secret

#### 3. Configure flAPI

```yaml
# flapi.yaml
auth:
  type: oidc
  oidc:
    provider-type: github
    client-id: YOUR_CLIENT_ID
    client-secret: ${GITHUB_CLIENT_SECRET}
    username-claim: login
    email-claim: email
    verify-expiration: false  # GitHub tokens don't always have expiration
```

#### 4. Environment Setup

```bash
export GITHUB_CLIENT_SECRET="your-secret"
```

#### 5. Get Token

For testing (requires user login flow):

```bash
# 1. User logs in and approves app
# 2. GitHub redirects with code
# 3. Exchange code for access token
curl https://github.com/login/oauth/access_token \
  -X POST \
  -H "Accept: application/json" \
  -d "client_id=CLIENT_ID&client_secret=CLIENT_SECRET&code=CODE" \
  | jq '.access_token'

# 4. Get user info
curl https://api.github.com/user \
  -H "Authorization: Bearer <token>" \
  | jq '{login, email, name}'
```

#### 6. Token Info (Different from OIDC)

GitHub tokens are personal access tokens, not JWT:

```bash
curl https://api.github.com/user \
  -H "Authorization: Bearer <token>"

# Returns:
{
  "login": "octocat",
  "id": 1,
  "email": "octocat@github.com",
  "name": "The Octocat",
  "company": "@github",
  "blog": "https://github.blog",
  "location": "San Francisco"
}
```

### Advanced: GitHub Teams/Orgs

Get user's organization/team membership:

```bash
# Get user's organizations
curl https://api.github.com/user/orgs \
  -H "Authorization: Bearer <token>" \
  | jq '.[].login'

# Get team memberships
curl https://api.github.com/user/teams \
  -H "Authorization: Bearer <token>" \
  | jq '.[] | {name, slug, organization}'
```

---

## Generic OIDC

### For Any OIDC-Compliant Provider

If your provider isn't listed above, use the generic OIDC configuration:

```yaml
# flapi.yaml
auth:
  type: oidc
  oidc:
    provider-type: generic  # Or omit this field
    issuer-url: https://your-oidc-provider.com
    client-id: your-client-id
    client-secret: ${OIDC_CLIENT_SECRET}
    allowed-audiences:
      - your-audience
    username-claim: preferred_username
    email-claim: email
    roles-claim: roles
    verify-expiration: true
    clock-skew-seconds: 300
```

### Discovery Process

Generic OIDC automatically:

1. Fetches `.well-known/openid-configuration` from issuer
2. Extracts endpoints:
   - `jwks_uri`: For JWKS keys
   - `token_endpoint`: For token exchange
   - `authorization_endpoint`: For OAuth flow
3. Caches metadata for 24 hours

### Verification Steps

Manually verify your provider's OIDC support:

```bash
# Check discovery endpoint
curl https://your-provider.com/.well-known/openid-configuration | jq

# Should contain these fields:
# - issuer
# - jwks_uri
# - token_endpoint
# - authorization_endpoint
# - id_token_signing_alg_values_supported (should include RS256, RS384, or RS512)

# Check JWKS
curl $(curl https://your-provider.com/.well-known/openid-configuration | jq -r '.jwks_uri') | jq
```

---

## Provider Comparison

| Feature | Google | Azure AD | Keycloak | Auth0 | Okta | GitHub |
|---------|--------|----------|----------|-------|------|--------|
| OIDC Support | ✅ Full | ✅ Full | ✅ Full | ✅ Full | ✅ Full | ⚠️ Partial |
| Custom Claims | Limited | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes | No |
| Roles/Groups | 🔧 Custom | ✅ Yes | ✅ Yes | 🔧 Custom | ✅ Yes | Teams only |
| Client Credentials | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes | No |
| Token Refresh | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes | No |
| Self-Hosted | No | No | ✅ Yes | No | No | No |
| Pricing | Free tier | Free + enterprise | Open source | Free + usage | Paid | Free |
| Best For | Google users | MS 365 orgs | Enterprise | SaaS | Large enterprises | GitHub devs |

