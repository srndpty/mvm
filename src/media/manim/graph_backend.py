"""検証済み中立点列だけを raster 化する。式の評価・sampling・時間進行は行わない。"""
import hashlib
import inspect
import json
import pathlib
import shutil
import subprocess
import sys

import cairo
import manim
import numpy as np
import PIL
from PIL import Image
from manim import MathTex, VMobject, config, tempconfig
from manim.camera.camera import Camera


def environment():
    if manim.__version__ != "0.21.0":
        raise RuntimeError("Manim 0.21.0 が必要です")
    values = {"manim": manim.__version__, "python": sys.version,
              "numpy": np.__version__, "pillow": PIL.__version__,
              "cairo": cairo.cairo_version_string(), "pycairo": cairo.version,
              "camera_source": hashlib.sha256(inspect.getsource(Camera).encode()).hexdigest(),
              "vmobject_source": hashlib.sha256(inspect.getsource(VMobject).encode()).hexdigest(),
              "tex_template": hashlib.sha256(config.tex_template.body.encode()).hexdigest()}
    for tool in ("latex", "dvisvgm", "kpsewhich"):
        executable = shutil.which(tool)
        if executable is None:
            raise RuntimeError(tool + " がありません")
        values[tool] = subprocess.check_output([executable, "--version"], timeout=10).decode(errors="strict").strip()
        values[tool + "_sha256"] = hashlib.sha256(pathlib.Path(executable).read_bytes()).hexdigest()
    for font in ("cmr10.tfm", "cmmi10.tfm", "cmsy10.tfm", "cmex10.tfm"):
        path = subprocess.check_output([shutil.which("kpsewhich"), font], timeout=10).decode().strip()
        if not path:
            raise RuntimeError("TeX font を解決できません: " + font)
        values[font] = hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()
    return "mvm-graph-toolchain/1\n" + "".join(k + "=" + json.dumps(v, ensure_ascii=True) + "\n" for k, v in sorted(values.items()))


def compose(under, mask, argb):
    # RGBA8 の各段階で整数丸め。Cairo の premultiplied RGB は使わず alpha 被覆だけを読む。
    a = (mask.astype(np.uint64) * (argb >> 24) + 127) // 255
    b = under[:, :, 3].astype(np.uint64)
    weight = a * 255 + b * (255 - a)
    out = np.zeros_like(under)
    for i, shift in enumerate((16, 8, 0)):
        numerator = ((argb >> shift) & 255) * a * 255 + under[:, :, i].astype(np.uint64) * b * (255 - a)
        out[:, :, i] = np.where(weight != 0, (numerator + weight // 2) // np.maximum(weight, 1), 0)
    out[:, :, 3] = (weight + 127) // 255
    return out


def render():
    pathlib.Path("phase.txt").write_text("startup", encoding="utf-8")
    request_bytes = pathlib.Path("request.json").read_bytes()
    if len(request_bytes) > 32 * 1024 * 1024:
        raise RuntimeError("request の上限を超えています")
    request = json.loads(request_bytes)
    if request["schema"] != "mvm-graph-render/1":
        raise RuntimeError("request schema が不正です")
    actual = environment()
    if not request["toolchain"].startswith(actual):
        raise RuntimeError("toolchain が preflight から変わりました")
    width, height = request["width"], request["height"]
    if not (0 < width <= 8192 and 0 < height <= 8192 and width * height <= 16777216):
        raise RuntimeError("canvas budget が不正です")
    report = []
    label_report = []
    with tempconfig({"pixel_width": width, "pixel_height": height, "frame_width": float(width),
                     "frame_height": float(height), "background_opacity": 0,
                     "tex_dir": "Tex", "media_dir": "media", "disable_caching": True}):
        camera = Camera(background_opacity=0)
        labels = []
        for index, label in enumerate(request["labels"]):
            if not label["text"]:
                continue
            try:
                glyph = MathTex(label["text"], font_size=48)
            except Exception as error:
                print("ラベルを描けません: " + str(error), file=sys.stderr)
                raise SystemExit(42) from error
            if not any(len(item.points) for item in glyph.get_family()) or glyph.width <= 0 or glyph.height <= 0:
                print("非空ラベルに drawable glyph がありません", file=sys.stderr)
                raise SystemExit(43)
            if label["rotated"]:
                glyph.rotate(np.pi / 2)
            left, top, bw, bh = label["content"]
            scale = min(bw / glyph.width, bh / glyph.height)
            glyph.scale(scale)
            glyph.move_to([left + bw / 2 - width / 2, height / 2 - top - bh / 2, 0])
            labels.append(glyph)
            bounds = [float(glyph.get_left()[0] + width / 2), float(height / 2 - glyph.get_top()[1]),
                      float(glyph.width), float(glyph.height)]
            band_left, band_top, band_width, band_height = label["band"]
            if bounds[0] < band_left or bounds[1] < band_top or bounds[0] + bounds[2] > band_left + band_width or bounds[1] + bounds[3] > band_top + band_height:
                raise RuntimeError("ラベルが指定帯からはみ出しています")
            label_report.append({"index": index, "text": glyph.tex_string, "bounds": bounds})
        frames = request["frames"]
        # 表示状態ではない照合専用 endpoint。進捗をここで計算せず中立な静止点列を再描画する。
        if len(frames) > 1:
            frames = frames + [{**frames[0], "index": "endpoint"}]
        for frame in frames:
            pathlib.Path("phase.txt").write_text("static" if frame["index"] == -1 else "draw", encoding="utf-8")
            pixels = np.zeros((height, width, 4), dtype=np.uint8)
            groups = [[] for _ in range(request["function_count"])]
            structure = f'{frame["index"]} {width} {height} {len(groups)} {frame["phase"][0]} {frame["phase"][1]}'
            for path_index, path in enumerate(frame["paths"]):
                points = np.asarray(path["points"], dtype=float)
                if points.ndim != 2 or points.shape[1] != 2 or len(points) < 2 or not np.isfinite(points).all():
                    raise RuntimeError("点列が不正です")
                mapped = np.column_stack((points[:, 0] - width / 2, height / 2 - points[:, 1], np.zeros(len(points))))
                # Camera は1 scene unit = 1 pixel。stroke_width * cairo_line_width_multiple を pixel 幅にする。
                obj = VMobject(stroke_color="#FFFFFF", stroke_opacity=1,
                               stroke_width=path["width"] / camera.cairo_line_width_multiple,
                               fill_opacity=0).set_points_as_corners(mapped)
                if path["function"] >= 0:
                    groups[path["function"]].append(obj)
                anchors = obj.get_start_anchors()
                if len(anchors) != len(points) - 1 or not np.array_equal(anchors, mapped[:-1]) or not np.array_equal(obj.get_end(), mapped[-1]):
                    raise RuntimeError("backend が点列を変えました")
                actual_points = np.vstack((anchors, obj.get_end()))
                geometry_hash = hashlib.sha256(actual_points.astype("<f8").tobytes()).hexdigest()
                structure += f' {path["function"]}:{path["segment"]}:{len(anchors) + 1}:{path["argb"]}:{path["width"]:.17g}:{geometry_hash}'
                camera.reset()
                camera.capture_mobjects([obj])
                mask = camera.pixel_array[:, :, 3].copy()
                # stroke の外側も plot の境界で切る。pixel center が plot 内の画素だけ採用する。
                left, top, pw, ph = request["plot"]
                xx = np.arange(width) + .5
                yy = np.arange(height) + .5
                mask[:, (xx < left) | (xx >= left + pw)] = 0
                mask[(yy < top) | (yy >= top + ph), :] = 0
                if frame["index"] == -1:
                    diagnostic = np.zeros((height, width, 4), dtype=np.uint8)
                    diagnostic[:, :, :3] = np.where(mask[:, :, None] > 0, 255, 0)
                    diagnostic[:, :, 3] = mask
                    Image.fromarray(diagnostic).save(f"coverage-{path_index}.png")
                pixels = compose(pixels, mask, path["argb"])
            for label in labels:
                camera.reset()
                camera.capture_mobjects([label])
                pixels = compose(pixels, camera.pixel_array[:, :, 3], 0xFFFFFFFF)
            name = "endpoint.png" if frame["index"] == "endpoint" else "static.png" if frame["index"] == -1 else f'frame-{frame["index"]}.png'
            Image.fromarray(pixels).save(name)
            if frame["index"] != "endpoint":
                report.append(structure + "\n")
    pathlib.Path("structure.txt").write_text("".join(report), encoding="utf-8")
    pathlib.Path("labels.txt").write_text("".join(str(item["index"]) + " " + item["text"].encode("utf-8").hex() + " " +
        " ".join(format(value, ".17g") for value in item["bounds"]) + "\n" for item in label_report), encoding="utf-8")


if __name__ == "__main__":
    if sys.argv[1:] == ["--fingerprint"]:
        print(environment(), end="")
    elif sys.argv[1:] == ["--render"]:
        render()
    else:
        raise SystemExit("呼び出し引数が不正です")
