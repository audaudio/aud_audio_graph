// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// Builds and runs the native tests of the graph engine with the address and
// the undefined behaviour sanitizer: `node scripts/test-native.js`. The
// binary is cached under .dart_tool by the hash of its inputs, so a run
// without changes only executes it. Pass `--no-sanitize` on a toolchain
// without the sanitizer runtimes.

'use strict';

const { createHash } = require('node:crypto');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const sanitize = !process.argv.includes('--no-sanitize');

// The `src` directory of a package, resolved through the package config.
function packageSrc(name) {
  const configPath = path.join(root, '.dart_tool', 'package_config.json');
  if (!fs.existsSync(configPath)) {
    throw new Error(`Run dart pub get first: ${configPath} is missing`);
  }
  const config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
  const entry = config.packages.find((p) => p.name === name);
  if (!entry) throw new Error(`Package ${name} is not in the package config`);
  const rootUri = new URL(entry.rootUri, `file://${configPath}`);
  return path.join(decodeURIComponent(rootUri.pathname), 'src');
}

function listFiles(directory, extension) {
  return fs
    .readdirSync(directory)
    .filter((file) => file.endsWith(extension))
    .map((file) => path.join(directory, file))
    .sort();
}

function compiler() {
  for (const candidate of ['clang++', 'c++', 'g++']) {
    const probe = spawnSync(candidate, ['--version'], { encoding: 'utf8' });
    if (probe.status === 0) return candidate;
  }
  throw new Error('No C++ compiler found (clang++, c++ or g++)');
}

function main() {
  const coreSrc = packageSrc('aud_audio_core');
  const sources = [
    ...listFiles(path.join(root, 'src'), '.cpp'),
    ...listFiles(path.join(root, 'test', 'native'), '.cpp'),
  ];
  const headers = [
    ...listFiles(path.join(root, 'src'), '.h'),
    ...listFiles(path.join(root, 'src'), '.hpp'),
    ...listFiles(path.join(root, 'test', 'native'), '.hpp'),
    ...listFiles(coreSrc, '.h'),
    ...listFiles(coreSrc, '.hpp'),
  ];
  const hash = createHash('sha256');
  hash.update(String(sanitize));
  for (const file of [...sources, ...headers]) {
    hash.update(file);
    hash.update(fs.readFileSync(file));
  }
  const outDir = path.join(root, '.dart_tool', 'aud_native_test');
  fs.mkdirSync(outDir, { recursive: true });
  const binary = path.join(outDir, `aud_graph_test_${hash.digest('hex').slice(0, 16)}`);
  if (!fs.existsSync(binary)) {
    const flags = [
      '-std=c++17',
      '-g',
      '-O1',
      '-Wall',
      '-Wextra',
      '-Wno-unused-parameter',
      '-fno-omit-frame-pointer',
      `-I${path.join(root, 'src')}`,
      `-I${coreSrc}`,
      '-o',
      binary,
      ...sources,
    ];
    if (sanitize) flags.unshift('-fsanitize=address,undefined');
    if (process.platform !== 'win32') flags.push('-lpthread');
    const build = spawnSync(compiler(), flags, { encoding: 'utf8' });
    if (build.status !== 0) {
      process.stderr.write(build.stdout + build.stderr);
      process.exit(build.status ?? 1);
    }
  }
  // The leak check exists on Linux only; Apple's runtime refuses the option.
  const leaks = process.platform === 'linux' ? 'detect_leaks=1' : 'detect_leaks=0';
  const run = spawnSync(binary, [], {
    encoding: 'utf8',
    env: {
      ...process.env,
      UBSAN_OPTIONS: 'print_stacktrace=1',
      ASAN_OPTIONS: leaks,
    },
  });
  process.stdout.write(run.stdout);
  process.stderr.write(run.stderr);
  process.exit(run.status ?? 1);
}

main();
