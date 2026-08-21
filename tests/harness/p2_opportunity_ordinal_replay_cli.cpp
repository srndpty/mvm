#include "p2_opportunity_ordinal_replay.h"

#include <algorithm>
#include <limits>
#include <map>
#include <vector>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace {

long long integer(const QJsonObject& object, const char* name) {
    return object.value(QLatin1String(name)).toVariant().toLongLong();
}

bool checkedMultiply(long long left, long long right, long long* output) {
    if (left < 0 || right < 0 ||
        (left != 0 && right > std::numeric_limits<long long>::max() / left))
        return false;
    *output = left * right;
    return true;
}

bool roundedIntervals(long long deltaQpc, long long refreshNumerator, long long qpcFrequency,
                      long long refreshDenominator, long long* intervals, bool* midpointTie) {
    long long scaled = 0;
    long long divisor = 0;
    if (!checkedMultiply(deltaQpc, refreshNumerator, &scaled) ||
        !checkedMultiply(qpcFrequency, refreshDenominator, &divisor) || divisor <= 0)
        return false;
    const long long quotient = scaled / divisor;
    const long long remainder = scaled % divisor;
    *midpointTie = remainder * 2 == divisor;
    *intervals = quotient + (remainder * 2 >= divisor ? 1 : 0);
    return true;
}

bool readObject(const QString& path, QJsonObject* object) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    *object = document.object();
    return true;
}

bool writeObject(const QString& path, const QJsonObject& object) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) >= 0;
}

QJsonObject summaryJson(const mvm::test::OpportunityOrdinalSummary& summary) {
    return {{"valid", summary.valid},
            {"error", mvm::test::opportunityOrdinalErrorName(summary.error)},
            {"scheduled", summary.scheduled},
            {"displayed", summary.displayed},
            {"true_dropped", summary.trueDropped},
            {"repeated", summary.repeated},
            {"past_source_domain", summary.pastSourceDomain},
            {"first_output_frame", summary.firstOutputFrame},
            {"last_output_frame", summary.lastOutputFrame},
            {"last_opportunity_ordinal", summary.lastOpportunityOrdinal},
            {"opportunity_gap_count", summary.opportunityGapCount},
            {"frame_zero_started", summary.frameZeroStarted},
            {"unique_frame_strictly_increasing", summary.uniqueFrameStrictlyIncreasing},
            {"source_domain_respected", summary.sourceDomainRespected},
            {"source_domain_conserved", summary.sourceDomainConserved}};
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QString inputPath;
    QString classificationPath;
    QString outputPath;
    const QStringList args = app.arguments();
    for (int index = 1; index < args.size(); ++index) {
        if (args[index] == "--input" && index + 1 < args.size())
            inputPath = args[++index];
        else if (args[index] == "--classification" && index + 1 < args.size())
            classificationPath = args[++index];
        else if (args[index] == "--output" && index + 1 < args.size())
            outputPath = args[++index];
        else {
            std::fprintf(stderr, "使い方: mvm_p2_opportunity_ordinal_replay --input <Q5 raw> "
                                 "--classification <Q5 classification> --output <JSON>\n");
            return 2;
        }
    }
    if (inputPath.isEmpty() || classificationPath.isEmpty() || outputPath.isEmpty()) {
        std::fprintf(stderr, "--input、--classification、--outputが必要です\n");
        return 2;
    }

    QJsonObject raw;
    QJsonObject classification;
    if (!readObject(inputPath, &raw) || !readObject(classificationPath, &classification)) {
        std::fprintf(stderr, "Q5入力JSONを読み取れません\n");
        return 3;
    }
    const QJsonObject presentation = raw.value("presentation_opportunity").toObject();
    const QJsonObject startTiming = presentation.value("dwm_timing_start").toObject();
    const QJsonObject stopTiming = presentation.value("dwm_timing_stop").toObject();
    const QJsonArray renders = presentation.value("render_records").toArray();
    const QJsonArray swaps = presentation.value("swap_records").toArray();
    const long long q5TrueLoss = integer(classification, "true_opportunity_loss_count");
    std::map<long long, long long> q5TrueLossByRender;
    long long q5EventTrueTotal = 0;
    for (const auto& value : classification.value("events").toArray()) {
        const QJsonObject event = value.toObject();
        const long long skipped = integer(event, "skipped");
        const long long actualLost = integer(event, "actual_lost_opportunities");
        const long long trueUnits = std::min(skipped, actualLost);
        if (trueUnits > 0) {
            q5TrueLossByRender[integer(event, "render_ordinal")] = trueUnits;
            q5EventTrueTotal += trueUnits;
        }
    }
    const long long refreshNumerator = integer(startTiming, "display_refresh_numerator");
    const long long refreshDenominator = integer(startTiming, "display_refresh_denominator");
    const long long qpcFrequency = integer(presentation, "qpc_frequency");
    const long long measurementStart = integer(presentation, "measurement_start_qpc");
    const long long measurementEnd = integer(presentation, "measurement_end_qpc_exclusive");
    const bool refreshStable =
        refreshNumerator > 0 && refreshDenominator > 0 &&
        refreshNumerator == integer(stopTiming, "display_refresh_numerator") &&
        refreshDenominator == integer(stopTiming, "display_refresh_denominator");
    const bool dwmAvailable =
        startTiming.value("available").toBool() && stopTiming.value("available").toBool() &&
        startTiming.value("display_config_available").toBool() &&
        stopTiming.value("display_config_available").toBool() &&
        integer(startTiming, "qpc_vblank") > 0 && integer(stopTiming, "qpc_vblank") > 0;
    const bool dwmContinuous =
        integer(stopTiming, "qpc_vblank") > integer(startTiming, "qpc_vblank");

    bool correspondenceComplete =
        !renders.isEmpty() && renders.size() == swaps.size() &&
        renders.size() == integer(raw, "measurement_present_callback_count") &&
        integer(presentation, "render_overflow_count") == 0 &&
        integer(presentation, "swap_overflow_count") == 0;
    bool unambiguous = true;
    std::vector<long long> opportunityOrdinals;
    opportunityOrdinals.reserve(static_cast<std::size_t>(swaps.size()));
    QJsonArray ledger;
    bool q5SitesWithinOrdinalGaps = true;
    long long ordinal = 0;
    long long previousSwapQpc = -1;
    for (qsizetype index = 0; index < swaps.size(); ++index) {
        const QJsonObject render = renders[index].toObject();
        const QJsonObject swap = swaps[index].toObject();
        const long long renderOrdinal = integer(render, "render_ordinal");
        const long long completedOrdinal = integer(swap, "completed_render_ordinal");
        const long long swapOrdinal = integer(swap, "swap_ordinal");
        const long long swapQpc = integer(swap, "swap_qpc");
        correspondenceComplete =
            correspondenceComplete && renderOrdinal == static_cast<long long>(index) &&
            completedOrdinal == renderOrdinal && swapOrdinal == static_cast<long long>(index) &&
            swapQpc >= measurementStart && swapQpc < measurementEnd;

        long long interval = 0;
        bool midpointTie = false;
        if (index > 0) {
            if (swapQpc < previousSwapQpc ||
                !roundedIntervals(swapQpc - previousSwapQpc, refreshNumerator, qpcFrequency,
                                  refreshDenominator, &interval, &midpointTie)) {
                unambiguous = false;
                interval = 0;
            }
            if (midpointTie)
                unambiguous = false;
            ordinal += interval;
        }
        const auto q5TrueIterator = q5TrueLossByRender.find(renderOrdinal);
        const long long q5TrueAtSite =
            q5TrueIterator == q5TrueLossByRender.end() ? 0 : q5TrueIterator->second;
        const long long actualGap = std::max(0LL, interval - 1);
        q5SitesWithinOrdinalGaps = q5SitesWithinOrdinalGaps && q5TrueAtSite <= actualGap;
        opportunityOrdinals.push_back(ordinal);
        ledger.append(QJsonObject{{"swap_ordinal", swapOrdinal},
                                  {"swap_qpc", swapQpc},
                                  {"physical_interval_from_previous", interval},
                                  {"actual_opportunity_gap", actualGap},
                                  {"q5_true_loss_at_synthetic_skip_site", q5TrueAtSite},
                                  {"opportunity_ordinal", ordinal},
                                  {"midpoint_tie", midpointTie}});
        previousSwapQpc = swapQpc;
    }

    const mvm::test::OpportunityOrdinalInput replayInput{3600,
                                                         60,
                                                         1,
                                                         refreshNumerator,
                                                         refreshDenominator,
                                                         refreshStable,
                                                         dwmAvailable,
                                                         dwmContinuous,
                                                         correspondenceComplete,
                                                         unambiguous,
                                                         opportunityOrdinals};
    const auto replay = mvm::test::replayOpportunityOrdinals(replayInput);

    std::vector<long long> completeOrdinals;
    if (!opportunityOrdinals.empty() && ordinal >= 0) {
        completeOrdinals.reserve(static_cast<std::size_t>(ordinal + 1));
        for (long long value = 0; value <= ordinal; ++value)
            completeOrdinals.push_back(value);
    }
    auto baselineInput = replayInput;
    baselineInput.opportunityOrdinals = completeOrdinals;
    const auto cadenceBaseline = mvm::test::replayOpportunityOrdinals(baselineInput);
    const long long missingOrdinalSourceLoss = replay.trueDropped - cadenceBaseline.trueDropped;
    const long long nonSyntheticSkipGapLoss = missingOrdinalSourceLoss - q5TrueLoss;
    const bool reconciliationExact =
        replay.valid && cadenceBaseline.valid && q5TrueLoss >= 0 &&
        q5EventTrueTotal == q5TrueLoss && q5SitesWithinOrdinalGaps &&
        missingOrdinalSourceLoss >= q5TrueLoss &&
        replay.trueDropped == cadenceBaseline.trueDropped + q5TrueLoss + nonSyntheticSkipGapLoss;
    const bool proofPass = replay.valid && correspondenceComplete && unambiguous &&
                           replay.firstOutputFrame == 0 && replay.lastOutputFrame < 3600 &&
                           replay.uniqueFrameStrictlyIncreasing &&
                           replay.displayed + replay.trueDropped == 3600 && reconciliationExact;

    const QJsonObject output{
        {"schema", "mvm.p5-e4-p2-q6-opportunity-ordinal.v1"},
        {"authority", "diagnostic_only_not_closure_evidence"},
        {"input", inputPath},
        {"classification_input", classificationPath},
        {"proof_pass", proofPass},
        {"source_fps", QJsonObject{{"numerator", 60}, {"denominator", 1}}},
        {"refresh", QJsonObject{{"numerator", refreshNumerator},
                                {"denominator", refreshDenominator},
                                {"stable", refreshStable}}},
        {"authority_checks", QJsonObject{{"dwm_available", dwmAvailable},
                                         {"dwm_continuous", dwmContinuous},
                                         {"swap_correspondence_complete", correspondenceComplete},
                                         {"opportunity_unambiguous", unambiguous}}},
        {"replay", summaryJson(replay)},
        {"complete_ordinal_cadence_baseline", summaryJson(cadenceBaseline)},
        {"reconciliation",
         QJsonObject{{"exact", reconciliationExact},
                     {"q6_true_dropped", replay.trueDropped},
                     {"cadence_and_domain_loss", cadenceBaseline.trueDropped},
                     {"q5_true_loss_at_synthetic_skip_sites", q5TrueLoss},
                     {"q5_event_true_total", q5EventTrueTotal},
                     {"q5_sites_within_actual_ordinal_gaps", q5SitesWithinOrdinalGaps},
                     {"other_ordinal_gap_source_loss", nonSyntheticSkipGapLoss},
                     {"missing_ordinal_source_loss", missingOrdinalSourceLoss},
                     {"identity",
                      "q6_true_dropped = cadence_and_domain_loss + "
                      "q5_true_loss_at_synthetic_skip_sites + other_ordinal_gap_source_loss"}}},
        {"ledger", ledger}};
    if (!writeObject(outputPath, output)) {
        std::fprintf(stderr, "Q6出力JSONを書き込めません\n");
        return 3;
    }
    if (!proofPass) {
        std::fprintf(stderr, "Q6 opportunity ordinal proofが成立しませんでした\n");
        return 4;
    }
    return 0;
}
