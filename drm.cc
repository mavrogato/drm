
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <string_view>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <libdrm/drm.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-drm-client.h>

#include "aux/tuple-support.hh"
#include "aux/io.hh"

/////////////////////////////////////////////////////////////////////////////
#include <concepts>
#include <memory>

namespace aux::inline wayland_client
{
    template <class> constexpr wl_interface const *const interface_ptr = nullptr;
    template <class T> concept client_like = interface_ptr<T> != nullptr;

    template <client_like T> void (*deleter)(T*) = [](T*) noexcept { };
    template <client_like T> struct listener_type { };

#define INTERN_CLIENT_LIKE(CLIENT, DELETER)                             \
    template <> constexpr wl_interface const *const interface_ptr<CLIENT> = &CLIENT##_interface; \
    template <> void (*deleter<CLIENT>)(CLIENT*) = DELETER;

    INTERN_CLIENT_LIKE(wl_display, wl_display_disconnect);

#undef INTERN_CLIENT_LIKE

#define INTERN_CLIENT_LIKE_LISTENER(CLIENT)                             \
    template <> constexpr wl_interface const *const interface_ptr<CLIENT> = &CLIENT##_interface; \
    template <> void (*deleter<CLIENT>)(CLIENT*) = CLIENT##_destroy;    \
    template <> struct listener_type<CLIENT> : CLIENT##_listener { };

    INTERN_CLIENT_LIKE_LISTENER(wl_registry)
    INTERN_CLIENT_LIKE_LISTENER(wl_drm)

#undef INTERN_CLIENT_LIKE_CONCEPT

    template <client_like T>
    [[nodiscard]] auto make_unique(T* raw = nullptr) noexcept {
        return std::unique_ptr<T, decltype (deleter<T>)>(raw, deleter<T>);
    }

} // ::aux::wayland_client

auto lamed(auto&& closure) {
    static auto cache = closure;
    return [](auto... args) {
        return cache(args...);
    };
}

#if 1
int main() {
    try {
        if (auto display = aux::make_unique(wl_display_connect(nullptr))) {
            if (auto registry = aux::make_unique(wl_display_get_registry(display.get()))) {
                auto drm = aux::make_unique<wl_drm>();
                static wl_registry_listener registry_listener = {
                    .global = lamed([&](auto, auto registry, uint32_t name, auto interface, uint32_t version) {
                        if (aux::interface_ptr<wl_drm>->name == std::string_view(interface)) {
                            drm.reset(static_cast<wl_drm*>(wl_registry_bind(registry,
                                                                            name,
                                                                            aux::interface_ptr<wl_drm>,
                                                                            version)));
                        }
                    }),
                };
                wl_registry_add_listener(registry.get(), &registry_listener, nullptr);
                wl_display_roundtrip(display.get());

                std::filesystem::path render_path;
                wl_drm_listener drm_listener = {
                    .device = lamed([&](auto, auto, auto name) {
                        std::cout << "device: " << (render_path = name) << std::endl;
                    }),
                    .format = [](auto, auto, auto format) {
                        auto fourcc = std::bit_cast<std::array<char, 4>>(format);
                        std::cout << "format: " << fourcc << std::endl;
                    },
                    .authenticated = [](auto...) {
                        std::cout << "authenticated!" << std::endl;
                    },
                    .capabilities = [](auto, auto, auto cap) {
                        std::cout << "caps: " << cap << std::endl;
                    },
                };
                wl_drm_add_listener(drm.get(), &drm_listener, nullptr);
                wl_display_roundtrip(display.get());

                auto card_path = render_path.parent_path() / "card0";
                std::cout << card_path << std::endl;

                if (auto fd = aux::unique_fd{open(card_path.c_str(), O_RDWR)}) {
                    struct drm_auth auth = {};
                    if (auto ret = ioctl(fd, DRM_IOCTL_GET_MAGIC, &auth); ret == 0) {
                        std::cout << auth.magic << std::endl;
                        if (auto ret = ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth.magic); ret == 0) {
                            std::cout << "Authenticated!" << std::endl;
                        }
                        else {
                            std::cerr << "Authentication failed: " << ret << std::endl;
                        }
                    }
                    else {
                        std::cerr << "???" << std::endl;
                    }
                }
            }
        }
        return 0;
    }
    catch (std::exception& ex) {
        std::cerr << "Exception occurred: " << ex.what();
    }
    return -1;
}

#else
template <class Ch>
auto& operator<<(std::basic_ostream<Ch>& output, struct drm_version const& version) noexcept {
    output << "[ version_major: " << version.version_major
           << ", version_minor: " << version.version_minor
           << ", version_patchlevel: " << version.version_patchlevel
           << ", name: " << (version.name ? std::string_view{version.name, version.name_len} : "null")
           << ", date: " << (version.date ? std::string_view{version.date, version.date_len} : "null")
           << ", desc: " << (version.date ? std::string_view{version.desc, version.desc_len} : "null")
           << " ]";
    return output;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (auto path = std::filesystem::path{argv[i]};
            std::filesystem::exists(path) &&
            std::filesystem::is_character_file(path))
        {
            std::cout << "#######################################################" << std::endl;
            std::cout << "path: " << path << std::endl;
            if (auto fd = aux::unique_fd{open(path.c_str(), O_RDWR)}) {
                auto drm_version = [&fd]() {
                    struct drm_version version = {};
                    if (auto ret = ioctl(fd, DRM_IOCTL_VERSION, &version); ret == 0) {
                        return version;
                    }
                    else {
                        throw std::runtime_error("ioctl(DRM_IOCTL_VERSION) failed...");
                    }
                };
                std::cout << "drm_version: " << drm_version() << std::endl;

                auto drm_capability = [](int fd, uint64_t cap) {
                    struct drm_get_cap get_cap = { .capability = cap };
                    if (auto ret = ioctl(fd, DRM_IOCTL_GET_CAP, &get_cap); ret == 0) {
                        return get_cap.value;
                    }
                    else {
                        throw std::runtime_error("ioctl(DRM_IOCTL_GET_CAP) failed...");
                    }
                };
#define DUMP(cap) (std::cout << #cap << ": " << drm_capability(fd, cap) << std::endl)
                DUMP(DRM_CAP_DUMB_BUFFER);
                DUMP(DRM_CAP_VBLANK_HIGH_CRTC);
                DUMP(DRM_CAP_DUMB_PREFERRED_DEPTH);
                DUMP(DRM_CAP_DUMB_PREFER_SHADOW);
                DUMP(DRM_CAP_PRIME);
                DUMP(DRM_CAP_TIMESTAMP_MONOTONIC);
                DUMP(DRM_CAP_ASYNC_PAGE_FLIP);
                DUMP(DRM_CAP_CURSOR_WIDTH);
                DUMP(DRM_CAP_CURSOR_HEIGHT);
                DUMP(DRM_CAP_ADDFB2_MODIFIERS);
                DUMP(DRM_CAP_PAGE_FLIP_TARGET);
                DUMP(DRM_CAP_CRTC_IN_VBLANK_EVENT);
                DUMP(DRM_CAP_SYNCOBJ);
                DUMP(DRM_CAP_SYNCOBJ_TIMELINE);
#undef DUMP

                // The master client only do that:

                struct drm_auth auth = {};
                if (auto ret = ioctl(fd, DRM_IOCTL_GET_MAGIC, &auth); ret == 0) {
                    if (auto ret = ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth); ret == 0) {
                        std::cout << "Authenticated!" << std::endl;
                    }
                    else {
                        std::cerr << "Authentication failed: " << ret << std::endl;
                    }
                }
                else {
                    std::cerr << "???" << std::endl;
                }
           }
        }
    }
    return 0;
}
#endif
