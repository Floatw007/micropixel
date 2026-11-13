#ifndef MICROPIXEL_SDK_SYSTEM_HPP
#define MICROPIXEL_SDK_SYSTEM_HPP

namespace micropixel {

// Requests a transition from the foreground Guest to the Host-owned system
// settings. The Host suspends the Guest before presenting privileged settings.
class System final {
   public:
    [[nodiscard]] bool OpenSettings() const;

   private:
    struct CapabilityToken final {};
    explicit constexpr System(CapabilityToken) {}
    friend class Application;
};

}  // namespace micropixel

#endif
