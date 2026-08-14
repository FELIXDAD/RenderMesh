#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace rm::ui {

enum class VisualState { Neutral, Success, Active, Warning, Error, Muted };

constexpr std::chrono::milliseconds MinimumRefreshInterval() {
    return std::chrono::milliseconds(16);
}

inline std::chrono::milliseconds RefreshDelayFor(
    std::chrono::steady_clock::duration elapsed) {
    const auto minimum = MinimumRefreshInterval();
    if (elapsed >= minimum) return std::chrono::milliseconds::zero();
    return std::chrono::duration_cast<std::chrono::milliseconds>(minimum - elapsed);
}

struct Row {
    std::wstring key;
    std::vector<std::wstring> cells;
    VisualState visualState = VisualState::Neutral;

    bool operator==(const Row&) const = default;
};

struct CellChange {
    size_t column = 0;
    std::wstring value;
};

struct WorkerActivityView {
    std::wstring jobName;
    std::vector<int> frames;
    size_t completedFrames = 0;
    bool rendering = false;
};

struct JobHistoryView {
    std::wstring id;
    std::wstring name;
    int priority = 0;
    std::chrono::system_clock::time_point createdAt{};
    std::chrono::system_clock::time_point finishedAt{};
};

bool IsExpiredJob(const JobHistoryView& job,
                  std::chrono::system_clock::time_point now);
std::wstring DisplayJobName(const JobHistoryView& job,
                            std::chrono::system_clock::time_point now);
void SortJobsForDisplay(std::vector<JobHistoryView>& jobs);
bool DispatchesBefore(const JobHistoryView& left, const JobHistoryView& right);

std::wstring FormatWorkerActivity(const WorkerActivityView& activity);

enum class OperationType { Insert, Erase, Move, Update };

struct Operation {
    OperationType type = OperationType::Update;
    size_t index = 0;
    size_t fromIndex = 0;
    Row row;
    std::vector<CellChange> cells;
};

class StableTableModel {
public:
    std::vector<Operation> Reconcile(std::vector<Row> next);
    const std::vector<Row>& Rows() const { return rows_; }
    std::optional<size_t> IndexOf(const std::wstring& key) const;

private:
    std::vector<Row> rows_;
};

}  // namespace rm::ui
