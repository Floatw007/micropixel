list(APPEND micropixel_sources
    "platform/network/managed_http_client.cpp"
    "platform/network/network_route_source.cpp"
)
list(APPEND micropixel_requires esp_http_client)
