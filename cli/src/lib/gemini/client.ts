import { GoogleGenerativeAI, SchemaType } from '@google/generative-ai';
import { GeminiAuthConfig, GeminiGeneratedConfig } from '../types';
import { Console } from '../console';

const MAX_RETRIES = 3;
const RETRY_DELAY_MS = 1000;

export class GeminiClient {
  private client: GoogleGenerativeAI | null = null;
  private authConfig: GeminiAuthConfig;

  constructor(authConfig: GeminiAuthConfig) {
    if (!authConfig.api_key) {
      throw new Error('API key is required for Gemini client');
    }
    this.authConfig = authConfig;
    this.client = new GoogleGenerativeAI(authConfig.api_key);
  }

  async generateEndpointConfig(
    description: string,
    connections: string[],
    tables: string[]
  ): Promise<GeminiGeneratedConfig> {
    const spinner = Console.spinner(
      'Analyzing with Gemini... (this may take a moment)'
    );

    try {
      const model = this.client!.getGenerativeModel({
        model: this.authConfig.model || 'gemini-2.0-flash-exp',
      });

      const systemPrompt = `You are an expert API configuration specialist. Your task is to generate flAPI endpoint configurations based on natural language descriptions.

flAPI transforms SQL queries + YAML configuration into REST APIs. You need to generate structured endpoint configurations that follow flAPI's patterns.

Key principles:
- Endpoint names should be descriptive and use snake_case
- URL paths should follow RESTful conventions (e.g., /customers/{id}/orders)
- Use path parameters for resource identifiers
- Use query parameters for filtering and optional fields
- For POST/PUT endpoints, use body parameters for complex data
- Always include appropriate validators for input validation
- Cache settings should consider query complexity and data volatility`;

      const userPrompt = `Generate an endpoint configuration for the following requirement:

${description}

Available connections: ${connections.join(', ')}
Available tables: ${tables.join(', ')}

IMPORTANT: You MUST respond with ONLY a valid JSON object (no markdown, no code blocks, no explanations outside the JSON).
The JSON must have exactly this structure:
{
  "endpoint_name": "string (descriptive name)",
  "url_path": "string (must start with /)",
  "description": "string",
  "connection": "string (choose from available)",
  "table": "string (choose from available)",
  "method": "GET|POST|PUT|DELETE",
  "parameters": [
    {
      "name": "string",
      "type": "string|integer|number|boolean",
      "required": boolean,
      "location": "query|path|body",
      "description": "string",
      "validators": []
    }
  ],
  "enable_cache": boolean,
  "cache_ttl": number (seconds, optional),
  "reasoning": "string (brief explanation)"
}`;

      for (let attempt = 0; attempt < MAX_RETRIES; attempt++) {
        try {
          const response = await model.generateContent({
            contents: [
              {
                parts: [
                  { text: systemPrompt },
                  { text: userPrompt },
                ],
              },
            ],
          });

          const text = response.response.text();

          // Try to parse the response
          const config = this.parseResponse(text);

          spinner.succeed('✓ Configuration generated');
          return config;
        } catch (error) {
          if (attempt === MAX_RETRIES - 1) {
            throw error;
          }

          // Exponential backoff for retries
          await this.delay(RETRY_DELAY_MS * Math.pow(2, attempt));
        }
      }

      throw new Error('Failed to generate configuration after retries');
    } catch (error) {
      spinner.fail('✗ Failed to generate with Gemini');

      if (error instanceof Error) {
        if (error.message.includes('API_KEY_INVALID')) {
          throw new Error('Invalid Gemini API key. Please check your credentials.');
        }
        if (error.message.includes('QUOTA_EXCEEDED')) {
          throw new Error(
            'API quota exceeded. Please check your Gemini API usage limits.'
          );
        }
        throw new Error(`Gemini API error: ${error.message}`);
      }

      throw error;
    }
  }

  private parseResponse(text: string): GeminiGeneratedConfig {
    // Remove markdown code blocks if present
    let jsonText = text.trim();
    if (jsonText.startsWith('```json')) {
      jsonText = jsonText.slice(7); // Remove ```json
    } else if (jsonText.startsWith('```')) {
      jsonText = jsonText.slice(3); // Remove ```
    }

    if (jsonText.endsWith('```')) {
      jsonText = jsonText.slice(0, -3); // Remove trailing ```
    }

    jsonText = jsonText.trim();

    try {
      const parsed = JSON.parse(jsonText);

      // Validate required fields
      const required = [
        'endpoint_name',
        'url_path',
        'description',
        'connection',
        'table',
        'method',
        'parameters',
        'enable_cache',
      ];

      for (const field of required) {
        if (!(field in parsed)) {
          throw new Error(`Missing required field: ${field}`);
        }
      }

      // Validate method is one of the allowed values
      if (!['GET', 'POST', 'PUT', 'DELETE'].includes(parsed.method)) {
        throw new Error(`Invalid HTTP method: ${parsed.method}`);
      }

      // Validate url_path starts with /
      if (!parsed.url_path.startsWith('/')) {
        throw new Error('URL path must start with /');
      }

      // Validate parameters array
      if (!Array.isArray(parsed.parameters)) {
        throw new Error('Parameters must be an array');
      }

      return parsed as GeminiGeneratedConfig;
    } catch (error) {
      if (error instanceof SyntaxError) {
        throw new Error(
          `Failed to parse Gemini response as JSON: ${error.message}\nResponse was: ${jsonText.slice(0, 200)}`
        );
      }
      throw error;
    }
  }

  private delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }
}

export function createGeminiClient(authConfig: GeminiAuthConfig): GeminiClient {
  return new GeminiClient(authConfig);
}
