#include <immer/vector_transient.hpp>
#include <sessions/handlers.hpp>
#include <state/config.hpp>
#include <state/data-structures.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions {

immer::vector<immer::box<events::EventBusHandlers>>
setup_lobbies_handlers(const immer::box<state::AppState> &app_state,
                       const std::string &runtime_dir,
                       const std::optional<AudioServer> &audio_server) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;

  // On create lobby event
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::CreateLobbyEvent>>(
      [=](const immer::box<events::CreateLobbyEvent> &lobby_settings) {
        logs::log(logs::info, "[LOBBY] Creating new lobby");
        auto ev_bus = app_state->event_bus;

        auto lobby = std::make_shared<events::Lobby>(
            events::Lobby{.id = lobby_settings->id,
                          .name = lobby_settings->name,
                          .pin = lobby_settings->pin,
                          .stop_when_everyone_leaves = lobby_settings->stop_when_everyone_leaves,
                          .runner = lobby_settings->runner});
        app_state->lobbies->update(
            [lobby](const immer::vector<events::Lobby> &lobbies) { return lobbies.push_back(*lobby); });

        { // Start Wayland compositor and Gstreamer producer pipeline
          logs::log(logs::debug, "[LOBBY] Create wayland compositor");

          virtual_display::DisplayMode display_mode = {.width = lobby_settings->video_settings.width,
                                                       .height = lobby_settings->video_settings.height,
                                                       .refreshRate = lobby_settings->video_settings.refresh_rate};

          auto wl_state =
              virtual_display::create_wayland_display({}, lobby_settings->video_settings.wayland_render_node);
          if (wl_state) {
            virtual_display::set_resolution(*wl_state, display_mode);

            lobby->wayland_display->store(wl_state);

            // Start Gstreamer producer pipeline
            std::thread([lobby, wl_state, display_mode, ev_bus]() {
              streaming::start_video_producer(lobby->id, wl_state, display_mode, ev_bus);
            }).detach();
          } else {
            logs::log(logs::error, "[LOBBY] Failed to create wayland compositor");
          }
        }

        { // Create audio virtual sink
          logs::log(logs::debug, "[LOBBY] Create audio virtual sink");
          auto pulse_sink_name = fmt::format("virtual_sink_{}", lobby->id);
          if (audio_server && audio_server->server) {
            auto channel_count = lobby_settings->audio_settings.channel_count;
            auto v_device = audio::create_virtual_sink(
                audio_server->server,
                audio::AudioDevice{.sink_name = pulse_sink_name, .mode = state::get_audio_mode(channel_count, true)});

            lobby->audio_sink->store(v_device);

            // Start Gstreamer producer pipeline
            std::thread([lobby, audio_server = audio_server->server, ev_bus, channel_count]() {
              auto sink_name = fmt::format("virtual_sink_{}.monitor", lobby->id);
              streaming::start_audio_producer(lobby->id,
                                              ev_bus,
                                              channel_count,
                                              sink_name,
                                              audio::get_server_name(audio_server));
            }).detach();
          }
        }

        { // Start runner
          logs::log(logs::debug, "[LOBBY] Start runner");
          std::string host_state_folder = utils::get_env("HOST_APPS_STATE_FOLDER", "/etc/wolf");
          auto full_path = std::filesystem::path(host_state_folder) / lobby_settings->runner_state_folder;
          logs::log(logs::debug, "Host app state folder: {}, creating paths", full_path.string());
          std::filesystem::create_directories(full_path);

          std::thread([=]() {
            start_runner(lobby->runner,
                         lobby->plugged_devices_queue,
                         immer::box<RunnerArgs>{RunnerArgs{.session_id = lobby->id,
                                                           .video_settings = lobby_settings->video_settings,
                                                           .wayland_display = lobby->wayland_display->load(),
                                                           .audio_server = audio_server,
                                                           .audio_sink = lobby->audio_sink->load(),
                                                           .state_folder = full_path,
                                                           .xdg_runtime_dir = runtime_dir,
                                                           .client_settings = lobby_settings->client_settings}});
            // Runner process ended, stop the lobby
            lobby->wayland_display->store(nullptr);

            app_state->event_bus->fire_event<immer::box<events::StopLobbyEvent>>(
                immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby->id}});
          }).detach();
        }
      }));

  // When a Moonlight client joins a lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::JoinLobbyEvent>>(
      [=](const immer::box<events::JoinLobbyEvent> &join_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), join_lobby_event->lobby_id);
        auto sessions = app_state->running_sessions->load();
        auto session = state::get_session_by_id(sessions.get(), join_lobby_event->moonlight_session_id);

        if (!lobby || !session) {
          logs::log(logs::error,
                    "[LOBBY] Failed to join lobby: lobby {} or session {} not found",
                    join_lobby_event->lobby_id,
                    join_lobby_event->moonlight_session_id);
          return;
        }
        logs::log(logs::info, "[LOBBY] Session {} joining lobby {}", session->session_id, lobby->id);

        // Update the lobby with the new session
        lobby->connected_sessions->update([session](const immer::vector<immer::box<std::string>> &connected_sessions) {
          return connected_sessions.push_back({std::to_string(session->session_id)});
        });

        // switch mouse and keyboard in session to use the lobby wayland server
        auto wl_state = lobby->wayland_display->load();
        session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
        session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));

        // Switch over all joypads present in the session into the lobby
        events::JoypadList joypads = session->joypads->load();
        for (auto [_joypad_nr, joypad] : joypads) {
          // Plug them into the lobby
          events::PlugDeviceEvent plug_ev{.session_id = lobby->id};
          std::visit(
              [&plug_ev](auto &pad) {
                plug_ev.udev_events = pad.get_udev_events();
                plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
              },
              *joypad);
          app_state->event_bus->fire_event(immer::box<events::PlugDeviceEvent>(plug_ev));
          // Unplug them from the current session
          app_state->event_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
              events::UnplugDeviceEvent{.session_id = std::to_string(session->session_id),
                                        .udev_events = plug_ev.udev_events,
                                        .udev_hw_db_entries = plug_ev.udev_hw_db_entries}});
        }
        // TODO: hotplug pen_tablet and touch_screen

        // Switch audio/video gstreamer stream producers
        app_state->event_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>{
            events::SwitchStreamProducerEvents{.session_id = session->session_id, .interpipe_src_id = lobby->id}});
      }));

  // When a Moonlight session leaves the lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::LeaveLobbyEvent>>(
      [=](const immer::box<events::LeaveLobbyEvent> &leave_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), leave_lobby_event->lobby_id);
        auto sessions = app_state->running_sessions->load();
        auto session = state::get_session_by_id(sessions.get(), leave_lobby_event->moonlight_session_id);

        if (!lobby || !session) {
          logs::log(logs::error,
                    "[LOBBY] Failed to leave lobby: lobby {} or session {} not found",
                    leave_lobby_event->lobby_id,
                    leave_lobby_event->moonlight_session_id);
          return;
        }
        logs::log(logs::info, "[LOBBY] Session {} leaving lobby {}", session->session_id, lobby->id);

        // Remove the current session from the lobby list
        lobby->connected_sessions->update([session](const immer::vector<immer::box<std::string>> &connected_sessions) {
          return connected_sessions | //
                 ranges::views::filter([session](const immer::box<std::string> &session_id) {
                   return *session_id != std::to_string(session->session_id);
                 }) | //
                 ranges::to<immer::vector<immer::box<std::string>>>();
        });

        // Switch over mouse and keyboard to use the original session wayland server
        auto wl_state = session->wayland_display->load();
        session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
        session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));

        // Switch over all joypads present in the lobby back into the original session
        events::JoypadList joypads = session->joypads->load();
        for (auto [_joypad_nr, joypad] : joypads) {
          // Plug them into original session
          events::PlugDeviceEvent plug_ev{.session_id = std::to_string(session->session_id)};
          std::visit(
              [&plug_ev](auto &pad) {
                plug_ev.udev_events = pad.get_udev_events();
                plug_ev.udev_hw_db_entries = pad.get_udev_hw_db_entries();
              },
              *joypad);
          app_state->event_bus->fire_event(immer::box<events::PlugDeviceEvent>(plug_ev));
          // Unplug them from the current lobby
          app_state->event_bus->fire_event(immer::box<events::UnplugDeviceEvent>{
              events::UnplugDeviceEvent{.session_id = lobby->id,
                                        .udev_events = plug_ev.udev_events,
                                        .udev_hw_db_entries = plug_ev.udev_hw_db_entries}});
        }
        // TODO: hotplug pen_tablet and touch_screen

        // Switch audio/video gstreamer stream producers
        app_state->event_bus->fire_event(immer::box<events::SwitchStreamProducerEvents>{
            events::SwitchStreamProducerEvents{.session_id = session->session_id,
                                               .interpipe_src_id = std::to_string(session->session_id)}});

        if (lobby->stop_when_everyone_leaves && lobby->connected_sessions->load()->size() == 0) {
          // Nobody left in the lobby, and it's set to stop when everyone leaves
          app_state->event_bus->fire_event(
              immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby->id}});
        }
      }));

  // Stopping a lobby will trigger leave for all the connected sessions
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
      [=](const immer::box<events::StopLobbyEvent> &stop_lobby_event) {
        auto lobbies = app_state->lobbies->load();
        auto lobby = state::get_lobby_by_id(lobbies.get(), stop_lobby_event->lobby_id);

        if (!lobby) {
          logs::log(logs::warning, "[LOBBY] lobby {} not found", stop_lobby_event->lobby_id);
          return;
        }
        logs::log(logs::info, "[LOBBY] stopping lobby {}", stop_lobby_event->lobby_id);

        immer::vector<immer::box<std::string>> sessions = lobby->connected_sessions->load();
        for (auto &session_id : sessions) {
          app_state->event_bus->fire_event(immer::box<events::LeaveLobbyEvent>{
              events::LeaveLobbyEvent{.lobby_id = lobby->id, .moonlight_session_id = std::stoul(*session_id)}});
        }

        // Finally, remove the lobby from the app_state
        app_state->lobbies->update([stop_lobby_event](const immer::vector<events::Lobby> &lobbies) {
          return lobbies | //
                 ranges::views::filter([stop_lobby_event](const events::Lobby &lobby) {
                   return lobby.id != stop_lobby_event->lobby_id;
                 }) | //
                 ranges::to<immer::vector<events::Lobby>>();
        });

        if (lobby->stop_when_everyone_leaves && lobby->connected_sessions->load()->size() == 0) {
          // Nobody left in the lobby, and it's set to stop when everyone leaves
          app_state->event_bus->fire_event(
              immer::box<events::StopLobbyEvent>{events::StopLobbyEvent{.lobby_id = lobby->id}});
        }
      }));

  // When a Moonlight session is being stopped we need to remove it from the lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopStreamEvent>>(
      [=](const immer::box<events::StopStreamEvent> &stop_stream_event) {
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        // Look thru all the lobbies for the ones that have the session
        for (const events::Lobby &lobby : lobbies) {
          immer::vector<immer::box<std::string>> sessions = lobby.connected_sessions->load();
          auto session = std::find_if(sessions.begin(), sessions.end(), [stop_stream_event](const auto &session) {
            return std::stoul(*session) == stop_stream_event->session_id;
          });
          if (session == sessions.end()) {
            continue;
          }
          logs::log(logs::info, "[LOBBY] removing session {} from lobby {}", stop_stream_event->session_id, lobby.id);
          // Remove the current session from the lobby list
          lobby.connected_sessions->update(
              [stop_stream_event](const immer::vector<immer::box<std::string>> &connected_sessions) {
                return connected_sessions | //
                       ranges::views::filter([stop_stream_event](const immer::box<std::string> &session_id) {
                         return *session_id != std::to_string(stop_stream_event->session_id);
                       }) | //
                       ranges::to<immer::vector<immer::box<std::string>>>();
              });
        }
      }));

  // On a PlugDeviceEvent, we have to add the device to the lobby queue so that the runner will pick it up
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [=](const immer::box<events::PlugDeviceEvent> &plug_device_event) {
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        // Look thru all the lobbies for the ones that have the session
        for (const events::Lobby &lobby : lobbies) {
          immer::vector<immer::box<std::string>> sessions = lobby.connected_sessions->load();
          auto session = std::find_if(sessions.begin(), sessions.end(), [plug_device_event](const auto &session) {
            return *session == plug_device_event->session_id;
          });
          if (session == sessions.end()) {
            continue;
          }
          logs::log(logs::info,
                    "[LOBBY] adding device to session {} in lobby {}",
                    plug_device_event->session_id,
                    lobby.id);

          // TODO: somehow block the PlugDeviceEvent handler in the underlying session!

          lobby.plugged_devices_queue->push(immer::box<events::PlugDeviceEvent>{
              events::PlugDeviceEvent{.session_id = lobby.id,
                                      .udev_events = plug_device_event->udev_events,
                                      .udev_hw_db_entries = plug_device_event->udev_hw_db_entries}});
        }
      }));

  // When a device is unplugged from a Moonlight session, we have to re-fire the event on our lobby
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::UnplugDeviceEvent>>(
      [=](const immer::box<events::UnplugDeviceEvent> &unplug_device_event) {
        immer::vector<events::Lobby> lobbies = app_state->lobbies->load();
        // Look thru all the lobbies for the ones that have the session
        for (const events::Lobby &lobby : lobbies) {
          immer::vector<immer::box<std::string>> sessions = lobby.connected_sessions->load();
          auto session = std::find_if(sessions.begin(), sessions.end(), [unplug_device_event](const auto &session) {
            return *session == unplug_device_event->session_id;
          });
          if (session == sessions.end()) {
            continue;
          }
          logs::log(logs::debug, "[LOBBY] Unplug device for session {}", unplug_device_event->session_id);
          app_state->event_bus->fire_event(
              events::UnplugDeviceEvent{.session_id = lobby.id,
                                        .udev_events = unplug_device_event->udev_events,
                                        .udev_hw_db_entries = unplug_device_event->udev_hw_db_entries});
        }
      }));

  return handlers.persistent();
}

} // namespace wolf::core::sessions