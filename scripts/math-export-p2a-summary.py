"""P2-A の report (mvm_math_score_export_parity) を 1 行ずつの表にする。値は report から読むだけ。

使い方: python math-export-p2a-summary.py <directory> [--json <出力>]
directory の下の */*-report.json をすべて読む (case 名は親 directory の名前)。
"""

import csv
import glob
import json
import os
import sys


def digests(path):
    if not os.path.exists(path):
        return {}
    return {row[0]: row[2] for row in csv.reader(open(path, encoding='utf-8'), delimiter='\t')}


def main():
    root = sys.argv[1]
    rows = []
    for path in sorted(glob.glob(root + '/**/*-report.json', recursive=True)):
        report = json.load(open(path, encoding='utf-8'))
        probe = report['property_probe']
        case = os.path.relpath(os.path.dirname(path), root).replace('\\', '/')
        stages = {item['stage']: item['t'] for item in report['mlt_stages']}
        render = stages.get('consumer_close', stages.get('failed', 0)) - stages.get('render', 0)
        get = probe['mlt_properties_get']
        rows.append(dict(
            case=case, fixture=report['fixture'], verdict=report['verdict'],
            staging=report['main_staging'].split(' ')[0] + ('+split' if report.get('split_transitions') else ''),
            stride=report['keyframe_stride'], transitions=report.get('overlay_transitions', 0),
            range=[report['range_first'], report['range_count']],
            render_threads=report['render_threads_override'] or 4,
            layers_max=report['max_concurrent_math'], clips=report['clips'],
            images=probe['composited_images'], keyframes=probe['animation_keyframes'],
            samples=probe['animation_samples'],
            compose_ms=probe['compose_ms_per_image'],
            string_get_ms=probe['animated_string_get_ms_per_image'],
            string_get_share=probe['animated_string_get_share_of_compose'],
            string_get_calls=sum(get[name]['calls'] for name in ('rect', 'fix_rotate_x', 'fix_shear_x')),
            anim_get_ms=1e3 * sum(probe[function][name]['seconds']
                                  for function in ('mlt_properties_anim_get_rect', 'mlt_properties_anim_get_double')
                                  for name in probe[function]) / max(1, probe['composited_images']),
            other_get_ms=1e3 * get['other']['seconds'] / max(1, probe['composited_images']),
            graph_build_s=probe['graph_build_seconds'], render_s=render,
            wall_s=report['wall_seconds'], cpu_s=report['cpu_seconds'],
            cpu_per_wall=report['cpu_seconds'] / report['wall_seconds'] if report['wall_seconds'] else 0,
            probe_hooks=report['property_probe_hooks'], probe_threads=report['property_probe_threads'],
            digest_file=os.path.join(os.path.dirname(path), report['fixture'] + '-pre-yuv.tsv')))
    for row in rows:
        print(f"{row['case']:44} {row['verdict'][:10]:10} {row['staging']:18} L={row['layers_max']:2} tr={row['transitions']:4} "
              f"img={row['images']:5} key={row['keyframes']:7} | 合成 {row['compose_ms']:8.1f} ms "
              f"文字列化 {row['string_get_ms']:8.1f} ms ({row['string_get_share'] * 100:5.1f}%) "
              f"anim_get {row['anim_get_ms']:6.2f} | build {row['graph_build_s']:5.2f}s "
              f"render {row['render_s']:6.1f}s cpu/wall {row['cpu_per_wall']:.2f}")
    if '--json' in sys.argv:
        with open(sys.argv[sys.argv.index('--json') + 1], 'w', encoding='utf-8') as out:
            json.dump(rows, out, ensure_ascii=False, indent=1)
    # 同じ画素になるはずの組 (--pairs a=b,...) の digest を比べる。frame 番号は各 report の中の番号。
    if '--pairs' in sys.argv:
        by_case = {row['case']: row for row in rows}
        for pair in sys.argv[sys.argv.index('--pairs') + 1].split(','):
            left, right = pair.split('=')
            if left not in by_case or right not in by_case:
                print(f'digest {left} = {right}: 比較できません (report がありません)')
                continue
            a, b = digests(by_case[left]['digest_file']), digests(by_case[right]['digest_file'])
            common = set(a) & set(b)
            differing = sum(a[frame] != b[frame] for frame in common)
            print(f'digest {left} = {right}: 共通 {len(common)} frame、不一致 {differing}'
                  + ('' if common else ' (比較した frame が無いので一致とは言えません)'))


if __name__ == '__main__':
    main()
