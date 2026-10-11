"""数式 layer の渡し方の比較 (P1) の report を表にする。値は report から読むだけで、再計算しない。

使い方: python math-sparse-p1-summary.py <run directory> [--json <出力>]
"""

import glob
import json
import sys


def main():
    root = sys.argv[1]
    rows = []
    for path in sorted(glob.glob(root + '/*/*-report.json')):
        report = json.load(open(path, encoding='utf-8'))
        compare = report.get('compare')
        case = path.replace('\\', '/').split('/')[-2]
        if not compare:
            print(f"{case:26} 比較なし verdict={report['verdict']} "
                  f"compose={report.get('compose_ms_per_image', 0):.1f}ms "
                  f"wall={report['wall_seconds']:.1f}s cpu={report['cpu_seconds']:.1f}s "
                  f"threads={report.get('render_threads_override', 0)}")
            continue
        base, cand, again = compare['baseline'], compare['candidate_run'], compare['baseline_repeat']
        # 改善率は candidate の直後に同じ条件で回した 2 回目の baseline と比べる。1 回目の baseline は
        # 全 frame の圧縮保持・FFmpeg の記録の集計・watchdog を同時に行うので条件が違う (参考値)。
        base_compose = again['compose_ms_per_image']
        base_render = again['render_seconds']
        row = dict(
            case=case, candidate=compare['candidate'], verdict=compare['verdict'],
            frames=compare['frames_expected'], exact=compare['frames_exact'],
            within_one=compare['frames_within_one'], beyond_one=compare['frames_beyond_one'],
            uncompared=compare['frames_uncompared'], differing_pixels=compare['differing_pixels'],
            pixels_beyond_one=compare['pixels_beyond_one'],
            max_difference=compare['max_channel_difference'],
            repeat_mismatches=compare['baseline_repeat_mismatches'],
            math_clips=compare['math_clips'], cropped_clips=compare['cropped_clips'],
            cropped_area_ratio_mean=compare['cropped_area_ratio_mean'],
            compose_ms_baseline_first=base['compose_ms_per_image'],
            compose_ms_baseline_repeat=again['compose_ms_per_image'],
            compose_ms_candidate=cand['compose_ms_per_image'],
            compose_speedup=base_compose / cand['compose_ms_per_image'] if cand['compose_ms_per_image'] else None,
            render_s_baseline_first=base['render_seconds'], render_s_baseline_repeat=again['render_seconds'],
            render_s_candidate=cand['render_seconds'],
            render_speedup=base_render / cand['render_seconds'] if cand['render_seconds'] else None,
            by_category=compare['by_category'], first_differing=compare['differing_frames_first'][:3])
        rows.append(row)
        print(f"{case:26} {row['candidate']:12} {row['verdict']:22} frames={row['frames']} "
              f"exact={row['exact']} ±1={row['within_one']} >1={row['beyond_one']} 未比較={row['uncompared']} "
              f"差のある画素={row['differing_pixels']} 最大差={row['max_difference']} "
              f"再現性の不一致={row['repeat_mismatches']}")
        print(f"{'':26} 切り出し {row['cropped_clips']}/{row['math_clips']} clip 面積比 {row['cropped_area_ratio_mean']:.4f} | "
              f"合成 {row['compose_ms_baseline_first']:.1f}/{row['compose_ms_baseline_repeat']:.1f} -> {row['compose_ms_candidate']:.1f} ms "
              f"(x{row['compose_speedup']:.2f}) | render {row['render_s_baseline_first']:.1f}/{row['render_s_baseline_repeat']:.1f} -> "
              f"{row['render_s_candidate']:.1f} s (x{row['render_speedup']:.2f})")
        for name, bucket in sorted(row['by_category'].items()):
            print(f"{'':28}{name:28} frames={bucket['frames']:4} exact={bucket['exact']:4} "
                  f"±1={bucket['within_one']:4} >1={bucket['beyond_one']:4}")
        for item in row['first_differing']:
            print(f"{'':28}例: frame {item['frame']} layers={item['layers']} 画素={item['differing_pixels']} "
                  f"最大差={item['max_channel_difference']} 範囲={item['region']}")
    if '--json' in sys.argv:
        with open(sys.argv[sys.argv.index('--json') + 1], 'w', encoding='utf-8') as out:
            json.dump(rows, out, ensure_ascii=False, indent=2)


if __name__ == '__main__':
    main()
