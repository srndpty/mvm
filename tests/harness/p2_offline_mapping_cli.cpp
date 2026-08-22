#include "core/presentation_opportunity_mapper.h"

#include <cstdio>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

namespace {

bool exactKeys(const QJsonObject& object, std::initializer_list<const char*> keys) {
    QSet<QString> expected;
    for (const auto* key : keys)
        expected.insert(QString::fromLatin1(key));
    QSet<QString> actual;
    for (const auto& key : object.keys())
        actual.insert(key);
    return actual == expected;
}

bool readObject(const QString& path, QJsonObject& output) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    output = document.object();
    return true;
}

bool writeObject(const QString& path, const QJsonObject& value) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(QJsonDocument(value).toJson(QJsonDocument::Indented)) >= 0;
}

long long integer(const QJsonObject& object, const char* name) {
    return object.value(QLatin1String(name)).toVariant().toLongLong();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QString inputPath;
    QString outputPath;
    const auto args = app.arguments();
    for (int index = 1; index < args.size(); ++index) {
        if (args[index] == QStringLiteral("--input") && index + 1 < args.size())
            inputPath = args[++index];
        else if (args[index] == QStringLiteral("--output") && index + 1 < args.size())
            outputPath = args[++index];
        else {
            std::fprintf(stderr, "使い方: mvm_p2_offline_mapping --input <visible JSON> "
                                 "--output <result JSON>\n");
            return 2;
        }
    }
    if (inputPath.isEmpty() || outputPath.isEmpty()) {
        std::fprintf(stderr, "--inputと--outputが必要です\n");
        return 2;
    }

    QJsonObject input;
    if (!readObject(inputPath, input) ||
        !exactKeys(input, {"schema", "case_id", "sync_interval", "measurement_start_qpc",
                           "measurement_end_qpc_exclusive", "vblank_samples", "callbacks"}) ||
        input.value("schema").toString() != QStringLiteral("mvm-p2-r4-visible-input-1")) {
        std::fprintf(stderr, "mapper-visible入力schemaまたはfieldが不正です\n");
        return 3;
    }

    mvm::core::ObservableMappingInput mappingInput;
    mappingInput.syncInterval = input.value("sync_interval").toInt();
    mappingInput.measurementStartQpc = integer(input, "measurement_start_qpc");
    mappingInput.measurementEndQpcExclusive = integer(input, "measurement_end_qpc_exclusive");
    for (const auto& value : input.value("vblank_samples").toArray()) {
        if (!value.isObject() || !exactKeys(value.toObject(), {"ordinal", "qpc"})) {
            std::fprintf(stderr, "VBlank sampleに許可されていないfieldがあります\n");
            return 3;
        }
        const auto object = value.toObject();
        mappingInput.vblankSamples.push_back({integer(object, "ordinal"), integer(object, "qpc")});
    }
    long long expectedSubmission = -1;
    for (const auto& value : input.value("callbacks").toArray()) {
        if (!value.isObject() ||
            !exactKeys(value.toObject(), {"submission_index", "synthetic_callback_qpc"})) {
            std::fprintf(stderr, "callbackにoracleまたはsynthetic delay fieldが混入しています\n");
            return 3;
        }
        const auto object = value.toObject();
        const auto submission = integer(object, "submission_index");
        if (expectedSubmission >= 0 && submission != expectedSubmission + 1) {
            std::fprintf(stderr, "submission orderが連続していません\n");
            return 3;
        }
        expectedSubmission = submission;
        mappingInput.callbackQpc.push_back(integer(object, "synthetic_callback_qpc"));
    }

    mvm::core::OpportunityCandidateMatrix candidates;
    if (!mvm::core::buildObservableCandidateMatrix(mappingInput, candidates)) {
        std::fprintf(stderr, "mapper-visible入力からcandidate relationを構築できません\n");
        return 3;
    }
    const auto result = mvm::core::solveOpportunityMapping(candidates);
    QJsonArray assignment;
    for (const auto ordinal : result.assignment)
        assignment.append(ordinal);
    const QJsonObject output{
        {"schema", "mvm-p2-r4-mapper-result-1"},
        {"case_id", input.value("case_id")},
        {"solution_class", mvm::core::mappingSolutionClassName(result.solutionClass)},
        {"saturated_solution_count", result.saturatedSolutionCount},
        {"assignment", assignment},
        {"admissibility_relation",
         "VISIBLE_PREFIX: opportunity_start_qpc <= synthetic_callback_qpc"},
        {"hidden_oracle_accessed", false},
    };
    if (!writeObject(outputPath, output)) {
        std::fprintf(stderr, "mapper結果を書き込めません\n");
        return 4;
    }
    return 0;
}
