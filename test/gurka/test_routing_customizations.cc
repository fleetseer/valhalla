#include "baldr/rapidjson_utils.h"
#include "gurka.h"
#include "test.h"
#include "tyr/serializers.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace valhalla;

namespace {

std::string serialize(rapidjson::Document& doc) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  return buffer.GetString();
}

void add_edge_whitelist(rapidjson::Document& doc, const std::vector<baldr::GraphId>& edge_whitelist) {
  auto& allocator = doc.GetAllocator();
  rapidjson::Value whitelist(rapidjson::kArrayType);
  for (const auto& edge_id : edge_whitelist) {
    rapidjson::Value value;
    value.SetUint64(edge_id.value);
    whitelist.PushBack(value, allocator);
  }
  doc.AddMember(rapidjson::StringRef("edge_whitelist"), whitelist, allocator);
}

std::string route_request(const gurka::map& map,
                          const std::vector<std::string>& locations,
                          const std::vector<baldr::GraphId>& edge_whitelist,
                          bool include_route_edge_ids = false,
                          const char* format = nullptr) {
  auto request_json =
      gurka::detail::build_valhalla_request({"locations"},
                                            {gurka::detail::to_lls(map.nodes, locations)});
  rapidjson::Document doc;
  doc.Parse(request_json.c_str());
  add_edge_whitelist(doc, edge_whitelist);
  if (include_route_edge_ids) {
    doc.AddMember(rapidjson::StringRef("include_route_edge_ids"), true, doc.GetAllocator());
  }
  if (format) {
    doc.AddMember(rapidjson::StringRef("format"),
                  rapidjson::Value().SetString(format, doc.GetAllocator()), doc.GetAllocator());
  }
  return serialize(doc);
}

std::string matrix_request(const gurka::map& map,
                           const std::vector<std::string>& sources,
                           const std::vector<std::string>& targets,
                           const std::vector<baldr::GraphId>& edge_whitelist,
                           bool include_route_edge_ids = false,
                           const char* format = nullptr,
                           bool verbose = true) {
  auto request_json =
      gurka::detail::build_valhalla_request({"sources", "targets"},
                                            {gurka::detail::to_lls(map.nodes, sources),
                                             gurka::detail::to_lls(map.nodes, targets)});
  rapidjson::Document doc;
  doc.Parse(request_json.c_str());
  doc["verbose"].SetBool(verbose);
  add_edge_whitelist(doc, edge_whitelist);
  if (include_route_edge_ids) {
    doc.AddMember(rapidjson::StringRef("include_route_edge_ids"), true, doc.GetAllocator());
  }
  if (format) {
    doc.AddMember(rapidjson::StringRef("format"),
                  rapidjson::Value().SetString(format, doc.GetAllocator()), doc.GetAllocator());
  }
  return serialize(doc);
}

} // namespace

class RoutingCustomizations : public ::testing::Test {
protected:
  static gurka::map map;

  static void SetUpTestSuite() {
    const std::string ascii_map = R"(
      A----B----C----D
    )";

    const gurka::ways ways = {
        {"AB", {{"highway", "primary"}}},
        {"BC", {{"highway", "primary"}}},
        {"CD", {{"highway", "primary"}}},
    };

    const auto layout = gurka::detail::map_to_coordinates(ascii_map, 100);
    map =
        gurka::buildtiles(layout, ways, {}, {}, VALHALLA_BUILD_DIR "test/data/routing_customizations",
                          {{"mjolnir.concurrency", "1"}, {"mjolnir.shortcuts", "0"}});
  }

  static baldr::GraphId edge_id(const std::string& begin, const std::string& end) {
    auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
    return std::get<0>(gurka::findEdgeByNodes(*reader, map.nodes, begin, end));
  }
};

gurka::map RoutingCustomizations::map = {};

TEST_F(RoutingCustomizations, WhitelistAllowsRoute) {
  const auto ab = edge_id("A", "B");
  const auto bc = edge_id("B", "C");

  auto result = gurka::do_action(Options::route, map, route_request(map, {"A", "C"}, {ab, bc}));

  gurka::assert::raw::expect_path(result, {"AB", "BC"});
}

TEST_F(RoutingCustomizations, WhitelistRejectsRouteWhenRequiredEdgeIsMissing) {
  const auto ab = edge_id("A", "B");

  EXPECT_THROW(gurka::do_action(Options::route, map, route_request(map, {"A", "C"}, {ab})),
               std::runtime_error);
}

TEST_F(RoutingCustomizations, MatrixReturnsNullForPairsClosedByWhitelist) {
  const auto ab = edge_id("A", "B");
  const auto bc = edge_id("B", "C");

  std::string response;
  auto result = gurka::do_action(Options::sources_to_targets, map,
                                 matrix_request(map, {"A", "D"}, {"C"}, {ab, bc}), {}, &response);
  EXPECT_EQ(result.matrix().edge_ids_size(), 0);

  rapidjson::Document doc;
  doc.Parse(response.c_str());
  ASSERT_FALSE(doc.HasParseError());

  const auto rows = doc["sources_to_targets"].GetArray();
  EXPECT_FALSE(rows[0].GetArray()[0].GetObject()["time"].IsNull());
  EXPECT_TRUE(rows[1].GetArray()[0].GetObject()["time"].IsNull());
  EXPECT_FALSE(rows[0].GetArray()[0].GetObject().HasMember("edge_ids"));
}

TEST_F(RoutingCustomizations, MatrixEdgeIdsMatchRouteEdgeIdsWhenRequested) {
  const auto ab = edge_id("A", "B");
  const auto bc = edge_id("B", "C");

  std::string matrix_response;
  auto matrix_result =
      gurka::do_action(Options::sources_to_targets, map,
                       matrix_request(map, {"A"}, {"C"}, {ab, bc}, true), {}, &matrix_response);

  ASSERT_EQ(matrix_result.matrix().edge_ids_size(), 1);
  const auto& matrix_edge_ids = matrix_result.matrix().edge_ids(0);
  ASSERT_EQ(matrix_edge_ids.edge_id_size(), 2);
  EXPECT_EQ(matrix_edge_ids.edge_id(0), ab.value);
  EXPECT_EQ(matrix_edge_ids.edge_id(1), bc.value);

  auto route_result =
      gurka::do_action(Options::route, map, route_request(map, {"A", "C"}, {ab, bc}, true));
  const auto& route_leg = route_result.directions().routes(0).legs(0);
  ASSERT_EQ(route_leg.edge_id_size(), matrix_edge_ids.edge_id_size());
  for (int i = 0; i < route_leg.edge_id_size(); ++i) {
    EXPECT_EQ(matrix_edge_ids.edge_id(i), route_leg.edge_id(i));
  }

  rapidjson::Document doc;
  doc.Parse(matrix_response.c_str());
  ASSERT_FALSE(doc.HasParseError());
  const auto& json_edge_ids =
      doc["sources_to_targets"].GetArray()[0].GetArray()[0].GetObject()["edge_ids"];
  ASSERT_TRUE(json_edge_ids.IsArray());
  ASSERT_EQ(json_edge_ids.Size(), 2);
  EXPECT_EQ(json_edge_ids[0].GetUint64(), ab.value);
  EXPECT_EQ(json_edge_ids[1].GetUint64(), bc.value);

  std::string slim_matrix_response;
  gurka::do_action(Options::sources_to_targets, map,
                   matrix_request(map, {"A"}, {"C"}, {ab, bc}, true, nullptr, false), {},
                   &slim_matrix_response);
  doc.Parse(slim_matrix_response.c_str());
  ASSERT_FALSE(doc.HasParseError());
  const auto& slim_edge_ids =
      doc["sources_to_targets"].GetObject()["edge_ids"].GetArray()[0].GetArray()[0].GetArray();
  ASSERT_EQ(slim_edge_ids.Size(), 2);
  EXPECT_EQ(slim_edge_ids[0].GetUint64(), ab.value);
  EXPECT_EQ(slim_edge_ids[1].GetUint64(), bc.value);

  std::string pbf_bytes;
  gurka::do_action(Options::sources_to_targets, map,
                   matrix_request(map, {"A"}, {"C"}, {ab, bc}, true, "pbf"), {}, &pbf_bytes);

  Api pbf_response;
  ASSERT_TRUE(pbf_response.ParseFromString(pbf_bytes));
  ASSERT_EQ(pbf_response.matrix().edge_ids_size(), 1);
  ASSERT_EQ(pbf_response.matrix().edge_ids(0).edge_id_size(), 2);
  EXPECT_EQ(pbf_response.matrix().edge_ids(0).edge_id(0), ab.value);
  EXPECT_EQ(pbf_response.matrix().edge_ids(0).edge_id(1), bc.value);
}

TEST_F(RoutingCustomizations, MatrixEdgeIdsAreEmptyForClosedWhitelist) {
  std::string response;
  auto result = gurka::do_action(Options::sources_to_targets, map,
                                 matrix_request(map, {"A"}, {"C"}, {}, true), {}, &response);

  ASSERT_EQ(result.matrix().edge_ids_size(), 1);
  EXPECT_EQ(result.matrix().edge_ids(0).edge_id_size(), 0);

  rapidjson::Document doc;
  doc.Parse(response.c_str());
  ASSERT_FALSE(doc.HasParseError());

  const auto& cell = doc["sources_to_targets"].GetArray()[0].GetArray()[0].GetObject();
  EXPECT_TRUE(cell["time"].IsNull());
  ASSERT_TRUE(cell["edge_ids"].IsArray());
  EXPECT_EQ(cell["edge_ids"].Size(), 0);
}

TEST_F(RoutingCustomizations, RouteEdgeIdsAreReturnedInLegOrderWhenRequested) {
  const auto ab = edge_id("A", "B");
  const auto bc = edge_id("B", "C");

  auto result = gurka::do_action(Options::route, map, route_request(map, {"A", "C"}, {ab, bc}, true));

  const auto& leg = result.directions().routes(0).legs(0);
  ASSERT_EQ(leg.edge_id_size(), 2);
  EXPECT_EQ(leg.edge_id(0), ab.value);
  EXPECT_EQ(leg.edge_id(1), bc.value);

  auto json = gurka::convert_to_json(result, Options_Format_json);
  ASSERT_FALSE(json.HasParseError());
  const auto& edge_ids = json["trip"]["legs"].GetArray()[0].GetObject()["edge_ids"];
  ASSERT_TRUE(edge_ids.IsArray());
  ASSERT_EQ(edge_ids.Size(), 2);
  EXPECT_EQ(edge_ids[0].GetUint64(), ab.value);
  EXPECT_EQ(edge_ids[1].GetUint64(), bc.value);
}

TEST_F(RoutingCustomizations, PbfRouteEdgeIdsAreReturnedInLegOrderWhenRequested) {
  const auto ab = edge_id("A", "B");
  const auto bc = edge_id("B", "C");

  std::string pbf_bytes;
  gurka::do_action(Options::route, map, route_request(map, {"A", "C"}, {ab, bc}, true, "pbf"), {},
                   &pbf_bytes);

  Api pbf_response;
  ASSERT_TRUE(pbf_response.ParseFromString(pbf_bytes));
  const auto& directions_leg = pbf_response.directions().routes(0).legs(0);
  ASSERT_EQ(directions_leg.edge_id_size(), 2);
  EXPECT_EQ(directions_leg.edge_id(0), ab.value);
  EXPECT_EQ(directions_leg.edge_id(1), bc.value);

  auto result = gurka::do_action(Options::route, map, route_request(map, {"A", "C"}, {ab, bc}, true));
  result.mutable_options()->set_format(Options::pbf);
  result.mutable_options()->mutable_pbf_field_selector()->set_trip(true);
  pbf_bytes = tyr::serializePbf(result);

  pbf_response.Clear();
  ASSERT_TRUE(pbf_response.ParseFromString(pbf_bytes));
  ASSERT_TRUE(pbf_response.has_trip());
  ASSERT_FALSE(pbf_response.has_directions());
  const auto& trip_leg = pbf_response.trip().routes(0).legs(0);
  ASSERT_EQ(trip_leg.edge_id_size(), 2);
  EXPECT_EQ(trip_leg.edge_id(0), ab.value);
  EXPECT_EQ(trip_leg.edge_id(1), bc.value);
}
