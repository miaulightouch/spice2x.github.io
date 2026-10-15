#!/usr/bin/env python3
"""Compile production control API and privilege helper against inert Windows adapters."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ADAPTERS = {
    'api/module.cpp': ['util/logging.h'],
    'api/request.cpp': ['../util/logging.h'],
    'api/response.cpp': ['util/logging.h'],
    'api/modules/control.cpp': ['launcher/shutdown.h', 'util/logging.h', 'util/crypt.h', 'util/utils.h'],
}
with tempfile.TemporaryDirectory(prefix='spice-control-test-') as tmp:
    temp = Path(tmp)
    sources = []
    for name, headers in ADAPTERS.items():
        content = (ROOT / name).read_text()
        for header in headers:
            original = '#include "' + header + '"'
            assert original in content, original
            content = content.replace(original, '#include "control_portable.h"')
        content = content.replace('crypt::', 'control_test_crypto::')
        source = temp / name.replace('/', '_')
        source.write_text(content)
        sources.append(str(source))
    utility = (ROOT / 'util/utils.cpp').read_text()
    start = utility.index('bool acquire_shutdown_privs() {')
    end = utility.index('\nvoid generate_ea_card', start)
    helper = temp / 'shutdown_privilege.cpp'
    helper.write_text('#include "control_portable.h"\n' + utility[start:end])
    executable = temp / 'test'
    command = [os.getenv('CXX', 'g++'), '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-Wno-class-memaccess',
               '-DRAPIDJSON_HAS_STDSTRING', '-DFMT_HEADER_ONLY',
               '-I'+str(ROOT), '-I'+str(ROOT / 'api'), '-I'+str(ROOT / 'api/modules'), '-I'+str(ROOT / 'api/tests'),
               *sources, str(helper), str(ROOT / 'api/tests/control_test.cpp'), '-o', str(executable)]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
