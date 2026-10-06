#!/usr/bin/env python3
"""
Run the TAP-Jig routine from the command line (no GUI) - same engine as the
Production tab. Exit status 0 = PASS, 1 = FAIL/ERROR, 2 = could not start.

    python tapjig_run.py --port COM7 --test-hex test.hex --production-hex prod.hex --tape-width 12
    python tapjig_run.py --port /dev/pts/5 --dry-isp --tape-width 12      # against sim_jig.py
"""
import argparse
import json
import os
import sys

import tapjig_engine as eng

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="the jig's USB-CDC port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--routine", default=os.path.join(HERE, "tapjig_routine_feeder_v03.json"))
    ap.add_argument("--test-hex", default="")
    ap.add_argument("--production-hex", default="")
    ap.add_argument("--avrdude", default="avrdude")
    ap.add_argument("--dry-isp", action="store_true", help="skip avrdude (bench use with sim_jig.py)")
    ap.add_argument("--tape-width", type=int, required=True, help="EIA-481 tape width in mm for this unit (8/12/16/24/32/44/56)")
    ap.add_argument("--operator", default="")
    ap.add_argument("--log-dir", default=os.path.join(HERE, "production_logs"))
    ap.add_argument("--stages", default="", help="comma separated stage ids to run (default: all)")
    ap.add_argument("--continue-on-fail", action="store_true")
    args = ap.parse_args()

    try:
        routine = eng.load_routine(args.routine)
        jig = eng.JigLink.open(args.port, args.baud)
        hello = jig.cmd("HELLO")
    except Exception as exc:
        print(f"cannot start: {exc}", file=sys.stderr)
        return 2
    print(f"jig: {dict(hello)}")
    isp = eng.Isp(routine.get("isp", {}), {"test": args.test_hex, "production": args.production_hex},
                  avrdude=args.avrdude, dry_run=args.dry_isp, log=lambda s: print("   " + s))
    ids = {int(x) for x in args.stages.split(",") if x.strip()} or None
    runner = eng.Runner(routine, jig, isp, {"tape_width_mm": args.tape_width, "operator": args.operator},
                        on_stage=lambda sid, status, detail: print(f"  stage {sid:>2}: {status:<8} {detail}") if status not in ("pending", "running") else None)
    result = runner.run(ids, stop_on_fail=not args.continue_on_fail)
    path = eng.write_log(result, args.log_dir)
    print(f"\n{'PASS' if result['passed'] else 'FAIL'}  serial={result['serial'] or '-'}  {result['seconds']}s  log: {path}")
    jig.close()
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
