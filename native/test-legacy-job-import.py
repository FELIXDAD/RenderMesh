"""Verifies the one-time import of 0.2.x job history (jobs.db) into job records.

Usage: python test-legacy-job-import.py <RenderMesh.exe>

Builds SQLite databases with the 0.2.x schema (small rows, rows that need
overflow pages, enough rows for interior b-tree pages, and a database whose
latest commits are still in the WAL), starts RenderMesh headless on each and
compares every imported record with the original payload.
"""
import json
import os
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile

SCHEMA = ("CREATE TABLE IF NOT EXISTS jobs(id TEXT PRIMARY KEY NOT NULL, created_at INTEGER NOT NULL,"
          "finished_at INTEGER NOT NULL DEFAULT 0, priority INTEGER NOT NULL DEFAULT 0,"
          "payload TEXT NOT NULL);")


def payload(index, frames):
    return json.dumps({
        "id": f"legacy{index:05d}", "name": f"作业 {index}", "created_at": 1700000000000 + index,
        "format": "PNG", "extension": ".png",
        "frames": [{"task_id": f"legacy{index:05d}-{f}", "frame": f, "state": 4,
                    "error": "x" * (f % 7)} for f in range(1, frames + 1)],
    }, ensure_ascii=False, separators=(",", ":"))


def build(directory, rows, wal_tail=0):
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, "jobs.db")
    connection = sqlite3.connect(path)
    connection.execute("PRAGMA journal_mode=WAL;")
    connection.execute("PRAGMA wal_autocheckpoint=0;")
    connection.execute(SCHEMA)
    expected = {}
    for index, frames in enumerate(rows):
        text = payload(index, frames)
        connection.execute("INSERT INTO jobs VALUES(?,?,?,?,?)",
                           (f"legacy{index:05d}", 1700000000000 + index, 0, 0, text))
        expected[f"legacy{index:05d}"] = text
        if wal_tail and index == len(rows) - wal_tail - 1:
            connection.commit()
            connection.execute("PRAGMA wal_checkpoint(TRUNCATE);")
    connection.commit()
    if wal_tail:
        # Copy while the connection is open so the last commits stay in the WAL.
        snapshot = directory + "-snapshot"
        os.makedirs(snapshot)
        for suffix in ("", "-wal"):
            shutil.copyfile(path + suffix, os.path.join(snapshot, "jobs.db" + suffix))
        connection.close()
        shutil.rmtree(directory)
        os.rename(snapshot, directory)
    else:
        connection.close()
    return expected


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def verify(exe, directory, expected):
    subprocess.run([exe, "--headless", "--run-seconds", "1", "--data-dir", directory,
                    "--port", str(free_port()), "--no-update-check"], check=True, timeout=60)
    records = os.path.join(directory, "jobdb")
    found = {name[:-5]: open(os.path.join(records, name), encoding="utf-8").read()
             for name in os.listdir(records) if name.endswith(".json")}
    if set(found) != set(expected):
        raise SystemExit(f"{directory}: imported {len(found)} of {len(expected)} jobs")
    # After loading, RenderMesh re-saves jobs in its current record format
    # (for example it stamps finished_at), so compare the job content.
    def content(text):
        job = json.loads(text)
        return job["id"], job["name"], [(f["task_id"], f["frame"], f["state"], f["error"])
                                        for f in job["frames"]]
    for job_id, text in expected.items():
        if content(found[job_id]) != content(text):
            raise SystemExit(f"{directory}: job {job_id} changed during import")
    if not os.path.exists(os.path.join(directory, "jobs.db.imported")):
        raise SystemExit(f"{directory}: legacy database was not retired")


def main():
    exe = os.path.abspath(sys.argv[1])
    root = tempfile.mkdtemp(prefix="RenderMesh-legacy-import-")
    try:
        cases = {
            "small": [1, 2, 3],
            "overflow": [1, 400, 5000, 40000],
            "interior": [3 + (index % 50) for index in range(600)],
        }
        for name, rows in cases.items():
            verify(exe, os.path.join(root, name), build(os.path.join(root, name), rows))
        wal_dir = os.path.join(root, "wal")
        verify(exe, wal_dir, build(wal_dir, [2, 300, 4000, 7, 9000, 1], wal_tail=3))
        print("LEGACY_JOB_IMPORT_OK")
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
