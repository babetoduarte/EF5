#ifndef COMMON_PARALLEL_H
#define COMMON_PARALLEL_H

#include <map>
#include <unordered_map>
#include <vector>

#define FAC_COUNT 1000

struct FAcPos {
  int index;
  long fac;
};

static bool SortNodesByFlowAccum(FAcPos d1, FAcPos d2);

bool SortNodesByFlowAccum(FAcPos d1, FAcPos d2) {
  return d1.fac < d2.fac;
}

/* extern std::vector<FAcPos> FAcIndexes; */
/* extern std::vector<long> FAcCount; */
std::vector<FAcPos> FAcIndexes;
std::unordered_map<long,long> FAcCount;
long MaxFAc;

#endif // COMMON_PARALLEL_H
