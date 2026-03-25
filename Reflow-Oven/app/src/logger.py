"""CSV session logger.

Opens a timestamped CSV file in the configured logs/ directory on construction.
Call log_sample() for every temperature reading, log_event() for discrete events.
"""

from __future__ import annotations

import csv
from datetime import datetime
from pathlib import Path


class SessionLogger:
    COLUMNS = ["timestamp", "uptime_ms", "tc_temp", "ref_temp", "stage_name", "event"]

    def __init__(self, logs_dir: str | Path = "logs"):
        self._logs_dir = Path(logs_dir)
        self._logs_dir.mkdir(parents=True, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        self._path = self._logs_dir / f"session_{ts}.csv"
        self._file = open(self._path, "w", newline="", encoding="utf-8")
        self._writer = csv.DictWriter(self._file, fieldnames=self.COLUMNS)
        self._writer.writeheader()
        self._file.flush()

    @property
    def path(self) -> Path:
        return self._path

    def log_sample(
        self,
        uptime_ms: int,
        tc_temp: float,
        ref_temp: float,
        stage_name: str = "",
        event: str = "",
    ) -> None:
        self._writer.writerow({
            "timestamp":  datetime.now().isoformat(timespec="milliseconds"),
            "uptime_ms":  uptime_ms,
            "tc_temp":    round(tc_temp,  2),
            "ref_temp":   round(ref_temp, 4),
            "stage_name": stage_name,
            "event":      event,
        })
        self._file.flush()

    def log_event(self, uptime_ms: int, event: str, stage_name: str = "") -> None:
        self.log_sample(uptime_ms, 0.0, 0.0, stage_name=stage_name, event=event)

    def close(self) -> None:
        if self._file and not self._file.closed:
            self._file.close()

    def __del__(self) -> None:
        self.close()
