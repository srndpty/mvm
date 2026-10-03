#include "project/subtitles.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace mvm::project {
namespace {
__extension__ using WideInteger = __int128;

bool convert(std::int64_t value, WideInteger num, WideInteger den, std::int64_t& output,
             std::string& error) {
    if (value < 0 || num <= 0 || den <= 0) {
        error = "字幕の時刻または時間基準が不正です";
        return false;
    }
    if (value != 0 && num > (std::numeric_limits<WideInteger>::max() - den / 2) / value) {
        error = "字幕の時間換算が範囲外です";
        return false;
    }
    const auto rounded = (static_cast<WideInteger>(value) * num + den / 2) / den;
    if (rounded > std::numeric_limits<std::int64_t>::max()) {
        error = "字幕の時刻が範囲外です";
        return false;
    }
    output = static_cast<std::int64_t>(rounded);
    return true;
}

bool validUtf8(std::string_view text, bool* nonBlank = nullptr) {
    if (nonBlank)
        *nonBlank = false;
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i++]);
        if (c < 128) {
            if ((c < 32 && c != '\n' && c != '\t' && c != '\r') || c == 127)
                return false;
            if (nonBlank && !std::isspace(c))
                *nonBlank = true;
            continue;
        }
        unsigned cp = 0, count = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            cp = c & 31;
            count = 1;
            minimum = 128;
        } else if (c >= 0xe0 && c <= 0xef) {
            cp = c & 15;
            count = 2;
            minimum = 2048;
        } else if (c >= 0xf0 && c <= 0xf4) {
            cp = c & 7;
            count = 3;
            minimum = 65536;
        } else
            return false;
        while (count--) {
            if (i == text.size())
                return false;
            c = static_cast<unsigned char>(text[i++]);
            if ((c & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        const bool space = cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
                           (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
                           cp == 0x202f || cp == 0x205f || cp == 0x3000;
        if (nonBlank && !space)
            *nonBlank = true;
    }
    return true;
}

bool color(const std::string& text) {
    return text.size() == 9 && text[0] == '#' &&
           std::all_of(text.begin() + 1, text.end(),
                       [](unsigned char c) { return std::isxdigit(c); });
}

bool timestamp(std::string_view text, std::int64_t& ms) {
    const auto colon = text.find(':');
    if (colon < 2 || colon == std::string_view::npos || text.size() != colon + 10 ||
        text[colon + 3] != ':' || text[colon + 6] != ',')
        return false;
    std::int64_t hours = 0;
    for (std::size_t i = 0; i < colon; ++i) {
        if (text[i] < '0' || text[i] > '9' || hours > 100000000)
            return false;
        hours = hours * 10 + text[i] - '0';
    }
    auto part = [&](std::size_t at, int count) {
        int value = 0;
        for (int j = 0; j < count; ++j) {
            const char c = text[at + static_cast<std::size_t>(j)];
            if (c < '0' || c > '9')
                return -1;
            value = value * 10 + c - '0';
        }
        return value;
    };
    const int minutes = part(colon + 1, 2), seconds = part(colon + 4, 2),
              millis = part(colon + 7, 3);
    if (minutes < 0 || minutes > 59 || seconds < 0 || seconds > 59 || millis < 0)
        return false;
    ms = ((hours * 60 + minutes) * 60 + seconds) * 1000 + millis;
    return true;
}

std::string formatTime(std::int64_t ms) {
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << ms / 3600000 << ':' << std::setw(2)
        << ms / 60000 % 60 << ':' << std::setw(2) << ms / 1000 % 60 << ',' << std::setw(3)
        << ms % 1000;
    return out.str();
}

// SRT の装飾記法か。対応していないので読み込みを拒否する。実際の記法だけを見る:
// <b> <i> <u> <s> <font ...> とその閉じタグ、ASS の上書き {\an8} など。
// "a < b" や "std::vector<int>" や "{ x > 0 }" は普通の本文として通す。
bool hasSrtMarkup(std::string_view line) {
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '{' && i + 1 < line.size() && line[i + 1] == '\\')
            return true;
        if (line[i] != '<')
            continue;
        std::size_t j = i + 1;
        if (j < line.size() && line[j] == '/')
            ++j;
        std::string name;
        while (j < line.size() && std::isalpha(static_cast<unsigned char>(line[j])))
            name += lower(line[j++]);
        if (j >= line.size())
            continue;
        if ((name == "b" || name == "i" || name == "u" || name == "s") && line[j] == '>')
            return true;
        if (name == "font" && (line[j] == '>' || line[j] == ' ') &&
            line.find('>', j) != std::string_view::npos)
            return true;
    }
    return false;
}

void sortCues(SubtitleTrack& track) {
    std::sort(track.cues.begin(), track.cues.end(),
              [](const auto& a, const auto& b) { return a.startFrame < b.startFrame; });
}

bool commit(Project& project, Project candidate, std::string& error) {
    if (!validateSubtitles(candidate, error))
        return false;
    project = std::move(candidate);
    return true;
}
} // namespace

bool subtitleCuesFromTimedText(const std::vector<SubtitleTimedText>& segments, std::int64_t fpsNum,
                               std::int64_t fpsDen, std::int64_t speedNum, std::int64_t speedDen,
                               std::int64_t offsetFrame, const std::function<std::string()>& newId,
                               std::vector<SubtitleCue>& cues, std::string& error) {
    if (fpsNum <= 0 || fpsDen <= 0 || speedNum <= 0 || speedDen <= 0 || offsetFrame < 0 ||
        segments.empty()) {
        error = "認識結果の時間基準または件数が不正です";
        return false;
    }
    // 設定済みレート・速度の範囲で積を制限し、128bitの積のoverflowも防ぐ。
    if (fpsNum > 1000000 || fpsDen > 1000000 || speedNum > 1000000 || speedDen > 1000000) {
        error = "認識結果の時間基準が範囲外です";
        return false;
    }
    std::vector<SubtitleCue> result;
    for (const auto& segment : segments) {
        SubtitleCue cue;
        cue.id = newId();
        cue.content = segment.content;
        const auto num = static_cast<WideInteger>(fpsNum) * speedDen;
        const auto den = static_cast<WideInteger>(fpsDen) * speedNum * 1000;
        if (segment.endMs <= segment.startMs ||
            !convert(segment.startMs, num, den, cue.startFrame, error) ||
            !convert(segment.endMs, num, den, cue.endFrame, error)) {
            error = "認識結果の時刻が不正です";
            return false;
        }
        if (cue.endFrame > std::numeric_limits<std::int64_t>::max() - offsetFrame) {
            error = "字幕の配置時刻が範囲外です";
            return false;
        }
        cue.startFrame += offsetFrame;
        cue.endFrame += offsetFrame;
        result.push_back(std::move(cue));
    }
    auto check = createDefaultProject();
    check.subtitles.emplace();
    check.subtitles->cues = result;
    if (!validateSubtitles(check, error))
        return false;
    cues = std::move(result);
    return true;
}

bool subtitleTimeToFrame(std::int64_t ms, std::int64_t num, std::int64_t den, std::int64_t& frame,
                         std::string& error) {
    return convert(ms, num, static_cast<WideInteger>(den) * 1000, frame, error);
}

bool subtitleFrameToTime(std::int64_t frame, std::int64_t num, std::int64_t den, std::int64_t& ms,
                         std::string& error) {
    return convert(frame, static_cast<WideInteger>(den) * 1000, num, ms, error);
}

bool validateSubtitles(const Project& project, std::string& error) {
    error.clear();
    if (!project.subtitles)
        return true;
    const auto& style = project.subtitles->style;
    if (style.fontFamily.empty() || style.fontSize < 1 || style.fontSize > project.outputHeight ||
        style.outlineWidth < 0 || style.outlineWidth > 32 || !color(style.color) ||
        !color(style.outlineColor) || !color(style.backgroundColor) ||
        (style.alignment != "left" && style.alignment != "center" && style.alignment != "right") ||
        !std::isfinite(style.sideMargin) || style.sideMargin < 0 || style.sideMargin >= 0.5 ||
        !std::isfinite(style.bottomMargin) || style.bottomMargin < 0 || style.bottomMargin >= 1) {
        error = "字幕の共通書式が不正です";
        return false;
    }
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> clipIds;
    for (const auto& clip : project.timelineClips)
        clipIds.insert(clip.id);
    std::int64_t previousEnd = 0;
    for (const auto& cue : project.subtitles->cues) {
        bool nonBlank = false;
        if (cue.id.empty() || !ids.insert(cue.id).second || cue.startFrame < previousEnd ||
            cue.endFrame <= cue.startFrame || cue.content.empty() ||
            !validUtf8(cue.content, &nonBlank) || !nonBlank) {
            error = "字幕のID・本文・区間または並び順が不正です: " + cue.id;
            return false;
        }
        if (!cue.linkClipId.empty() && !clipIds.contains(cue.linkClipId)) {
            error = "字幕のリンク先 clip がありません: " + cue.id;
            return false;
        }
        previousEnd = cue.endFrame;
    }
    return true;
}

std::optional<std::int64_t> linkedSubtitleStart(const Project& project,
                                                const std::unordered_set<std::string>& clipIds) {
    std::optional<std::int64_t> start;
    if (project.subtitles)
        for (const auto& cue : project.subtitles->cues)
            if (!cue.linkClipId.empty() && clipIds.contains(cue.linkClipId))
                start = std::min(start.value_or(cue.startFrame), cue.startFrame);
    return start;
}

bool shiftLinkedSubtitles(Project& project, const std::unordered_set<std::string>& clipIds,
                          std::int64_t delta, std::string& error) {
    if (!project.subtitles || delta == 0)
        return true;
    bool moved = false;
    for (auto& cue : project.subtitles->cues) {
        if (cue.linkClipId.empty() || !clipIds.contains(cue.linkClipId))
            continue;
        if (cue.startFrame + delta < 0 ||
            (delta > 0 && cue.endFrame > std::numeric_limits<std::int64_t>::max() - delta)) {
            error = "リンクした字幕の移動先が範囲外です";
            return false;
        }
        cue.startFrame += delta;
        cue.endFrame += delta;
        moved = true;
    }
    if (!moved)
        return true;
    sortCues(*project.subtitles);
    const auto& cues = project.subtitles->cues;
    for (std::size_t i = 1; i < cues.size(); ++i)
        if (cues[i].startFrame < cues[i - 1].endFrame) {
            // 字幕は 1 本のトラックで重なりを持てない。どちらを優先するか決められないので、
            // 部分的に動かさず移動全体を拒否する。
            error = "リンクした字幕が他の字幕と重なるため移動できません";
            return false;
        }
    return true;
}

void eraseLinkedSubtitles(Project& project, const std::unordered_set<std::string>& clipIds) {
    if (project.subtitles)
        std::erase_if(project.subtitles->cues, [&](const SubtitleCue& cue) {
            return !cue.linkClipId.empty() && clipIds.contains(cue.linkClipId);
        });
}

void relinkSubtitlesAfterSplit(Project& project, const std::string& leftId,
                               const std::string& rightId, std::int64_t frame,
                               const std::function<std::string()>& newId) {
    if (!project.subtitles)
        return;
    std::vector<SubtitleCue> rights;
    for (auto& cue : project.subtitles->cues) {
        if (cue.linkClipId != leftId)
            continue;
        if (cue.startFrame >= frame) {
            cue.linkClipId = rightId;
        } else if (cue.endFrame > frame) {
            auto right = cue;
            right.id = newId();
            right.startFrame = frame;
            right.linkClipId = rightId;
            const double ratio = static_cast<double>(frame - cue.startFrame) /
                                 static_cast<double>(cue.endFrame - cue.startFrame);
            std::tie(cue.content, right.content) =
                splitSubtitleText(cue.content, ratio, std::nullopt);
            cue.endFrame = frame;
            rights.push_back(std::move(right));
        }
    }
    project.subtitles->cues.insert(project.subtitles->cues.end(), rights.begin(), rights.end());
    sortCues(*project.subtitles);
}

namespace {
// clip の時間の対応。timeline の frame t と素材の秒 s の関係:
//   s = in + (t - start) / timelineFps * speed
struct ClipMapping {
    long double start = 0;  // timeline frame
    long double inSec = 0;  // 素材の秒
    long double outSec = 0; // 素材の秒
    long double speed = 1;
    bool operator==(const ClipMapping&) const = default;
};

ClipMapping clipMapping(const TimelineClip& clip) {
    const long double sourceFps =
        static_cast<long double>(clip.sourceFpsNum) /
        static_cast<long double>(std::max<std::int64_t>(1, clip.sourceFpsDen));
    return {static_cast<long double>(clip.timelineStartFrame),
            static_cast<long double>(clip.sourceInFrame) / sourceFps,
            static_cast<long double>(clip.sourceOutFrame) / sourceFps,
            static_cast<long double>(clip.speedNum) /
                static_cast<long double>(std::max<std::int64_t>(1, clip.speedDen))};
}
} // namespace

bool remapLinkedSubtitles(const Project& before, Project& candidate, std::string& error) {
    if (!before.subtitles || !candidate.subtitles)
        return true;
    const long double timelineFps = static_cast<long double>(candidate.timelineFpsNum) /
                                    static_cast<long double>(candidate.timelineFpsDen);
    // 時間の対応が変わった clip。素材の時間軸を持たない clip (文字・静止画) は対象外。
    std::unordered_map<std::string, std::pair<ClipMapping, ClipMapping>> changed;
    // clip 数に比例する時間で済ませる (clip ごとに全 clip を探さない)。
    std::unordered_map<std::string_view, const TimelineClip*> beforeById;
    beforeById.reserve(before.timelineClips.size());
    for (const auto& clip : before.timelineClips)
        beforeById.emplace(clip.id, &clip);
    for (const auto& after : candidate.timelineClips) {
        if (hasSyntheticSourceDomain(after) || after.sourceFpsNum <= 0)
            continue;
        const auto entry = beforeById.find(after.id);
        if (entry == beforeById.end())
            continue;
        const auto* found = entry->second;
        if (found->sourceFpsNum != after.sourceFpsNum || found->sourceFpsDen != after.sourceFpsDen)
            continue;
        const auto old = clipMapping(*found), now = clipMapping(after);
        if (!(old == now))
            changed.emplace(after.id, std::pair{old, now});
    }
    if (changed.empty())
        return true;
    // 対象の clip にリンクした字幕は、編集前の位置から作り直す。
    std::erase_if(candidate.subtitles->cues,
                  [&](const SubtitleCue& cue) { return changed.contains(cue.linkClipId); });
    for (const auto& cue : before.subtitles->cues) {
        const auto found = changed.find(cue.linkClipId);
        if (found == changed.end())
            continue;
        const auto& [old, now] = found->second;
        const auto toSource = [&](long double frame) {
            return old.inSec + (frame - old.start) / timelineFps * old.speed;
        };
        auto startSec = toSource(static_cast<long double>(cue.startFrame));
        auto endSec = toSource(static_cast<long double>(cue.endFrame));
        // trim で素材の範囲から外した部分を除く。元から clip の外にはみ出していた部分は残す。
        if (now.inSec > old.inSec && startSec < now.inSec && endSec > old.inSec)
            startSec = std::max(startSec, now.inSec);
        if (now.outSec < old.outSec && endSec > now.outSec && startSec < old.outSec)
            endSec = std::min(endSec, now.outSec);
        if (endSec <= startSec)
            continue;
        const auto toTimeline = [&](long double seconds) {
            return std::llround(now.start + (seconds - now.inSec) / now.speed * timelineFps);
        };
        auto remapped = cue;
        remapped.startFrame = std::max<std::int64_t>(0, toTimeline(startSec));
        remapped.endFrame = toTimeline(endSec);
        // 音声は残っているのに frame へ丸めると 0 frame になる。黙って消さずに拒否する。
        if (remapped.endFrame <= remapped.startFrame) {
            error = "この速度ではリンクした字幕の区間を 1 frame 以上で表せません: " + cue.content;
            return false;
        }
        candidate.subtitles->cues.push_back(std::move(remapped));
    }
    sortCues(*candidate.subtitles);
    const auto& cues = candidate.subtitles->cues;
    for (std::size_t i = 1; i < cues.size(); ++i)
        if (cues[i].startFrame < cues[i - 1].endFrame) {
            error = "リンクした字幕が他の字幕と重なるため、この編集はできません";
            return false;
        }
    return true;
}

std::size_t unlinkSubtitles(Project& project, const std::unordered_set<std::string>& clipIds) {
    std::size_t count = 0;
    if (project.subtitles)
        for (auto& cue : project.subtitles->cues)
            if (!cue.linkClipId.empty() && clipIds.contains(cue.linkClipId)) {
                cue.linkClipId.clear();
                ++count;
            }
    return count;
}

void reconcileSubtitleLinks(Project& project) {
    if (!project.subtitles)
        return;
    std::unordered_set<std::string> clipIds;
    for (const auto& clip : project.timelineClips)
        clipIds.insert(clip.id);
    for (auto& cue : project.subtitles->cues)
        if (!cue.linkClipId.empty() && !clipIds.contains(cue.linkClipId))
            cue.linkClipId.clear();
}

bool subtitleContainsFrame(const SubtitleCue& cue, std::int64_t frame) {
    return frame >= cue.startFrame && frame < cue.endFrame;
}

const SubtitleCue* activeSubtitleAt(const Project& project, std::int64_t frame) {
    if (!project.subtitles || !project.subtitles->visible)
        return nullptr;
    const auto& cues = project.subtitles->cues;
    auto it = std::upper_bound(cues.begin(), cues.end(), frame,
                               [](auto value, const auto& cue) { return value < cue.startFrame; });
    if (it == cues.begin())
        return nullptr;
    --it;
    return subtitleContainsFrame(*it, frame) ? &*it : nullptr;
}

bool applySubtitleCues(Project& project, std::vector<SubtitleCue> cues, bool replace,
                       std::string& error) {
    Project candidate = project;
    if (!candidate.subtitles)
        candidate.subtitles.emplace();
    if (replace)
        candidate.subtitles->cues.clear();
    candidate.subtitles->cues.insert(candidate.subtitles->cues.end(), cues.begin(), cues.end());
    sortCues(*candidate.subtitles);
    return commit(project, std::move(candidate), error);
}

bool editSubtitle(Project& project, SubtitleCue cue, std::string& error) {
    Project candidate = project;
    if (!candidate.subtitles)
        candidate.subtitles.emplace();
    auto& cues = candidate.subtitles->cues;
    auto it = std::find_if(cues.begin(), cues.end(), [&](const auto& c) { return c.id == cue.id; });
    if (it == cues.end())
        cues.push_back(std::move(cue));
    else
        *it = std::move(cue);
    sortCues(*candidate.subtitles);
    return commit(project, std::move(candidate), error);
}

bool deleteSubtitle(Project& project, const std::string& id, std::string& error) {
    Project candidate = project;
    if (!candidate.subtitles ||
        std::erase_if(candidate.subtitles->cues, [&](const auto& c) { return c.id == id; }) == 0) {
        error = "削除する字幕がありません";
        return false;
    }
    return commit(project, std::move(candidate), error);
}

std::pair<std::string, std::string> splitSubtitleText(const std::string& content, double ratio,
                                                      std::optional<std::size_t> textBreak) {
    // UTF-8 を文字 (code point) の境界で扱う。検証済みの本文だけが来る。
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < content.size(); ++i)
        if ((static_cast<unsigned char>(content[i]) & 0xc0) != 0x80)
            starts.push_back(i);
    const auto count = starts.size();
    if (count < 2)
        return {content, content};
    const auto charAt = [&](std::size_t index) {
        const auto end = index + 1 < count ? starts[index + 1] : content.size();
        return std::string_view(content).substr(starts[index], end - starts[index]);
    };
    std::size_t at = 0;
    if (textBreak && *textBreak > 0 && *textBreak < count) {
        at = *textBreak;
    } else {
        const auto target = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::llround(std::clamp(ratio, 0.0, 1.0) * count)), 1,
            count - 1);
        // 句読点・空白・改行の直後を候補にし、比率の位置に最も近いものを選ぶ。
        static constexpr std::string_view kBreaks[] = {"、", "。", "，", "．", "！", "？", "!",
                                                       "?",  "…",  " ",  "　", "\n", ",",  "."};
        std::optional<std::size_t> best;
        for (std::size_t i = 1; i < count; ++i) {
            const auto previous = charAt(i - 1);
            if (std::find(std::begin(kBreaks), std::end(kBreaks), previous) == std::end(kBreaks))
                continue;
            const auto distance = i > target ? i - target : target - i;
            if (!best || distance < (*best > target ? *best - target : target - *best))
                best = i;
        }
        at = best.value_or(target);
    }
    const auto trim = [](std::string text) {
        const auto first = text.find_first_not_of(" \n\r\t");
        if (first == std::string::npos)
            return std::string{};
        return text.substr(first, text.find_last_not_of(" \n\r\t") - first + 1);
    };
    auto left = trim(content.substr(0, starts[at]));
    auto right = trim(content.substr(starts[at]));
    // 片方が空白だけになる位置では分けない (空の字幕は作れない)。
    if (left.empty() || right.empty())
        return {content, content};
    return {left, right};
}

bool splitSubtitle(Project& project, const std::string& id, std::int64_t frame,
                   const std::string& newId, std::string& error,
                   std::optional<std::size_t> textBreak) {
    Project candidate = project;
    if (candidate.subtitles)
        for (auto& cue : candidate.subtitles->cues)
            if (cue.id == id && cue.startFrame < frame && frame < cue.endFrame) {
                auto right = cue;
                right.id = newId;
                right.startFrame = frame;
                const double ratio = static_cast<double>(frame - cue.startFrame) /
                                     static_cast<double>(cue.endFrame - cue.startFrame);
                std::tie(cue.content, right.content) =
                    splitSubtitleText(cue.content, ratio, textBreak);
                cue.endFrame = frame;
                candidate.subtitles->cues.push_back(std::move(right));
                sortCues(*candidate.subtitles);
                return commit(project, std::move(candidate), error);
            }
    error = "字幕の区間内で分割してください";
    return false;
}

bool mergeSubtitle(Project& project, const std::string& id, std::string& error) {
    Project candidate = project;
    if (candidate.subtitles) {
        auto& cues = candidate.subtitles->cues;
        for (std::size_t i = 0; i + 1 < cues.size(); ++i)
            if (cues[i].id == id) {
                cues[i].endFrame = cues[i + 1].endFrame;
                cues[i].content += '\n' + cues[i + 1].content;
                cues.erase(cues.begin() + static_cast<std::ptrdiff_t>(i + 1));
                return commit(project, std::move(candidate), error);
            }
    }
    error = "結合する次の字幕がありません";
    return false;
}

bool editSubtitleTime(Project& project, std::int64_t start, std::int64_t removed,
                      std::int64_t inserted, const std::function<std::string()>& newId,
                      std::string& error) {
    if (start < 0 || removed < 0 || inserted < 0 ||
        start > std::numeric_limits<std::int64_t>::max() - removed) {
        error = "字幕の時間編集区間が不正です";
        return false;
    }
    if (!project.subtitles || (removed == 0 && inserted == 0))
        return true;
    std::vector<SubtitleCue> output;
    const auto end = start + removed;
    for (const auto& cue : project.subtitles->cues) {
        if (cue.startFrame < start) {
            auto left = cue;
            left.endFrame = std::min(cue.endFrame, start);
            output.push_back(std::move(left));
        }
        if (cue.endFrame > end) {
            auto right = cue;
            right.startFrame = std::max(cue.startFrame, end);
            if (right.startFrame != cue.startFrame && cue.startFrame < start)
                right.id = newId();
            const WideInteger a = static_cast<WideInteger>(right.startFrame) - removed + inserted,
                              b = static_cast<WideInteger>(right.endFrame) - removed + inserted;
            if (b > std::numeric_limits<std::int64_t>::max()) {
                error = "字幕の移動先が範囲外です";
                return false;
            }
            right.startFrame = static_cast<std::int64_t>(a);
            right.endFrame = static_cast<std::int64_t>(b);
            output.push_back(std::move(right));
        }
    }
    Project candidate = project;
    candidate.subtitles->cues = std::move(output);
    return commit(project, std::move(candidate), error);
}

bool parseSrt(std::string_view text, std::int64_t num, std::int64_t den,
              const std::function<std::string()>& newId, std::vector<SubtitleCue>& cues,
              std::string& error) {
    if (text.starts_with("\xef\xbb\xbf"))
        text.remove_prefix(3);
    if (!validUtf8(text)) {
        error = "SRTはUTF-8で保存してください";
        return false;
    }
    std::istringstream in{std::string(text)};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    std::vector<SubtitleCue> parsed;
    auto fail = [&](std::size_t i, const std::string& reason) {
        error = "SRT " + std::to_string(i + 1) + "行目: " + reason;
        return false;
    };
    for (std::size_t i = 0; i < lines.size();) {
        if (lines[i].empty()) {
            ++i;
            continue;
        }
        const auto first = i;
        if (!std::all_of(lines[i].begin(), lines[i].end(),
                         [](unsigned char c) { return std::isdigit(c); }))
            return fail(i, "字幕番号が不正です");
        if (++i >= lines.size())
            return fail(first, "時刻がありません");
        const auto arrow = lines[i].find(" --> ");
        std::int64_t a = 0, b = 0;
        if (arrow == std::string::npos ||
            !timestamp(std::string_view(lines[i]).substr(0, arrow), a) ||
            !timestamp(std::string_view(lines[i]).substr(arrow + 5), b) || a >= b)
            return fail(i, "時刻または未対応の位置指定です");
        SubtitleCue cue;
        cue.id = newId();
        if (!subtitleTimeToFrame(a, num, den, cue.startFrame, error) ||
            !subtitleTimeToFrame(b, num, den, cue.endFrame, error))
            return fail(i, error);
        for (++i; i < lines.size() && !lines[i].empty(); ++i) {
            if (hasSrtMarkup(lines[i]))
                return fail(i, "装飾記法には対応していません");
            if (!cue.content.empty())
                cue.content += '\n';
            cue.content += lines[i];
        }
        if (cue.content.empty() || cue.endFrame <= cue.startFrame ||
            (!parsed.empty() && cue.startFrame < parsed.back().endFrame))
            return fail(first, "空本文・重複・並び順または丸め後の尺が不正です");
        parsed.push_back(std::move(cue));
    }
    if (parsed.empty()) {
        error = "SRTに字幕がありません";
        return false;
    }
    Project check = createDefaultProject();
    check.subtitles.emplace();
    check.subtitles->cues = parsed;
    if (!validateSubtitles(check, error))
        return false;
    cues = std::move(parsed);
    error.clear();
    return true;
}

bool writeSrt(const Project& project, std::string& text, std::string& error) {
    if (!validateSubtitles(project, error))
        return false;
    std::ostringstream out;
    if (project.subtitles) {
        std::size_t index = 0;
        for (const auto& cue : project.subtitles->cues) {
            std::int64_t a = 0, b = 0;
            if (!subtitleFrameToTime(cue.startFrame, project.timelineFpsNum, project.timelineFpsDen,
                                     a, error) ||
                !subtitleFrameToTime(cue.endFrame, project.timelineFpsNum, project.timelineFpsDen,
                                     b, error))
                return false;
            out << ++index << '\n'
                << formatTime(a) << " --> " << formatTime(b) << '\n'
                << cue.content << "\n\n";
        }
    }
    const auto serialized = out.str();
    if (!serialized.empty()) {
        std::vector<SubtitleCue> verified;
        std::size_t index = 0;
        if (!parseSrt(
                serialized, project.timelineFpsNum, project.timelineFpsDen,
                [&] {
                    return index < project.subtitles->cues.size()
                               ? project.subtitles->cues[index++].id
                               : "srt-extra-" + std::to_string(++index);
                },
                verified, error)) {
            error = "SRTを書き出せません: " + error;
            return false;
        }
        // SRT が表せるのは区間と本文だけ。clip とのリンク (linkClipId) は mvm の Project の
        // 情報なので比べない (比べると、文字起こしした字幕を SRT へ出せなくなる)。
        const auto& original = project.subtitles->cues;
        const bool kept = verified.size() == original.size() &&
                          std::equal(verified.begin(), verified.end(), original.begin(),
                                     [](const SubtitleCue& reparsed, const SubtitleCue& cue) {
                                         return reparsed.id == cue.id &&
                                                reparsed.startFrame == cue.startFrame &&
                                                reparsed.endFrame == cue.endFrame &&
                                                reparsed.content == cue.content;
                                     });
        if (!kept) {
            error = "SRTのミリ秒単位で本文・字幕区間を保持できません";
            return false;
        }
    }
    text = serialized;
    return true;
}
} // namespace mvm::project
