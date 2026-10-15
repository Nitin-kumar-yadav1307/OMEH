/*
 * sync_registry.h — device discovery + single-instance guard.
 *
 * Single-TU module, part of pipewire_sync.cpp. Included AFTER sync_config.h
 * and sync_types.h. Owns the PipeWire registry listeners that discover
 * Bluetooth sink nodes (and the virtual sink when it appears), plus
 * another_instance_running() which refuses to start a second copy.
 */
#pragma once


/* refuse to run twice — overlapping instances fight over routing */
static bool another_instance_running()
{
    FILE *p = popen("pgrep -x -c pipewire_sync 2>/dev/null", "r");
    if (!p) return false;
    char line[64] = {};
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    pclose(p);
    /* 1 match = ourselves; more than 1 = someone else */
    return ok && atoi(line) > 1;
}

/* ------------------------------------------------------------------ */
/* Registry: discover every Bluetooth sink node                        */
/* ------------------------------------------------------------------ */
static void registry_event_global(void *data, uint32_t id,
                                  uint32_t permissions,
                                  const char *type, uint32_t version,
                                  const struct spa_dict *props)
{
    (void)data; (void)permissions; (void)version;

    if (!props || strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    /* Did our virtual "Sync Master" sink appear? */
    const char *cls_check = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *nm_check  = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (nm_check && strstr(nm_check, "sync_master") &&
        cls_check && strstr(cls_check, "Audio/Sink"))
    {
        app.virtual_sink_id = id;
        /* WirePlumber may rename it (e.g. "input.sync_master") —
           remember the ACTUAL node name for reliable pinning */
        app.virtual_sink_name = nm_check;
        fprintf(stdout, "[registry] virtual sink ready: %s (node %u)\n",
                nm_check, id);
        fflush(stdout);
        return;   /* never treat it as a Bluetooth device */
    }

    const char *media_class =
        spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class || strcmp(media_class, "Audio/Sink") != 0)
        return;

    /* Bluetooth only */
    const char *dev_api   = spa_dict_lookup(props, PW_KEY_DEVICE_API);
    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);

    bool is_bt =
        (dev_api && strstr(dev_api, "bluez5")) ||
        (node_name && strncmp(node_name, "bluez_output", 12) == 0);

    if (!is_bt)
        return;

    /* Skip our own helper nodes just in case */
    if (node_name && strstr(node_name, "sync-"))
        return;

    DeviceOut *dev = new DeviceOut();
    dev->node_id = id;
    dev->node_id_str = std::to_string(id);

    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    dev->name = (desc && *desc) ? desc :
                (node_name ? node_name : dev->node_id_str);
    dev->target_name = node_name ? node_name : dev->node_id_str;

    app.devices.push_back(dev);

    fprintf(stdout, "[registry] found Bluetooth sink: %s (node %u)\n",
            dev->name.c_str(), id);
    fflush(stdout);
}

static void registry_event_remove(void *data, uint32_t id)
{
    (void)data;

    /* an earbud disappeared: mark the device gone, destroy its stream.
       When it comes back the global handler re-adds it (probing again)
       and any user-supplied trim survives, keyed by node name. */
    for (DeviceOut *dev : app.devices)
    {
        if (dev->node_id == id && !dev->gone)
        {
            fprintf(stdout, "[registry] device gone: %s (node %u)\n",
                    dev->name.c_str(), id);
            fflush(stdout);
            dev->gone = true;
            if (dev->stream)
            {
                pw_stream_destroy(dev->stream);
                dev->stream = nullptr;
            }
            dev->started   = false;
            dev->measuring = true;
            dev->meas_sum = 0.0; dev->meas_count = 0; dev->meas_frames = 0;
            dev->meas_q_sum = 0.0; dev->meas_q_count = 0;
            dev->avg_level = -1.0; dev->ratio = 1.0;
            auto it = app.trim_by_name.find(dev->target_name);
            if (it != app.trim_by_name.end())
                dev->extra_delay_ms = it->second;
            break;
        }
    }

    if (id == app.virtual_sink_id)
    {
        app.virtual_sink_id = 0;
        app.virtual_sink_name.clear();
    }
}

static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global  = registry_event_global,
    .global_remove = registry_event_remove,
};

