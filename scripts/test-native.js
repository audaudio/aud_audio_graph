// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// Builds and runs the native tests of the graph engine, all of them with
// the debug watchdog (`AUD_GRAPH_WATCHDOG`), in up to three builds
// (ticket 20):
//
// - the address and the undefined behaviour sanitizer,
// - Clang's RealtimeSanitizer when a compiler on this machine has it, with
//   a probe that proves the run catches an allocation on the audio thread,
// - the thread sanitizer for the tests that render on a thread of their
//   own while the control thread works.
//
// The binaries are cached under .dart_tool by the hash of their inputs, so
// a run without changes only executes them.
//
//   node scripts/test-native.js               every build this machine has
//   node scripts/test-native.js --no-sanitize  one build, no sanitizer
//   node scripts/test-native.js --rtsan        fail without rtsan
//   node scripts/test-native.js --no-rtsan     skip the rtsan build
//   node scripts/test-native.js --no-tsan      skip the thread sanitizer

'use strict';

const { createHash } = require('node:crypto');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const args = process.argv.slice(2);
const sanitize = !args.includes('--no-sanitize');
const wantRtsan = args.includes('--rtsan');
const skipRtsan = args.includes('--no-rtsan') || !sanitize;
const skipTsan = args.includes('--no-tsan') || !sanitize;

// Compilers that may carry the RealtimeSanitizer: Apple's clang refuses
// it on arm64, LLVM from Homebrew has it.
const rtsanCandidates = [
  process.env.AUD_RTSAN_CXX,
  '/opt/homebrew/opt/llvm/bin/clang++',
  '/usr/local/opt/llvm/bin/clang++',
  'clang++',
].filter(Boolean);

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

// Whether `compiler` exists and builds a program with `flags`.
function works(compiler, flags) {
  const probe = spawnSync(compiler, ['--version'], { encoding: 'utf8' });
  if (probe.status !== 0) return false;
  if (flags.length === 0) return true;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'aud-probe-'));
  try {
    const source = path.join(dir, 'probe.cpp');
    fs.writeFileSync(source, 'int main() { return 0; }\n');
    const build = spawnSync(
      compiler,
      [...flags, source, '-o', path.join(dir, 'probe')],
      { encoding: 'utf8' },
    );
    return build.status === 0;
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

function defaultCompiler() {
  for (const candidate of ['clang++', 'c++', 'g++']) {
    if (works(candidate, [])) return candidate;
  }
  throw new Error('No C++ compiler found (clang++, c++ or g++)');
}

function rtsanCompiler() {
  for (const candidate of rtsanCandidates) {
    if (works(candidate, ['-fsanitize=realtime'])) return candidate;
  }
  return null;
}

// Builds one variant of the test binary unless it is cached; returns its
// path.
function build(name, compiler, extraFlags, sources, headers, coreSrc) {
  const hash = createHash('sha256');
  hash.update(name);
  hash.update(compiler);
  hash.update(extraFlags.join(' '));
  for (const file of [...sources, ...headers]) {
    hash.update(file);
    hash.update(fs.readFileSync(file));
  }
  const outDir = path.join(root, '.dart_tool', 'aud_native_test');
  fs.mkdirSync(outDir, { recursive: true });
  const binary = path.join(
    outDir,
    `aud_graph_test_${name}_${hash.digest('hex').slice(0, 16)}`,
  );
  if (fs.existsSync(binary)) return binary;
  const flags = [
    ...extraFlags,
    '-std=c++17',
    '-g',
    '-O1',
    '-Wall',
    '-Wextra',
    '-Wno-unused-parameter',
    '-fno-omit-frame-pointer',
    '-DAUD_GRAPH_WATCHDOG=1',
    `-I${path.join(root, 'src')}`,
    `-I${coreSrc}`,
    '-o',
    binary,
    ...sources,
  ];
  if (process.platform !== 'win32') flags.push('-lpthread');
  const result = spawnSync(compiler, flags, { encoding: 'utf8' });
  if (result.status !== 0) {
    process.stderr.write(result.stdout + result.stderr);
    process.exit(result.status ?? 1);
  }
  return binary;
}

function execute(binary, env) {
  return spawnSync(binary, [], {
    encoding: 'utf8',
    env: { ...process.env, ...env },
    maxBuffer: 64 * 1024 * 1024,
  });
}

function run(name, binary, env) {
  process.stdout.write(`${name}: ${path.basename(binary)}\n`);
  const result = execute(binary, env);
  process.stdout.write(result.stdout);
  process.stderr.write(result.stderr);
  if (result.status !== 0) process.exit(result.status ?? 1);
}

// The rtsan run must catch what aud.test.allocator does on the audio
// thread once it stops telling the sanitizer; otherwise its silence would
// prove nothing.
function probeRtsan(binary, env) {
  const result = execute(binary, {
    ...env,
    AUD_TEST_FILTER: 'watchdog_counts_allocations',
    AUD_TEST_RTSAN_PROBE: '1',
  });
  if (result.status === 0 || !result.stderr.includes('RealtimeSanitizer')) {
    process.stderr.write(
      'rtsan: the probe allocation on the audio thread was not caught\n' +
        result.stdout +
        result.stderr,
    );
    process.exit(1);
  }
  process.stdout.write('rtsan: the probe allocation was caught\n');
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
  const compiler = defaultCompiler();
  // The leak check exists on Linux only; Apple's runtime refuses the option.
  const leaks =
    process.platform === 'linux' ? 'detect_leaks=1' : 'detect_leaks=0';
  const main = build(
    sanitize ? 'asan' : 'plain',
    compiler,
    sanitize ? ['-fsanitize=address,undefined'] : [],
    sources,
    headers,
    coreSrc,
  );
  run(sanitize ? 'asan+ubsan' : 'plain', main, {
    UBSAN_OPTIONS: 'print_stacktrace=1:halt_on_error=1',
    ASAN_OPTIONS: leaks,
  });
  if (!skipRtsan) {
    const rtsanCxx = rtsanCompiler();
    if (rtsanCxx === null) {
      if (wantRtsan) {
        throw new Error('No compiler with -fsanitize=realtime found');
      }
      process.stdout.write('rtsan: no compiler has it, skipped\n');
    } else {
      const rtsan = build(
        'rtsan',
        rtsanCxx,
        ['-fsanitize=realtime', '-DAUD_GRAPH_RTSAN=1', '-Wno-function-effects'],
        sources,
        headers,
        coreSrc,
      );
      // Every violation aborts the run: the tests fail on any.
      const env = { RTSAN_OPTIONS: 'halt_on_error=true' };
      run('rtsan', rtsan, env);
      probeRtsan(rtsan, env);
    }
  }
  if (!skipTsan) {
    if (!works(compiler, ['-fsanitize=thread'])) {
      process.stdout.write('tsan: the compiler lacks it, skipped\n');
      return;
    }
    const tsan = build(
      'tsan',
      compiler,
      ['-fsanitize=thread'],
      sources,
      headers,
      coreSrc,
    );
    run('tsan', tsan, { TSAN_OPTIONS: 'halt_on_error=1:second_deadlock_stack=1' });
  }
}

main();
