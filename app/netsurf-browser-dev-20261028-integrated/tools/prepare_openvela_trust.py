#!/usr/bin/env python3
"""Export public roots already trusted by this Windows host for device testing."""
import argparse
from pathlib import Path
import ssl
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
roots=[]
for certificate,encoding,trust in ssl.enum_certificates('ROOT'):
    if encoding=='x509_asn' and (trust is True or ssl.Purpose.SERVER_AUTH.oid in trust):
        roots.append(ssl.DER_cert_to_PEM_cert(certificate))
data=''.join(roots).encode('ascii')
if not roots or len(data)>256*1024:raise RuntimeError('empty or oversized trusted-root bundle')
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(data)
print('Trusted public roots exported:',len(roots),'bytes=',len(data))
