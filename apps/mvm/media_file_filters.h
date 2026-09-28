#ifndef MVM_APPS_MVM_MEDIA_FILE_FILTERS_H
#define MVM_APPS_MVM_MEDIA_FILE_FILTERS_H

#include <QString>
#include <QStringList>

namespace mvm::app {

// ファイル選択ダイアログの拡張子の表。**UX のためだけに使い、判定には使わない。**
// 素材の種別は probeMediaFile が内容で決める (拡張子を変えた素材も読める)。
// 「すべて」も必ず選べるようにしているので、表に無い形式も試せる。
//
// 画像は静止画 decoder の許可表 (media_stream_facts.cpp の kStillImageCodecs) が扱う形式、
// 音声・動画は UCRT64 の FFmpeg が demux できる代表的な形式を並べた。
inline QStringList mediaFileNameFilters() {
    const QString video = QStringLiteral(
        "*.mp4 *.mov *.mkv *.ts *.m2ts *.mts *.avi *.webm *.wmv *.mpg *.mpeg *.m4v *.3gp *.mxf");
    const QString audio = QStringLiteral(
        "*.wav *.mp3 *.m4a *.aac *.flac *.ogg *.oga *.opus *.wma *.aif *.aiff *.ac3 *.mka");
    const QString image = QStringLiteral(
        "*.png *.jpg *.jpeg *.jpe *.jfif *.bmp *.dib *.gif *.webp *.tif *.tiff *.tga *.qoi *.psd "
        "*.jxl *.jp2 *.dds *.pcx *.ppm *.pgm *.pbm *.pnm *.sgi");
    return {QStringLiteral("メディア (") + video + u' ' + audio + u' ' + image + u')',
            QStringLiteral("動画 (") + video + u')', QStringLiteral("音声 (") + audio + u')',
            QStringLiteral("画像 (") + image + u')', QStringLiteral("すべて (*)")};
}

} // namespace mvm::app

#endif // MVM_APPS_MVM_MEDIA_FILE_FILTERS_H
