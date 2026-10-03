import * as path from 'node:path';

/**
 * Resolve a server-supplied relative path inside `root`, or throw.
 *
 * The explorer's open/delete/rename commands joined a path taken from the server's
 * file listing onto the local templates directory. A path such as `../../x` from a
 * hostile or compromised server then pointed outside it.
 */
export function resolveInside(root: string, relative: string): string {
  if (!relative || path.isAbsolute(relative) || relative.includes('\0')) {
    throw new Error(`Refusing path outside the templates directory: ${JSON.stringify(relative)}`);
  }
  const base = path.resolve(root);
  const full = path.resolve(base, relative);
  const rel = path.relative(base, full);
  if (rel === '' || rel.startsWith('..') || path.isAbsolute(rel)) {
    throw new Error(`Refusing path outside the templates directory: ${JSON.stringify(relative)}`);
  }
  return full;
}

/** The local directory the explorer maps server paths onto. */
export function templatesRoot(workspaceRoot: string): string {
  return path.join(workspaceRoot, 'examples', 'sqls');
}
