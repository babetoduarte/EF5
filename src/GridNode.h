#ifndef GRID_NODE_H
#define GRID_NODE_H

#include "GaugeConfigSection.h"
#include <limits>
#include <vector>

const unsigned long INVALID_DOWNSTREAM_NODE = std::numeric_limits<unsigned long>::max();

struct BasicGridNode {};

struct GridNode {
  long x;
  long y;
  RefLoc refLoc;
  float slope;
  long fac;
  float area;
  float contribArea;
  float horLen;
  bool channelGridCell;
  GaugeConfigSection *gauge;
  unsigned long downStreamNode;
  unsigned long index;
  unsigned long modelIndex;
  BasicGridNode *modelNode;
};

typedef std::vector<GridNode> GridNodeVec;

#endif
