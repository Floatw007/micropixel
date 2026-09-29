#include <assert.h>
#include <string.h>

#include "sdk/json_reader.hpp"

using micropixel::json::Reader;
using micropixel::json::Type;
using micropixel::json::Value;

int main() {
    const char payload[] =
        "{\"items\":[{\"name\":\"米饭\",\"percent\":42.50,\"live\":true},{\"name\":\"汤\",\"percent\":null}]}";
    Reader reader(payload, sizeof(payload) - 1U, {.max_depth = 6U, .max_items_per_container = 4U});
    Value root{}, items{}, first{}, name{}, percent{}, live{};
    uint16_t count = 0U;
    int64_t fixed = 0;
    bool flag = false;
    assert(reader.Parse(root) && root.type == Type::kObject);
    assert(reader.Find(root, "items", items) && reader.Count(items, count) && count == 2U);
    assert(reader.At(items, 0U, first));
    assert(reader.Find(first, "name", name) && Reader::String(name) == "米饭");
    assert(reader.Find(first, "percent", percent) && Reader::Fixed(percent, 2U, fixed) && fixed == 4250);
    assert(reader.Find(first, "live", live) && Reader::Boolean(live, flag) && flag);

    const char invalid_utf8[] = {'[', '"', static_cast<char>(0xc0), static_cast<char>(0x80), '"', ']'};
    Reader invalid(invalid_utf8, sizeof(invalid_utf8));
    assert(!invalid.Parse(root));
    const char too_deep[] = "[[[0]]]";
    Reader bounded(too_deep, sizeof(too_deep) - 1U, {.max_depth = 2U, .max_items_per_container = 4U});
    assert(!bounded.Parse(root));
    const char too_many[] = "[1,2,3]";
    Reader capped(too_many, sizeof(too_many) - 1U, {.max_depth = 2U, .max_items_per_container = 2U});
    assert(!capped.Parse(root));
    const char exponent[] = "1e2";
    Reader exponent_reader(exponent, sizeof(exponent) - 1U);
    assert(exponent_reader.Parse(root));
    assert(!Reader::Fixed(root, 2U, fixed));
    return 0;
}
