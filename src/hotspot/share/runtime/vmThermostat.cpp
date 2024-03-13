/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "logging/log.hpp"
#include "runtime/handshake.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/os.hpp"
#include "runtime/threadSMR.inline.hpp"
#include "runtime/vmThermostat.hpp"
#include "utilities/ticks.hpp"

static constexpr uint64_t sampling_interval_nanos = 100000; // 1000 us
static constexpr int      samples_per_window      = 1000;   // 100  ms

//                          01234567890123456789
static const char* NAMES = "@?n.nvb0123Css#S###P####";

VMThermostat* VMThermostat::_thermostat;

void VMThermostat::initialize() {
  if (!log_is_enabled(Info, thermostat) &&
      !log_is_enabled(Info, profile)) {
    return;
  }

  HandleMark hm(JavaThread::current());
  EXCEPTION_MARK;

  const char* name = "VM Thermostat Thread";
  Handle thread_oop = JavaThread::create_system_thread_object(name, CHECK);

  _thermostat = new VMThermostat();
  JavaThread::vm_exit_on_osthread_failure(_thermostat);
  JavaThread::start_internal_daemon(THREAD, _thermostat, thread_oop, NearMaxPriority);
}

VMThermostat::VMThermostat()
  : JavaThread(thread_entry),
    _start_nanos(0),
    _nticks(0) {
}

void VMThermostat::thread_entry(JavaThread* thread, TRAPS) {
  static_cast<VMThermostat*>(thread)->run_loop();
}

// Returns how many sampling windows were missed due to latency problems
int VMThermostat::wait_for_tick() {
  if (_nticks++ == 0) {
    // First tick, set start time and record startup pause for premain
    const Ticks now = Ticks::now();
    _start_nanos = now.nanoseconds();
    return _start_nanos / sampling_interval_nanos;
  }

  for (;;) {
    // We might wake up spuriously from wait, so always recalculate
    // the timeout after a wakeup to see if we need to wait again.
    const Ticks now = Ticks::now();
    const uint64_t now_nanos = now.nanoseconds();
    const uint64_t next_nanos = _start_nanos + (sampling_interval_nanos * _nticks);
    const int64_t timeout_nanos = next_nanos - now_nanos;

    if (timeout_nanos > 0) {
      // Wait
      ThreadBlockInVM tbivm(_thermostat);
      if (timeout_nanos > NANOUNITS_PER_MILLIUNIT) {
        // More than a millisecond to sleep
        os::naked_short_sleep(timeout_nanos / NANOUNITS_PER_MILLIUNIT);
      } else {
        // Less than a millisecond to sleep
        os::naked_short_nanosleep(timeout_nanos);
      }
    } else {
      // Tick
      int overslept_ticks = 0;
      if (timeout_nanos < 0) {
        const uint64_t overslept = -timeout_nanos;
        if (overslept > sampling_interval_nanos) {
          // Missed one or more ticks. Bump _nticks accordingly to
          // avoid firing a string of immediate ticks to make up
          // for the ones we missed.
          overslept_ticks = overslept / sampling_interval_nanos;
          _nticks += overslept_ticks;
        }
      }

      return overslept_ticks;
    }
  }
}

static void trace_frame(outputStream* st, frame f, JavaThread* jt) {
  CodeBlob* cb = f.cb();
  if (cb != nullptr) {
    cb->print_on(st);
  } else {
    st->print_cr("cb == nullptr");
  }
  f.print_value_on(st, jt);
}
static void trace_sample(outputStream* st, JavaThread* jt, frame f) {
  jt->print_thread_state_on(st);
  trace_frame(st, jt->last_frame(), jt);
  trace_frame(st, f, jt);
}

void VMThermostat::ThermostatHandshake::do_thread(Thread* thread) {
  JavaThread* jt = JavaThread::cast(thread);
  if (!jt->has_last_Java_frame()) {
    // No java frame, no action
    _sample = NO_JAVA;
    return;
  }

    JavaThreadState state = jt->thread_state();
    if (state == _thread_in_native || state == _thread_in_native_trans) {
      _sample = IN_NATIVE; // in runtime?
      return;

//    } else if (state == _thread_in_vm || state == _thread_in_vm_trans) {
//      if (UseNewCode2) {
//        _sample = IN_RUNTIME;
//        return;
//      }
    } else if (state == _thread_blocked || state == _thread_blocked_trans) {
      _sample = BLOCKED;
      return;
    }

  // Sample top frame to see if the program is doing something
  // we know is slower than it could be
  frame f = jt->last_frame();

  // Skip any stub frames etc
  RegisterMap map(jt,
                  RegisterMap::UpdateMap::skip,
                  RegisterMap::ProcessFrames::skip,
                  RegisterMap::WalkContinuation::skip);
  if (f.is_safepoint_blob_frame()) {
    f = f.sender(&map);
  }
  if (UseNewCode) {
    if (f.is_entry_frame()               || // native->java entry frame; skip?
        f.is_runtime_frame()             || // SharedRuntime/Runtime1/OptoRuntime? skip?
        f.is_exception_blob_frame()      || // C2-specific? skip?
        f.is_deoptimization_blob_frame() || // C1-specific? skip?
        f.is_uncommon_trap_blob_frame()) {  // C2-specific? skip?
      f = f.sender(&map);
    }
  }

  if (UseNewCode && f.is_empty()) {
    _sample = NO_JAVA;
  } else if (f.is_interpreted_frame()) {
    _sample = INTERPRETED;
  } else if (f.is_compiled_frame()) {
    nmethod* nm = f.cb()->as_nmethod();
    int type = COMPILED_BASE;
    if (nm->is_scc()) {
      type = SHARED_BASE;
    }
    if (nm->preloaded()) {
      type = PRELOADED_BASE;
    }
    _sample = (Mode)(type + nm->comp_level() - 1);
  } else if (f.is_native_frame()) {
    _sample = IN_NATIVE;
  } else if (f.is_entry_frame()               || // native->java entry frame; skip?
             f.is_runtime_frame()             || // SharedRuntime/Runtime1/OptoRuntime? skip?
             f.is_exception_blob_frame()      || // C2-specific? skip?
             f.is_deoptimization_blob_frame() || // C1-specific? skip?
             f.is_uncommon_trap_blob_frame()) {  // C2-specific? skip?
    // TODO: should they be skipped instead?
    guarantee(!UseNewCode, "not skipped");
    _sample = IN_RUNTIME;
  } else {
    LogStreamHandle(Debug, profile) log;
    if (log.is_enabled()) {
      trace_sample(&log, jt, f);
      jt->print_native_stack_on(&log);
    }
    _sample = UNKNOWN;
  }
}

static JavaThread* select_target(ThreadsListHandle& tlh) {
  ResourceMark rm;

  int length = tlh.length();
  GrowableArray<JavaThread*> candidates(length);
  for (int i = 0; i < length; i++) {
    JavaThread* target = tlh.thread_at(i);
    if (target->profile_execution()) {
      candidates.append(target);
    }
  }
  if (candidates.length() == 0) {
    return nullptr;
  }
  // Pick a random candidate
  return candidates.at(os::random() % candidates.length());
}

int VMThermostat::sample(int overslept, Sample& the_sample) {
  int idle_ticks = overslept;

  ThermostatHandshake op;
  ThreadsListHandle tlh;
  JavaThread* target = select_target(tlh);

  if (target == nullptr) {
    // No interesting threads running? Well there is no known slowdown
    // on any threads at the moment then
    the_sample = NONE;
    return idle_ticks;
  }

  const Ticks before = Ticks::now();
  Handshake::execute(&op, &tlh, target);
  const Ticks after = Ticks::now();

  Tickspan duration = after - before;
  idle_ticks += duration.nanoseconds() / sampling_interval_nanos;

  the_sample = op.sample();

  return idle_ticks;
}

// Sample a window
void VMThermostat::sample_window(int& overflowed_idle_samples, Sample& overflow_sample,
                                 Sample* samples, Sample* missed) {
  ResourceMark rm(JavaThread::current());

  int current = 0;

  while (current < MIN2(overflowed_idle_samples, samples_per_window)) {
    missed[current] = overflow_sample;
    samples[current] = IDLE;
    ++current;
  }

  if (current == samples_per_window) {
    overflowed_idle_samples -= samples_per_window;
    return;
  }

  if (overflow_sample.mode() != NONE) {
    // Fill in the overflow sample from last time
    guarantee(overflow_sample.mode() != IDLE, "");
    missed[current] = NONE;
    samples[current] = overflow_sample;
    ++current;
    if (current == samples_per_window) {
      overflow_sample = NONE;
      overflowed_idle_samples = 0;
    }
  }

  while (current < samples_per_window) {
    int result = wait_for_tick();

    Sample the_sample;
    int idle_ticks = sample(result, the_sample);
    int remaining_ticks = samples_per_window - current;

    int consumed_idle_ticks = MIN2(idle_ticks, remaining_ticks);

    log_trace(profile)("%3d [%c]: idle_ticks =%4d; remaining_ticks =%4d; consumed_idle_ticks =%4d",
                       current, NAMES[the_sample.mode()], idle_ticks, remaining_ticks, consumed_idle_ticks);

    for (int i = 0; i < consumed_idle_ticks; ++i) {
      missed[current] = the_sample;
      samples[current] = Sample(IDLE);
      ++current;
    }

    if (current == samples_per_window) {
      overflow_sample = the_sample;
      overflowed_idle_samples = idle_ticks - consumed_idle_ticks;
      return;
    }

    missed[current] = NONE;
    samples[current] = the_sample;
    ++current;
  }

  // No overflow to the next window
  overflowed_idle_samples = 0;
  overflow_sample = NONE;
}

int order_floats(const float* v1, const float* v2) {
  if (*v1 > *v2) {
    return -1;
  }

  if (*v1 < *v2) {
    return 1;
  }

  return 0;
}

float VMThermostat::sample2perf(Sample sample) {
  switch (sample.mode()) {
    // Sampled relative performance difference between tiers in a sample program
    case IDLE:          return 0.0f;

    case INTERPRETED:   return 0.0318f;

    case COMPILED_T1:
    case SC_T1:         return 0.5f;    // tier1

    case COMPILED_T2:
    case SC_T2:         return 0.4209f; // tier2

    case COMPILED_T3:   return 0.2364f; // tier3

    case COMPILED_T4:
    case SC_T4:
    case PRELOADED_T4:  return 1.0f;    // tier4

    default: return 1.0f;
  }
}

float VMThermostat::calculate_average(Sample* samples, int from) {
  float sum = 0.0f;
  for (int i = from; i < samples_per_window; ++i) {
    sum += sample2perf(samples[i]);
  }
  float avg = sum / (samples_per_window - from);
  return avg;
}

float VMThermostat::calculate_percentile(VMThermostat::Sample* samples, int percentile) {
  int ignore = percentile * samples_per_window / 1000;
  return int((1.0f - calculate_average(samples, ignore)) * 100.0f);
}

void VMThermostat::report_window_perf(Sample* samples, int window_number) {
  if (window_number == 0) { // print header
    log_info(thermostat)("Time ms\tP0\tP50\tP90");
  }

  qsort(samples, samples_per_window, sizeof(float), (_sort_Fn)order_floats);
  int p0 = calculate_percentile(samples, 0);
  int p50 = calculate_percentile(samples, 500);
  int p90 = calculate_percentile(samples, 900);
  log_info(thermostat)(UINT64_FORMAT "\t%d\t%d\t%d", uint64_t(window_number) * samples_per_window * sampling_interval_nanos / NANOUNITS_PER_MILLIUNIT, p0, p50, p90);
}

void VMThermostat::report_window_prof(Sample* samples, int number_of_samples, int window_number) {
  if (window_number == 0) {
    log_info (profile)("    #   int |  T1  T2  T3  T4 | SC1 SC2 SC4 | SC5 | nat  vm blo | unk not || mis || cli clo i+l ||");
    log_debug(profile)("    #    i  |  c   c   c   C  |  s   s   S  |  P  |  n   v   b  |  ?   n  ||  .  ||             ||");
  }
  int histogram[total_number] = {0};
  for (int i = 0; i < number_of_samples; i++) {
    Sample s = samples[i];
    histogram[s.mode()]++;
  }
  log_info(profile)("%6llu  %3d | %3d %3d %3d %3d | %3d %3d %3d | %3d | %3d %3d %3d | %3d %3d || %3d || --- --- --- || =%d",
                    uint64_t(window_number) * number_of_samples * sampling_interval_nanos / NANOUNITS_PER_MILLIUNIT,
                    histogram[INTERPRETED],
                    histogram[COMPILED_T1], histogram[COMPILED_T2], histogram[COMPILED_T3], histogram[ COMPILED_T4],
                    histogram[      SC_T1], histogram[      SC_T2],                         histogram[       SC_T4],
                    histogram[PRELOADED_T4],
                    histogram[IN_NATIVE], histogram[IN_RUNTIME], histogram[BLOCKED],
                    histogram[UNKNOWN], histogram[NO_JAVA],
                    histogram[IDLE],

                    histogram[INTERPRETED] +
                    histogram[COMPILED_T1] + histogram[COMPILED_T2] + histogram[COMPILED_T3] + histogram[ COMPILED_T4] +
                    histogram[      SC_T1] + histogram[      SC_T2] +                          histogram[       SC_T4] +
                    histogram[PRELOADED_T4] +
                    histogram[IN_NATIVE] + histogram[IN_RUNTIME] + histogram[BLOCKED] +
                    histogram[UNKNOWN] + histogram[NO_JAVA] +
                    histogram[IDLE]);

  LogStreamHandle(Debug, profile) log;
  if (log.is_enabled()) {
    for (int i = 0; i < number_of_samples; i++) {
      if (i % 100 == 0) {
        uint64_t ts = (uint64_t(window_number) * number_of_samples + i) * sampling_interval_nanos / NANOUNITS_PER_MILLIUNIT;
        log.print("%6llu: ", ts);
      }

      Sample s = samples[i];
      log.print("%c", NAMES[(int)s.mode()]);

      if ((i+1) % 100 == 0) {
        log.print_raw("\n");
      }
    }
  }
}

void VMThermostat::run_loop() {
  int overflowed_idle_samples = 0;
  Sample overflow_sample = NONE;
  Sample samples[samples_per_window];
  Sample missed [samples_per_window];
  int window_number = 0;
  const int split = 5;
  while (true) {
    sample_window(overflowed_idle_samples, overflow_sample, samples, missed);
    //report_window_perf(samples, window_number);

//    report_window_prof(samples, samples_per_window, window_number);
    for (int i = 0; i < split; i++) {
      assert(samples_per_window % split == 0, "");
      int wsize = samples_per_window / split;
      Sample* base = samples + i * wsize;
      report_window_prof(base, wsize, split*window_number + i);
    }
    ++window_number;
  }
}
