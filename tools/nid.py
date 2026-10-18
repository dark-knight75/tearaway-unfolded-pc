"""Add PS4 functions to src/import_names.inc by name, without an aerolib.inl.

A NID is the first 8 bytes of SHA-1(name + fixed suffix), base64 with '-' for '/'.
Usage: python tools/nid.py LIBRARY NAME [NAME ...]   e.g. libSceCamera sceCameraOpen
"""
import base64
import hashlib
import struct
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT / 'scripts'))
from link_modules import RUNTIME_SPELLING  # noqa: E402

INC = PORT / 'src' / 'import_names.inc'
SUFFIX = bytes.fromhex('518D64A635DED8C1E6B039B1C3E55230')


def nid(name):
    digest = hashlib.sha1(name.encode() + SUFFIX).digest()
    return base64.b64encode(struct.pack('>Q', struct.unpack('<Q', digest[:8])[0]), b'+-').decode().rstrip('=')


def scoped(library, name):
    codes = RUNTIME_SPELLING.get(library)
    return f'{nid(name)}#{codes[0]}#{codes[1]}' if codes else f'{nid(name)}#{library}'


def main():
    library, names = sys.argv[1], sys.argv[2:]
    text = INC.read_text(encoding='utf-8')
    added = [f'{{"{scoped(library, n)}","{n}"}},' for n in names if f'"{scoped(library, n)}"' not in text]
    if added:
        INC.write_text(text.rstrip('\n') + '\n' + '\n'.join(added) + '\n', encoding='utf-8')
    print(f'{len(added)} names added to {INC.relative_to(PORT)}')


if __name__ == '__main__':
    # Known values from the table: the hash must reproduce them.
    assert nid('scePadOpen') == 'xk0AcarP3V4' and nid('scePadReadState') == 'YndgXqQVV7c'
    main()
