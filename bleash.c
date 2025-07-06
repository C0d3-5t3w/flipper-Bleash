#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <bt/bt_service/bt.h>
#include <bt/bt_service/bt_keys_storage.h>
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
    Bt* bt;
    bool background_running;
    int8_t last_rssi;
    bool was_connected;
    bool running;
    bool should_exit;
    FuriThread* thread;
    FuriMutex* mutex;
    volatile bool processing;
    FuriTimer* update_timer;
    bool is_active;
    BtStatus bt_status;
} Bleash;

static void log_event(Bleash* b, int8_t rssi) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char line[128];

    const char* status_str = "Unknown";
    switch(b->bt_status) {
    case BtStatusOff:
        status_str = "Off";
        break;
    case BtStatusAdvertising:
        status_str = "Advertising";
        break;
    case BtStatusConnected:
        status_str = "Connected";
        break;
    case BtStatusUnavailable:
        status_str = "Unavailable";
        break;
    }

    size_t len = snprintf(
        line,
        sizeof(line),
        "%04d-%02d-%02d %02d:%02d:%02d: BT=%s RSSI=%d\n",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        status_str,
        rssi);

    File* f = storage_file_alloc(b->storage);
    if(f && storage_file_open(f, LOG_FILE_PATH, FSAM_WRITE, FSOM_OPEN_ALWAYS | FSOM_OPEN_APPEND)) {
        storage_file_write(f, (uint8_t*)line, len);
        storage_file_close(f);
    }
    if(f) storage_file_free(f);
}

static void bt_status_changed_callback(BtStatus status, void* context) {
    Bleash* bleash = context;
    if(bleash && bleash->mutex) {
        furi_mutex_acquire(bleash->mutex, FuriWaitForever);
        bleash->bt_status = status;
        bleash->was_connected = (status == BtStatusConnected);
        furi_mutex_release(bleash->mutex);
    }
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

    // Draw connection status
    const char* status_str = "Unknown";
    switch(bleash->bt_status) {
    case BtStatusOff:
        status_str = "BT Off";
        break;
    case BtStatusAdvertising:
        status_str = "Advertising";
        break;
    case BtStatusConnected:
        status_str = "Connected";
        break;
    case BtStatusUnavailable:
        status_str = "Unavailable";
        break;
    }

    canvas_draw_str(canvas, 2, 24, status_str);

    // Draw RSSI if connected
    if(bleash->bt_status == BtStatusConnected || bleash->last_rssi > -127) {
        char rssi_str[32];
        snprintf(rssi_str, sizeof(rssi_str), "Signal: %d dBm", bleash->last_rssi);
        canvas_draw_str(canvas, 2, 36, rssi_str);

        // Draw signal strength indicator
        draw_battery_indicator(canvas, 90, 29, bleash->last_rssi);
    }

    // Draw monitoring status
    canvas_set_font(canvas, FontPrimary);
    if(bleash->background_running) {
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter, "Monitoring ON");
    } else {
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignCenter, "Monitoring OFF");
    }

    // Draw controls hint
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 55, "OK: Toggle");
    canvas_draw_str_aligned(canvas, 126, 55, AlignRight, AlignBottom, "Back: Hide");
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Long Back: Exit");
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
    if(!bleash) return -1;

    bleash->processing = true;
    FURI_LOG_I(TAG, "Worker thread started");

    while(!bleash->should_exit) {
        if(!bleash->mutex) {
            FURI_LOG_E(TAG, "Mutex is null, exiting worker");
            break;
        }

        furi_mutex_acquire(bleash->mutex, FuriWaitForever);

        if(bleash->background_running) {
            // Check BLE connection status safely
            bool bt_active = false;
            bool bt_available = false;

            // Try to check BT status with error handling
            if(furi_hal_bt_is_gatt_gap_supported()) {
                bt_active = furi_hal_bt_is_active();
                bt_available = true;
            } else {
                FURI_LOG_W(TAG, "BT GATT/GAP not supported");
                bt_available = false;
            }

            if(bt_available) {
                bleash->was_connected = (bleash->bt_status == BtStatusConnected);

                // Calculate mock RSSI based on connection state
                if(bleash->was_connected) {
                    bleash->last_rssi = -45; // Mock good signal when connected
                } else if(bt_active) {
                    bleash->last_rssi = -75; // Mock weak signal when advertising
                } else {
                    bleash->last_rssi = -127; // No signal
                }

                // Alert on weak signal if connected
                if(bleash->was_connected && bleash->last_rssi < RSSI_THRESHOLD) {
                    FURI_LOG_W(TAG, "Weak signal detected: %d dBm", bleash->last_rssi);

                    // Safe notification calls
                    if(bleash->notifications) {
                        notification_message(bleash->notifications, &sequence_set_vibro_on);
                        log_event(bleash, bleash->last_rssi);
                        furi_delay_ms(200);
                        notification_message(bleash->notifications, &sequence_reset_vibro);

                        // Brief red blink
                        notification_message(bleash->notifications, &sequence_blink_red_10);
                    }
                }

                // Alert on complete disconnection
                if(!bt_active && bleash->was_connected) {
                    FURI_LOG_W(TAG, "BLE connection lost");

                    if(bleash->notifications) {
                        // Double vibrate pattern
                        notification_message(bleash->notifications, &sequence_set_vibro_on);
                        furi_delay_ms(150);
                        notification_message(bleash->notifications, &sequence_reset_vibro);
                        furi_delay_ms(100);
                        notification_message(bleash->notifications, &sequence_set_vibro_on);
                        furi_delay_ms(150);
                        notification_message(bleash->notifications, &sequence_reset_vibro);

                        log_event(bleash, -127);
                    }
                }
            } else {
                // BT not available
                bleash->last_rssi = -127;
                bleash->was_connected = false;
                FURI_LOG_W(TAG, "BT not available");
            }
        }

        furi_mutex_release(bleash->mutex);
        furi_delay_ms(POLL_INTERVAL_MS);
    }

    FURI_LOG_I(TAG, "Worker thread stopping");
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

            // Provide feedback with LED color
            notification_message(
                b->notifications,
                b->background_running ? &sequence_blink_green_10 : &sequence_blink_red_10);
        } else if(event->key == InputKeyBack) {
            // Just hide GUI, keep monitoring in background
            b->running = false;
        }
    } else if(event->type == InputTypeLong) {
        if(event->key == InputKeyBack) {
            // Long press Back = full exit
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

    // Initialize storage first
    bleash->storage = furi_record_open(RECORD_STORAGE);
    if(!bleash_init_storage(bleash)) {
        furi_record_close(RECORD_STORAGE);
        free(bleash);
        return 1;
    }

    // Initialize BT service
    bleash->bt = furi_record_open(RECORD_BT);
    if(!bleash->bt) {
        FURI_LOG_E(TAG, "Failed to open BT record");
        furi_record_close(RECORD_STORAGE);
        free(bleash);
        return 1;
    }

    // Initialize other services
    bleash->notifications = furi_record_open(RECORD_NOTIFICATION);
    bleash->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    // Set up BT status callback
    bt_set_status_changed_callback(bleash->bt, bt_status_changed_callback, bleash);

    // Load saved state and initialize
    load_state(bleash);
    bleash->last_rssi = -127; // Initialize to no signal
    bleash->was_connected = false;
    bleash->bt_status = BtStatusOff;

    // Set up GUI
    bleash->event_queue = furi_message_queue_alloc(8, sizeof(BleashEvent));
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

    // Main event loop
    BleashEvent event;
    while(bleash->running) {
        if(furi_message_queue_get(bleash->event_queue, &event, 100) == FuriStatusOk) {
            if(event.type == BleashEventTypeKey) {
                if(event.input.key == InputKeyBack) {
                    break;
                }
            } else if(event.type == BleashEventTypeTick) {
                view_port_update(bleash->view_port);
            }
        }
    }

    // Clean up GUI (but keep worker running if not fully exiting)
    if(bleash->update_timer) {
        furi_timer_stop(bleash->update_timer);
        furi_timer_free(bleash->update_timer);
        bleash->update_timer = NULL;
    }

    if(bleash->gui && bleash->view_port) {
        gui_remove_view_port(bleash->gui, bleash->view_port);
    }

    if(bleash->view_port) {
        view_port_free(bleash->view_port);
        bleash->view_port = NULL;
    }

    if(bleash->event_queue) {
        furi_message_queue_free(bleash->event_queue);
        bleash->event_queue = NULL;
    }

    if(bleash->gui) {
        furi_record_close(RECORD_GUI);
        bleash->gui = NULL;
    }

    // Check if we should fully exit or just hide GUI
    if(bleash->should_exit) {
        FURI_LOG_I(TAG, "Fully exiting app");

        // Signal worker to stop
        bleash->should_exit = true;

        // Wait for worker to finish
        if(bleash->thread) {
            furi_thread_join(bleash->thread);
            furi_thread_free(bleash->thread);
        }

        // Clean up remaining services
        if(bleash->bt) {
            bt_set_status_changed_callback(bleash->bt, NULL, NULL);
            furi_record_close(RECORD_BT);
        }

        if(bleash->notifications) {
            furi_record_close(RECORD_NOTIFICATION);
        }

        if(bleash->storage) {
            furi_record_close(RECORD_STORAGE);
        }

        if(bleash->mutex) {
            furi_mutex_free(bleash->mutex);
        }

        free(bleash);
    } else {
        FURI_LOG_I(TAG, "Hiding GUI, keeping worker running in background");

        // Keep worker and core services running
        // The app will continue monitoring in background

        // Note: In a real implementation, you might want to set up a way
        // to restore the GUI later, perhaps through a system menu entry
        // or by detecting when the app is launched again
    }

    return 0;
}
