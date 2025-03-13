#include <immer/array_transient.hpp>
#include <immer/map_transient.hpp>
#include <immer/vector_transient.hpp>
#include <sessions/handlers.hpp>
#include <state/sessions.hpp>
#include <streaming/streaming.hpp>

namespace wolf::core::sessions {

using session_devices = immer::map<std::string /* session_id */, std::shared_ptr<events::devices_atom_queue>>;

immer::vector<immer::box<events::EventBusHandlers>>
setup_moonlight_handlers(const immer::box<state::AppState> &app_state,
                         const std::string &runtime_dir,
                         const std::optional<AudioServer> &audio_server) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;

  /*
   * A queue of devices that are waiting to be plugged, mapped by session_id
   * This way we can accumulate devices here until the docker container is up and running
   */
  auto plugged_devices_queue = std::make_shared<immer::atom<session_devices>>();

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StopStreamEvent>>(
      [&app_state, plugged_devices_queue](const immer::box<events::StopStreamEvent> &ev) {
        // Remove session from app state so that HTTP/S applist gets updated
        // This should effectively destroy the virtual Wayland session since it holds the last reference
        app_state->running_sessions->update([&ev](const immer::vector<events::StreamSession> &ses_v) {
          return state::remove_session(ses_v, {.session_id = ev->session_id});
        });

        plugged_devices_queue->update([=](const auto map) { return map.erase(std::to_string(ev->session_id)); });
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::PlugDeviceEvent>>(
      [plugged_devices_queue](const immer::box<events::PlugDeviceEvent> &hotplug_ev) {
        logs::log(logs::debug, "{} received hot-plug device event", hotplug_ev->session_id);

        if (auto session_devices_queue = plugged_devices_queue->load()->find(hotplug_ev->session_id)) {
          session_devices_queue->get()->push(hotplug_ev);
        } else {
          logs::log(logs::warning, "Unable to find plugged_devices_queue for session {}", hotplug_ev->session_id);
        }
      }));

  // Run process and our custom wayland as soon as a new StreamSession is created
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StreamSession>>(
      [=](const immer::box<events::StreamSession> &session) {
        /* Initialise plugged device queue */
        auto devices_q = std::make_shared<events::devices_atom_queue>();
        plugged_devices_queue->update(
            [=](const session_devices map) { return map.set(std::to_string(session->session_id), devices_q); });

        if (session->app->start_virtual_compositor) {
          logs::log(logs::debug, "[STREAM_SESSION] Create wayland compositor");

          auto render_node = session->app->render_node;
          auto wl_state = virtual_display::create_wayland_display({}, render_node);
          if (!wl_state) {
            logs::log(logs::error, "Unable to create wayland compositor");
            return;
          }
          virtual_display::set_resolution(
              *wl_state,
              {session->display_mode.width, session->display_mode.height, session->display_mode.refreshRate});

          // Set the wayland display
          session->wayland_display->store(wl_state);

          // Set virtual devices
          session->mouse->emplace(virtual_display::WaylandMouse(wl_state));
          session->keyboard->emplace(virtual_display::WaylandKeyboard(wl_state));

          // Start Gstreamer producer pipeline
          std::thread([session, wl_state]() {
            streaming::start_video_producer(std::to_string(session->session_id),
                                            wl_state,
                                            {.width = session->display_mode.width,
                                             .height = session->display_mode.height,
                                             .refreshRate = session->display_mode.refreshRate},
                                            session->event_bus);
          }).detach();
        } else {
          // Create virtual devices
          auto mouse = input::Mouse::create();
          if (!mouse) {
            logs::log(logs::error, "Failed to create mouse: {}", mouse.getErrorMessage());
          } else {
            auto mouse_ptr = input::Mouse(std::move(*mouse));
            devices_q->push(immer::box<events::PlugDeviceEvent>(
                events::PlugDeviceEvent{.session_id = std::to_string(session->session_id),
                                        .udev_events = mouse_ptr.get_udev_events(),
                                        .udev_hw_db_entries = mouse_ptr.get_udev_hw_db_entries()}));
            session->mouse->emplace(std::move(mouse_ptr));
          }

          auto keyboard = input::Keyboard::create();
          if (!keyboard) {
            logs::log(logs::error, "Failed to create keyboard: {}", keyboard.getErrorMessage());
          } else {
            auto keyboard_ptr = input::Keyboard(std::move(*keyboard));
            devices_q->push(immer::box<events::PlugDeviceEvent>(
                events::PlugDeviceEvent{.session_id = std::to_string(session->session_id),
                                        .udev_events = keyboard_ptr.get_udev_events(),
                                        .udev_hw_db_entries = keyboard_ptr.get_udev_hw_db_entries()}));
            session->keyboard->emplace(std::move(keyboard_ptr));
          }
        }

        /* Create audio virtual sink */
        logs::log(logs::debug, "[STREAM_SESSION] Create virtual audio sink");
        auto pulse_sink_name = fmt::format("virtual_sink_{}", session->session_id);
        std::shared_ptr<audio::VSink> v_device;
        if (session->app->start_audio_server && audio_server && audio_server->server) {
          v_device = audio::create_virtual_sink(
              audio_server->server,
              audio::AudioDevice{.sink_name = pulse_sink_name,
                                 .mode = state::get_audio_mode(session->audio_channel_count, true)});
          session->audio_sink->store(v_device);

          std::thread([session, audio_server = audio_server->server]() {
            auto sink_name = fmt::format("virtual_sink_{}.monitor", session->session_id);
            streaming::start_audio_producer(std::to_string(session->session_id),
                                            session->event_bus,
                                            session->audio_channel_count,
                                            sink_name,
                                            audio::get_server_name(audio_server));
          }).detach();
        }

        logs::log(logs::debug, "[STREAM_SESSION] Start stream");
        session->event_bus->fire_event(immer::box<events::StartRunner>(
            events::StartRunner{.stop_stream_when_over = true,
                                .runner = session->app->runner,
                                .stream_session = std::make_shared<events::StreamSession>(*session)}));
      }));

  /* Start runner */
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::StartRunner>>(
      [=](const immer::box<events::StartRunner> &run_session) {
        auto session_id = std::to_string(run_session->stream_session->session_id);
        auto devices_q = plugged_devices_queue->load()->find(session_id);
        if (!devices_q) {
          logs::log(logs::warning, "No devices queue found for session {}", session_id);
          return;
        }

        std::thread([=]() {
          start_runner(run_session->runner,
                       *devices_q,
                       immer::box<RunnerArgs>{RunnerArgs{
                           .session_id = session_id,
                           .video_settings = {.width = run_session->stream_session->display_mode.width,
                                              .height = run_session->stream_session->display_mode.height,
                                              .refresh_rate = run_session->stream_session->display_mode.refreshRate,
                                              .wayland_render_node = run_session->stream_session->app->render_node,
                                              .runner_render_node = run_session->stream_session->app->render_node},
                           .wayland_display = run_session->stream_session->wayland_display->load(),
                           .audio_server = audio_server,
                           .audio_sink = run_session->stream_session->audio_sink->load(),
                           .state_folder = run_session->stream_session->app_state_folder,
                           .xdg_runtime_dir = runtime_dir,
                           .client_settings = run_session->stream_session->client_settings}});

          // Runner process ended
          if (run_session->stop_stream_when_over) {
            run_session->stream_session->wayland_display->store(nullptr);

            app_state->event_bus->fire_event(immer::box<events::StopStreamEvent>(
                events::StopStreamEvent{.session_id = run_session->stream_session->session_id}));
          }
        }).detach();
      }));

  // Video streaming pipeline
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::VideoSession>>(
      [=](const immer::box<events::VideoSession> &sess) {
        std::thread([=]() {
          boost::promise<unsigned short> port_promise;
          auto port_fut = port_promise.get_future();
          std::once_flag called;
          auto ev_handler = app_state->event_bus->register_handler<immer::box<events::RTPVideoPingEvent>>(
              [pp = std::ref(port_promise), &called, sess](const immer::box<events::RTPVideoPingEvent> &ping_ev) {
                std::call_once(called, [=]() { // We'll keep receiving PING requests, but we only want the first one
                  if (ping_ev->client_ip == sess->client_ip) {
                    pp.get().set_value(ping_ev->client_port); // This throws when set multiple times
                  }
                });
              });

          std::shared_ptr<std::atomic_bool> cancel_job = std::make_shared<std::atomic<bool>>(false);
          auto cancel_event = app_state->event_bus->register_handler<immer::box<events::VideoSession>>(
              [=](const immer::box<events::VideoSession> &new_sess) {
                if (new_sess->session_id == sess->session_id) {
                  // A new VideoSession has been queued whilst we still haven't received a PING
                  *cancel_job = true;
                }
              });

          logs::log(logs::debug, "Video session {}, waiting for PING...", sess->session_id);

          // Stop here until we get a PING
          unsigned short client_port = 0;
          if (sess->wait_for_ping) {
            auto status = port_fut.wait_for(boost::chrono::milliseconds(DEFAULT_SESSION_TIMEOUT_MILLIS));
            if (status != boost::future_status::ready) {
              logs::log(logs::warning, "Video session {} timed out waiting for PING", sess->session_id);
              return;
            }
            client_port = port_fut.get();
            cancel_event.unregister();
            ev_handler.unregister();

            if (*cancel_job) {
              return;
            }
          }

          streaming::start_streaming_video(sess, app_state->event_bus, client_port);
        }).detach();
      }));

  // Audio streaming pipeline
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::AudioSession>>(
      [=](const immer::box<events::AudioSession> &sess) {
        std::thread([=]() {
          boost::promise<unsigned short> port_promise;
          auto port_fut = port_promise.get_future();
          std::once_flag called;
          auto ev_handler = app_state->event_bus->register_handler<immer::box<events::RTPAudioPingEvent>>(
              [pp = std::ref(port_promise), &called, sess](const immer::box<events::RTPAudioPingEvent> &ping_ev) {
                std::call_once(called, [=]() { // We'll keep receiving PING requests, but we only want the first one
                  if (ping_ev->client_ip == sess->client_ip) {
                    pp.get().set_value(ping_ev->client_port); // This throws when set multiple times
                  }
                });
              });

          std::shared_ptr<std::atomic_bool> cancel_job = std::make_shared<std::atomic<bool>>(false);
          auto cancel_event = app_state->event_bus->register_handler<immer::box<events::AudioSession>>(
              [=](const immer::box<events::AudioSession> &new_sess) {
                if (new_sess->session_id == sess->session_id) {
                  // A new AudioSession has been queued whilst we still haven't received a PING
                  *cancel_job = true;
                }
              });

          logs::log(logs::debug, "Audio session {}, waiting for PING...", sess->session_id);

          // Stop here until we get a PING
          unsigned short client_port = 0;
          if (sess->wait_for_ping) {
            auto status = port_fut.wait_for(boost::chrono::milliseconds(DEFAULT_SESSION_TIMEOUT_MILLIS));
            if (status != boost::future_status::ready) {
              logs::log(logs::warning, "Audio session {} timed out waiting for PING", sess->session_id);
              return;
            }
            client_port = port_fut.get();
            cancel_event.unregister();
            ev_handler.unregister();

            if (*cancel_job) {
              return;
            }
          }

          auto audio_server_name = audio_server ? audio::get_server_name(audio_server->server)
                                                : std::optional<std::string>();
          auto sink_name = fmt::format("virtual_sink_{}.monitor", sess->session_id);
          auto server_name = audio_server_name ? audio_server_name.value() : "";

          streaming::start_streaming_audio(sess, app_state->event_bus, client_port, sink_name, server_name);
        }).detach();
      }));

  return handlers.persistent();
}

} // namespace wolf::core::sessions