import subprocess
import os
import sys

def test_host_suite():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
    exe = os.path.join(root, "build_host", "host_tests.exe")
    assert os.path.exists(exe), f"Host tests executable not found at {exe}"
    result = subprocess.run([exe], capture_output=True, text=True)
    print(result.stdout)
    if result.stderr:
        print(result.stderr, file=sys.stderr)
    assert result.returncode == 0, f"Host tests failed with return code {result.returncode}"
    assert "PASS: 3430 assertions" in result.stdout or "assertions" in result.stdout
