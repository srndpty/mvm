"""Windows の minidump から、例外と各 thread の native stack を出す (docs/crash-evidence.md)。

標準 library だけで動く (UCRT64 の python で実行する)。この開発機には minidump を読む debugger
(cdb・WinDbg・lldb) が無いので、x64 の unwind をここで行う。

2 種類の stack を、出力で区別する:
  unwind  PE の例外 directory (.pdata) の RUNTIME_FUNCTION と UNWIND_INFO に従って、レジスタを
          1 frame ずつ戻した結果。各 frame の module は、disk の image の TimeDateStamp・SizeOfImage が
          dump の記録と一致したものだけを使う。一致しない・image が無い module に入ったら、そこで止める
          (推測で続けない)。
  scan    stack の memory のうち、読み込まれた module の範囲を指す値を並べただけのもの (heuristic)。
          本当の戻り先でない値を含む。unwind が途中で止まったときの手がかりにだけ使う。

既知の制限: 例外の frame が epilog の途中にあると unwind を誤ることがある (epilog の判定はしない)。

関数名: Windows の DLL 以外は addr2line を試す (build/ の image は RelWithDebInfo で関数と行が付く)。
名前が出なければ export 表の直前の名前 + offset で、これは関数名の断定ではない。

使い方: python minidump-stack.py <dump> [--addr2line <exe>] [--json <out>]
"""

import bisect
import json
import os
import struct
import subprocess
import sys

STREAM_THREADS, STREAM_MODULES, STREAM_MEMORY, STREAM_EXCEPTION, STREAM_MEMORY64 = 3, 4, 5, 6, 9
REGISTERS = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
             'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']


class Dump:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        if d[:4] != b'MDMP':
            raise ValueError('minidump ではありません')
        count, directory = struct.unpack_from('<II', d, 8)
        self.streams = {}
        for i in range(count):
            kind, size, rva = struct.unpack_from('<III', d, directory + 12 * i)
            self.streams.setdefault(kind, (rva, size))
        self.ranges = []
        if STREAM_MEMORY in self.streams:
            rva, _ = self.streams[STREAM_MEMORY]
            n, = struct.unpack_from('<I', d, rva)
            for i in range(n):
                start, size, at = struct.unpack_from('<QII', d, rva + 4 + 16 * i)
                self.ranges.append((start, size, at))
        if STREAM_MEMORY64 in self.streams:
            rva, _ = self.streams[STREAM_MEMORY64]
            n, base = struct.unpack_from('<QQ', d, rva)
            at = base
            for i in range(n):
                start, size = struct.unpack_from('<QQ', d, rva + 16 + 16 * i)
                self.ranges.append((start, size, at))
                at += size
        self.ranges.sort()
        self.starts = [r[0] for r in self.ranges]
        self.modules = self._modules()
        self.threads = self._threads()
        self.exception = self._exception()

    def string(self, rva):
        n, = struct.unpack_from('<I', self.data, rva)
        return self.data[rva + 4: rva + 4 + n].decode('utf-16-le')

    def _modules(self):
        rva, _ = self.streams[STREAM_MODULES]
        n, = struct.unpack_from('<I', self.data, rva)
        out = []
        for i in range(n):
            at = rva + 4 + 108 * i
            base, size, _checksum, stamp, name = struct.unpack_from('<QIIII', self.data, at)
            out.append({'base': base, 'size': size, 'timestamp': stamp, 'path': self.string(name)})
        out.sort(key=lambda m: m['base'])
        return out

    def _threads(self):
        rva, _ = self.streams[STREAM_THREADS]
        n, = struct.unpack_from('<I', self.data, rva)
        out = []
        for i in range(n):
            at = rva + 4 + 48 * i
            tid, = struct.unpack_from('<I', self.data, at)
            csize, crva = struct.unpack_from('<II', self.data, at + 40)
            out.append({'id': tid, 'context': self.context(crva, csize)})
        return out

    def _exception(self):
        if STREAM_EXCEPTION not in self.streams:
            return None
        rva, _ = self.streams[STREAM_EXCEPTION]
        tid, = struct.unpack_from('<I', self.data, rva)
        code, flags, _record, address, nparams = struct.unpack_from('<IIQQI', self.data, rva + 8)
        params = list(struct.unpack_from('<15Q', self.data, rva + 8 + 32))[:nparams]
        csize, crva = struct.unpack_from('<II', self.data, rva + 8 + 152)
        return {'thread': tid, 'code': code, 'flags': flags, 'address': address,
                'parameters': params, 'context': self.context(crva, csize)}

    def context(self, rva, size):
        if size < 0x100:
            return None
        regs = dict(zip(REGISTERS, struct.unpack_from('<16Q', self.data, rva + 0x78)))
        regs['rip'], = struct.unpack_from('<Q', self.data, rva + 0xF8)
        return regs

    def read(self, address, size):
        i = bisect.bisect_right(self.starts, address) - 1
        if i < 0:
            return None
        start, length, at = self.ranges[i]
        if address + size > start + length:
            return None
        return self.data[at + address - start: at + address - start + size]

    def u64(self, address):
        b = self.read(address, 8)
        return None if b is None else struct.unpack('<Q', b)[0]

    def module_of(self, address):
        for m in self.modules:
            if m['base'] <= address < m['base'] + m['size']:
                return m
        return None


class Image:
    """disk 上の PE。dump の module と同じ build のときだけ使う。"""

    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        pe, = struct.unpack_from('<I', d, 0x3C)
        if d[pe:pe + 4] != b'PE\0\0':
            raise ValueError('PE ではありません')
        sections, = struct.unpack_from('<H', d, pe + 6)
        self.timestamp, = struct.unpack_from('<I', d, pe + 8)
        optional_size, = struct.unpack_from('<H', d, pe + 20)
        opt = pe + 24
        if struct.unpack_from('<H', d, opt)[0] != 0x20B:
            raise ValueError('PE32+ ではありません')
        self.image_base, = struct.unpack_from('<Q', d, opt + 24)
        self.size, = struct.unpack_from('<I', d, opt + 56)
        dirs = opt + 112
        self.export_dir = struct.unpack_from('<II', d, dirs)
        self.exception_dir = struct.unpack_from('<II', d, dirs + 8 * 3)
        self.sections = []
        for i in range(sections):
            at = opt + optional_size + 40 * i
            vsize, va, rawsize, raw = struct.unpack_from('<IIII', d, at + 8)
            self.sections.append((va, max(vsize, rawsize), raw, rawsize))
        rva, size = self.exception_dir
        self.functions = []
        if rva:
            base = self.offset(rva)
            for i in range(size // 12):
                self.functions.append(struct.unpack_from('<III', d, base + 12 * i))
        self.function_starts = [f[0] for f in self.functions]
        self._exports = None

    def offset(self, rva):
        for va, vsize, raw, rawsize in self.sections:
            if va <= rva < va + vsize:
                if rva - va >= rawsize:
                    raise ValueError(f'RVA {rva:#x} は file に無い')
                return raw + rva - va
        raise ValueError(f'RVA {rva:#x} が section に無い')

    def function(self, rva):
        i = bisect.bisect_right(self.function_starts, rva) - 1
        if i >= 0 and self.functions[i][0] <= rva < self.functions[i][1]:
            return self.functions[i]
        return None

    def exports(self):
        if self._exports is None:
            self._exports = []
            rva, _ = self.export_dir
            if rva:
                d = self.data
                at = self.offset(rva)
                nfunc, nname, afunc, aname, aord = struct.unpack_from('<IIIII', d, at + 20)
                functions = [struct.unpack_from('<I', d, self.offset(afunc) + 4 * i)[0] for i in range(nfunc)]
                for i in range(nname):
                    name_rva, = struct.unpack_from('<I', d, self.offset(aname) + 4 * i)
                    ordinal, = struct.unpack_from('<H', d, self.offset(aord) + 2 * i)
                    o = self.offset(name_rva)
                    name = d[o:d.index(b'\0', o)].decode('ascii', 'replace')
                    self._exports.append((functions[ordinal], name))
                self._exports.sort()
        return self._exports


def unwind_one(dump, image, base, regs):
    """1 frame 戻す。戻せなければ理由の文字列を返す。"""
    rip = regs['rip']
    rva = rip - base
    function = image.function(rva)
    if function is None:
        # leaf 関数 (stack を使わない): 戻り先は rsp の先頭。
        ret = dump.u64(regs['rsp'])
        if ret is None:
            return 'stack の memory が dump に無い'
        regs['rip'] = ret
        regs['rsp'] += 8
        return None
    begin = function[0]
    unwind_rva = function[2]
    in_prolog = rva - begin
    first = True
    machine_frame = False
    while True:
        at = image.offset(unwind_rva)
        d = image.data
        version_flags, prolog_size, count, frame = struct.unpack_from('<BBBB', d, at)
        version, flags = version_flags & 7, version_flags >> 3
        if version not in (1, 2):
            return f'UNWIND_INFO の版 {version} を扱えない'
        frame_register, frame_offset = frame & 15, frame >> 4
        codes = [struct.unpack_from('<BB', d, at + 4 + 2 * i) for i in range(count)]
        i = 0
        while i < count:
            code_offset, op_info = codes[i]
            op, info = op_info & 15, op_info >> 4
            # prolog の途中なら、まだ実行していない操作は戻さない (chain の先は常に全部戻す)。
            applies = not first or code_offset <= in_prolog
            if op == 0:  # PUSH_NONVOL
                if applies:
                    value = dump.u64(regs['rsp'])
                    if value is None:
                        return 'stack の memory が dump に無い'
                    regs[REGISTERS[info]] = value
                    regs['rsp'] += 8
                i += 1
            elif op == 1:  # ALLOC_LARGE
                if info == 0:
                    size = codes[i + 1][0] | codes[i + 1][1] << 8
                    size *= 8
                    i += 2
                else:
                    lo = codes[i + 1][0] | codes[i + 1][1] << 8
                    hi = codes[i + 2][0] | codes[i + 2][1] << 8
                    size = lo | hi << 16
                    i += 3
                if applies:
                    regs['rsp'] += size
            elif op == 2:  # ALLOC_SMALL
                if applies:
                    regs['rsp'] += info * 8 + 8
                i += 1
            elif op == 3:  # SET_FPREG
                if applies:
                    regs['rsp'] = regs[REGISTERS[frame_register]] - frame_offset * 16
                i += 1
            elif op in (4, 5):  # SAVE_NONVOL / SAVE_NONVOL_FAR
                if op == 4:
                    offset = (codes[i + 1][0] | codes[i + 1][1] << 8) * 8
                    i += 2
                else:
                    offset = (codes[i + 1][0] | codes[i + 1][1] << 8) | (codes[i + 2][0] | codes[i + 2][1] << 8) << 16
                    i += 3
                if applies:
                    value = dump.u64(regs['rsp'] + offset)
                    if value is None:
                        return 'stack の memory が dump に無い'
                    regs[REGISTERS[info]] = value
            elif op == 6:  # EPILOG (版 2)。戻す操作ではない。
                i += 2 if version == 2 else 1
            elif op == 8:  # SAVE_XMM128
                i += 2
            elif op == 9:  # SAVE_XMM128_FAR
                i += 3
            elif op == 10:  # PUSH_MACHFRAME
                if applies:
                    sp = regs['rsp'] + (8 if info else 0)
                    rip_value, rsp_value = dump.u64(sp), dump.u64(sp + 24)
                    if rip_value is None or rsp_value is None:
                        return 'stack の memory が dump に無い'
                    regs['rip'], regs['rsp'] = rip_value, rsp_value
                    machine_frame = True
                i += 1
            else:
                return f'unwind の操作 {op} を扱えない'
        if flags & 4:  # CHAININFO: 親の関数の unwind 情報へ続く
            chained = at + 4 + 2 * (count + (count & 1))
            _begin, _end, unwind_rva = struct.unpack_from('<III', d, chained)
            first = False
            continue
        break
    if not machine_frame:
        ret = dump.u64(regs['rsp'])
        if ret is None:
            return 'stack の memory が dump に無い'
        regs['rip'] = ret
        regs['rsp'] += 8
    return None


class Symbolizer:
    def __init__(self, addr2line):
        self.addr2line = addr2line
        self.images = {}
        self.cache = {}

    def image(self, module):
        """dump の module と同じ build の disk の image。違えば (None, 理由)。"""
        path = module['path']
        if path not in self.images:
            if not os.path.exists(path):
                self.images[path] = (None, 'image が disk に無い')
            else:
                try:
                    img = Image(path)
                    if img.timestamp != module['timestamp'] or img.size != module['size']:
                        self.images[path] = (None, f'disk の image が dump と別の build (TimeDateStamp {img.timestamp:#x} != {module["timestamp"]:#x})')
                    else:
                        self.images[path] = (img, None)
                except (OSError, ValueError) as e:
                    self.images[path] = (None, f'image を読めない: {e}')
        return self.images[path]

    def name(self, module, rva, call_site):
        key = (module['path'], rva)
        if key in self.cache:
            return self.cache[key]
        img, _ = self.image(module)
        text = ''
        if img is not None:
            lookup = rva - 1 if call_site else rva
            # Windows の DLL には DWARF が無い。それ以外 (build の image・UCRT64 の DLL) は addr2line を試す。
            if self.addr2line and not module['path'].lower().startswith('c:\\windows\\'):
                result = subprocess.run([self.addr2line, '-f', '-C', '-i', '-e', module['path'],
                                         hex(img.image_base + lookup)], capture_output=True, text=True)
                lines = [x for x in result.stdout.splitlines() if x]
                pairs = [f'{lines[k]} {lines[k + 1]}' for k in range(0, len(lines) - 1, 2)]
                if pairs and not pairs[0].startswith('??'):
                    text = ' <- '.join(pairs)
            if not text:
                exports = img.exports()
                i = bisect.bisect_right(exports, (lookup, '￿')) - 1
                if i >= 0:
                    text = f'{exports[i][1]}+{lookup - exports[i][0]:#x} (export 表の直前の名前。推定)'
        self.cache[key] = text
        return text


def describe(dump, symbolizer, address, call_site):
    module = dump.module_of(address)
    if module is None:
        return {'address': f'{address:#x}', 'module': None}
    rva = address - module['base']
    return {'address': f'{address:#x}', 'module': os.path.basename(module['path']), 'rva': f'{rva:#x}',
            'symbol': symbolizer.name(module, rva, call_site)}


def unwind(dump, symbolizer, context, limit=128):
    regs = dict(context)
    frames, stop = [], None
    for depth in range(limit):
        rip = regs['rip']
        frame = describe(dump, symbolizer, rip, call_site=depth > 0)
        frame['kind'] = 'unwind'
        frames.append(frame)
        module = dump.module_of(rip)
        if module is None:
            stop = 'rip が module の外'
            break
        img, why = symbolizer.image(module)
        if img is None:
            stop = f'{os.path.basename(module["path"])}: {why}'
            break
        before = regs['rsp']
        try:
            stop = unwind_one(dump, img, module['base'], regs)
        except (ValueError, struct.error, IndexError) as e:
            stop = f'unwind 情報を読めない: {e}'
        if stop:
            break
        if regs['rip'] == 0:
            stop = '最後の frame (rip = 0)'
            break
        if regs['rsp'] <= before:
            stop = 'rsp が増えない (unwind の誤り)'
            break
    else:
        stop = f'{limit} frame で打ち切り'
    return frames, stop


def scan(dump, symbolizer, rsp, size=0x2000, limit=48):
    out = []
    for at in range(rsp, rsp + size, 8):
        value = dump.u64(at)
        if value is None:
            break
        if dump.module_of(value):
            frame = describe(dump, symbolizer, value, call_site=True)
            frame['kind'] = 'scan'
            frame['stack_slot'] = f'{at:#x}'
            out.append(frame)
            if len(out) >= limit:
                break
    return out


def analyze(path, addr2line):
    dump = Dump(path)
    symbolizer = Symbolizer(addr2line)
    report = {'dump': path, 'modules': [], 'threads': []}
    for m in dump.modules:
        img, why = symbolizer.image(m)
        report['modules'].append({'path': m['path'], 'base': f'{m["base"]:#x}', 'size': m['size'],
                                  'timestamp': f'{m["timestamp"]:#x}', 'image_matches': img is not None,
                                  'note': why})
    ex = dump.exception
    if ex:
        report['exception'] = {'thread': ex['thread'], 'code': f'{ex["code"]:#010x}',
                               'flags': ex['flags'], 'parameters': [f'{p:#x}' for p in ex['parameters']],
                               'address': describe(dump, symbolizer, ex['address'], call_site=False)}
    for t in dump.threads:
        faulting = ex is not None and t['id'] == ex['thread']
        context = ex['context'] if faulting and ex['context'] else t['context']
        if context is None:
            continue
        frames, stop = unwind(dump, symbolizer, context)
        entry = {'id': t['id'], 'faulting': faulting, 'unwind': frames, 'unwind_stopped': stop}
        if faulting or not frames or len(frames) < 3:
            entry['scan'] = scan(dump, symbolizer, context['rsp'])
        report['threads'].append(entry)
    report['threads'].sort(key=lambda t: not t['faulting'])
    return report


def render(report):
    lines = [f'dump: {report["dump"]}']
    ex = report.get('exception')
    if ex:
        a = ex['address']
        lines.append(f'例外: {ex["code"]} thread {ex["thread"]} 引数 {ex["parameters"]} '
                     f'address {a["address"]} {a.get("module")}+{a.get("rva")} {a.get("symbol", "")}')
    mismatched = [m for m in report['modules'] if not m['image_matches']]
    lines.append(f'module: {len(report["modules"])} 個 (disk の image と一致しない・無い: {len(mismatched)} 個)')
    for t in report['threads']:
        lines.append(f'--- thread {t["id"]}{" (例外)" if t["faulting"] else ""}  [unwind]')
        for i, f in enumerate(t['unwind']):
            lines.append(f'  #{i:<2} {f["address"]} {f.get("module")}+{f.get("rva")}  {f.get("symbol", "")}')
        lines.append(f'  (unwind の終わり: {t["unwind_stopped"]})')
        if 'scan' in t:
            lines.append('  [scan: heuristic。本当の戻り先でない値を含む]')
            for f in t['scan']:
                lines.append(f'    {f["stack_slot"]}: {f["address"]} {f.get("module")}+{f.get("rva")}  {f.get("symbol", "")}')
    return '\n'.join(lines)


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = argv[1]
    addr2line = None
    out = None
    i = 2
    while i < len(argv):
        if argv[i] == '--addr2line':
            addr2line = argv[i + 1]
            i += 2
        elif argv[i] == '--json':
            out = argv[i + 1]
            i += 2
        else:
            print(f'不明な引数: {argv[i]}', file=sys.stderr)
            return 2
    report = analyze(path, addr2line)
    if out:
        with open(out, 'x', encoding='utf-8') as f:
            json.dump(report, f, ensure_ascii=False, indent=1)
    sys.stdout.reconfigure(encoding='utf-8')
    print(render(report))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
