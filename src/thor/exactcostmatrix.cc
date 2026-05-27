#include "thor/exactcostmatrix.h"
#include "baldr/datetime.h"
#include "midgard/logging.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

using namespace valhalla::baldr;
using namespace valhalla::sif;

namespace {

constexpr float kStaleQueueTolerance = 0.001f;
constexpr uint32_t kMaxExactMatrixIterations = 20000000;
const std::string kExactCostMatrixName = "exactcostmatrix";

} // namespace

namespace valhalla {
namespace thor {

ExactCostMatrix::ExactCostMatrix(const boost::property_tree::ptree& config)
    : TimeDistanceMatrix(config) {
}

const std::string& ExactCostMatrix::name() {
  return kExactCostMatrixName;
}

bool ExactCostMatrix::SourceToTarget(Api& request,
                                     GraphReader& graphreader,
                                     const mode_costing_t& mode_costing,
                                     const travel_mode_t mode,
                                     const float max_matrix_distance) {
  request.mutable_matrix()->set_algorithm(Matrix::CostMatrix);

  if (request.options().shape_format() != no_shape) {
    add_warning(request, 207);
  }

  mode_ = mode;
  costing_ = mode_costing[static_cast<uint32_t>(mode_)];
  max_expansion_distance_ = request.options().expansion_max_distance();

  return ComputeExactMatrix<ExpansionType::forward>(request, graphreader, max_matrix_distance);
}

template <const ExpansionType expansion_direction, const bool FORWARD>
bool ExactCostMatrix::ComputeExactMatrix(Api& request,
                                         GraphReader& graphreader,
                                         const float max_matrix_distance) {
  const bool invariant = request.options().date_time_type() == Options::invariant;
  auto& origins = *request.mutable_options()->mutable_sources();
  auto& destinations = *request.mutable_options()->mutable_targets();
  const auto num_elements = static_cast<size_t>(origins.size()) * destinations.size();
  auto time_infos = SetTime(origins, graphreader);

  InitDestinations<expansion_direction>(graphreader, destinations);
  reserve_pbf_arrays(*request.mutable_matrix(), num_elements, request.options().verbose(),
                     costing_->pass(), request.options().include_route_edge_ids());

  bool connection_failed = false;
  for (int origin_index = 0; origin_index < origins.size(); ++origin_index) {
    edgelabels_.reserve(max_reserved_labels_count_);
    auto& origin = origins.Get(origin_index);
    const auto& time_info = time_infos[origin_index];
    current_cost_threshold_ = std::numeric_limits<float>::max();
    const auto max_path_distance = max_matrix_distance > 0
                                       ? static_cast<uint32_t>(max_matrix_distance)
                                       : std::numeric_limits<uint32_t>::max();

    settled_count_ = 0;
    SetDestinationEdges();
    std::vector<ExactDestinationPath> paths(destinations_.size());

    ExactQueue queue;
    uint64_t sequence = 0;
    SetOriginExact<expansion_direction>(graphreader, origin, time_info, queue, sequence);

    uint32_t iterations = 0;
    while (!queue.empty()) {
      const auto item = queue.top();
      queue.pop();
      if (item.label_index >= edgelabels_.size()) {
        continue;
      }

      const auto& current_label = edgelabels_[item.label_index];
      if (std::fabs(item.cost - current_label.sortcost()) > kStaleQueueTolerance) {
        continue;
      }

      auto pred = current_label;
      if (pred.cost().cost > current_cost_threshold_) {
        break;
      }

      graph_tile_ptr pred_tile = graphreader.GetGraphTile(pred.edgeid());
      if (pred_tile == nullptr) {
        continue;
      }

      if (!pred.origin()) {
        auto status = edgestatus_.Get(pred.edgeid());
        if (status.set() == EdgeSet::kPermanent && status.index() != item.label_index) {
          continue;
        }
        if (status.set() == EdgeSet::kPermanent) {
          continue;
        }
        edgestatus_.Update(pred.edgeid(), EdgeSet::kPermanent);
      }

      auto destedge = dest_edges_.find(pred.edgeid());
      if (destedge != dest_edges_.end()) {
        const DirectedEdge* edge = pred_tile->directededge(pred.edgeid());
        if (UpdateDestinationsExact<expansion_direction>(origin, destinations, destedge->second, edge,
                                                         pred_tile, graphreader, pred, item.label_index,
                                                         time_info, paths)) {
          break;
        }
      }

      ExpandExact<expansion_direction>(graphreader, pred.endnode(), pred, item.label_index, false,
                                       time_info, invariant, max_path_distance, queue, sequence);

      if (interrupt_ && (iterations++ % kInterruptIterationsInterval) == 0) {
        (*interrupt_)();
      }
      if (iterations >= kMaxExactMatrixIterations) {
        throw valhalla_exception_t{430};
      }
    }

    FormExactMatrix(request, graphreader, static_cast<uint32_t>(origin_index), paths);
    for (const auto& dest : destinations_) {
      if (dest.best_cost.cost == kMaxCost) {
        connection_failed = true;
        break;
      }
    }
    reset();
  }

  return !connection_failed;
}

template <const ExpansionType expansion_direction, const bool FORWARD>
void ExactCostMatrix::SetOriginExact(GraphReader& graphreader,
                                     const Location& origin,
                                     const TimeInfo& time_info,
                                     ExactQueue& queue,
                                     uint64_t& sequence) {
  bool has_other_edges = false;
  std::for_each(origin.correlation().edges().begin(), origin.correlation().edges().end(),
                [&has_other_edges](const valhalla::PathEdge& e) {
                  has_other_edges = has_other_edges || (FORWARD ? !e.end_node() : !e.begin_node());
                });

  for (const auto& edge : origin.correlation().edges()) {
    if ((FORWARD ? edge.end_node() : edge.begin_node()) && has_other_edges) {
      continue;
    }

    GraphId edgeid(edge.graph_id());
    if (FORWARD ? costing_->AvoidAsOriginEdge(edgeid, edge.percent_along())
                : costing_->AvoidAsDestinationEdge(edgeid, edge.percent_along())) {
      continue;
    }

    graph_tile_ptr tile = graphreader.GetGraphTile(edgeid);
    if (tile == nullptr) {
      continue;
    }
    const DirectedEdge* directededge = tile->directededge(edgeid);

    graph_tile_ptr endtile = graphreader.GetGraphTile(directededge->endnode());
    if (endtile == nullptr) {
      continue;
    }

    uint8_t flow_sources;
    Cost cost;
    float dist;
    GraphId opp_edge_id;
    const DirectedEdge* opp_dir_edge = nullptr;
    if (FORWARD) {
      const auto percent_along = 1.0f - edge.percent_along();
      cost = costing_->PartialEdgeCost(directededge, edgeid, tile, time_info, flow_sources,
                                       edge.percent_along(), 1.0f);
      dist = static_cast<uint32_t>(directededge->length() * percent_along);
    } else {
      opp_edge_id = graphreader.GetOpposingEdgeId(edgeid);
      if (!opp_edge_id.is_valid()) {
        continue;
      }
      opp_dir_edge = graphreader.GetOpposingEdge(edgeid);
      cost = costing_->PartialEdgeCost(opp_dir_edge, opp_edge_id, endtile, time_info, flow_sources,
                                       0.0f, edge.percent_along());
      dist = static_cast<uint32_t>(directededge->length() * edge.percent_along());
    }

    cost.cost += edge.distance();

    auto destonly_restriction_mask =
        costing_->GetExemptedAccessRestrictions(directededge, tile, edgeid);

    if (FORWARD) {
      edgelabels_.emplace_back(kInvalidLabel, edgeid, directededge, cost, cost.cost, mode_, dist,
                               kInvalidRestriction, !costing_->IsClosed(directededge, tile),
                               static_cast<bool>(flow_sources & kDefaultFlowMask),
                               InternalTurn::kNoTurn, 0,
                               directededge->destonly() ||
                                   (costing_->is_hgv() && directededge->destonly_hgv()),
                               directededge->forwardaccess() & kTruckAccess,
                               destonly_restriction_mask);
    } else {
      edgelabels_.emplace_back(kInvalidLabel, opp_edge_id, opp_dir_edge, cost, cost.cost, mode_, dist,
                               kInvalidRestriction, !costing_->IsClosed(directededge, tile),
                               static_cast<bool>(flow_sources & kDefaultFlowMask),
                               InternalTurn::kNoTurn, 0,
                               directededge->destonly() ||
                                   (costing_->is_hgv() && directededge->destonly_hgv()),
                               directededge->forwardaccess() & kTruckAccess,
                               destonly_restriction_mask);
    }

    edgelabels_.back().set_origin();
    queue.push({edgelabels_.back().cost().cost, static_cast<uint32_t>(edgelabels_.size() - 1),
                sequence++});
  }
}

template <const ExpansionType expansion_direction, const bool FORWARD>
void ExactCostMatrix::ExpandExact(GraphReader& graphreader,
                                  const GraphId& node,
                                  const EdgeLabel& pred,
                                  const uint32_t pred_idx,
                                  const bool from_transition,
                                  const TimeInfo& time_info,
                                  const bool invariant,
                                  const uint32_t max_path_distance,
                                  ExactQueue& queue,
                                  uint64_t& sequence) {
  graph_tile_ptr tile = graphreader.GetGraphTile(node);
  if (tile == nullptr) {
    return;
  }
  const NodeInfo* nodeinfo = tile->node(node);
  if (!costing_->Allowed(nodeinfo)) {
    return;
  }

  auto offset_time = from_transition
                         ? time_info
                         : (FORWARD ? time_info.forward(invariant ? 0.f : pred.cost().secs,
                                                        static_cast<int>(nodeinfo->timezone()))
                                    : time_info.reverse(invariant ? 0.f : pred.cost().secs,
                                                        static_cast<int>(nodeinfo->timezone())));

  const DirectedEdge* opp_pred_edge = nullptr;
  if (!FORWARD) {
    opp_pred_edge = tile->directededge(nodeinfo->edge_index());
    for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, opp_pred_edge++) {
      if (opp_pred_edge->localedgeidx() == pred.opp_local_idx()) {
        break;
      }
    }
  }

  GraphId edgeid(node.tileid(), node.level(), nodeinfo->edge_index());
  EdgeStatusInfo* es = edgestatus_.GetPtr(edgeid, tile);
  const DirectedEdge* directededge = tile->directededge(nodeinfo->edge_index());
  for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++, ++edgeid, ++es) {
    if (directededge->is_shortcut() || es->set() == EdgeSet::kPermanent) {
      continue;
    }

    graph_tile_ptr t2 = nullptr;
    GraphId opp_edge_id;
    const DirectedEdge* opp_edge = nullptr;
    if (!FORWARD) {
      t2 = directededge->leaves_tile() ? graphreader.GetGraphTile(directededge->endnode()) : tile;
      if (t2 == nullptr) {
        continue;
      }
      opp_edge_id = t2->GetOpposingEdgeId(directededge);
      opp_edge = t2->directededge(opp_edge_id);
    }

    uint8_t restriction_idx = kInvalidRestriction;
    uint8_t destonly_restriction_mask = pred.destonly_access_restr_mask();
    const bool is_dest = dest_edges_.find(edgeid) != dest_edges_.cend();
    if (FORWARD) {
      if (!costing_->Allowed(directededge, is_dest, pred, tile, edgeid, offset_time.local_time,
                             nodeinfo->timezone(), restriction_idx, destonly_restriction_mask) ||
          costing_->Restricted(directededge, pred, edgelabels_, tile, edgeid, true, nullptr,
                               offset_time.local_time, nodeinfo->timezone())) {
        continue;
      }
    } else {
      if (!costing_->AllowedReverse(directededge, pred, opp_edge, t2, opp_edge_id,
                                    offset_time.local_time, nodeinfo->timezone(), restriction_idx,
                                    destonly_restriction_mask) ||
          (costing_->Restricted(directededge, pred, edgelabels_, tile, edgeid, false, nullptr,
                                offset_time.local_time, nodeinfo->timezone()))) {
        continue;
      }
    }

    uint8_t flow_sources;
    auto newcost = FORWARD ? costing_->EdgeCost(directededge, edgeid, tile, offset_time, flow_sources)
                           : costing_->EdgeCost(opp_edge, opp_edge_id, t2, offset_time, flow_sources);
    auto reader_getter = [&graphreader]() { return baldr::LimitedGraphReader(graphreader); };
    auto transition_cost =
        FORWARD ? costing_->TransitionCost(directededge, nodeinfo, pred, tile, reader_getter)
                : costing_->TransitionCostReverse(directededge->localedgeidx(), nodeinfo, opp_edge,
                                                  opp_pred_edge, t2, pred.edgeid(), reader_getter,
                                                  static_cast<bool>(flow_sources & kDefaultFlowMask),
                                                  pred.internal_turn());
    newcost += pred.cost() + transition_cost;
    uint32_t path_distance = pred.path_distance() + directededge->length();
    if (path_distance > max_path_distance) {
      continue;
    }
    if (max_expansion_distance_ > 0 && path_distance > max_expansion_distance_) {
      continue;
    }

    if (es->set() == EdgeSet::kTemporary) {
      auto& lab = edgelabels_[es->index()];
      if (newcost.cost < lab.cost().cost) {
        lab.Update(pred_idx, newcost, newcost.cost, path_distance, restriction_idx);
        queue.push({newcost.cost, es->index(), sequence++});
      }
      continue;
    }

    uint32_t idx = edgelabels_.size();
    if (FORWARD) {
      edgelabels_.emplace_back(pred_idx, edgeid, directededge, newcost, newcost.cost, mode_,
                               path_distance, restriction_idx,
                               (pred.closure_pruning() || !(costing_->IsClosed(directededge, tile))),
                               0 != (flow_sources & kDefaultFlowMask),
                               costing_->TurnType(pred.opp_local_idx(), nodeinfo, directededge), 0,
                               directededge->destonly() ||
                                   (costing_->is_hgv() && directededge->destonly_hgv()),
                               directededge->forwardaccess() & kTruckAccess,
                               destonly_restriction_mask);
    } else {
      edgelabels_.emplace_back(pred_idx, edgeid, directededge, newcost, newcost.cost, mode_,
                               path_distance, restriction_idx,
                               (pred.closure_pruning() || !(costing_->IsClosed(opp_edge, t2))),
                               0 != (flow_sources & kDefaultFlowMask),
                               costing_->TurnType(directededge->localedgeidx(), nodeinfo, opp_edge,
                                                  opp_pred_edge),
                               0,
                               opp_edge->destonly() ||
                                   (costing_->is_hgv() && opp_edge->destonly_hgv()),
                               opp_edge->forwardaccess() & kTruckAccess, destonly_restriction_mask);
    }

    *es = {EdgeSet::kTemporary, idx};
    queue.push({newcost.cost, idx, sequence++});
  }

  if (!from_transition && nodeinfo->transition_count() > 0) {
    const NodeTransition* trans = tile->transition(nodeinfo->transition_index());
    for (uint32_t i = 0; i < nodeinfo->transition_count(); ++i, ++trans) {
      ExpandExact<expansion_direction>(graphreader, trans->endnode(), pred, pred_idx, true,
                                       offset_time, invariant, max_path_distance, queue, sequence);
    }
  }
}

template <const ExpansionType expansion_direction, const bool FORWARD>
bool ExactCostMatrix::UpdateDestinationsExact(
    const Location& origin,
    const google::protobuf::RepeatedPtrField<Location>& locations,
    std::vector<uint32_t>& destination_indexes,
    const DirectedEdge* edge,
    const graph_tile_ptr& tile,
    GraphReader& reader,
    const EdgeLabel& pred,
    const uint32_t pred_idx,
    const TimeInfo& time_info,
    std::vector<ExactDestinationPath>& paths) {
  for (auto dest_idx : destination_indexes) {
    Destination& dest = destinations_[dest_idx];
    auto& dest_loc = locations.Get(dest_idx);

    if (dest.settled) {
      continue;
    }

    auto dest_available = dest.dest_edges_available.find(pred.edgeid());
    if (dest_available == dest.dest_edges_available.end()) {
      if (!IsTrivial(pred.edgeid(), origin, locations.Get(dest_idx))) {
        LOG_ERROR("Could not find the destination edge");
      }
      continue;
    }

    auto settle_dest = [&]() {
      dest.dest_edges_available.erase(dest_available);
      if (dest.dest_edges_available.empty()) {
        dest.settled = true;
        settled_count_++;
      }
    };

    if (origin.ll().lat() == dest_loc.ll().lat() && origin.ll().lng() == dest_loc.ll().lng()) {
      dest.best_cost = Cost{0.f, 0.f};
      dest.distance = 0;
      paths[dest_idx] = {true, kInvalidLabel};
      settle_dest();
      continue;
    }

    auto dest_edge = dest.dest_edges_percent_along.find(pred.edgeid());
    if (pred.predecessor() == kInvalidLabel && !IsTrivial(pred.edgeid(), origin, dest_loc)) {
      continue;
    }

    uint8_t flow_sources;
    float remainder = dest_edge->second;
    auto opp_edge_id = reader.GetOpposingEdgeId(pred.edgeid());
    auto opp_tile = reader.GetGraphTile(opp_edge_id);
    if (opp_tile == nullptr) {
      continue;
    }
    auto begin_node = reader.GetBeginNodeId(edge, opp_tile);
    uint64_t timezone_index = opp_tile->node(begin_node)->timezone();

    auto secs = pred.predecessor() == kInvalidLabel ? 0.f : edgelabels_[pred.predecessor()].cost().secs;
    auto offset_time =
        FORWARD ? time_info.forward(secs, timezone_index) : time_info.reverse(secs, timezone_index);
    Cost newcost =
        pred.cost() -
        (costing_->EdgeCost(edge, pred.edgeid(), tile, offset_time, flow_sources) * remainder);
    if (newcost.cost < dest.best_cost.cost) {
      dest.best_cost = newcost;
      dest.distance = pred.path_distance() - (edge->length() * remainder);
      paths[dest_idx] = {true, pred_idx};
    }

    settle_dest();
  }

  return settled_count_ == destinations_.size();
}

std::vector<GraphId> ExactCostMatrix::RecoverPathEdges(GraphReader& reader,
                                                       const ExactDestinationPath& path) const {
  std::vector<GraphId> path_edges;
  if (!path.found || path.label_index == kInvalidLabel) {
    return path_edges;
  }

  for (auto edgelabel_index = path.label_index; edgelabel_index != kInvalidLabel;
       edgelabel_index = edgelabels_[edgelabel_index].predecessor()) {
    const EdgeLabel& edgelabel = edgelabels_[edgelabel_index];
    graph_tile_ptr tile;
    const DirectedEdge* edge = reader.directededge(edgelabel.edgeid(), tile);
    if (edge == nullptr) {
      throw tile_gone_error_t("ExactCostMatrix::RecoverPathEdges failed", edgelabel.edgeid());
    }

    if (edge->is_shortcut()) {
      auto superseded = reader.RecoverShortcut(edgelabel.edgeid());
      std::move(superseded.rbegin(), superseded.rend(), std::back_inserter(path_edges));
    } else {
      path_edges.push_back(edgelabel.edgeid());
    }
  }

  std::reverse(path_edges.begin(), path_edges.end());
  return path_edges;
}

void ExactCostMatrix::FormExactMatrix(Api& request,
                                      GraphReader& reader,
                                      uint32_t origin_index,
                                      const std::vector<ExactDestinationPath>& paths) {
  valhalla::Matrix& matrix = *request.mutable_matrix();
  for (uint32_t target_index = 0; target_index < destinations_.size(); ++target_index) {
    const auto connection_idx = origin_index * request.options().targets().size() + target_index;
    const auto& dest = destinations_[target_index];
    matrix.mutable_from_indices()->Set(connection_idx, origin_index);
    matrix.mutable_to_indices()->Set(connection_idx, target_index);

    if (dest.best_cost.cost == kMaxCost) {
      matrix.mutable_second_pass()->Set(connection_idx, true);
      matrix.mutable_distances()->Set(connection_idx, static_cast<uint32_t>(kMaxCost));
      matrix.mutable_times()->Set(connection_idx, kMaxCost);
      continue;
    }

    matrix.mutable_distances()->Set(connection_idx, dest.distance);
    matrix.mutable_times()->Set(connection_idx, dest.best_cost.secs);

    if (request.options().include_route_edge_ids()) {
      auto* edge_ids = matrix.mutable_edge_ids(connection_idx);
      edge_ids->clear_edge_id();
      const auto path_edges = RecoverPathEdges(reader, paths[target_index]);
      edge_ids->mutable_edge_id()->Reserve(path_edges.size());
      for (const auto& path_edge : path_edges) {
        edge_ids->add_edge_id(path_edge.value);
      }
    }
  }
}

template bool ExactCostMatrix::ComputeExactMatrix<ExpansionType::forward, true>(
    Api& request,
    GraphReader& graphreader,
    const float max_matrix_distance);
template void ExactCostMatrix::SetOriginExact<ExpansionType::forward, true>(
    GraphReader& graphreader,
    const Location& origin,
    const TimeInfo& time_info,
    ExactQueue& queue,
    uint64_t& sequence);
template void ExactCostMatrix::ExpandExact<ExpansionType::forward, true>(
    GraphReader& graphreader,
    const GraphId& node,
    const EdgeLabel& pred,
    const uint32_t pred_idx,
    const bool from_transition,
    const TimeInfo& time_info,
    const bool invariant,
    const uint32_t max_path_distance,
    ExactQueue& queue,
    uint64_t& sequence);
template bool ExactCostMatrix::UpdateDestinationsExact<ExpansionType::forward, true>(
    const Location& origin,
    const google::protobuf::RepeatedPtrField<Location>& locations,
    std::vector<uint32_t>& destination_indexes,
    const DirectedEdge* edge,
    const graph_tile_ptr& tile,
    GraphReader& reader,
    const EdgeLabel& pred,
    const uint32_t pred_idx,
    const TimeInfo& time_info,
    std::vector<ExactDestinationPath>& paths);

} // namespace thor
} // namespace valhalla
