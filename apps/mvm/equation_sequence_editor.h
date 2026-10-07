#ifndef MVM_APPS_MVM_EQUATION_SEQUENCE_EDITOR_H
#define MVM_APPS_MVM_EQUATION_SEQUENCE_EDITOR_H
// P3-5: EquationSequence の authoring UI と Project の操作の間に立つ controller。
//
//   QML (EquationSequenceInspector.qml)   表示用の値 (view / status) を読み、利用者の意図を呼ぶ
//       ↓
//   EquationSequenceEditor                UI の選択 (Project ではない) と、意図を domain の操作へ写す
//       ↓
//   MvmController::editEquationSequenceData   一操作 = Project の確定一回 = Undo 一回
//
// QML は TeX・PartId の参照・時間・compile の状態・byte offset を解釈しない。部分式の範囲は
// QML から UTF-16 の選択で受け取り、ここで domain の検査付き変換 (core::utf16ToUtf8Offset) を通す。
// 状態の問い合わせ (refreshStatus) は読むだけで、描画を要求しない (P3-4.1)。
#include "project/equation_binding_edit.h"
#include "project/equation_sequence.h"
#include "project/project.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTextDocument>
#include <QVariantMap>

namespace mvm::app {
class MvmController;

class EquationSequenceEditor : public QObject {
    Q_OBJECT
    // 選択中の EquationSequence clip の構造と選択 (Project が変わるか選択が変わると作り直す)。
    // clip が選ばれていなければ空。
    Q_PROPERTY(QVariantMap view READ view NOTIFY viewChanged)
    // preview / 描画の状態 (読むだけ)。refreshStatus で作り直す。
    Q_PROPERTY(QVariantMap status READ status NOTIFY statusChanged)

public:
    explicit EquationSequenceEditor(MvmController& controller);
    ~EquationSequenceEditor() override;

    QVariantMap view() const { return view_; }
    QVariantMap status() const { return status_; }

    // ---- 選択 (UI の状態。Project には保存しない) ----
    Q_INVOKABLE void selectState(const QString& stateId);
    Q_INVOKABLE void selectPart(const QString& partId);
    Q_INVOKABLE void selectAction(const QString& actionId);

    // ---- 状態 ----
    Q_INVOKABLE bool insertState(bool after);
    Q_INVOKABLE bool deleteSelectedState();
    Q_INVOKABLE bool moveSelectedState(int delta);
    Q_INVOKABLE bool setHoldFrames(qint64 frames);
    Q_INVOKABLE bool setTransitionFrames(const QString& transitionId, qint64 frames);

    // ---- 式の編集 (編集欄の session。確定で Project の操作一回) ----
    // textDocument は QML の TextEdit.textDocument (QQuickTextDocument) か QTextDocument。
    // 確定までの変更を document の contentsChange で記録し、記録が今の文字と一致するときだけ
    // P3-2 の信頼済み編集として確定する。一致しなければ全体置換 (全 binding を無効) にする。
    Q_INVOKABLE void beginSourceEdit(QObject* textDocument);
    Q_INVOKABLE QVariantMap commitSourceEdit(const QString& text);
    Q_INVOKABLE void cancelSourceEdit();
    Q_INVOKABLE bool sourceEditActive() const { return session_.has_value(); }

    // ---- 部分式 (選択は UTF-16。editorText は編集欄の今の文字で、確定済みの式と一致すること) ----
    Q_INVOKABLE bool addPart(int selectionStart, int selectionEnd, const QString& editorText,
                             const QString& label);
    // Invalid / Bound の部分式は同じ PartId へ結び直し、欠落 (Missing) は同じ PartId で作り直す。
    Q_INVOKABLE bool rebindSelectedPart(int selectionStart, int selectionEnd,
                                        const QString& editorText);
    Q_INVOKABLE bool renameSelectedPart(const QString& label);
    Q_INVOKABLE bool deleteSelectedPart();

    // ---- 変形の明示対応 ----
    Q_INVOKABLE bool addCorrespondence(const QString& transitionId, const QString& fromPart,
                                       const QString& toPart);
    Q_INVOKABLE bool removeCorrespondence(const QString& transitionId, const QString& fromPart,
                                          const QString& toPart);

    // ---- action (outline / pulse だけ) ----
    Q_INVOKABLE bool addAction(const QString& partId, const QString& operation, qint64 start,
                               qint64 duration);
    Q_INVOKABLE bool updateSelectedAction(const QString& partId, const QString& operation,
                                          qint64 start, qint64 duration);
    Q_INVOKABLE bool deleteSelectedAction();
    // 利用者の明示の操作でだけ再生位置を action の先頭へ動かす (編集では動かさない)。
    Q_INVOKABLE bool seekToSelectedAction();

    // 読むだけ: MvmController::equationSequencePreviewStatus を引き直す。描画を要求しない。
    Q_INVOKABLE void refreshStatus();

    // 試験用: 状態の問い合わせの回数 (QML の polling が実際に通っていること)。
    int statusRefreshCountForTest() const { return statusRefreshes_; }

Q_SIGNALS:
    void viewChanged();
    void statusChanged();

private:
    // 今の選択と、利用者が最後に明示的に選んだ ID (wanted)。選んだ ID が Undo などで消えたら
    // 近い生存を選び、同じ ID が Redo などで戻れば wanted の ID を選び直す。
    struct Selection {
        QString clipId;
        project::StateId state;
        std::size_t stateIndex = 0;
        std::optional<project::PartId> part;
        std::size_t partIndex = 0;
        std::optional<project::ActionId> action;
        std::optional<project::StateId> wantedState;
        std::optional<project::PartId> wantedPart;
        std::optional<project::ActionId> wantedAction;
    };
    struct SourceSession {
        QString clipId;
        project::StateId state;
        QPointer<QTextDocument> document;
        QMetaObject::Connection connection;
        std::string baseSource;
        QString shadow;
        std::vector<project::TrustedEquationEdit> edits;
        bool trusted = true;
        bool external = false; // document の外で始めた (記録が無い)
    };

    const project::TimelineClip* currentClip() const;
    const project::EquationState* selectedState() const;
    void onControllerStateChanged();
    void rebuildView();
    void resolveSelection();
    void setMessage(const QString& message, bool error);
    // 一操作 = editEquationSequenceData 一回 (Undo 一回)。拒否なら Project を変えず message を残す。
    bool commit(const std::function<bool(project::EquationSequenceClipData&, std::string&)>& edit,
                const QString& done);
    void onContentsChange(int position, int removed, int added);
    void endSession();
    std::optional<std::pair<std::int64_t, std::int64_t>>
    selectionBytes(int selectionStart, int selectionEnd, const QString& editorText,
                   QString& error) const;
    std::string newId() const;

    MvmController& controller_;
    QMetaObject::Connection stateConnection_;
    Selection selection_;
    std::optional<SourceSession> session_;
    // view を作り直す必要があるかの比較 (data・選択・fps・message が同じなら作り直さない)。
    std::optional<project::EquationSequenceClipData> viewData_;
    QString viewKey_;
    QVariantMap view_;
    QVariantMap status_;
    QString message_;
    bool messageError_ = false;
    int statusRefreshes_ = 0;
};
} // namespace mvm::app
#endif
