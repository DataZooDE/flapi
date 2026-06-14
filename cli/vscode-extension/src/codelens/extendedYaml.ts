import * as yaml from 'yaml';

/**
 * A parsed `{{include:<section> from <file> [if <condition>]}}` directive.
 */
export interface IncludeDirective {
  section: string;
  file: string;
  condition?: string;
  raw: string;
}

// Matches: {{include:request from customer-common.yaml}}
//          {{include:auth from customer-common.yaml if env.SECURE}}
const INCLUDE_RE = /\{\{include:([A-Za-z0-9_-]+)\s+from\s+([^\s}]+?)(?:\s+if\s+([^}]+?))?\s*\}\}/g;

/**
 * Extract every active include directive from extended YAML text.
 * Directives inside YAML comments (lines starting with `#`) are ignored, mirroring
 * how the server's ExtendedYamlParser treats commented-out includes.
 */
export function parseIncludeDirectives(text: string): IncludeDirective[] {
  const directives: IncludeDirective[] = [];
  for (const rawLine of text.split(/\r?\n/)) {
    if (rawLine.trimStart().startsWith('#')) {
      continue;
    }
    INCLUDE_RE.lastIndex = 0;
    let match: RegExpExecArray | null;
    while ((match = INCLUDE_RE.exec(rawLine)) !== null) {
      directives.push({
        section: match[1],
        file: match[2],
        condition: match[3]?.trim(),
        raw: match[0],
      });
    }
  }
  return directives;
}

/**
 * Remove active include directives (and resulting blank lines) so the remaining
 * text is parseable as plain YAML. Commented directives are left untouched.
 */
function stripActiveIncludes(text: string): string {
  return text
    .split(/\r?\n/)
    .map((line) => {
      if (line.trimStart().startsWith('#')) {
        return line;
      }
      return line.replace(INCLUDE_RE, '');
    })
    .filter((line, idx, lines) => {
      // Drop lines that became empty *only* as a result of stripping a directive.
      const collapsed = line.trim().length === 0;
      const prevWasDirectiveBlank = idx > 0 && lines[idx - 1] !== undefined;
      return !(collapsed && prevWasDirectiveBlank && line === '');
    })
    .join('\n');
}

/**
 * Resolve a flapi extended-YAML document into a plain config object by inlining
 * `{{include:<section> from <file>}}` directives.
 *
 * `readFile` is injected (rather than touching the filesystem directly) so this
 * function stays pure and unit-testable; it receives the directive's `file` value
 * verbatim and should return the referenced file's contents, or `undefined` if it
 * cannot be read.
 *
 * Conditional includes (`... if env.X`) are resolved unconditionally: for parsing
 * purposes we want the most complete view of the config, and the condition cannot
 * be evaluated without the server's environment.
 */
export function resolveExtendedYaml(
  text: string,
  readFile: (file: string) => string | undefined,
): any {
  const base = yaml.parse(stripActiveIncludes(text)) ?? {};
  const directives = parseIncludeDirectives(text);
  const sourceCache = new Map<string, any>();

  for (const directive of directives) {
    if (Object.prototype.hasOwnProperty.call(base, directive.section)) {
      // An explicit value in the host file wins over the include.
      continue;
    }

    let source = sourceCache.get(directive.file);
    if (source === undefined) {
      const contents = readFile(directive.file);
      if (contents === undefined) {
        continue;
      }
      try {
        source = yaml.parse(contents) ?? {};
      } catch {
        source = {};
      }
      sourceCache.set(directive.file, source);
    }

    if (source && Object.prototype.hasOwnProperty.call(source, directive.section)) {
      base[directive.section] = source[directive.section];
    }
  }

  return base;
}
