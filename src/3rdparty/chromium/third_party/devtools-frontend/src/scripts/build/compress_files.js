// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const fs = require('fs');
const crypto = require('crypto');
const zlib = require('zlib');
const {pipeline, Readable} = require('stream');

const {promises: pfs} = fs;

// The hash is only used to detect unchanged inputs, so it need not be
// cryptographic. Some Node builds (e.g. on OS/2) lack OpenSSL digests and
// throw "Digest method not supported"; fall back to FNV-1a in that case.
function fnv1a(data) {
  let h1 = 0x811c9dc5;
  let h2 = 0x01000193;
  for (let i = 0; i < data.length; i++) {
    h1 = Math.imul(h1 ^ data[i], 0x01000193) >>> 0;
    h2 = Math.imul(h2 ^ data[i], 0x811c9dc5) >>> 0;
  }
  return 'fnv-' + h1.toString(16).padStart(8, '0') +
      h2.toString(16).padStart(8, '0') + '-' + data.length.toString(16);
}

function sha1(data) {
  try {
    return crypto.createHash('sha1').update(data, 'binary').digest('hex');
  } catch (e) {
    return fnv1a(data);
  }
}

async function readTextFile(filename) {
  return pfs.readFile(filename, 'utf8');
}

function fileExists(filename) {
  return fs.existsSync(filename);
}

async function writeTextFile(filename, data) {
  return pfs.writeFile(filename, data, 'utf8');
}

async function readBinaryFile(filename) {
  return pfs.readFile(filename);
}

async function brotli(sourceData, compressedFilename, brotliOptions) {
  const sizeBytes = sourceData.length;

  // This replicates the following compression logic:
  // https://source.chromium.org/chromium/chromium/src/+/main:tools/grit/grit/node/base.py;l=649;drc=84ef659584d3beb83b44cc168d02244dbd6b8f87
  const array = new BigUint64Array(1);
  // The length of the uncompressed data as 8 bytes little-endian.
  new DataView(array.buffer).setBigUint64(0, BigInt(sizeBytes), true);

  // BROTLI_CONST is prepended to brotli compressed data in order to
  // easily check if a resource has been brotli compressed.
  // It should be kept in sync with https://source.chromium.org/chromium/chromium/src/+/main:tools/grit/grit/constants.py;l=25;drc=84ef659584d3beb83b44cc168d02244dbd6b8f87.
  const brotliConst = new Uint8Array(2);
  brotliConst[0] = 0x1E;
  brotliConst[1] = 0x9B;

  // The length of the uncompressed data is also appended to the start,
  // truncated to 6 bytes, little-endian.
  const sizeHeader = new Uint8Array(array.buffer).slice(0, 6).buffer;
  const output = fs.createWriteStream(compressedFilename);
  output.write(Buffer.from(brotliConst));
  output.write(Buffer.from(sizeHeader));
  return new Promise((resolve, reject) => {
    pipeline(Readable.from(sourceData), zlib.createBrotliCompress(brotliOptions), output, err => {
      return err ? reject(err) : resolve();
    });
  });
}

// The default encoder (quality 11, 4 MB window) needs a lot of memory. If it
// fails (ERR_BROTLI_COMPRESSION_FAILED, seen on 32-bit OS/2), retry with
// cheaper settings; any valid brotli stream decompresses the same way.
const BROTLI_FALLBACKS = [
  undefined,
  {params: {[zlib.constants.BROTLI_PARAM_QUALITY]: 9,
            [zlib.constants.BROTLI_PARAM_LGWIN]: 20}},
  {params: {[zlib.constants.BROTLI_PARAM_QUALITY]: 5,
            [zlib.constants.BROTLI_PARAM_LGWIN]: 16}},
];

async function brotliWithFallback(sourceData, compressedFilename) {
  let lastError;
  for (const options of BROTLI_FALLBACKS) {
    try {
      await brotli(sourceData, compressedFilename, options);
      return;
    } catch (err) {
      lastError = err;
    }
  }
  throw lastError;
}

async function compressFile(filename) {
  const compressedFilename = filename + '.compressed';
  const hashFilename = filename + '.hash';
  const prevHash = fileExists(hashFilename) ? await readTextFile(hashFilename) : '';
  const sourceData = await readBinaryFile(filename);
  const currHash = sha1(sourceData);
  // Also recompress if the output is missing, and only record the hash once
  // compression has finished, so an interrupted run can't leave a matching
  // .hash next to a missing or truncated .compressed file.
  if (prevHash !== currHash || !fileExists(compressedFilename)) {
    await brotliWithFallback(sourceData, compressedFilename);
    await writeTextFile(hashFilename, currHash);
  }
}

// Compressing every file at once runs one brotli encoder per file at the same
// time. On 32-bit OS/2 that exhausts the process's memory: node dies in
// native code (SIGABRT or a silent exit code 1) somewhere between 20 and 50
// concurrent encoders. Compress a few files at a time instead.
const MAX_CONCURRENT = 8;

async function main(argv) {
  const fileListPosition = argv.indexOf('--file_list');
  const fileList = argv[fileListPosition + 1];
  const fileListContents = await readTextFile(fileList);
  const files = fileListContents.split(' ').map(filename => filename.trim());
  let next = 0;
  async function worker() {
    while (next < files.length) {
      await compressFile(files[next++]);
    }
  }
  await Promise.all(Array.from({length: Math.min(MAX_CONCURRENT, files.length)}, worker));
}

main(process.argv).catch(err => {
  console.log('compress_files.js failure', err);
  process.exit(1);
});
