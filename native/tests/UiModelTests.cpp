#include "UiModel.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using rm::ui::OperationType;
using rm::ui::Row;
using rm::ui::StableTableModel;
using rm::ui::WorkerActivityView;
using namespace std::chrono_literals;

Row MakeRow(std::wstring key, std::initializer_list<const wchar_t*> cells) {
    Row row;
    row.key = std::move(key);
    for (const wchar_t* cell : cells) row.cells.emplace_back(cell);
    return row;
}

[[noreturn]] void Fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void Expect(bool condition, const std::string& message) {
    if (!condition) Fail(message);
}

void InitialRowsAreInsertedOnce() {
    StableTableModel model;
    auto operations = model.Reconcile({
        MakeRow(L"job-a", {L"A", L"Queued"}),
        MakeRow(L"job-b", {L"B", L"Rendering"}),
    });

    Expect(operations.size() == 2, "initial population should insert two rows");
    Expect(operations[0].type == OperationType::Insert && operations[0].index == 0,
           "first row should be inserted at index zero");
    Expect(operations[1].type == OperationType::Insert && operations[1].index == 1,
           "second row should be inserted at index one");
    Expect(model.Rows().size() == 2, "model should retain the reconciled rows");
}

void IdenticalRowsProduceNoControlWork() {
    StableTableModel model;
    const std::vector<Row> rows = {
        MakeRow(L"job-a", {L"A", L"Queued"}),
        MakeRow(L"job-b", {L"B", L"Rendering"}),
    };
    model.Reconcile(rows);

    auto operations = model.Reconcile(rows);

    Expect(operations.empty(), "identical snapshots must not mutate the list control");
}

void OneChangedCellProducesOneCellUpdate() {
    StableTableModel model;
    model.Reconcile({MakeRow(L"job-a", {L"A", L"Queued", L"0 / 20"})});

    auto operations = model.Reconcile({
        MakeRow(L"job-a", {L"A", L"Rendering", L"0 / 20"}),
    });

    Expect(operations.size() == 1, "one changed cell should create one operation");
    Expect(operations[0].type == OperationType::Update,
           "a stable row should be updated, not replaced");
    Expect(operations[0].cells.size() == 1 && operations[0].cells[0].column == 1,
           "only the changed status cell should be written");
}

void HeadInsertionPreservesExistingRows() {
    StableTableModel model;
    model.Reconcile({
        MakeRow(L"event-b", {L"B"}),
        MakeRow(L"event-a", {L"A"}),
    });

    auto operations = model.Reconcile({
        MakeRow(L"event-c", {L"C"}),
        MakeRow(L"event-b", {L"B"}),
        MakeRow(L"event-a", {L"A"}),
    });

    Expect(operations.size() == 1, "a head insertion should not rewrite existing rows");
    Expect(operations[0].type == OperationType::Insert && operations[0].index == 0,
           "the new entity should be inserted at the head");
    Expect(model.IndexOf(L"event-b").value_or(99) == 1,
           "existing row identity should survive the insertion");
}

void ReorderMovesOnlyTheStableEntity() {
    StableTableModel model;
    model.Reconcile({
        MakeRow(L"node-a", {L"A"}),
        MakeRow(L"node-b", {L"B"}),
        MakeRow(L"node-c", {L"C"}),
    });

    auto operations = model.Reconcile({
        MakeRow(L"node-a", {L"A"}),
        MakeRow(L"node-c", {L"C"}),
        MakeRow(L"node-b", {L"B"}),
    });

    Expect(operations.size() == 1, "a reorder should produce one move");
    Expect(operations[0].type == OperationType::Move && operations[0].fromIndex == 2 &&
               operations[0].index == 1,
           "the stable entity should move from its old index to the new index");
}

void RemovalErasesOnlyTheMissingEntity() {
    StableTableModel model;
    model.Reconcile({
        MakeRow(L"node-a", {L"A"}),
        MakeRow(L"node-b", {L"B"}),
        MakeRow(L"node-c", {L"C"}),
    });

    auto operations = model.Reconcile({
        MakeRow(L"node-a", {L"A"}),
        MakeRow(L"node-c", {L"C"}),
    });

    Expect(operations.size() == 1, "one missing entity should produce one erase");
    Expect(operations[0].type == OperationType::Erase && operations[0].index == 1,
           "only the missing stable entity should be erased");
}

void RefreshBurstIsCoalescedToOneDisplayFrame() {
    Expect(rm::ui::RefreshDelayFor(0ms) == 16ms,
           "the first burst inside a frame should wait for the frame boundary");
    Expect(rm::ui::RefreshDelayFor(15ms) == 1ms,
           "a burst just before the boundary should wait only the remaining time");
    Expect(rm::ui::RefreshDelayFor(16ms) == 0ms,
           "a burst at the boundary should refresh immediately");
    Expect(rm::ui::RefreshDelayFor(200ms) == 0ms,
           "an old snapshot should refresh immediately");
}

void ActiveWorkerShowsJobFramesAndProgress() {
    WorkerActivityView activity;
    activity.jobName = L"测试作业";
    activity.frames = {1, 2, 3, 4, 5};
    activity.completedFrames = 2;
    activity.rendering = true;

    Expect(rm::ui::FormatWorkerActivity(activity) ==
               L"测试作业 · 帧 1–5 · 2/5",
           "an active worker should expose the job, frame range, and package progress");
}

void EmptyWorkerActivityUsesAnEmDash() {
    Expect(rm::ui::FormatWorkerActivity({}) == L"—",
           "an idle worker should not show an assigned count");
}

}  // namespace

int main() {
    InitialRowsAreInsertedOnce();
    IdenticalRowsProduceNoControlWork();
    OneChangedCellProducesOneCellUpdate();
    HeadInsertionPreservesExistingRows();
    ReorderMovesOnlyTheStableEntity();
    RemovalErasesOnlyTheMissingEntity();
    RefreshBurstIsCoalescedToOneDisplayFrame();
    ActiveWorkerShowsJobFramesAndProgress();
    EmptyWorkerActivityUsesAnEmDash();
    std::cout << "UI_MODEL_TESTS_OK\n";
    return 0;
}
