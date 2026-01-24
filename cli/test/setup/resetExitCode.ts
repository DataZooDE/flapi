import { afterEach, afterAll, beforeAll } from 'vitest';

const reset = () => {
  process.exitCode = 0;
};

beforeAll(reset);
afterEach(reset);
afterAll(reset);
