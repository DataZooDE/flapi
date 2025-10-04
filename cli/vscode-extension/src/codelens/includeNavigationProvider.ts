import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

/**
 * CodeLens provider for navigating YAML include directives
 * Detects patterns like: {{include:section from file.yaml}}
 */
export class IncludeNavigationProvider implements vscode.CodeLensProvider {
  private _onDidChangeCodeLenses: vscode.EventEmitter<void> = new vscode.EventEmitter<void>();
  public readonly onDidChangeCodeLenses: vscode.Event<void> = this._onDidChangeCodeLenses.event;

  // Pattern to match include directives: {{include:section from file.yaml}}
  private readonly includePattern = /\{\{include:([^}]+)\}\}/g;

  constructor() {}

  public refresh(): void {
    this._onDidChangeCodeLenses.fire();
  }

  public provideCodeLenses(
    document: vscode.TextDocument,
    token: vscode.CancellationToken
  ): vscode.CodeLens[] | Thenable<vscode.CodeLens[]> {
    const codeLenses: vscode.CodeLens[] = [];

    // Only process YAML files
    if (!document.fileName.endsWith('.yaml') && !document.fileName.endsWith('.yml')) {
      return codeLenses;
    }

    const text = document.getText();
    const lines = text.split('\n');

    for (let lineIndex = 0; lineIndex < lines.length; lineIndex++) {
      const line = lines[lineIndex];
      
      // Reset regex state
      this.includePattern.lastIndex = 0;
      
      let match: RegExpExecArray | null;
      while ((match = this.includePattern.exec(line)) !== null) {
        const fullMatch = match[0]; // {{include:section from file.yaml}}
        const includeContent = match[1].trim(); // section from file.yaml
        
        // Parse the include directive
        const includeInfo = this.parseIncludeDirective(includeContent);
        if (!includeInfo) {
          continue;
        }

        // Calculate the range for the entire include directive
        const startChar = match.index;
        const endChar = match.index + fullMatch.length;
        const range = new vscode.Range(lineIndex, startChar, lineIndex, endChar);

        // Create CodeLens for navigation
        const codeLens = new vscode.CodeLens(range);
        codeLens.command = {
          title: `→ Open ${includeInfo.file}${includeInfo.section ? `:${includeInfo.section}` : ''}`,
          command: 'flapi.navigateToInclude',
          arguments: [document.uri, includeInfo]
        };
        
        codeLenses.push(codeLens);
      }
    }

    return codeLenses;
  }

  /**
   * Parse include directive into file and optional section
   * Examples:
   *   "section from file.yaml" -> { file: "file.yaml", section: "section" }
   *   "file.yaml" -> { file: "file.yaml", section: null }
   */
  private parseIncludeDirective(content: string): { file: string; section: string | null } | null {
    const trimmed = content.trim();
    
    // Check for "section from file" pattern
    const fromMatch = trimmed.match(/^(.+?)\s+from\s+(.+)$/);
    if (fromMatch) {
      return {
        section: fromMatch[1].trim(),
        file: fromMatch[2].trim()
      };
    }
    
    // Just a file reference
    if (trimmed.endsWith('.yaml') || trimmed.endsWith('.yml')) {
      return {
        file: trimmed,
        section: null
      };
    }
    
    return null;
  }
}

/**
 * Navigate to an included YAML file and optionally jump to a specific section
 */
export async function navigateToInclude(
  sourceUri: vscode.Uri,
  includeInfo: { file: string; section: string | null }
): Promise<void> {
  try {
    // Resolve the target file path relative to the source file
    const sourceDir = path.dirname(sourceUri.fsPath);
    const targetPath = path.resolve(sourceDir, includeInfo.file);

    // Check if file exists
    if (!fs.existsSync(targetPath)) {
      vscode.window.showErrorMessage(`Include file not found: ${includeInfo.file}`);
      return;
    }

    // Open the target file
    const targetUri = vscode.Uri.file(targetPath);
    const document = await vscode.workspace.openTextDocument(targetUri);
    
    // Find the section if specified
    let targetLine = 0;
    if (includeInfo.section) {
      targetLine = findSectionInDocument(document, includeInfo.section);
    }

    // Show the document with the cursor at the target line
    const editor = await vscode.window.showTextDocument(document, {
      viewColumn: vscode.ViewColumn.Active,
      preview: false
    });

    // Move cursor to the target line
    if (targetLine > 0) {
      const position = new vscode.Position(targetLine, 0);
      editor.selection = new vscode.Selection(position, position);
      editor.revealRange(
        new vscode.Range(position, position),
        vscode.TextEditorRevealType.InCenter
      );
    }

  } catch (error) {
    vscode.window.showErrorMessage(`Failed to navigate to include: ${error}`);
  }
}

/**
 * Find a section in a YAML document
 * Looks for patterns like:
 *   section:
 *   # Section
 *   # == Section ==
 */
function findSectionInDocument(document: vscode.TextDocument, sectionName: string): number {
  const text = document.getText();
  const lines = text.split('\n');

  // Normalize section name for comparison
  const normalizedSection = sectionName.toLowerCase().trim();

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    
    // Match YAML key: "section:"
    if (line.startsWith(normalizedSection + ':')) {
      return i;
    }

    // Match comment headers: "# Section" or "# == Section =="
    if (line.startsWith('#')) {
      const commentContent = line.substring(1).trim();
      const stripped = commentContent.replace(/^[=\s]+|[=\s]+$/g, '').toLowerCase();
      
      if (stripped === normalizedSection || commentContent.toLowerCase().includes(normalizedSection)) {
        return i;
      }
    }

    // Match capitalized section names in comments
    const words = line.split(/\s+/);
    for (const word of words) {
      if (word.toLowerCase() === normalizedSection) {
        return i;
      }
    }
  }

  // Section not found, return 0 (top of file)
  return 0;
}

