#include <assert.h>
#include <string>

#include "runtime/abi/network_policy.hpp"

int main() {
    using micropixel::runtime::ValidIdempotencyKey;
    using micropixel::runtime::ValidRelativeNetworkPath;
    assert(ValidRelativeNetworkPath("/api/merchant/orders?limit=32"));
    assert(ValidRelativeNetworkPath("/api/products/dot..name"));
    assert(!ValidRelativeNetworkPath("https://example.com/api"));
    assert(!ValidRelativeNetworkPath("//example.com/api"));
    assert(!ValidRelativeNetworkPath("/api/../admin"));
    assert(!ValidRelativeNetworkPath("/api\\admin"));
    assert(!ValidRelativeNetworkPath("/api/merchant orders"));
    assert(!ValidRelativeNetworkPath(std::string(257U, 'a')));
    assert(ValidIdempotencyKey("p4-12345678"));
    assert(ValidIdempotencyKey(""));
    assert(!ValidIdempotencyKey("contains space"));
    assert(!ValidIdempotencyKey(std::string(65U, 'x')));
    return 0;
}
