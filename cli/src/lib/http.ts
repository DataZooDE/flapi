// One HTTP client for the CLI and the VS Code extension. This was a drifting copy
// of shared/src/lib/http.ts; never re-implement it here.
export { createApiClient, AxiosResponseError, isAxiosError } from '@flapi/shared';
