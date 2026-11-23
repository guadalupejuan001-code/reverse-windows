"""Resolve the two explicitly pinned products for the physical ARM64 job."""
import json
from pathlib import Path
import re
import sys

pins = json.loads(Path(sys.argv[1]).read_text())
for role in ('reference', 'candidate'):
    record = pins.get(role)
    if not record:
        raise SystemExit(f'A published {role} product and its Mundo X pin are required')
    revisions = (record.get('product_sha'), record.get('mundo_x_sha'))
    if not all(isinstance(sha, str) and re.fullmatch('[0-9a-f]{40}', sha) for sha in revisions):
        raise SystemExit(f'Enter full Git SHAs for the {role} product and Mundo X')
    if role == 'candidate' and record['product_sha'] == pins['reference']['product_sha']:
        raise SystemExit('The candidate product SHA is the reference SHA')
    print(role, *revisions, sep='\t')
