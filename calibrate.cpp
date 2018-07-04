#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <atomic>

struct Times {
  std::chrono::high_resolution_clock::time_point post;
  std::chrono::high_resolution_clock::time_point start;
  std::chrono::high_resolution_clock::time_point end;
};

double timeDiff(std::chrono::high_resolution_clock::time_point& a,
		std::chrono::high_resolution_clock::time_point& b) {
  return std::chrono::duration_cast<
	   std::chrono::duration<double>>(b-a).count();
}

uint64_t avoidOptimization = 0;

// This will calibrate a delay loop and return a number to which to
// count to create a busy worker of `seconds` seconds.
int64_t x = 0;

int64_t runner(int64_t n, int64_t i) {
  for (int64_t j = 0; j < n; j++) {
    x += j*j;
  }
  return x;
};

std::atomic<bool> raus(false);

void stoerfeuer() {
  int64_t i = 1;
  while (!raus) {
    runner(25000, i++);
  }
}

int main(int argc, char* argv[]) {

  double seconds = 0.001;
  size_t nrStoer = 0;
  if (argc > 1) {
    seconds = std::strtold(argv[1], nullptr);
    if (argc > 2) {
      nrStoer = std::atoi(argv[2]);
    }
  }
  // Create Stoerfeuer:
  std::vector<std::thread> stoerer;
  for (size_t i = 0; i < nrStoer; ++i) {
    stoerer.emplace_back(stoerfeuer);
  }

  std::cout << "Calibrating for time " << seconds << " ..." << std::endl;
  std::cout << "Warming up CPU..." << std::endl;

  // First count some to warm up the CPU:
  avoidOptimization += runner(10000000000, 1);

  std::cout << "Warmup complete! Now racing for real!" << std::endl;
  int64_t n = 500;
  double t = 0.0;
  int64_t i = 1;
  // First start with counting to n and double until it took longer
  // than seconds:
  std::cout << "Find ballpark:" << std::endl;
  while (t < seconds) {
    n *= 2;
    auto startTime = std::chrono::high_resolution_clock::now();
    avoidOptimization += runner(n, ++i);
    auto endTime = std::chrono::high_resolution_clock::now();
    t = timeDiff(startTime, endTime);
    std::cout << "Counted to " << n << " in time " << t << "." << std::endl;
  }

  int rep = 100;
  if (rep * seconds > 5.0) {
    rep = 5.0 / seconds;
  }

  // Now search for the right value:
  std::cout << "Fine tune: (" << rep << " tries):" << std::endl;
  int64_t sum = 0;
  for (int j = 0; j < rep; ++j) {
    n = static_cast<int64_t>(n * (seconds / t));
    auto startTime = std::chrono::high_resolution_clock::now();
    avoidOptimization += runner(n, ++i);
    auto endTime = std::chrono::high_resolution_clock::now();
    t = timeDiff(startTime, endTime);
    std::cout << "Counted to " << n << " in time " << t << "." << std::endl;
    sum += n;
  }
  n = sum/rep;
  // Finally, verify:
  std::cout << "Verification (" << rep << " tries):" << std::endl;
  double sumd = 0;
  std::vector<double> times;
  times.reserve(rep);
  for (int j = 0; j < rep; ++j) {
    auto startTime = std::chrono::high_resolution_clock::now();
    avoidOptimization += runner(n, ++i);
    auto endTime = std::chrono::high_resolution_clock::now();
    t = timeDiff(startTime, endTime);
    std::cout << "Counted to " << n << " in time " << t << "." << std::endl;
    sumd += t;
    times.push_back(t);
  }
  
  std::sort(times.begin(), times.end());
  size_t mid = times.size()/2;
  double median = times.size() & 1 ? times[mid]
                                   : (times[mid-1] + times[mid]) / 2;
  std::cout << "Calibration result: n=" << n << ", t(avg)=" << sumd/rep
    << ", t(med)=" << median << std::endl;

  raus = true;
  for (auto& s : stoerer) {
    s.join();
  }
  return 0;
}


