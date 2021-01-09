#ifdef __linux__
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <libgen.h>
#include <random>
#include <string>
#include <vector>

#include "linux-perf-events.h"
#include "aligned_alloc.h"
#include "pospopcnt_avx512bw.h"
#include "pospopcnt.h"
#ifdef ALIGN
#include "memalloc.h"
#define memory_allocate(size) aligned_alloc(64, (size))
#else
#define memory_allocate(size) malloc(size)
#endif

// Function pointer definition.
typedef void (*pospopcnt_u16_method_type)(const uint16_t *data, uint32_t len,
                                          uint32_t *flags);

extern "C" void count16avx512(uint64_t flags[16], const uint16_t *buf, size_t len);

static void pospopcnt_count16avx512(const uint16_t *data, uint32_t len, uint32_t *flags)
{

	// TODO: fix type mismatch in flags
	count16avx512((uint64_t*)flags, data, len);
}

#define PPOPCNT_NUMBER_METHODS 5
pospopcnt_u16_method_type pospopcnt_u16_methods[] = {
  pospopcnt_u16_scalar, pospopcnt_u16_avx512bw_harvey_seal_1KB, pospopcnt_u16_avx512bw_harvey_seal_512B, pospopcnt_u16_avx512bw_harvey_seal_256B, pospopcnt_count16avx512,
};

static const char *const pospopcnt_u16_method_names[] = {
  "pospopcnt_u16_scalar", "pospopcnt_u16_avx512bw_harvey_seal_1KB", "pospopcnt_u16_avx512bw_harvey_seal_512B", "pospopcnt_u16_avx512bw_harvey_seal_256B", "pospopcnt_count16avx512",
};

void print16(uint32_t *flags) {
  for (int k = 0; k < 16; k++)
    printf(" %8u ", flags[k]);
  printf("\n");
}

std::vector<unsigned long long>
compute_mins(std::vector<std::vector<unsigned long long> > allresults) {
  if (allresults.size() == 0)
    return std::vector<unsigned long long>();

  std::vector<unsigned long long> answer = allresults[0];

  for (size_t k = 1; k < allresults.size(); k++) {
    assert(allresults[k].size() == answer.size());
    for (size_t z = 0; z < answer.size(); z++) {
      if (allresults[k][z] < answer[z])
        answer[z] = allresults[k][z];
    }
  }
  return answer;
}

std::vector<double>
compute_averages(std::vector<std::vector<unsigned long long> > allresults) {
  if (allresults.size() == 0)
    return std::vector<double>();

  std::vector<double> answer(allresults[0].size());

  for (size_t k = 0; k < allresults.size(); k++) {
    assert(allresults[k].size() == answer.size());
    for (size_t z = 0; z < answer.size(); z++) {
      answer[z] += allresults[k][z];
    }
  }

  for (size_t z = 0; z < answer.size(); z++) {
    answer[z] /= allresults.size();
  }
  return answer;
}

/**
 * @brief
 *
 * @param n          Number of integers.
 * @param iterations Number of iterations.
 * @param fn         Target function pointer.
 * @param verbose    Flag enabling verbose output.
 * @return           Returns true if the results are correct. Returns false if
 *the results
 *                   are either incorrect or the target function is not
 *supported.
 */
bool benchmark(uint32_t n, uint32_t iterations, pospopcnt_u16_method_type fn,
               bool verbose, bool test) {
  std::vector<int> evts;
  uint16_t *vdata = (uint16_t *)memory_allocate(n * sizeof(uint16_t));
  std::unique_ptr<uint16_t, decltype(&free)> dataholder(vdata, free);
  if (verbose) {
    printf("alignment: %d\n", get_alignment(vdata));
  }
  evts.push_back(PERF_COUNT_HW_CPU_CYCLES);
  evts.push_back(PERF_COUNT_HW_INSTRUCTIONS);
  evts.push_back(PERF_COUNT_HW_BRANCH_MISSES);
  evts.push_back(PERF_COUNT_HW_CACHE_REFERENCES);
  evts.push_back(PERF_COUNT_HW_CACHE_MISSES);
  evts.push_back(PERF_COUNT_HW_REF_CPU_CYCLES);
  LinuxEvents<PERF_TYPE_HARDWARE> unified(evts);
  std::vector<unsigned long long> results; // tmp buffer
  std::vector<std::vector<unsigned long long> > allresults;
  results.resize(evts.size());

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 0xFFFF);

  bool isok = true;
  for (uint32_t i = 0; i < iterations; i++) {
    for (size_t k = 0; k < n; k++) {
      vdata[k] = dis(gen); // random init.
    }
    uint32_t correctflags[16] = { 0 };
    pospopcnt_u16_scalar(vdata, n, correctflags); // this is our gold standard
//    uint32_t flags[16] = { 0 };
    uint32_t flags[32] = { 0 }; // kludge!

    unified.start();
    fn(vdata, n, flags);
    unified.end(results);

    uint64_t tot_obs = 0;
    for (size_t k = 0; k < 16; ++k)
      tot_obs += flags[k];
    if (tot_obs == 0) { // when a method is not supported it returns all zero
      return false;
    }

    for (size_t k = 0; k < 16; k++) {
      if (correctflags[k] != flags[k]) {
        if (test) {
          printf("bug:\n");
          printf("expected : ");
          print16(correctflags);
          printf("got      : ");
          print16(flags);
          return false;
        } else {
          isok = false;
        }
      }
    }
    allresults.push_back(results);
  }

  std::vector<unsigned long long> mins = compute_mins(allresults);
  std::vector<double> avg = compute_averages(allresults);

  if (verbose) {
    printf("instructions per cycle %4.2f, cycles per 16-bit word:  %4.3f, "
           "instructions per 16-bit word %4.3f \n",
           double(mins[1]) / mins[0], double(mins[0]) / n, double(mins[1]) / n);
    // first we display mins
    printf("min: %8llu cycles, %8llu instructions, \t%8llu branch mis., %8llu "
           "cache ref., %8llu cache mis.\n",
           mins[0], mins[1], mins[2], mins[3], mins[4]);
    printf("avg: %8.1f cycles, %8.1f instructions, \t%8.1f branch mis., %8.1f "
           "cache ref., %8.1f cache mis.\n",
           avg[0], avg[1], avg[2], avg[3], avg[4]);
  } else {
    printf(
        "cycles per 16-bit word:  %4.3f; ref cycles per 16-bit word: %4.3f \n",
        double(mins[0]) / n, double(mins[5]) / n);
  }

  return isok;
}

/**
 * @brief
 *
 * @param n          Number of integers.
 * @parem m          Number of arrays.
 * @param iterations Number of iterations.
 * @param fn         Target function pointer.
 * @param verbose    Flag enabling verbose output.
 * @return           Returns true if the results are correct. Returns false if
 *the results
 *                   are either incorrect or the target function is not
 *supported.
 */
template <class C>
bool benchmarkMany(C & vdata, uint32_t n, uint32_t m, uint32_t iterations,
                   pospopcnt_u16_method_type fn, bool verbose, bool test) {
  std::vector<int> evts;
#ifdef ALIGN
  for (auto &x : vdata) {
    assert(get_alignment(x.data()) == 64);
  }
#endif
  if (verbose) {
    printf("alignments: ");
    for (auto &x : vdata) {
      printf("%d ", get_alignment(x.data()));
    }
    printf("\n");
  }
  evts.push_back(PERF_COUNT_HW_CPU_CYCLES);
  evts.push_back(PERF_COUNT_HW_INSTRUCTIONS);
  evts.push_back(PERF_COUNT_HW_BRANCH_MISSES);
  evts.push_back(PERF_COUNT_HW_CACHE_REFERENCES);
  evts.push_back(PERF_COUNT_HW_CACHE_MISSES);
  evts.push_back(PERF_COUNT_HW_REF_CPU_CYCLES);
  LinuxEvents<PERF_TYPE_HARDWARE> unified(evts);
  std::vector<unsigned long long> results; // tmp buffer
  std::vector<std::vector<unsigned long long> > allresults;
  std::vector<double> timings;
  std::vector<double> freqs;
  results.resize(evts.size());

  bool isok = true;
  uint32_t test_iterations = 1; // we run one test iteration
  for (uint32_t i = 0; i < test_iterations; i++) {
    std::vector<std::vector<uint32_t> > correctflags(m,
                                                     std::vector<uint32_t>(16));
    for (size_t k = 0; k < m; k++) {
      pospopcnt_u16_scalar(vdata[k].data(), vdata[k].size(),
                           correctflags[k].data()); // this is our gold standard
    }
    std::vector<std::vector<uint32_t> > flags(m, std::vector<uint32_t>(16));
    for (size_t k = 0; k < m; k++) {
      fn(vdata[k].data(), vdata[k].size(), flags[k].data());
    }

    uint64_t tot_obs = 0;
    for (size_t km = 0; km < m; ++km)
      for (size_t k = 0; k < 16; ++k)
        tot_obs += flags[km][k];
    if (tot_obs == 0) { // when a method is not supported it returns all zero
      return false;
    }
    for (size_t km = 0; km < m; ++km) {
      for (size_t k = 0; k < 16; k++) {
        if (correctflags[km][k] != flags[km][k]) {
          if (test) {
            printf("bug:\n");
            printf("expected : ");
            print16(correctflags[km].data());
            printf("got      : ");
            print16(flags[km].data());
            return false;
          } else {
            isok = false;
          }
        }
      }
    }
  }

  for (uint32_t i = 0; i < iterations; i++) {
    std::vector<std::vector<uint32_t> > flags(m, std::vector<uint32_t>(16));
    auto start = std::chrono::steady_clock::now();
    unified.start();
    for (size_t k = 0; k < m; k++) {
      fn(vdata[k].data(), vdata[k].size(), flags[k].data());
    }
    unified.end(results);
    auto end = std::chrono::steady_clock::now();

    std::chrono::duration<double> secs = end - start;
    double time_in_s = secs.count();
    timings.push_back(time_in_s);
    freqs.push_back(results[0]/(1000000000*time_in_s));
    allresults.push_back(results);
  }

  std::vector<unsigned long long> mins = compute_mins(allresults);
  std::vector<double> avg = compute_averages(allresults);
  double min_timing = *min_element(timings.begin(), timings.end());
  double min_freq = *min_element(freqs.begin(), freqs.end());
  double max_freq = *max_element(freqs.begin(), freqs.end());
  
  double speedinGBs = (m * n * sizeof(uint16_t)) / (min_timing * 1000000000.0);

  if (verbose) {
    printf("instructions per cycle %4.2f, cycles per 16-bit word:  %4.3f, "
           "instructions per 16-bit word %4.3f \n",
           double(mins[1]) / mins[0], double(mins[0]) / (n * m),
           double(mins[1]) / (n * m));
    // first we display mins
    printf("min: %8llu cycles, %8llu instructions, \t%8llu branch mis., %8llu "
           "cache ref., %8llu cache mis.\n",
           mins[0], mins[1], mins[2], mins[3], mins[4]);
    printf("avg: %8.1f cycles, %8.1f instructions, \t%8.1f branch mis., %8.1f "
           "cache ref., %8.1f cache mis.\n",
           avg[0], avg[1], avg[2], avg[3], avg[4]);
    printf(" %4.3f GB/s \n", speedinGBs);
    printf("estimated clock in range %4.3f GHz to %4.3f GHz\n", min_freq, max_freq);
  } else {
    printf(
        "cycles per 16-bit word:  %4.3f; ref cycles per 16-bit word: %4.3f; speed in GB/s %4.3f \n",
        double(mins[0]) / (n * m), double(mins[5]) / (n * m), speedinGBs);
  }

  return isok;
}
template <class C>
void  benchmarkCopy(C & vdata, uint32_t n, uint32_t m, uint32_t iterations, bool verbose) {
  std::vector<int> evts;
  size_t maxsize = 0;
#ifdef ALIGN
  for (auto &x : vdata) { 
     if(maxsize < x.size()) maxsize = x.size();
     assert(get_alignment(x.data()) == 64);
  }
#endif
  for (auto &x : vdata) { 
     if(maxsize < x.size()) maxsize = x.size();
  }
  if (verbose) {
    printf("alignments: ");
    for (auto &x : vdata) {
      printf("%d ", get_alignment(x.data()));
    }
    printf("\n");
  }
  evts.push_back(PERF_COUNT_HW_CPU_CYCLES);
  evts.push_back(PERF_COUNT_HW_INSTRUCTIONS);
  evts.push_back(PERF_COUNT_HW_BRANCH_MISSES);
  evts.push_back(PERF_COUNT_HW_CACHE_REFERENCES);
  evts.push_back(PERF_COUNT_HW_CACHE_MISSES);
  evts.push_back(PERF_COUNT_HW_REF_CPU_CYCLES);
  LinuxEvents<PERF_TYPE_HARDWARE> unified(evts);
  std::vector<unsigned long long> results; // tmp buffer
  std::vector<std::vector<unsigned long long> > allresults;
  std::vector<double> timings;
  std::vector<double> freqs;
  results.resize(evts.size());
  std::vector<uint16_t> copybuf(maxsize);

  for (uint32_t i = 0; i < iterations; i++) {
    std::vector<std::vector<uint32_t> > flags(m, std::vector<uint32_t>(16));
    auto start = std::chrono::steady_clock::now();
    unified.start();
    for (size_t k = 0; k < m; k++) {
      ::memcpy(copybuf.data(),vdata[k].data(),vdata[k].size()); 
    }
    unified.end(results);
    auto end = std::chrono::steady_clock::now();

    std::chrono::duration<double> secs = end - start;
    double time_in_s = secs.count();
    timings.push_back(time_in_s);
    freqs.push_back(results[0]/(1000000000*time_in_s));
    allresults.push_back(results);
  }

  std::vector<unsigned long long> mins = compute_mins(allresults);
  std::vector<double> avg = compute_averages(allresults);
  double min_timing = *min_element(timings.begin(), timings.end());
  double min_freq = *min_element(freqs.begin(), freqs.end());
  double max_freq = *max_element(freqs.begin(), freqs.end());
  
  double speedinGBs = (m * n * sizeof(uint16_t)) / (min_timing * 1000000000.0);

  if (verbose) {
    printf("instructions per cycle %4.2f, cycles per 16-bit word:  %4.3f, "
           "instructions per 16-bit word %4.3f \n",
           double(mins[1]) / mins[0], double(mins[0]) / (n * m),
           double(mins[1]) / (n * m));
    // first we display mins
    printf("min: %8llu cycles, %8llu instructions, \t%8llu branch mis., %8llu "
           "cache ref., %8llu cache mis.\n",
           mins[0], mins[1], mins[2], mins[3], mins[4]);
    printf("avg: %8.1f cycles, %8.1f instructions, \t%8.1f branch mis., %8.1f "
           "cache ref., %8.1f cache mis.\n",
           avg[0], avg[1], avg[2], avg[3], avg[4]);
    printf(" %4.3f GB/s \n", speedinGBs);
    printf("estimated clock in range %4.3f GHz to %4.3f GHz\n", min_freq, max_freq);
  } else {
    printf(
        "cycles per 16-bit word:  %4.3f; ref cycles per 16-bit word: %4.3f; speed in GB/s %4.3f \n",
        double(mins[0]) / (n * m), double(mins[5]) / (n * m), speedinGBs);
  }

}

void measureoverhead(uint32_t n, uint32_t iterations, bool verbose) {
  std::vector<int> evts;
  evts.push_back(PERF_COUNT_HW_CPU_CYCLES);
  evts.push_back(PERF_COUNT_HW_INSTRUCTIONS);
  evts.push_back(PERF_COUNT_HW_BRANCH_MISSES);
  evts.push_back(PERF_COUNT_HW_CACHE_REFERENCES);
  evts.push_back(PERF_COUNT_HW_CACHE_MISSES);
  evts.push_back(PERF_COUNT_HW_REF_CPU_CYCLES);
  LinuxEvents<PERF_TYPE_HARDWARE> unified(evts);
  std::vector<unsigned long long> results; // tmp buffer
  std::vector<std::vector<unsigned long long> > allresults;
  results.resize(evts.size());

  for (uint32_t i = 0; i < iterations; i++) {
    unified.start();
    unified.end(results);
    allresults.push_back(results);
  }

  std::vector<unsigned long long> mins = compute_mins(allresults);
  std::vector<double> avg = compute_averages(allresults);
  printf("%-40s\t", "nothing");
  if (verbose) {
    printf("instructions per cycle %4.2f, cycles per 16-bit word:  %4.3f, "
           "instructions per 16-bit word %4.3f \n",
           double(mins[1]) / mins[0], double(mins[0]) / n, double(mins[1]) / n);
    // first we display mins
    printf("min: %8llu cycles, %8llu instructions, \t%8llu branch mis., %8llu "
           "cache ref., %8llu cache mis.\n",
           mins[0], mins[1], mins[2], mins[3], mins[4]);
    printf("avg: %8.1f cycles, %8.1f instructions, \t%8.1f branch mis., %8.1f "
           "cache ref., %8.1f cache mis.\n",
           avg[0], avg[1], avg[2], avg[3], avg[4]);
  } else {
    printf(
        "cycles per 16-bit word:  %4.3f; ref cycles per 16-bit word: %4.3f \n",
        double(mins[0]) / n, double(mins[5]) / n);
  }
}

static void print_usage(char *command) {
  printf(" Try %s -n 100000 -i 15 -v \n", command);
  printf("-n is the number of 16-bit words \n");
  printf("-i is the number of tests or iterations \n");
  printf("-v makes things verbose\n");
}

int main(int argc, char **argv) {
  size_t n = 10000000;
  size_t m = 1;
  size_t iterations = 0;
  bool verbose = false;
  int c;

  while ((c = getopt(argc, argv, "vhm:n:i:")) != -1) {
    switch (c) {
    case 'n':
      n = atoll(optarg);
      break;
    case 'm':
      m = atoll(optarg);
      break;
    case 'v':
      verbose = true;
      break;
    case 'h':
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    case 'i':
      iterations = atoi(optarg);
      break;
    default:
      abort();
    }
  }

  if (n > UINT32_MAX) {
    printf("setting n to %u \n", UINT32_MAX);
    n = UINT32_MAX;
  }

  if (iterations > UINT32_MAX) {
    printf("setting iterations to %u \n", UINT32_MAX);
    iterations = UINT32_MAX;
  }

  if (iterations == 0) {
    if (m * n < 1000000)
      iterations = 10000;
    else
      iterations = 100;
  }
  printf("n = %zu m = %zu \n", n, m);
  printf("iterations = %zu \n", iterations);
  if (n == 0) {
    printf("n cannot be zero.\n");
    return EXIT_FAILURE;
  }

  size_t array_in_bytes = sizeof(uint16_t) * n * m;
  if (array_in_bytes < 1024) {
    printf("array size: %zu B\n", array_in_bytes);
  } else if (array_in_bytes < 1024 * 1024) {
    printf("array size: %.3f kB\n", array_in_bytes / 1024.);
  } else {
    printf("array size: %.3f MB\n", array_in_bytes / (1024 * 1024.));
  }

  measureoverhead(n * m, iterations, verbose);
  int maxtrial = 3;
#ifdef ALIGN
  std::vector<std::vector<uint16_t, AlignedSTLAllocator<uint16_t, 64> > > vdata(
      m, std::vector<uint16_t, AlignedSTLAllocator<uint16_t, 64> >(n));
#else
  std::vector<std::vector<uint16_t> > vdata(m, std::vector<uint16_t>(n));
#endif
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 0xFFFF);
  for (size_t k = 0; k < vdata.size(); k++) {
      for (size_t k2 = 0; k2 < vdata[k].size(); k2++) {
        vdata[k][k2] = dis(gen); // random init.
      }
  }
  printf("%-40s\t", "memcpy");
  benchmarkCopy(vdata, n, m, iterations, verbose);
  printf("\n");
   
  for (int t = 0; t < maxtrial; t++) {
    printf("\n== Trial %d out of %d \n", t + 1, maxtrial);
    for (size_t k = 0; k < PPOPCNT_NUMBER_METHODS; k++) {
      printf("\n");
      printf("%-40s\t", pospopcnt_u16_method_names[k]);
      fflush(NULL);
      bool isok = benchmarkMany(vdata, n, m, iterations, pospopcnt_u16_methods[k],
                                verbose, true);
      if (isok == false) {
        printf("Problem detected with %s.\n", pospopcnt_u16_method_names[k]);
      }
      if (verbose)
        printf("\n");
    }
  }
  if (!verbose)
    printf("Try -v to get more details.\n");

  return EXIT_SUCCESS;
}
#else //  __linux__

#include <stdio.h>
#include <stdlib.h>

int main() {
  printf("This is a linux-specific benchmark\n");
  return EXIT_SUCCESS;
}

#endif
