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
parser.add_argument('evidence', type=Path)
args = parser.parse_args()
cpus = sorted(os.sched_getaffinity(0))[:2]
if len(cpus) != 2 or os.uname().machine != 'aarch64':
    raise SystemExit('the comparison requires two pinned native ARM64 CPUs')
os.sched_setaffinity(0, cpus)
programs = {'reference': args.reference.resolve(), 'candidate': args.candidate.resolve()}
raw = args.evidence / 'samples'
raw.mkdir()
results = {version: [] for version in programs}
for trial in range(5):
    order = list(programs) if trial % 2 == 0 else list(reversed(programs))
    for version in order:
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        result = subprocess.run([programs[version]], capture_output=True, text=True, timeout=90)
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        (raw / f'{version}-{trial}.stdout').write_text(result.stdout)
        (raw / f'{version}-{trial}.stderr').write_text(result.stderr)
        samples = {}
        for line in result.stdout.splitlines():
            if not line.startswith('SMP_PAUSE_WAIT_PASS '):
                raise RuntimeError(f'unexpected standard output: {line}')
            data = {key: int(value) for token in line.split()[1:]
                    for key, value in [token.split('=', 1)]}
            processors = data['vcpus']
            stamps = sum(line.startswith(f'SMP_PAUSE_WAIT_TIMESTAMP vcpus={processors} ')
                         for line in result.stderr.splitlines())
            if data['passive_acknowledgements'] != processors - 2 or data['exchanged_per_cpu'] != 16 or stamps != 16:
                raise RuntimeError('the exchange did not validate the same useful guest work')
            samples[processors] = data
            print(version, trial, line, flush=True)
        if result.returncode:
            print(version, trial, result.stderr.splitlines()[-1], flush=True)
            if version == 'candidate' or 'SMP_PAUSE_WAIT_INCOMPLETE vcpus=16' not in result.stderr or 2 not in samples:
                raise RuntimeError(f'{version} trial {trial} failed the guest oracle')
        results[version].append({'trial': trial, 'return_code': result.returncode, 'samples': samples,
                                 'host_cpu_seconds': after.ru_utime - before.ru_utime + after.ru_stime - before.ru_stime})

summary = {}
for processors in (2, 16):
    summary[processors] = {}
    for version, observations in results.items():
        complete = [row['samples'][processors] for row in observations if processors in row['samples']]
        summary[processors][version] = {
            'completed': len(complete),
            'median_wall_ns': statistics.median(sample['elapsed_ns'] for sample in complete) if complete else None,
            'median_windows': statistics.median(sample['windows'] for sample in complete) if complete else None,
            'median_guest_steps': statistics.median(sample['total_steps'] for sample in complete) if complete else None,
        }
(args.evidence / 'paired.json').write_text(json.dumps({'cpus': cpus, 'summary': summary, 'samples': results}, indent=2) + '\n')
print(json.dumps(summary, indent=2))

header = ['# Intercambio TSC con vCPU en espera activa', '',
          'Cinco pares intercalados en Linux ARM64 con dos CPU anfitrionas fijadas.',
          'Cada réplica válida verificó 32 publicaciones TSC y 14 confirmaciones cuando hubo 16 vCPU.',
          'Las medianas usan las réplicas que completaron el caso.', '',
          '| vCPU | Revisión | Réplicas completas | Mediana de tiempo (ms) | Mediana de ventanas | Mediana de pasos |',
          '| --- | --- | --- | --- | --- | --- |']
for processors, versions in summary.items():
    for version, data in versions.items():
        elapsed = data['median_wall_ns']
        elapsed = f'{elapsed / 1e6:.2f}' if elapsed is not None else 'Sin medición'
        header.append(f'| {processors} | {version} | {data["completed"]} de 5 | {elapsed} | '
                      f'{data["median_windows"]} | {data["median_guest_steps"]} |')
header.extend(['', 'La prueba mide el intercambio sintético y el reparto del bus. Cada ventana solicita 8 ms.',
               'Los archivos por réplica contienen las marcas publicadas y todas las ventanas de cada agotamiento del plazo.'])
(args.evidence / 'Resumen.md').write_text('\n'.join(header) + '\n')
