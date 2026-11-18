"""Check physical C++ source limits, including code inside formatter exclusions."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ctags", default="ctags", help="Universal Ctags with JSON support")
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    errors = []
    for path in args.files:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if len(line.expandtabs(2)) > 100:
                errors.append(f"{path}:{number}: line exceeds 100 columns")

    result = subprocess.run(
        [args.ctags, "--options=NONE", "--output-format=json", "--fields=+ne",
         "--kinds-C++=f", "-o", "-", *args.files],
        check=True, stdout=subprocess.PIPE, text=True,
    )
    for line in result.stdout.splitlines():
        tag = json.loads(line)
        if tag.get("kind") == "function":
            length = tag["end"] - tag["line"] + 1
            if length > 70:
                errors.append(f"{tag['path']}:{tag['line']}: {tag['name']} spans {length} lines")
    if errors:
        raise SystemExit("\n".join(errors))


if __name__ == "__main__":
    main()
