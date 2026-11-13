#include "runtime/services/voice_service.hpp"

#include <cstring>

#include "runtime/event_queue.hpp"

namespace micropixel::runtime {

VoiceService::VoiceService(device::VoiceService& voice, EventQueue& events, int64_t clock_origin_us)
    : voice_(voice), events_(events), clock_origin_us_(clock_origin_us), bound_(true) {
    voice_.BindUpdateSink(OnUpdate, this);
}

VoiceService::~VoiceService() { Shutdown(); }

void VoiceService::Shutdown() {
    if (!bound_) return;
    (void)voice_.CancelSession();
    voice_.UnbindUpdateSink(this);
    bound_ = false;
}

void VoiceService::OnUpdate(void* context, const device::VoiceUpdate& update) {
    if (context != nullptr) static_cast<VoiceService*>(context)->Push(update);
}

void VoiceService::Push(const device::VoiceUpdate& update) {
    if (!bound_ || update.event_id < MICROPIXEL_VOICE_EVENT_STATE_CHANGED ||
        update.event_id > MICROPIXEL_VOICE_EVENT_LOCAL_COMMAND) {
        return;
    }
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = update.event_id;
    event.service_id = MICROPIXEL_SERVICE_VOICE;
    const uint64_t origin = static_cast<uint64_t>(clock_origin_us_);
    event.timestamp_us = update.timestamp_us >= origin ? update.timestamp_us - origin : 0U;
    event.sequence = ++sequence_;
    event.status = update.status;
    std::memcpy(event.payload, &update.payload, sizeof(update.payload));
    if (update.event_id == MICROPIXEL_VOICE_EVENT_INPUT_LEVEL ||
        update.event_id == MICROPIXEL_VOICE_EVENT_TRANSCRIPT_UPDATED ||
        update.event_id == MICROPIXEL_VOICE_EVENT_RESPONSE_UPDATED) {
        (void)events_.PushAdvisory(event);
    } else {
        (void)events_.PushRequired(event);
    }
}

}  // namespace micropixel::runtime
