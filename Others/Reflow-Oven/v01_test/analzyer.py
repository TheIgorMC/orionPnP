import argparse
import csv
import datetime as dt
import os
import threading
import sys
import time

import serial
from serial.tools import list_ports


def default_output_path() -> str:
	ts = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
	return os.path.join(os.path.dirname(__file__), f"reflow_log_{ts}.csv")


def parse_data_line(line: str):
	"""
	Parse expected board format: time_ms,temp_C,top,bottom
	Also accepts error lines from firmware: time_ms,ERR,
	"""
	parts = [p.strip() for p in line.split(",")]
	if len(parts) < 2:
		return None

	# Expected data row with 4 columns
	if len(parts) >= 4 and parts[0].isdigit():
		return {
			"device_time_ms": parts[0],
			"temp_C": parts[1],
			"top": parts[2],
			"bottom": parts[3],
			"raw_line": line,
		}

	# Firmware error line: "1234,ERR,"
	if parts[0].isdigit() and parts[1].upper() == "ERR":
		return {
			"device_time_ms": parts[0],
			"temp_C": "ERR",
			"top": "",
			"bottom": "",
			"raw_line": line,
		}

	return None


def find_first_serial_port() -> str:
	ports = list(list_ports.comports())
	if not ports:
		raise RuntimeError("No serial ports found. Connect the board and retry.")
	return ports[0].device


def _port_text(port_info) -> str:
	values = [
		port_info.device or "",
		port_info.description or "",
		getattr(port_info, "manufacturer", "") or "",
		getattr(port_info, "product", "") or "",
		getattr(port_info, "hwid", "") or "",
	]
	return " ".join(values).lower()


def choose_serial_port(port_arg: str | None) -> str:
	if port_arg:
		return port_arg

	ports = list(list_ports.comports())
	if not ports:
		raise RuntimeError("No serial ports found. Connect the board and retry.")

	# Prefer Arduino Pro Micro style ports by keywords in metadata.
	preferred_keywords = ["arduino", "micro"]
	preferred = [p for p in ports if any(k in _port_text(p) for k in preferred_keywords)]

	if len(preferred) == 1:
		p = preferred[0]
		print(f"Auto-selected preferred port: {p.device} ({p.description})")
		return p.device

	if len(preferred) > 1:
		candidates = preferred
		print("Multiple preferred ports found (Arduino/Micro). Select one:")
	else:
		candidates = ports
		print("No Arduino/Micro keywords found. Select a serial port:")

	for idx, p in enumerate(candidates, start=1):
		manufacturer = getattr(p, "manufacturer", "") or "unknown"
		print(f"  [{idx}] {p.device} | {p.description} | {manufacturer}")

	while True:
		choice = input(f"Select port [1-{len(candidates)}] (default 1): ").strip()
		if not choice:
			return candidates[0].device
		if choice.isdigit():
			n = int(choice)
			if 1 <= n <= len(candidates):
				return candidates[n - 1].device
		print("Invalid selection. Try again.")


def command_input_worker(ser: serial.Serial, stop_event: threading.Event) -> None:
	valid_commands = {"A", "X", "T", "B", "S"}
	print("Interactive commands enabled: A=All ON, T=Top, B=Bottom, X=All OFF, S=Status, Q=Quit")
	while not stop_event.is_set():
		try:
			user_in = input("CMD> ").strip().upper()
		except EOFError:
			stop_event.set()
			return

		if not user_in:
			continue

		cmd = user_in[0]
		if cmd == "Q":
			stop_event.set()
			print("Exit requested from console.")
			return

		if cmd not in valid_commands:
			print("Invalid command. Use A, X, T, B, S or Q.")
			continue

		try:
			ser.write(cmd.encode("ascii"))
			ser.flush()
			print(f"# SENT: {cmd}")
		except serial.SerialException as exc:
			print(f"Serial write error: {exc}")
			stop_event.set()
			return


def main() -> int:
	parser = argparse.ArgumentParser(
		description="Capture REFLOW OVEN serial output and save parsed rows to CSV."
	)
	parser.add_argument("--port", help="Serial port (e.g. COM5). If omitted, first port is used.")
	parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate (default: 115200).")
	parser.add_argument("--output", default=default_output_path(), help="Output CSV path.")
	parser.add_argument(
		"--reset-wait",
		type=float,
		default=2.0,
		help="Seconds to wait after toggling DTR reset (default: 2.0).",
	)
	args = parser.parse_args()

	port = choose_serial_port(args.port)
	os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

	print(f"Opening {port} @ {args.baud}...")
	print(f"Saving CSV to: {os.path.abspath(args.output)}")

	with serial.Serial(port, args.baud, timeout=1) as ser, open(
		args.output, "w", newline="", encoding="utf-8"
	) as out_file:
		writer = csv.DictWriter(
			out_file,
			fieldnames=[
				"host_timestamp_iso",
				"device_time_ms",
				"temp_C",
				"top",
				"bottom",
				"raw_line",
			],
		)
		writer.writeheader()

		# Arduino boards typically reset on serial connect. Toggle DTR to start from t=0.
		ser.dtr = False
		time.sleep(0.2)
		ser.reset_input_buffer()
		ser.dtr = True
		time.sleep(args.reset_wait)

		stop_event = threading.Event()
		input_thread = threading.Thread(
			target=command_input_worker,
			args=(ser, stop_event),
			daemon=True,
		)
		input_thread.start()

		print("Listening and logging... Press Ctrl+C or type Q to stop.")
		try:
			while not stop_event.is_set():
				raw = ser.readline()
				if not raw:
					continue

				try:
					line = raw.decode("utf-8", errors="replace").strip()
				except UnicodeDecodeError:
					continue

				if not line:
					continue

				# Show all incoming lines in console for visibility
				print(line)

				# Ignore comments/header from firmware, keep only data rows in CSV
				if line.startswith("#") or line.lower().startswith("time_ms"):
					continue

				row = parse_data_line(line)
				if row is None:
					continue

				row["host_timestamp_iso"] = dt.datetime.now().isoformat(timespec="milliseconds")
				writer.writerow(row)
				out_file.flush()

		except KeyboardInterrupt:
			stop_event.set()
			print("\nStopped by user.")

	print("CSV saved.")
	return 0


if __name__ == "__main__":
	sys.exit(main())
