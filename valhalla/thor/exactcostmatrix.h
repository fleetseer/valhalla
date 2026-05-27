#ifndef VALHALLA_THOR_EXACTCOSTMATRIX_H_
#define VALHALLA_THOR_EXACTCOSTMATRIX_H_

#include <valhalla/thor/timedistancematrix.h>

#include <queue>

namespace valhalla {
namespace thor {

/**
 * Exact per-source matrix algorithm.
 *
 * This intentionally keeps the CostMatrix request/response surface, including
 * route-edge IDs, but uses a strict Dijkstra expansion for each source rather
 * than the shared many-to-many CostMatrix connection heuristic.
 */
class ExactCostMatrix : public TimeDistanceMatrix {
public:
  explicit ExactCostMatrix(const boost::property_tree::ptree& config = {});

  bool SourceToTarget(Api& request,
                      baldr::GraphReader& graphreader,
                      const sif::mode_costing_t& mode_costing,
                      const sif::travel_mode_t mode,
                      const float max_matrix_distance) override;

  const std::string& name() override;

protected:
  struct QueueItem {
    float cost;
    uint32_t label_index;
    uint64_t sequence;
  };

  struct QueueItemCompare {
    bool operator()(const QueueItem& lhs, const QueueItem& rhs) const {
      if (lhs.cost == rhs.cost) {
        return lhs.sequence > rhs.sequence;
      }
      return lhs.cost > rhs.cost;
    }
  };

  using ExactQueue = std::priority_queue<QueueItem, std::vector<QueueItem>, QueueItemCompare>;

  struct ExactDestinationPath {
    bool found = false;
    uint32_t label_index = baldr::kInvalidLabel;
  };

  template <const ExpansionType expansion_direction,
            const bool FORWARD = expansion_direction == ExpansionType::forward>
  bool ComputeExactMatrix(Api& request, baldr::GraphReader& graphreader, const float max_matrix_distance);

  template <const ExpansionType expansion_direction,
            const bool FORWARD = expansion_direction == ExpansionType::forward>
  void SetOriginExact(baldr::GraphReader& graphreader,
                      const valhalla::Location& origin,
                      const baldr::TimeInfo& time_info,
                      ExactQueue& queue,
                      uint64_t& sequence);

  template <const ExpansionType expansion_direction,
            const bool FORWARD = expansion_direction == ExpansionType::forward>
  void ExpandExact(baldr::GraphReader& graphreader,
                   const baldr::GraphId& node,
                   const sif::EdgeLabel& pred,
                   const uint32_t pred_idx,
                   const bool from_transition,
                   const baldr::TimeInfo& time_info,
                   const bool invariant,
                   const uint32_t max_path_distance,
                   ExactQueue& queue,
                   uint64_t& sequence);

  template <const ExpansionType expansion_direction,
            const bool FORWARD = expansion_direction == ExpansionType::forward>
  bool UpdateDestinationsExact(const valhalla::Location& origin,
                               const google::protobuf::RepeatedPtrField<valhalla::Location>& locations,
                               std::vector<uint32_t>& destination_indexes,
                               const baldr::DirectedEdge* edge,
                               const baldr::graph_tile_ptr& tile,
                               baldr::GraphReader& reader,
                               const sif::EdgeLabel& pred,
                               const uint32_t pred_idx,
                               const baldr::TimeInfo& time_info,
                               std::vector<ExactDestinationPath>& paths);

  void FormExactMatrix(Api& request,
                       baldr::GraphReader& reader,
                       uint32_t origin_index,
                       const std::vector<ExactDestinationPath>& paths);

  std::vector<baldr::GraphId> RecoverPathEdges(baldr::GraphReader& reader,
                                               const ExactDestinationPath& path) const;
};

} // namespace thor
} // namespace valhalla

#endif // VALHALLA_THOR_EXACTCOSTMATRIX_H_
