"""Symbol-hygiene scenarios for highlightctx.

A shared library that defines an STB_GNU_UNIQUE symbol which nothing else in
the process defines is marked NODELETE by glibc: dlclose() silently does
nothing, so `/znc updatemod` reloads the already-resident old code while
reporting success, and the module only really updates on a full ZNC restart.

libstdc++ emits such symbols for some inline internals — notably
std::__detail::__to_chars_10_impl<...>::__digits, reached through
std::to_string on GCC 11 — so this can reappear from an innocuous-looking
source change or a compiler upgrade. These scenarios fail the build if it does.

Imported by suite.py; every function follows the suite's check() convention.
"""
import os
import shutil
import subprocess


def install(ns):
    check = ns["check"]
    V090 = ns["V090"]          # module under test
    V080 = ns["V080"]          # baseline
    ZNC_BIN = ns["ZNC_BIN"]

    def unique_syms(path):
        """STB_GNU_UNIQUE symbols defined by an ELF object ('u' in nm -D)."""
        if not shutil.which("nm"):
            return None
        out = subprocess.run(["nm", "-D", path], capture_output=True, text=True)
        if out.returncode != 0:
            return None
        syms = set()
        for line in out.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[1] == "u":
                syms.add(parts[2])
            elif len(parts) == 2 and parts[0] == "u":
                syms.add(parts[1])
        return syms

    def demangle(syms):
        if not syms or not shutil.which("c++filt"):
            return sorted(syms or [])
        out = subprocess.run(["c++filt"], input="\n".join(sorted(syms)),
                             capture_output=True, text=True)
        return out.stdout.splitlines() if out.returncode == 0 else sorted(syms)

    def sym_module_is_unloadable():
        """The module must not define a unique symbol that ZNC does not.

        Such a symbol pins the library in memory and breaks in-place updates.
        Symbols the ZNC binary also defines are harmless: the module's
        reference binds to ZNC's copy, so the unique-symbol path never marks
        the module itself.
        """
        scn = "sym_module_is_unloadable"
        mod = unique_syms(V090)
        znc = unique_syms(ZNC_BIN)
        if mod is None or znc is None:
            check(scn, "nm available to inspect symbols", False, "nm/binutils missing or unreadable binary")
            return
        pinning = mod - znc
        print("    info: module unique syms=%d, znc unique syms=%d, pinning=%d" % (len(mod), len(znc), len(pinning)))
        check(scn, "module defines no unique symbol that ZNC lacks (would break /znc updatemod)",
              not pinning, "pinning symbols:\n      " + "\n      ".join(demangle(pinning)))

    def sym_no_to_string_internals():
        """Guard the specific regression: std::to_string's __digits tables."""
        scn = "sym_no_to_string_internals"
        mod = unique_syms(V090)
        if mod is None:
            check(scn, "nm available", False, "")
            return
        offenders = [s for s in mod if "__to_chars" in s or "piecewise_construct" in s]
        check(scn, "no __to_chars_10_impl/__digits or piecewise_construct unique symbols",
              not offenders, repr(demangle(set(offenders))))

    def sym_baseline_comparison():
        """Report the baseline's symbols for contrast; informational only."""
        scn = "sym_baseline_comparison"
        base = unique_syms(V080)
        mod = unique_syms(V090)
        znc = unique_syms(ZNC_BIN)
        if base is None or mod is None or znc is None:
            return
        print("    info: baseline pinning syms=%d, under-test pinning syms=%d"
              % (len(base - znc), len(mod - znc)))
        check(scn, "under-test build has no more pinning symbols than the baseline",
              len(mod - znc) <= len(base - znc), "%d vs %d" % (len(mod - znc), len(base - znc)))

    ns["SYMBOLS"] = [sym_module_is_unloadable, sym_no_to_string_internals, sym_baseline_comparison]
