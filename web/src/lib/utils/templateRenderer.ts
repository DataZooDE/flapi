import yaml from 'js-yaml';
import Mustache from 'mustache';

export interface TemplateContext {
  conn: Record<string, any>;
  params: Record<string, any>;
  [key: string]: any;
}

export function renderYamlTemplate(yamlContent: string): string {
  try {
    // Parse YAML to ensure it's valid
    const parsed = yaml.load(yamlContent);
    // Convert back to YAML string for consistent formatting
    return yaml.dump(parsed, {
      indent: 2,
      lineWidth: -1,
      noRefs: true,
    });
  } catch (error) {
    console.error('Error rendering YAML template:', error);
    throw error;
  }
}

export function renderSqlTemplate(sqlTemplate: string, context: TemplateContext): string {
  try {
    return Mustache.render(sqlTemplate, context);
  } catch (error) {
    console.error('Error rendering SQL template:', error);
    throw error;
  }
}

export function validateYaml(content: string): boolean {
  try {
    yaml.load(content);
    return true;
  } catch (error) {
    return false;
  }
}

export function validateSql(content: string): boolean {
  try {
    // Basic validation - check if the template can be parsed by Mustache
    Mustache.parse(content);
    return true;
  } catch (error) {
    return false;
  }
} 