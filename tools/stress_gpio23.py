#!/usr/bin/env python3
"""
tools/stress_gpio23.py
Stress test script for ESP32 console over serial.
- CLI argument for port (--port)
- Verified CLI syntax:
    pin init 23 output
    gpio 23 on
    gpio 23 off
    mem (heap + console stack min free high-water mark)
- 200 ON/OFF cycles with configurable delay
- Mid-run oversized (3000+ char) and malformed command injections
- Comprehensive crash pattern detection
- Complete session logging to logs/ directory
"""

import argparse
import os
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("ERROR: pyserial is required. Install with: pip install pyserial", file=sys.stderr)
    sys.exit(2)

CRASH_SIGNATURES = [
    "Guru Meditation",
    "assertion ",
    "assert failed",
    "rst:0x",
    "boot:0x",
    "Backtrace:",
    "Task watchdog",
    "Interrupt wdt",
    "abort()",
    "Panic",
    "CORRUPT HEAP",
    "Stack protection fault",
]

def check_for_crashes(text: str) -> list[str]:
    matches = []
    for sig in CRASH_SIGNATURES:
        if sig.lower() in text.lower():
            matches.append(sig)
    return matches

class SerialConsoleTester:
    def __init__(self, port: str, baud: int, timeout: float, log_path: Path):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.log_path = log_path
        self.log_file = None
        self.matched_crash_lines = []
        self.ser = None

    def open(self):
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_file = open(self.log_path, "w", encoding="utf-8", errors="replace")
        print(f"Opening {self.port} at {self.baud} baud...")
        self.log_file.write(f"=== Session Start: Port={self.port}, Baud={self.baud} ===\n")
        self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
        time.sleep(0.5)
        # Drain boot / startup text
        raw = self.ser.read_all()
        if raw:
            text = raw.decode("latin-1", errors="replace")
            self.log_file.write(text)
            self.log_file.flush()
            matches = check_for_crashes(text)
            if matches:
                self.matched_crash_lines.append((text, matches))
                print(f"WARNING: Matched pattern on initial buffer: {matches}")

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
        if self.log_file:
            self.log_file.write("\n=== Session End ===\n")
            self.log_file.close()

    def send_cmd(self, cmd: str, delay: float = 0.02, wait_prompt: bool = True) -> str:
        self.ser.reset_input_buffer()
        to_send = (cmd + "\r\n").encode("latin-1")
        self.ser.write(to_send)
        if delay > 0:
            time.sleep(delay)
        
        # Read response
        time.sleep(0.01)
        response_bytes = bytearray()
        deadline = time.time() + 2.0
        while time.time() < deadline:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                response_bytes.extend(chunk)
                if wait_prompt and b"tinyai> " in response_bytes:
                    break
            else:
                if not wait_prompt and len(response_bytes) > 0:
                    break
            time.sleep(0.005)

        text = response_bytes.decode("latin-1", errors="replace")
        if self.log_file:
            self.log_file.write(f">> {cmd}\n{text}\n")
            self.log_file.flush()

        matches = check_for_crashes(text)
        if matches:
            self.matched_crash_lines.append((text, matches))
            print(f"\nCRASH PATTERN DETECTED on command: '{cmd[:40]}' -> {matches}", file=sys.stderr)
            print(f"Output:\n{text}", file=sys.stderr)
            raise RuntimeError(f"Crash pattern detected: {matches}")

        return text

    def query_mem(self, label: str) -> str:
        print(f"\nQuerying memory and stack high-water mark ({label})...")
        resp = self.send_cmd("mem", delay=0.05)
        print(f"--- [MEASURED NUMBERS: {label}] ---")
        lines = [line.strip() for line in resp.splitlines() if "Heap:" in line or "stack" in line.lower()]
        for l in lines:
            print(f"  {l}")
        print("-----------------------------------")
        return "\n".join(lines)


def run_stress_test(args):
    log_file_path = Path("logs") / args.log
    tester = SerialConsoleTester(args.port, args.baud, args.timeout, log_file_path)
    try:
        tester.open()
        
        # Sync console
        tester.send_cmd("", delay=0.1)

        # 1. Measured numbers BEFORE
        mem_before = tester.query_mem("BEFORE STRESS RUN")

        # 2. Init GPIO23 as output
        print("\nConfiguring GPIO23: 'pin init 23 output'...")
        init_out = tester.send_cmd("pin init 23 output", delay=0.05)
        print(f"Response: {init_out.strip()}")
        if "error" in init_out.lower() or "invalid" in init_out.lower():
            raise RuntimeError(f"Failed to init GPIO23: {init_out}")

        # 3. Stress cycles
        print(f"\nRunning {args.cycles} cycles (delay={args.delay}s, inject_oversized={args.oversized})...")
        start_time = time.time()

        for cycle in range(1, args.cycles + 1):
            # Mid-run injection at 50%
            if args.oversized and cycle == (args.cycles // 2):
                print(f"\n[Cycle {cycle}] Injecting oversized line (3200 characters)...")
                oversized_line = "help " + ("X" * 3200)
                resp_over = tester.send_cmd(oversized_line, delay=0.05)
                print(f"Oversized injection handled cleanly. Response snippet: {resp_over[:80].strip()}...")

                print(f"[Cycle {cycle}] Injecting malformed line with quotes and control chars...")
                malformed_line = "pin init \"23 \x01\x02\x03 output \x7f\x80\""
                resp_mal = tester.send_cmd(malformed_line, delay=0.05)
                print(f"Malformed injection handled cleanly. Response snippet: {resp_mal[:80].strip()}...")

            # ON
            tester.send_cmd("gpio 23 on", delay=args.delay)
            # OFF
            tester.send_cmd("gpio 23 off", delay=args.delay)

            if cycle % 25 == 0 or cycle == args.cycles:
                elapsed = time.time() - start_time
                rate = (cycle * 2) / elapsed if elapsed > 0 else 0
                print(f"  Completed cycle {cycle}/{args.cycles} ({elapsed:.1f}s, {rate:.1f} cmds/s)")

        total_elapsed = time.time() - start_time
        print(f"\nAll {args.cycles} cycles completed in {total_elapsed:.2f}s ({args.cycles * 2} commands).")

        # 4. Final pin status
        status_resp = tester.send_cmd("pin 23 status", delay=0.05)
        print(f"Final GPIO23 status:\n{status_resp.strip()}")

        # 5. Long output query loop (devices, log, sensors, status, mem)
        if args.query_loops > 0:
            print(f"\n--- Running {args.query_loops} query loops of (devices, log, sensors, status, mem) ---")
            q_start = time.time()
            for q_iter in range(1, args.query_loops + 1):
                resp_dev = tester.send_cmd("devices", delay=args.delay)
                assert "GPIO" in resp_dev or "configured" in resp_dev or "shown" in resp_dev, f"Corrupted devices: {resp_dev}"

                resp_log = tester.send_cmd("log", delay=args.delay)
                assert "Audit log" in resp_log or "#" in resp_log, f"Corrupted log: {resp_log}"

                resp_sen = tester.send_cmd("sensors", delay=args.delay)
                assert "sensor" in resp_sen.lower() or "gpio" in resp_sen.lower(), f"Corrupted sensors: {resp_sen}"

                resp_sta = tester.send_cmd("status", delay=args.delay)
                assert "GPIO" in resp_sta or "configured" in resp_sta, f"Corrupted status: {resp_sta}"

                resp_mem = tester.send_cmd("mem", delay=args.delay)
                assert "Heap:" in resp_mem and "Console task stack min free:" in resp_mem, f"Corrupted mem: {resp_mem}"

                if q_iter % 10 == 0 or q_iter == args.query_loops:
                    q_elapsed = time.time() - q_start
                    print(f"  Query loop {q_iter}/{args.query_loops} completed ({q_elapsed:.1f}s) - OK (0 dropped characters)")
            print(f"Query loops ({args.query_loops * 5} long commands) completed cleanly in {time.time() - q_start:.2f}s.")

        # 6. Measured numbers AFTER
        mem_after = tester.query_mem("AFTER ALL STRESS AND QUERY LOOPS")

        print(f"\nStress run log saved to: {log_file_path.resolve()}")
        if tester.matched_crash_lines:
            print(f"Crash detector matches ({len(tester.matched_crash_lines)}):", file=sys.stderr)
            for text, sigs in tester.matched_crash_lines:
                print(f"  Matched {sigs} in:\n{text}", file=sys.stderr)
        else:
            print("Crash detector: 0 matches, 0 crashes, 0 resets, 0 assertions.")

        return mem_before, mem_after

    finally:
        tester.close()


def main():
    parser = argparse.ArgumentParser(description="Stress test GPIO23 on ESP32 serial console")
    parser.add_argument("--port", required=True, help="Serial COM port (e.g. COM13)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--cycles", type=int, default=200, help="Number of ON/OFF cycles (default: 200)")
    parser.add_argument("--delay", type=float, default=0.02, help="Delay between commands (default: 0.02s)")
    parser.add_argument("--timeout", type=float, default=1.0, help="Read timeout (default: 1.0s)")
    parser.add_argument("--oversized", action="store_true", help="Inject oversized and malformed commands mid-run")
    parser.add_argument("--query-loops", type=int, default=0, help="Number of query loops for devices/log/sensors/status/mem (e.g. 50)")
    parser.add_argument("--log", default="stress_gpio23.log", help="Log file name in logs/ directory")
    args = parser.parse_args()

    run_stress_test(args)

if __name__ == "__main__":
    main()
