#include "UiModel.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace rm::ui {

namespace {

std::vector<CellChange> ChangedCells(const Row& current, const Row& next) {
    std::vector<CellChange> changes;
    const size_t count = std::max(current.cells.size(), next.cells.size());
    for (size_t column = 0; column < count; ++column) {
        const std::wstring currentValue = column < current.cells.size()
            ? current.cells[column] : std::wstring{};
        const std::wstring nextValue = column < next.cells.size()
            ? next.cells[column] : std::wstring{};
        if (currentValue != nextValue) changes.push_back({column, nextValue});
    }
    return changes;
}

std::wstring CompactFrames(const std::vector<int>& frames) {
    if (frames.empty()) return {};
    std::wstring value;
    size_t index = 0;
    while (index < frames.size()) {
        const int first = frames[index];
        int last = first;
        while (index + 1 < frames.size() && frames[index + 1] == last + 1) {
            last = frames[++index];
        }
        if (!value.empty()) value += L", ";
        value += std::to_wstring(first);
        if (last != first) value += L"\u2013" + std::to_wstring(last);
        ++index;
    }
    return value;
}

}  // namespace

std::wstring FormatWorkerActivity(const WorkerActivityView& activity) {
    if (activity.jobName.empty() && activity.frames.empty()) return L"\u2014";

    std::wstring value;
    if (!activity.rendering) value = L"\u51c6\u5907\u4e2d \u00b7 ";
    value += activity.jobName.empty() ? L"\u672a\u547d\u540d\u4f5c\u4e1a" : activity.jobName;
    if (!activity.frames.empty()) {
        value += L" \u00b7 \u5e27 " + CompactFrames(activity.frames);
        value += L" \u00b7 " + std::to_wstring(
            std::min(activity.completedFrames, activity.frames.size())) + L"/" +
            std::to_wstring(activity.frames.size());
    }
    return value;
}

std::vector<Operation> StableTableModel::Reconcile(std::vector<Row> next) {
    std::vector<Operation> operations;

    std::unordered_set<std::wstring> nextKeys;
    nextKeys.reserve(next.size());
    for (const auto& row : next) nextKeys.insert(row.key);
    for (size_t index = rows_.size(); index-- > 0;) {
        if (nextKeys.contains(rows_[index].key)) continue;
        rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(index));
        operations.push_back({OperationType::Erase, index, index, {}, {}});
    }

    for (size_t target = 0; target < next.size(); ++target) {
        if (target < rows_.size() && rows_[target].key == next[target].key) {
            auto cells = ChangedCells(rows_[target], next[target]);
            if (!cells.empty() || rows_[target].visualState != next[target].visualState) {
                operations.push_back({OperationType::Update, target, target, next[target],
                                      std::move(cells)});
            }
            rows_[target] = next[target];
            continue;
        }

        auto existing = std::find_if(
            rows_.begin() + static_cast<std::ptrdiff_t>(std::min(target, rows_.size())),
            rows_.end(), [&](const Row& row) { return row.key == next[target].key; });
        if (existing == rows_.end()) {
            rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(target), next[target]);
            operations.push_back({OperationType::Insert, target, target, next[target], {}});
            continue;
        }

        const size_t from = static_cast<size_t>(std::distance(rows_.begin(), existing));
        Row moved = std::move(*existing);
        rows_.erase(existing);
        rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(target), std::move(moved));
        operations.push_back({OperationType::Move, target, from, next[target], {}});
        rows_[target] = next[target];
    }

    while (rows_.size() > next.size()) {
        const size_t index = rows_.size() - 1;
        rows_.pop_back();
        operations.push_back({OperationType::Erase, index, index, {}, {}});
    }

    return operations;
}

std::optional<size_t> StableTableModel::IndexOf(const std::wstring& key) const {
    auto found = std::find_if(rows_.begin(), rows_.end(),
                              [&](const Row& row) { return row.key == key; });
    if (found == rows_.end()) return std::nullopt;
    return static_cast<size_t>(std::distance(rows_.begin(), found));
}

}  // namespace rm::ui
