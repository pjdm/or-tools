// Copyright 2010-2018 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/sat/diffn.h"

#include <algorithm>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "ortools/base/map_util.h"
#include "ortools/sat/sat_solver.h"
#include "ortools/util/sort.h"

namespace operations_research {
namespace sat {

namespace {

// Returns true iff the 2 given intervals are disjoint. If their union is one
// point, this also returns true.
bool IntervalAreDisjointForSure(IntegerValue min_a, IntegerValue max_a,
                                IntegerValue min_b, IntegerValue max_b) {
  return min_a >= max_b || min_b >= max_a;
}

// Returns the distance from interval a to the "bounding interval" of a and b.
IntegerValue DistanceToBoundingInterval(IntegerValue min_a, IntegerValue max_a,
                                        IntegerValue min_b,
                                        IntegerValue max_b) {
  return std::max(min_a - min_b, max_b - max_a);
}

}  // namespace

  NonOverlappingRectanglesPropagator::NonOverlappingRectanglesPropagator(
    const std::vector<IntegerVariable>& start_x,
    const std::vector<IntegerVariable>& size_x,
    const std::vector<IntegerVariable>& end_x,
    const std::vector<IntegerVariable>& start_y,
    const std::vector<IntegerVariable>& size_y,
    const std::vector<IntegerVariable>& end_y, bool strict,
    IntegerTrail* integer_trail)
    : CpPropagator(integer_trail),
      start_x_(start_x),
      size_x_(size_x),
      end_x_(end_x),
      start_y_(start_y),
      size_y_(size_y),
      end_y_(end_y),
      strict_(strict) {
  const int n = start_x.size();
  CHECK_GT(n, 0);
  neighbors_.resize(n * (n - 1));
  neighbors_begins_.resize(n);
  neighbors_ends_.resize(n);
  for (int i = 0; i < n; ++i) {
    fixed_x_.push_back(Min(size_x_[i]) == Max(size_x_[i]));
    fixed_y_.push_back(Min(size_y_[i]) == Max(size_y_[i]));
    const int begin = i * (n - 1);
    neighbors_begins_[i] = begin;
    neighbors_ends_[i] = begin + (n - 1);
    for (int j = 0; j < n; ++j) {
      if (j == i) continue;
      neighbors_[begin + (j > i ? j - 1 : j)] = j;
    }
  }
}

NonOverlappingRectanglesPropagator::~NonOverlappingRectanglesPropagator() {}

bool NonOverlappingRectanglesPropagator::Propagate() {
  const int n = start_x_.size();
  cached_areas_.resize(n);
  for (int box = 0; box < n; ++box) {
    // We never change the min-size of a box, so this stays valid.
    cached_areas_[box] = Min(size_x_[box]) * Min(size_y_[box]);
  }

  while (true) {
    const int64 saved_stamp = integer_trail_->num_enqueues();
    for (int box = 0; box < n; ++box) {
      if (!strict_ && cached_areas_[box] == 0) continue;

      UpdateNeighbors(box);
      if (!FailWhenEnergyIsTooLarge(box)) return false;

      const int end = neighbors_ends_[box];
      for (int i = neighbors_begins_[box]; i < end; ++i) {
        if (!PushOneBox(box, neighbors_[i])) return false;
      }
    }
    if (saved_stamp == integer_trail_->num_enqueues()) break;
  }

  return CheckEnergyOnProjections();
}

bool NonOverlappingRectanglesPropagator::CheckEnergyOnProjections() {
  if (!CheckEnergyOnOneDimension(start_x_, size_x_, end_x_, start_y_, size_y_,
                                 end_y_)) {
    return false;
  }
  return CheckEnergyOnOneDimension(start_y_, size_y_, end_y_, start_x_, size_x_,
                                   end_x_);
}

bool NonOverlappingRectanglesPropagator::CheckEnergyOnOneDimension(
    const std::vector<IntegerVariable>& starts,
    const std::vector<IntegerVariable>& sizes,
    const std::vector<IntegerVariable>& ends,
    const std::vector<IntegerVariable>& other_starts,
    const std::vector<IntegerVariable>& other_sizes,
    const std::vector<IntegerVariable>& other_ends) {
  const int n = starts.size();
  absl::flat_hash_map<int64, std::vector<int>> mandatory_boxes;
  std::set<int64> events;
  for (int box = 0; box < n; ++box) {
    const int64 start_max = Max(starts[box]).value();
    const int64 end_min = Min(ends[box]).value();

    if (start_max < end_min) {
      for (int64 t = start_max; t < end_min; ++t) {
        mandatory_boxes[t].push_back(box);
      }
      events.insert(start_max);
      events.insert(end_min);
    }
  }

  const auto box_min = [&](int b) { return Min(other_starts[b]).value(); };
  const auto box_max = [&](int b) { return Max(other_ends[b]).value(); };

  absl::flat_hash_set<std::vector<int>> box_combinations;
  for (const auto& it : mandatory_boxes) {
    if (it.second.size() <= 2) continue;
    box_combinations.insert(it.second);
  }

  for (const auto& comb_it : box_combinations) {
    std::vector<int> boxes = comb_it;
    if (boxes.size() <= 2) continue;

    for (int direction = 0; direction <= 1; ++direction) {
      int64 min_hull = kint64max;
      int64 max_hull = kint64min;
      int64 min_energy = 0;
      if (direction) {
        std::sort(
            boxes.begin(), boxes.end(), [&box_min, &box_max](int b1, int b2) {
              return box_min(b1) < box_min(b2) ||
                     (box_min(b1) == box_min(b2) && box_max(b1) < box_max(b2));
            });
      } else {
        std::sort(
            boxes.begin(), boxes.end(), [&box_min, &box_max](int b1, int b2) {
              return box_max(b1) > box_max(b2) ||
                     (box_max(b1) == box_max(b2) && box_min(b1) > box_min(b2));
            });
      }
      int last_restart = 0;
      bool restart = true;
      while (restart) {
        restart = false;
        for (int b1 = last_restart; b1 < boxes.size(); ++b1) {
          const int box = boxes[b1];
          const int64 new_min = box_min(box);
          const int64 new_max = box_max(box);
          const int64 min_size = Min(other_sizes[box]).value();

          if (new_min > max_hull || new_max < min_hull) {
            // Disjoint from previous cluster: reset computation.
            min_hull = new_min;
            max_hull = new_max;
            min_energy = min_size;
            last_restart = b1;
          } else {
            min_hull = std::min(min_hull, new_min);
            max_hull = std::max(max_hull, new_max);
            min_energy += min_size;
          }

          if (min_energy > max_hull - min_hull) {
            integer_reason_.clear();
            for (int b2 = 0; b2 <= b1; ++b2) {
              AddBoxReason(boxes[b2]);
            }
            return integer_trail_->ReportConflict(integer_reason_);
          }
        }
      }
    }
  }
  return true;
}

void NonOverlappingRectanglesPropagator::RegisterWith(
    GenericLiteralWatcher* watcher) {
  const int id = watcher->Register(this);
  for (int i = 0; i < start_x_.size(); ++i) {
    watcher->WatchIntegerVariable(start_x_[i], id);
    if (!fixed_x_[i]) {
      watcher->WatchLowerBound(size_x_[i], id);
      watcher->WatchIntegerVariable(end_x_[i], id);
    }

    watcher->WatchIntegerVariable(start_y_[i], id);
    if (!fixed_y_[i]) {
      watcher->WatchLowerBound(size_y_[i], id);
      watcher->WatchIntegerVariable(end_y_[i], id);
    }
    watcher->RegisterReversibleInt(id, &neighbors_ends_[i]);
  }
}

void NonOverlappingRectanglesPropagator::AddBoxReason(int box) {
  AddBoundsReason(start_x_[box], &integer_reason_);
  if (!fixed_x_[box]) {
    AddLowerBoundReason(size_x_[box], &integer_reason_);
    AddBoundsReason(end_x_[box], &integer_reason_);
  }

  AddBoundsReason(start_y_[box], &integer_reason_);
  if (!fixed_y_[box]) {
    AddLowerBoundReason(size_y_[box], &integer_reason_);
    AddBoundsReason(end_y_[box], &integer_reason_);
  }
}

void NonOverlappingRectanglesPropagator::AddBoxInRectangleReason(
    int box, IntegerValue xmin, IntegerValue xmax, IntegerValue ymin,
    IntegerValue ymax) {
  integer_reason_.push_back(
      IntegerLiteral::GreaterOrEqual(start_x_[box], xmin));
  if (fixed_x_[box]) {
    integer_reason_.push_back(
        IntegerLiteral::LowerOrEqual(start_x_[box], xmax - Min(size_x_[box])));
  } else {
    integer_reason_.push_back(IntegerLiteral::LowerOrEqual(end_x_[box], xmax));
    AddLowerBoundReason(size_x_[box], &integer_reason_);
  }

  integer_reason_.push_back(
      IntegerLiteral::GreaterOrEqual(start_y_[box], ymin));
  if (fixed_y_[box]) {
    integer_reason_.push_back(
        IntegerLiteral::LowerOrEqual(start_y_[box], ymax - Min(size_y_[box])));
  } else {
    integer_reason_.push_back(IntegerLiteral::LowerOrEqual(end_y_[box], ymax));
    AddLowerBoundReason(size_y_[box], &integer_reason_);
  }
}

void NonOverlappingRectanglesPropagator::UpdateNeighbors(int box) {
  tmp_removed_.clear();
  cached_distance_to_bounding_box_.resize(start_x_.size());
  const IntegerValue box_x_min = Min(start_x_[box]);
  const IntegerValue box_x_max = Max(end_x_[box]);
  const IntegerValue box_y_min = Min(start_y_[box]);
  const IntegerValue box_y_max = Max(end_y_[box]);
  int new_index = neighbors_begins_[box];
  const int end = neighbors_ends_[box];
  for (int i = new_index; i < end; ++i) {
    const int other = neighbors_[i];

    const IntegerValue other_x_min = Min(start_x_[other]);
    const IntegerValue other_x_max = Max(end_x_[other]);
    if (IntervalAreDisjointForSure(box_x_min, box_x_max, other_x_min,
                                   other_x_max)) {
      tmp_removed_.push_back(other);
      continue;
    }

    const IntegerValue other_y_min = Min(start_y_[other]);
    const IntegerValue other_y_max = Max(end_y_[other]);
    if (IntervalAreDisjointForSure(box_y_min, box_y_max, other_y_min,
                                   other_y_max)) {
      tmp_removed_.push_back(other);
      continue;
    }

    neighbors_[new_index++] = other;
    cached_distance_to_bounding_box_[other] =
        std::max(DistanceToBoundingInterval(box_x_min, box_x_max, other_x_min,
                                            other_x_max),
                 DistanceToBoundingInterval(box_y_min, box_y_max, other_y_min,
                                            other_y_max));
  }
  neighbors_ends_[box] = new_index;
  for (int i = 0; i < tmp_removed_.size();) {
    neighbors_[new_index++] = tmp_removed_[i++];
  }
  IncrementalSort(neighbors_.begin() + neighbors_begins_[box],
                  neighbors_.begin() + neighbors_ends_[box],
                  [this](int i, int j) {
                    return cached_distance_to_bounding_box_[i] <
                           cached_distance_to_bounding_box_[j];
                  });
}

bool NonOverlappingRectanglesPropagator::FailWhenEnergyIsTooLarge(int box) {
  // Note that we only consider the smallest dimension of each boxes here.
  IntegerValue area_min_x = Min(start_x_[box]);
  IntegerValue area_max_x = Max(end_x_[box]);
  IntegerValue area_min_y = Min(start_y_[box]);
  IntegerValue area_max_y = Max(end_y_[box]);
  IntegerValue sum_of_areas = cached_areas_[box];

  IntegerValue total_sum_of_areas = sum_of_areas;
  const int end = neighbors_ends_[box];
  for (int i = neighbors_begins_[box]; i < end; ++i) {
    const int other = neighbors_[i];
    total_sum_of_areas += cached_areas_[other];
  }

  // TODO(user): Is there a better order, maybe sort by distance
  // with the current box.
  for (int i = neighbors_begins_[box]; i < end; ++i) {
    const int other = neighbors_[i];
    if (cached_areas_[other] == 0) continue;

    // Update Bounding box.
    area_min_x = std::min(area_min_x, Min(start_x_[other]));
    area_max_x = std::max(area_max_x, Max(end_x_[other]));
    area_min_y = std::min(area_min_y, Min(start_y_[other]));
    area_max_y = std::max(area_max_y, Max(end_y_[other]));

    // Update sum of areas.
    sum_of_areas += cached_areas_[other];
    const IntegerValue bounding_area =
        (area_max_x - area_min_x) * (area_max_y - area_min_y);
    if (bounding_area >= total_sum_of_areas) {
      // Nothing will be deduced. Exiting.
      return true;
    }

    if (sum_of_areas > bounding_area) {
      integer_reason_.clear();
      AddBoxInRectangleReason(box, area_min_x, area_max_x, area_min_y,
                              area_max_y);
      for (int j = neighbors_begins_[box]; j <= i; ++j) {
        const int other = neighbors_[j];
        if (cached_areas_[other] == 0) continue;
        AddBoxInRectangleReason(other, area_min_x, area_max_x, area_min_y,
                                area_max_y);
      }
      return integer_trail_->ReportConflict(integer_reason_);
    }
  }
  return true;
}

bool NonOverlappingRectanglesPropagator::FirstBoxIsBeforeSecondBox(
    const std::vector<IntegerVariable>& starts,
    const std::vector<IntegerVariable>& sizes,
    const std::vector<IntegerVariable>& ends, const std::vector<bool>& fixed,
    int box, int other) {
  const IntegerValue min_end = Min(ends[box]);
  if (min_end > Min(starts[other])) {
    integer_reason_.clear();
    AddBoxReason(box);
    AddBoxReason(other);
    if (!SetMin(starts[other], min_end, integer_reason_)) return false;
  }

  const IntegerValue other_max_start = Max(starts[other]);
  if (other_max_start < Max(ends[box])) {
    integer_reason_.clear();
    AddBoxReason(box);
    AddBoxReason(other);
    if (fixed[box]) {
      if (!SetMax(starts[box], other_max_start - Min(sizes[box]),
                  integer_reason_)) {
        return false;
      }
    } else {
      if (!SetMax(ends[box], other_max_start, integer_reason_)) return false;
    }
  }
  return true;
}

bool NonOverlappingRectanglesPropagator::PushOneBox(int box, int other) {
  if (!strict_ && cached_areas_[other] == 0) return true;

  // For each direction and each order, we test if the boxes can be disjoint.
  const int state = (Min(end_x_[box]) <= Max(start_x_[other])) +
                    2 * (Min(end_x_[other]) <= Max(start_x_[box])) +
                    4 * (Min(end_y_[box]) <= Max(start_y_[other])) +
                    8 * (Min(end_y_[other]) <= Max(start_y_[box]));

  // This is an "hack" to be able to easily test for none or for one
  // and only one of the conditions below.
  integer_reason_.clear();
  switch (state) {
    case 0: {
      AddBoxReason(box);
      AddBoxReason(other);
      return integer_trail_->ReportConflict(integer_reason_);
    }
    case 1:
      return FirstBoxIsBeforeSecondBox(start_x_, size_x_, end_x_, fixed_x_, box,
                                       other);
    case 2:
      return FirstBoxIsBeforeSecondBox(start_x_, size_x_, end_x_, fixed_x_,
                                       other, box);
    case 4:
      return FirstBoxIsBeforeSecondBox(start_y_, size_y_, end_y_, fixed_y_, box,
                                       other);
    case 8:
      return FirstBoxIsBeforeSecondBox(start_y_, size_y_, end_y_, fixed_y_,
                                       other, box);
    default:
      break;
  }
  return true;
}



FixedDiff2::FixedDiff2(const std::vector<IntegerVariable>& start_x,
                       const std::vector<IntegerValue>& size_x,
                       const std::vector<IntegerVariable>& start_y,
                       const std::vector<IntegerValue>& size_y,
                       IntegerTrail* integer_trail)
    : CpPropagator(integer_trail),
      start_x_(start_x),
      size_x_(size_x),
      start_y_(start_y),
      size_y_(size_y) {
  const int n = start_x.size();
  CHECK_GT(n, 0);
  neighbors_.resize(n * (n - 1));
  neighbors_begins_.resize(n);
  neighbors_ends_.resize(n);
  for (int i = 0; i < n; ++i) {
    const int begin = i * (n - 1);
    neighbors_begins_[i] = begin;
    neighbors_ends_[i] = begin + (n - 1);
    for (int j = 0; j < n; ++j) {
      if (j == i) continue;
      neighbors_[begin + (j > i ? j - 1 : j)] = j;
    }
  }
}

FixedDiff2::~FixedDiff2() {}

bool FixedDiff2::Propagate() {
  const int n = start_x_.size();
  cached_areas_.resize(n);
  for (int box = 0; box < n; ++box) {
    // We never change the min-size of a box, so this stays valid.
    cached_areas_[box] = size_x_[box] * size_y_[box];
  }

  while (true) {
    const int64 saved_stamp = integer_trail_->num_enqueues();
    for (int box = 0; box < n; ++box) {

      UpdateNeighbors(box);
      if (!FailWhenEnergyIsTooLarge(box)) return false;

      const int end = neighbors_ends_[box];
      for (int i = neighbors_begins_[box]; i < end; ++i) {
        if (!PushOneBox(box, neighbors_[i])) return false;
      }
    }
    if (saved_stamp == integer_trail_->num_enqueues()) break;
  }

  return CheckEnergyOnProjections();
}

bool FixedDiff2::CheckEnergyOnProjections() {
  if (!CheckEnergyOnOneDimension(start_x_, size_x_,start_y_, size_y_)) {
    return false;
  }
  return CheckEnergyOnOneDimension(start_y_, size_y_, start_x_, size_x_);
}

bool FixedDiff2::CheckEnergyOnOneDimension(
    const std::vector<IntegerVariable>& starts,
    const std::vector<IntegerValue>& sizes,
    const std::vector<IntegerVariable>& other_starts,
    const std::vector<IntegerValue>& other_sizes) {
  const int n = starts.size();
  absl::flat_hash_map<int64, std::vector<int>> mandatory_boxes;
  std::set<int64> events;
  for (int box = 0; box < n; ++box) {
    const int64 start_max = Max(starts[box]).value();
    const int64 end_min = Min(starts[box]).value() + sizes[box].value();

    if (start_max < end_min) {
      for (int64 t = start_max; t < end_min; ++t) {
        mandatory_boxes[t].push_back(box);
      }
      events.insert(start_max);
      events.insert(end_min);
    }
  }

  const auto box_min = [&](int b) { return Min(other_starts[b]).value(); };
  const auto box_max = [&](int b) {
    return Max(other_starts[b]).value() + other_sizes[b].value();
  };

  absl::flat_hash_set<std::vector<int>> visited_combinations;

  for (const int64 e : events) {
    std::vector<int> boxes = mandatory_boxes[e];
    if (boxes.size() <= 2) continue;
    if (gtl::ContainsKey(visited_combinations, boxes)) continue;
    visited_combinations.insert(boxes);

    for (int direction = 0; direction <= 1; ++direction) {
      int64 min_hull = kint64max;
      int64 max_hull = kint64min;
      int64 min_energy = 0;
      if (direction) {
        std::sort(
            boxes.begin(), boxes.end(), [&box_min, &box_max](int b1, int b2) {
              return box_min(b1) < box_min(b2) ||
                     (box_min(b1) == box_min(b2) && box_max(b1) < box_max(b2));
            });
      } else {
        std::sort(
            boxes.begin(), boxes.end(), [&box_min, &box_max](int b1, int b2) {
              return box_max(b1) > box_max(b2) ||
                     (box_max(b1) == box_max(b2) && box_min(b1) > box_min(b2));
            });
      }
      int last_restart = 0;
      bool restart = true;
      while (restart) {
        restart = false;
        for (int b1 = last_restart; b1 < boxes.size(); ++b1) {
          const int box = boxes[b1];
          const int64 new_min = box_min(box);
          const int64 new_max = box_max(box);
          const int64 min_size = Min(other_sizes[box]).value();

          if (new_min > max_hull || new_max < min_hull) {
            // Disjoint from previous cluster: reset computation.
            min_hull = new_min;
            max_hull = new_max;
            min_energy = min_size;
            last_restart = b1;
          } else {
            min_hull = std::min(min_hull, new_min);
            max_hull = std::max(max_hull, new_max);
            min_energy += min_size;
          }

          if (min_energy > max_hull - min_hull) {
            integer_reason_.clear();
            for (int b2 = 0; b2 <= b1; ++b2) {
              AddBoxReason(boxes[b2]);
            }
            return integer_trail_->ReportConflict(integer_reason_);
          }
        }
      }
    }
  }
  return true;
}

void FixedDiff2::RegisterWith(
    GenericLiteralWatcher* watcher) {
  const int id = watcher->Register(this);
  for (int i = 0; i < start_x_.size(); ++i) {
    watcher->WatchIntegerVariable(start_x_[i], id);
    watcher->WatchIntegerVariable(start_y_[i], id);
    watcher->RegisterReversibleInt(id, &neighbors_ends_[i]);
  }
}

void FixedDiff2::AddBoxReason(int box) {
  AddBoundsReason(start_x_[box], &integer_reason_);
  AddBoundsReason(start_y_[box], &integer_reason_);
}

void FixedDiff2::AddBoxInRectangleReason(int box, IntegerValue xmin,
                                         IntegerValue xmax, IntegerValue ymin,
                                         IntegerValue ymax) {
  integer_reason_.push_back(
      IntegerLiteral::GreaterOrEqual(start_x_[box], xmin));
  integer_reason_.push_back(
      IntegerLiteral::LowerOrEqual(start_x_[box], xmax - Min(size_x_[box])));

  integer_reason_.push_back(
      IntegerLiteral::GreaterOrEqual(start_y_[box], ymin));
  integer_reason_.push_back(
      IntegerLiteral::LowerOrEqual(start_y_[box], ymax - Min(size_y_[box])));
}

void FixedDiff2::UpdateNeighbors(int box) {
  tmp_removed_.clear();
  cached_distance_to_bounding_box_.resize(start_x_.size());
  const IntegerValue box_x_min = Min(start_x_[box]);
  const IntegerValue box_x_max = Max(start_x_[box]) + size_x_[box];
  const IntegerValue box_y_min = Min(start_y_[box]);
  const IntegerValue box_y_max = Max(start_x_[box]) + size_y_[box];
  int new_index = neighbors_begins_[box];
  const int end = neighbors_ends_[box];
  for (int i = new_index; i < end; ++i) {
    const int other = neighbors_[i];

    const IntegerValue other_x_min = Min(start_x_[other]);
    const IntegerValue other_x_max = Max(start_x_[other]) + size_x_[box];
    if (IntervalAreDisjointForSure(box_x_min, box_x_max, other_x_min,
                                   other_x_max)) {
      tmp_removed_.push_back(other);
      continue;
    }

    const IntegerValue other_y_min = Min(start_y_[other]);
    const IntegerValue other_y_max = Max(start_y_[other]) + size_y_[box];
    if (IntervalAreDisjointForSure(box_y_min, box_y_max, other_y_min,
                                   other_y_max)) {
      tmp_removed_.push_back(other);
      continue;
    }

    neighbors_[new_index++] = other;
    cached_distance_to_bounding_box_[other] =
        std::max(DistanceToBoundingInterval(box_x_min, box_x_max, other_x_min,
                                            other_x_max),
                 DistanceToBoundingInterval(box_y_min, box_y_max, other_y_min,
                                            other_y_max));
  }
  neighbors_ends_[box] = new_index;
  for (int i = 0; i < tmp_removed_.size();) {
    neighbors_[new_index++] = tmp_removed_[i++];
  }
  IncrementalSort(neighbors_.begin() + neighbors_begins_[box],
                  neighbors_.begin() + neighbors_ends_[box],
                  [this](int i, int j) {
                    return cached_distance_to_bounding_box_[i] <
                           cached_distance_to_bounding_box_[j];
                  });
}

bool FixedDiff2::FailWhenEnergyIsTooLarge(int box) {
  // Note that we only consider the smallest dimension of each boxes here.
  IntegerValue area_min_x = Min(start_x_[box]);
  IntegerValue area_max_x = Max(start_x_[box]) + size_x_[box];
  IntegerValue area_min_y = Min(start_y_[box]);
  IntegerValue area_max_y = Max(start_y_[box]) + size_y_[box];
  IntegerValue sum_of_areas = cached_areas_[box];

  IntegerValue total_sum_of_areas = sum_of_areas;
  const int end = neighbors_ends_[box];
  for (int i = neighbors_begins_[box]; i < end; ++i) {
    const int other = neighbors_[i];
    total_sum_of_areas += cached_areas_[other];
  }

  // TODO(user): Is there a better order, maybe sort by distance
  // with the current box.
  for (int i = neighbors_begins_[box]; i < end; ++i) {
    const int other = neighbors_[i];
    if (cached_areas_[other] == 0) continue;

    // Update Bounding box.
    area_min_x = std::min(area_min_x, Min(start_x_[other]));
    area_max_x = std::max(area_max_x, Max(start_x_[other]) + size_x_[other]);
    area_min_y = std::min(area_min_y, Min(start_y_[other]));
    area_max_y = std::max(area_max_y, Max(start_y_[other]) + size_y_[other]);

    // Update sum of areas.
    sum_of_areas += cached_areas_[other];
    const IntegerValue bounding_area =
        (area_max_x - area_min_x) * (area_max_y - area_min_y);
    if (bounding_area >= total_sum_of_areas) {
      // Nothing will be deduced. Exiting.
      return true;
    }

    if (sum_of_areas > bounding_area) {
      integer_reason_.clear();
      AddBoxInRectangleReason(box, area_min_x, area_max_x, area_min_y,
                              area_max_y);
      for (int j = neighbors_begins_[box]; j <= i; ++j) {
        const int other = neighbors_[j];
        if (cached_areas_[other] == 0) continue;
        AddBoxInRectangleReason(other, area_min_x, area_max_x, area_min_y,
                                area_max_y);
      }
      return integer_trail_->ReportConflict(integer_reason_);
    }
  }
  return true;
}

bool FixedDiff2::FirstBoxIsBeforeSecondBox(
    const std::vector<IntegerVariable>& starts,
    const std::vector<IntegerValue>& sizes, int box, int other) {
  const IntegerValue min_end = Min(starts[box]) + sizes[box];
  if (min_end > Min(starts[other])) {
    integer_reason_.clear();
    AddBoxReason(box);
    AddBoxReason(other);
    if (!SetMin(starts[other], min_end, integer_reason_)) return false;
  }

  const IntegerValue other_max_start = Max(starts[other]) - sizes[box];
  if (other_max_start < Max(starts[box])) {
    integer_reason_.clear();
    AddBoxReason(box);
    AddBoxReason(other);
    if (!SetMax(starts[box], other_max_start, integer_reason_)) return false;
  }
  return true;
}

bool FixedDiff2::PushOneBox(int box, int other) {
  const auto min_end_x = [this](int box) {
    return Min(start_x_[box]) + size_x_[box];
  };
  const auto min_end_y = [this](int box) {
    return Min(start_y_[box]) + size_y_[box];
  };

  // For each direction and each order, we test if the boxes can be disjoint.
  const int state = (min_end_x(box) <= Max(start_x_[other])) +
                    2 * (min_end_x(other) <= Max(start_x_[box])) +
                    4 * (min_end_y(box) <= Max(start_y_[other])) +
                    8 * (min_end_y(box) <= Max(start_y_[box]));

  // This is an "hack" to be able to easily test for none or for one
  // and only one of the conditions below.
  integer_reason_.clear();
  switch (state) {
    case 0: {
      AddBoxReason(box);
      AddBoxReason(other);
      return integer_trail_->ReportConflict(integer_reason_);
    }
    case 1:
      return FirstBoxIsBeforeSecondBox(start_x_, size_x_, box, other);
    case 2:
      return FirstBoxIsBeforeSecondBox(start_x_, size_x_, other, box);
    case 4:
      return FirstBoxIsBeforeSecondBox(start_y_, size_y_, box, other);
    case 8:
      return FirstBoxIsBeforeSecondBox(start_y_, size_y_, other, box);
    default:
      break;
  }
  return true;
}

}  // namespace sat
}  // namespace operations_research

