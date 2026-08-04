// mvm Phase 0 / S1 - Qt probe
//
// 目的:
//   リンクされた Qt が UCRT64 版であり、実際に起動できることを確認する。
//
//   この開発機には他プロジェクト用の Qt 6.8.3 (MSVC ビルド) が
//   C:/Users/lambe/sdk/Qt/6.8.3 にある。configure 時のガード
//   (cmake/mvm_toolchain_guard.cmake) に加え、実行時にも実体を出力して
//   目視で確認できるようにしておく。
//
// 終了コード: 0 = 正常

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QString>
#include <QSysInfo>
#include <QTextStream>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "=== mvm Phase 0 / S1 : Qt probe ===\n";
    out << "Qt version   : " << QT_VERSION_STR << " (runtime " << qVersion() << ")\n";
    out << "build ABI    : " << QSysInfo::buildAbi() << '\n';
    out << "prefix       : " << QLibraryInfo::path(QLibraryInfo::PrefixPath) << '\n';
    out << "libraries    : " << QLibraryInfo::path(QLibraryInfo::LibrariesPath) << '\n';
    out << "QML imports  : " << QLibraryInfo::path(QLibraryInfo::QmlImportsPath) << '\n';
    out << "plugins      : " << QLibraryInfo::path(QLibraryInfo::PluginsPath) << '\n';

    const QString prefix = QLibraryInfo::path(QLibraryInfo::PrefixPath);
    const bool isUcrt64 = prefix.contains(QStringLiteral("msys64/ucrt64"), Qt::CaseInsensitive) ||
                          prefix.contains(QStringLiteral("msys64\\ucrt64"), Qt::CaseInsensitive);

    out << '\n';
    if (isUcrt64) {
        out << "OK   UCRT64 版の Qt を使用しています。\n";
        return 0;
    }

    out << "FAIL Qt が UCRT64 の外を指しています: " << prefix << '\n';
    out << "     mvm Phase 0 は MSYS2 UCRT64 で統一する方針です。\n";
    return 1;
}
