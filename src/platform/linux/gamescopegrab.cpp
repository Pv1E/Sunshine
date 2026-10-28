/**
 * @file src/platform/linux/gamescopegrab.cpp
 * @brief Capture gamescope's own PipeWire screencast node, without a portal or a Wayland session.
 */
// standard includes
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

// lib includes
#include <lizardbyte/common/env.h>
#include <pipewire/pipewire.h>

// local includes
#include "pipewire.cpp"
#include "src/platform/common.h"
#include "src/video.h"
#include "src/video_colorspace.h"

using namespace std::literals;

namespace gamescope {
  /**
   * @brief Node name to look for; gamescope names its stream "gamescope", SUNSHINE_GAMESCOPE_NODE overrides it.
   */
  std::string node_name() {
    std::string name;
    if (!lizardbyte::common::get_env("SUNSHINE_GAMESCOPE_NODE", name) || name.empty()) {
      name = "gamescope";
    }
    return name;
  }

  /**
   * @brief Result of a one-shot registry scan for the gamescope node.
   */
  struct node_t {
    uint32_t id = PW_ID_ANY;  ///< PipeWire global id of the node.
    uint64_t serial = SPA_ID_INVALID;  ///< PipeWire object serial of the node.
  };

  struct lookup_t {
    std::string name;
    node_t node;
    int pending = 0;
    struct pw_main_loop *loop = nullptr;
  };

  void on_global(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const struct spa_dict *props) {
    auto *lookup = static_cast<lookup_t *>(data);
    if (props == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
      return;
    }
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (name == nullptr || lookup->name != name) {
      return;
    }
    lookup->node.id = id;
    if (const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL)) {
      lookup->node.serial = std::strtoull(serial, nullptr, 10);
    }
  }

  void on_core_done(void *data, uint32_t id, int seq) {
    auto *lookup = static_cast<lookup_t *>(data);
    if (id == PW_ID_CORE && seq == lookup->pending) {
      pw_main_loop_quit(lookup->loop);
    }
  }

  /**
   * @brief Connect to the local PipeWire daemon and find the gamescope node.
   *
   * @return The node, with id PW_ID_ANY when it was not found.
   */
  node_t find_node() {
    lookup_t lookup;
    lookup.name = node_name();

    pw_init(nullptr, nullptr);
    lookup.loop = pw_main_loop_new(nullptr);
    if (lookup.loop == nullptr) {
      return {};
    }
    auto *context = pw_context_new(pw_main_loop_get_loop(lookup.loop), nullptr, 0);
    auto *core = context ? pw_context_connect(context, nullptr, 0) : nullptr;
    if (core == nullptr) {
      BOOST_LOG(debug) << "[gamescopegrab] No PipeWire daemon to connect to"sv;
      if (context) {
        pw_context_destroy(context);
      }
      pw_main_loop_destroy(lookup.loop);
      return {};
    }

    static const struct pw_registry_events registry_events = {
      .version = PW_VERSION_REGISTRY_EVENTS,
      .global = on_global,
    };
    static const struct pw_core_events core_events = {
      .version = PW_VERSION_CORE_EVENTS,
      .done = on_core_done,
    };

    auto *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    struct spa_hook registry_listener {};
    struct spa_hook core_listener {};
    pw_registry_add_listener(registry, &registry_listener, &registry_events, &lookup);
    pw_core_add_listener(core, &core_listener, &core_events, &lookup);

    // One sync round trip delivers every global that already exists.
    lookup.pending = pw_core_sync(core, PW_ID_CORE, 0);
    pw_main_loop_run(lookup.loop);

    spa_hook_remove(&core_listener);
    spa_hook_remove(&registry_listener);
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(lookup.loop);
    return lookup.node;
  }

  /**
   * @brief Display backed by gamescope's PipeWire node.
   */
  class gamescope_t: public pipewire::pipewire_display_t {
  public:
    explicit gamescope_t(const ::video::config_t &config) {
      this->width = config.width;
      this->height = config.height;

      // gamescope converts to NV12 on the GPU, which leaves the encoder a plane copy instead of a CPU color conversion.
      const auto colorspace = ::video::colorspace_from_client_config(config, false);
      if (config.chromaSamplingType == 0 && colorspace.bit_depth == 8 &&
          (colorspace.colorspace == ::video::colorspace_e::rec601 || colorspace.colorspace == ::video::colorspace_e::rec709)) {
        this->pipewire.request_nv12(
          colorspace.colorspace == ::video::colorspace_e::rec601 ? SPA_VIDEO_COLOR_MATRIX_BT601 : SPA_VIDEO_COLOR_MATRIX_BT709,
          colorspace.full_range ? SPA_VIDEO_COLOR_RANGE_0_255 : SPA_VIDEO_COLOR_RANGE_16_235
        );
      }
    }

    int configure_stream(const std::string &display_name [[maybe_unused]], int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_object_serial) override {
      const auto node = find_node();
      if (node.id == PW_ID_ANY) {
        BOOST_LOG(error) << "[gamescopegrab] No PipeWire node named '"sv << node_name() << "'; is gamescope running with PipeWire?"sv;
        return -1;
      }
      BOOST_LOG(info) << "[gamescopegrab] Using PipeWire node "sv << node.id << " (serial "sv << node.serial << ')';

      out_pipewire_fd = -1;
      out_pipewire_node = node.id;
      out_pipewire_object_serial = node.serial;
      this->offset_x = 0;
      this->offset_y = 0;
      return 0;
    }

    // gamescope is the only output: there are no Wayland monitors to query, the stream size is the whole screen.
    void verify_and_update_display_parameters() override {
      this->logical_width = this->width;
      this->logical_height = this->height;
      this->env_width = this->width;
      this->env_height = this->height;
      this->env_logical_width = this->width;
      this->env_logical_height = this->height;
    }
  };
}  // namespace gamescope

namespace platf {
  /**
   * @brief Create a display that captures gamescope's PipeWire node.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name (ignored, gamescope has one output).
   * @param config Configuration values to apply.
   * @return Display backend, or nullptr.
   */
  std::shared_ptr<display_t> gamescope_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[gamescopegrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    auto display = std::make_shared<gamescope::gamescope_t>(config);
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }
    return display;
  }

  /**
   * @brief List the gamescope display when its PipeWire node exists.
   *
   * @return One display name, or an empty list.
   */
  std::vector<std::string> gamescope_display_names() {
    if (gamescope::find_node().id == PW_ID_ANY) {
      return {};
    }
    return {"gamescope"};
  }
}  // namespace platf
