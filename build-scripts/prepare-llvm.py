#!/usr/bin/env python3
"""Prepare pinned LLVM plus project patches without altering a developer checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def prepare(output):
    output=output.resolve()
    if output in (ROOT,ROOT/'build',ROOT/'third_party') or output.is_relative_to(ROOT/'src') or output.is_relative_to(ROOT/'third_party'):
        raise ValueError('LLVM preparation requires a dedicated build directory')
    revision=re.search(r'^LLVM_COMMIT=(\w+)$',(ROOT/'src/config/build/versions.env').read_text(),re.M)[1]
    patches=sorted((ROOT/'src/patches/llvm').glob('*.patch'))
    identity=dict(revision=revision,patches={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in patches})
    stamp=output/'.golem-source.json'
    if stamp.is_file():
        if json.loads(stamp.read_text())!=identity:
            raise RuntimeError(f'Prepared LLVM inputs changed; choose a fresh output directory: {output}')
        return output
    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f'Refusing to overwrite an unowned directory: {output}')
    output.mkdir(parents=True,exist_ok=True)
    archive=subprocess.Popen(['git','-C',str(ROOT/'third_party/llvm-project'),'archive',revision],stdout=subprocess.PIPE)
    try:
        subprocess.run(['tar','-x','-C',str(output)],stdin=archive.stdout,check=True)
    finally:
        archive.stdout.close()
        code=archive.wait()
    if code: raise RuntimeError('LLVM archive failed')
    for patch in patches:
        subprocess.run(['patch','-p1','--batch','--forward','-i',str(patch)],cwd=output,check=True)
    stamp.write_text(json.dumps(identity,indent=2)+'\n')
    return output


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    print(prepare(parser.parse_args().output))
