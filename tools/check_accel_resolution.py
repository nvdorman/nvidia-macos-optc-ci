#!/usr/bin/env python3
"""Exercise the production primary-table contract with explicit caller locking."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
fixture = r'''
#include <cassert>
#include <cstdio>
#include <mutex>
#include <thread>
#include "nvrm_devtab.h"
struct Device { unsigned references=1; bool stopped=false; };
static NvrmDevTab<Device> table{};
static std::mutex lock;
static Device *acquire() {
    std::lock_guard<std::mutex> guard(lock);
    Device *d=table.primary();
    if (d) { assert(!d->stopped); ++d->references; }
    return d;
}
int main() {
    Device first,second;
    { std::lock_guard<std::mutex> guard(lock); table.add(&first,false,false); }
    Device *held=acquire(); assert(held==&first && first.references==2);
    { std::lock_guard<std::mutex> guard(lock); table.remove(&first,false); first.stopped=true; }
    assert(!acquire() && first.references==2); // acquired references survive removal
    { std::lock_guard<std::mutex> guard(lock); --held->references; table.add(&second,false,false); }
    assert(table.primary()==&second);
    { std::lock_guard<std::mutex> guard(lock); table.remove(&first,false); }
    assert(table.primary()==&second); // an old owner cannot remove a new epoch
    std::thread reader([] { for (unsigned i=0;i<10000;++i) {
        Device *d=acquire();
        if (d) { std::lock_guard<std::mutex> guard(lock); assert(d==table.primary() || d->stopped); --d->references; }
    }});
    { std::lock_guard<std::mutex> guard(lock); table.remove(&second,false); second.stopped=true; }
    reader.join(); assert(!acquire() && second.references==1);
    assert(first.references==1);
    std::puts("PASS primary acquisition, exact removal, retained in-flight readers");
}
'''
with tempfile.TemporaryDirectory(prefix="optc-resolution-") as directory:
    path = Path(directory)
    source = path / "resolution.cpp"
    source.write_text(fixture)
    executable = path / "resolution"
    subprocess.run(["clang++", "-std=c++17", "-pthread", "-fsanitize=address,undefined",
                    "-I" + str(root / "kexts/NVRM"), str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
