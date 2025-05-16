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
#define STATE_FILE_PATH            "/ext/Bleash/bleash.state"
#define RSSI_THRESHOLD             -70
#define POLL_INTERVAL_MS           1000
#define DEFAULT_BACKGROUND_RUNNING false
#define BLE_APP_NAME               "BLE Leash"
#define BACKGROUND_WORKER_STACK    2048
#define VIEW_UPDATE_INTERVAL       500

typedef enum {
    BleashEventTypeKey,
    BleashEventTypeTick,
    BleashEventTypeExit,
} BleashEventType;

typedef struct {
    BleashEventType type;
    InputEvent input;
} BleashEvent;

typedef struct {
    FuriMessageQueue* event_queue;
    ViewPort* view_port;
    Gui* gui;
    Storage* storage;
    NotificationApp* notifications;
    bool background_running;
    int8_t last_rssi;
    bool was_connected;
    bool running;
    bool should_exit; // Add new flag for clean exit
    FuriThread* thread;
    FuriMutex* mutex;
    volatile bool processing; // Add volatile flag for thread sync
    FuriTimer* update_timer;
    bool is_active;
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

static void draw_battery_indicator(Canvas* canvas, int x, int y, int8_t rssi) {
    uint8_t bars = ((rssi + 130) / 10);
    if(bars > 5) bars = 5;

    // Draw battery outline
    canvas_draw_frame(canvas, x, y, 15, 8);
    canvas_draw_box(canvas, x + 15, y + 2, 2, 4);

    // Draw battery bars
    for(uint8_t i = 0; i < bars; i++) {
        canvas_draw_box(canvas, x + 2 + (i * 3), y + 2, 2, 4);
    }
}

static void draw_status_view(Canvas* canvas, Bleash* bleash) {
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    // Draw header
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, BLE_APP_NAME);
    canvas_draw_line(canvas, 0, 11, 128, 11);

    // Draw RSSI and connection status
    char rssi_str[32];
    snprintf(rssi_str, sizeof(rssi_str), "Signal: %d dBm", bleash->last_rssi);
    canvas_draw_str(canvas, 2, 24, rssi_str);

    // Draw signal strength indicator
    draw_battery_indicator(canvas, 90, 17, bleash->last_rssi);

    // Draw status
    canvas_set_font(canvas, FontPrimary);
    if(bleash->background_running) {
        canvas_draw_str_aligned(
            canvas,
            64,
            38,
            AlignCenter,
            AlignCenter,
            bleash->was_connected ? "Connected" : "No Device");
    } else {
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "Monitoring Off");
    }

    // Draw controls hint
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 63, "OK: Toggle");
    canvas_draw_str_aligned(canvas, 126, 63, AlignRight, AlignBottom, "Back: Hide");
}

static void bleash_update_timer_callback(void* context) {
    Bleash* bleash = context;
    BleashEvent event = {.type = BleashEventTypeTick};
    furi_message_queue_put(bleash->event_queue, &event, 0);
}

static void draw_callback(Canvas* canvas, void* ctx) {
    Bleash* b = ctx;
    draw_status_view(canvas, b);
}

static void save_state(Bleash* b) {
    File* file = storage_file_alloc(b->storage);
    if(storage_file_open(file, STATE_FILE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, &b->background_running, sizeof(bool));
        storage_file_close(file);
    }
    storage_file_free(file);
}

static void load_state(Bleash* b) {
    File* file = storage_file_alloc(b->storage);
    if(storage_file_open(file, STATE_FILE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_read(file, &b->background_running, sizeof(bool));
        storage_file_close(file);
    }
    storage_file_free(file);
}

static int32_t bleash_worker(void* context) {
    Bleash* bleash = context;
    bleash->processing = true;

    while(!bleash->should_exit) {
        furi_mutex_acquire(bleash->mutex, FuriWaitForever);
        if(bleash->background_running) {
            int8_t rssi = furi_hal_bt_get_rssi();
            bleash->last_rssi = rssi;
            bleash->was_connected = (rssi < 0);

            if(rssi < RSSI_THRESHOLD && rssi < 0) {
                notification_message(bleash->notifications, &sequence_set_vibro_on);
                log_event(bleash, rssi);
                furi_delay_ms(100);
                notification_message(bleash->notifications, &sequence_reset_vibro);
            }
        }
        furi_mutex_release(bleash->mutex);
        furi_delay_ms(POLL_INTERVAL_MS);
    }

    bleash->processing = false;
    return 0;
}

static void input_callback(InputEvent* event, void* ctx) {
    Bleash* b = ctx;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk) {
            furi_mutex_acquire(b->mutex, FuriWaitForever);
            b->background_running = !b->background_running;
            save_state(b);
            furi_mutex_release(b->mutex);
            notification_message(
                b->notifications,
                b->background_running ? &sequence_set_only_green_255 : &sequence_set_only_red_255);
        } else if(event->key == InputKeyBack) {
            b->should_exit = true;
            b->running = false;
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
    Bleash* bleash = malloc(sizeof(Bleash));
    memset(bleash, 0, sizeof(Bleash));

    bleash->storage = furi_record_open(RECORD_STORAGE);
    if(!bleash_init_storage(bleash)) {
        furi_record_close(RECORD_STORAGE);
        free(bleash);
        return 1;
    }

    bleash->notifications = furi_record_open(RECORD_NOTIFICATION);
    bleash->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    load_state(bleash);
    bleash->last_rssi = 0;
    bleash->was_connected = false;

    bleash->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    bleash->view_port = view_port_alloc();
    bleash->gui = furi_record_open(RECORD_GUI);
    view_port_draw_callback_set(bleash->view_port, draw_callback, bleash);
    view_port_input_callback_set(bleash->view_port, input_callback, bleash);
    gui_add_view_port(bleash->gui, bleash->view_port, GuiLayerFullscreen);

    bleash->running = true;
    bleash->should_exit = false;
    bleash->processing = false;

    // Setup view update timer
    bleash->update_timer =
        furi_timer_alloc(bleash_update_timer_callback, FuriTimerTypePeriodic, bleash);
    furi_timer_start(bleash->update_timer, VIEW_UPDATE_INTERVAL);

    // Start background worker
    bleash->thread =
        furi_thread_alloc_ex("BleashWorker", BACKGROUND_WORKER_STACK, bleash_worker, bleash);
    furi_thread_start(bleash->thread);

    BleashEvent event;
    while(bleash->running) {
        if(furi_message_queue_get(bleash->event_queue, &event, 100) == FuriStatusOk) {
            if(event.type == BleashEventTypeKey) {
                if(event.input.key == InputKeyBack) {
                    // Just hide UI but keep monitoring
                    break;
                }
            } else if(event.type == BleashEventTypeTick) {
                view_port_update(bleash->view_port);
            }
        }
    }

    // Cleanup UI but keep worker running
    furi_timer_free(bleash->update_timer);
    gui_remove_view_port(bleash->gui, bleash->view_port);
    view_port_free(bleash->view_port);
    furi_message_queue_free(bleash->event_queue);
    furi_record_close(RECORD_GUI);

    // Keep other services for background operation
    return 0;
}
