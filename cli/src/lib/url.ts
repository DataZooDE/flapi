/**
 * The slug codec lives in @flapi/shared and nowhere else.
 *
 * It used to be duplicated here. The duplicate drifted: when the codec changed
 * to the '~1'/'~0' escaping this file was rewritten and shared/src/lib/url.ts
 * was not, so the VSCode extension and shared apiClient kept encoding
 * '/customers/' as 'customers-slash' against a server that now expects
 * '-customers-'. Every config call from the extension 404'd, and the unit test
 * could not see it because it imported this copy rather than the bundled one.
 *
 * Re-export, never re-implement.
 */
export { pathToSlug, slugToPath, buildEndpointUrl } from '@flapi/shared';
