#!/usr/bin/env python3
"""Compile the production buttons module against inert API/input adapters."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'api/modules/buttons.cpp').read_text()
source = source.replace('#include "buttons.h"', '#include "buttons_portable.h"')
for include in ('misc/eamuse.h', 'cfg/button.h', 'launcher/launcher.h', 'games/io.h',
                'util/utils.h', 'acio/mdxf/mdxf_poll.h'):
    source = source.replace(f'#include "{include}"', '')

with tempfile.TemporaryDirectory(prefix='spice-buttons-') as directory:
    temp = Path(directory)
    (temp / 'buttons.cpp').write_text(source)
    (temp / 'buttons_portable.h').write_text((ROOT / 'api/tests/buttons_portable.h').read_text())
    (temp / 'buttons_test.cpp').write_text((ROOT / 'api/tests/buttons_test.cpp').read_text())
    executable = temp / 'test'
    command = [os.getenv('CXX', 'g++'), '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-Wno-class-memaccess',
               '-I' + str(ROOT), '-I' + str(temp),
               str(temp / 'buttons.cpp'), str(temp / 'buttons_test.cpp'), '-o', str(executable)]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
