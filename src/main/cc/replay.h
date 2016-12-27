#ifndef __REPLAY_H
#define __REPLAY_H

#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cassert>
#include "sample.h"

enum PUT_VALUE_TYPE { PUT_POKE, PUT_LOAD, PUT_FORCE };

template<class T> struct replay_data_t {
  std::vector<T> signals;
  std::map<std::string, size_t> signal_map;
};

template <class T> class replay_t {
public:
  replay_t(): cycles(0L), log(false), pass(true), is_exit(false) { }
 
  void init(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    for (auto &arg: args) {
      if (arg.find("+sample=") == 0) {
        load_samples(arg.c_str() + 8);
      }
      if (arg.find("+verbose") == 0) {
        log = true;
      }
    }
  }
 
  virtual int finish() {
    fprintf(stderr, "[%s] Runs %" PRIu64 " cycles\n",
      pass ? "PASS" : "FAIL", cycles);
    for (size_t i = 0 ; i < samples.size() ; i++) {
      delete samples[i];
    }
    samples.clear();
    return exitcode();
  }

  void reset(size_t n) {
    biguint_t one = 1;
    size_t id = replay_data.signal_map["reset"];
    put_value(replay_data.signals[id], &one, PUT_POKE);
    take_steps(n);
  }

  virtual void replay() {
    reset(5);
    for (size_t k = 0 ; k < samples.size() ; k++) {
      sample_t *sample = samples[k];
      std::cerr << " * REPLAY AT CYCLE " << sample->get_cycle() << " * " << std::endl;
      reset(5);
      for (size_t i = 0 ; i < sample->get_cmds().size() ; i++) {
        sample_inst_t* cmd = sample->get_cmds()[i];
        if (step_t* p = dynamic_cast<step_t*>(cmd)) {
          step(p->n);
        }
        if (load_t* p = dynamic_cast<load_t*>(cmd)) {
          if (p->idx < 0) {
            load(chains[p->type][p->id], p->value);
          } else {
            std::string signal = chains[p->type][p->id] + "[" + std::to_string(p->idx) + "]";
            load(signal, p->value);
          }
        }
        if (poke_t* p = dynamic_cast<poke_t*>(cmd)) {
          poke(chains[p->type][p->id], p->value);
        }
        if (expect_t* p = dynamic_cast<expect_t*>(cmd)) {
          pass &= expect(chains[p->type][p->id], p->value);
        }
      }
    }
    is_exit = true;
  }

protected:
  replay_data_t<T> replay_data;

  bool done() { return is_exit; }
  int exitcode() { return pass ? EXIT_SUCCESS : EXIT_FAILURE; }

private:
  uint64_t cycles;
  bool log;
  bool pass;
  bool is_exit;
  std::vector<sample_t*> samples;
  std::vector<std::vector<std::string>> chains;

  void load_samples(const char* filename) {
    std::ifstream file(filename);
    if (!file) {
      fprintf(stderr, "Cannot open %s\n", filename);
      exit(EXIT_FAILURE);
    }
    std::string line;
    size_t steps = 0;
    sample_t* sample = NULL;
    while (std::getline(file, line)) {
      std::istringstream iss(line);
      size_t type, t, id, n;
      ssize_t idx;
      uint64_t cycles;
      std::string signal, dummy;
      biguint_t *value = NULL;
      iss >> type;
      switch(static_cast<SAMPLE_INST_TYPE>(type)) {
        case SIGNALS:
          iss >> t >> signal;
          while(chains.size() <= t) {
            chains.push_back(std::vector<std::string>());
          }
          chains[t].push_back(signal);
          break;
        case CYCLE:
          iss >> dummy >> cycles;
          sample = new sample_t(cycles);
          samples.push_back(sample);
          steps = 0;
          break;
        case LOAD:
          value = new biguint_t;
          iss >> t >> id >> *value >> idx;
          sample->add_cmd(new load_t(t, id, value, idx));
          break;
        case FORCE:
          value = new biguint_t;
          iss >> t >> id >> *value;
          sample->add_cmd(new force_t(t, id, value));
          break;
        case POKE:
          value = new biguint_t;
          iss >> t >> id >> *value;
          sample->add_cmd(new poke_t(t, id, value));
          break;
        case STEP:
          iss >> n;
          sample->add_cmd(new step_t(n));
          steps += n;
          break;
        case EXPECT:
          value = new biguint_t;
          iss >> t >> id >> *value;
          if (steps > 1) sample->add_cmd(new expect_t(t, id, value));
          break;
        default:
          break;
      }
    }
    file.close();
  }

  virtual void take_steps(size_t) = 0;
  virtual void put_value(T& sig, biguint_t* data, PUT_VALUE_TYPE type) = 0;
  virtual biguint_t get_value(T& sig) = 0;

  inline void step(size_t n) {
    cycles += n;
    if (log) std::cerr << " * STEP " << n << " -> " << cycles << " *" << std::endl;
    take_steps(n);
  }

  inline void check_signal(const std::string& signal) {
    assert(replay_data.signal_map.find(signal) != replay_data.signal_map.end());
  }

  inline void force(const std::string& node, biguint_t* data) {
    if (log) std::cerr << " * FORCE " << node << " <- 0x" << *data << " *" << std::endl;
    check_signal(node);
    size_t id = replay_data.signal_map[node];
    put_value(replay_data.signals[id], data, PUT_FORCE);
  }

  inline void load(const std::string& node, biguint_t* data) {
    if (log) std::cerr << " * LOAD " << node << " <- 0x" << *data << " *" << std::endl;
    check_signal(node);
    size_t id = replay_data.signal_map[node];
    put_value(replay_data.signals[id], data, PUT_LOAD);
  }

  inline void poke(const std::string& node, biguint_t* data) {
    if (log) std::cerr << " * POKE " << node << " <- 0x" << *data << " *" << std::endl;
    check_signal(node);
    size_t id = replay_data.signal_map[node];
    put_value(replay_data.signals[id], data, PUT_POKE);
  }

  inline bool expect(const std::string& node, biguint_t* expected) {
    check_signal(node);
    size_t id = replay_data.signal_map[node];
    biguint_t value = get_value(replay_data.signals[id]);
    bool pass = value == *expected || cycles <= 1;
    if (log) {
      std::cerr << " * EXPECT " << node << " -> 0x" << value << " ?= 0x" << *expected;
      std::cerr << (pass ? " : PASS" : " : FAIL") << " *" << std::endl;
    }
    return pass;
  }
};

#endif //__REPLAY_H
