#include "firmware_app.hpp"
#include "host/logging/system_log_buffer.hpp"
#include "platform/platform.hpp"
#include "runtime/wamr/diagnostics.h"

extern "C" void app_main(void) {
    micropixel_start_heap_trace();
    (void)micropixel::firmware::logging::StartSystemLogCapture();
    micropixel::firmware::FirmwareApp app(micropixel::platform::ConfiguredPlatform());
    app.Run();
}
