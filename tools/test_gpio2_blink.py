#!/usr/bin/env python3
"""
tools/test_gpio2_blink.py
Interactive/automated test for GPIO2 scoped authorization and timed blink.
Tests:
1. Denied blink without authorization.
2. Scoped session authorization grant ('gpio authorize 2').
3. Timed blink execution ('gpio blink 2 2000').
4. Awaiting timer completion notification ('Execution: completed').
5. Revoking session authorization ('gpio revoke 2').
6. Re-verifying denied status after revoke.
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("ERROR: pyserial is required.", file=sys.stderr)
    sys.exit(2)

def main():
    parser = argparse.ArgumentParser(description="Test GPIO2 timed blink and authorization")
    parser.add_argument("--port", default="COM13", help="Serial port (default: COM13)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--duration", type=int, default=2000, help="Blink duration in ms (default: 2000)")
    args = parser.parse_args()

    print(f"Opening {args.port} at {args.baud} baud...")
    ser = serial.Serial(args.port, args.baud, timeout=1.0)
    time.sleep(0.5)
    ser.read_all()

    def send_and_wait(cmd: str, wait_time: float = 0.5) -> str:
        ser.reset_input_buffer()
        print(f"\n>> {cmd}")
        ser.write((cmd + "\r\n").encode("latin-1"))
        time.sleep(wait_time)
        res = bytearray()
        while ser.in_waiting:
            res.extend(ser.read(ser.in_waiting))
            time.sleep(0.02)
        txt = res.decode("latin-1", errors="replace").strip()
        print(txt)
        return txt

    print("\n--- STEP 1: Attempt blink on GPIO2 WITHOUT authorization (Expect Denied) ---")
    resp1 = send_and_wait("gpio blink 2 1000", wait_time=0.3)
    assert "denied" in resp1.lower(), "Expected validation: denied"

    print("\n--- STEP 2: Grant session authorization for GPIO2 ---")
    resp2 = send_and_wait("gpio authorize 2", wait_time=0.3)
    assert "authorization granted" in resp2.lower(), "Expected authorization granted"

    print(f"\n--- STEP 3: Execute timed blink on GPIO2 for {args.duration} ms (Watch the LED!) ---")
    resp3 = send_and_wait(f"gpio blink 2 {args.duration}", wait_time=0.3)
    assert "approved" in resp3.lower() and "started" in resp3.lower(), "Expected approved and started"

    print(f"Waiting for timer completion ({args.duration / 1000.0 + 0.5}s)...")
    time.sleep(args.duration / 1000.0 + 0.5)
    # Read completion output
    res = bytearray()
    while ser.in_waiting:
        res.extend(ser.read(ser.in_waiting))
        time.sleep(0.02)
    completion_text = res.decode("latin-1", errors="replace").strip()
    print(f"Completion output: {completion_text}")
    assert "completed" in completion_text.lower(), "Expected 'Execution: completed'"

    print("\n--- STEP 4: Revoke session authorization for GPIO2 ---")
    resp4 = send_and_wait("gpio revoke 2", wait_time=0.3)
    assert "revoked" in resp4.lower(), "Expected authorization revoked"

    print("\n--- STEP 5: Verify blink is denied again after revoke ---")
    resp5 = send_and_wait("gpio blink 2 1000", wait_time=0.3)
    assert "denied" in resp5.lower(), "Expected validation: denied after revoke"

    print("\nSUCCESS: All GPIO2 scoped authorization and timed blink checks PASSED.")
    ser.close()

if __name__ == "__main__":
    main()
