const path = require('path');
const MonacoWebpackPlugin = require('monaco-editor-webpack-plugin');

module.exports = {
  mode: 'production',
  entry: path.resolve(__dirname, 'src/index.ts'),
  output: {
    filename: 'endpointEditor.bundle.js',
    path: path.resolve(__dirname),
    libraryTarget: 'umd',
    globalObject: 'self',
  },
  resolve: {
    extensions: ['.ts', '.js'],
  },
  module: {
    rules: [
      {
        test: /\.ts$/,
        use: {
          loader: 'ts-loader',
          options: {
            configFile: path.resolve(__dirname, 'tsconfig.json'),
          },
        },
        exclude: /node_modules/,
      },
      {
        test: /\.css$/,
        use: ['style-loader', 'css-loader'],
      },
    ],
  },
  plugins: [
    new MonacoWebpackPlugin({
      // Exclude all built-in languages to avoid loading language workers
      languages: [],
      features: [
        '!gotoSymbol',
        '!gotoSymbol-multiple',
        '!quickCommand',
        '!quickHelp',
        '!quickOutline',
        '!referenceSearch',
        '!rename',
        '!suggest',
        '!toggleHighContrast',
        '!toggleTabFocusMode',
        '!toggleWordWrap',
        '!viewportSemanticTokens',
        '!wordHighlighter',
        '!wordOperations',
        '!wordPartOperations',
      ],
    }),
  ],
};
