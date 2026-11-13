#!/usr/bin/env python
# Copyright 2017 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from os import path as os_path
import errno
import platform
import random
import subprocess
import sys
import os
import time

def which(cmd):
    pathenv = os.getenv('PATH')
    for p in pathenv.split(os_path.pathsep):
        p = os_path.join(p, cmd)
        if os_path.exists(p) and os.access(p, os.X_OK):
            return p
    return None

def GetBinaryPath():
  if (sys.platform == 'win32' or sys.platform == 'os2knix'):
    nodejs = which('node.exe')
    if nodejs:
      return nodejs
  else:
    nodejs = which('nodejs')
    if nodejs:
      return nodejs
    nodejs = which('node')
    if nodejs:
      return nodejs

  darwin_name = ('node-darwin-arm64' if platform.machine() == 'arm64' else
                 'node-darwin-x64')
  return os_path.join(os_path.dirname(__file__), *{
    'Darwin': ('mac', darwin_name, 'bin', 'node'),
    'Linux': ('linux', 'node-linux-x64', 'bin', 'node'),
    'Windows': ('win', 'node.exe'),
  }[platform.system()])


def _StartProcess(cmd):
  return subprocess.Popen(
      cmd, cwd=os.getcwd(), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
      universal_newlines=True)


def _StartProcessOS2(cmd):
  # On OS/2 the loader can't start a second instance of an EXE whose code
  # object is larger than somewhere between 32 and 64 MB while another
  # instance is running ("The segment is in use by another process"), and
  # node.exe 24 is ~96 MB. kLIBC reports this as EBUSY. Until node.exe is
  # split into a small EXE and a DLL, wait for the other node.exe to exit,
  # which in effect runs node one at a time. NODE_OS2_BUSY_TIMEOUT (seconds,
  # default 1800) limits the wait.
  timeout = float(os.environ.get('NODE_OS2_BUSY_TIMEOUT', '1800'))
  deadline = time.time() + timeout
  delay = 0.1
  while True:
    try:
      return _StartProcess(cmd)
    except OSError as e:
      if e.errno != errno.EBUSY or time.time() >= deadline:
        raise
    time.sleep(delay + random.uniform(0, delay))
    delay = min(delay * 2, 2.0)


def RunNode(cmd_parts, stdout=None):
  cmd = [GetBinaryPath()] + cmd_parts
  if sys.platform == 'os2knix':
    process = _StartProcessOS2(cmd)
  else:
    process = _StartProcess(cmd)
  stdout, stderr = process.communicate()

  if process.returncode != 0:
    # Handle cases where stderr is empty, even though the command failed, for
    # example https://github.com/microsoft/TypeScript/issues/615
    err = stderr if len(stderr) > 0 else stdout
    raise RuntimeError('Command \'%s\' failed\n%s' % (' '.join(cmd), err))

  return stdout

if __name__ == '__main__':
  RunNode(sys.argv[1:])
