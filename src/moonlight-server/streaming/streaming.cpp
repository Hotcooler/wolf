#include <control/control.hpp>
#include <gstreamer-1.0/gst/video/video-info.h>
#include <immer/array.hpp>
#include <immer/box.hpp>
#include <memory>
#include <streaming/streaming.hpp>

namespace streaming {

using namespace wolf::core::gstreamer;
using namespace wolf::core;

struct GstBusData {
  std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready;
  gst_element_ptr wayland_plugin;
};

gboolean structure_each(GQuark field_id, const GValue *value, gpointer user_data) {
  auto field_str = std::string(g_quark_to_string(field_id));
  if (!G_VALUE_HOLDS_STRING(value)) {
    logs::log(logs::warning, "Wayland source message: {} = {}", field_str, "not a string");
    return FALSE;
  }
  auto value_str = g_value_get_string(value);
  logs::log(logs::debug, "Wayland source message: {} = {}", field_str, value_str);

  if (field_str == "WAYLAND_DISPLAY") {
    logs::log(logs::info, "Wayland display ready, listening on: {}", value_str);
    auto bus_data = static_cast<GstBusData *>(user_data);
    bus_data->on_ready->set_value(
        WaylandDisplayReady{.wayland_socket_name = value_str, .wayland_plugin = bus_data->wayland_plugin});
  }

  return TRUE;
}

gboolean bus_watcher(GstBus *bus, GstMessage *msg, gpointer data) {
  switch (GST_MESSAGE_TYPE(msg)) {
  case GST_MESSAGE_APPLICATION: {
    auto structure = gst_message_get_structure(msg);
    if (gst_structure_has_name(structure, "wayland.src")) {
      gst_structure_foreach(structure, structure_each, data);
    }
  }
  }
  return TRUE;
}

void start_video_producer(std::size_t session_id,
                          const std::string &render_node,
                          const virtual_display::DisplayMode &display_mode,
                          std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready,
                          std::shared_ptr<events::EventBusType> event_bus) {
  auto pipeline = fmt::format(
      "waylanddisplaysrc name=wolf_wayland_source render_node={render_node} ! "
      "video/x-raw, width={width}, height={height}, framerate={fps}/1 ! "                      // TODO: DMA-BUF
      "queue leaky=downstream max-size-buffers=1 ! "                                                                               //
      "interpipesink name={session_id}_video sync=true async=false max-buffers=1", //
      fmt::arg("render_node", render_node),
      fmt::arg("session_id", session_id),
      fmt::arg("width", display_mode.width),
      fmt::arg("height", display_mode.height),
      fmt::arg("fps", display_mode.refreshRate));
  logs::log(logs::debug, "[GSTREAMER] Starting video producer: {}", pipeline);
  auto bus_data_ptr =
      std::make_shared<GstBusData>(GstBusData{.on_ready = std::move(on_ready), .wayland_plugin = nullptr});
  run_pipeline(pipeline, [=](auto pipeline, auto loop) {
    logs::log(logs::debug, "Setting up waylanddisplaysrc");

    auto wayland_plugin_el = gst_bin_get_by_name(GST_BIN(pipeline.get()), "wolf_wayland_source");
    auto wayland_plugin_ptr = gstreamer::gst_element_ptr(wayland_plugin_el, ::gst_object_unref);
    bus_data_ptr->wayland_plugin.swap(wayland_plugin_ptr);

    auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
    gst_bus_add_watch(bus, bus_watcher, bus_data_ptr.get());
    gst_object_unref(bus);

    // TODO: pause and resume? Should we do it?

    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (ev->session_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping video producer: {}", session_id);
            g_main_loop_quit(loop.get());
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler)};
  });
}

void start_audio_producer(std::size_t session_id,
                          const std::shared_ptr<events::EventBusType> &event_bus,
                          int channel_count,
                          const std::string &sink_name,
                          const std::string &server_name) {
  auto pipeline = fmt::format(
      "pulsesrc device=\"{sink_name}\" server=\"{server_name}\" ! " //
      "audio/x-raw, channels={channels}, rate=48000 ! "             //
      "queue leaky=downstream max-size-buffers=3 ! "                                                    //
      "interpipesink name=\"{session_id}_audio\" sync=true async=false max-buffers=3",
      fmt::arg("session_id", session_id),
      fmt::arg("channels", channel_count),
      fmt::arg("sink_name", sink_name),
      fmt::arg("server_name", server_name));
  logs::log(logs::debug, "[GSTREAMER] Starting audio producer: {}", pipeline);

  run_pipeline(pipeline, [=](auto pipeline, auto loop) {
    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (ev->session_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping audio producer: {}", session_id);
            g_main_loop_quit(loop.get());
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler)};
  });
}

/**
 * Start VIDEO pipeline
 */
void start_streaming_video(const immer::box<events::VideoSession> &video_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           unsigned short client_port) {
  std::string color_range = (video_session->color_range == events::ColorRange::JPEG) ? "jpeg" : "mpeg2";
  std::string color_space;
  switch (video_session->color_space) {
  case events::ColorSpace::BT601:
    color_space = "bt601";
    break;
  case events::ColorSpace::BT709:
    color_space = "bt709";
    break;
  case events::ColorSpace::BT2020:
    color_space = "bt2020";
    break;
  }

  auto pipeline = fmt::format(fmt::runtime(video_session->gst_pipeline),
                              fmt::arg("session_id", video_session->session_id),
                              fmt::arg("width", video_session->display_mode.width),
                              fmt::arg("height", video_session->display_mode.height),
                              fmt::arg("fps", video_session->display_mode.refreshRate),
                              fmt::arg("bitrate", video_session->bitrate_kbps),
                              fmt::arg("client_port", client_port),
                              fmt::arg("client_ip", video_session->client_ip),
                              fmt::arg("payload_size", video_session->packet_size),
                              fmt::arg("fec_percentage", video_session->fec_percentage),
                              fmt::arg("min_required_fec_packets", video_session->min_required_fec_packets),
                              fmt::arg("slices_per_frame", video_session->slices_per_frame),
                              fmt::arg("color_space", color_space),
                              fmt::arg("color_range", color_range),
                              fmt::arg("host_port", video_session->port));
  logs::log(logs::debug, "Starting video pipeline: \n{}", pipeline);

  run_pipeline(pipeline, [video_session, event_bus](auto pipeline, auto loop) {
    /*
     * The force IDR event will be triggered by the control stream.
     * We have to pass this back into the gstreamer pipeline
     * in order to force the encoder to produce a new IDR packet
     */
    auto idr_handler = event_bus->register_handler<immer::box<events::IDRRequestEvent>>(
        [sess_id = video_session->session_id, pipeline](const immer::box<events::IDRRequestEvent> &ctrl_ev) {
          if (ctrl_ev->session_id == sess_id) {
            logs::log(logs::debug, "[GSTREAMER] Forcing IDR");
            // Force IDR event, see: https://github.com/centricular/gstwebrtc-demos/issues/186
            // https://gstreamer.freedesktop.org/documentation/additional/design/keyframe-force.html?gi-language=c
            wolf::core::gstreamer::send_message(
                pipeline.get(),
                gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, NULL));
          }
        });

    auto pause_handler = event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
        [sess_id = video_session->session_id, loop](const immer::box<events::PauseStreamEvent> &ev) {
          if (ev->session_id == sess_id) {
            logs::log(logs::debug, "[GSTREAMER] Pausing pipeline: {}", sess_id);

            /**
             * Unfortunately here we can't just pause the pipeline,
             * when a pipeline will be resumed there are a lot of breaking changes
             * like:
             *  - Client IP:PORT
             *  - AES key and IV for encrypted payloads
             *  - Client resolution, framerate, and encoding
             *
             *  The only solution is to kill the pipeline and re-create it again
             * when a resume happens
             */

            g_main_loop_quit(loop.get());
          }
        });

    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [sess_id = video_session->session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (ev->session_id == sess_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping pipeline: {}", sess_id);
            g_main_loop_quit(loop.get());
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(idr_handler),
                                                              std::move(pause_handler),
                                                              std::move(stop_handler)};
  });
}

/**
 * Start AUDIO pipeline
 */
void start_streaming_audio(const immer::box<events::AudioSession> &audio_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           unsigned short client_port,
                           const std::string &sink_name,
                           const std::string &server_name) {
  auto pipeline = fmt::format(
      fmt::runtime(audio_session->gst_pipeline),
      fmt::arg("session_id", audio_session->session_id),
      fmt::arg("channels", audio_session->audio_mode.channels),
      fmt::arg("bitrate", audio_session->audio_mode.bitrate),
      // TODO: opusenc hardcodes those two
      // https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/1.24.6/subprojects/gst-plugins-base/ext/opus/gstopusenc.c#L661-666
      fmt::arg("streams", audio_session->audio_mode.streams),
      fmt::arg("coupled_streams", audio_session->audio_mode.coupled_streams),
      fmt::arg("sink_name", sink_name),
      fmt::arg("server_name", server_name),
      fmt::arg("packet_duration", audio_session->packet_duration),
      fmt::arg("aes_key", audio_session->aes_key),
      fmt::arg("aes_iv", audio_session->aes_iv),
      fmt::arg("encrypt", audio_session->encrypt_audio),
      fmt::arg("client_port", client_port),
      fmt::arg("client_ip", audio_session->client_ip),
      fmt::arg("host_port", audio_session->port));
  logs::log(logs::debug, "Starting audio pipeline: \n{}", pipeline);

  run_pipeline(pipeline, [session_id = audio_session->session_id, event_bus](auto pipeline, auto loop) {
    auto pause_handler = event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
        [session_id, loop](const immer::box<events::PauseStreamEvent> &ev) {
          if (ev->session_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Pausing pipeline: {}", session_id);

            /**
             * Unfortunately here we can't just pause the pipeline,
             * when a pipeline will be resumed there are a lot of breaking changes
             * like:
             *  - Client IP:PORT
             *  - AES key and IV for encrypted payloads
             *  - Client resolution, framerate, and encoding
             *
             *  The only solution is to kill the pipeline and re-create it again
             * when a resume happens
             */

            g_main_loop_quit(loop.get());
          }
        });

    auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
        [session_id, loop](const immer::box<events::StopStreamEvent> &ev) {
          if (ev->session_id == session_id) {
            logs::log(logs::debug, "[GSTREAMER] Stopping pipeline: {}", session_id);
            g_main_loop_quit(loop.get());
          }
        });

    return immer::array<immer::box<events::EventBusHandlers>>{std::move(pause_handler), std::move(stop_handler)};
  });
}

} // namespace streaming