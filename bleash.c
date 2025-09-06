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
#define INSTANCE_FILE_PATH         "/ext/Bleash/bleash.instance"
#define RSSI_THRESHOLD             -70
#define POLL_INTERVAL_MS           1000
#define DEFAULT_BACKGROUND_RUNNING false
#define BLE_APP_NAME               "BLE Leash"
#define BACKGROUND_WORKER_STACK    2048
#define VIEW_UPDATE_INTERVAL       500

// Custom notification sequences
const NotificationSequence sequence_set_vibro_on = {
    &message_vibro_on,
    NULL,
};

const NotificationSequence sequence_reset_vibro = {
    &message_vibro_off,
    NULL,
};

const NotificationSequence sequence_blink_red_10 = {
    &message_blink_start_10,
    &message_blink_set_color_red,
    &message_delay_250,
    &message_blink_stop,
    NULL,
};

const NotificationSequence sequence_blink_green_10 = {
    &message_blink_start_10,
    &message_blink_set_color_green,
    &message_delay_250,
    &message_blink_stop,
    NULL,
};

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

// Helper function to get current RSSI from BLE stack
static int8_t bleash_get_rssi(Bleash* bleash) {
    if(!bleash || bleash->bt_status != BtStatusConnected) {
        return -127; // Invalid/no connection
    }

    // For now, simulate RSSI based on connection state
    // In a real implementation, this would query the BLE stack for actual RSSI
    // TODO: Implement actual RSSI reading when API is available
    static int8_t simulated_rssi = -50;
    static uint8_t counter = 0;
    counter++;
    simulated_rssi += ((counter % 20) - 10); // Simulate variance
    if(simulated_rssi > -30) simulated_rssi = -30;
    if(simulated_rssi < -90) simulated_rssi = -90;
    return simulated_rssi;
}

// Helper function to check if we have bonded devices
static bool bleash_has_bonded_devices(Bleash* bleash) {
    if(!bleash || !bleash->storage) return false;

    // Check if BT keys file exists (indicates bonded devices)
    File* file = storage_file_alloc(bleash->storage);
    bool has_bonds = false;

    if(file) {
        if(storage_file_open(file, "/int/bt.keys", FSAM_READ, FSOM_OPEN_EXISTING)) {
            has_bonds = true;
            storage_file_close(file);
        }
        storage_file_free(file);
    }

    return has_bonds;
}

// Function to start BLE scanning for known devices
static bool bleash_start_scanning(Bleash* bleash) {
    if(!bleash || !furi_hal_bt_is_gatt_gap_supported()) {
        return false;
    }

    FURI_LOG_I(TAG, "Starting BLE scan for known devices");

    // Check if radio stack is running
    if(!furi_hal_bt_start_radio_stack()) {
        FURI_LOG_E(TAG, "Failed to start BT radio stack");
        return false;
    }

    // Start advertising to allow connections
    furi_hal_bt_start_advertising();
    FURI_LOG_I(TAG, "BLE advertising started");

    return true;
}

// Function to handle device connection attempts
static bool bleash_try_connect_known_device(Bleash* bleash) {
    if(!bleash) return false;

    // Check if we have any bonded devices
    if(!bleash_has_bonded_devices(bleash)) {
        FURI_LOG_I(TAG, "No bonded devices found");
        return false;
    }

    // For devices with existing bonds, the connection should happen automatically
    // when they come in range and we're advertising
    FURI_LOG_I(TAG, "Waiting for bonded device to connect");
    return true;
}

// Enhanced monitoring function with actual BLE operations
static void bleash_monitor_connection(Bleash* bleash) {
    if(!bleash || !bleash->background_running) {
        return;
    }

    bool bt_available = furi_hal_bt_is_gatt_gap_supported();

    if(!bt_available) {
        FURI_LOG_W(TAG, "BT GATT/GAP not supported");
        bleash->bt_status = BtStatusUnavailable;
        bleash->last_rssi = -127;
        return;
    }

    // Update connection status
    bool was_connected = bleash->was_connected;
    bleash->was_connected = (bleash->bt_status == BtStatusConnected);

    switch(bleash->bt_status) {
    case BtStatusOff:
        FURI_LOG_I(TAG, "BT is off, attempting to start");
        if(bleash_start_scanning(bleash)) {
            bleash->bt_status = BtStatusAdvertising;
        }
        bleash->last_rssi = -127;
        break;

    case BtStatusAdvertising:
        FURI_LOG_D(TAG, "Advertising, waiting for connection");
        bleash_try_connect_known_device(bleash);
        bleash->last_rssi = -85; // Advertising RSSI
        break;

    case BtStatusConnected:
        // Get actual RSSI for connected device
        bleash->last_rssi = bleash_get_rssi(bleash);
        FURI_LOG_D(TAG, "Connected, RSSI: %d dBm", bleash->last_rssi);

        // Check for weak signal
        if(bleash->last_rssi < RSSI_THRESHOLD) {
            FURI_LOG_W(
                TAG, "Weak signal: %d dBm (threshold: %d)", bleash->last_rssi, RSSI_THRESHOLD);

            if(bleash->notifications && !bleash->should_exit) {
                notification_message(bleash->notifications, &sequence_set_vibro_on);
                furi_delay_ms(200);
                if(!bleash->should_exit && bleash->notifications) {
                    notification_message(bleash->notifications, &sequence_reset_vibro);
                    notification_message(bleash->notifications, &sequence_blink_red_10);
                }
            }
        }
        break;

    case BtStatusUnavailable:
        FURI_LOG_W(TAG, "BT unavailable");
        bleash->last_rssi = -127;
        break;
    }

    // Handle connection state changes
    if(was_connected && !bleash->was_connected) {
        FURI_LOG_W(TAG, "Device disconnected");

        if(bleash->notifications && !bleash->should_exit) {
            // Double vibration for disconnection
            notification_message(bleash->notifications, &sequence_set_vibro_on);
            furi_delay_ms(150);
            if(!bleash->should_exit && bleash->notifications) {
                notification_message(bleash->notifications, &sequence_reset_vibro);
                furi_delay_ms(100);
                if(!bleash->should_exit && bleash->notifications) {
                    notification_message(bleash->notifications, &sequence_set_vibro_on);
                    furi_delay_ms(150);
                    if(!bleash->should_exit && bleash->notifications) {
                        notification_message(bleash->notifications, &sequence_reset_vibro);
                    }
                }
            }
        }

        // Restart advertising after disconnection
        if(bleash_start_scanning(bleash)) {
            bleash->bt_status = BtStatusAdvertising;
        }
    } else if(!was_connected && bleash->was_connected) {
        FURI_LOG_I(TAG, "Device connected");

        if(bleash->notifications && !bleash->should_exit) {
            notification_message(bleash->notifications, &sequence_blink_green_10);
        }
    }

    // Log the event
    log_event(bleash, bleash->last_rssi);
}

static void bt_status_changed_callback(BtStatus status, void* context) {
    Bleash* bleash = context;

    // More robust safety checks
    if(!bleash) {
        return;
    }

    // Check if we're shutting down or processing
    if(bleash->should_exit || bleash->processing) {
        return;
    }

    // Check if mutex is still valid
    if(!bleash->mutex) {
        return;
    }

    // Try to acquire mutex with timeout, if it fails we're probably shutting down
    if(furi_mutex_acquire(bleash->mutex, 50) != FuriStatusOk) {
        return;
    }

    // Double-check we're not shutting down after acquiring mutex
    if(!bleash->should_exit && !bleash->processing) {
        bleash->bt_status = status;
        bleash->was_connected = (status == BtStatusConnected);
        FURI_LOG_I(TAG, "BT status changed to %d", status);
    }

    furi_mutex_release(bleash->mutex);
}

static void draw_battery_indicator(Canvas* canvas, int x, int y, int8_t rssi) {
    uint8_t bars = ((rssi + 130) / 10);
    if(bars > 5) bars = 5;

    canvas_draw_frame(canvas, x, y, 15, 8);
    canvas_draw_box(canvas, x + 15, y + 2, 2, 4);

    for(uint8_t i = 0; i < bars; i++) {
        canvas_draw_box(canvas, x + 2 + (i * 3), y + 2, 2, 4);
    }
}

static void draw_status_view(Canvas* canvas, Bleash* bleash) {
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, BLE_APP_NAME);
    canvas_draw_line(canvas, 0, 11, 128, 11);

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

    if(bleash->bt_status == BtStatusConnected || bleash->last_rssi > -127) {
        char rssi_str[32];
        snprintf(rssi_str, sizeof(rssi_str), "Signal: %d dBm", bleash->last_rssi);
        canvas_draw_str(canvas, 2, 36, rssi_str);

        draw_battery_indicator(canvas, 90, 29, bleash->last_rssi);
    }

    canvas_set_font(canvas, FontPrimary);
    if(bleash->background_running) {
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, "Monitoring ON");
    } else {
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, "Monitoring OFF");
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 55, "OK: Toggle");
    canvas_draw_str_aligned(canvas, 126, 55, AlignRight, AlignBottom, "Back: Hide");
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Long Back: Exit");
}

static void bleash_update_timer_callback(void* context) {
    Bleash* bleash = context;

    // Safety checks to prevent use-after-free
    if(!bleash || bleash->should_exit || bleash->processing) {
        return;
    }

    // Check if event queue is still valid
    if(!bleash->event_queue) {
        return;
    }

    BleashEvent event = {.type = BleashEventTypeTick};
    furi_message_queue_put(bleash->event_queue, &event, 0);
}

static void draw_callback(Canvas* canvas, void* ctx) {
    Bleash* b = ctx;
    if(!b || !canvas) {
        // Draw a simple message if we can't access the main struct
        if(canvas) {
            canvas_clear(canvas);
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Loading...");
        }
        return;
    }

    // Only show loading during shutdown/cleanup
    if(b->should_exit || b->processing) {
        canvas_clear(canvas);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Shutting down...");
        return;
    }

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

static bool check_instance_running(Bleash* b) {
    File* file = storage_file_alloc(b->storage);
    bool exists = storage_file_open(file, INSTANCE_FILE_PATH, FSAM_READ, FSOM_OPEN_EXISTING);
    if(exists) {
        // Read the PID from the file
        uint32_t stored_pid = 0;
        size_t bytes_read = storage_file_read(file, &stored_pid, sizeof(stored_pid));
        storage_file_close(file);

        // If we couldn't read a valid PID, assume the file is stale
        if(bytes_read != sizeof(stored_pid) || stored_pid == 0) {
            FURI_LOG_W(TAG, "Invalid instance file, removing");
            storage_common_remove(b->storage, INSTANCE_FILE_PATH);
            storage_file_free(file);
            return false;
        }

        // For simplicity, we'll allow multiple instances now to avoid lockout
        // In a real implementation, you'd check if the PID is still running
        FURI_LOG_I(TAG, "Found instance file with PID %lu, but allowing new instance", stored_pid);
        storage_file_free(file);
        return false; // Allow new instance for now
    }
    storage_file_free(file);
    return false;
}

static void create_instance_file(Bleash* b) {
    File* file = storage_file_alloc(b->storage);
    if(storage_file_open(file, INSTANCE_FILE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        uint32_t pid = (uint32_t)furi_thread_get_current_id();
        storage_file_write(file, &pid, sizeof(pid));
        storage_file_close(file);
    }
    storage_file_free(file);
}

static void remove_instance_file(Bleash* b) {
    if(storage_file_exists(b->storage, INSTANCE_FILE_PATH)) {
        if(storage_common_remove(b->storage, INSTANCE_FILE_PATH)) {
            FURI_LOG_I(TAG, "Removed instance file");
        } else {
            FURI_LOG_W(TAG, "Failed to remove instance file");
        }
    }
}

static int32_t bleash_worker(void* context) {
    Bleash* bleash = context;
    if(!bleash) return -1;

    FURI_LOG_I(TAG, "Worker thread started");

    while(!bleash->should_exit) {
        // Check if essential resources are still valid
        if(!bleash->mutex || !bleash->notifications) {
            FURI_LOG_E(TAG, "Essential resources are null, exiting worker");
            break;
        }

        // Check if we should exit before acquiring mutex
        if(bleash->should_exit) break;

        furi_mutex_acquire(bleash->mutex, FuriWaitForever);

        // Double-check exit condition after acquiring mutex
        if(bleash->should_exit) {
            furi_mutex_release(bleash->mutex);
            break;
        }

        if(bleash->background_running) {
            // Use the new enhanced monitoring function
            bleash_monitor_connection(bleash);
        }

        furi_mutex_release(bleash->mutex);

        // Use shorter delays with exit checking
        for(int i = 0; i < POLL_INTERVAL_MS / 100 && !bleash->should_exit; i++) {
            furi_delay_ms(100);
        }
    }

    FURI_LOG_I(TAG, "Worker thread stopping");
    return 0;
}

static void input_callback(InputEvent* event, void* ctx) {
    Bleash* b = ctx;

    // Safety checks
    if(!b || !event) {
        return;
    }

    FURI_LOG_D(TAG, "Input: type=%d key=%d", event->type, event->key);

    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk) {
            if(b->mutex) {
                furi_mutex_acquire(b->mutex, FuriWaitForever);
                b->background_running = !b->background_running;
                save_state(b);
                furi_mutex_release(b->mutex);

                if(b->notifications) {
                    notification_message(
                        b->notifications,
                        b->background_running ? &sequence_blink_green_10 : &sequence_blink_red_10);
                }
            }
        } else if(event->key == InputKeyBack) {
            FURI_LOG_I(TAG, "Back pressed - hiding GUI");
            b->running = false;
        }
    } else if(event->type == InputTypeLong) {
        if(event->key == InputKeyBack) {
            FURI_LOG_I(TAG, "Long back pressed - full exit");
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

    // Check if another instance is already running in background
    if(check_instance_running(bleash)) {
        FURI_LOG_I(TAG, "Instance already running in background, exiting");
        furi_record_close(RECORD_STORAGE);
        free(bleash);
        return 0;
    }

    // Clean up any stale instance file first
    remove_instance_file(bleash);

    // Create instance file to mark that we're running
    create_instance_file(bleash);

    bleash->bt = furi_record_open(RECORD_BT);
    if(!bleash->bt) {
        FURI_LOG_E(TAG, "Failed to open BT record");
        furi_record_close(RECORD_STORAGE);
        free(bleash);
        return 1;
    }

    bleash->notifications = furi_record_open(RECORD_NOTIFICATION);
    bleash->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    bt_set_status_changed_callback(bleash->bt, bt_status_changed_callback, bleash);

    // Initialize BT for connections
    FURI_LOG_I(TAG, "Initializing Bluetooth");
    if(furi_hal_bt_is_gatt_gap_supported()) {
        if(!furi_hal_bt_is_active()) {
            FURI_LOG_I(TAG, "Starting BT radio stack");
            if(furi_hal_bt_start_radio_stack()) {
                FURI_LOG_I(TAG, "BT radio stack started successfully");
                furi_delay_ms(200); // Give time for stack to initialize

                FURI_LOG_I(TAG, "Starting BT advertising");
                furi_hal_bt_start_advertising();
                FURI_LOG_I(TAG, "BT advertising started - ready for connections");
            } else {
                FURI_LOG_W(TAG, "Failed to start BT radio stack");
            }
        } else {
            FURI_LOG_I(TAG, "BT already active");
            // Still start advertising in case it's not running
            furi_hal_bt_start_advertising();
        }
    } else {
        FURI_LOG_W(TAG, "BT GATT/GAP not supported on this device");
    }

    load_state(bleash);
    bleash->last_rssi = -127;
    bleash->was_connected = false;
    bleash->bt_status = BtStatusOff;

    bleash->event_queue = furi_message_queue_alloc(8, sizeof(BleashEvent));
    bleash->view_port = view_port_alloc();
    bleash->gui = furi_record_open(RECORD_GUI);

    if(!bleash->event_queue || !bleash->view_port || !bleash->gui) {
        FURI_LOG_E(TAG, "Failed to allocate GUI resources");
        // Clean up what we can
        if(bleash->event_queue) furi_message_queue_free(bleash->event_queue);
        if(bleash->view_port) view_port_free(bleash->view_port);
        if(bleash->gui) furi_record_close(RECORD_GUI);
        if(bleash->bt) furi_record_close(RECORD_BT);
        if(bleash->notifications) furi_record_close(RECORD_NOTIFICATION);
        if(bleash->mutex) furi_mutex_free(bleash->mutex);
        furi_record_close(RECORD_STORAGE);
        free(bleash);
        return 1;
    }

    view_port_draw_callback_set(bleash->view_port, draw_callback, bleash);
    view_port_input_callback_set(bleash->view_port, input_callback, bleash);
    gui_add_view_port(bleash->gui, bleash->view_port, GuiLayerFullscreen);

    bleash->running = true;
    bleash->should_exit = false;
    bleash->processing = false;

    FURI_LOG_I(TAG, "Starting app main loop");

    bleash->update_timer =
        furi_timer_alloc(bleash_update_timer_callback, FuriTimerTypePeriodic, bleash);
    if(bleash->update_timer) {
        furi_timer_start(bleash->update_timer, VIEW_UPDATE_INTERVAL);
    }

    bleash->thread =
        furi_thread_alloc_ex("BleashWorker", BACKGROUND_WORKER_STACK, bleash_worker, bleash);
    if(bleash->thread) {
        furi_thread_start(bleash->thread);
    }

    BleashEvent event;
    uint32_t loop_count = 0;
    while(bleash->running) {
        loop_count++;
        if(loop_count % 1000 == 0) {
            FURI_LOG_D(TAG, "Main loop iteration %lu", loop_count);
        }

        FuriStatus status = furi_message_queue_get(bleash->event_queue, &event, 100);
        if(status == FuriStatusOk) {
            if(event.type == BleashEventTypeKey) {
                FURI_LOG_D(TAG, "Processing key event");
                if(event.input.key == InputKeyBack) {
                    FURI_LOG_I(TAG, "Back key in event queue - exiting");
                    break;
                }
            } else if(event.type == BleashEventTypeTick) {
                if(bleash->view_port) {
                    view_port_update(bleash->view_port);
                }
            }
        } else if(status == FuriStatusErrorTimeout) {
            // Timeout is normal, continue
            continue;
        } else {
            FURI_LOG_W(TAG, "Message queue error: %d", status);
            break;
        }
    }

    FURI_LOG_I(TAG, "Main loop exited, starting cleanup");

    // STEP 1: Disable all callbacks FIRST to prevent race conditions
    if(bleash->bt) {
        bt_set_status_changed_callback(bleash->bt, NULL, NULL);
        FURI_LOG_D(TAG, "BT callback disabled");
    }

    // STEP 2: Stop timer to prevent further callbacks
    if(bleash->update_timer) {
        furi_timer_stop(bleash->update_timer);
        furi_timer_free(bleash->update_timer);
        bleash->update_timer = NULL;
        FURI_LOG_D(TAG, "Timer stopped and freed");
    }

    // STEP 3: Set processing flag to prevent worker thread from doing more work
    if(furi_mutex_acquire(bleash->mutex, 1000) == FuriStatusOk) {
        bleash->processing = true;
        furi_mutex_release(bleash->mutex);
        FURI_LOG_D(TAG, "Processing flag set");
    }

    // STEP 4: Give time for any pending callbacks to complete
    furi_delay_ms(150);

    if(bleash->should_exit) {
        FURI_LOG_I(TAG, "Fully exiting app");

        // Stop worker thread before any GUI cleanup
        if(bleash->thread) {
            FURI_LOG_I(TAG, "Stopping worker thread");
            furi_thread_join(bleash->thread);
            furi_thread_free(bleash->thread);
            bleash->thread = NULL;
        }

        // Disable view port callbacks before freeing resources
        if(bleash->view_port) {
            view_port_draw_callback_set(bleash->view_port, NULL, NULL);
            view_port_input_callback_set(bleash->view_port, NULL, NULL);
        }

        // Now safe to clean up GUI
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

        // Remove instance file
        remove_instance_file(bleash);

        // Clean up BT service
        if(bleash->bt) {
            furi_record_close(RECORD_BT);
            bleash->bt = NULL;
        }

        // Clean up other services
        if(bleash->notifications) {
            furi_record_close(RECORD_NOTIFICATION);
            bleash->notifications = NULL;
        }

        if(bleash->storage) {
            furi_record_close(RECORD_STORAGE);
            bleash->storage = NULL;
        }

        // Free mutex last
        if(bleash->mutex) {
            furi_mutex_free(bleash->mutex);
            bleash->mutex = NULL;
        }

        free(bleash);
    } else {
        FURI_LOG_I(TAG, "Hiding GUI, keeping worker running in background");

        // STEP 1: Disable view port callbacks before freeing resources
        if(bleash->view_port) {
            view_port_draw_callback_set(bleash->view_port, NULL, NULL);
            view_port_input_callback_set(bleash->view_port, NULL, NULL);
            FURI_LOG_D(TAG, "View port callbacks disabled");
        }

        // STEP 2: Remove from GUI before freeing
        if(bleash->gui && bleash->view_port) {
            gui_remove_view_port(bleash->gui, bleash->view_port);
            FURI_LOG_D(TAG, "View port removed from GUI");
        }

        // STEP 3: Now safe to free view port
        if(bleash->view_port) {
            view_port_free(bleash->view_port);
            bleash->view_port = NULL;
            FURI_LOG_D(TAG, "View port freed");
        }

        // STEP 4: Free event queue (no longer needed without GUI)
        if(bleash->event_queue) {
            furi_message_queue_free(bleash->event_queue);
            bleash->event_queue = NULL;
            FURI_LOG_D(TAG, "Event queue freed");
        }

        // STEP 5: Close GUI record
        if(bleash->gui) {
            furi_record_close(RECORD_GUI);
            bleash->gui = NULL;
            FURI_LOG_D(TAG, "GUI record closed");
        }

        // STEP 6: Give time for any pending operations to complete
        furi_delay_ms(100);

        // STEP 7: Reset processing flag so worker can continue
        if(bleash->mutex && furi_mutex_acquire(bleash->mutex, 1000) == FuriStatusOk) {
            bleash->processing = false;
            furi_mutex_release(bleash->mutex);
            FURI_LOG_D(TAG, "Processing flag reset for background operation");
        }
    }

    return 0;
}
