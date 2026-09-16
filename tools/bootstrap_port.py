"""Verify explicit local inputs, generate private AOT sources, and build a port target.

This script does not download assets, extract an ISO, launch a game, or use a
player profile. Generated game C++ and provenance stay in the ignored build tree.
"""
import argparse
import hashlib
import json
import platform
import shlex
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from verify_port_inputs import source_hashes, sys_file_hashes

ROOT = Path(__file__).resolve().parents[1]
PINS = json.loads((ROOT / "tools/port_source_pins.json").read_text(encoding="utf-8"))


def sha(path, algorithm="sha256"):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def run(command):
    print("+ " + shlex.join(str(value) for value in command), flush=True)
    subprocess.run([str(value) for value in command], cwd=ROOT, check=True)


def git_head(path):
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--decomp-root", type=Path, required=True)
    parser.add_argument("--dol", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/mac-headless")
    parser.add_argument("--frontend", choices=("headless", "windows"), default="headless")
    parser.add_argument("--stage", choices=("generate", "configure", "build"), default="build")
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=4)
    parser.add_argument("--cmake-generator", default="Ninja")
    parser.add_argument("--build-type", choices=("Debug", "Release", "RelWithDebInfo"), default="Release")
    parser.add_argument("--transport-tests", action="store_true",
                        help="explicitly enable local IPv4-loopback transport fixtures")
    parser.add_argument("--macos-arch", choices=("arm64", "x86_64"),
                        default=platform.machine() if sys.platform == "darwin" else None,
                        help="explicit macOS target architecture (defaults to the current host)")
    codes = parser.add_mutually_exclusive_group(required=True)
    codes.add_argument("--gct-base", help="explicit guest load address for the pinned Slippi code set")
    codes.add_argument("--no-slippi", action="store_true", help="explicit vanilla-only diagnostic translation")
    parser.add_argument("--playback", action="store_true",
                        help="translate against the Slippi Playback code set (port/slippi_sys_playback) for replay re-simulation")
    parser.add_argument("--extra-gct", type=Path, help="playback: a replay's code list (gecko_list.bin) to translate as well")
    parser.add_argument("--extra-gct-base", help="playback: guest address where the game installs that list")
    args = parser.parse_args()
    if (args.extra_gct is None) != (args.extra_gct_base is None):
        parser.error("--extra-gct and --extra-gct-base go together")
    if args.extra_gct and not args.playback:
        parser.error("--extra-gct is only meaningful with --playback")
    if args.macos_arch and sys.platform != "darwin":
        parser.error("--macos-arch requires a macOS host/toolchain")

    decomp = args.decomp_root.resolve(strict=True)
    dol = args.dol.resolve(strict=True)
    build = args.build_dir.resolve()
    try:
        relative_build = build.relative_to((ROOT / "build").resolve())
    except ValueError:
        parser.error("--build-dir must be a child of this checkout's ignored build/ directory")
    if relative_build == Path("."):
        parser.error("use a named child of build/, not the shared build directory itself")
    if git_head(decomp) != PINS["decomp"]["commit"]:
        parser.error("--decomp-root HEAD must equal pinned revision " + PINS["decomp"]["commit"])
    for relative, expected in PINS["decomp"]["files_sha256"].items():
        path = decomp / relative
        if not path.is_file() or sha(path) != expected:
            parser.error("decomp input differs from the pinned source: " + str(path))
    if sha(dol, "sha1") != PINS["dol"]["sha1"]:
        parser.error("--dol must be the verified vanilla Melee NTSC 1.02 image")
    if args.gct_base:
        try:
            base = int(args.gct_base, 0)
        except ValueError:
            parser.error("--gct-base must be an integer such as " + PINS["slippi"]["gct_base"])
        if not 0x80000000 <= base < 0x81800000 or base % 4:
            parser.error("--gct-base must be an aligned address in GameCube main RAM")

    generated = build / "generated/guest"
    generated.mkdir(parents=True, exist_ok=True)
    sys_dir = ROOT / ("port/slippi_sys_playback" if args.playback else PINS["slippi"]["sys_dir"])
    slippi_inputs = []
    # Both Slippi-enabled and vanilla translation profiles use the same explicit
    # runtime asset tree; --no-slippi changes guest patch generation only.
    slippi_sys_hashes = sys_file_hashes(sys_dir)
    if not args.no_slippi and not args.playback:  # the pins describe the online code set
        for relative, expected in PINS["slippi"]["files_sha256"].items():
            path = sys_dir / relative
            if not path.is_file() or sha(path) != expected:
                parser.error("Slippi input differs from the pinned upstream profile: " + str(path))
            slippi_inputs.append(path)
    # Runtime GameFiles/diffs must match the same upstream source commit,
    # including inventory. Do not accept a dirty replacement Sys tree.
    tracked = subprocess.check_output(["git", "-C", str(ROOT), "ls-tree", "-r", "--name-only",
        PINS["upstream"]["commit"], "--", str(sys_dir.relative_to(ROOT))], text=True).splitlines()
    actual = {str((sys_dir / relative).relative_to(ROOT)) for relative in slippi_sys_hashes}
    if actual != set(tracked):
        parser.error("Slippi Sys file inventory differs from the pinned upstream commit")
    for relative in tracked:
        path = ROOT / relative
        expected_blob = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse",
            PINS["upstream"]["commit"] + ":" + relative], text=True).strip()
        actual_blob = subprocess.check_output(["git", "-C", str(ROOT), "hash-object", str(path)], text=True).strip()
        if expected_blob != actual_blob:
            parser.error("Slippi runtime asset differs from the pinned upstream commit: " + relative)
    before_hashes = source_hashes()
    command = [sys.executable, ROOT / "port/recomp/recomp.py", "--dol", dol,
               "--symbols", decomp / "config/GALE01/symbols.txt", "--out", generated]
    if args.no_slippi:
        command.append("--no-slippi")
    else:
        command += ["--sys-dir", sys_dir, "--gct-base", args.gct_base]
        if args.extra_gct:
            command += ["--extra-gct", args.extra_gct.resolve(strict=True), "--extra-gct-base", args.extra_gct_base]
        print("Slippi GCT base is a build-profile assumption; verify the actual guest load address at runtime.", flush=True)
    run(command)
    fobj = build / "generated/FObjHost.cpp"
    run([sys.executable, ROOT / "tools/generate_fobj_host.py", fobj, "--decomp-root", decomp])
    if before_hashes != source_hashes():
        parser.error("build source changed during generation; rerun after the source checkpoint stabilizes")
    for path in slippi_inputs:
        if sha(path) != PINS["slippi"]["files_sha256"][str(path.relative_to(sys_dir))]:
            parser.error("Slippi input changed during generation; rerun after the source checkpoint stabilizes")
    if sys_file_hashes(sys_dir) != slippi_sys_hashes:
        parser.error("Slippi runtime asset inventory/content changed during generation")
    provenance = {
        "schema": 1,
        "source_root": str(ROOT),
        "kind": "local generated game code; do not commit or distribute",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "upstream_pin": PINS["upstream"], "checkout_head": git_head(ROOT),
        "decomp_pin": PINS["decomp"], "decomp_root": str(decomp),
        "dol": {"path": str(dol), "sha1": PINS["dol"]["sha1"]},
        "gct_base": args.gct_base, "gct_base_runtime_verified": False,
        "slippi_enabled": not args.no_slippi, "playback": args.playback,
        "extra_gct": {"path": str(args.extra_gct), "sha256": sha(args.extra_gct), "base": args.extra_gct_base} if args.extra_gct else None,
        "requested_build": {"frontend": args.frontend, "build_type": args.build_type,
                            "generator": args.cmake_generator, "macos_arch": args.macos_arch,
                            "transport_tests": args.transport_tests},
        "generator_command": [str(value) for value in command],
        "build_sources_sha256": before_hashes,
        "slippi_sha256": {str(path.relative_to(ROOT)): sha(path) for path in slippi_inputs},
        "slippi_sys_dir": str(sys_dir), "slippi_sys_files_sha256": slippi_sys_hashes,
        "fobj_generated": {"path": str(fobj), "sha256": sha(fobj)},
        "generated_sha256": {path.name: sha(path) for path in sorted(generated.iterdir()) if path.is_file()},
        "generated_dir": str(generated),
    }
    manifest_text = json.dumps(provenance, indent=2) + "\n"
    manifest_digest = hashlib.sha256(manifest_text.encode("utf-8")).hexdigest()
    manifest_directory = build / "manifests"
    manifest_directory.mkdir(exist_ok=True)
    manifest_path = manifest_directory / (manifest_digest + ".json")
    if not manifest_path.exists():
        with manifest_path.open("x", encoding="utf-8") as output:
            output.write(manifest_text)
    elif sha(manifest_path) != manifest_digest:
        parser.error("immutable input manifest identity collision")
    # This convenience copy can advance. The executable binds the immutable
    # manifest path so a subsequent build does not invalidate an older artifact.
    (build / "port-inputs.json").write_text(manifest_text, encoding="utf-8")
    if args.stage == "generate":
        return
    headless = args.frontend == "headless"
    configure = ["cmake", "-S", ROOT, "-B", build, "-G", args.cmake_generator,
         "-DCMAKE_BUILD_TYPE=" + args.build_type, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
         "-DMELEE_DECOMP_ROOT=" + str(decomp), "-DMELEE_DOL_PATH=" + str(dol),
         "-DMELEE_PORT_GENERATED_DIR=" + str(generated), "-DMELEE_BUILD_PORT_TESTS=ON",
         "-DMELEE_PORT_INPUT_MANIFEST=" + str(manifest_path),
         "-DMELEE_BUILD_PORT_TRANSPORT_TESTS=" + ("ON" if args.transport_tests else "OFF"),
         "-DMELEE_BUILD_PORT_HEADLESS=" + ("ON" if headless else "OFF"),
         "-DMELEE_BUILD_EXPERIMENTAL_PORT=" + ("OFF" if headless else "ON")]
    if args.macos_arch:
        configure.append("-DCMAKE_OSX_ARCHITECTURES=" + args.macos_arch)
    run(configure)
    if args.stage == "build":
        run(["cmake", "--build", build, "--config", args.build_type, "--parallel", str(args.jobs)])
    print("Build artifacts: " + str(build), flush=True)


if __name__ == "__main__":
    main()
