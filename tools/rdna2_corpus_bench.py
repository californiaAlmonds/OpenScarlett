#!/usr/bin/env python3
"""OpenScarlett GFX10.3 (RDNA2) 779-Opcode Coverage Auditor & Shader Corpus Benchmark CLI.

MIT License - OpenScarlett tooling.

Runs `openscarlett.exe rdna2-bench` to generate & validate the reproducible GFX10.3
shader corpus, verifies `docs/rdna2_coverage.json` schema integrity, prints human-readable
per-family opcode coverage & per-stage/per-workload shader pass-rate tables, and returns
non-zero on any failure.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent

REQUIRED_FAMILIES = [
    "SOP1",
    "SOP2",
    "SOPK",
    "SOPC",
    "SOPP",
    "SMEM",
    "VOP1",
    "VOP2",
    "VOPC",
    "VOP3",
    "VOP3P",
    "VINTRP",
    "MUBUF",
    "MTBUF",
    "MIMG",
    "DS",
    "FLAT",
    "GLOBAL",
    "SCRATCH",
    "EXP",
]

REQUIRED_STAGES = ["CS", "VS", "PS", "MS", "AS"]

REQUIRED_WORKLOADS = [
    "pbr_ibl",
    "cascaded_shadows",
    "deferred_gbuffer",
    "skeletal_skinning",
    "meshlet_culling",
    "clustered_tiled_lighting",
    "postfx_bloom_tonemap_fsr_taa",
    "bvh4_ray_traversal",
    "lds_prefix_sum_radix_sort",
    "wave32_wave64_ballot_permlane",
    "texture_2d_3d_cube_mip_gather_atomics",
    "scratch_register_spilling",
]

EXPECTED_OPCODES_TOTAL = 779


def find_openscarlett_exe(explicit: pathlib.Path | None) -> pathlib.Path | None:
    if explicit is not None:
        return explicit if explicit.exists() else None
    candidates = [
        ROOT / "build" / "openscarlett.exe",
        ROOT / "build" / "Release" / "openscarlett.exe",
        ROOT / "build" / "Debug" / "openscarlett.exe",
        ROOT / "build" / "openscarlett",
    ]
    for cand in candidates:
        if cand.exists():
            return cand
    which = shutil.which("openscarlett.exe") or shutil.which("openscarlett")
    return pathlib.Path(which) if which else None


def pct(num: int, den: int) -> str:
    if den <= 0:
        return "  0.0%"
    return f"{(100.0 * num / den):5.1f}%"


def validate_report_schema(
    data: dict[str, Any],
    *,
    min_pass_rate: float = 100.0,
    min_opcodes: int = 1,
    require_full_corpus: bool = False,
) -> list[str]:
    errors: list[str] = []

    if not isinstance(data, dict):
        return ["Top-level JSON value must be an object"]

    if data.get("schema_version") != 1:
        errors.append(f"Expected schema_version == 1, got {data.get('schema_version')!r}")

    isa_target = data.get("isa_target")
    if not isinstance(isa_target, str) or "GFX10.3" not in isa_target:
        errors.append(f"Expected isa_target containing 'GFX10.3', got {isa_target!r}")

    required_int_keys = [
        "opcodes_total",
        "opcodes_decoded",
        "opcodes_lifted_spirv",
        "opcodes_lifted_dxil",
        "opcodes_cpu_ref",
        "opcodes_gpu_verified",
        "corpus_shaders_total",
        "corpus_shaders_passed",
        "corpus_spirv_validated",
        "spirv_val_external_validated",
        "corpus_dxil_compiled",
        "corpus_gpu_executed",
    ]
    for k in required_int_keys:
        v = data.get(k)
        if not isinstance(v, int) or isinstance(v, bool) or v < 0:
            errors.append(f"Missing or invalid non-negative integer key: {k!r} (got {v!r})")

    if errors:
        return errors

    op_total = int(data["opcodes_total"])
    op_dec = int(data["opcodes_decoded"])
    op_spv = int(data["opcodes_lifted_spirv"])
    op_dxil = int(data["opcodes_lifted_dxil"])
    op_cpu = int(data["opcodes_cpu_ref"])
    op_gpu = int(data["opcodes_gpu_verified"])

    if op_total != EXPECTED_OPCODES_TOTAL:
        errors.append(f"Expected opcodes_total == {EXPECTED_OPCODES_TOTAL}, got {op_total}")
    if op_dec < min_opcodes:
        errors.append(f"opcodes_decoded ({op_dec}) < required minimum ({min_opcodes})")
    for name, val in [
        ("opcodes_decoded", op_dec),
        ("opcodes_lifted_spirv", op_spv),
        ("opcodes_lifted_dxil", op_dxil),
        ("opcodes_cpu_ref", op_cpu),
        ("opcodes_gpu_verified", op_gpu),
    ]:
        if val > op_total:
            errors.append(f"{name} ({val}) exceeds opcodes_total ({op_total})")

    c_total = int(data["corpus_shaders_total"])
    c_passed = int(data["corpus_shaders_passed"])
    if c_total <= 0:
        errors.append(f"corpus_shaders_total must be > 0, got {c_total}")
    if require_full_corpus and c_total < 3000:
        errors.append(f"--full requires corpus_shaders_total >= 3000, got {c_total}")
    if c_passed > c_total:
        errors.append(f"corpus_shaders_passed ({c_passed}) > corpus_shaders_total ({c_total})")
    if c_total > 0:
        rate = 100.0 * c_passed / c_total
        if rate + 1e-9 < min_pass_rate:
            errors.append(f"Corpus pass rate {rate:.2f}% < minimum {min_pass_rate:.2f}%")

    # Validate families
    families = data.get("families")
    if not isinstance(families, dict):
        errors.append("'families' must be an object keyed by encoding name")
    else:
        missing_fams = [f for f in REQUIRED_FAMILIES if f not in families]
        if missing_fams:
            errors.append(f"Missing required encoding families: {missing_fams}")
        fam_total_sum = 0
        fam_dec_sum = 0
        for fname in REQUIRED_FAMILIES:
            entry = families.get(fname)
            if not isinstance(entry, dict):
                continue
            ft = entry.get("total")
            fd = entry.get("decoded")
            if not isinstance(ft, int) or not isinstance(fd, int) or ft < 0 or fd < 0:
                errors.append(f"Invalid counts in family {fname}: {entry!r}")
                continue
            if fd > ft:
                errors.append(f"Family {fname} decoded ({fd}) > total ({ft})")
            fam_total_sum += ft
            fam_dec_sum += fd
        if fam_total_sum != op_total:
            errors.append(f"Sum of family totals ({fam_total_sum}) != opcodes_total ({op_total})")
        if fam_dec_sum != op_dec:
            errors.append(f"Sum of family decoded ({fam_dec_sum}) != opcodes_decoded ({op_dec})")

    # Validate stages
    stages = data.get("stages")
    if not isinstance(stages, dict):
        errors.append("'stages' must be an object keyed by stage name")
    else:
        missing_stages = [s for s in REQUIRED_STAGES if s not in stages]
        if missing_stages:
            errors.append(f"Missing required shader stages: {missing_stages}")
        st_total_sum = 0
        st_pass_sum = 0
        for sname in REQUIRED_STAGES:
            entry = stages.get(sname)
            if not isinstance(entry, dict):
                continue
            st = entry.get("total", -1)
            sp = entry.get("passed", -1)
            if not isinstance(st, int) or not isinstance(sp, int) or st < 0 or sp < 0 or sp > st:
                errors.append(f"Invalid counts in stage {sname}: {entry!r}")
                continue
            st_total_sum += st
            st_pass_sum += sp
        if st_total_sum != c_total:
            errors.append(f"Sum of stage totals ({st_total_sum}) != corpus_shaders_total ({c_total})")
        if st_pass_sum != c_passed:
            errors.append(f"Sum of stage passed ({st_pass_sum}) != corpus_shaders_passed ({c_passed})")

    # Validate workloads
    workloads = data.get("workloads")
    if not isinstance(workloads, dict):
        errors.append("'workloads' must be an object keyed by workload name")
    else:
        missing_wl = [w for w in REQUIRED_WORKLOADS if w not in workloads]
        if missing_wl:
            errors.append(f"Missing required AAA workloads: {missing_wl}")
        wl_total_sum = 0
        wl_pass_sum = 0
        for wname in REQUIRED_WORKLOADS:
            entry = workloads.get(wname)
            if not isinstance(entry, dict):
                continue
            wt = entry.get("total", -1)
            wp = entry.get("passed", -1)
            if not isinstance(wt, int) or not isinstance(wp, int) or wt < 0 or wp < 0 or wp > wt:
                errors.append(f"Invalid counts in workload {wname}: {entry!r}")
                continue
            wl_total_sum += wt
            wl_pass_sum += wp
        if wl_total_sum != c_total:
            errors.append(f"Sum of workload totals ({wl_total_sum}) != corpus_shaders_total ({c_total})")
        if wl_pass_sum != c_passed:
            errors.append(f"Sum of workload passed ({wl_pass_sum}) != corpus_shaders_passed ({c_passed})")

    return errors


def print_report_tables(data: dict[str, Any]) -> None:
    op_total = int(data["opcodes_total"])
    op_dec = int(data["opcodes_decoded"])
    op_spv = int(data["opcodes_lifted_spirv"])
    op_dxil = int(data["opcodes_lifted_dxil"])
    op_cpu = int(data["opcodes_cpu_ref"])
    op_gpu = int(data["opcodes_gpu_verified"])
    c_total = int(data["corpus_shaders_total"])
    c_passed = int(data["corpus_shaders_passed"])

    print("=" * 88)
    print(f"OpenScarlett RDNA2 / GFX10.3 ISA Coverage & Shader Corpus Report (v{data.get('schema_version')})")
    print(f"ISA Target       : {data.get('isa_target')}")
    print(
        f"Opcode Coverage  : {op_dec}/{op_total} decoded ({pct(op_dec, op_total).strip()}) | "
        f"SPIR-V={op_spv} | DXIL={op_dxil} | CPU={op_cpu} | GPU={op_gpu}"
    )
    print(
        f"Shader Corpus    : {c_passed}/{c_total} passed ({pct(c_passed, c_total).strip()}) | "
        f"SPIR-V={data.get('corpus_spirv_validated', 0)} (spirv-val={data.get('spirv_val_external_validated', 0)}) | "
        f"DXIL={data.get('corpus_dxil_compiled', 0)} | GPU={data.get('corpus_gpu_executed', 0)}"
    )
    if "elapsed_seconds" in data:
        print(f"Elapsed Time     : {float(data['elapsed_seconds']):.2f}s")
    print("=" * 88)

    # Per-family table
    print("\n[1] Per-Encoding GFX10.3 Opcode Coverage")
    print(f"{'Family':<10} {'Total':>6} {'Decoded':>8} {'Cov%':>7} {'SPIR-V':>8} {'DXIL':>8} {'CPU-Ref':>8} {'GPU-Ver':>8}")
    print("-" * 71)
    families = data.get("families", {})
    for fname in REQUIRED_FAMILIES:
        f = families.get(fname, {})
        ft = int(f.get("total", 0))
        fd = int(f.get("decoded", 0))
        fs = int(f.get("lifted_spirv", 0))
        fx = int(f.get("lifted_dxil", 0))
        fc = int(f.get("cpu_ref", 0))
        fg = int(f.get("gpu_verified", 0))
        print(f"{fname:<10} {ft:>6} {fd:>8} {pct(fd, ft):>7} {fs:>8} {fx:>8} {fc:>8} {fg:>8}")
    print("-" * 71)
    print(
        f"{'TOTAL':<10} {op_total:>6} {op_dec:>8} {pct(op_dec, op_total):>7} "
        f"{op_spv:>8} {op_dxil:>8} {op_cpu:>8} {op_gpu:>8}"
    )

    # Per-stage table
    print("\n[2] Per-Stage Shader Corpus Validation")
    print(f"{'Stage':<8} {'Total':>7} {'Passed':>7} {'Pass%':>7} {'Disasm':>7} {'SPIR-V':>7} {'DXIL':>7} {'CPU-Ref':>8} {'GPU':>6}")
    print("-" * 73)
    stages = data.get("stages", {})
    for sname in REQUIRED_STAGES:
        s = stages.get(sname, {})
        st = int(s.get("total", 0))
        sp = int(s.get("passed", 0))
        sd = int(s.get("disasm_passed", 0))
        ss = int(s.get("spirv_passed", 0))
        sx = int(s.get("dxil_passed", 0))
        sc = int(s.get("cpu_ref_passed", 0))
        sg = int(s.get("gpu_passed", 0))
        print(f"{sname:<8} {st:>7} {sp:>7} {pct(sp, st):>7} {sd:>7} {ss:>7} {sx:>7} {sc:>8} {sg:>6}")
    print("-" * 73)
    print(
        f"{'TOTAL':<8} {c_total:>7} {c_passed:>7} {pct(c_passed, c_total):>7} "
        f"{c_passed:>7} {int(data.get('corpus_spirv_validated', 0)):>7} "
        f"{int(data.get('corpus_dxil_compiled', 0)):>7} "
        f"{int(data.get('corpus_cpu_ref_executed', 0)):>8} "
        f"{int(data.get('corpus_gpu_executed', 0)):>6}"
    )

    # Per-workload table
    print("\n[3] Per-Workload AAA Archetype Breakdown")
    print(f"{'Workload':<38} {'Total':>6} {'Passed':>7} {'Pass%':>7} {'SPIR-V':>7} {'DXIL':>6} {'CPU':>5} {'GPU':>5}")
    print("-" * 88)
    workloads = data.get("workloads", {})
    for wname in REQUIRED_WORKLOADS:
        w = workloads.get(wname, {})
        wt = int(w.get("total", 0))
        wp = int(w.get("passed", 0))
        ws = int(w.get("spirv_passed", 0))
        wx = int(w.get("dxil_passed", 0))
        wc = int(w.get("cpu_ref_passed", 0))
        wg = int(w.get("gpu_passed", 0))
        print(f"{wname:<38} {wt:>6} {wp:>7} {pct(wp, wt):>7} {ws:>7} {wx:>6} {wc:>5} {wg:>5}")
    print("-" * 88)


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Run OpenScarlett GFX10.3 779-opcode ISA coverage & 3,000+ shader corpus benchmark."
    )
    p.add_argument("--exe", type=pathlib.Path, default=None, help="Path to openscarlett.exe")
    p.add_argument("--json", type=pathlib.Path, default=ROOT / "docs" / "rdna2_coverage.json", help="Output JSON path")
    p.add_argument("--verify-json", type=pathlib.Path, default=None, help="Verify an existing JSON report without running openscarlett.exe")
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--full", action="store_true", help="Run full 3,120+ shader corpus benchmark (default)")
    mode.add_argument("--fast", action="store_true", help="Run fast smoke sweep (~240 shaders)")
    p.add_argument("--corpus-count", type=int, default=None, help="Override total corpus shader count")
    p.add_argument("--external-sample", type=int, default=None, help="Override stratified spirv-val/DXC sample count")
    p.add_argument("--validate-all-external", action="store_true", help="Run spirv-val and dxc.exe on every corpus shader")
    p.add_argument("--no-gpu", action="store_true", help="Skip live D3D12/Vulkan GPU execution tests")
    p.add_argument("--no-external", action="store_true", help="Skip external spirv-val.exe and dxc.exe invocations")
    p.add_argument("--min-pass-rate", type=float, default=100.0, help="Minimum required corpus pass rate percentage")
    p.add_argument("--min-opcodes", type=int, default=1, help="Minimum required decoded opcodes count")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)

    if args.verify_json is not None:
        json_path = args.verify_json
        if not json_path.exists():
            print(f"[ERROR] JSON report not found: {json_path}", file=sys.stderr)
            return 2
        try:
            data = json.loads(json_path.read_text(encoding="utf-8"))
        except Exception as exc:
            print(f"[ERROR] Failed to parse JSON {json_path}: {exc}", file=sys.stderr)
            return 2
        errors = validate_report_schema(
            data,
            min_pass_rate=args.min_pass_rate,
            min_opcodes=args.min_opcodes,
            require_full_corpus=bool(args.full),
        )
        print_report_tables(data)
        if errors:
            print("\n[FAIL] Schema / coverage verification errors:", file=sys.stderr)
            for err in errors:
                print(f"  - {err}", file=sys.stderr)
            return 1
        print(f"\n[PASS] Verified {json_path} (schema_version=1, {data['opcodes_decoded']}/{data['opcodes_total']} opcodes, {data['corpus_shaders_passed']}/{data['corpus_shaders_total']} shaders).")
        return 0

    exe = find_openscarlett_exe(args.exe)
    if exe is None:
        print(
            "[ERROR] Could not locate openscarlett.exe (pass --exe <path> or build in build/).",
            file=sys.stderr,
        )
        return 2

    json_out = args.json
    json_out.parent.mkdir(parents=True, exist_ok=True)

    cmd: list[str] = [str(exe), "rdna2-bench"]
    if args.fast:
        cmd.append("--fast")
    else:
        cmd.append("--full")
    if args.corpus_count is not None:
        cmd.extend(["--corpus-count", str(args.corpus_count)])
    if args.external_sample is not None:
        cmd.extend(["--external-sample", str(args.external_sample)])
    if args.validate_all_external:
        cmd.append("--validate-all-external")
    if args.no_gpu:
        cmd.append("--no-gpu")
    if args.no_external:
        cmd.append("--no-external")
    cmd.extend(["--json", str(json_out)])

    print(f"[rdna2_corpus_bench] Executing: {' '.join(cmd)}")
    env = os.environ.copy()
    proc = subprocess.run(cmd, env=env, check=False)
    if proc.returncode != 0:
        print(f"[ERROR] openscarlett rdna2-bench exited with code {proc.returncode}", file=sys.stderr)
        return proc.returncode

    if not json_out.exists():
        print(f"[ERROR] Expected JSON output file was not written: {json_out}", file=sys.stderr)
        return 1

    try:
        data = json.loads(json_out.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"[ERROR] Invalid JSON generated at {json_out}: {exc}", file=sys.stderr)
        return 1

    errors = validate_report_schema(
        data,
        min_pass_rate=args.min_pass_rate,
        min_opcodes=args.min_opcodes,
        require_full_corpus=(not args.fast and args.corpus_count is None),
    )
    print_report_tables(data)
    if errors:
        print("\n[FAIL] Schema / coverage verification errors:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print(
        f"\n[PASS] Generated & verified {json_out} "
        f"({data['opcodes_decoded']}/{data['opcodes_total']} opcodes, "
        f"{data['corpus_shaders_passed']}/{data['corpus_shaders_total']} shaders)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
