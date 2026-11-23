import argparse
import json
import os
from pathlib import Path
import resource
import statistics
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('reference', type=Path)
parser.add_argument('candidate', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
cpus = sorted(os.sched_getaffinity(0))[:2]
if len(cpus) != 2:
    raise SystemExit('two host CPUs are required for the SMP comparison')
os.sched_setaffinity(0, cpus)
if os.uname().machine != 'aarch64':
    raise SystemExit('the benchmark requires native Linux ARM64')
rounds = 500003
programs = {'reference': args.reference.resolve(), 'candidate': args.candidate.resolve()}
scenarios = ('native-both', 'mixed-ap')
samples = {scenario: {version: [] for version in programs} for scenario in scenarios}
fields = ('steps', 'bsp_steps', 'ap_steps', 'bsp_checksum', 'ap_checksum')
for repeat in range(6):
    versions = list(programs) if repeat % 2 == 0 else list(reversed(programs))
    for scenario in scenarios if repeat % 2 == 0 else reversed(scenarios):
        paired = {}
        for version in versions:
            usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
            process = subprocess.run([programs[version], scenario, str(rounds)],
                                     capture_output=True, text=True)
            usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
            if process.returncode or not process.stdout.startswith('SHORT_REGISTER_BUS_SAMPLE '):
                raise RuntimeError(f'{version} {scenario}: {process.stdout}{process.stderr}')
            data = {key: int(value) for token in process.stdout.split()[1:]
                    for key, value in [token.split('=', 1)]}
            data['host_cpu_seconds'] = usage_after.ru_utime - usage_before.ru_utime + (
                usage_after.ru_stime - usage_before.ru_stime)
            required_native_path = ('bsp_off_bus_steps' if version == 'reference' else 'bsp_on_bus_steps')
            if not data.get(required_native_path):
                raise RuntimeError(f'expected path was not exercised: {version} {process.stdout}')
            paired[version] = data
            samples[scenario][version].append(data)
            print(version, scenario, process.stdout.strip(), flush=True)
        if any(paired['reference'][field] != paired['candidate'][field] for field in fields):
            raise RuntimeError('reference and candidate retired different guest work')

summary = {}
for scenario, versions in samples.items():
    summary[scenario] = {}
    for version, observations in versions.items():
        summary[scenario][version] = {
            'median_wall_ns': statistics.median(sample['elapsed_ns'] for sample in observations),
            'median_host_cpu_seconds': statistics.median(sample['host_cpu_seconds'] for sample in observations),
            'median_bsp_off_bus_steps': statistics.median(sample['bsp_off_bus_steps'] for sample in observations),
            'median_bsp_on_bus_steps': statistics.median(sample.get('bsp_on_bus_steps', 0) for sample in observations),
        }
    summary[scenario]['candidate_vs_reference_wall_percent'] = 100 * (
        summary[scenario]['candidate']['median_wall_ns'] / summary[scenario]['reference']['median_wall_ns'] - 1)
args.output.write_text(json.dumps({'cpus': cpus, 'guest_rounds': rounds, 'samples': samples,
                                  'summary': summary}, indent=2) + '\n')
print(json.dumps(summary, indent=2))
