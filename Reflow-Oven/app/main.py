"""Orion Reflow Oven — PC application entry point.

Run from the app/ directory:
    python main.py

Requirements (install once):
    pip install -r requirements.txt
"""

from __future__ import annotations

import sys
from pathlib import Path

# Ensure the src package is importable when run as `python main.py` from app/
sys.path.insert(0, str(Path(__file__).parent))

from src.gui.app import run

if __name__ == "__main__":
    run()
