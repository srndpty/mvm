"""固定した項が動かないことと、不正な編集を拒否することを独立に検証する。"""

import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

root = Path(__file__).resolve().parents[2]
loader = importlib.util.spec_from_file_location('math_score', root / 'scripts/math_authoring/math_score.py')
score_module = importlib.util.module_from_spec(loader)
loader.loader.exec_module(score_module)


def recipe():
    return dict(format='mvm-math-score/1', id='試験', canvas=[1920, 1080], fps=[60, 1],
                objects={'x': {'source': 'x^2'}, 'a': {'source': 'a'}}, states=[
                    dict(id='元', duration_frames=120, transition_frames=0,
                         parts={'x': dict(x=800, y=500), 'a': dict(x=700, y=500)}),
                    dict(id='移動', duration_frames=120, transition_frames=30, moves=['x'], fixed=['a'],
                         parts={'x': dict(x=820, y=450), 'a': dict(x=700, y=500, opacity=50)}),
                    dict(id='消去', duration_frames=120, transition_frames=30, fixed=['x'],
                         parts={'x': dict(x=820, y=450)}),
                ])


def project():
    return dict(output_width=1920, output_height=1080, timeline_fps_num=60, timeline_fps_den=1,
                video_tracks=[dict(name='数式', muted=False, hidden=False)], timeline_markers=[],
                timeline_transitions=[], timeline_clips=[dict(id='元', kind='math', track_kind='video',
                                                              track_index=0, effects={}, math={})],
                subtitles=dict(style={'font_family': '元の書体'}, cues=[
                    dict(id='字幕', start_frame=120, end_frame=240, content='変更しない本文', link_clip_id='元')]))


class ScoreTests(unittest.TestCase):
    def test_fixed_part_has_no_position_curve(self):
        result, report = score_module.compile_score(recipe(), project())
        a = next(c for c in result['timeline_clips'] if c['math']['source'] == 'a')
        self.assertEqual(a['effects']['position_x_keys'], [])
        self.assertEqual(a['effects']['position_y_keys'], [])
        self.assertEqual([(k['frame'], k['value']) for k in a['effects']['opacity_keys']],
                         [(0, 100), (120, 100), (150, 50), (240, 50), (270, 0)])
        self.assertEqual(report['frames'], 360)

    def test_only_declared_part_moves(self):
        result, _ = score_module.compile_score(recipe(), project())
        x = next(c for c in result['timeline_clips'] if c['math']['source'] == 'x^2')
        self.assertEqual([k['frame'] for k in x['effects']['position_x_keys']], [0, 120, 150, 270])
        self.assertAlmostEqual(x['effects']['position_x_keys'][2]['value'], -7.291666666666667)
        self.assertEqual(x['effects']['opacity_keys'], [])

    def test_undeclared_move_is_rejected(self):
        data = recipe()
        data['states'][1]['moves'] = []
        with self.assertRaisesRegex(score_module.ScoreError, 'moves'):
            score_module.compile_score(data, project())

    def test_fixed_target_move_is_rejected(self):
        data = recipe()
        data['states'][1]['parts']['a']['x'] = 701
        with self.assertRaises(score_module.ScoreError):
            score_module.compile_score(data, project())

    def test_missing_part_is_rejected(self):
        data = recipe()
        data['states'][1]['parts']['unknown'] = dict(x=700, y=500)
        with self.assertRaisesRegex(score_module.ScoreError, '未定義'):
            score_module.compile_score(data, project())

    def test_nonfinite_coordinates_are_rejected(self):
        data = recipe()
        data['states'][1]['parts']['x']['x'] = float('nan')
        with self.assertRaisesRegex(score_module.ScoreError, '有限'):
            score_module.compile_score(data, project())

    def test_transition_must_leave_a_hold(self):
        data = recipe()
        data['states'][1]['transition_frames'] = 120
        with self.assertRaisesRegex(score_module.ScoreError, '未満'):
            score_module.compile_score(data, project())

    def test_appearance_delay_does_not_move_the_part(self):
        data = recipe()
        data['objects']['denominator'] = dict(source='a')
        data['states'][1]['parts']['denominator'] = dict(x=800, y=550, enter_delay_frames=20)
        result, _ = score_module.compile_score(data, project())
        c = next(c for c in result['timeline_clips'] if c['timeline_start_frame'] == 120)
        self.assertEqual(c['effects']['position_x_keys'], [])
        self.assertEqual([(k['frame'], k['value']) for k in c['effects']['opacity_keys']],
                         [(0,0), (20,0), (30,100), (120,100), (150,0)])

    def test_appearance_delay_outside_transition_is_rejected(self):
        data = recipe()
        data['states'][1]['parts']['x']['enter_delay_frames'] = 31
        with self.assertRaises(score_module.ScoreError):
            score_module.compile_score(data, project())

    def test_input_and_subtitle_are_preserved(self):
        data, base = recipe(), project()
        before_data, before_base = copy.deepcopy(data), copy.deepcopy(base)
        result, _ = score_module.compile_score(data, base)
        self.assertEqual(data, before_data)
        self.assertEqual(base, before_base)
        self.assertEqual(result['subtitles']['cues'][0]['content'], '変更しない本文')
        self.assertEqual(result['subtitles']['style']['font_family'], '元の書体')
        self.assertNotIn('link_clip_id', result['subtitles']['cues'][0])

    def test_native_ids_are_stable_after_retime(self):
        data = recipe()
        before, _ = score_module.compile_score(data, project())
        data['states'][0]['duration_frames'] = 180
        after, _ = score_module.compile_score(data, project())
        self.assertEqual([c['id'] for c in before['timeline_clips']], [c['id'] for c in after['timeline_clips']])
        self.assertEqual(after['timeline_clips'][0]['source_out_frame'],
                         before['timeline_clips'][0]['source_out_frame'] + 60)

    def test_bound_subtitle_follows_state_retime(self):
        data = recipe()
        data['states'][1]['subtitle_cue_id'] = '字幕'
        data['states'][0]['duration_frames'] = 180
        result, _ = score_module.compile_score(data, project())
        cue = result['subtitles']['cues'][0]
        self.assertEqual((cue['start_frame'], cue['end_frame']), (180,300))
        self.assertEqual(cue['content'], '変更しない本文')

    def test_missing_subtitle_binding_is_rejected(self):
        data = recipe()
        data['states'][1]['subtitle_cue_id'] = '存在しない字幕'
        with self.assertRaisesRegex(score_module.ScoreError, '存在しない'):
            score_module.compile_score(data, project())

    def test_project_without_subtitles_is_supported(self):
        base = project()
        base['subtitles'] = None
        result, _ = score_module.compile_score(recipe(), base)
        self.assertIsNone(result['subtitles'])

    def test_entrance_uses_explicit_start_and_target(self):
        data = recipe()
        data['objects']['new'] = dict(source=r'\pm')
        data['states'][1]['parts']['new'] = dict(x=1000, y=500, enter_from=dict(x=1200, y=500))
        result, report = score_module.compile_score(data, project())
        c = next(c for c in result['timeline_clips'] if c['id'] == report['objects']['new'][0])
        keys = c['effects']['position_x_keys']
        self.assertEqual([(k['frame'], round(k['value'], 6)) for k in keys],
                         [(0, 12.5), (30, 2.083333), (150, 2.083333)])

    def test_visible_part_cannot_use_entrance(self):
        data = recipe()
        data['states'][1]['parts']['x']['enter_from'] = dict(x=1000, y=500)
        with self.assertRaisesRegex(score_module.ScoreError, '非表示'):
            score_module.compile_score(data, project())

    def test_highlight_preserves_geometry_and_returns_to_transparency(self):
        data = recipe()
        data['states'][1]['highlight'] = ['x']
        result, report = score_module.compile_score(data, project())
        by_id = {c['id']: c for c in result['timeline_clips']}
        original = by_id[report['objects']['x'][0]]
        overlay = by_id[report['objects']['x/highlight'][0]]
        self.assertEqual(original['effects']['position_x_keys'], overlay['effects']['position_x_keys'])
        self.assertEqual(original['effects']['position_y_keys'], overlay['effects']['position_y_keys'])
        self.assertEqual(overlay['math']['color'], '#FFCC2222')
        self.assertEqual(overlay['effects']['opacity_keys'][-1]['value'], 0)

    def test_undefined_highlight_is_rejected(self):
        data = recipe()
        data['states'][1]['highlight'] = ['存在しない項']
        with self.assertRaisesRegex(score_module.ScoreError, '強調'):
            score_module.compile_score(data, project())

    def test_delayed_entrance_holds_start_until_delay(self):
        data = recipe()
        data['objects']['new'] = dict(source=r'\pm')
        data['states'][1]['parts']['new'] = dict(x=1000, y=500, enter_from=dict(x=1200, y=500),
                                               enter_delay_frames=15)
        result, report = score_module.compile_score(data, project())
        c = next(c for c in result['timeline_clips'] if c['id'] == report['objects']['new'][0])
        self.assertEqual([k['frame'] for k in c['effects']['position_x_keys']], [0,15,30,150])
        self.assertEqual(c['effects']['position_x_keys'][0]['value'],
                         c['effects']['position_x_keys'][1]['value'])

    def test_exit_can_finish_before_entrance(self):
        data = recipe()
        data['states'][2]['exit_frames'] = 12
        result, report = score_module.compile_score(data, project())
        c = next(c for c in result['timeline_clips'] if c['id'] == report['objects']['a'][0])
        self.assertEqual(c['effects']['opacity_keys'][-1]['frame'], 252)
        self.assertEqual(c['source_out_frame'], 253)

    def test_exit_cannot_exceed_transition(self):
        data = recipe()
        data['states'][2]['exit_frames'] = 31
        with self.assertRaisesRegex(score_module.ScoreError, '消去の尺'):
            score_module.compile_score(data, project())

    def test_entrance_cannot_start_outside_canvas(self):
        data = recipe()
        data['objects']['new'] = dict(source=r'\pm')
        data['states'][1]['parts']['new'] = dict(x=1000, y=500, enter_from=dict(x=1921, y=500))
        with self.assertRaisesRegex(score_module.ScoreError, '開始位置'):
            score_module.compile_score(data, project())

    def test_disappeared_part_reappears_at_its_new_position(self):
        data = recipe()
        data['states'].append(dict(id='再登場', duration_frames=120, transition_frames=30,
                                   parts={'x': dict(x=820, y=450), 'a': dict(x=1100, y=500)}))
        result, report = score_module.compile_score(data, project())
        self.assertEqual(len(report['objects']['a']), 2)
        second = next(c for c in result['timeline_clips'] if c['id'] == report['objects']['a'][1])
        self.assertEqual(second['effects']['position_x_keys'], [])
        self.assertEqual([(k['frame'], k['value']) for k in second['effects']['opacity_keys']], [(0, 0), (30, 100)])

    def test_native_rejection_preserves_published_project(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            score = folder / 'score.json'
            base = folder / 'base.mvm'
            output = folder / 'output.mvm'
            score.write_text(json.dumps(recipe()), encoding='utf-8')
            base.write_text(json.dumps(project()), encoding='utf-8')
            output.write_text('既存の保存結果', encoding='utf-8')
            arguments = ['math_score', '--score', str(score), '--project', str(base),
                         '--output', str(output), '--report', str(folder / 'report.json'),
                         '--validator', '製品の検証CLI']
            rejected = type('結果', (), dict(returncode=1, stderr='不正なProject'.encode('utf-8')))()
            with patch.object(sys, 'argv', arguments), patch.object(score_module.subprocess, 'run', return_value=rejected):
                with self.assertRaises(SystemExit) as error:
                    score_module.main()
            self.assertEqual(error.exception.code, 1)
            self.assertEqual(output.read_text(encoding='utf-8'), '既存の保存結果')
            self.assertEqual(sorted(p.name for p in folder.iterdir()), ['base.mvm', 'output.mvm', 'score.json'])

    def test_motion_defaults_to_ease_out_and_opacity_keeps_ease_in_out(self):
        result, _ = score_module.compile_score(recipe(), project())
        x = next(c for c in result['timeline_clips'] if c['math']['source'] == 'x^2')
        a = next(c for c in result['timeline_clips'] if c['math']['source'] == 'a')
        self.assertEqual({k['interpolation'] for k in x['effects']['position_x_keys']}, {'ease_out'})
        self.assertEqual({k['interpolation'] for k in a['effects']['opacity_keys']}, {'ease_in_out'})

    def test_linear_motion_can_be_selected(self):
        data = recipe()
        data['motion_interpolation'] = 'linear'
        result, _ = score_module.compile_score(data, project())
        x = next(c for c in result['timeline_clips'] if c['math']['source'] == 'x^2')
        self.assertEqual({k['interpolation'] for k in x['effects']['position_x_keys']}, {'linear'})

    def test_unknown_motion_is_rejected(self):
        data = recipe()
        data['motion_interpolation'] = '未知の補間'
        with self.assertRaisesRegex(score_module.ScoreError, '補間'):
            score_module.compile_score(data, project())

    def test_horizontal_scale_animates_without_vertical_scale(self):
        data = recipe()
        for stage in data['states'][1:]:
            stage['parts']['x']['scale_x'] = 125
        result, _ = score_module.compile_score(data, project())
        x = next(c for c in result['timeline_clips'] if c['math']['source'] == 'x^2')
        self.assertEqual([k['value'] for k in x['effects']['scale_x_keys']], [100, 100, 125, 125])
        self.assertEqual({k['interpolation'] for k in x['effects']['scale_x_keys']}, {'ease_out'})
        self.assertEqual(x['effects']['scale_y_keys'], [])

    def test_nonpositive_or_nonfinite_scale_is_rejected(self):
        for scale in (0, -1, float('nan'), float('inf')):
            with self.subTest(scale=scale):
                data = recipe()
                data['states'][0]['parts']['x']['scale_x'] = scale
                with self.assertRaisesRegex(score_module.ScoreError, '拡大率'):
                    score_module.compile_score(data, project())

    def test_scale_change_requires_declared_move(self):
        data = recipe()
        data['states'][1]['parts']['a']['scale_x'] = 125
        with self.assertRaises(score_module.ScoreError):
            score_module.compile_score(data, project())

    def test_zero_exit_hides_part_at_transition_boundary(self):
        data = recipe()
        data['states'][2]['exit_frames'] = 0
        result, _ = score_module.compile_score(data, project())
        a = next(c for c in result['timeline_clips'] if c['math']['source'] == 'a')
        self.assertEqual(a['effects']['opacity_keys'][-1]['frame'], 240)
        self.assertEqual(a['effects']['opacity_keys'][-1]['value'], 0)
        self.assertEqual(a['source_out_frame'], 241)

    def test_report_cannot_overwrite_input(self):
        arguments = ['math_score', '--score', 'score.json', '--project', 'base.mvm',
                     '--output', 'output.mvm', '--report', 'base.mvm', '--validator', '検証CLI']
        with patch.object(sys, 'argv', arguments):
            with self.assertRaises(SystemExit) as error:
                score_module.main()
        self.assertEqual(error.exception.code, 1)


if __name__ == '__main__':
    unittest.main()
