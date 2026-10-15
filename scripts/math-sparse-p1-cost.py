"""数式 layer の合成対象画素を、Project と数式の cache から数える (P1 のコスト分析)。

MLT の affine は layer ごとに出力の全画素を走査し、素材の範囲内の画素すべてで補間と合成を行う
(transition_affine.c の sliced_proc)。全画面の透過 PNG では、字形の有無によらず全画素が対象になる。
ここでは frame ごとに、有効な数式 layer の数と、次の 3 通りの合成対象画素を数える。
  full      全画面 (現在の製品)
  anchored  左上を原点のまま、字形の右端・下端 + 余白 2 画素まで
  bbox      字形の範囲 + 余白 2 画素
拡縮・回転・crop を持つ clip は切り出しの対象外なので、anchored / bbox でも全画面として数える。
位置は合成対象の画素数に影響しない (範囲の大きさだけで決まる) ので見ない。

使い方: python math-sparse-p1-cost.py <project.mvm> <report.json (math_artifacts を含む)> <出力.json>
"""

import json
import os
import statistics
import sys

from PIL import Image

MARGIN = 2


def main():
    project_path, report_path, output_path = sys.argv[1:4]
    project = json.load(open(project_path, encoding='utf-8'))
    report = json.load(open(report_path, encoding='utf-8'))
    width, height = project['output_width'], project['output_height']
    full = width * height
    cache = os.path.join(os.path.dirname(os.path.abspath(project_path)), 'cache', 'math',
                         os.path.basename(project_path))
    artifacts = {item['clip']: item['png'] for item in report['math_artifacts']}
    boxes = {}
    clips = []
    for clip in project['timeline_clips']:
        if clip['kind'] != 'math' or not clip.get('enabled', True):
            continue
        png = artifacts.get(clip['id'])
        if png is None:
            raise SystemExit(f"成果物の記録が無い数式 clip があります: {clip['id']}")
        if png not in boxes:
            alpha = Image.open(os.path.join(cache, png)).convert('RGBA').getchannel('A')
            box = alpha.getbbox()
            if box is None:
                raise SystemExit(f'字形の無い成果物です: {png}')
            boxes[png] = (box[2] - box[0], box[3] - box[1])
        box_width, box_height = boxes[png]
        effects = clip['effects']
        eligible = (effects['scale_x_percent'] == 100 and effects['scale_y_percent'] == 100
                    and not effects['scale_x_keys'] and not effects['scale_y_keys']
                    and effects['rotation_degrees'] == 0 and not effects['rotation_keys']
                    and all(effects[f'crop_{side}_percent'] == 0 and not effects[f'crop_{side}_keys']
                            for side in ('left', 'top', 'right', 'bottom')))
        # 数式は出力の中央に置かれる (余りは左上寄せ)。
        left = (width - box_width) // 2
        top = (height - box_height) // 2
        bbox_pixels = min(width, box_width + 2 * MARGIN) * min(height, box_height + 2 * MARGIN)
        anchored_pixels = min(width, left + box_width + MARGIN) * min(height, top + box_height + MARGIN)
        start = clip['timeline_start_frame']
        end = start + clip['source_out_frame'] - clip['source_in_frame']
        clips.append(dict(start=start, end=end, eligible=eligible, box=[box_width, box_height],
                          bbox=bbox_pixels if eligible else full,
                          anchored=anchored_pixels if eligible else full))
    total = max(clip['end'] for clip in clips)
    layers = [0] * total
    pixels = {name: [0] * total for name in ('bbox', 'anchored')}
    for clip in clips:
        for frame in range(clip['start'], min(clip['end'], total)):
            layers[frame] += 1
            pixels['bbox'][frame] += clip['bbox']
            pixels['anchored'][frame] += clip['anchored']
    full_total = sum(layers) * full
    ratios = [clip['box'][0] * clip['box'][1] / full for clip in clips]
    summary = dict(
        project=os.path.basename(project_path), canvas=[width, height], frames=total,
        math_clips=len(clips), eligible_clips=sum(clip['eligible'] for clip in clips),
        distinct_artifacts=len(boxes),
        glyph_box_area_ratio=dict(min=min(ratios), median=statistics.median(ratios),
                                  mean=statistics.fmean(ratios), max=max(ratios)),
        layers_per_frame=dict(min=min(layers), mean=statistics.fmean(layers), max=max(layers)),
        composited_pixels_per_frame=dict(
            full=dict(mean=full_total / total, max=max(layers) * full),
            anchored=dict(mean=sum(pixels['anchored']) / total, max=max(pixels['anchored'])),
            bbox=dict(mean=sum(pixels['bbox']) / total, max=max(pixels['bbox']))),
        total_ratio_to_full=dict(anchored=sum(pixels['anchored']) / full_total,
                                 bbox=sum(pixels['bbox']) / full_total),
        # 10 frame ごとの抜き取り (frame, layer 数, full, anchored, bbox の画素数)。
        samples=[[frame, layers[frame], layers[frame] * full, pixels['anchored'][frame],
                  pixels['bbox'][frame]] for frame in range(0, total, 10)])
    with open(output_path, 'w', encoding='utf-8') as out:
        json.dump(summary, out, ensure_ascii=False, indent=1)
    print(json.dumps({key: value for key, value in summary.items() if key != 'samples'},
                     ensure_ascii=False, indent=1))


if __name__ == '__main__':
    main()
