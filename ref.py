import os
import re

# Regeln für die Ersetzung
# 1. Modulnamen: SC.Threading -> sc.threading
# 2. Namespaces: SC::Threading -> sc::threading
replacements = [
    (
        r"export module SC\.([a-zA-Z0-9\.]*)",
        lambda m: f"export module sc.{m.group(1).lower()}",
    ),
    (r"import :([A-Za-z]+)", lambda m: f"import :{m.group(1).lower()}"),
    # 2. Namespace Definitionen (erfasst namespace SC::Threading {)
    (
        r"namespace SC::([a-zA-Z0-9_:]*)",
        lambda m: f"namespace sc::{m.group(1).lower()}",
    ),
    # 3. Tiefere Pfade (erfasst SC::Threading::Impl -> sc::threading::impl)
    (r"SC::([a-zA-Z0-9_:]*)", lambda m: f"sc::{m.group(1).lower()}"),
]


def refactor_file(filepath):
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()

    new_content = content
    for pattern, replacement in replacements:
        if callable(replacement):
            new_content = re.sub(pattern, replacement, new_content)
        else:
            new_content = re.sub(pattern, replacement, new_content)

    if new_content != content:
        with open(filepath, "w", encoding="utf-8") as f:
            f.write(new_content)
        print(f"Refactored: {filepath}")


def run_refactor(root_dir):
    extensions = (".cpp", ".cppm", ".hpp", ".h")
    for root, _, files in os.walk(root_dir):
        for file in files:
            if file.endswith(extensions):
                refactor_file(os.path.join(root, file))


if __name__ == "__main__":
    run_refactor("src")
