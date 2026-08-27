#include "media/gpu_preview/output_scheduler.h"
#include "p2_render_opportunity_replay.h"

#include <vector>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace {

struct RawRecord {
    long long callbackQpc = 0;
    long long schedulerNowQpc = 0;
    bool decisionDue = false;
    long long skipped = 0;
    long long outputFrame = -1;
    bool repeated = false;
};

long long integer(const QJsonObject& object, const char* name) {
    return object.value(QLatin1String(name)).toVariant().toLongLong();
}

bool writeJson(const QString& path, const QJsonObject& root) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "出力JSONを開けません: %s\n", qPrintable(path));
        return false;
    }
    return file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) >= 0;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    QString inputPath;
    QString outputPath;
    for (int index = 1; index < args.size(); ++index) {
        if (args[index] == "--input" && index + 1 < args.size())
            inputPath = args[++index];
        else if (args[index] == "--output" && index + 1 < args.size())
            outputPath = args[++index];
        else {
            std::fprintf(stderr,
                         "使い方: mvm_p2_scheduler_replay --input <Q3 raw> --output <JSON>\n");
            return 2;
        }
    }
    if (inputPath.isEmpty() || outputPath.isEmpty()) {
        std::fprintf(stderr, "--inputと--outputが必要です\n");
        return 2;
    }

    QFile input(inputPath);
    if (!input.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "Q3 rawを開けません: %s\n", qPrintable(inputPath));
        return 3;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(input.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        std::fprintf(stderr, "Q3 raw JSONが不正です: %s\n", qPrintable(parseError.errorString()));
        return 3;
    }
    const QJsonObject raw = document.object();
    const QJsonObject attribution = raw.value("scheduler_phase_attribution").toObject();
    const QJsonArray recordJson = attribution.value("records").toArray();
    if (!raw.value("diagnostic_scheduler_phase_ring").toBool() || recordJson.isEmpty()) {
        std::fprintf(stderr, "Q3 scheduler phase ringがありません\n");
        return 3;
    }

    std::vector<RawRecord> records;
    std::vector<long long> callbackQpcs;
    records.reserve(static_cast<std::size_t>(recordJson.size()));
    callbackQpcs.reserve(static_cast<std::size_t>(recordJson.size()));
    for (const auto value : recordJson) {
        const QJsonObject item = value.toObject();
        RawRecord record{integer(item, "callback_qpc"),
                         integer(item, "scheduler_now_qpc"),
                         item.value("decision_due").toBool(),
                         integer(item, "decision_skipped_deadline_count"),
                         integer(item, "decision_output_frame"),
                         item.value("repeated_this_callback").toBool()};
        records.push_back(record);
        callbackQpcs.push_back(record.callbackQpc);
    }

    const long long frequency = integer(attribution, "qpc_frequency");
    const long long requiredFrames = integer(raw, "required_measurement_frame_count");
    const QJsonObject firstRecord = recordJson.first().toObject();
    const long long startQpc = integer(firstRecord, "next_deadline_qpc");
    const long long endQpc = startQpc + (requiredFrames * frequency) / 60;
    bool exactDecisionReplay = integer(firstRecord, "scheduler_next_frame_before") == 0;
    bool noNonSchedulerRepeat = true;
    bool strictlyIncreasing = true;
    long long displayed = 0;
    long long repeated = 0;
    long long dropped = 0;
    long long firstOutput = -1;
    long long previousOutput = -1;
    mvm::gpu::OutputScheduler60Hz current;
    current.start(startQpc, frequency);
    for (const auto& record : records) {
        const auto decision = current.takeDueBefore(record.schedulerNowQpc, endQpc);
        exactDecisionReplay =
            exactDecisionReplay && decision.due == record.decisionDue &&
            decision.skippedDeadlineCount == record.skipped &&
            (!decision.due || decision.output.outputFrameNumber == record.outputFrame);
        noNonSchedulerRepeat = noNonSchedulerRepeat && !(decision.due && record.repeated);
        if (!decision.due) {
            ++repeated;
            exactDecisionReplay = exactDecisionReplay && record.repeated;
            continue;
        }
        dropped += decision.skippedDeadlineCount;
        ++displayed;
        if (firstOutput < 0)
            firstOutput = decision.output.outputFrameNumber;
        strictlyIncreasing =
            strictlyIncreasing && decision.output.outputFrameNumber > previousOutput;
        previousOutput = decision.output.outputFrameNumber;
    }
    dropped += current.closeBefore(endQpc);
    const long long scheduled = displayed + dropped;
    const bool currentRawExact =
        exactDecisionReplay && noNonSchedulerRepeat && strictlyIncreasing &&
        static_cast<long long>(records.size()) ==
            integer(raw, "measurement_present_callback_count") &&
        scheduled == integer(raw, "measurement_scheduled_output_count") &&
        displayed == integer(raw, "measurement_displayed_composition_count") &&
        dropped == integer(raw, "measurement_drop_scheduler_deadline") &&
        repeated == integer(raw, "measurement_repeated_present_count") &&
        firstOutput == integer(raw, "measurement_first_output_frame");

    const auto candidate = mvm::test::replayNearestOpportunitySlots(callbackQpcs, startQpc, endQpc,
                                                                    frequency, requiredFrames);
    const bool candidateInvariants =
        candidate.scheduled == requiredFrames && candidate.frameIdentityStrictlyIncreasing &&
        candidate.frameZeroStarted && candidate.measurementRangeRespected &&
        candidate.displayed + candidate.deadlineDrop == requiredFrames;

    const QJsonObject output{
        {"schema", "mvm.p5-e4-p2-q4-replay.v1"},
        {"authority", "diagnostic_only_not_closure_evidence"},
        {"input", inputPath},
        {"qpc_frequency", frequency},
        {"start_qpc", startQpc},
        {"end_qpc_exclusive", endQpc},
        {"required_frame_count", requiredFrames},
        {"callback_count", static_cast<qint64>(records.size())},
        {"current_replay", QJsonObject{{"exact", currentRawExact},
                                       {"decision_exact", exactDecisionReplay},
                                       {"no_non_scheduler_repeat", noNonSchedulerRepeat},
                                       {"scheduled", scheduled},
                                       {"displayed", displayed},
                                       {"deadline_drop", dropped},
                                       {"repeated", repeated},
                                       {"first_output_frame", firstOutput},
                                       {"last_output_frame", previousOutput},
                                       {"frame_identity_strictly_increasing", strictlyIncreasing}}},
        {"nearest_slot_candidate",
         QJsonObject{
             {"invariants_pass", candidateInvariants},
             {"scheduled", candidate.scheduled},
             {"displayed", candidate.displayed},
             {"deadline_drop", candidate.deadlineDrop},
             {"repeated", candidate.repeated},
             {"first_output_frame", candidate.firstOutputFrame},
             {"last_output_frame", candidate.lastOutputFrame},
             {"frame_identity_strictly_increasing", candidate.frameIdentityStrictlyIncreasing},
             {"frame_zero_started", candidate.frameZeroStarted},
             {"measurement_range_respected", candidate.measurementRangeRespected}}}};
    if (!writeJson(outputPath, output))
        return 3;
    if (!currentRawExact) {
        std::fprintf(stderr, "現行scheduler replayがQ3 rawをexact再現しませんでした\n");
        return 4;
    }
    if (!candidateInvariants) {
        std::fprintf(stderr, "nearest-slot candidateが基本不変条件を満たしませんでした\n");
        return 5;
    }
    return 0;
}
