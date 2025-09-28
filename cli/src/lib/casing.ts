import kebabCase from 'lodash.kebabcase';
import camelCase from 'camelcase';

export type JsonObject = Record<string, unknown>;

export function toHyphenCaseKeys<T extends JsonObject>(input: T): JsonObject {
  return transformKeys(input, (key) => kebabCase(key));
}

export function toCamelCaseKeys<T extends JsonObject>(input: T): JsonObject {
  return transformKeys(input, (key) => camelCase(key));
}

function transformKeys(obj: JsonObject, transform: (key: string) => string): JsonObject {
  const result: JsonObject = {};
  for (const [key, value] of Object.entries(obj)) {
    if (Array.isArray(value)) {
      result[transform(key)] = value.map((item) =>
        typeof item === 'object' && item !== null ? transformKeys(item as JsonObject, transform) : item,
      );
    } else if (value && typeof value === 'object') {
      result[transform(key)] = transformKeys(value as JsonObject, transform);
    } else {
      result[transform(key)] = value;
    }
  }
  return result;
}

