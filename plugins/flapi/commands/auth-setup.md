---
description: Secure flAPI endpoints and MCP tools - Basic, JWT/bearer, OIDC (Google, Azure AD, Keycloak, ...), roles, MCP OAuth - and verify it
argument-hint: "[auth method or identity provider] [which endpoints]"
---

Set up authentication and authorization for flAPI. Request: $ARGUMENTS

Use the `flapi-auth` skill.

1. Find out: who calls the API (people via an IdP, services with a shared secret,
   AI agents over MCP), the identity provider if any, and which endpoints need
   which roles. Read the current `flapi.yaml` and endpoint YAMLs.
2. Recommend one scheme in a sentence and why: OIDC for an existing IdP, bearer JWT
   for service-to-service, Basic only for internal tools. For MCP clients that
   support it, configure OAuth protected-resource metadata.
3. Write the config. Secrets (JWT secret, client secret, passwords) come from
   whitelisted environment variables, never literals.
4. Validate (`flapi --validate-config`; read the startup security warnings), start
   the server and prove it with curl:
   - no credentials -> 401
   - valid credentials, wrong role -> 403
   - valid credentials and role -> 200
   and for MCP the same through `/mcp/jsonrpc`.
5. Check rejections are visible: `flapii metrics` shows the 401/403/429 counters.
6. Report what is protected, how to obtain a token, and what to rotate where.
