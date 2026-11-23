// Hand-written shape checks for the exported JSON files (spec 4.4 "Loading"). Every check
// names the file and the place in it, so a refused file can be reported in plain words.

export class DataError extends Error {
  constructor(
    readonly file: string,
    readonly problem: string,
  ) {
    super(`${file}: ${problem}`);
    this.name = 'DataError';
  }
}

export type JsonObject = Record<string, unknown>;

export class Check {
  constructor(readonly file: string) {}

  fail(path: string, problem: string): never {
    throw new DataError(this.file, path === '' ? problem : `${path} ${problem}`);
  }

  object(value: unknown, path: string): JsonObject {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) {
      this.fail(path, 'should be an object');
    }
    return value as JsonObject;
  }

  /** The field `key` of `object`, which must be present (it may be null). */
  field(object: JsonObject, key: string, path: string): unknown {
    if (!(key in object)) this.fail(join(path, key), 'is missing');
    return object[key];
  }

  array(value: unknown, path: string, length?: number): unknown[] {
    if (!Array.isArray(value)) this.fail(path, 'should be a list');
    if (length !== undefined && value.length !== length) {
      this.fail(path, `should have ${length} items but has ${value.length}`);
    }
    return value;
  }

  number(value: unknown, path: string): number {
    if (typeof value !== 'number' || !Number.isFinite(value)) this.fail(path, 'should be a number');
    return value;
  }

  integer(value: unknown, path: string): number {
    if (!Number.isInteger(value)) this.fail(path, 'should be a whole number');
    return value as number;
  }

  /** An index into a list of `count` things called `what` (satellites, sites, ...). */
  index(value: unknown, count: number, path: string, what: string): number {
    const i = this.integer(value, path);
    if (i < 0 || i >= count) {
      this.fail(path, `is ${i}, but there are only ${count} ${what} (indices 0 to ${count - 1})`);
    }
    return i;
  }

  string(value: unknown, path: string): string {
    if (typeof value !== 'string') this.fail(path, 'should be a text');
    return value;
  }

  boolean(value: unknown, path: string): boolean {
    if (typeof value !== 'boolean') this.fail(path, 'should be true or false');
    return value;
  }

  oneOf<T extends string>(value: unknown, options: readonly T[], path: string): T {
    const text = this.string(value, path);
    if (!(options as readonly string[]).includes(text)) {
      this.fail(path, `is "${text}", which is not one of ${options.join(', ')}`);
    }
    return text as T;
  }

  orNull<T>(value: unknown, read: (value: unknown) => T): T | null {
    return value === null ? null : read(value);
  }

  /** Each item of a list, read by `read` with its own path. */
  list<T>(value: unknown, path: string, read: (item: unknown, path: string) => T): T[] {
    return this.array(value, path).map((item, i) => read(item, `${path}[${i}]`));
  }
}

export function join(path: string, key: string): string {
  return path === '' ? key : `${path}.${key}`;
}
