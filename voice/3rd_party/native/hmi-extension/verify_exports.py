"""Check the SDK against both its upstream and HMI C declarations."""

from pathlib import Path
import sys


upstream, extension, library = map(Path, sys.argv[1:4])
symbol_tool = sys.argv[4] if len(sys.argv) > 4 else None
kind = {".dll": "pe", ".dylib": "macho"}[library.suffix]
sys.path.insert(0, str(upstream / "tests" / "capi"))
from export_surface import declared_symbols, exported_symbols


expected = declared_symbols(upstream / "include" / "audiocpp.h")
extensions = declared_symbols(extension / "audiocpp_hmi.h")
if not expected or not extensions:
    sys.exit("Could not read both SDK declaration sets")
expected |= extensions
actual = exported_symbols(kind, str(library), symbol_tool)
if actual is None:
    sys.exit("Could not inspect SDK exports")
if expected != actual:
    sys.exit(f"Missing SDK exports: {sorted(expected - actual)}\nUnexpected SDK exports: {sorted(actual - expected)}")
print(f"Verified {len(actual)} SDK exports")
