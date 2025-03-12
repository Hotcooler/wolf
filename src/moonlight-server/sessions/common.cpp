#include <immer/array_transient.hpp>
#include <immer/map_transient.hpp>
#include <platforms/hw.hpp>
#include <sessions/handlers.hpp>

namespace wolf::core::sessions {

void start_runner(std::shared_ptr<events::Runner> runner,
                  std::shared_ptr<events::devices_atom_queue> plugged_devices_queue,
                  immer::box<RunnerArgs> args) {
  /* Setup devices paths */
  auto all_devices = immer::array_transient<std::string>();

  /* Setup mounted paths */
  immer::array_transient<std::pair<std::string, std::string>> mounted_paths;

  /* Setup environment paths */
  immer::map_transient<std::string, std::string> full_env;
  full_env.set("XDG_RUNTIME_DIR", args->xdg_runtime_dir);
  full_env.set("WOLF_SESSION_ID", args->session_id);

  auto pulse_sink_name = fmt::format("virtual_sink_{}", args->session_id);
  auto audio_server_name = args->audio_server ? audio::get_server_name(args->audio_server->server) : "";
  full_env.set("PULSE_SINK", pulse_sink_name);
  full_env.set("PULSE_SOURCE", pulse_sink_name + ".monitor");
  full_env.set("PULSE_SERVER", audio_server_name);
  mounted_paths.push_back({audio_server_name, audio_server_name});

  if (auto wl_state = args->wayland_display) {
    full_env.set("GAMESCOPE_WIDTH", std::to_string(args->video_settings.width));
    full_env.set("GAMESCOPE_HEIGHT", std::to_string(args->video_settings.height));
    full_env.set("GAMESCOPE_REFRESH", std::to_string(args->video_settings.refresh_rate));

    /* Setup additional devices paths */
    auto graphic_devices = virtual_display::get_devices(*wl_state);
    std::copy(graphic_devices.begin(), graphic_devices.end(), std::back_inserter(all_devices));

    /* Setup additional env paths */
    for (const auto &env : virtual_display::get_env(*wl_state)) {
      auto split = utils::split(env, '=');

      if (split[0] == "WAYLAND_DISPLAY") {
        auto socket_path = fmt::format("{}/{}", args->xdg_runtime_dir, split[1]);
        logs::log(logs::debug, "WAYLAND_DISPLAY={}", socket_path);
        mounted_paths.push_back({socket_path, socket_path});
      }

      full_env.set(utils::to_string(split[0]), utils::to_string(split[1]));
    }
  }

  /* Adding custom state folder */
  mounted_paths.push_back({args->state_folder, "/home/retro"});

  /* GPU specific adjustments */
  auto render_node = args->video_settings.runner_render_node;
  auto additional_devices = linked_devices(render_node);
  std::copy(additional_devices.begin(), additional_devices.end(), std::back_inserter(all_devices));

  auto gpu_vendor = get_vendor(render_node);
  if (gpu_vendor == NVIDIA) {
    if (auto driver_volume = utils::get_env("NVIDIA_DRIVER_VOLUME_NAME")) {
      logs::log(logs::info, "Mounting nvidia driver {}:/usr/nvidia", driver_volume);
      mounted_paths.push_back({driver_volume, "/usr/nvidia"});
    }
  } else if (gpu_vendor == INTEL) {
    full_env.set("INTEL_DEBUG", "norbc"); // see: https://github.com/games-on-whales/wolf/issues/50
  }

  full_env.set("PUID", std::to_string(args->client_settings->run_uid));
  full_env.set("PGID", std::to_string(args->client_settings->run_gid));

  /* Finally run the app, this will stop here until over */
  runner->run(args->session_id,
              args->state_folder,
              plugged_devices_queue,
              all_devices.persistent(),
              mounted_paths.persistent(),
              full_env.persistent(),
              render_node);

  if (args->audio_server && args->audio_sink) {
    logs::log(logs::debug, "[STREAM_SESSION] Remove virtual audio sink");
    audio::delete_virtual_sink(args->audio_server->server, args->audio_sink);
  }
}

} // namespace wolf::core::sessions