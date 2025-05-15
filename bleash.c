#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <furi_hal_rtc.h>
#include <stdio.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/canvas.h>

#define TAG                        "Bleash"
#define LOG_FOLDER_PATH            "/ext/Bleash"
#define LOG_FILE_PATH              "/ext/Bleash/bleash.log"
#define RSSI_THRESHOLD             -70 // dBm
#define POLL_INTERVAL_MS           1000 // ms
#define DEFAULT_BACKGROUND_RUNNING false

typedef struct {
    FuriMessageQueue* event_queue;
    ViewPort* view_port;
    Gui* gui;
    Storage* storage;
    NotificationApp* notifications;
    bool background_running;
    int8_t last_rssi;
    bool was_connected;
} Bleash;

static void log_event(Bleash* b, int8_t rssi) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char line[64];
    size_t len = snprintf(
        line,
        sizeof(line),
        "%04d-%02d-%02d %02d:%02d:%02d: RSSI %d\n",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        rssi);

    File* f = storage_file_alloc(b->storage);
    if(f && storage_file_open(f, LOG_FILE_PATH, FSAM_WRITE, FSOM_OPEN_ALWAYS | FSOM_OPEN_APPEND)) {
        storage_file_write(f, (uint8_t*)line, len);
        storage_file_close(f);
    }
    if(f) storage_file_free(f);
}

static void draw_callback(Canvas* canvas, void* ctx) {
    Bleash* b = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    // Draw status
    canvas_draw_str(canvas, 2, 12, "BLE Leash Active:");
    canvas_draw_str(canvas, 2, 24, b->background_running ? "Monitoring" : "Paused");

    // Draw RSSI value
    char rssi_str[32];
    snprintf(rssi_str, sizeof(rssi_str), "RSSI: %d dBm", b->last_rssi);
    canvas_draw_str(canvas, 2, 36, rssi_str);

    // Draw connection status
    canvas_draw_str(canvas, 2, 48, b->was_connected ? "Connected" : "Disconnected");

    // Draw controls
    canvas_draw_str(canvas, 2, 60, "OK to toggle | Back to exit");
}

static void input_callback(InputEvent* event, void* ctx) {
    Bleash* b = ctx;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk) {
            b->background_running = !b->background_running;
            notification_message(
                b->notifications,
                b->background_running ? &sequence_set_only_green_255 : &sequence_set_only_red_255);
        } else if(event->key == InputKeyBack) {
            furi_message_queue_put(b->event_queue, &event, FuriWaitForever);
        }
    }
}

static bool bleash_init_storage(Bleash* app) {
    if(!storage_dir_exists(app->storage, LOG_FOLDER_PATH)) {
        FURI_LOG_I(TAG, "Creating log directory");
        if(!storage_common_mkdir(app->storage, LOG_FOLDER_PATH)) {
            FURI_LOG_E(TAG, "Failed to create log directory");
            return false;
        }
    }
    return true;
}

int32_t BLEASH(void* p) {
    UNUSED(p);
    Bleash b = {0};

    // Initialize app
    b.storage = furi_record_open(RECORD_STORAGE);
    if(!bleash_init_storage(&b)) {
        furi_record_close(RECORD_STORAGE);
        return 1;
    }

    b.notifications = furi_record_open(RECORD_NOTIFICATION);
    b.background_running = DEFAULT_BACKGROUND_RUNNING;
    b.last_rssi = 0;
    b.was_connected = false;

    // Set up GUI
    b.event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    b.view_port = view_port_alloc();
    b.gui = furi_record_open(RECORD_GUI);
    view_port_draw_callback_set(b.view_port, draw_callback, &b);
    view_port_input_callback_set(b.view_port, input_callback, &b);
    gui_add_view_port(b.gui, b.view_port, GuiLayerFullscreen);

    // Main monitoring loop
    InputEvent event;
    bool running = true;
    while(running) {
        // Check for exit event
        if(furi_message_queue_get(b.event_queue, &event, 100) == FuriStatusOk) {
            if(event.key == InputKeyBack) running = false;
        }

        if(b.background_running) {
            int8_t rssi = furi_hal_bt_get_rssi();
            b.last_rssi = rssi;
            b.was_connected = (rssi < 0); // Any valid RSSI indicates connection

            // Alert if signal is weak
            if(rssi < RSSI_THRESHOLD && rssi < 0) {
                notification_message(b.notifications, &sequence_set_vibro_on);
                log_event(&b, rssi);
                furi_delay_ms(100);
                notification_message(b.notifications, &sequence_reset_vibro);
            }
        }

        view_port_update(b.view_port);
        furi_delay_ms(POLL_INTERVAL_MS);
    }

    // Cleanup
    gui_remove_view_port(b.gui, b.view_port);
    view_port_free(b.view_port);
    furi_message_queue_free(b.event_queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);

    return 0;
}
