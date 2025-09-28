import * as colorette from 'colorette';
import ora, { Ora } from 'ora';
import logSymbols from 'log-symbols';
import chalk from 'chalk';
import boxen from 'boxen';
import figures from 'figures';

export type Spinner = Ora;

export const Console = {
  color(color: keyof typeof colorette, text: string) {
    const fn = colorette[color];
    return typeof fn === 'function' ? fn(text) : text;
  },

  success(message: string) {
    console.log(chalk.green(`${figures.tick} ${message}`));
  },

  info(message: string) {
    console.log(message);
  },

  warn(message: string) {
    console.log(chalk.yellow(`${figures.warning} ${message}`));
  },

  error(message: string) {
    console.error(chalk.red(`${figures.cross} ${message}`));
  },

  spinner(text: string): Spinner {
    return ora({
      text: chalk.blue(text),
      spinner: 'dots',
    }).start();
  },

  /**
   * Create a beautiful box around text
   */
  box(text: string, title?: string): string {
    const options = {
      padding: 1,
      margin: 1,
      borderStyle: 'round' as const,
      borderColor: 'blue' as const,
      title,
      titleAlignment: 'center' as const,
    };
    return boxen(text, options);
  },

  /**
   * Create a panel with title and content
   */
  panel(content: string, title?: string): string {
    return boxen(content, {
      title,
      borderStyle: 'singleDouble' as const,
      borderColor: 'cyan' as const,
      padding: 1,
      margin: 1,
    });
  },

  /**
   * Pretty print JSON
   */
  json(data: unknown, indent = 2): string {
    return JSON.stringify(data, null, indent);
  },

  /**
   * Create a header with styling
   */
  header(text: string): string {
    return chalk.bold.blue(`\n${figures.pointer} ${text}\n`);
  },

  /**
   * Create a section divider
   */
  divider(text?: string): string {
    if (text) {
      const line = '─'.repeat(50);
      return chalk.gray(`${line} ${text} ${line}`);
    }
    return chalk.gray('─'.repeat(80));
  },
};

