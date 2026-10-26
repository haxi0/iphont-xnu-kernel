import os
import shutil
import subprocess
import tempfile
import unittest
from iphont import device


class NativeProtocolTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'C compiler unavailable; native protocol fixture not checked')
    def test_compiled_loader_protocol(self):
        with tempfile.TemporaryDirectory(prefix='iphont-native-') as tmp:
            exe = os.path.join(tmp, 'protocol')
            source = device.path('tools/iphont/tests/loader_protocol.c')
            compile_result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', source, '-o', exe], capture_output=True, text=True, timeout=30)
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            result = subprocess.run([exe], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
