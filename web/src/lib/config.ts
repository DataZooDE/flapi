// In development, use the proxy. In production, use the configured URL
const isDev = import.meta.env.DEV;
export const API_BASE_URL = isDev ? '' : (import.meta.env.VITE_API_BASE_URL || 'http://localhost:8080'); 