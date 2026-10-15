"""gdb が名前を出せなかった MLT の stack frame を、DLL の export 表から解決する。

MSYS2 の MLT は debug 情報を持たないので、gdb は libmlt の frame を ?? と表示する。
module の load 先は、名前の分かっている戻り先 (export_rgba_image が呼んだ mlt_frame_get_image) から
求める。export されていない static 関数は「直前の export + offset」としか分からないので、
offset が大きい frame は関数名を断定しない (出力に offset を残す)。

使い方: python math-score-export-resolve-stacks.py <stacks.txt> [objdump.exe]
"""

import bisect
import collections
import re
import subprocess
import sys

OBJDUMP = sys.argv[2] if len(sys.argv) > 2 else r'C:\msys64\ucrt64\bin\objdump.exe'
MODULES = {
    'libmlt-7.dll': r'C:\msys64\ucrt64\bin\libmlt-7.dll',
    'libmltplus.dll': r'C:\msys64\ucrt64\lib\mlt\libmltplus.dll',
}


def exports(path):
    text = subprocess.run([OBJDUMP, '-p', path], capture_output=True, text=True, check=True).stdout
    # 番地の表 (index -> RVA) と名前の表 (index -> 名前) を index で突き合わせる。
    addresses, names = {}, {}
    section = None
    for line in text.splitlines():
        if line.startswith('Export Address Table --'):
            section = 'address'
            continue
        if line.startswith('[Ordinal/Name Pointer] Table'):
            section = 'name'
            continue
        if section == 'address':
            match = re.match(r'\s*\[\s*(\d+)\]\s+\+base\[\s*\d+\]\s+([0-9a-f]+)\s+Export RVA', line)
            if match:
                addresses[int(match.group(1))] = int(match.group(2), 16)
        elif section == 'name':
            match = re.match(r'\s*\[\s*(\d+)\]\s+\+base\[\s*\d+\]\s+[0-9a-f]+\s+(\S+)', line)
            if match:
                names[int(match.group(1))] = match.group(2)
    return sorted((addresses[index], name) for index, name in names.items() if index in addresses)


def main():
    stacks = open(sys.argv[1], encoding='utf-8', errors='replace').read()
    tables = {name: exports(path) for name, path in MODULES.items()}
    for name, table in tables.items():
        if not table:
            raise SystemExit(f'{name} の export 表を読めません')
    threads = [block for block in stacks.split('\n\n') if block.startswith('Thread ')]
    frame = re.compile(r'#(\d+)\s+0x([0-9a-f]+) in (\S+) \(.*?(?:from (\S+))?$')

    # libmlt の load 先: export_rgba_image の直後の frame は mlt_frame_get_image の中にある。
    base = {}
    for block in threads:
        lines = block.splitlines()
        for index, line in enumerate(lines):
            if 'export_rgba_image' in line and index > 0:
                match = frame.match(lines[index - 1])
                if match and (match.group(4) or '').endswith('libmlt-7.dll'):
                    rva = dict((n, a) for a, n in tables['libmlt-7.dll'])['mlt_frame_get_image']
                    base['libmlt-7.dll'] = (int(match.group(2), 16) - rva) & ~0xFFFF
    anchor = 'mlt_frame_get_image'
    if 'libmlt-7.dll' not in base:
        # layer が多いと合成の再帰が深く、採った 40 frame に export_rgba_image が入らない。
        # consumer の thread にある observed_get_frame の呼び出し元 (mlt_service_get_frame) を使う。
        anchor = 'mlt_service_get_frame'
        for block in threads:
            lines = block.splitlines()
            for index, line in enumerate(lines):
                if 'observed_get_frame' in line and index + 1 < len(lines):
                    match = frame.match(lines[index + 1])
                    if match and (match.group(4) or '').endswith('libmlt-7.dll'):
                        rva = dict((n, a) for a, n in tables['libmlt-7.dll'])[anchor]
                        base['libmlt-7.dll'] = (int(match.group(2), 16) - rva) & ~0xFFFF
    if 'libmlt-7.dll' not in base:
        raise SystemExit('load 先を決める frame が stack にありません')
    print(f"libmlt-7.dll の load 先: {base['libmlt-7.dll']:#x} ({anchor} の戻り先から)")

    def resolve(address, module):
        table = tables[module]
        rva = address - base[module]
        index = bisect.bisect_right([a for a, _ in table], rva) - 1
        if index < 0:
            return f'?+{rva:#x}'
        return f'{table[index][1]}+{rva - table[index][0]:#x}'

    running = collections.Counter()
    for block in threads:
        lines = block.splitlines()
        first = frame.match(lines[1]) if len(lines) > 1 else None
        if not first:
            continue
        waiting = 'Wait' in first.group(3) or 'Sleep' in first.group(3)
        chain = []
        for line in lines[1:14]:
            match = frame.match(line)
            if not match:
                continue
            module = (match.group(4) or '').replace('\\', '/').split('/')[-1]
            if module in base:
                chain.append(resolve(int(match.group(2), 16), module))
            elif module:
                chain.append(f'{match.group(3)}@{module}' if match.group(3) != '??' else f'??@{module}')
            else:
                chain.append(match.group(3))
        # 待機中の slice worker (thread pool) は並べない。合成・frame 取得の経路にいる thread だけ。
        if any('libmltplus' in item or item.startswith(('mlt_frame_', 'mlt_property_',
                                                         'mlt_animation_', 'mlt_service_get_frame'))
               for item in chain):
            state = 'WAIT' if waiting else 'RUN '
            print(state, lines[0].split('(')[0].strip(), ' < '.join(chain[:10]))
            if not waiting:
                running[chain[0]] += 1
    print('実行中の thread の先頭 frame:', dict(running))


if __name__ == '__main__':
    main()
