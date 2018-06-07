#include "KinematicRoute.h"
#include "AscGrid.h"
#include "DatedName.h"
//#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <limits>
#include <queue>
#include <utility>
#include <stdexcept>
#include <assert.h>
#include "omp.h"

const float STD_AN_INFINITY_POSITIVE = std::numeric_limits<float>::infinity();

extern "C" {
  int myisfinite(float x) {
    return(!(x == STD_AN_INFINITY_POSITIVE));
  }
}

// fac -> index map
static std::map<long,std::vector<int> > FAcMapping;

std::pair<std::vector<int>, std::vector<int>> GetDependencies(const std::vector<GridNode> &nodes){
  std::vector<int> deps(nodes.size(),0); //How many upstream dependencies each node has
  std::vector<int> ordering;             //Order in which nodes must be processed to ensure causal independence
  std::vector<int> levels;               //Set of half-open intervals defining parallelisable sets of nodes

  ordering.reserve(nodes.size());        //Ordering will eventually include every node

  //q will be used to generate a breadth-first traversal of the flow graph
  std::queue<int> q;                     

  //Add an initial marker so each level is defined by a pair of integers
  //indicating a half-open interval [start,end).
  levels.emplace_back(0);

  // for(unsigned int i=0;i<nodes.size();i++) 
  //   std::cout << "dn "<<nodes[i].downStreamNode << std::endl;

  // throw std::runtime_error("stop");

  //Loop through all nodes figuring out how many upstream dependencies they each
  //have
  for(unsigned int i=0;i<nodes.size();i++) 
    if (nodes[i].downStreamNode != INVALID_DOWNSTREAM_NODE)
      deps[nodes[i].downStreamNode]++;

  //Find those nodes with no upstream dependencies
  for(unsigned int i=0;i<deps.size();i++)
    if(deps[i]==0)                       //Node has no upstream dependencies, so it's now part of the frontier of the BFS
      q.emplace(i);                      //Node was a local maxima, so it is part of the initial frontier of the BFS
  q.emplace(-1);                         //Special marker that indicates the end of a level
    
  while(true){                           //Break condition is below
    const auto c = q.front();            //Index of current node
    q.pop();

    if(c==-1){                           //This is a level marker, so we need to make a note
      levels.emplace_back(ordering.size());   //Make the note
      if(q.empty())                      //If the queue is empty after eliminating the level marker
        break;                           //then we've reached the end.
      q.emplace(-1);                     //Put a new marker on the end of the queue
      continue;
    }

    const auto &cn = nodes[c];           //The current node itself
    const auto dn  = cn.downStreamNode;  //Index of the downstream node
    ordering.emplace_back(c);                 //Place the node in the ordering
    if (dn != INVALID_DOWNSTREAM_NODE)
      if(--deps[dn]==0)                    //Downstream neighbour has one fewer upstream dependency. Add it to the queue if there are no upstream dependencies.
	q.emplace(cn.downStreamNode);      //Place the downstream neighbour into the queue
  }

  //Add a final marker so each level is defined by a pair of integers indicating
  //a half-open interval [start,end).
  levels.emplace_back(ordering.size());

  //Vector allocation is exponential, so we end up using more space than we
  //need. This shrinks the vectors to their minimum necessary length, i.e. there
  //is no reserved space.
  ordering.shrink_to_fit();
  levels.shrink_to_fit();

  return std::make_pair(ordering,levels);
}

static const char *stateStrings[] = {
    "pCQ",
    "pOQ",
    "IR",
};

KWRoute::KWRoute() {
}

KWRoute::~KWRoute() {}

float KWRoute::SetObsInflow(long index, float inflow) {
  KWGridNode *cNode = &(kwNodes[index]);
  GridNode *node = &nodes->at(index);
  float prev;
  if (!node->channelGridCell) {
    prev = cNode->states[STATE_KW_PQ] * node->horLen;
    cNode->states[STATE_KW_PQ] = inflow / node->horLen;
    cNode->incomingWaterOverland = inflow / node->horLen;
  } else {
    prev = cNode->states[STATE_KW_PQ];
    float diff = 0.0;
    if (inflow > 1.0 && prev > 1.0) {
      diff = inflow / prev - 1.0;
    }
    GaugeConfigSection *thisGauge = node->gauge;
    float multiplier = 1.0;
    if (diff > 0.2) {
      multiplier = 0.5;
    } else if (diff < -0.2) {
      multiplier = 2.0;
    }
    printf(" Multiplier %f,%f,%f,%f ", multiplier, diff, inflow, prev);
    if (multiplier != 1.0) {
      size_t numNodes = nodes->size();
      for (size_t i = 0; i < numNodes; i++) {
        node = &nodes->at(i);
        if (node->gauge == thisGauge) {
          KWGridNode *cNode = &(kwNodes[i]);
          cNode->params[PARAM_KINEMATIC_ALPHA] *= multiplier;
          if (cNode->params[PARAM_KINEMATIC_ALPHA] < 0.01) {
            cNode->params[PARAM_KINEMATIC_ALPHA] = 0.01;
          } else if (cNode->params[PARAM_KINEMATIC_ALPHA] > 200.0) {
            cNode->params[PARAM_KINEMATIC_ALPHA] = 200.0;
          }
        }
      }
    }
    cNode->states[STATE_KW_PQ] = inflow;
    cNode->incomingWaterChannel = inflow;
  }
  return prev;
}

bool KWRoute::InitializeModel(
    std::vector<GridNode> *newNodes,
    std::map<GaugeConfigSection *, float *> *paramSettings,
    std::vector<FloatGrid *> *paramGrids) {

  nodes = newNodes;
  if (kwNodes.size() != nodes->size()) {
    kwNodes.resize(nodes->size());
  }

  auto orderingLevelsPair = GetDependencies(*nodes);
  ordering = orderingLevelsPair.first;
  levels = orderingLevelsPair.second;

  #pragma acc enter data create(this[0:1])

  // Fill in modelIndex in the gridNodes
  size_t numNodes = nodes->size();
  for (size_t i = 0; i < numNodes; i++) {
    GridNode *node = &nodes->at(i);
    node->modelIndex = i;
    KWGridNode *cNode = &(kwNodes[i]);
    cNode->slopeSqrt = pow(node->slope, 0.5f);
    cNode->hillSlopeSqrt = pow(node->slope * 0.5, 0.5f);
    cNode->incomingWater[KW_LAYER_INTERFLOW] = 0.0;
    cNode->incomingWater[KW_LAYER_FASTFLOW] = 0.0;
    cNode->incomingWaterOverland = 0.0;
    cNode->incomingWaterChannel = 0.0;
    for (int p = 0; p < STATE_KW_QTY; p++) {
      cNode->states[p] = 0.0;
    }
    FAcMapping[node->fac].push_back(i);
  }

  // printf("Index1 is %li and index2 is %li\n", index1, index2);
  InitializeParameters(paramSettings, paramGrids);
  initialized = false;
  maxSpeed = 1.0;

  gridSize = nodes->size();
  nodesPtr = nodes->data();
#pragma acc enter data copyin(nodesPtr[0:gridSize])

  kwNodesPtr = kwNodes.data();
  assert(gridSize == kwNodes.size());
#pragma acc enter data copyin(kwNodesPtr[0:gridSize])

  orderingPtr = ordering.data();
  assert(gridSize == ordering.size());
#pragma acc enter data copyin(orderingPtr[0:gridSize])

  return true;
}

void KWRoute::InitializeStates(TimeVar *beginTime, char *statePath,
                               std::vector<float> *fastFlow,
                               std::vector<float> *slowFlow) {
  DatedName timeStr;
  timeStr.SetNameStr("YYYYMMDD_HHUU");
  timeStr.ProcessNameLoose(NULL);
  timeStr.UpdateName(beginTime->GetTM());

  char buffer[255];
  for (int p = 0; p < STATE_KW_QTY; p++) {
    sprintf(buffer, "%s/kwr_%s_%s.tif", statePath, stateStrings[p],
            timeStr.GetName());

    FloatGrid *sGrid = ReadFloatTifGrid(buffer);
    if (sGrid) {
      printf("Using Kinematic Wave Routing %s State Grid %s\n", stateStrings[p],
             buffer);
      if (g_DEM->IsSpatialMatch(sGrid)) {
        for (size_t i = 0; i < nodes->size(); i++) {
          GridNode *node = &nodes->at(i);
          KWGridNode *cNode = &(kwNodes[i]);
          if (sGrid->data[node->y][node->x] != sGrid->noData) {
            cNode->states[p] = sGrid->data[node->y][node->x];
          }
        }
      } else {
        GridLoc pt;
        for (size_t i = 0; i < nodes->size(); i++) {
          GridNode *node = &(nodes->at(i));
          KWGridNode *cNode = &(kwNodes[i]);
          if (sGrid->GetGridLoc(node->refLoc.x, node->refLoc.y, &pt) &&
              sGrid->data[pt.y][pt.x] != sGrid->noData) {
            cNode->states[p] = sGrid->data[pt.y][pt.x];
          }
        }
      }
      delete sGrid;
    } else {
      printf("Kinematic Wave Routing %s State Grid %s not found!\n",
             stateStrings[p], buffer);
    }
  }

fastFlowPtr = fastFlow->data();
assert(gridSize == fastFlow->size());
#pragma acc enter data copyin(fastFlowPtr[0:gridSize])

slowFlowPtr = slowFlow->data();
assert(gridSize == slowFlow->size());
#pragma acc enter data copyin(slowFlowPtr[0:gridSize])
}

void KWRoute::SaveStates(TimeVar *currentTime, char *statePath,
                         GridWriterFull *gridWriter) {
  DatedName timeStr;
  timeStr.SetNameStr("YYYYMMDD_HHUU");
  timeStr.ProcessNameLoose(NULL);
  timeStr.UpdateName(currentTime->GetTM());

  std::vector<float> dataVals;
  dataVals.resize(nodes->size());

  char buffer[255];
  for (int p = 0; p < STATE_KW_QTY; p++) {
    sprintf(buffer, "%s/kwr_%s_%s.tif", statePath, stateStrings[p],
            timeStr.GetName());
    for (size_t i = 0; i < nodes->size(); i++) {
      KWGridNode *cNode = &(kwNodes[i]);
      dataVals[i] = cNode->states[p];
    }
    gridWriter->WriteGrid(nodes, &dataVals, buffer, false);
  }
}

bool KWRoute::Route(float stepHours, std::vector<float> *fastFlow,
                    std::vector<float> *slowFlow,
                    std::vector<float> *discharge) {

  if (!initialized) {
    initialized = true;
    InitializeRouting(stepHours * 3600.0f);
  }

  size_t numNodes = nodes->size();

  dischargePtr = discharge->data();

  for(unsigned int lvl = 0; lvl < levels.size() - 1; lvl++) {
    const auto lvlstart = levels[lvl];
    const auto lvlend = levels[lvl + 1];
#pragma acc parallel loop independent async(1) default(present)
    for(unsigned int o = lvlstart; o < lvlend; o++) {
      const auto c = orderingPtr[o];
      RouteInt(stepHours * 3600.0f, &nodesPtr[c], &kwNodesPtr[c], fastFlowPtr[c], slowFlowPtr[c]);
      // const auto c = ordering.at(o);
      // RouteInt(stepHours * 3600.0f, &nodes->at(c), &kwNodes.at(c), fastFlow->at(c), slowFlow->at(c));
    }
  }
#pragma acc wait(1)

#pragma acc parallel loop independent default(present)
  for (size_t i = 0; i < numNodes; i++)
  {
    KWGridNode *cNode = &(kwNodesPtr[i]);
    slowFlowPtr[i] = 0.0; // cNode->incomingWater[KW_LAYER_INTERFLOW];
    fastFlowPtr[i] = 0.0; // cNode->incomingWater[KW_LAYER_FASTFLOW];
    cNode->incomingWaterOverland = 0.0;
    cNode->incomingWaterChannel = 0.0;
    if (!cNode->channelGridCell)
    {
      float q = cNode->incomingWater[KW_LAYER_FASTFLOW] * nodesPtr[i].horLen;
      q += (cNode->incomingWater[KW_LAYER_INTERFLOW] * nodesPtr[i].area / 3.6);
      dischargePtr[i]= q; // * (stepHours * 3600.0f);
    }
    else
    {
      dischargePtr[i] = cNode->incomingWater[KW_LAYER_FASTFLOW];
    }
    cNode->states[STATE_KW_IR] =
        cNode->states[STATE_KW_IR] + cNode->incomingWater[KW_LAYER_INTERFLOW];
    cNode->incomingWater[KW_LAYER_INTERFLOW] =
        0.0; // Zero here so we can save states
    cNode->incomingWater[KW_LAYER_FASTFLOW] =
        0.0; // Zero here so we can save states
    }
#pragma acc exit data copyout(nodesPtr[0:gridSize], kwNodesPtr[0:gridSize], fastFlowPtr[0:gridSize], slowFlowPtr[0:gridSize], dischargePtr[0:gridSize])
  // InitializeRouting(stepHours * 3600.0f);

  return true;
}

void KWRoute::RouteInt(float stepSeconds, GridNode *node, KWGridNode *cNode,
                       float fastFlow, float slowFlow) {
  if (!cNode->channelGridCell) {
    
    float beta = 0.6;
    float alpha = cNode->params[PARAM_KINEMATIC_ALPHA0];

    fastFlow /= 1000.0;          // mm to m
    float newInWater = fastFlow; // / node->horLen;

    float A, B, C, D, E;
    float backDiffq = 0.0;
    if (cNode->incomingWaterOverland + cNode->states[STATE_KW_PQ] > 0.0) {
      backDiffq =
          pow((cNode->incomingWaterOverland + cNode->states[STATE_KW_PQ]) / 2.0,
              beta - 1.0);
      if (!myisfinite(backDiffq)) {
        backDiffq = 0.0;
      }
    }
    A = (stepSeconds / node->horLen) * cNode->incomingWaterOverland;
    B = alpha * beta * cNode->states[STATE_KW_PQ] * backDiffq;
    C = stepSeconds * newInWater;
    D = stepSeconds / node->horLen;
    E = alpha * beta * backDiffq;
    float estq = (A + B + C) / (D + E); // cms/m
    float rhs = A + alpha * pow(cNode->states[STATE_KW_PQ], beta) +
                stepSeconds * newInWater;
    
    for (int itr = 0; itr < 10; itr++) {
      float resError =
          (stepSeconds / node->horLen) * estq + alpha * pow(estq, beta) - rhs;
      if (!myisfinite(resError)) {
        resError = 0.0;
      }
      if (fabsf(resError) < 0.01) {
        break;
      }
      float resErrorD1 =
          (stepSeconds / node->horLen) + alpha * beta * pow(estq, beta - 1.0);
      if (!myisfinite(resErrorD1)) {
        resErrorD1 = 1.0;
      }
      estq = estq - resError / resErrorD1;
      if (estq < 0) {
        estq = 0.0;
      }
    }
    if (estq < 0.0) {
      estq = 0.0;
    }

    float newq = estq;

    cNode->states[STATE_KW_PQ] = newq;
    if (node->downStreamNode != INVALID_DOWNSTREAM_NODE) {
      int index = nodesPtr[node->downStreamNode].modelIndex;
      //#pragma acc atomic update
 	kwNodesPtr[index].incomingWaterOverland += newq;
     }

    cNode->incomingWater[KW_LAYER_FASTFLOW] = newq;
    // Add Interflow Excess Water to Reservoir
    cNode->states[STATE_KW_IR] += slowFlow;
    double interflowLeak =
        cNode->states[STATE_KW_IR] * cNode->params[PARAM_KINEMATIC_LEAKI];
    // printf(" %f ", interflowLeak);
    cNode->states[STATE_KW_IR] -= interflowLeak;
    if (cNode->states[STATE_KW_IR] < 0) {
      cNode->states[STATE_KW_IR] = 0;
    }

    if (cNode->routeCNode[0][KW_LAYER_INTERFLOW]) {
      double interflowLeak0 =
          interflowLeak * cNode->routeAmount[0][KW_LAYER_INTERFLOW] *
          node->area / cNode->routeNode[0][KW_LAYER_INTERFLOW]->area;
      double leakAmount = interflowLeak0;
      // if (cNode->routeNode[0][KW_LAYER_INTERFLOW]->channelGridCell) {
      // printf(" 0 got %f ",
      // cNode->routeCNode[0][KW_LAYER_INTERFLOW]->incomingWater[KW_LAYER_INTERFLOW]);
      // }
      cNode->routeCNode[0][KW_LAYER_INTERFLOW]
          ->incomingWater[KW_LAYER_INTERFLOW] += leakAmount;
      // *res += leakAmount; // Make this an atomic add for parallelization
    }

    if (cNode->routeCNode[1][KW_LAYER_INTERFLOW]) {
      double interflowLeak1 =
          interflowLeak * cNode->routeAmount[1][KW_LAYER_INTERFLOW] *
          node->area / cNode->routeNode[1][KW_LAYER_INTERFLOW]->area;
      double leakAmount = interflowLeak1;
      // printf(" 1 got %f ", leakAmount);
      double *res = &(cNode->routeCNode[1][KW_LAYER_INTERFLOW]
                          ->incomingWater[KW_LAYER_INTERFLOW]);
      *res += leakAmount; // Make this an atomic add for parallelization
    }
  } else {
    // First do overland routing

    float beta = 0.6;
    float alpha = cNode->params[PARAM_KINEMATIC_ALPHA0];
    slowFlow += cNode->incomingWater[KW_LAYER_INTERFLOW];
    // printf(" %f ", cNode->incomingWater[KW_LAYER_INTERFLOW]);
    fastFlow /= 1000.0; // mm to m
    slowFlow /= 1000.0; // mm to m
    float newInWater = (fastFlow + slowFlow);

    float A, B, C, D, E;
    float backDiffq = 0.0;
    if (cNode->incomingWaterOverland + cNode->states[STATE_KW_PO] > 0.0) {
      backDiffq =
          pow((cNode->incomingWaterOverland + cNode->states[STATE_KW_PO]) / 2.0,
              beta - 1.0);
      if (!myisfinite(backDiffq)) {
        backDiffq = 0.0;
      }
    }
    A = (stepSeconds / node->horLen) * cNode->incomingWaterOverland;
    B = alpha * beta * cNode->states[STATE_KW_PO] * backDiffq;
    C = stepSeconds * newInWater;
    D = stepSeconds / node->horLen;
    E = alpha * beta * backDiffq;
    float estq = (A + B + C) / (D + E); // cms/m
    float rhs = A + alpha * pow(cNode->states[STATE_KW_PO], beta) +
                stepSeconds * newInWater;
    for (int itr = 0; itr < 10; itr++) {
      float resError =
          (stepSeconds / node->horLen) * estq + alpha * pow(estq, beta) - rhs;
      if (!myisfinite(resError)) {
        resError = 0.0;
      }
      if (fabsf(resError) < 0.01) {
        break;
      }
      float resErrorD1 =
          (stepSeconds / node->horLen) + alpha * beta * pow(estq, beta - 1.0);
      if (!myisfinite(resErrorD1)) {
        resErrorD1 = 1.0;
      }
      estq = estq - resError / resErrorD1;
      if (estq < 0) {
        estq = 0.0;
      }
    }
    if (estq < 0) {
      estq = 0.0;
    }
    float newq = estq;

    cNode->states[STATE_KW_PO] = newq;

    // Here we compute channel routing
    beta = cNode->params[PARAM_KINEMATIC_BETA];
    alpha = cNode->params[PARAM_KINEMATIC_ALPHA];

    // Channel Flow
    // Compute Q at current grid point
    float backDiffQ = 0.0;
    if (cNode->incomingWaterChannel + cNode->states[STATE_KW_PQ] > 0.0) {
      backDiffQ =
          pow((cNode->incomingWaterChannel + cNode->states[STATE_KW_PQ]) / 2.0,
              beta - 1.0);
      if (!myisfinite(backDiffQ)) {
        backDiffQ = 0.0;
      }
    }

    A = (stepSeconds / node->horLen) * cNode->incomingWaterChannel;
    B = alpha * beta * cNode->states[STATE_KW_PQ] * backDiffQ;
    C = stepSeconds * newq;
    D = stepSeconds / node->horLen;
    E = alpha * beta * backDiffQ;
    float estQ = (A + B + C) / (D + E); // cms
    rhs =
        A + alpha * pow(cNode->states[STATE_KW_PQ], beta) + stepSeconds * newq;
    for (int itr = 0; itr < 10; itr++) {
      float resError =
          (stepSeconds / node->horLen) * estQ + alpha * pow(estQ, beta) - rhs;
      if (!myisfinite(resError)) {
        resError = 0.0;
      }
      if (fabsf(resError) < 0.01) {
        break;
      }
      float resErrorD1 =
          (stepSeconds / node->horLen) + alpha * beta * pow(estQ, beta - 1.0);
      if (!myisfinite(resErrorD1)) {
        resErrorD1 = 1.0;
      }
      estQ = estQ - resError / resErrorD1;
      if (estQ < 0) {
        estQ = 0.0;
      }
    }
    if (estQ < 0) {
      estQ = 0.0;
    }
    float newWater = estQ;
    // if (newWater != newWater) {
    // printf("New water is %f (%f, %f) %f %f [%f %f %f %f %f] %f %f\n", newWater,
    // cNode->incomingWaterChannel, cNode->states[STATE_KW_PQ], newq,
    // cNode->incomingWaterOverland, A, B, C, D, E, alpha, 0.0);
    // }
    cNode->states[STATE_KW_PQ] =
        newWater; // Update previous Q for further routing if "steps" > 1
    if (node->downStreamNode != INVALID_DOWNSTREAM_NODE) {
      int index = nodesPtr[node->downStreamNode].modelIndex;
      //#pragma acc atomic update
 	kwNodesPtr[index].incomingWaterChannel += newWater;
     }

    cNode->incomingWater[KW_LAYER_FASTFLOW] = newWater;
    cNode->incomingWater[KW_LAYER_INTERFLOW] = 0.0;
  }
}

void KWRoute::InitializeParameters(
    std::map<GaugeConfigSection *, float *> *paramSettings,
    std::vector<FloatGrid *> *paramGrids) {

  // This pass distributes parameters
  size_t numNodes = nodes->size();
  size_t unused = 0;
  for (size_t i = 0; i < numNodes; i++) {
    GridNode *node = &nodes->at(i);
    KWGridNode *cNode = &(kwNodes[i]);
    if (!node->gauge) {
      unused++;
      continue;
    }
    // Copy all of the parameters over
    memcpy(cNode->params, (*paramSettings)[node->gauge],
           sizeof(float) * PARAM_KINEMATIC_QTY);

    if (!paramGrids->at(PARAM_KINEMATIC_ISU)) {
      cNode->states[STATE_KW_IR] = cNode->params[PARAM_KINEMATIC_ISU];
    }
    cNode->incomingWater[KW_LAYER_INTERFLOW] = 0.0;
    cNode->incomingWater[KW_LAYER_FASTFLOW] = 0.0;

    // Deal with the distributed parameters here
    GridLoc pt;
    for (size_t paramI = 0; paramI < PARAM_KINEMATIC_QTY; paramI++) {
      FloatGrid *grid = paramGrids->at(paramI);
      if (grid && g_DEM->IsSpatialMatch(grid)) {
        if (grid->data[node->y][node->x] == 0) {
          grid->data[node->y][node->x] = 0.01;
        }
        cNode->params[paramI] *= grid->data[node->y][node->x];
      } else if (grid &&
                 grid->GetGridLoc(node->refLoc.x, node->refLoc.y, &pt)) {
        if (grid->data[pt.y][pt.x] == 0) {
          grid->data[pt.y][pt.x] = 0.01;
          // printf("Using nodata value in param %s\n",
          // modelParamStrings[MODEL_CREST][paramI]);
        }
        cNode->params[paramI] *= grid->data[pt.y][pt.x];
      }
    }

    if (cNode->params[PARAM_KINEMATIC_LEAKI] < 0.0) {
      // printf("Node Leak Interflow(%f) is less than 0, setting to 0.\n",
      // cNode->params[PARAM_KINEMATIC_LEAKI]);
      cNode->params[PARAM_KINEMATIC_LEAKI] = 0.0;
    } else if (cNode->params[PARAM_KINEMATIC_LEAKI] > 1.0) {
      // printf("Node Leak Interflow(%f) is greater than 1, setting to 1.\n",
      // cNode->params[PARAM_KINEMATIC_LEAKI]);
      cNode->params[PARAM_KINEMATIC_LEAKI] = 1.0;
    }

    if (cNode->params[PARAM_KINEMATIC_ALPHA] < 0.0) {
      // printf("Node Alpha(%f) is less than 0, setting to 1.\n",
      // cNode->params[PARAM_KINEMATIC_ALPHA]);
      cNode->params[PARAM_KINEMATIC_ALPHA] = 1.0;
    }

    if (cNode->params[PARAM_KINEMATIC_ALPHA0] < 0.0) {
      // printf("Node Alpha0(%f) is less than 0, setting to 1.\n",
      // cNode->params[PARAM_KINEMATIC_ALPHA0]);
      cNode->params[PARAM_KINEMATIC_ALPHA0] = 1.0;
    }

    if (cNode->params[PARAM_KINEMATIC_BETA] < 0.0) {
      // printf("Node Beta(%f) is less than 0, setting to 0.6.\n",
      // cNode->params[PARAM_KINEMATIC_BETA]);
      cNode->params[PARAM_KINEMATIC_BETA] = 0.6;
    }

    if (node->fac > cNode->params[PARAM_KINEMATIC_TH]) {
      node->channelGridCell = true;
      cNode->channelGridCell = true;
    } else {
      node->channelGridCell = false;
      cNode->channelGridCell = false;
    }
  }
}

void KWRoute::InitializeRouting(float timeSeconds) {

  // This pass distributes parameters & calculates the time it takes for water
  // to cross the grid cell.
  size_t numNodes = nodes->size();
  for (size_t i = 0; i < numNodes; i++) {
    GridNode *node = &nodes->at(i);
    KWGridNode *cNode = &(kwNodes[i]);

    // Calculate the water speed for interflow
    float speedUnder = cNode->params[PARAM_KINEMATIC_UNDER] * cNode->slopeSqrt;

    float nexTimeUnder = node->horLen / speedUnder;
    cNode->nexTime[KW_LAYER_INTERFLOW] = nexTimeUnder;
  }

  // This pass figures out which cell water is routed to
  for (size_t i = 0; i < numNodes; i++) {
    GridNode *currentNode, *previousNode;
    float currentSeconds, previousSeconds;
    GridNode *node = &nodes->at(i);
    KWGridNode *cNode = &(kwNodes[i]);

    // Interflow routing
    previousSeconds = 0;
    currentSeconds = 0;
    currentNode = node;
    previousNode = NULL;
    while (currentSeconds < timeSeconds && currentNode &&
           !kwNodes[currentNode->modelIndex].channelGridCell) {
      if (currentNode) {
        previousSeconds = currentSeconds;
        previousNode = currentNode;
        currentSeconds +=
            kwNodes[currentNode->modelIndex].nexTime[KW_LAYER_INTERFLOW];
        if (currentNode->downStreamNode != INVALID_DOWNSTREAM_NODE) {
          currentNode = &(nodes->at(currentNode->downStreamNode));
        } else {
          currentNode = NULL;
        }
      } else {
        if (timeSeconds > currentSeconds) {
          previousNode = NULL;
        }
        break; // We have effectively run out of nodes to transverse, this is
               // done!
      }
    }

    cNode->routeNode[0][KW_LAYER_INTERFLOW] = currentNode;
    cNode->routeCNode[0][KW_LAYER_INTERFLOW] =
        (currentNode) ? &(kwNodes[currentNode->modelIndex]) : NULL;
    cNode->routeNode[1][KW_LAYER_INTERFLOW] = previousNode;
    cNode->routeCNode[1][KW_LAYER_INTERFLOW] =
        (previousNode) ? &(kwNodes[previousNode->modelIndex]) : NULL;
    if (currentNode && !kwNodes[currentNode->modelIndex].channelGridCell) {
      if ((currentSeconds - previousSeconds) > 0) {
        cNode->routeAmount[0][KW_LAYER_INTERFLOW] =
            (timeSeconds - previousSeconds) /
            (currentSeconds - previousSeconds);
        cNode->routeAmount[1][KW_LAYER_INTERFLOW] =
            1.0 - cNode->routeAmount[0][KW_LAYER_INTERFLOW];
      }
    } else {
      cNode->routeAmount[0][KW_LAYER_INTERFLOW] = 1.0;
      cNode->routeAmount[1][KW_LAYER_INTERFLOW] = 0.0;
    }
  }
}
