import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { pathToSlug, slugToPath, buildEndpointUrl } from '../../src/lib/url';

// The same fixture the C++ suite reads. It exists because these two
// implementations drifted: the CLI shipped '-slash-' while the server used
// '-slash', and when the server's codec changed the CLI was not updated at
// all - so every config command would have 404'd against the new server.
// Both binaries ship in the same flapi-io wheel, so the release would have
// broken its own client.
const fixture = JSON.parse(
  readFileSync(join(__dirname, '../../../test/fixtures/slug_codec.json'), 'utf8'),
) as { cases: Array<{ path: string; slug: string }> };

describe('slug codec', () => {
  it('matches the server for every shared case', () => {
    for (const { path, slug } of fixture.cases) {
      expect(pathToSlug(path), `encode ${JSON.stringify(path)}`).toBe(slug);
      expect(slugToPath(slug), `decode ${JSON.stringify(slug)}`).toBe(path);
    }
  });

  it('is injective across the shared cases', () => {
    const seen = new Map<string, string>();
    for (const { path } of fixture.cases) {
      const slug = pathToSlug(path);
      const clash = seen.get(slug);
      expect(clash, `${JSON.stringify(path)} collides with ${JSON.stringify(clash)}`).toBeUndefined();
      seen.set(slug, path);
    }
  });

  it('does not confuse a hyphen with a slash', () => {
    // The defect behind #123: "/a-b" and "/a/b" shared one config-service URL,
    // and a round trip rewrote the first into the second.
    expect(pathToSlug('/order-items')).not.toBe(pathToSlug('/order/items'));
    expect(slugToPath(pathToSlug('/order-items'))).toBe('/order-items');
    expect(slugToPath(pathToSlug('/order/items'))).toBe('/order/items');
  });

  it('survives its own URL encoding', () => {
    // buildEndpointUrl percent-encodes the slug. A '%'-based escape would be
    // double-encoded here, and RFC 3986 permits a normaliser to decode an
    // escaped unreserved character - which would turn "-order%2Ditems" back
    // into "-order-items" and reintroduce the collision.
    for (const { path } of fixture.cases) {
      const slug = pathToSlug(path);
      expect(slug).not.toContain('%');
      expect(decodeURIComponent(encodeURIComponent(slug))).toBe(slug);
    }
  });

  it('builds the config-service URL a server would resolve', () => {
    expect(buildEndpointUrl('/customers/')).toBe(
      '/api/v1/_config/endpoints/-customers-',
    );
    expect(buildEndpointUrl('/order-items', 'cache')).toBe(
      '/api/v1/_config/endpoints/-order~1items/cache',
    );
  });
});
