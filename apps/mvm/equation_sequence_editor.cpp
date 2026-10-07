#include "equation_sequence_editor.h"

#include "app/equation_sequence_authoring.h"
#include "core/source_frame_mapping.h"
#include "core/text_offsets.h"
#include "mvm_controller.h"
#include "project/equation_sequence_edit.h"

#include <algorithm>
#include <limits>

#include <QQuickTextDocument>
#include <QUuid>
#include <QVariantList>

namespace mvm::app {
namespace {
// 尺の入力の上限 (1 時間 @ 1000fps)。domain は overflow を検査するが、UI の入力の桁を抑える。
constexpr std::int64_t kMaximumEditFrames = 3'600'000;

QString qs(const std::string& text) {
    return QString::fromStdString(text);
}

const char* operationName(project::EquationOperation operation) {
    return operation == project::EquationOperation::Pulse ? "pulse" : "outline";
}

QString operationText(project::EquationOperation operation) {
    return operation == project::EquationOperation::Pulse ? QStringLiteral("pulse (拡大と強調色)")
                                                          : QStringLiteral("outline (囲み線)");
}

std::optional<project::EquationOperation> parseOperation(const QString& name) {
    if (name == QStringLiteral("outline"))
        return project::EquationOperation::Outline;
    if (name == QStringLiteral("pulse"))
        return project::EquationOperation::Pulse;
    return std::nullopt;
}

QString partDisplayLabel(const std::string& label, const std::string& text) {
    if (label.empty())
        return text.empty() ? QStringLiteral("(名前なし)")
                            : QStringLiteral("「") + qs(text) + QStringLiteral("」");
    return qs(label) + (text.empty() ? QString() : QStringLiteral("「") + qs(text) +
                                                       QStringLiteral("」"));
}

const project::SemanticPart* findPart(const project::EquationState& state,
                                      const project::PartId& id) {
    for (const auto& p : state.parts)
        if (p.id == id)
            return &p;
    return nullptr;
}

QTextDocument* documentOf(QObject* object) {
    if (auto* quick = qobject_cast<QQuickTextDocument*>(object))
        return quick->textDocument();
    return qobject_cast<QTextDocument*>(object);
}
} // namespace

EquationSequenceEditor::EquationSequenceEditor(MvmController& controller)
    : QObject(nullptr), controller_(controller) {
    stateConnection_ = connect(&controller_, &MvmController::stateChanged, this,
                               &EquationSequenceEditor::onControllerStateChanged);
}

EquationSequenceEditor::~EquationSequenceEditor() {
    disconnect(stateConnection_);
    endSession();
}

std::string EquationSequenceEditor::newId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

const project::TimelineClip* EquationSequenceEditor::currentClip() const {
    const auto& project = controller_.currentProject();
    const int index = controller_.currentClipIndex();
    if (index < 0 || index >= static_cast<int>(project.timelineClips.size()))
        return nullptr;
    const auto& clip = project.timelineClips[static_cast<std::size_t>(index)];
    return clip.kind == project::TimelineClipKind::EquationSequence ? &clip : nullptr;
}

const project::EquationState* EquationSequenceEditor::selectedState() const {
    const auto* clip = currentClip();
    if (!clip)
        return nullptr;
    for (const auto& s : clip->equationSequence.states)
        if (s.id == selection_.state)
            return &s;
    return nullptr;
}

void EquationSequenceEditor::onControllerStateChanged() {
    rebuildView();
}

void EquationSequenceEditor::setMessage(const QString& message, bool error) {
    message_ = message;
    messageError_ = error;
}

void EquationSequenceEditor::resolveSelection() {
    const auto* clip = currentClip();
    if (!clip) {
        selection_ = {};
        return;
    }
    const auto clipId = qs(clip->id);
    const auto& data = clip->equationSequence;
    if (selection_.clipId != clipId) {
        // 別の clip を選んだ。前の clip の UI の選択と message は持ち越さない。
        selection_ = {};
        selection_.clipId = clipId;
        message_.clear();
    }
    const auto findState = [&](const project::StateId& id) {
        return std::find_if(data.states.begin(), data.states.end(),
                            [&](const auto& s) { return s.id == id; });
    };
    if (selection_.wantedState && findState(*selection_.wantedState) != data.states.end() &&
        *selection_.wantedState != selection_.state) {
        // 消えていた選択が Undo / Redo で戻った。
        selection_.state = *selection_.wantedState;
        selection_.part.reset();
        selection_.action.reset();
    }
    const auto found = findState(selection_.state);
    if (found != data.states.end()) {
        selection_.stateIndex = static_cast<std::size_t>(found - data.states.begin());
    } else {
        // 削除・Undo などで消えた。同じ位置 (末尾を超えるなら最後) の生存を選び、部分式の選択は外す。
        const auto index = nearestSurvivingIndex(selection_.stateIndex, data.states.size());
        if (!index) {
            selection_.state = {};
            selection_.part.reset();
            selection_.action.reset();
            return;
        }
        selection_.stateIndex = *index;
        selection_.state = data.states[*index].id;
        selection_.part.reset();
        selection_.action.reset();
    }
    const auto parts = equationPartViews(data, selection_.state);
    if (selection_.wantedPart &&
        std::any_of(parts.begin(), parts.end(),
                    [&](const auto& v) { return v.id == *selection_.wantedPart; }))
        selection_.part = selection_.wantedPart;
    if (selection_.wantedAction &&
        std::any_of(data.actions.begin(), data.actions.end(), [&](const auto& a) {
            return a.id == *selection_.wantedAction && a.state == selection_.state;
        }))
        selection_.action = selection_.wantedAction;
    if (selection_.part) {
        const auto part = std::find_if(parts.begin(), parts.end(),
                                       [&](const auto& v) { return v.id == *selection_.part; });
        if (part != parts.end()) {
            selection_.partIndex = static_cast<std::size_t>(part - parts.begin());
        } else if (const auto index = nearestSurvivingIndex(selection_.partIndex, parts.size())) {
            selection_.partIndex = *index;
            selection_.part = parts[*index].id;
        } else {
            selection_.part.reset();
        }
    }
    if (selection_.action &&
        std::none_of(data.actions.begin(), data.actions.end(), [&](const auto& a) {
            return a.id == *selection_.action && a.state == selection_.state;
        }))
        selection_.action.reset();
}

void EquationSequenceEditor::rebuildView() {
    const auto* clip = currentClip();
    if (!clip) {
        selection_ = {};
        if (!view_.isEmpty()) {
            view_.clear();
            viewData_.reset();
            viewKey_.clear();
            Q_EMIT viewChanged();
        }
        if (!status_.isEmpty()) {
            status_.clear();
            Q_EMIT statusChanged();
        }
        return;
    }
    resolveSelection();
    const auto& data = clip->equationSequence;
    const auto fpsNum = clip->sourceFpsNum, fpsDen = clip->sourceFpsDen;
    const QString key =
        qs(clip->id) + u'|' + qs(selection_.state.value) + u'|' +
        (selection_.part ? qs(selection_.part->value) : QString()) + u'|' +
        (selection_.action ? qs(selection_.action->value) : QString()) + u'|' + message_ + u'|' +
        (messageError_ ? u'e' : u'n') + u'|' + QString::number(fpsNum) + u'/' +
        QString::number(fpsDen) + u'|' + QString::number(clip->sourceInFrame) + u'|' +
        QString::number(clip->sourceOutFrame) + u'|' +
        QString::number(controller_.currentProject().timelineFpsNum) + u'/' +
        QString::number(controller_.currentProject().timelineFpsDen) + u'|' +
        QString::number(controller_.outputHeight());
    if (viewData_ && *viewData_ == data && key == viewKey_)
        return;

    QVariantMap view;
    view.insert(QStringLiteral("clipId"), qs(clip->id));
    view.insert(QStringLiteral("clipName"), qs(clip->name));
    view.insert(QStringLiteral("stateCount"), static_cast<int>(data.states.size()));
    view.insert(QStringLiteral("fpsText"),
                QString::number(fpsNum) + u'/' + QString::number(fpsDen) + QStringLiteral(" fps"));
    std::vector<project::EquationInterval> intervals;
    std::int64_t length = 0;
    std::string ignored;
    project::equationIntervals(data, intervals, length, ignored);
    view.insert(QStringLiteral("lengthText"), qs(equationFramesText(length, fpsNum, fpsDen)));
    view.insert(QStringLiteral("message"), message_);
    view.insert(QStringLiteral("messageError"), messageError_);

    // 出力 fps で 1 frame も出ない action (P3-1 の renderability)。
    const auto renderability = project::equationRenderability(
        *clip,
        {controller_.currentProject().timelineFpsNum, controller_.currentProject().timelineFpsDen},
        controller_.outputHeight());

    QVariantList states;
    for (std::size_t i = 0; i < data.states.size(); ++i) {
        const auto& s = data.states[i];
        const auto parts = equationPartViews(data, s.id);
        const bool problem = std::any_of(parts.begin(), parts.end(), [](const auto& v) {
            return v.status != EquationPartStatus::Bound;
        });
        const auto actionCount = std::count_if(data.actions.begin(), data.actions.end(),
                                               [&](const auto& a) { return a.state == s.id; });
        states.push_back(QVariantMap{
            {QStringLiteral("id"), qs(s.id.value)},
            {QStringLiteral("index"), static_cast<int>(i)},
            {QStringLiteral("summary"), qs(s.equation.source).simplified()},
            {QStringLiteral("holdFrames"), static_cast<qint64>(s.holdFrames)},
            {QStringLiteral("holdText"), qs(equationFramesText(s.holdFrames, fpsNum, fpsDen))},
            {QStringLiteral("partCount"), static_cast<int>(s.parts.size())},
            {QStringLiteral("actionCount"), static_cast<int>(actionCount)},
            {QStringLiteral("problem"), problem},
            {QStringLiteral("selected"), s.id == selection_.state}});
    }
    view.insert(QStringLiteral("states"), states);

    const auto& state = data.states[selection_.stateIndex];
    const auto index = selection_.stateIndex;
    const bool middle = index > 0 && index + 1 < data.states.size();
    int adjacentCorrespondence = 0;
    int adjacentTransitions = 0;
    for (const auto& t : data.transitions)
        if (t.from == state.id || t.to == state.id) {
            ++adjacentTransitions;
            adjacentCorrespondence += static_cast<int>(t.correspondence.size());
        }
    const auto ownedActions = std::count_if(data.actions.begin(), data.actions.end(),
                                            [&](const auto& a) { return a.state == state.id; });
    view.insert(
        QStringLiteral("state"),
        QVariantMap{
            {QStringLiteral("id"), qs(state.id.value)},
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("source"), qs(state.equation.source)},
            {QStringLiteral("holdFrames"), static_cast<qint64>(state.holdFrames)},
            {QStringLiteral("holdText"), qs(equationFramesText(state.holdFrames, fpsNum, fpsDen))},
            {QStringLiteral("canDelete"), data.states.size() > 1},
            {QStringLiteral("canMoveUp"), index > 0},
            {QStringLiteral("canMoveDown"), index + 1 < data.states.size()},
            {QStringLiteral("deleteActions"), static_cast<int>(ownedActions)},
            {QStringLiteral("deleteTransitions"), adjacentTransitions},
            {QStringLiteral("deleteCorrespondence"), adjacentCorrespondence},
            {QStringLiteral("deleteJoinsNeighbors"), middle},
            {QStringLiteral("maximumFrames"), static_cast<qint64>(kMaximumEditFrames)}});

    // 部分式 (選んだ状態)。
    const auto parts = equationPartViews(data, state.id);
    QVariantList partRows;
    QVariantMap selectedPart;
    QVariantList actionTargets;
    for (const auto& v : parts) {
        QVariantMap row{
            {QStringLiteral("id"), qs(v.id.value)},
            {QStringLiteral("label"), qs(v.label)},
            {QStringLiteral("displayLabel"),
             v.exists ? partDisplayLabel(v.label, v.expectedText) : QStringLiteral("(削除済みの部分式)")},
            {QStringLiteral("expectedText"), qs(v.expectedText)},
            {QStringLiteral("status"), QString::fromLatin1(equationPartStatusName(v.status))},
            {QStringLiteral("statusText"), qs(equationPartStatusText(v.status))},
            {QStringLiteral("repairable"), equationPartRepairable(v.status)},
            {QStringLiteral("exists"), v.exists},
            {QStringLiteral("hasRange"), v.rangeUtf16.has_value()},
            {QStringLiteral("rangeStart"), v.rangeUtf16 ? static_cast<int>(v.rangeUtf16->first) : -1},
            {QStringLiteral("rangeEnd"), v.rangeUtf16 ? static_cast<int>(v.rangeUtf16->second) : -1},
            {QStringLiteral("rangeText"),
             v.rangeUtf16 ? QString::number(v.rangeUtf16->first + 1) + QStringLiteral("〜") +
                                QString::number(v.rangeUtf16->second) + QStringLiteral(" 文字目")
                          : QStringLiteral("今の式に範囲がありません")},
            {QStringLiteral("actionCount"), static_cast<int>(v.actions.size())},
            {QStringLiteral("correspondenceCount"), static_cast<int>(v.correspondences.size())},
            {QStringLiteral("selected"), selection_.part && *selection_.part == v.id}};
        if (selection_.part && *selection_.part == v.id)
            selectedPart = row;
        if (v.status == EquationPartStatus::Bound)
            actionTargets.push_back(QVariantMap{
                {QStringLiteral("id"), qs(v.id.value)},
                {QStringLiteral("label"), partDisplayLabel(v.label, v.expectedText)}});
        partRows.push_back(row);
    }
    view.insert(QStringLiteral("parts"), partRows);
    view.insert(QStringLiteral("selectedPart"), selectedPart);
    view.insert(QStringLiteral("actionTargets"), actionTargets);

    // 選んだ状態の前後の変形。対応の候補は今の Bound で、既に使った側を除く (確定前に重複を防ぐ)。
    QVariantList transitions;
    for (const auto& t : data.transitions) {
        if (t.from != state.id && t.to != state.id)
            continue;
        const auto fromIndex = static_cast<std::size_t>(
            std::find_if(data.states.begin(), data.states.end(),
                         [&](const auto& s) { return s.id == t.from; }) -
            data.states.begin());
        const auto& from = data.states[fromIndex];
        const auto& to = data.states[fromIndex + 1];
        QVariantList pairs;
        for (const auto& pair : t.correspondence) {
            const auto* a = findPart(from, pair.from);
            const auto* b = findPart(to, pair.to);
            pairs.push_back(QVariantMap{
                {QStringLiteral("fromPart"), qs(pair.from.value)},
                {QStringLiteral("toPart"), qs(pair.to.value)},
                {QStringLiteral("fromLabel"),
                 a ? partDisplayLabel(a->label, a->binding.expectedText) : QStringLiteral("?")},
                {QStringLiteral("toLabel"),
                 b ? partDisplayLabel(b->label, b->binding.expectedText) : QStringLiteral("?")}});
        }
        const auto candidates = [&](const project::EquationState& s, bool source) {
            QVariantList list;
            for (const auto& v : equationPartViews(data, s.id)) {
                if (v.status != EquationPartStatus::Bound)
                    continue;
                const bool used =
                    std::any_of(t.correspondence.begin(), t.correspondence.end(),
                                [&](const auto& p) { return (source ? p.from : p.to) == v.id; });
                if (!used)
                    list.push_back(QVariantMap{
                        {QStringLiteral("id"), qs(v.id.value)},
                        {QStringLiteral("label"), partDisplayLabel(v.label, v.expectedText)}});
            }
            return list;
        };
        transitions.push_back(QVariantMap{
            {QStringLiteral("id"), qs(t.id.value)},
            {QStringLiteral("role"),
             t.to == state.id ? QStringLiteral("incoming") : QStringLiteral("outgoing")},
            {QStringLiteral("fromIndex"), static_cast<int>(fromIndex)},
            {QStringLiteral("toIndex"), static_cast<int>(fromIndex + 1)},
            {QStringLiteral("title"), QStringLiteral("[%1] → [%2] の変形").arg(fromIndex).arg(fromIndex + 1)},
            {QStringLiteral("frames"), static_cast<qint64>(t.frames)},
            {QStringLiteral("framesText"), qs(equationFramesText(t.frames, fpsNum, fpsDen))},
            {QStringLiteral("correspondence"), pairs},
            {QStringLiteral("fromCandidates"), candidates(from, true)},
            {QStringLiteral("toCandidates"), candidates(to, false)}});
    }
    view.insert(QStringLiteral("transitions"), transitions);

    // action (選んだ状態が所有するもの、開始の順)。
    std::vector<const project::EquationAction*> owned;
    for (const auto& a : data.actions)
        if (a.state == state.id)
            owned.push_back(&a);
    std::stable_sort(owned.begin(), owned.end(),
                     [](const auto* a, const auto* b) { return a->start < b->start; });
    QVariantList actions;
    QVariantMap selectedAction;
    for (const auto* a : owned) {
        const auto* target = findPart(state, a->target);
        const bool unsampled = std::find(renderability.unsampledActions.begin(),
                                         renderability.unsampledActions.end(),
                                         a->id) != renderability.unsampledActions.end();
        QVariantMap row{
            {QStringLiteral("id"), qs(a->id.value)},
            {QStringLiteral("partId"), qs(a->target.value)},
            {QStringLiteral("partLabel"),
             target ? partDisplayLabel(target->label, target->binding.expectedText)
                    : QStringLiteral("(見つからない部分式)")},
            {QStringLiteral("targetStatus"),
             a->targetStatus == project::EquationTargetStatus::Missing ? QStringLiteral("missing")
                                                                       : QStringLiteral("present")},
            {QStringLiteral("operation"), QString::fromLatin1(operationName(a->operation))},
            {QStringLiteral("operationText"), operationText(a->operation)},
            {QStringLiteral("start"), static_cast<qint64>(a->start)},
            {QStringLiteral("duration"), static_cast<qint64>(a->duration)},
            {QStringLiteral("intervalText"),
             QStringLiteral("開始 ") + qs(equationFramesText(a->start, fpsNum, fpsDen)) +
                 QStringLiteral(" / 長さ ") + qs(equationFramesText(a->duration, fpsNum, fpsDen))},
            {QStringLiteral("unsampled"), unsampled},
            {QStringLiteral("selected"), selection_.action && *selection_.action == a->id}};
        if (selection_.action && *selection_.action == a->id)
            selectedAction = row;
        actions.push_back(row);
    }
    view.insert(QStringLiteral("actions"), actions);
    view.insert(QStringLiteral("selectedAction"), selectedAction);

    view_ = std::move(view);
    viewData_ = data;
    viewKey_ = key;
    Q_EMIT viewChanged();
}

void EquationSequenceEditor::selectState(const QString& stateId) {
    const auto* clip = currentClip();
    if (!clip)
        return;
    const project::StateId id{stateId.toStdString()};
    const auto& states = clip->equationSequence.states;
    const auto found =
        std::find_if(states.begin(), states.end(), [&](const auto& s) { return s.id == id; });
    if (found == states.end())
        return;
    selection_.wantedState = id;
    selection_.wantedPart.reset();
    selection_.wantedAction.reset();
    if (id == selection_.state)
        return;
    selection_.state = id;
    selection_.stateIndex = static_cast<std::size_t>(found - states.begin());
    selection_.part.reset();
    selection_.action.reset();
    message_.clear();
    rebuildView();
}

void EquationSequenceEditor::selectPart(const QString& partId) {
    const auto* clip = currentClip();
    if (!clip)
        return;
    if (partId.isEmpty()) {
        selection_.part.reset();
        selection_.wantedPart.reset();
        rebuildView();
        return;
    }
    const auto parts = equationPartViews(clip->equationSequence, selection_.state);
    const auto found = std::find_if(parts.begin(), parts.end(), [&](const auto& v) {
        return v.id.value == partId.toStdString();
    });
    if (found == parts.end())
        return;
    selection_.part = found->id;
    selection_.wantedPart = found->id;
    selection_.partIndex = static_cast<std::size_t>(found - parts.begin());
    rebuildView();
}

void EquationSequenceEditor::selectAction(const QString& actionId) {
    const auto* clip = currentClip();
    if (!clip)
        return;
    if (actionId.isEmpty()) {
        selection_.action.reset();
        selection_.wantedAction.reset();
        rebuildView();
        return;
    }
    for (const auto& a : clip->equationSequence.actions)
        if (a.id.value == actionId.toStdString() && a.state == selection_.state) {
            selection_.action = a.id;
            selection_.wantedAction = a.id;
            rebuildView();
            return;
        }
}

bool EquationSequenceEditor::commit(
    const std::function<bool(project::EquationSequenceClipData&, std::string&)>& edit,
    const QString& done) {
    const auto* clip = currentClip();
    if (!clip) {
        setMessage(QStringLiteral("数式 sequence が選択されていません"), true);
        rebuildView();
        return false;
    }
    std::string domainError;
    const auto clipId = clip->id;
    const bool ok = controller_.editEquationSequenceData(
        clipId, [&](project::EquationSequenceClipData& data, std::string& error) {
            if (edit(data, error))
                return true;
            domainError = error;
            return false;
        });
    if (!ok) {
        setMessage(domainError.empty() ? controller_.statusText() : qs(domainError), true);
        rebuildView();
        return false;
    }
    setMessage(done, false);
    rebuildView();
    return true;
}

bool EquationSequenceEditor::insertState(bool after) {
    const auto* clip = currentClip();
    const auto* reference = selectedState();
    if (!clip || !reference)
        return false;
    const auto& data = clip->equationSequence;
    const auto position = selection_.stateIndex + (after ? 1 : 0);
    const auto hold = equationDefaultHoldFrames(clip->sourceFpsNum, clip->sourceFpsDen);
    const auto frames = equationDefaultTransitionFrames(clip->sourceFpsNum, clip->sourceFpsDen);
    // 新しい状態は選んだ状態の式と書式を写す (部分式・action・対応は写さない)。
    auto state = newEquationState(reference->equation, reference->equation.source, hold,
                                  [this] { return newId(); });
    std::vector<project::EquationStepTransition> edges;
    if (position > 0)
        edges.push_back({{newId()}, data.states[position - 1].id, state.id, frames, {}});
    if (position < data.states.size())
        edges.push_back({{newId()}, state.id, data.states[position].id, frames, {}});
    const auto id = state.id;
    const int height = controller_.outputHeight();
    if (!commit(
            [&](auto& d, std::string& error) {
                return project::insertEquationState(d, position, state, edges, height, error);
            },
            QStringLiteral("状態を追加しました (前後は新しい変形で結び、対応は作っていません)")))
        return false;
    selectState(qs(id.value));
    return true;
}

bool EquationSequenceEditor::deleteSelectedState() {
    const auto* clip = currentClip();
    const auto* state = selectedState();
    if (!clip || !state)
        return false;
    const auto& data = clip->equationSequence;
    if (data.states.size() == 1) {
        setMessage(QStringLiteral("最後の状態は削除できません (sequence には状態が 1 つ以上必要です)"),
                   true);
        rebuildView();
        return false;
    }
    const auto i = selection_.stateIndex;
    std::optional<project::EquationStepTransition> edge;
    if (i > 0 && i + 1 < data.states.size())
        edge = project::EquationStepTransition{
            {newId()},
            data.states[i - 1].id,
            data.states[i + 1].id,
            equationDefaultTransitionFrames(clip->sourceFpsNum, clip->sourceFpsDen),
            {}};
    const auto id = state->id;
    const int height = controller_.outputHeight();
    return commit(
        [&](auto& d, std::string& error) {
            return project::deleteEquationState(d, id, edge, height, error);
        },
        QStringLiteral("状態と、その action・隣接する変形を削除しました"));
}

bool EquationSequenceEditor::moveSelectedState(int delta) {
    const auto* clip = currentClip();
    const auto* state = selectedState();
    if (!clip || !state || delta == 0)
        return false;
    const auto target = static_cast<long long>(selection_.stateIndex) + delta;
    if (target < 0 || target >= static_cast<long long>(clip->equationSequence.states.size())) {
        setMessage(QStringLiteral("これ以上移動できません"), true);
        rebuildView();
        return false;
    }
    const auto id = state->id;
    const auto frames = equationDefaultTransitionFrames(clip->sourceFpsNum, clip->sourceFpsDen);
    const int height = controller_.outputHeight();
    if (!commit(
            [&](auto& d, std::string& error) {
                return project::moveEquationState(d, id, static_cast<std::size_t>(target),
                                                  [this] { return newId(); }, frames, height,
                                                  error);
            },
            QStringLiteral("状態を移動しました (隣接が変わった変形は新しく作り、対応は引き継いでいません)")))
        return false;
    selection_.stateIndex = static_cast<std::size_t>(target);
    rebuildView();
    return true;
}

bool EquationSequenceEditor::setHoldFrames(qint64 frames) {
    const auto* state = selectedState();
    if (!state)
        return false;
    if (frames < 1 || frames > kMaximumEditFrames) {
        setMessage(QStringLiteral("hold は 1 から %1 frame までです").arg(kMaximumEditFrames), true);
        rebuildView();
        return false;
    }
    if (frames == state->holdFrames)
        return true;
    const auto id = state->id;
    const int height = controller_.outputHeight();
    return commit(
        [&](auto& d, std::string& error) {
            return project::changeEquationHold(d, id, frames, height, error);
        },
        QStringLiteral("hold を変更しました"));
}

bool EquationSequenceEditor::setTransitionFrames(const QString& transitionId, qint64 frames) {
    if (!currentClip())
        return false;
    if (frames < 1 || frames > kMaximumEditFrames) {
        setMessage(QStringLiteral("変形の長さは 1 から %1 frame までです").arg(kMaximumEditFrames),
                   true);
        rebuildView();
        return false;
    }
    const project::TransitionId id{transitionId.toStdString()};
    const int height = controller_.outputHeight();
    return commit(
        [&](auto& d, std::string& error) {
            for (const auto& t : d.transitions)
                if (t.id == id && t.frames == frames)
                    return true;
            return project::changeEquationTransition(d, id, frames, height, error);
        },
        QStringLiteral("変形の長さを変更しました"));
}

// ---- 式の編集 ----

void EquationSequenceEditor::endSession() {
    if (!session_)
        return;
    disconnect(session_->connection);
    session_.reset();
}

void EquationSequenceEditor::beginSourceEdit(QObject* textDocument) {
    const auto* clip = currentClip();
    const auto* state = selectedState();
    if (!clip || !state)
        return;
    auto* document = documentOf(textDocument);
    const auto clipId = qs(clip->id);
    // 確定を断られた入力を保持したまま focus を戻した: 同じ document・同じ対象なら記録を続ける。
    if (session_ && session_->document && session_->document == document &&
        session_->clipId == clipId && session_->state == state->id &&
        session_->shadow == document->toPlainText())
        return;
    endSession();
    SourceSession session;
    session.clipId = clipId;
    session.state = state->id;
    session.document = document;
    session.baseSource = state->equation.source;
    session.external = document == nullptr;
    session.shadow = document ? document->toPlainText() : QString();
    // 編集欄の文字が今の式と違う状態で始めた記録は、今の式に対する位置として信頼しない。
    session.trusted = document && session.shadow == qs(state->equation.source);
    if (document) {
        // contentsChange は document の layout があるときだけ通知される (QTextDocument の実装)。
        // 編集欄 (TextEdit) は必ず持つが、無い document でも記録を落とさないよう作らせる。
        document->documentLayout();
        session.connection = connect(document, &QTextDocument::contentsChange, this,
                                     &EquationSequenceEditor::onContentsChange);
    }
    session_ = std::move(session);
}

void EquationSequenceEditor::onContentsChange(int position, int removed, int added) {
    if (!session_ || !session_->trusted || !session_->document)
        return;
    const QString text = session_->document->toPlainText();
    const QString& old = session_->shadow;
    if (text == old)
        return; // 書式だけの通知 (文字は変わっていない)
    qsizetype pos = position, rem = removed, add = added;
    // QTextDocument の位置は末尾の段落区切り (文字ではない) まで数え、変更がそこに掛かると
    // 片側だけ 1 多く報告することがある (全体の置換で removed = 長さ + 1, added = 0 を実測)。
    // 文字の無い末尾の外だけを切り詰め、下の再構成の検査で記録の正しさを確かめる。
    // それ以外の食い違いは推測で直さず、記録を信頼しない (確定は全体置換になる)。
    if (pos >= 0 && pos <= old.size() && pos <= text.size()) {
        rem = std::min(rem, old.size() - pos);
        add = std::min(add, text.size() - pos);
    }
    const bool consistent =
        pos >= 0 && rem >= 0 && add >= 0 && pos + rem <= old.size() && pos + add <= text.size() &&
        text.size() - add == old.size() - rem &&
        QStringView(text).left(pos) == QStringView(old).left(pos) &&
        QStringView(text).mid(pos + add) == QStringView(old).mid(pos + rem);
    if (!consistent) {
        session_->trusted = false;
        return;
    }
    project::TrustedEquationEdit edit;
    edit.beginUtf16 = static_cast<std::size_t>(pos);
    edit.endUtf16 = static_cast<std::size_t>(pos + rem);
    edit.replacement = text.mid(pos, add).toStdString();
    // 旧式・新式は確定時に base から順に組み直す (編集ごとに全文を持たない)。
    session_->edits.push_back(std::move(edit));
    session_->shadow = text;
}

QVariantMap EquationSequenceEditor::commitSourceEdit(const QString& text) {
    QVariantMap result{{QStringLiteral("ok"), false},
                       {QStringLiteral("changed"), false},
                       {QStringLiteral("fullReplacement"), false},
                       {QStringLiteral("discarded"), false}};
    // 対象は session を始めた clip・状態 (選択が変わった後の確定でも、元の対象へ書く)。
    QString clipId;
    project::StateId stateId;
    if (session_) {
        clipId = session_->clipId;
        stateId = session_->state;
    } else if (const auto* clip = currentClip(); clip && selectedState()) {
        clipId = qs(clip->id);
        stateId = selection_.state;
    }
    const auto& project = controller_.currentProject();
    const auto clip = std::find_if(project.timelineClips.begin(), project.timelineClips.end(),
                                   [&](const auto& c) {
                                       return qs(c.id) == clipId &&
                                              c.kind == project::TimelineClipKind::EquationSequence;
                                   });
    const project::EquationState* state = nullptr;
    if (clip != project.timelineClips.end())
        for (const auto& s : clip->equationSequence.states)
            if (s.id == stateId)
                state = &s;
    if (!state) {
        endSession();
        setMessage(QStringLiteral("編集中の状態が削除されたため、入力を破棄しました"), true);
        result.insert(QStringLiteral("discarded"), true);
        result.insert(QStringLiteral("message"), message_);
        rebuildView();
        return result;
    }
    const auto newSource = text.toStdString();
    if (newSource == state->equation.source) {
        // 打って消した入力など、文字が変わっていない確定は Project を変えない。
        endSession();
        result.insert(QStringLiteral("ok"), true);
        return result;
    }
    const bool trusted = session_ && session_->trusted && !session_->edits.empty() &&
                         session_->shadow == text &&
                         session_->baseSource == state->equation.source;
    std::vector<project::TrustedEquationEdit> edits;
    if (trusted) {
        QString current = qs(session_->baseSource);
        for (const auto& recorded : session_->edits) {
            auto edit = recorded;
            const auto replacement = qs(recorded.replacement);
            const auto begin = static_cast<qsizetype>(recorded.beginUtf16);
            const auto end = static_cast<qsizetype>(recorded.endUtf16);
            edit.oldSource = current.toStdString();
            current = current.left(begin) + replacement + current.mid(end);
            edit.newSource = current.toStdString();
            edits.push_back(std::move(edit));
        }
    }
    const auto before = equationPartViews(clip->equationSequence, stateId);
    const int height = controller_.outputHeight();
    std::string domainError;
    bool usedTrusted = trusted;
    const auto apply = [&](project::EquationSequenceClipData& d, std::string& error) {
        if (usedTrusted &&
            applyTrustedEquationEdits(d, stateId, edits, [this] { return newId(); }, height, error))
            return true;
        // 記録が信頼できない・適用できない: 明示の全体置換 (全 binding を無効、文字列で探さない)。
        usedTrusted = false;
        if (project::replaceEquationSource(d, stateId, newSource, newId(), height, error))
            return true;
        domainError = error;
        return false;
    };
    const auto savedClipId = clip->id;
    if (!controller_.editEquationSequenceData(savedClipId, apply)) {
        setMessage(QStringLiteral("式を確定できません: ") +
                       (domainError.empty() ? controller_.statusText() : qs(domainError)),
                   true);
        result.insert(QStringLiteral("message"), message_);
        rebuildView();
        return result;
    }
    endSession();
    QStringList invalidated;
    const auto& after = controller_.currentProject();
    for (const auto& c : after.timelineClips) {
        if (c.id != savedClipId)
            continue;
        for (const auto& v : equationPartViews(c.equationSequence, stateId)) {
            const auto old = std::find_if(before.begin(), before.end(),
                                          [&](const auto& b) { return b.id == v.id; });
            if (v.status == EquationPartStatus::InvalidBinding && old != before.end() &&
                old->status != EquationPartStatus::InvalidBinding)
                invalidated.push_back(partDisplayLabel(v.label, v.expectedText));
        }
    }
    QString done = usedTrusted ? QStringLiteral("式を確定しました")
                               : QStringLiteral("式を全体置換として確定しました");
    if (!invalidated.isEmpty())
        done += QStringLiteral("。範囲が壊れた部分式: ") + invalidated.join(QStringLiteral("、")) +
                QStringLiteral(" (範囲を選び直して再設定してください。変形の対応は外れました)");
    setMessage(done, false);
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("changed"), true);
    result.insert(QStringLiteral("fullReplacement"), !usedTrusted);
    result.insert(QStringLiteral("invalidated"), invalidated);
    result.insert(QStringLiteral("message"), message_);
    rebuildView();
    return result;
}

void EquationSequenceEditor::cancelSourceEdit() {
    endSession();
}

// ---- 部分式 ----

std::optional<std::pair<std::int64_t, std::int64_t>>
EquationSequenceEditor::selectionBytes(int selectionStart, int selectionEnd,
                                       const QString& editorText, QString& error) const {
    const auto* state = selectedState();
    if (!state) {
        error = QStringLiteral("状態が選択されていません");
        return std::nullopt;
    }
    // 範囲は確定済みの式に対する位置。未確定の入力があれば位置の基準が違う。
    if (editorText != qs(state->equation.source) || (session_ && !session_->edits.empty())) {
        error = QStringLiteral("未確定の入力があります。式を確定してから範囲を選んでください");
        return std::nullopt;
    }
    const auto begin = std::min(selectionStart, selectionEnd);
    const auto end = std::max(selectionStart, selectionEnd);
    if (begin < 0 || begin == end) {
        error = QStringLiteral("式の中で範囲を選択してください");
        return std::nullopt;
    }
    // UTF-16 → UTF-8 は domain の検査付き変換だけで行う (サロゲートの内側・終端外は拒否)。
    const auto& source = state->equation.source;
    const auto b = core::utf16ToUtf8Offset(source, static_cast<std::size_t>(begin));
    const auto e = core::utf16ToUtf8Offset(source, static_cast<std::size_t>(end));
    if (!b || !e) {
        error = QStringLiteral("選択の端が文字の境界にありません");
        return std::nullopt;
    }
    return std::make_pair(static_cast<std::int64_t>(*b), static_cast<std::int64_t>(*e));
}

bool EquationSequenceEditor::addPart(int selectionStart, int selectionEnd,
                                     const QString& editorText, const QString& label) {
    const auto* state = selectedState();
    if (!state)
        return false;
    QString error;
    const auto range = selectionBytes(selectionStart, selectionEnd, editorText, error);
    if (!range) {
        setMessage(error, true);
        rebuildView();
        return false;
    }
    project::SemanticPart part;
    part.id = {newId()};
    part.label = label.trimmed().toStdString();
    part.binding = {state->revision, range->first, range->second,
                    state->equation.source.substr(static_cast<std::size_t>(range->first),
                                                  static_cast<std::size_t>(range->second -
                                                                           range->first)),
                    project::BindingStatus::Bound};
    // compiler が分離できない範囲は作らない (Project に描けない部分式を増やさない)。
    const auto status = equationPartStatus(*state, part);
    if (status != EquationPartStatus::Bound) {
        setMessage(QStringLiteral("この範囲は部分式にできません: ") +
                       qs(equationPartStatusText(status)),
                   true);
        rebuildView();
        return false;
    }
    const auto stateId = state->id;
    const auto partId = part.id;
    const int height = controller_.outputHeight();
    if (!commit(
            [&](auto& d, std::string& e) {
                return project::addEquationPart(d, stateId, part, height, e);
            },
            QStringLiteral("部分式を追加しました")))
        return false;
    selectPart(qs(partId.value));
    return true;
}

bool EquationSequenceEditor::rebindSelectedPart(int selectionStart, int selectionEnd,
                                                const QString& editorText) {
    const auto* clip = currentClip();
    const auto* state = selectedState();
    if (!clip || !state || !selection_.part) {
        setMessage(QStringLiteral("再設定する部分式を選んでください"), true);
        rebuildView();
        return false;
    }
    QString error;
    const auto range = selectionBytes(selectionStart, selectionEnd, editorText, error);
    if (!range) {
        setMessage(error, true);
        rebuildView();
        return false;
    }
    const auto partId = *selection_.part;
    const auto* existing = findPart(*state, partId);
    project::SemanticPart part;
    part.id = partId;
    part.label = existing ? existing->label : std::string{};
    part.binding = {state->revision, range->first, range->second,
                    state->equation.source.substr(static_cast<std::size_t>(range->first),
                                                  static_cast<std::size_t>(range->second -
                                                                           range->first)),
                    project::BindingStatus::Bound};
    const auto status = equationPartStatus(*state, part);
    if (status != EquationPartStatus::Bound) {
        setMessage(QStringLiteral("この範囲は部分式にできません: ") +
                       qs(equationPartStatusText(status)),
                   true);
        rebuildView();
        return false;
    }
    const auto stateId = state->id;
    const int height = controller_.outputHeight();
    if (existing) {
        const bool wasInvalid = existing->binding.status != project::BindingStatus::Bound;
        return commit(
            [&](auto& d, std::string& e) {
                return project::rebindEquationPart(d, stateId, partId, part.binding, height, e);
            },
            wasInvalid ? QStringLiteral("部分式を修復しました (同じ部分式のまま)。変形の対応は自動では"
                                        "戻りません。必要なら対応を設定し直してください")
                       : QStringLiteral("部分式の範囲を再設定しました"));
    }
    const auto referencing = std::count_if(
        clip->equationSequence.actions.begin(), clip->equationSequence.actions.end(),
        [&](const auto& a) { return a.state == stateId && a.target == partId; });
    return commit(
        [&](auto& d, std::string& e) {
            return project::restoreMissingEquationPart(d, stateId, part, height, e);
        },
        QStringLiteral("見つからなかった部分式を同じ部分式として作り直し、action %1 件を有効に戻しました。"
                       "変形の対応は自動では戻りません")
            .arg(referencing));
}

bool EquationSequenceEditor::renameSelectedPart(const QString& label) {
    const auto* state = selectedState();
    if (!state || !selection_.part || !findPart(*state, *selection_.part))
        return false;
    const auto stateId = state->id;
    const auto partId = *selection_.part;
    const auto text = label.trimmed().toStdString();
    if (findPart(*state, partId)->label == text)
        return true;
    const int height = controller_.outputHeight();
    return commit(
        [&](auto& d, std::string& e) {
            return project::renameEquationPart(d, stateId, partId, text, height, e);
        },
        QStringLiteral("部分式の名前を変更しました"));
}

bool EquationSequenceEditor::deleteSelectedPart() {
    const auto* clip = currentClip();
    const auto* state = selectedState();
    if (!clip || !state || !selection_.part)
        return false;
    if (!findPart(*state, *selection_.part)) {
        setMessage(QStringLiteral("削除済みの部分式です。範囲を選んで作り直すか、action を削除してください"),
                   true);
        rebuildView();
        return false;
    }
    const auto stateId = state->id;
    const auto partId = *selection_.part;
    const auto views = equationPartViews(clip->equationSequence, stateId);
    const auto view = std::find_if(views.begin(), views.end(),
                                   [&](const auto& v) { return v.id == partId; });
    const int height = controller_.outputHeight();
    return commit(
        [&](auto& d, std::string& e) {
            return project::deleteEquationPart(d, stateId, partId, height, e);
        },
        QStringLiteral("部分式を削除しました。action %1 件は「見つからない部分式」を参照しています。"
                       "変形の対応 %2 件を外しました")
            .arg(view != views.end() ? view->actions.size() : 0)
            .arg(view != views.end() ? view->correspondences.size() : 0));
}

// ---- 変形の明示対応 ----

bool EquationSequenceEditor::addCorrespondence(const QString& transitionId,
                                               const QString& fromPart, const QString& toPart) {
    if (!currentClip())
        return false;
    if (fromPart.isEmpty() || toPart.isEmpty()) {
        setMessage(QStringLiteral("前と後の部分式を両方選んでください"), true);
        rebuildView();
        return false;
    }
    const project::TransitionId id{transitionId.toStdString()};
    const project::PartPair pair{{fromPart.toStdString()}, {toPart.toStdString()}};
    const int height = controller_.outputHeight();
    return commit(
        [&](auto& d, std::string& e) {
            return project::addEquationCorrespondence(d, id, pair, height, e);
        },
        QStringLiteral("変形の対応を追加しました"));
}

bool EquationSequenceEditor::removeCorrespondence(const QString& transitionId,
                                                  const QString& fromPart, const QString& toPart) {
    if (!currentClip())
        return false;
    const project::TransitionId id{transitionId.toStdString()};
    const project::PartPair pair{{fromPart.toStdString()}, {toPart.toStdString()}};
    const int height = controller_.outputHeight();
    return commit(
        [&](auto& d, std::string& e) {
            return project::removeEquationCorrespondence(d, id, pair, height, e);
        },
        QStringLiteral("変形の対応を外しました (自動の照合に戻ります)"));
}

// ---- action ----

bool EquationSequenceEditor::addAction(const QString& partId, const QString& operation,
                                       qint64 start, qint64 duration) {
    const auto* state = selectedState();
    if (!state)
        return false;
    const auto op = parseOperation(operation);
    if (!op || partId.isEmpty() || !findPart(*state, {partId.toStdString()})) {
        setMessage(QStringLiteral("強調の対象 (この状態の部分式) と種類 (outline / pulse) を選んでください"),
                   true);
        rebuildView();
        return false;
    }
    project::EquationAction action{{newId()},       state->id, {partId.toStdString()},
                                   project::EquationTargetStatus::Present,
                                   start,           duration,  *op};
    const auto actionId = action.id;
    const int height = controller_.outputHeight();
    if (!commit(
            [&](auto& d, std::string& e) {
                return project::addEquationAction(d, action, height, e);
            },
            QStringLiteral("強調を追加しました")))
        return false;
    selectAction(qs(actionId.value));
    return true;
}

bool EquationSequenceEditor::updateSelectedAction(const QString& partId,
                                                  const QString& operation, qint64 start,
                                                  qint64 duration) {
    const auto* clip = currentClip();
    const auto* state = selectedState();
    if (!clip || !state || !selection_.action)
        return false;
    const auto& actions = clip->equationSequence.actions;
    const auto found = std::find_if(actions.begin(), actions.end(),
                                    [&](const auto& a) { return a.id == *selection_.action; });
    const auto op = parseOperation(operation);
    if (found == actions.end() || !op) {
        setMessage(QStringLiteral("強調の種類は outline / pulse だけです"), true);
        rebuildView();
        return false;
    }
    auto updated = *found;
    updated.target = {partId.toStdString()};
    if (findPart(*state, updated.target))
        updated.targetStatus = project::EquationTargetStatus::Present;
    else if (updated.target == found->target &&
             found->targetStatus == project::EquationTargetStatus::Missing)
        updated.targetStatus = project::EquationTargetStatus::Missing;
    else {
        setMessage(QStringLiteral("強調の対象はこの状態の部分式から選んでください"), true);
        rebuildView();
        return false;
    }
    updated.operation = *op;
    updated.start = start;
    updated.duration = duration;
    if (updated == *found)
        return true;
    const int height = controller_.outputHeight();
    return commit(
        [&](auto& d, std::string& e) {
            return project::updateEquationAction(d, updated, height, e);
        },
        QStringLiteral("強調を変更しました"));
}

bool EquationSequenceEditor::deleteSelectedAction() {
    if (!currentClip() || !selection_.action)
        return false;
    const auto id = *selection_.action;
    const int height = controller_.outputHeight();
    return commit(
        [&](auto& d, std::string& e) { return project::deleteEquationAction(d, id, height, e); },
        QStringLiteral("強調を削除しました"));
}

bool EquationSequenceEditor::seekToSelectedAction() {
    const auto* clip = currentClip();
    if (!clip || !selection_.action)
        return false;
    const auto& data = clip->equationSequence;
    std::vector<project::EquationInterval> intervals;
    std::int64_t length = 0;
    std::string error;
    if (!project::equationIntervals(data, intervals, length, error))
        return false;
    for (const auto& a : data.actions) {
        if (a.id != *selection_.action)
            continue;
        const auto index = static_cast<std::size_t>(
            std::find_if(data.states.begin(), data.states.end(),
                         [&](const auto& s) { return s.id == a.state; }) -
            data.states.begin());
        const auto interval = std::find_if(intervals.begin(), intervals.end(), [&](const auto& i) {
            return !i.transition && i.index == index;
        });
        const core::FrameRate source{clip->sourceFpsNum, clip->sourceFpsDen};
        const core::FrameRate output{controller_.currentProject().timelineFpsNum,
                                     controller_.currentProject().timelineFpsDen};
        // P3-1 の写像: 素材 frame s の output 位置は ceil(s R)。可視範囲の先頭からの差を足す。
        const auto begin = interval->begin + a.start;
        const auto first = core::convertFrameBoundary(std::max(begin, clip->sourceInFrame), source,
                                                      output, true);
        const auto origin = core::convertFrameBoundary(clip->sourceInFrame, source, output, true);
        const auto visibleEnd =
            core::convertFrameBoundary(clip->sourceOutFrame, source, output, true);
        if (!first || !origin || !visibleEnd || *first >= *visibleEnd ||
            begin + a.duration <= clip->sourceInFrame) {
            setMessage(QStringLiteral("この強調は clip の表示範囲の外です"), true);
            rebuildView();
            return false;
        }
        return controller_.seekTimelineFrame(clip->timelineStartFrame + (*first - *origin));
    }
    return false;
}

// ---- 状態 (読むだけ) ----

void EquationSequenceEditor::refreshStatus() {
    ++statusRefreshes_;
    const auto* clip = currentClip();
    if (!clip) {
        if (!status_.isEmpty()) {
            status_.clear();
            Q_EMIT statusChanged();
        }
        return;
    }
    const auto clipId = qs(clip->id);
    const auto s = controller_.equationSequencePreviewStatus(clipId, controller_.playheadFrame());
    QVariantMap status;
    status.insert(QStringLiteral("clipId"), clipId);
    // Compile
    status.insert(QStringLiteral("compile"), s.compile == EquationCompileFailure::None
                                                 ? QStringLiteral("ready")
                                                 : QStringLiteral("failed"));
    status.insert(QStringLiteral("compileText"), qs(equationCompileFailureText(s.compile)));
    status.insert(QStringLiteral("compileRepairable"), equationCompileFailureRepairable(s.compile));
    // Renderer (backend)
    QString renderer, rendererText;
    switch (s.backend) {
    case MathRasterCache::BackendState::Checking:
        renderer = QStringLiteral("checking");
        rendererText = QStringLiteral("確認中");
        break;
    case MathRasterCache::BackendState::Available:
        renderer = QStringLiteral("available");
        rendererText = QStringLiteral("利用可能");
        break;
    case MathRasterCache::BackendState::Unavailable:
        renderer = QStringLiteral("unavailable");
        rendererText = QStringLiteral("利用不可 (数式の描画環境を利用できません)");
        break;
    }
    status.insert(QStringLiteral("renderer"), renderer);
    status.insert(QStringLiteral("rendererText"), rendererText);
    // Artifact (disk)
    QString artifact, artifactText;
    if (s.compile != EquationCompileFailure::None) {
        artifact = QStringLiteral("blocked");
        artifactText = QStringLiteral("作成しません (内容の修復が必要)");
    } else if (s.backend != MathRasterCache::BackendState::Available) {
        artifact = QStringLiteral("blocked");
        artifactText = QStringLiteral("作成できません (描画環境が利用不可)");
    } else {
        switch (s.disk) {
        case MathRasterCache::State::Pending:
            artifact = QStringLiteral("pending");
            artifactText = s.time ? QStringLiteral("描画中")
                                  : QStringLiteral("未要求 (再生位置がこの clip の外です)");
            break;
        case MathRasterCache::State::Ready:
            artifact = QStringLiteral("ready");
            artifactText = QStringLiteral("完了");
            break;
        case MathRasterCache::State::Failed:
            artifact = QStringLiteral("failed");
            artifactText = qs(equationBackendFailureText(s.diskFailure));
            break;
        case MathRasterCache::State::Unavailable:
            artifact = QStringLiteral("unavailable");
            artifactText = QStringLiteral("利用不可");
            break;
        }
    }
    status.insert(QStringLiteral("artifact"), artifact);
    status.insert(QStringLiteral("artifactText"), artifactText);
    status.insert(QStringLiteral("artifactFailure"),
                  QString::fromLatin1(math::equationBackendFailureName(s.diskFailure)));
    // Residency (今の frame の層)
    QString residency, residencyText;
    const bool needsLayers = s.time && s.time->lookup.kind != EquationFrameKind::Hold;
    if (!s.time) {
        residency = QStringLiteral("none");
        residencyText = QStringLiteral("—");
    } else if (!needsLayers) {
        residency = QStringLiteral("static");
        residencyText = QStringLiteral("静止だけ (層は不要)");
    } else {
        switch (s.residency) {
        case MathRasterCache::Residency::NotReady:
            residency = QStringLiteral("not_ready");
            residencyText = QStringLiteral("未読込");
            break;
        case MathRasterCache::Residency::Loading:
            residency = QStringLiteral("loading");
            residencyText = QStringLiteral("読込中");
            break;
        case MathRasterCache::Residency::Resident:
            residency = QStringLiteral("resident");
            residencyText = QStringLiteral("読込済み");
            break;
        case MathRasterCache::Residency::OverBudget:
            residency = QStringLiteral("over_budget");
            residencyText = QStringLiteral("preview の memory 上限 (静止で代用)");
            break;
        case MathRasterCache::Residency::Failed:
            residency = QStringLiteral("failed");
            residencyText = QStringLiteral("読込失敗");
            break;
        }
    }
    status.insert(QStringLiteral("residency"), residency);
    status.insert(QStringLiteral("residencyText"), residencyText);
    // Preview (今の frame で見せているもの)
    QString preview, previewText;
    if (!s.time) {
        preview = QStringLiteral("outside");
        previewText = QStringLiteral("再生位置はこの clip の外です");
    } else {
        const auto& lookup = s.time->lookup;
        switch (s.shown.kind) {
        case EquationPreviewShownKind::None:
            preview = QStringLiteral("none");
            previewText = QStringLiteral("表示なし (状態の静止を準備中)");
            break;
        case EquationPreviewShownKind::Static:
            if (s.compile != EquationCompileFailure::None) {
                preview = QStringLiteral("current_state_only");
                previewText = QStringLiteral("状態 [%1] の静止だけ (修復するまで変形・強調を出しません)")
                                  .arg(s.shown.index);
            } else if (lookup.kind == EquationFrameKind::Hold) {
                preview = QStringLiteral("static");
                previewText = QStringLiteral("状態 [%1] の静止").arg(s.shown.index);
            } else {
                preview = QStringLiteral("static_fallback");
                previewText = QStringLiteral("状態 [%1] の静止で代用 (変形・強調の描画待ち)")
                                  .arg(s.shown.index);
            }
            break;
        case EquationPreviewShownKind::Transition:
            preview = QStringLiteral("transition");
            previewText = QStringLiteral("変形 [%1] の %2 / %3 frame")
                              .arg(s.shown.index)
                              .arg(s.shown.frame + 1)
                              .arg(lookup.frames);
            break;
        case EquationPreviewShownKind::Action:
            preview = QStringLiteral("action");
            previewText = QStringLiteral("強調の %1 / %2 frame").arg(s.shown.frame + 1).arg(lookup.frames);
            break;
        }
    }
    status.insert(QStringLiteral("preview"), preview);
    status.insert(QStringLiteral("previewText"), previewText);
    // 利用者が次に何をすべきかの分類 (内容の修復 / 構造 / 環境 / 描画結果 / memory)。
    QString category, headline;
    if (s.compile == EquationCompileFailure::InvalidSequence) {
        category = QStringLiteral("structure");
        headline = qs(equationCompileFailureText(s.compile));
    } else if (s.compile != EquationCompileFailure::None) {
        category = QStringLiteral("content");
        headline = QStringLiteral("内容の修復が必要です: ") + qs(equationCompileFailureText(s.compile));
    } else if (s.backend == MathRasterCache::BackendState::Unavailable) {
        category = QStringLiteral("environment");
        headline = QStringLiteral("数式の描画環境 (Manim) を利用できません。Project は保存できます");
    } else if (s.disk == MathRasterCache::State::Failed) {
        category = equationBackendFailureIsContent(s.diskFailure) ? QStringLiteral("content")
                                                                   : QStringLiteral("artifact");
        headline = qs(equationBackendFailureText(s.diskFailure));
    } else if (needsLayers && s.residency == MathRasterCache::Residency::OverBudget) {
        category = QStringLiteral("memory");
        headline = QStringLiteral("preview の memory 上限のため静止で表示しています (Project と描画結果は正常です)");
    }
    status.insert(QStringLiteral("category"), category);
    status.insert(QStringLiteral("headline"), headline);
    // backend の生の文は主の表示にせず、詳細としてだけ出す。
    QStringList detail;
    if (!s.diskMessage.isEmpty())
        detail.push_back(s.diskMessage);
    if (!s.residencyMessage.isEmpty())
        detail.push_back(s.residencyMessage);
    status.insert(QStringLiteral("detail"), detail.join(u'\n'));
    status.insert(QStringLiteral("exportText"),
                  QStringLiteral("書き出しには現在の入力の描画結果が必要です"));
    if (status != status_) {
        status_ = std::move(status);
        Q_EMIT statusChanged();
    }
}
} // namespace mvm::app
