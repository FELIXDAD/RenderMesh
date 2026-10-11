// One-time import of the job history written by RenderMesh 0.2.x, which kept
// each job as a JSON payload row in an SQLite database (jobs.db). This reader
// understands just enough of the SQLite 3 file format to walk that one table:
// rowid table b-trees, overflow chains and committed frames still in the WAL.

#include "Core.hpp"

#include <set>

namespace rm {
namespace {

uint32_t Be16(const unsigned char* data) { return (uint32_t{data[0]} << 8) | data[1]; }
uint32_t Be32(const unsigned char* data) {
    return (uint32_t{data[0]} << 24) | (uint32_t{data[1]} << 16) |
        (uint32_t{data[2]} << 8) | data[3];
}

bool ReadVarint(const unsigned char*& data, const unsigned char* end, uint64_t& value) {
    value = 0;
    for (int index = 0; index < 9; ++index) {
        if (data >= end) return false;
        const unsigned char byte = *data++;
        if (index == 8) {
            value = (value << 8) | byte;
            return true;
        }
        value = (value << 7) | (byte & 0x7f);
        if (!(byte & 0x80)) return true;
    }
    return true;
}

struct Database {
    std::string file;
    std::string wal;
    std::map<uint32_t, size_t> walPages;  // page number -> offset of the page in `wal`
    uint32_t pageSize = 0;
    uint32_t usable = 0;

    const unsigned char* Page(uint32_t number) const {
        if (number == 0) return nullptr;
        if (auto found = walPages.find(number); found != walPages.end())
            return reinterpret_cast<const unsigned char*>(wal.data() + found->second);
        const uint64_t offset = uint64_t{number - 1} * pageSize;
        if (offset + pageSize > file.size()) return nullptr;
        return reinterpret_cast<const unsigned char*>(file.data() + offset);
    }

    // Reads the payload of the table b-tree cell at `cell` in `page`.
    bool Payload(const unsigned char* page, uint32_t cell, std::string& payload) const {
        const unsigned char* data = page + cell;
        const unsigned char* end = page + pageSize;
        uint64_t size = 0, rowid = 0;
        if (cell >= pageSize || !ReadVarint(data, end, size) || !ReadVarint(data, end, rowid) ||
            size > (1ull << 31))
            return false;
        const uint64_t maxLocal = usable - 35;
        uint64_t local = size;
        if (size > maxLocal) {
            const uint64_t minLocal = (uint64_t{usable - 12} * 32 / 255) - 23;
            const uint64_t surplus = minLocal + (size - minLocal) % (usable - 4);
            local = surplus <= maxLocal ? surplus : minLocal;
        }
        if (data + local > end) return false;
        payload.assign(reinterpret_cast<const char*>(data), static_cast<size_t>(local));
        if (local == size) return true;
        if (data + local + 4 > end) return false;
        uint32_t overflow = Be32(data + local);
        std::set<uint32_t> visited;
        while (payload.size() < size) {
            const unsigned char* next = Page(overflow);
            if (!next || !visited.insert(overflow).second) return false;
            const size_t take = static_cast<size_t>(
                std::min<uint64_t>(usable - 4, size - payload.size()));
            payload.append(reinterpret_cast<const char*>(next + 4), take);
            overflow = Be32(next);
        }
        return true;
    }

    // Calls `row` with the payload of every row of the table rooted at `root`.
    bool Walk(uint32_t root, const std::function<void(const std::string&)>& row) const {
        std::vector<uint32_t> pending{root};
        std::set<uint32_t> visited;
        std::string payload;
        while (!pending.empty()) {
            const uint32_t number = pending.back();
            pending.pop_back();
            const unsigned char* page = Page(number);
            if (!page || !visited.insert(number).second || visited.size() > 1000000)
                return false;
            const uint32_t header = number == 1 ? 100 : 0;
            const unsigned char type = page[header];
            const uint32_t cells = Be16(page + header + 3);
            const uint32_t pointers = header + (type == 0x05 ? 12 : 8);
            if (type != 0x05 && type != 0x0d) return false;
            if (pointers + cells * 2 > pageSize) return false;
            if (type == 0x05) pending.push_back(Be32(page + header + 8));
            for (uint32_t index = 0; index < cells; ++index) {
                const uint32_t cell = Be16(page + pointers + index * 2);
                if (cell + 4 > pageSize) return false;
                if (type == 0x05) {
                    pending.push_back(Be32(page + cell));
                } else {
                    if (!Payload(page, cell, payload)) return false;
                    row(payload);
                }
            }
        }
        return true;
    }
};

// Splits an SQLite record into its column values (text and integers only).
std::vector<std::string> Columns(const std::string& record) {
    std::vector<std::string> columns;
    const auto* begin = reinterpret_cast<const unsigned char*>(record.data());
    const auto* end = begin + record.size();
    const unsigned char* cursor = begin;
    uint64_t headerSize = 0;
    if (!ReadVarint(cursor, end, headerSize) || headerSize > record.size()) return {};
    const unsigned char* headerEnd = begin + headerSize;
    const unsigned char* body = headerEnd;
    while (cursor < headerEnd) {
        uint64_t serial = 0;
        if (!ReadVarint(cursor, headerEnd, serial)) return {};
        uint64_t size = 0;
        if (serial >= 1 && serial <= 4) size = serial;
        else if (serial == 5) size = 6;
        else if (serial == 6 || serial == 7) size = 8;
        else if (serial >= 12) size = (serial - 12) / 2;
        if (body + size > end) return {};
        if (serial >= 1 && serial <= 6) {
            int64_t value = (body[0] & 0x80) ? -1 : 0;
            for (uint64_t index = 0; index < size; ++index) value = (value << 8) | body[index];
            columns.push_back(std::to_string(value));
        } else if (serial == 8 || serial == 9) {
            columns.push_back(serial == 8 ? "0" : "1");
        } else {
            columns.emplace_back(reinterpret_cast<const char*>(body), static_cast<size_t>(size));
        }
        body += size;
    }
    return columns;
}

void LoadWal(Database& database) {
    const auto& wal = database.wal;
    if (wal.size() < 32) return;
    const auto* data = reinterpret_cast<const unsigned char*>(wal.data());
    const uint32_t magic = Be32(data);
    if ((magic != 0x377f0682 && magic != 0x377f0683) || Be32(data + 8) != database.pageSize)
        return;
    const uint32_t salt1 = Be32(data + 16), salt2 = Be32(data + 20);
    std::map<uint32_t, size_t> uncommitted;
    for (size_t offset = 32; offset + 24 + database.pageSize <= wal.size();
         offset += 24 + database.pageSize) {
        const unsigned char* frame = data + offset;
        if (Be32(frame + 8) != salt1 || Be32(frame + 12) != salt2) break;
        uncommitted[Be32(frame)] = offset + 24;
        if (Be32(frame + 4) != 0) {
            for (const auto& [page, at] : uncommitted) database.walPages[page] = at;
            uncommitted.clear();
        }
    }
}

}  // namespace

bool ReadLegacyJobPayloads(const fs::path& path, std::vector<std::string>& payloads,
                           std::string& error) {
    payloads.clear();
    Database database;
    if (!ReadFileText(path, database.file) || database.file.size() < 100 ||
        database.file.compare(0, 16, std::string("SQLite format 3\0", 16)) != 0) {
        error = "not an SQLite 3 database";
        return false;
    }
    const auto* header = reinterpret_cast<const unsigned char*>(database.file.data());
    database.pageSize = Be16(header + 16) == 1 ? 65536 : Be16(header + 16);
    database.usable = database.pageSize - header[20];
    if (database.pageSize < 512 || database.usable < 480 || Be32(header + 56) > 1) {
        error = "unsupported SQLite page layout or text encoding";
        return false;
    }
    fs::path wal = path;
    wal += L"-wal";
    ReadFileText(wal, database.wal);
    LoadWal(database);

    uint32_t jobsRoot = 0;
    bool ok = database.Walk(1, [&](const std::string& record) {
        const auto columns = Columns(record);
        if (columns.size() >= 4 && columns[0] == "table" && columns[1] == "jobs")
            jobsRoot = static_cast<uint32_t>(std::strtoul(columns[3].c_str(), nullptr, 10));
    });
    if (!ok || !jobsRoot) {
        error = ok ? "jobs table not found" : "schema table is damaged";
        return false;
    }
    ok = database.Walk(jobsRoot, [&](const std::string& record) {
        auto columns = Columns(record);
        if (columns.size() >= 5) payloads.push_back(std::move(columns[4]));
    });
    if (!ok) error = "jobs table is damaged";
    return ok;
}

}  // namespace rm
