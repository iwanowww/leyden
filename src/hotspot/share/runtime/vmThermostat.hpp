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

#ifndef SHARE_RUNTIME_VMTHERMOSTAT_HPP
#define SHARE_RUNTIME_VMTHERMOSTAT_HPP

#include "runtime/javaThread.hpp"

class VMThermostat : public JavaThread {
private:
  static VMThermostat* _thermostat;

  enum Mode {
    NONE    = 0,
    UNKNOWN = 1,
    NO_JAVA = 2,
    IDLE    = 3,

    IN_NATIVE  = 4,
    IN_RUNTIME = 5,
    BLOCKED    = 6,

    // Execution modes
    INTERPRETED = 7,
    COMPILED_BASE = INTERPRETED + 1,
    COMPILED_T1 = COMPILED_BASE + 0,
    COMPILED_T2 = COMPILED_BASE + 1,
    COMPILED_T3 = COMPILED_BASE + 2,
    COMPILED_T4 = COMPILED_BASE + 3,

    SHARED_BASE = COMPILED_BASE + 4,
    SC_T1 = SHARED_BASE + 0,
    SC_T2 = SHARED_BASE + 1,
    SC_T3 = SHARED_BASE + 2,
    SC_T4 = SHARED_BASE + 3,

    PRELOADED_BASE = SHARED_BASE + 4,
    PRELOADED_T1 = PRELOADED_BASE + 0,
    PRELOADED_T2 = PRELOADED_BASE + 1,
    PRELOADED_T3 = PRELOADED_BASE + 2,
    PRELOADED_T4 = PRELOADED_BASE + 3,

    total_number
  };

  class Sample {
    Mode _mode;
    Mode _caller_mode;
    int _bits;

  public:
    Sample() : _mode(UNKNOWN), _caller_mode(UNKNOWN) , _bits(0) {}
    Sample(Mode m) : _mode(m), _caller_mode(UNKNOWN) , _bits(0) {}

    Mode mode()        const { return _mode;        }
    Mode caller_mode() const { return _caller_mode; }
    int  bits()        const { return _bits;        }
  };

  static float sample2perf(Sample);

  class ThermostatHandshake : public HandshakeClosure {
  private:
    Sample _sample;

  public:
    ThermostatHandshake() : HandshakeClosure("ThermostatHandshake") {}

    virtual void do_thread(Thread* thread);
    Sample sample() const { return _sample; }
  };

  uint64_t _start_nanos;
  uint64_t _nticks;

  int sample(int overslept, Sample& the_sample);
  void sample_window(int& overflowed_idle_samples, Sample& overflow_sample, Sample* samples, Sample* missed);
  void report_window_perf(Sample* samples, int sample_number);
  void report_window_prof(Sample* samples, int number_of_samples, int sample_number);
  int wait_for_tick();
  VMThermostat();

  float calculate_average(Sample* samples, int from);
  float calculate_percentile(Sample* samples, int percentile);

public:
  static void initialize();
  static void thread_entry(JavaThread* thread, TRAPS);
  bool is_hidden_from_external_view() const override { return true; }
  void run_loop();
};

#endif // SHARE_RUNTIME_VMTHERMOSTAT_HPP
