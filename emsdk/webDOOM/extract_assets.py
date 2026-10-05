#!/usr/bin/env python3
"""Extract files from an emscripten file_packager .data package.

The .data file is a raw concatenation of the packaged files; the byte
offsets live in the metadata object inside the corresponding .js loader.
"""
import json
import os
import re
import sys


def extract(js_path, data_path, dest_root):
    with open(js_path, 'r', encoding='utf-8', errors='replace') as f:
        js = f.read()

    start = js.index('loadPackage(') + len('loadPackage(')
    # balance braces to find the end of the metadata object
    depth = 0
    meta_txt = None
    for i in range(start, len(js)):
        if js[i] == '{':
            depth += 1
        elif js[i] == '}':
            depth -= 1
            if depth == 0:
                meta_txt = js[start:i + 1]
                break
    if meta_txt is None:
        raise SystemExit('cannot locate metadata in %s' % js_path)

    meta_txt = re.sub(r',(\s*[}\]])', r'\1', meta_txt)  # tolerate trailing commas
    meta = json.loads(meta_txt)
    files = meta['files']
    size = os.path.getsize(data_path)
    assert size == meta['remote_package_size'], (size, meta['remote_package_size'])

    n = 0
    with open(data_path, 'rb') as data:
        for ent in files:
            name = ent['filename'].lstrip('/')
            out = os.path.join(dest_root, name)
            parent = os.path.dirname(out)
            if parent:
                os.makedirs(parent, exist_ok=True)
            data.seek(ent['start'])
            blob = data.read(ent['end'] - ent['start'])
            with open(out, 'wb') as fh:
                fh.write(blob)
            n += 1
    print('%s -> %d files extracted to %s' % (os.path.basename(js_path), n, dest_root))


if __name__ == '__main__':
    extract(sys.argv[1], sys.argv[2], sys.argv[3])
