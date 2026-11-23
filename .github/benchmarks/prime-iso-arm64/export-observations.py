"""Publish boot counters, hashed observations and visible native guest frames."""
import argparse
from hashlib import sha256
import json
import os
import re
from pathlib import Path
from PIL import Image
from summarize import RANGES, continuation_record, load_checkpoint


def lines(path):
    if path.exists():
        with path.open() as stream:
            for line in stream:
                if not line.endswith("\n"):
                    break
                yield json.loads(line)


def render(frame, label, run, output, frames):
    temporary_source = frame.pop("temporary_source")
    if frame["kind"] != "guest_vbe_rgb":
        return
    if temporary_source != f"{label}.ppm" or not (label.isdecimal() or label == "terminal"):
        raise ValueError("Unexpected temporary frame name")
    with Image.open(run / temporary_source) as picture:
        if picture.mode != "RGB" or picture.size != (frame["width"], frame["height"]):
            raise ValueError("RGB frame geometry does not match observation")
        fingerprint = sha256(picture.tobytes()).hexdigest()
        if fingerprint != frame["rgb_or_text_sha256"]:
            raise ValueError("RGB frame SHA-256 does not match observation")
        channels = picture.getextrema()
        if all(low == high for low, high in channels):
            frame["uniform_rgb"] = [low for low, _ in channels]
            return
        if fingerprint not in frames:
            name = f"guest-{label}.png"
            picture.save(output / name, optimize=True)
            frames[fingerprint] = name
        frame["png"] = frames[fingerprint]


cli = argparse.ArgumentParser(description=__doc__)
cli.add_argument("--run", required=True, type=Path)
cli.add_argument("--transport-log", required=True, type=Path)
cli.add_argument("--output", required=True, type=Path)
cli.add_argument("--exit-code", required=True, type=int)
args = cli.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
observations = list(lines(args.run / "boot-observations.jsonl"))
frames = {}
for observation in observations:
    render(observation["frame"], observation["label"], args.run, args.output, frames)
checkpoints = [load_checkpoint(row) for row in lines(args.run / "milestones.jsonl")]
trace = list(lines(args.run / "bounded-trace.jsonl"))
if len(trace) > 64:
    raise ValueError("Instruction trace exceeded its fixed limit")
transport_log = args.transport_log.read_text()
transport = RANGES.search(transport_log)
extents = re.search(r"REMOTE_ISO_BOOT_RANGES el_torito_image=(\d+) bootmgr=(\d+) bcd=(\d+) boot_wim=(\d+)",
                    transport_log)
boot_extents = None if extents is None else dict(zip(("el_torito_image", "bootmgr", "bcd", "boot_wim"),
                                                     map(int, extents.groups())))
verified = None
if transport:
    fields = ("requests", "verified_ranges", "downloaded_bytes", "delivered_bytes", "failed",
              "first_fetched_offset", "last_fetched_byte")
    verified = dict(zip(fields, map(int, transport.groups())))
stop = args.run / "stop.json"
summary = {
    "product_sha": os.environ["PRODUCT_SHA"],
    "mundo_x_sha": os.environ["MUNDO_X_SHA"],
    "runner_architecture": os.environ["RUNNER_ARCH"],
    "affinity_cpu": int(os.environ["PROBE_CPU"]),
    "host_available_mib": int(os.environ["AVAILABLE_MIB"]),
    "host_threshold_mib": int(os.environ["THRESHOLD_MIB"]),
    "probe_exit_code": args.exit_code,
    "optical_media": "Microsoft Windows 10 22H2 Enterprise Evaluation x64",
    "optical_media_bytes": 5_550_497_792,
    "optical_media_etag": "0x10B3D1548F58FA2A3866686CB570F06A16F6734DD99BBA1C7F0542A849EDD8E4",
    "transport": verified,
    "lab_iso_full_sha256_from_independent_media_validation": "ef7312733a9f5d7d51cfa04ac497671995674ca5e1058d5164d6028f0938d668",
    "observed_returned_extent_bytes_including_repeated_reads": boot_extents,
    "stop": json.loads(stop.read_text()) if stop.exists() else {"stop_reason": "no_terminal_record"},
    "continuation": continuation_record(args.run, int(os.environ["STOP_TARGET"])),
    "checkpoints": checkpoints,
    "boot_observations": observations,
    "trace_at_8_65b_if_module_unnamed": trace,
}
if checkpoints:
    summary["last_sampled_steps"] = checkpoints[-1]["actual_steps"]
(args.output / "boot-diagnostic.json").write_text(json.dumps(summary, indent=2) + "\n")
for row in observations:
    print(f"observed {row['label']} steps={row['steps']} mode={row['cpu_mode']} rip={row['rip']} "
          f"frame={row['frame']['kind']} module={row['module']}")
print(f"Recorded stop status {summary['stop']}")
valid_source = verified and verified["requests"] == verified["verified_ranges"] > 0 and not verified["failed"]
raise SystemExit(0 if valid_source and (checkpoints or observations) else 1)
