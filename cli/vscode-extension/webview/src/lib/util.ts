export function debounce<T extends (...args: any[]) => void>(fn: T, wait = 400) {
  let handle: ReturnType<typeof setTimeout> | undefined;
  return (...args: Parameters<T>) => {
    if (handle !== undefined) {
      clearTimeout(handle);
    }
    handle = setTimeout(() => {
      fn(...args);
    }, wait) as unknown as ReturnType<typeof setTimeout>;
  };
}

export function tryParseJson<T>(value: string, fallback: T): { data: T; error?: string } {
  if (!value || value.trim() === '') {
    return { data: fallback };
  }
  try {
    return { data: JSON.parse(value) };
  } catch (error: any) {
    return { data: fallback, error: error?.message ?? String(error) };
  }
}

export function prettyJson(value: unknown): string {
  if (value === undefined || value === null) {
    return '';
  }
  return JSON.stringify(value, null, 2);
}

export function clamp(value: number, min: number, max: number) {
  return Math.min(max, Math.max(min, value));
}
