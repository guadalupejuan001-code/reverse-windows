import argparse
import json
import os
from pathlib import Path
import resource
import statistics
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('build', type=Path)
parser.add_argument('evidence', type=Path)
args = parser.parse_args()
cpus = sorted(os.sched_getaffinity(0))[:2]
if len(cpus) != 2 or os.uname().machine != 'aarch64':
    raise SystemExit('the comparison requires two pinned native ARM64 CPUs')
os.sched_setaffinity(0, cpus)
versions = ('gap8', 'gap24')
modes = ('called', 'useful')
samples = {mode: {version: [] for version in versions} for mode in modes}
raw = args.evidence / 'samples'
raw.mkdir()
for trial in range(5):
    for mode in modes if trial % 2 == 0 else reversed(modes):
        for version in versions if trial % 2 == 0 else reversed(versions):
            before = resource.getrusage(resource.RUSAGE_CHILDREN)
            program = (args.build / version).resolve()
            result = subprocess.run([program, mode, '16'], capture_output=True, text=True, timeout=90)
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            (raw / f'{mode}-{version}-{trial}.stdout').write_text(result.stdout)
            (raw / f'{mode}-{version}-{trial}.stderr').write_text(result.stderr)
            if result.returncode:
                if version == 'gap24' or 'SMP_PAUSE_WAIT_INCOMPLETE vcpus=16' not in result.stderr:
                    raise RuntimeError(f'{version} {mode}: {result.stderr}')
                samples[mode][version].append({'trial': trial, 'failure': result.stderr})
                print(version, mode, trial, result.stderr.strip(), flush=True)
                continue
            line = result.stdout.strip()
            if not line.startswith('SMP_PAUSE_WAIT_PASS '):
                raise RuntimeError(f'unexpected result: {line}')
            values = dict(token.split('=', 1) for token in line.split()[1:])
            if values.pop('mode') != mode:
                raise RuntimeError(f'incorrect fixture mode: {line}')
            metrics = {key: int(value) for key, value in values.items()}
            if metrics['vcpus'] != 16 or metrics['passive_acknowledgements'] != 14 or metrics['exchanged_per_cpu'] != 16:
                raise RuntimeError(f'incorrect guest evidence: {line}')
            if mode == 'useful' and metrics['helper_calls'] != 14 * 512:
                raise RuntimeError('a guest worker dropped useful work')
            if not metrics['bsp_native'] or not metrics['ap_native']:
                raise RuntimeError('intended ARM64 path was not active')
            samples[mode][version].append({'trial': trial, **metrics,
                'host_cpu_seconds': after.ru_utime - before.ru_utime + after.ru_stime - before.ru_stime})
            print(version, mode, trial, line, flush=True)

summary = {}
for mode, by_version in samples.items():
    summary[mode] = {}
    for version, all_rows in by_version.items():
        rows = [row for row in all_rows if 'failure' not in row]
        summary[mode][version] = {'completed': len(rows), **{
            'median_' + field: statistics.median(row[field] for row in rows) if rows else None
            for field in ('elapsed_ns', 'windows', 'total_steps', 'helper_calls', 'pause_yields', 'host_cpu_seconds')}}
    previous = summary[mode]['gap8']['median_elapsed_ns']
    candidate = summary[mode]['gap24']['median_elapsed_ns']
    summary[mode]['wall_change_percent'] = 100 * (candidate / previous - 1) if previous and candidate else None
(args.evidence / 'paired.json').write_text(json.dumps({'cpus': cpus, 'summary': summary, 'samples': samples}, indent=2) + '\n')
print(json.dumps(summary, indent=2))
lines = ['# Separación entre instrucciones PAUSE en ARM64 nativo', '',
         'Cinco comparaciones intercaladas en las mismas dos CPU anfitrionas.',
         'Cada réplica válida completó 32 publicaciones TSC y 14 confirmaciones.',
         'En la carga útil se comprobaron 7.168 incrementos de RAM por réplica.', '',
         '| Carga | Límite de separación | Réplicas completas | Mediana de tiempo (ms) | Mediana de ventanas |',
         '| --- | ---: | ---: | ---: | ---: |']
for mode, row in summary.items():
    for version in versions:
        data = row[version]
        median = data['median_elapsed_ns']
        elapsed = f'{median / 1e6:.2f}' if median is not None else 'Sin medición'
        lines.append(f'| {mode} | {version.removeprefix("gap")} | {data["completed"]} de 5 | {elapsed} | '
                     f'{data["median_windows"]} |')
lines.extend(['', 'Prueba sintética. Cada ventana solicita 8 ms. Las medianas usan las réplicas completas.'])
(args.evidence / 'Resumen.md').write_text('\n'.join(lines) + '\n')
