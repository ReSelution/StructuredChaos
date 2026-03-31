#!/usr/bin/env python3
import sys
import os
import subprocess
import time
import argparse
from datetime import timedelta

def main():
    parser = argparse.ArgumentParser(description="Chaos Stress Test CLI")
    parser.add_argument("program", help="Pfad zum Executable")
    parser.add_argument("-n", "--runs", type=int, default=100, help="Anzahl der Durchläufe")
    parser.add_argument("-k", "--keep", action="store_true", help="Alle Logs behalten")
    args = parser.parse_args()

    if not os.path.isfile(args.program) or not os.access(args.program, os.X_OK):
        print(f"❌ Fehler: '{args.program}' nicht gefunden oder nicht ausführbar.")
        sys.exit(1)

    log_dir = "./stress_test_logs"
    os.makedirs(log_dir, exist_ok=True)

    print(f"🚀 Starte Stress-Test: {args.program}")
    print(f"🔢 Durchläufe: {args.runs}")
    print("-" * 60)

    total_start = time.perf_counter()
    times = []

    try:
        for i in range(1, args.runs + 1):
            run_start = time.perf_counter()
            log_path = os.path.join(log_dir, f"run_{i}.log")

            # Prozess ausführen
            with open(log_path, "w") as log_file:
                result = subprocess.run([args.program], stdout=log_file, stderr=subprocess.STDOUT)

            run_end = time.perf_counter()
            duration = run_end - run_start
            times.append(duration)

            if result.returncode != 0:
                print(f"\n\n💥 CRASH in Durchlauf {i} (Exit Code: {result.returncode})")
                print("-" * 60)
                with open(log_path, "r") as f:
                    print("".join(f.readlines()[-20:])) # Letzte 20 Zeilen
                sys.exit(1)

            if not args.keep:
                os.remove(log_path)

            # Berechnungen für ETA
            avg_time = sum(times) / len(times)
            remaining_runs = args.runs - i
            eta_seconds = remaining_runs * avg_time
            eta_str = str(timedelta(seconds=int(eta_seconds)))

            # Fortschritts-UI
            sys.stdout.write(
                f"\r[OK] {i}/{args.runs} | Letzter: {duration:.3f}s | Avg: {avg_time:.3f}s | ETA: {eta_str} "
            )
            sys.stdout.flush()

    except KeyboardInterrupt:
        print("\n\n🛑 Test vom User abgebrochen.")
        sys.exit(0)

    total_duration = time.perf_counter() - total_start
    print(f"\n\n✅ Alle {args.runs} Durchläufe erfolgreich!")
    print(f"📊 Gesamtzeit: {timedelta(seconds=int(total_duration))}")
    print(f"⏱️  Schnellster: {min(times):.4f}s | Langsamster: {max(times):.4f}s")

if __name__ == "__main__":
    main()
