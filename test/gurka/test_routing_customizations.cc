#include "baldr/rapidjson_utils.h"
#include "gurka.h"
#include "test.h"

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
                          bool include_route_edge_ids = false) {
  auto request_json =
      gurka::detail::build_valhalla_request({"locations"},
                                            {gurka::detail::to_lls(map.nodes, locations)});
  rapidjson::Document doc;
  doc.Parse(request_json.c_str());
  add_edge_whitelist(doc, edge_whitelist);
  if (include_route_edge_ids) {
    doc.AddMember(rapidjson::StringRef("include_route_edge_ids"), true, doc.GetAllocator());
  }
  return serialize(doc);
}

std::string matrix_request(const gurka::map& map,
                           const std::vector<std::string>& sources,
                           const std::vector<std::string>& targets,
                           const std::vector<baldr::GraphId>& edge_whitelist) {
  auto request_json =
      gurka::detail::build_valhalla_request({"sources", "targets"},
                                            {gurka::detail::to_lls(map.nodes, sources),
                                             gurka::detail::to_lls(map.nodes, targets)});
  rapidjson::Document doc;
  doc.Parse(request_json.c_str());
  add_edge_whitelist(doc, edge_whitelist);
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
  gurka::do_action(Options::sources_to_targets, map, matrix_request(map, {"A", "D"}, {"C"}, {ab, bc}),
                   {}, &response);

  rapidjson::Document doc;
  doc.Parse(response.c_str());
  ASSERT_FALSE(doc.HasParseError());

  const auto rows = doc["sources_to_targets"].GetArray();
  EXPECT_FALSE(rows[0].GetArray()[0].GetObject()["time"].IsNull());
  EXPECT_TRUE(rows[1].GetArray()[0].GetObject()["time"].IsNull());
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
