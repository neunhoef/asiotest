#ifndef PRETTY_TIME_H
#define PRETTY_TIME_H

std::string prettyTime(uint64_t nanoseconds)
{
  if (nanoseconds < 10000) {
    return std::to_string(nanoseconds) + " ns";
  } else if (nanoseconds < 10000000) {
    return std::to_string((nanoseconds / 10) / 100.0) + " us";
  } else if (nanoseconds < 10000000000) {
    return std::to_string((nanoseconds / 10000) / 100.0) + " ms";
  } else {
    return std::to_string((nanoseconds / 10000000) / 100.0) + " s";
  }
}

#endif
