#!/usr/bin/env python3
"""Check Lean proofs and replay the model's traces through MEX."""

import argparse
import json
from itertools import zip_longest
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent


def run(command, stdin=None):
    result = subprocess.run(command, cwd=ROOT, input=stdin, capture_output=True,
                            text=True, timeout=60)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise ValueError(f"{command[0]} exited with {result.returncode}\n{detail}")
    return result.stdout


def verify(binary, label, inputs, expected):
    try:
        observed = run([binary], "\n".join(inputs) + "\n").splitlines()
    except ValueError as error:
        raise ValueError(f"{label}: {error}") from error
    for step, (want, got) in enumerate(zip_longest(expected, observed), 1):
        if want != got:
            context = ""
            if len(inputs) == len(expected) + 1 and step < len(inputs):
                context = f", input {inputs[step]}"
            raise ValueError(f"{label}, output {step}{context}\n"
                             f"expected: {want}\nobserved: {got}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lean", required=True, type=Path)
    parser.add_argument("--matching", required=True, type=Path)
    parser.add_argument("--depth", required=True, type=Path)
    args = parser.parse_args()
    version = (ROOT / "lean-toolchain").read_text().strip().split(":v", 1)[1]
    lean = args.lean.absolute()
    if not run([lean, "--version"]).startswith(f"Lean (version {version},"):
        raise ValueError(f"Use Lean {version} from lean-toolchain")
    report = run([lean, "--run", "Model.lean"])
    matching = depth = commands = 0
    for line in report.splitlines():
        kind, (label, (inputs, expected)) = json.loads(line)
        if kind == "matching":
            binary = args.matching
            matching += 1
            commands += len(expected)
        elif kind == "depth":
            binary = args.depth
            depth += 1
        else:
            raise ValueError(f"Unexpected trace kind: {kind}")
        verify(binary.resolve(), label, inputs, expected)
    if not matching or not depth:
        raise ValueError("Lean must generate both matching and depth traces")
    print(f"Lean proofs passed. MEX matched {commands} commands in {matching} matching traces "
          f"and {depth} market depth traces.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
