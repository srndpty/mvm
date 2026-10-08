"""P4-0 の独立検証。C++ の点列を実 Manim で直接 RGBA に描く。"""

import time

START = time.perf_counter()
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import traceback

import manim
import numpy as np
from PIL import Image
from manim import Axes, MathTex, Scene, VMobject, tempconfig

IMPORT_SECONDS = time.perf_counter() - START


def check(value, message):
    if not value:
        raise ValueError(message)


def validate_raster(pixels):
    check(pixels.shape == (480, 854, 4), '描画の寸法または channel が一致しません')
    check(np.any(pixels[:, :, 3] > 0), '可視 fixture の画素が空です')


def validate_label(label, source):
    check(label.tex_string == source and len(label.get_all_points()) > 0,
          'ラベルの構造または glyph が一致しません')


def compile_curve(executable, source, viewport):
    result = subprocess.run([str(executable), source, *map(str, viewport)],
                            capture_output=True, text=True, timeout=30)
    row = json.loads(result.stdout)
    check(result.returncode in (0, 2), "式の試作が異常終了しました")
    check((result.returncode == 0) == (row['status'] == 'Finite'), "終了値と状態が一致しません")
    return row


def clipped(a, b, viewport, cut):
    xmin, xmax, ymin, ymax = viewport
    xmax = min(xmax, cut)
    dx, dy = b[0]-a[0], b[1]-a[1]
    low, high = 0., 1.
    for p, q in ((-dx, a[0]-xmin), (dx, xmax-a[0]),
                 (-dy, a[1]-ymin), (dy, ymax-a[1])):
        if p == 0:
            if q < 0:
                return None
        elif p < 0:
            low = max(low, q/p)
        else:
            high = min(high, q/p)
    if low >= high:
        return None
    return [(a[0]+t*dx, a[1]+t*dy) for t in (low, high)]


def graph_objects(spec, curves, progress):
    xmin, xmax, ymin, ymax = spec['viewport']
    objects = []

    def coordinate(point):
        return np.array([-5+(point[0]-xmin)/(xmax-xmin)*10,
                         -2.8125+(point[1]-ymin)/(ymax-ymin)*5.625, 0])

    def line(points, color, width):
        objects.append(VMobject(stroke_color=color, stroke_width=width)
                       .set_points_as_corners([coordinate(p) for p in points]))

    if spec.get('grid', True):
        for x in np.linspace(xmin, xmax, 11):
            line([(x, ymin), (x, ymax)], '#333A44', .8)
        for y in np.linspace(ymin, ymax, 9):
            line([(xmin, y), (xmax, y)], '#333A44', .8)
    if xmin <= 0 <= xmax:
        line([(0, ymin), (0, ymax)], '#FFFFFF', 2)
    if ymin <= 0 <= ymax:
        line([(xmin, 0), (xmax, 0)], '#FFFFFF', 2)
    colors = ['#FF6655', '#55CCFF', '#FFE066']
    cut = xmin+(xmax-xmin)*progress
    for index, curve in enumerate(curves):
        for segment in curve['segments']:
            # 切断済みの辺だけを描き、Manim の補間・plot は使わない。
            run = []
            for a, b in zip(segment, segment[1:]):
                edge = clipped(a, b, spec['viewport'], cut)
                if edge is None:
                    if len(run) >= 2:
                        line(run, colors[index], 3)
                    run = []
                else:
                    if run and not np.allclose(run[-1], edge[0], rtol=0, atol=1e-12):
                        line(run, colors[index], 3)
                        run = []
                    if not run:
                        run.append(edge[0])
                    run.append(edge[1])
            if len(run) >= 2:
                line(run, colors[index], 3)
    for i, source in enumerate(spec.get('labels', [])):
        label = MathTex(source, font_size=20)
        validate_label(label, source)
        limit = 5 if i == 1 else 10
        if label.width > limit:
            label.scale(limit/label.width)
        if i == 1:
            label.rotate(np.pi/2)
        label.move_to(([0, -3.25, 0], [-5.7, 0, 0], [0, 3.25, 0])[i])
        objects.append(label)
    return objects


def render(scene, spec, curves, progress, path):
    objects = graph_objects(spec, curves, progress)
    scene.camera.reset()
    scene.camera.capture_mobjects(objects)
    pixels = np.array(scene.camera.pixel_array)
    validate_raster(pixels)
    Image.fromarray(pixels).save(path)
    return pixels


def main():
    parser = argparse.ArgumentParser(description='P4-0 の実 Manim 検証')
    parser.add_argument('compiler', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    opts = parser.parse_args()
    out = opts.output.resolve()
    if out.exists():
        parser.error('出力先が存在します。新しい証拠 directory が必要です')
    out.mkdir(parents=True)
    report = {'schema': 'mvm-graph-p40-spike/1', 'manim': manim.__version__,
              'import_seconds': IMPORT_SECONDS, 'cases': [], 'negative': [], 'failures': []}
    try:
        check(manim.__version__ == '0.21.0', 'Manim 0.21.0 が必要です')
        report['toolchain'] = {tool: subprocess.check_output([tool, '--version'], text=True).splitlines()[0]
                               for tool in ('latex', 'dvisvgm')}
        startup = time.perf_counter()
        probe = subprocess.run([sys.executable, '-c', 'import manim; print(manim.__version__)'],
                               capture_output=True, text=True, timeout=30)
        report['renderer_startup_seconds'] = time.perf_counter()-startup
        report['startup_probe'] = {'exit': probe.returncode, 'stdout': probe.stdout, 'stderr': probe.stderr}
        check(probe.returncode == 0 and probe.stdout.strip() == '0.21.0', 'renderer 起動の確認が失敗しました')
        report['prototype'] = json.loads(subprocess.check_output([str(opts.compiler), '--test'], text=True))
        negatives = [('x+', 'InvalidExpression'), ('foo(x)', 'UnsupportedExpression'),
                     ("__import__('os').system('echo bad')", 'InvalidExpression'),
                     ('x.real', 'InvalidExpression'), ('x[0]', 'InvalidExpression'),
                     ('lambda x:x', 'UnsupportedExpression'), ('[x for x in x]', 'InvalidExpression'),
                     ('open(1)', 'UnsupportedExpression'), ('sqrt(-1)', 'NoFiniteSamples')]
        for source, expected in negatives:
            row = compile_curve(opts.compiler, source, [-5, 5, -5, 5])
            report['negative'].append({'source': source, **row, 'expected': expected})
            check(row['status'] == expected, '負例の拒否理由が一致しません')
        row = compile_curve(opts.compiler, 'x', [5, -5, -5, 5])
        report['negative'].append({'case': 'invalid-viewport', **row})
        check(row['status'] == 'InvalidViewport', '不正 viewport が通過しました')
        for source, poles in [('1/(x-.013)', [.013]), ('tan(x)',
                              [-1.5*np.pi, -.5*np.pi, .5*np.pi, 1.5*np.pi])]:
            curve = compile_curve(opts.compiler, source, [-5, 5, -5, 5])
            check(curve['status'] == 'Finite' and len(curve['segments']) >= 2,
                  '不連続の fixture が空振りです')
            check(all(not a[0] < pole < b[0] for s in curve['segments']
                      for a, b in zip(s, s[1:]) for pole in poles), '標本間の極を接続しました')
            report['negative'].append({'case': 'discontinuity', 'source': source,
                                       'segments': len(curve['segments']), 'rejected_bridge': True})
        cases = [
            ('square', ['x^2'], [-5, 5, -5, 25], {}),
            ('sin', ['sin(x)'], [-5, 5, -2, 2], {}),
            ('reciprocal', ['1/x'], [-5, 5, -5, 5], {}),
            ('sqrt', ['sqrt(x)'], [-5, 5, -2, 4], {}),
            ('tan', ['tan(x)'], [-5, 5, -5, 5], {}),
            ('log', ['log(x)'], [-5, 5, -5, 5], {}),
            ('two', ['x^2', 'sin(x)'], [-5, 5, -5, 25], {}),
            ('three', ['x^2', 'sin(x)', '1/x'], [-5, 5, -5, 25], {}),
            ('off-origin', ['x^2'], [1, 5, 1, 25], {}),
            ('labels', ['x^2', 'sin(x)', '1/x'], [-5, 5, -5, 25],
             {'labels': [r'x\quad\text{long horizontal axis label}',
                         r'y\quad\text{long vertical axis label}',
                         r'y=x^2,\quad y=\sin(x),\quad y=\frac{1}{x}']}),
            ('no-grid', ['x^2'], [-5, 5, -5, 25], {'grid': False}),
        ]
        with tempconfig({'tex_dir': str(out/'Tex'), 'media_dir': str(out/'media'),
                         'pixel_width': 854, 'pixel_height': 480, 'frame_width': 13.34375,
                         'frame_height': 7.5, 'background_opacity': 0, 'disable_caching': True}):
            scene = Scene()
            # 比較候補 A。callback は固定した安全な AST の演算だけから作る。
            axes = Axes(x_range=[-5, 5, 1], y_range=[-5, 5, 1], tips=False)
            begin = time.perf_counter()
            callback = axes.plot(lambda x: 1/x, x_range=[-5, 5, .02],
                                 discontinuities=[0], use_smoothing=False)
            report['backend_sampling'] = {'seconds': time.perf_counter()-begin,
                'points': len(callback.get_all_points()), 'subpaths': len(callback.get_subpaths()),
                'requires_explicit_discontinuities': True}
            scene.camera.reset()
            scene.camera.capture_mobjects([axes, callback])
            Image.fromarray(np.array(scene.camera.pixel_array)).save(out/'backend-plot.png')
            for name, sources, viewport, extra in cases:
                begin = time.perf_counter()
                spec = {'viewport': viewport, **extra}
                curves = [compile_curve(opts.compiler, source, viewport) for source in sources]
                check(all(c['status'] == 'Finite' for c in curves), '描画候補の式が失敗しました')
                (out/(name+'-geometry.json')).write_text(json.dumps(curves), encoding='utf-8')
                if name in ('reciprocal', 'tan'):
                    check(all(not (a[0] < pole < b[0])
                              for c in curves for s in c['segments'] for a, b in zip(s, s[1:])
                              for pole in ([0] if name == 'reciprocal' else
                                           [-1.5*np.pi, -.5*np.pi, .5*np.pi, 1.5*np.pi])),
                          '不連続点を辺が横断しました')
                static_path = out/(name+'.png')
                pixels = render(scene, spec, curves, 1, static_path)
                if name == 'three':
                    static_pixels = pixels.copy()
                alpha = pixels[:, :, 3]
                check(np.any(alpha > 0) and np.any(alpha == 0) and np.any((alpha > 0) & (alpha < 255)),
                      '透明背景または反エイリアスがありません')
                if name == 'three':
                    report['resolved_colors'] = ['#FFFF6655', '#FF55CCFF', '#FFFFE066']
                    report['opaque_curve_color_pixels'] = []
                    for color in ([255, 102, 85, 255], [85, 204, 255, 255], [255, 224, 102, 255]):
                        count = int(np.count_nonzero(np.all(pixels == color, axis=2)))
                        check(count > 0, '解決済みの曲線色の画素がありません')
                        report['opaque_curve_color_pixels'].append(count)
                report['cases'].append({'name': name, 'static_seconds': time.perf_counter()-begin,
                    'bytes': static_path.stat().st_size, 'rgba_bytes': pixels.nbytes,
                    'sha256': hashlib.sha256(pixels.tobytes()).hexdigest(),
                    'partial_alpha': int(np.count_nonzero((alpha > 0) & (alpha < 255))),
                    'transparent_rgb_nonzero': int(np.count_nonzero(pixels[alpha == 0, :3])),
                    'segments': [len(c['segments']) for c in curves],
                    'undefined': [c['undefined'] for c in curves], 'jumps': [c['jumps'] for c in curves]})
                if name == 'three':
                    report['draw'] = []
                    for n in (1, 3):
                        begin = time.perf_counter()
                        monotonic = {}
                        for i in range(n+1):
                            if i == n:
                                generation_seconds = time.perf_counter()-begin
                            p = out/f'draw-{n}-{i}.png'
                            monotonic[i] = render(scene, spec, curves, i/n, p)
                        check(np.array_equal(monotonic[n], static_pixels), 'Draw の端点が static と一致しません')
                        if n == 3:
                            check(len({hashlib.sha256(v.tobytes()).hexdigest() for v in monotonic.values()}) == 4,
                                  'Draw が空振りで一致しました')
                        order = [n-1, 0, n//2, n]
                        for request, i in enumerate(order):
                            # 毎回新しい camera と object から始め、animation 履歴を持たない。
                            direct = render(Scene(), spec, curves, i/n, out/f'direct-{n}-{request}-{i}.png')
                            check(np.array_equal(direct, monotonic[i]), '任意順の frame が一致しません')
                        report['draw'].append({'frames': n, 'requests': order, 'equal': True,
                            'generation_seconds': generation_seconds,
                            'seconds': time.perf_counter()-begin,
                            'bytes': sum((out/f'draw-{n}-{i}.png').stat().st_size for i in range(n)),
                            'hashes': [hashlib.sha256(monotonic[i].tobytes()).hexdigest() for i in range(n+1)]})
            label = MathTex('x')
            try:
                validate_label(label, 'y')
            except ValueError:
                report['negative'].append({'case': 'label-structure-mismatch', 'rejected': True})
            else:
                raise ValueError('誤った label 構造が通過しました')
            # 正常系と同じ検査を、寸法と空 alpha の変異へ適用する。
            for bad in (np.zeros((1, 1, 4), dtype=np.uint8), np.zeros((480, 854, 4), dtype=np.uint8)):
                try:
                    validate_raster(bad)
                except ValueError:
                    report['negative'].append({'case': 'raster-mismatch', 'rejected': True})
                else:
                    raise ValueError('破損 raster が通過しました')
    except Exception:
        report['failures'].append(traceback.format_exc())
    report['total_seconds'] = time.perf_counter()-START
    report['artifact_bytes'] = sum(p.stat().st_size for p in out.rglob('*') if p.is_file())
    (out/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('検証完了: ' + str(out/'report.json'))
    return bool(report['failures'])


if __name__ == '__main__':
    raise SystemExit(main())
