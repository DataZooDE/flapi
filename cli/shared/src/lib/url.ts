const SLASH_REPLACEMENT = '-slash-';
const EMPTY_REPLACEMENT = 'empty';

/**
 * Convert a URL path to a URL-safe slug for use in route parameters
 * Examples:
 *   "/customers/" -> "customers-slash"
 *   "/publicis" -> "publicis"
 *   "/sap/functions" -> "sap-slash-functions"
 *   "" -> "empty"
 */
export function pathToSlug(path: string): string {
  if (!path) {
    return EMPTY_REPLACEMENT;
  }

  let slug = path;

  // Remove leading slash if present
  if (slug.startsWith('/')) {
    slug = slug.slice(1);
  }

  // Handle trailing slash
  let hasTrailingSlash = false;
  if (slug.endsWith('/')) {
    hasTrailingSlash = true;
    slug = slug.slice(0, -1);
  }

  // Replace internal slashes with hyphens
  slug = slug.replace(/\//g, '-');

  // Replace any remaining special characters with hyphens
  slug = slug.replace(/[^a-zA-Z0-9\-_]/g, '-');

  // Remove multiple consecutive hyphens
  slug = slug.replace(/-+/g, '-');

  // Remove leading/trailing hyphens
  slug = slug.replace(/^-+|-+$/g, '');

  // Add trailing slash indicator
  if (hasTrailingSlash) {
    slug += '-slash';
  }

  return slug || EMPTY_REPLACEMENT;
}

/**
 * Convert a slug back to the original URL path
 * Examples:
 *   "customers-slash" -> "/customers/"
 *   "publicis" -> "/publicis"
 *   "sap-slash-functions" -> "/sap/functions"
 *   "empty" -> ""
 */
export function slugToPath(slug: string): string {
  if (slug === EMPTY_REPLACEMENT) {
    return '';
  }

  let path = slug;

  // Check for trailing slash indicator
  let hasTrailingSlash = false;
  if (path.endsWith('-slash')) {
    hasTrailingSlash = true;
    path = path.slice(0, -6); // Remove "-slash"
  }

  // Replace hyphens back to slashes
  path = path.replace(/-/g, '/');

  // Add leading slash
  if (path) {
    path = '/' + path;
  }

  // Add trailing slash if needed
  if (hasTrailingSlash) {
    path += '/';
  }

  return path;
}

export function buildEndpointUrl(path: string, suffix = ''): string {
  const slug = pathToSlug(path);
  const base = `/api/v1/_config/endpoints/${slug}`;

  if (!suffix) {
    return base;
  }

  const normalizedSuffix = suffix.replace(/^\/+/, '');
  return `${base}/${normalizedSuffix}`;
}