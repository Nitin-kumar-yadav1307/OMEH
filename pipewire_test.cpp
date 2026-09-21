#include <iostream>
#include <cstdint>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

struct AppData
{
    struct pw_stream *stream;
    uint64_t sample_count = 0;
};

/*
 * Generate the same test pulse on each stream.
 */
void on_process(void *userdata)
{
    AppData *app =
        static_cast<AppData *>(userdata);

    struct pw_buffer *b =
        pw_stream_dequeue_buffer(app->stream);

    if (b == nullptr)
        return;

    struct spa_buffer *buf = b->buffer;

    struct spa_data *data =
        &buf->datas[0];

    if (data->data == nullptr)
    {
        pw_stream_queue_buffer(app->stream, b);
        return;
    }

    int16_t *samples =
        static_cast<int16_t *>(data->data);

    /*
     * Stereo:
     * 2 channels × 16 bits
     */
    uint32_t frames =
        data->maxsize / (sizeof(int16_t) * 2);

    /*
     * Generate a 10 ms pulse every second.
     *
     * 48000 samples/sec
     * 480 samples = 10 ms
     */
    for (uint32_t i = 0; i < frames; i++)
    {
        uint64_t position =
            app->sample_count % 48000;

        int16_t sample = 0;

        if (position < 480)
            sample = 30000;

        samples[i * 2] = sample;
        samples[i * 2 + 1] = sample;

        app->sample_count++;
    }

    data->chunk->offset = 0;

    data->chunk->stride =
        sizeof(int16_t) * 2;

    data->chunk->size =
        frames * sizeof(int16_t) * 2;

    pw_stream_queue_buffer(
        app->stream,
        b
    );
}


struct pw_stream *create_stream(
    struct pw_main_loop *loop,
    const char *stream_name,
    const char *target,
    AppData *appData)
{
    struct pw_properties *props =
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",

            /*
             * IMPORTANT:
             * Tell PipeWire exactly which
             * Bluetooth sink this stream belongs to.
             */
            PW_KEY_TARGET_OBJECT, target,

            nullptr
        );

    struct pw_stream_events events = {};

    events.version =
        PW_VERSION_STREAM_EVENTS;

    events.process =
        on_process;

    struct pw_stream *stream =
        pw_stream_new_simple(
            pw_main_loop_get_loop(loop),
            stream_name,
            props,
            &events,
            appData
        );

    return stream;
}


bool connect_stream(
    struct pw_stream *stream)
{
    const struct spa_pod *params[1];

    uint8_t buffer[1024];

    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(
            buffer,
            sizeof(buffer)
        );

    struct spa_audio_info_raw audio_info = {};

    audio_info.format =
        SPA_AUDIO_FORMAT_S16;

    audio_info.channels = 2;

    audio_info.rate = 48000;

    params[0] =
        spa_format_audio_raw_build(
            &builder,
            SPA_PARAM_EnumFormat,
            &audio_info
        );

    int result =
        pw_stream_connect(
            stream,
            PW_DIRECTION_OUTPUT,
            PW_ID_ANY,

            static_cast<pw_stream_flags>(
                PW_STREAM_FLAG_MAP_BUFFERS |
                PW_STREAM_FLAG_RT_PROCESS
            ),

            params,
            1
        );

    return result == 0;
}


int main(int argc, char *argv[])
{
    pw_init(&argc, &argv);

    struct pw_main_loop *loop =
        pw_main_loop_new(nullptr);

    /*
     * 3r
     */
    AppData app3r{};
    AppData app2r{};

    struct pw_stream *stream3r =
        create_stream(
            loop,
            "My Audio Stream 3r",
            "bluez_output.41_42_D8_E4_B4_72.1",
            &app3r
        );

    struct pw_stream *stream2r =
        create_stream(
            loop,
            "My Audio Stream 2r",
            "bluez_output.B0_A3_F2_29_8E_C8.1",
            &app2r
        );

    app3r.stream = stream3r;
    app2r.stream = stream2r;
    
    if (!stream3r || !stream2r)
    {
        std::cerr
            << "Failed to create streams\n";

        return 1;
    }

    if (!connect_stream(stream3r))
    {
        std::cerr
            << "Failed to connect 3r stream\n";

        return 1;
    }

    if (!connect_stream(stream2r))
    {
        std::cerr
            << "Failed to connect 2r stream\n";

        return 1;
    }

    std::cout
        << "Both Bluetooth streams started.\n";

    pw_main_loop_run(loop);

    return 0;
}