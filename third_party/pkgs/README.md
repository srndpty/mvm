# third_party/pkgs

`scripts/freeze-deps.ps1` が MSYS2 の pacman キャッシュから、実際にインストールされた
`.pkg.tar.zst` をここへ退避する。

## なぜ必要か

MSYS2 は rolling repository であり、古い version はミラーから消える。凍結しておかないと
数か月後に「同じ構成でビルドし直す」ことができなくなり、Phase 0 の判定根拠が再現不能になる。

## git 管理外である理由

合計で数百 MB〜1 GB 規模の binary であり、リポジトリに入れるのは現実的でない。
git にコミットするのは `docs/deps-lock.txt`（package 名と正確な version の記録）のみ。

このディレクトリの中身は、開発機のバックアップ対象として別途保全すること。

## 復元

```powershell
pwsh scripts/bootstrap-msys2.ps1 -FromFrozen
```
