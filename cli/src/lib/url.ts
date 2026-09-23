const EMPTY_REPLACEMENT = 'empty';

/**
 * Convert a URL path to a URL-safe slug for the config service.
 *
 *   "/customers/"     -> "-customers-"
 *   "/sap/functions"  -> "-sap-functions"
 *   "/order-items"    -> "-order~1items"
 *   "/"               -> "-"
 *   ""                -> "empty"
 *
 * MUST stay byte-identical to PathUtils::pathToSlug in src/path_utils.cpp.
 * Both are exercised against test/fixtures/slug_codec.json; that fixture
 * exists because these two drifted - the CLI shipped '-slash-' while the
 * server used '-slash', and when the codec changed the CLI was not updated at
 * all, so every config command would have 404'd against the new server. Both
 * binaries ship in the same flapi-io wheel.
 *
 * '/' becomes '-' so the common case stays readable; a literal '-' or '~' is
 * escaped JSON-pointer style. The alphabet contains no '%' on purpose:
 * buildEndpointUrl below calls encodeURIComponent, and RFC 3986 lets any
 * normaliser decode an escaped unreserved character - so a '%'-based escape
 * could be silently undone in transit and reintroduce the collision this
 * codec exists to remove.
 */
export function pathToSlug(path: string): string {
  if (!path) {
    return EMPTY_REPLACEMENT;
  }

  let slug = '';
  for (const ch of path) {
    if (ch === '/') {
      slug += '-';
    } else if (ch === '-') {
      slug += '~1';
    } else if (ch === '~') {
      slug += '~0';
    } else {
      slug += ch;
    }
  }
  return slug;
}

/**
 * Inverse of pathToSlug. slugToPath(pathToSlug(p)) === p for every p.
 */
export function slugToPath(slug: string): string {
  if (slug === EMPTY_REPLACEMENT) {
    return '';
  }

  let path = '';
  for (let i = 0; i < slug.length; ) {
    if (slug[i] === '-') {
      path += '/';
      i += 1;
    } else if (slug[i] === '~' && slug[i + 1] === '1') {
      path += '-';
      i += 2;
    } else if (slug[i] === '~' && slug[i + 1] === '0') {
      path += '~';
      i += 2;
    } else {
      path += slug[i];
      i += 1;
    }
  }
  return path;
}

export function buildEndpointUrl(path: string, suffix = ''): string {
  const slug = pathToSlug(path);
  const encoded = encodeURIComponent(slug);
  const base = `/api/v1/_config/endpoints/${encoded}`;

  if (!suffix) {
    return base;
  }

  const normalizedSuffix = suffix.replace(/^\/+/g, '');
  return `${base}/${normalizedSuffix}`;
}
