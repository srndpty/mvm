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
    return dict(format='mvm-math-score/1', id='試験', replace_clip_ids=['元'],
                canvas=[1920, 1080], fps=[60, 1],
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
                    dict(id='字幕', start_frame=120, end_frame=240, content='変更しない本文')]))


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
        first = next(c for c in result['timeline_clips'] if c['id'] == report['objects']['a'][0])
        second = next(c for c in result['timeline_clips'] if c['id'] == report['objects']['a'][1])
        self.assertEqual(first['track_index'], second['track_index'])
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

    def test_unused_template_track_is_kept_and_generated_tracks_follow_it(self):
        base = project()
        result, _ = score_module.compile_score(recipe(), base)
        self.assertEqual(result['video_tracks'][0], base['video_tracks'][0])
        self.assertGreater(len(result['video_tracks']), 1)
        self.assertTrue(all(clip['track_index'] >= 1 for clip in result['timeline_clips']))
        self.assertTrue(all(track['muted'] is False and track['solo'] is False and
                            track['mixer_gain_db'] == 0.0 and track['mixer_pan'] == 0.0
                            for track in result['video_tracks'][1:]))

    def test_original_tracks_and_unrelated_content_survive_repeated_generation(self):
        data = recipe()
        data['preserve_clip_ids'] = ['ユーザー数式']
        base = project()
        base['video_tracks'] = [
            dict(name='背景', muted=True, solo=False, mixer_name='', mixer_gain_db=0.0, mixer_pan=0.0),
            dict(name='空き', muted=False, solo=False, mixer_name='', mixer_gain_db=0.0, mixer_pan=0.0),
            dict(name='手前', muted=False, solo=False, mixer_name='', mixer_gain_db=0.0, mixer_pan=0.0),
        ]
        base['audio_tracks'] = [dict(name='ナレーション', muted=False, solo=True, mixer_name='主音声',
                                     mixer_gain_db=-6.0, mixer_pan=0.5)]
        base['timeline_clips'].extend([
            dict(id='上の映像', kind='video', track_kind='video', track_index=2, name='既存'),
            dict(id='ユーザー数式', kind='math', track_kind='video', track_index=2, effects={}, math={}),
            dict(id='mvm-math-score:別の配置:項:段階', kind='math', track_kind='video', track_index=2,
                 effects={}, math={}),
        ])
        base['timeline_markers'] = [17]
        base['subtitles']['cues'].append(
            dict(id='別字幕', start_frame=10, end_frame=20, content='残す', link_clip_id='ユーザー数式'))
        first, _ = score_module.compile_score(data, base)
        self.assertEqual(first['video_tracks'][:3], base['video_tracks'])
        self.assertEqual(first['audio_tracks'], base['audio_tracks'])
        self.assertTrue(all(track['muted'] is False for track in first['video_tracks'][3:]))
        ids = [clip['id'] for clip in first['timeline_clips']]
        self.assertEqual(ids.count('上の映像'), 1)
        self.assertEqual(ids.count('ユーザー数式'), 1)
        self.assertEqual(ids.count('mvm-math-score:別の配置:項:段階'), 1)
        self.assertNotIn('元', ids)
        self.assertEqual(next(clip['track_index'] for clip in first['timeline_clips']
                              if clip['id'] == '上の映像'), 2)
        self.assertEqual(next(clip['track_index'] for clip in first['timeline_clips']
                              if clip['id'] == 'ユーザー数式'), 2)
        self.assertIn(17, first['timeline_markers'])
        unrelated = next(cue for cue in first['subtitles']['cues'] if cue['id'] == '別字幕')
        self.assertEqual(unrelated, dict(id='別字幕', start_frame=10, end_frame=20, content='残す',
                                         link_clip_id='ユーザー数式'))
        second, _ = score_module.compile_score(data, first)
        self.assertEqual(second, first)
        self.assertEqual([clip['id'] for clip in second['timeline_clips']].count('ユーザー数式'), 1)

    def test_unclassified_math_clip_is_rejected(self):
        base = project()
        base['timeline_clips'].append(
            dict(id='ユーザー数式', kind='math', track_kind='video', track_index=0, effects={}, math={}))
        with self.assertRaisesRegex(score_module.ScoreError, '所有者'):
            score_module.compile_score(recipe(), base)

    def test_unclaimed_marker_overlapping_a_score_marker_is_rejected(self):
        base = project()
        base['timeline_markers'] = [0]
        with self.assertRaisesRegex(score_module.ScoreError, 'マーカー'):
            score_module.compile_score(recipe(), base)

    def test_subtitle_link_to_replaced_clip_requires_explicit_rebind(self):
        base = project()
        base['subtitles']['cues'][0]['link_clip_id'] = '元'
        with self.assertRaisesRegex(score_module.ScoreError, '付け直す'):
            score_module.compile_score(recipe(), base)

    def test_explicit_subtitle_rebind_detaches_only_the_removed_clip(self):
        data = recipe()
        data['states'][1]['subtitle_cue_id'] = '字幕'
        base = project()
        base['subtitles']['cues'][0]['link_clip_id'] = '元'
        result, _ = score_module.compile_score(data, base)
        cue = result['subtitles']['cues'][0]
        self.assertNotIn('link_clip_id', cue)
        self.assertEqual((cue['start_frame'], cue['end_frame'], cue['content']), (120, 240, '変更しない本文'))

    def test_rebinding_a_subtitle_linked_to_a_surviving_clip_is_rejected(self):
        data = recipe()
        data['preserve_clip_ids'] = ['ユーザー数式']
        data['states'][1]['subtitle_cue_id'] = '別字幕'
        base = project()
        base['timeline_clips'].append(
            dict(id='ユーザー数式', kind='math', track_kind='video', track_index=0, effects={}, math={}))
        base['subtitles']['cues'].append(
            dict(id='別字幕', start_frame=1, end_frame=2, content='残す', link_clip_id='ユーザー数式'))
        with self.assertRaisesRegex(score_module.ScoreError, '結び付いた'):
            score_module.compile_score(data, base)

    def test_malformed_owned_clip_id_is_rejected(self):
        base = project()
        base['timeline_clips'].append(
            dict(id='mvm-math-score:壊れた', kind='math', track_kind='video', track_index=0,
                 effects={}, math={}))
        with self.assertRaisesRegex(score_module.ScoreError, '所有ID'):
            score_module.compile_score(recipe(), base)

    def test_regeneration_without_marker_ownership_is_rejected(self):
        first, _ = score_module.compile_score(recipe(), project())
        del first[score_module.OWNERSHIP_KEY]
        with self.assertRaisesRegex(score_module.ScoreError, '所有記録'):
            score_module.compile_score(recipe(), first)

    def test_regeneration_without_ownership_is_rejected_even_with_zero_markers(self):
        # マーカーが残っていなくても、足したtrackの記録が無い再生成は推測になる。
        first, _ = score_module.compile_score(recipe(), project())
        generated = [clip['id'] for clip in first['timeline_clips']
                     if clip['id'].startswith('mvm-math-score:試験:')]
        self.assertTrue(generated)
        del first[score_module.OWNERSHIP_KEY]
        first['timeline_markers'] = []
        before = copy.deepcopy(first)
        with self.assertRaisesRegex(score_module.ScoreError, '所有記録'):
            score_module.compile_score(recipe(), first)
        self.assertEqual(first, before)

    def test_zero_marker_regeneration_with_ownership_is_still_accepted(self):
        # 対照群: 上の拒否がマーカー0件そのものではなく、所有記録の欠落で起きることを示す。
        first, _ = score_module.compile_score(recipe(), project())
        owned = first[score_module.OWNERSHIP_KEY]['scores']['試験']
        first['timeline_markers'] = []
        owned['marker_frames'] = []
        second, _ = score_module.compile_score(recipe(), first)
        self.assertEqual(sorted(clip['id'] for clip in second['timeline_clips']),
                         sorted(clip['id'] for clip in first['timeline_clips']))

    def test_generated_track_name_collision_is_rejected(self):
        base = project()
        base['video_tracks'].append(
            dict(name='部分式 1', muted=False, solo=False, mixer_name='', mixer_gain_db=0.0, mixer_pan=0.0))
        with self.assertRaisesRegex(score_module.ScoreError, 'track名'):
            score_module.compile_score(recipe(), base)

    def test_transition_touching_a_surviving_clip_is_rejected(self):
        data = recipe()
        data['preserve_clip_ids'] = ['ユーザー数式']
        base = project()
        base['timeline_clips'].append(
            dict(id='ユーザー数式', kind='math', track_kind='video', track_index=0, effects={}, math={}))
        base['timeline_transitions'] = [
            dict(id='境界', incoming_clip_id='ユーザー数式', outgoing_clip_id='元')]
        with self.assertRaisesRegex(score_module.ScoreError, 'トランジション'):
            score_module.compile_score(data, base)

    def test_same_clip_cannot_be_preserved_and_replaced(self):
        data = recipe()
        data['preserve_clip_ids'] = ['元']
        with self.assertRaisesRegex(score_module.ScoreError, '両方'):
            score_module.compile_score(data, project())

    def test_invalid_json_preserves_the_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            score = folder / 'score.json'
            base = folder / 'base.mvm'
            output = folder / 'output.mvm'
            report = folder / 'report.json'
            score.write_text('{', encoding='utf-8')
            base.write_text(json.dumps(project()), encoding='utf-8')
            output.write_text('既存の保存結果', encoding='utf-8')
            report.write_text('既存の検証結果', encoding='utf-8')
            arguments = ['math_score', '--score', str(score), '--project', str(base),
                         '--output', str(output), '--report', str(report), '--validator', '検証CLI']
            with patch.object(sys, 'argv', arguments), \
                    patch.object(score_module.subprocess, 'run') as validator:
                with self.assertRaises(SystemExit) as error:
                    score_module.main()
            self.assertEqual(error.exception.code, 1)
            self.assertEqual(output.read_text(encoding='utf-8'), '既存の保存結果')
            self.assertEqual(report.read_text(encoding='utf-8'), '既存の検証結果')
            self.assertEqual(validator.call_count, 0)
            self.assertFalse(any(path.suffix == '.mvm' and path.name not in {'base.mvm', 'output.mvm'}
                                 for path in folder.iterdir()))

    def test_repeated_publication_is_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            score = folder / 'score.json'
            base = folder / 'base.mvm'
            output = folder / 'output.mvm'
            report = folder / 'report.json'
            score.write_text(json.dumps(recipe()), encoding='utf-8')
            base.write_text(json.dumps(project()), encoding='utf-8')
            arguments = ['math_score', '--score', str(score), '--project', str(base),
                         '--output', str(output), '--report', str(report), '--validator', '検証CLI']
            accepted = type('結果', (), dict(returncode=0, stderr=b''))()
            with patch.object(sys, 'argv', arguments), \
                    patch.object(score_module.subprocess, 'run', return_value=accepted):
                score_module.main()
            first_project = output.read_bytes()
            first_report = report.read_bytes()
            with patch.object(sys, 'argv', arguments), \
                    patch.object(score_module.subprocess, 'run', return_value=accepted):
                score_module.main()
            self.assertEqual(output.read_bytes(), first_project)
            self.assertEqual(report.read_bytes(), first_report)


if __name__ == '__main__':
    unittest.main()
