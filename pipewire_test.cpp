#include <iostream>
#include <cmath>
#include <cstdint>
#include <ctime>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

static long long previous_timestamp = 0;


struct AppData
{
    struct pw_stream *stream;
};


void on_process(void *userdata)
{

    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    long long timestamp =
        ts.tv_sec * 1000000000LL + ts.tv_nsec;

    std::cout << "Audio callback: "
              << timestamp
              << " ns\n";


    AppData *data =
        static_cast<AppData *>(userdata);

    struct pw_buffer *b =
        pw_stream_dequeue_buffer(data->stream);

    if (b == nullptr)
        return;

    struct spa_buffer *buf =
        b->buffer;

    struct spa_data *data_block =
        &buf->datas[0];

    if (data_block->data == nullptr)
        return;

    int16_t *samples =
        static_cast<int16_t *>(data_block->data);


    static float phase = 0.0f;

    const float frequencies[] = {
    440.0f,   // A4
    554.37f,  // C#5
    659.25f,  // E5
    880.0f,   // A5
    659.25f,  // E5
    554.37f,  // C#5
    440.0f    // A4
  };

    const int note_count = 7;
    static uint64_t sample_count = 0;
    const float sample_rate = 44100.0f;
    const float amplitude = 0.5f;


   
    static int note = 0;

    const uint64_t samples_per_note =
    static_cast<uint64_t>(sample_rate * 0.4f);


    uint32_t frames =
        data_block->maxsize /
        (sizeof(int16_t) * 2);

        std::cout << "Frames: "
          << frames
          << "\n";


     if (previous_timestamp != 0)
    {
        long long interval =
            timestamp - previous_timestamp;

        std::cout << "Callback interval: "
                  << interval
                  << " ns\n";
    }

    previous_timestamp = timestamp;

    for (uint32_t i = 0; i < frames; i++)
    {
        float value =
            std::sin(
                2.0f *
                3.14159265f *
                phase
            ) * amplitude;


        int16_t sample =
            static_cast<int16_t>(
                value * 32767.0f
            );


        // Left channel
        samples[i * 2] = sample;

        // Right channel
        samples[i * 2 + 1] = sample;


        phase += frequencies[note] / sample_rate;

        if (phase >= 1.0f)
          phase -= 1.0f;

      sample_count++;

      if (sample_count >= samples_per_note)
      {
          sample_count = 0;
          note++;

          if (note >= note_count)
              note = 0;
      }
    }


    // Tell PipeWire how much audio we produced
    data_block->chunk->offset = 0;

    data_block->chunk->stride =
        sizeof(int16_t) * 2;

    data_block->chunk->size =
        frames * sizeof(int16_t) * 2;


    // Give the buffer back to PipeWire
    pw_stream_queue_buffer(
        data->stream,
        b
    );
}


int main(int argc, char *argv[])
{
    pw_init(&argc, &argv);


    struct pw_main_loop *loop;

    struct pw_stream *stream;


    loop = pw_main_loop_new(nullptr);


    const struct spa_pod *params[1];


    struct pw_stream_events events = {};

    events.version =
        PW_VERSION_STREAM_EVENTS;

    events.process =
        on_process;


    struct pw_properties *props;

    props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        nullptr
    );


    AppData appData;


    stream = pw_stream_new_simple(
        pw_main_loop_get_loop(loop),
        "My Audio Stream",
        props,
        &events,
        &appData
    );


    appData.stream = stream;


    uint8_t buffer[1024];

    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(
            buffer,
            sizeof(buffer)
        );


    struct spa_audio_info_raw audio_info = {};

    audio_info.format =
        SPA_AUDIO_FORMAT_S16;

    audio_info.channels =
        2;

    audio_info.rate =
        44100;


    params[0] =
        spa_format_audio_raw_build(
            &builder,
            SPA_PARAM_EnumFormat,
            &audio_info
        );


    pw_stream_connect(
        stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,

        static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS
        ),

        params,
        1
    );


    pw_main_loop_run(loop);


    return 0;
}