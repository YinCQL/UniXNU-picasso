#!/usr/bin/env python3
"""Run host regressions against helpers extracted from the canonical USB patch.

Uses MSVC on Windows or clang++/g++ elsewhere, without installing tools. Generated
sources, compiler output, executables and a JSON report remain in --output.
The fixture simulates MMIO, locks, clocks, console callbacks and scheduling; it
does not compile the complete kernel or validate hardware timing/reset behavior.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HOST_TEST = r"""#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define __unused [[maybe_unused]]
#define _Atomic /* Serial fixture; concurrent atomics are tested by the backend fixture. */
#define NSEC_PER_MSEC 1000000ULL
enum { acquire, release };
using wait_result_t = int;
using boolean_t = bool;
static bool irq_enabled;
static boolean_t ml_set_interrupts_enabled(bool enabled) {
    bool previous = irq_enabled;
    irq_enabled = enabled;
    return previous;
}

struct {
    unsigned poll_lock;
    bool panic_mode, reboot_pending, hang_pending, panic_pending, reset_pending;
    bool watchdog_armed, hold, trace_polling, rx_busy;
    uintptr_t wdt;
    uint64_t events;
} ud;
static bool pur_live;
enum Action { NoAction, EnterPanic, RequestReboot, RequestHang, RequestPanic, RequestReset };
static Action action;
static unsigned poll_calls, pet_calls, reboot_calls, hang_calls, panic_calls;
static unsigned wait_calls, yield_calls, stop_after;
static uint64_t fake_now_ns, yield_step;
static volatile uint64_t armcpu_timer_interrupts;
static bool in_poll, replay_console, in_printf, panic_on_yield;
static std::vector<std::string> logs, sequence;
struct Stop {};

static void picasso_usb_console_drive_poll(void);
static bool os_atomic_cmpxchg(unsigned *p, unsigned from, unsigned to, int) {
    if (*p != from) return false;
    *p = to;
    return true;
}
static void os_atomic_store(unsigned *p, unsigned value, int) { *p = value; }
static unsigned os_atomic_load(const unsigned *p, int) { return *p; }
static void ud_poll(void) {
    assert(ud.poll_lock == 1 && !in_poll);
    in_poll = true;
    ++poll_calls;
    sequence.emplace_back("poll");
    switch (action) {
    case EnterPanic: ud.panic_mode = true; break;
    case RequestReboot: ud.reboot_pending = true; break;
    case RequestHang: ud.hang_pending = true; break;
    case RequestPanic: ud.panic_pending = true; break;
    case RequestReset: ud.reset_pending = true; break;
    case NoAction: break;
    }
    in_poll = false;
}
static void usb_wr(uintptr_t base, unsigned off, unsigned value) {
    assert(!in_poll && ud.poll_lock == 0 && !ud.panic_mode && !irq_enabled);
    assert(base == ud.wdt && off == 4u && value == 1);
    ++pet_calls;
    sequence.emplace_back("pet");
}
static unsigned usb_rd(uintptr_t base, unsigned off) {
    assert(base == ud.wdt && !irq_enabled);
    if (off == 8u) return 1u;
    assert(off == 0x10u || off == 0x14u);
    return 491519u;
}
static uint64_t mach_absolute_time(void) {
    sequence.emplace_back("clock");
    return fake_now_ns;
}
static void absolutetime_to_nanoseconds(uint64_t value, uint64_t *ns) { *ns = value; }
static void ud_finish_reboot(void) { assert(ud.poll_lock == 0); ++reboot_calls; }
static void ud_finish_hang(void) { assert(ud.poll_lock == 0); ++hang_calls; }
static void panic(const char *) { assert(ud.poll_lock == 0); ++panic_calls; ud.panic_mode = true; }
static void picasso_usb_busy_wait(unsigned value) { assert(value == 100); ++wait_calls; }
static void thread_yield_internal(int value) {
    assert(value == 1 && !in_poll && ud.poll_lock == 0);
    ++yield_calls;
    sequence.emplace_back("yield");
    fake_now_ns += yield_step;
    armcpu_timer_interrupts = armcpu_timer_interrupts + 1;
    if (panic_on_yield) ud.panic_mode = true;
    if (yield_calls >= stop_after) throw Stop{};
}
static int test_printf(const char *format, ...) {
    assert(!in_poll); // Caller never owns the poll lock while logging.
    char text[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    assert(length >= 0 && static_cast<size_t>(length) < sizeof(text));
    logs.emplace_back(text);
    sequence.emplace_back("log");
    if (replay_console && !in_printf) {
        in_printf = true;
        picasso_usb_console_drive_poll();
        in_printf = false;
    }
    return length;
}
#define printf test_printf
#include "production.h"
#undef printf

static void reset(void) {
    ud = {};
    picasso_wdt_lock = picasso_wdt_terminal = 0;
    picasso_wdt_owner = nullptr;
    irq_enabled = true;
    ud.watchdog_armed = true;
    ud.wdt = 0x1000;
    pur_live = true;
    action = NoAction;
    poll_calls = pet_calls = reboot_calls = hang_calls = panic_calls = 0;
    wait_calls = yield_calls = 0;
    stop_after = 1;
    fake_now_ns = 5000000000ULL;
    yield_step = 1000000;
    armcpu_timer_interrupts = 0;
    in_poll = replay_console = in_printf = panic_on_yield = false;
    logs.clear();
    sequence.clear();
}
static unsigned containing(const char *part) {
    unsigned count = 0;
    for (const auto &line : logs) if (line.find(part) != std::string::npos) ++count;
    return count;
}
static void run_thread(unsigned iterations) {
    stop_after = iterations;
    try { ud_thread(nullptr, 0); assert(false); } catch (const Stop &) {}
}
static void passed(const char *name) { std::printf("PASS: %s\n", name); }

int main(void) {
    reset();
    ud_poller_progress progress = {};
    ud_background_poll(&progress);
    assert(poll_calls == 1 && pet_calls == 1 && progress.have_pet);
    assert(logs.size() == 3 && containing("first poll begin") == 1 && containing("first poll end") == 1);
    assert(containing("gap_valid=0") == 1 && ud.poll_lock == 0);
    const std::vector<std::string> expected = {"clock", "log", "poll", "pet", "clock", "log", "log"};
    assert(sequence == expected);
    passed("successful poll pets before timing/logging and releases its lock");

    reset();
    run_thread(7);
    assert(pet_calls == 7 && poll_calls == 7 && wait_calls == 7 && yield_calls == 7);
    assert(containing("watchdog pet") == UD_WATCHDOG_INITIAL_REPORTS);
    assert(containing("heartbeat") == 0);
    assert(containing("first yield begin depress_ms=1 mono_ns=5000000000 timer_irqs=0 timer_irq_valid=1") == 1);
    assert(containing("first yield end mono_ns=5001000000 timer_irqs=1 timer_irq_valid=1") == 1);
    assert(logs.size() == 9); // Startup + first-poll pair + four pets + first-yield pair.
    unsigned pending_pet = 0;
    for (const auto &entry : sequence) {
        if (entry == "pet") ++pending_pet;
        if (entry == "yield") { assert(pending_pet == 1); pending_pet = 0; }
    }
    passed("production thread pets before each finite-depression yield and reports only the first yield pair");

    reset();
    run_thread(1); // The mock never returns from its first yield.
    assert(pet_calls == 1 && containing("first yield begin") == 1 && containing("first yield end") == 0);
    passed("an unreturned yield leaves an unmatched begin marker without claiming scheduling progress");

    reset();
    panic_on_yield = true;
    run_thread(7);
    assert(pet_calls == 1 && containing("first yield begin") == 1 && containing("first yield end") == 0);
    passed("panic observed after yielding suppresses the end diagnostic and subsequent normal pets");

    reset();
    progress = {};
    assert(ud_timer_irq_snapshot(nullptr) == 0);
    armcpu_timer_interrupts = 4294967298ULL;
    ud_background_poll(&progress);
    armcpu_timer_interrupts = 0;
    ud_background_poll(&progress);
    assert(pet_calls == 2 && containing("timer_irqs=4294967298 timer_irq_valid=1") == 1);
    assert(containing("timer_irqs=0 timer_irq_valid=1") == 1);
    passed("optional timer snapshot handles null and preserves 64-bit counts without gating pets");

    reset();
    progress = {};
    ud.poll_lock = 1; // An independent owner, not this helper.
    replay_console = true;
    for (unsigned i = 0; i < 12; ++i) ud_background_poll(&progress);
    assert(poll_calls == 0 && pet_calls == 0 && ud.poll_lock == 1 && !progress.have_pet);
    assert(containing("first poll end normal=0 petted=0") == 1 && logs.size() == 2);
    ud.poll_lock = 0;
    ud_background_poll(&progress);
    assert(pet_calls == 1 && progress.have_pet && containing("gap_valid=0") == 1);
    passed("busy try-lock neither polls nor pets; recovery immediately services the watchdog");

    reset();
    progress = {};
    ud.panic_mode = true;
    for (unsigned i = 0; i < 5; ++i) ud_background_poll(&progress);
    assert(pet_calls == 0 && logs.empty() && !progress.have_pet && ud.poll_lock == 0);
    passed("existing panic mode does not pet or emit ordinary progress logs");

    reset();
    progress = {};
    action = EnterPanic;
    ud_background_poll(&progress);
    assert(pet_calls == 0 && logs.size() == 1 && !progress.have_pet && ud.poll_lock == 0);
    passed("panic reached during polling suppresses the pet and post-poll diagnostics");

    for (Action request : {RequestReboot, RequestHang, RequestPanic, RequestReset}) {
        reset();
        progress = {};
        action = request;
        ud_background_poll(&progress);
        assert(pet_calls == 0 && !progress.have_pet && ud.poll_lock == 0);
        assert(reboot_calls + hang_calls + panic_calls == 1);
    }
    passed("pending reboot/hang/panic/reset runs unlocked and is not normal watchdog progress");

    for (bool mapped : {false, true}) {
        reset();
        progress = {};
        if (mapped) ud.watchdog_armed = false; else ud.wdt = 0;
        for (unsigned i = 0; i < 10; ++i) ud_background_poll(&progress);
        assert(pet_calls == 0 && !progress.have_pet && containing("watchdog pet") == 0);
    }
    passed("disarmed or unmapped watchdog never reports a fictitious pet");

    reset();
    progress = {};
    replay_console = true;
    for (unsigned i = 0; i < 30; ++i) ud_background_poll(&progress);
    assert(pet_calls == 30 && poll_calls > 30 && logs.size() == 6);
    assert(containing("first poll begin") == 1 && containing("first poll end") == 1);
    passed("printf-triggered console reentry can poll but cannot recursively pet or amplify diagnostics");

    reset();
    for (unsigned i = 0; i < 30; ++i) picasso_usb_console_drive_poll();
    assert(poll_calls == 30 && pet_calls == 0 && logs.empty());
    passed("ordinary console-driven polls retain their existing no-pet behavior");

    reset();
    progress = {};
    fake_now_ns = 0;
    for (unsigned i = 0; i < 5000; ++i) ud_background_poll(&progress);
    assert(pet_calls == 5000 && logs.size() == 6 && progress.have_pet && progress.last_pet_ns == 0);
    assert(containing("gap_valid=0") == 1 && containing("gap_valid=1") == 3);
    passed("frozen clock at zero does not gate service or bypass the initial report cap");

    reset();
    progress = {};
    for (unsigned i = 0; i < 4; ++i) { fake_now_ns += 1; ud_background_poll(&progress); }
    fake_now_ns = 1;
    ud_background_poll(&progress);
    assert(pet_calls == 5 && progress.late_reports == 0 && logs.size() == 6);
    fake_now_ns = 2;
    ud_background_poll(&progress);
    assert(pet_calls == 6 && progress.late_reports == 0);
    passed("backward timing sample does not underflow into a spurious long interval");

    reset();
    progress = {};
    for (unsigned i = 0; i < 4; ++i) ud_background_poll(&progress);
    fake_now_ns += UD_WATCHDOG_LATE_NS - 1;
    ud_background_poll(&progress);
    assert(logs.size() == 6);
    fake_now_ns += UD_WATCHDOG_LATE_NS;
    ud_background_poll(&progress);
    assert(logs.size() == 7 && progress.late_reports == 1);
    for (unsigned i = 0; i < 100; ++i) { fake_now_ns += UD_WATCHDOG_LATE_NS; ud_background_poll(&progress); }
    assert(progress.initial_reports == 4 && progress.late_reports == 8);
    assert(logs.size() == 14 && containing("watchdog pet") == 12 && pet_calls == 106);
    passed("one-second threshold and 14-line poll-helper diagnostic cap preserve continued pets");

    reset();
    yield_step = UD_WATCHDOG_LATE_NS;
    run_thread(100);
    assert(pet_calls == 100 && containing("watchdog pet") == 12);
    assert(containing("first yield begin") == 1 && containing("first yield end") == 1);
    assert(logs.size() == 17); // Existing startup + at most 16 new diagnostics.
    passed("full production thread caps poll/yield diagnostics while continuing long-gap pets");

    reset();
    progress = {};
    ud_background_poll(&progress);
    ud.poll_lock = 1;
    fake_now_ns += 12000000000ULL;
    for (unsigned i = 0; i < 10; ++i) ud_background_poll(&progress);
    assert(pet_calls == 1 && progress.last_pet_ns == 5000000000ULL);
    ud.poll_lock = 0;
    ud_background_poll(&progress);
    assert(pet_calls == 2 && containing("gap_ns=12000000000 gap_valid=1 late=1") == 1);
    passed("a lock-busy interval is retained in the next actual pet gap");

    std::puts("PASS: 17 production-helper regression groups; host simulation, not hardware timing validation");
}
"""


def extract_function(source, name):
    match = re.search(r'^static (?:void|bool|uint64_t)\n' + re.escape(name) + r'\(', source, re.M)
    if not match:
        raise ValueError(f'Production function not found: {name}')
    begin = match.start()
    cursor = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[begin:cursor] + '\n'


def extract_production(patch, out):
    text = patch.read_text(encoding='utf-8')
    file_start = text.index('+++ b/bsd/dev/picasso_usb.c\n')
    begin = text.index('@@ -0,0 +1,', file_start)
    end = text.index('\ndiff --git ', begin)
    lines = text[begin:end].splitlines()
    if not all(line.startswith('+') for line in lines[1:]):
        raise ValueError('Expected the complete new-file hunk for picasso_usb.c')
    count = int(re.search(r'\+1,(\d+)', lines[0])[1])
    if count != len(lines) - 1:
        raise ValueError(f'Incorrect new-file hunk count: {count} != {len(lines) - 1}')
    source = '\n'.join(line[1:] for line in lines[1:]) + '\n'
    (out / 'picasso_usb.c').write_text(source, encoding='utf-8', newline='\n')
    backend_names = ['picasso_watchdog_enter', 'picasso_watchdog_leave',
                     'picasso_watchdog_live_locked', 'picasso_watchdog_config_locked',
                     'picasso_watchdog_pet_locked']
    names = ['picasso_usb_watchdog_pet', 'ud_poll_serialized', 'picasso_usb_console_drive_poll',
             'ud_monotonic_ns', 'ud_timer_irq_snapshot', 'ud_background_poll', 'ud_thread']
    definitions = source[source.index('#define UD_WATCHDOG_INITIAL_REPORTS'):
                         source.index('static uint64_t\nud_monotonic_ns')]
    watchdog_constants = '\n'.join(re.findall(r'^#define (?:APSS_WDT_|PICASSO_WDT_TICKS)[^\n]+', source, re.M)) + '\n'
    backend_state = '\n'.join(re.search(r'^static [^\n]*\b' + name + r';$', source, re.M)[0]
                              for name in ('picasso_wdt_lock', 'picasso_wdt_terminal', 'picasso_wdt_owner')) + '\n'
    parts = ([watchdog_constants, backend_state] + [extract_function(source, name) for name in backend_names] +
             [extract_function(source, name) for name in names[:3]] + [definitions] +
             [extract_function(source, name) for name in names[3:]])
    (out / 'production.h').write_text('\n'.join(parts), encoding='utf-8', newline='\n')
    if '#define APSS_WDT_TIMEOUT_MS 15000u' not in source:
        raise ValueError('The 15-second watchdog timeout changed')
    if 'extern volatile uint64_t armcpu_timer_interrupts __attribute__((weak_import));' not in source:
        raise ValueError('The diagnostic ArmCpu counter must remain an optional weak import')
    return {'patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
            'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
            'extracted_functions': backend_names + names, 'new_file_hunk_lines': count}


def find_vcvars():
    program_files = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))
    vswhere = program_files / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if vswhere.is_file():
        result = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                                 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                 '-property', 'installationPath'],
                                capture_output=True, text=True, check=True)
        for line in result.stdout.splitlines():
            candidate = Path(line) / 'VC/Auxiliary/Build/vcvars64.bat'
            if candidate.is_file():
                return candidate
    candidate = program_files / 'Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat'
    return candidate if candidate.is_file() else None


def run_case(out, name, asan, cxx, vcvars):
    compile_log = out / f'{name}-compile.log'
    run_log = out / f'{name}-run.log'
    exe = out / (name + ('.exe' if os.name == 'nt' else ''))
    if vcvars:
        flags = '/nologo /std:c++17 /EHsc /W4 /WX /Od'
        if asan:
            flags += f' /fsanitize=address /Zi /Fd:{name}-compile.pdb'
        command = f'cl {flags} /Fo:{name}.obj /Fe:{exe.name} WatchdogService.cpp'
        if asan:
            command += f' /link /INCREMENTAL:NO /PDB:{name}.pdb'
        # All generated file arguments are fixed basenames. CALL quotes the
        # discovered VS path; %~dp0 selects this script's output directory.
        batch = out / f'{name}-compile.cmd'
        batch.write_text(
            '@echo off\nsetlocal\n'
            f'call "{vcvars}" > "%~dp0{name}-toolchain.log" 2>&1\n'
            'if errorlevel 1 exit /b 1\ncd /d "%~dp0"\n'
            f'{command} > {compile_log.name} 2>&1\nexit /b %errorlevel%\n',
            encoding='utf-8', newline='\r\n')
        compiled = subprocess.run(['cmd', '/d', '/c', batch.name], cwd=out,
                                  capture_output=True, text=True)
    else:
        command = [cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O0', '-g']
        if asan:
            command += ['-fsanitize=address', '-fno-omit-frame-pointer']
        command += ['WatchdogService.cpp', '-o', str(exe)]
        with compile_log.open('w', encoding='utf-8') as log:
            compiled = subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT)
    result = {'name': name, 'asan': asan, 'compile_command': command,
              'compile_exit': compiled.returncode, 'compile_log': str(compile_log)}
    if compiled.returncode:
        print(compile_log.read_text(encoding='utf-8', errors='replace') if compile_log.exists()
              else f'Compiler environment setup failed; inspect {name}-toolchain.log', file=sys.stderr)
        return result
    if vcvars:
        # The ASan runtime DLL is on the toolchain PATH established by vcvars.
        # A child compile process cannot change this Python process's PATH.
        batch = out / f'{name}-run.cmd'
        batch.write_text(
            '@echo off\nsetlocal\n'
            f'call "{vcvars}" > "%~dp0{name}-run-toolchain.log" 2>&1\n'
            'if errorlevel 1 exit /b 1\ncd /d "%~dp0"\n'
            f'{exe.name} > {run_log.name} 2>&1\nexit /b %errorlevel%\n',
            encoding='utf-8', newline='\r\n')
        ran = subprocess.run(['cmd', '/d', '/c', batch.name], cwd=out,
                             capture_output=True, text=True)
    else:
        with run_log.open('w', encoding='utf-8') as log:
            ran = subprocess.run([str(exe)], cwd=out, stdout=log, stderr=subprocess.STDOUT)
    if not run_log.exists():
        result.update(run_exit=ran.returncode, run_log=str(run_log))
        print(f'Run environment setup failed; inspect {name}-run-toolchain.log', file=sys.stderr)
        return result
    output = run_log.read_text(encoding='utf-8', errors='replace')
    result.update(run_exit=ran.returncode, run_log=str(run_log))
    print(output, end='')
    return result


def main():
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=repo / 'out/usb-watchdog-service')
    parser.add_argument('--cxx', help='clang++/g++ executable; Windows defaults to installed MSVC')
    parser.add_argument('--no-asan', action='store_true', help='Run only the ordinary host fixture')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = extract_production(repo / 'patches/picasso-usb-console-xnu.patch', out)
    (out / 'WatchdogService.cpp').write_text(HOST_TEST, encoding='utf-8', newline='\n')
    vcvars = find_vcvars() if os.name == 'nt' and not args.cxx else None
    cxx = args.cxx or shutil.which('clang++') or shutil.which('g++')
    if not vcvars and not cxx:
        parser.error('No MSVC vcvars64.bat, clang++ or g++ found; no tools were installed')
    report['scope'] = ('Production helpers/thread extracted without edits; simulated MMIO, locks, '
                       'clock, console and scheduling. No kernel build or hardware timing validation.')
    report['host_test_sha256'] = hashlib.sha256(HOST_TEST.encode()).hexdigest()
    report['runs'] = []
    for name, asan in [('WatchdogService', False)] + ([] if args.no_asan else [('WatchdogService-asan', True)]):
        run = run_case(out, name, asan, cxx, vcvars)
        report['runs'].append(run)
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        if run.get('compile_exit') != 0 or run.get('run_exit') != 0:
            return 1
    print(f'PASS: report saved to {out / "results.json"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
