import { describe, it, expect } from 'vitest';
import { parsePayload } from '../../src/lib/payload';

describe('parsePayload', () => {
  it('parses JSON when extension is json', () => {
    const result = parsePayload('{"foo":1}', 'data.json');
    expect(result).toEqual({ foo: 1 });
  });

  it('parses YAML when extension is yaml', () => {
    const result = parsePayload('foo: 1', 'data.yaml');
    expect(result).toEqual({ foo: 1 });
  });

  it('detects JSON without extension', () => {
    const result = parsePayload('{"bar":2}');
    expect(result).toEqual({ bar: 2 });
  });

  it('falls back to YAML when JSON detection fails', () => {
    const result = parsePayload('baz: qux');
    expect(result).toEqual({ baz: 'qux' });
  });

  it('throws on empty payload', () => {
    expect(() => parsePayload('')).toThrowError();
  });
});
