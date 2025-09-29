import * as vscode from 'vscode';
import type { AxiosInstance } from 'axios';
import { buildEndpointUrl, pathToSlug, slugToPath } from '@flapi/shared';
import type { FlapiNode } from '../explorer/FlapiExplorerProvider';

function ensureSlash(path: unknown): string {
  const value = typeof path === 'string' ? path : String(path ?? '');
  if (!value) {
    return '/';
  }
  return value.startsWith('/') ? value : `/${value}`;
}

function getPathFromNode(node?: FlapiNode): string | null {
  if (!node) {
    return null;
  }
  const raw: unknown = (node.extra?.path as unknown) ?? (node.id as unknown);
  const path = ensureSlash(raw);
  return path;
}

export function getSlugFromNode(node?: FlapiNode): string | null {
  const path = getPathFromNode(node);
  if (!path || typeof path !== 'string') {
    return null;
  }
  const existing = node?.extra?.slug;
  if (typeof existing === 'string' && existing.length > 0) {
    return existing;
  }
  return pathToSlug(path);
}

function getPathFromUri(uri: vscode.Uri): string | null {
  if (uri.query) {
    const params = new URLSearchParams(uri.query);
    const encoded = params.get('path');
    if (encoded) {
      return ensureSlash(decodeURIComponent(encoded));
    }
  }
  const slug = uri.path.replace(/^\//, '').replace(/\.[^.]+$/, '');
  return ensureSlash(slugToPath(slug));
}

class EndpointContentProvider implements vscode.TextDocumentContentProvider {
  constructor(private readonly client: AxiosInstance) {}

  async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
    const path = getPathFromUri(uri);
    if (!path) {
      return '';
    }
    try {
      const response = await this.client.get(buildEndpointUrl(path));
      return JSON.stringify(response.data ?? {}, null, 2);
    } catch (error) {
      void vscode.window.showErrorMessage(`Failed to load endpoint ${path}: ${String(error)}`);
      return '';
    }
  }
}

class TemplateContentProvider implements vscode.TextDocumentContentProvider {
  constructor(private readonly client: AxiosInstance) {}

  async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
    const path = getPathFromUri(uri);
    if (!path) {
      return '';
    }
    try {
      const response = await this.client.get(buildEndpointUrl(path, 'template'));
      const data = response.data;
      if (typeof data === 'string') {
        return data;
      }
      if (data && typeof data === 'object' && 'template' in data) {
        return String((data as Record<string, unknown>).template ?? '');
      }
      return '';
    } catch (error) {
      void vscode.window.showErrorMessage(`Failed to load template ${path}: ${String(error)}`);
      return '';
    }
  }
}

class CacheContentProvider implements vscode.TextDocumentContentProvider {
  constructor(private readonly client: AxiosInstance) {}

  async provideTextDocumentContent(uri: vscode.Uri): Promise<string> {
    const path = getPathFromUri(uri);
    if (!path) {
      return '';
    }
    try {
      const response = await this.client.get(buildEndpointUrl(path, 'cache'));
      return JSON.stringify(response.data ?? {}, null, 2);
    } catch (error) {
      void vscode.window.showErrorMessage(`Failed to load cache config for ${path}: ${String(error)}`);
      return '';
    }
  }
}

function resolveTemplateContext(node: FlapiNode | undefined): { path: string; slug: string } | null {
  if (node) {
    const path = getPathFromNode(node);
    const slug = getSlugFromNode(node);
    if (path && slug) {
      return { path, slug };
    }
  }

  const editor = vscode.window.activeTextEditor;
  if (editor && editor.document.uri.scheme === 'flapi-template') {
    const path = getPathFromUri(editor.document.uri);
    if (path) {
      return { path, slug: pathToSlug(path) };
    }
  }

  void vscode.window.showWarningMessage('Open a flapi template editor tab first.');
  return null;
}

function resolveCacheContext(node: FlapiNode | undefined): { path: string; slug: string } | null {
  if (node) {
    const path = getPathFromNode(node);
    const slug = getSlugFromNode(node);
    if (path && slug) {
      return { path, slug };
    }
  }

  const editor = vscode.window.activeTextEditor;
  if (editor && editor.document.uri.scheme === 'flapi-cache') {
    const path = getPathFromUri(editor.document.uri);
    if (path) {
      return { path, slug: pathToSlug(path) };
    }
  }

  void vscode.window.showWarningMessage('Open a flapi cache editor tab first.');
  return null;
}

async function saveTemplate(client: AxiosInstance, node?: FlapiNode) {
  const context = resolveTemplateContext(node);
  if (!context) {
    return;
  }
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.uri.scheme !== 'flapi-template') {
    void vscode.window.showWarningMessage('Focus a flapi template editor tab before saving.');
    return;
  }
  try {
    await client.put(buildEndpointUrl(context.path, 'template'), {
      template: editor.document.getText(),
    });
    void vscode.window.setStatusBarMessage('Template saved', 1500);
  } catch (error) {
    void vscode.window.showErrorMessage(`Failed to save template: ${String(error)}`);
  }
}

async function saveCache(client: AxiosInstance, node?: FlapiNode) {
  const context = resolveCacheContext(node);
  if (!context) {
    return;
  }
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.uri.scheme !== 'flapi-cache') {
    void vscode.window.showWarningMessage('Focus a flapi cache editor tab before saving.');
    return;
  }
  try {
    const content = editor.document.getText();
    const payload = JSON.parse(content);
    await client.put(buildEndpointUrl(context.path, 'cache'), payload);
    void vscode.window.setStatusBarMessage('Cache config saved', 1500);
  } catch (error) {
    void vscode.window.showErrorMessage(`Failed to save cache config: ${String(error)}`);
  }
}

async function expandTemplate(client: AxiosInstance, node?: FlapiNode) {
  const context = resolveTemplateContext(node);
  if (!context) {
    return;
  }
  const input = await vscode.window.showInputBox({
    prompt: 'Template expansion parameters (JSON)',
    value: '{}',
  });
  if (input === undefined) {
    return;
  }
  try {
    const params = input.trim() ? JSON.parse(input) : {};
    const response = await client.post(buildEndpointUrl(context.path, 'template/expand'), {
      parameters: params,
    });
    const doc = await vscode.workspace.openTextDocument({
      content: JSON.stringify(response.data ?? {}, null, 2),
      language: 'json',
    });
    await vscode.window.showTextDocument(doc, { preview: false });
  } catch (error) {
    void vscode.window.showErrorMessage(`Failed to expand template: ${String(error)}`);
  }
}

async function testTemplate(client: AxiosInstance, node?: FlapiNode) {
  const context = resolveTemplateContext(node);
  if (!context) {
    return;
  }
  const input = await vscode.window.showInputBox({
    prompt: 'Template test parameters (JSON)',
    value: '{}',
  });
  if (input === undefined) {
    return;
  }
  try {
    const params = input.trim() ? JSON.parse(input) : {};
    const response = await client.post(buildEndpointUrl(context.path, 'template/test'), {
      parameters: params,
    });
    const doc = await vscode.workspace.openTextDocument({
      content: JSON.stringify(response.data ?? {}, null, 2),
      language: 'json',
    });
    await vscode.window.showTextDocument(doc, { preview: false });
  } catch (error) {
    void vscode.window.showErrorMessage(`Failed to test template: ${String(error)}`);
  }
}

export function registerContentProviders(context: vscode.ExtensionContext, client: AxiosInstance): vscode.Disposable {
  const disposables: vscode.Disposable[] = [];

  disposables.push(
    vscode.workspace.registerTextDocumentContentProvider('flapi-endpoint', new EndpointContentProvider(client)),
    vscode.workspace.registerTextDocumentContentProvider('flapi-template', new TemplateContentProvider(client)),
    vscode.workspace.registerTextDocumentContentProvider('flapi-cache', new CacheContentProvider(client))
  );

  const composite = vscode.Disposable.from(...disposables);
  context.subscriptions.push(composite);
  return composite;
}
