export interface EditorStatePayload {
  path: string;
  slug: string;
  template: string;
  cacheTemplate: string;
  config: any;
  rawConfig: any;
  cache: any;
  version: number;
  force?: boolean;
}

export interface SchemaCatalog {
  name: string;
  tables: Record<string, SchemaTable>;
}

export interface SchemaTable {
  name: string;
  columns: SchemaColumn[];
}

export interface SchemaColumn {
  name: string;
  type: string;
  nullable: boolean;
}

export interface EditorPersistedState {
  tab: 'request' | 'sql' | 'cache';
  sql: string;
  cacheTemplate: string;
  requestJson: string;
  cacheJson: string;
  params: string;
  paramsResult: string;
  autoExpandSql: boolean;
  autoExpandCache: boolean;
}

export type PreviewKind = 'expanded' | 'result';
