/**
 * Types for endpoint testing functionality
 */

export interface EndpointTestState {
  slug: string; // e.g., "/customers/" or "/publicis"
  baseUrl: string; // e.g., "http://localhost:8080"
  method: string; // GET, POST, PUT, DELETE
  lastUsed: Date;
  parameters: Record<string, string>;
  headers: Record<string, string>;
  body?: string; // For POST/PUT requests
  history: RequestHistory[];
}

export interface RequestHistory {
  timestamp: Date;
  parameters: Record<string, string>;
  headers: Record<string, string>;
  body?: string;
  response: ResponseInfo;
}

export interface ResponseInfo {
  status: number;
  statusText: string;
  time: number; // milliseconds
  size: number; // bytes
  headers: Record<string, string>;
  body: string;
  contentType?: string;
  cookies?: Array<{ name: string; value: string; attributes?: string }>;
  // Write operation specific fields
  rows_affected?: number;
  returned_data?: any[];
  last_insert_id?: number;
  isWriteOperation?: boolean;
}

export interface RequestFieldDefinition {
  fieldName: string;
  fieldIn: string; // "query", "path", "header", "body"
  description?: string;
  required: boolean;
  defaultValue?: string;
  type?: string;
}

export interface OperationConfig {
  type?: 'read' | 'write';
  validate_before_write?: boolean;
  returns_data?: boolean;
  transaction?: boolean;
}

export interface EndpointConfig {
  urlPath: string;
  method: string;
  requestFields: RequestFieldDefinition[];
  withPagination: boolean;
  operation?: OperationConfig;
  auth?: {
    enabled: boolean;
    type: string;
    users?: Array<{
      username: string;
      password: string;
      roles?: string[];
    }>;
    from_aws_secretmanager?: {
      secret_name: string;
      region?: string;
    };
  };
  templateSource: string;
}

export interface AuthConfig {
  type: 'basic' | 'bearer' | 'apikey' | 'none';
  username?: string;
  password?: string;
  token?: string;
  apiKey?: string;
  headerName?: string;
}

export interface TestEndpointMessage {
  command: 'test' | 'updateParams' | 'updateHeaders' | 'loadHistory' | 'clearHistory';
  data?: any;
}

export interface TestEndpointResponse {
  success: boolean;
  response?: ResponseInfo;
  error?: string;
}

