#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define MAX_BUF 64
#define MAX_GPIO_PINS 32
#define MONITOR_DIR "/run/ledd"
#define EVENT_BUF_LEN (MAX_GPIO_PINS * (sizeof(struct inotify_event) + 256))

// Structure to track individual GPIO pin state
typedef struct {
  int gpio_pin;
  int initial_state;
  double blink_interval_ms; // Interval in milliseconds
  volatile int is_blinking;
  pthread_t blink_thread;
  pthread_mutex_t state_lock;
} gpio_state_t;

// Global state
static volatile sig_atomic_t keep_running = 1;
static gpio_state_t gpio_states[MAX_GPIO_PINS];
static int num_active_gpios = 0;
static pthread_mutex_t gpio_list_lock = PTHREAD_MUTEX_INITIALIZER;

// Function prototypes
static void *blink_led_thread(void *arg);
static int export_gpio(int gpio);
static int unexport_gpio(int gpio);
static int set_gpio_value(int gpio, int value);
static int get_gpio_value(int gpio);
static void handle_signal(int sig);
static void setup_signal_handling(void);
static void init_daemon(void);
static int parse_gpio_pin_from_filename(const char *filename, int *gpio_pin);
static int parse_control_file(const char *filename, int *gpio_pin, double *interval_ms);
static int read_blink_interval_from_file(const char *file_path, double *interval_ms);
static void start_blinking_for_gpio(int gpio_pin, double interval_ms);
static void stop_blinking_for_gpio(int gpio_pin);
static void cleanup_all_gpios(void);
static int setup_inotify_watch(void);

int main(int argc, char *argv[])
{
  if (argc != 1) {
    fprintf(stderr, "Usage: %s\n", argv[0]);
    fprintf(stderr, "Monitors /run/ledd/ directory for control files\n");
    fprintf(stderr, "Control file format: <PIN_NUMBER>\n");
    fprintf(stderr, "Control file content: <interval_in_milliseconds>\n");
    exit(EXIT_FAILURE);
  }

  // Initialize daemon
  init_daemon();
  setup_signal_handling();

  // Open syslog connection
  openlog("ledd", LOG_PID, LOG_DAEMON);

  // Create monitor directory if it doesn't exist
  if (mkdir(MONITOR_DIR, 0755) == -1 && errno != EEXIST) {
    syslog(LOG_ERR, "Failed to create monitor directory %s: %s", MONITOR_DIR, strerror(errno));
    exit(EXIT_FAILURE);
  }

  // Setup inotify watch
  int inotify_fd = setup_inotify_watch();
  if (inotify_fd == -1) {
    syslog(LOG_ERR, "Failed to setup inotify watch");
    exit(EXIT_FAILURE);
  }

  syslog(LOG_INFO, "LED daemon started, monitoring %s", MONITOR_DIR);

  // Main event loop
  char event_buf[EVENT_BUF_LEN];
  fd_set readfds;

  while (keep_running) {
    FD_ZERO(&readfds);
    FD_SET(inotify_fd, &readfds);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int ret = select(inotify_fd + 1, &readfds, NULL, NULL, &tv);
    if (ret == -1) {
      if (errno != EINTR) {
        syslog(LOG_ERR, "select() failed: %s", strerror(errno));
      }
      continue;
    }

    if (ret > 0 && FD_ISSET(inotify_fd, &readfds)) {
      int len = read(inotify_fd, event_buf, EVENT_BUF_LEN);
      if (len == -1) {
        syslog(LOG_ERR, "read() from inotify failed: %s", strerror(errno));
        continue;
      }

      // Process inotify events
      int i = 0;
      while (i < len) {
        struct inotify_event *event = (struct inotify_event *)&event_buf[i];
        if (event->len > 0) {
          int gpio_pin;
          double interval_ms;

          if (event->mask & IN_CREATE) {
            if (parse_control_file(event->name, &gpio_pin, &interval_ms) == 0) {
              syslog(LOG_INFO, "Control file created: %s (GPIO %d, interval %.0f ms)", event->name,
                     gpio_pin, interval_ms);
              start_blinking_for_gpio(gpio_pin, interval_ms);
            }
          } else if (event->mask & IN_DELETE) {
            if (parse_gpio_pin_from_filename(event->name, &gpio_pin) == 0) {
              syslog(LOG_INFO, "Control file deleted: %s (GPIO %d)", event->name, gpio_pin);
              stop_blinking_for_gpio(gpio_pin);
            }
          }
        }
        i += sizeof(struct inotify_event) + event->len;
      }
    }
  }

  cleanup_all_gpios();
  close(inotify_fd);
  syslog(LOG_INFO, "LED daemon stopped");
  closelog();
  return EXIT_SUCCESS;
}

// Thread function for blinking a single GPIO pin
static void *blink_led_thread(void *arg)
{
  gpio_state_t *state = (gpio_state_t *)arg;
  unsigned int sleep_time_us = (unsigned int)(state->blink_interval_ms * 1000);

  syslog(LOG_DEBUG, "Blink thread started for GPIO %d with interval %.0f ms", state->gpio_pin,
         state->blink_interval_ms);

  while (keep_running) {
    pthread_mutex_lock(&state->state_lock);
    if (!state->is_blinking) {
      pthread_mutex_unlock(&state->state_lock);
      break;
    }
    pthread_mutex_unlock(&state->state_lock);

    // Toggle GPIO: set to ON (opposite of initial state)
    int on_value = 1 - state->initial_state;
    if (set_gpio_value(state->gpio_pin, on_value) == -1) {
      syslog(LOG_ERR, "Failed to set GPIO %d to ON", state->gpio_pin);
      break;
    }

    usleep(sleep_time_us);

    pthread_mutex_lock(&state->state_lock);
    if (!state->is_blinking) {
      pthread_mutex_unlock(&state->state_lock);
      break;
    }
    pthread_mutex_unlock(&state->state_lock);

    // Toggle GPIO: set to OFF (initial state)
    if (set_gpio_value(state->gpio_pin, state->initial_state) == -1) {
      syslog(LOG_ERR, "Failed to set GPIO %d to OFF", state->gpio_pin);
      break;
    }

    usleep(sleep_time_us);
  }

  // Restore GPIO to initial state
  set_gpio_value(state->gpio_pin, state->initial_state);
  syslog(LOG_DEBUG, "Blink thread stopped for GPIO %d", state->gpio_pin);
  return NULL;
}

static int export_gpio(int gpio)
{
  char path[MAX_BUF];
  snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);

  // Check if already exported
  if (access(path, F_OK) == 0) {
    return 0; // Already exported
  }

  // Export the GPIO
  FILE *export_file = fopen("/sys/class/gpio/export", "w");
  if (export_file == NULL) {
    syslog(LOG_ERR, "Failed to open /sys/class/gpio/export: %s", strerror(errno));
    return -1;
  }

  fprintf(export_file, "%d", gpio);
  fclose(export_file);

  // Set direction to output
  snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
  FILE *direction_file = fopen(path, "w");
  if (direction_file == NULL) {
    syslog(LOG_ERR, "Failed to set GPIO %d direction: %s", gpio, strerror(errno));
    return -1;
  }

  fprintf(direction_file, "out");
  fclose(direction_file);

  return 0;
}

static int unexport_gpio(int gpio)
{
  FILE *unexport_file = fopen("/sys/class/gpio/unexport", "w");
  if (unexport_file == NULL) {
    syslog(LOG_ERR, "Failed to open /sys/class/gpio/unexport: %s", strerror(errno));
    return -1;
  }

  fprintf(unexport_file, "%d", gpio);
  fclose(unexport_file);
  return 0;
}

static int set_gpio_value(int gpio, int value)
{
  char buf[MAX_BUF];
  snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/value", gpio);
  FILE *fd = fopen(buf, "w");
  if (fd == NULL) {
    syslog(LOG_ERR, "Failed to open GPIO value for GPIO %d: %s", gpio, strerror(errno));
    return -1;
  }
  fprintf(fd, "%d", value);
  fclose(fd);
  return 0;
}

static int get_gpio_value(int gpio)
{
  char buf[MAX_BUF];
  snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/value", gpio);
  FILE *fd = fopen(buf, "r");
  if (fd == NULL) {
    syslog(LOG_ERR, "Failed to open GPIO value for GPIO %d: %s", gpio, strerror(errno));
    return -1;
  }
  int value = -1;
  fscanf(fd, "%d", &value);
  fclose(fd);
  return value;
}

// Parse control filename to extract GPIO pin number
// Expected format: <PIN_NUMBER>
// Returns 0 on success, -1 on failure
static int parse_gpio_pin_from_filename(const char *filename, int *gpio_pin)
{
  if (filename == NULL || gpio_pin == NULL) {
    return -1;
  }

  char *endptr;
  errno = 0;
  long pin = strtol(filename, &endptr, 10);

  if (errno != 0 || *endptr != '\0' || pin < 0 || pin > 1000) {
    syslog(LOG_WARNING, "Invalid GPIO pin in filename: %s", filename);
    return -1;
  }

  *gpio_pin = (int)pin;
  return 0;
}

static int parse_control_file(const char *filename, int *gpio_pin, double *interval_ms)
{
  if (filename == NULL || gpio_pin == NULL || interval_ms == NULL) {
    return -1;
  }

  if (parse_gpio_pin_from_filename(filename, gpio_pin) == -1) {
    return -1;
  }

  // Read interval from file
  if (read_blink_interval_from_file(filename, interval_ms) == -1) {
    return -1;
  }

  return 0;
}

// Read blinking interval from control file
// File content should be a number representing milliseconds
// Returns 0 on success, -1 on failure
static int read_blink_interval_from_file(const char *file_path, double *interval_ms)
{
  if (file_path == NULL || interval_ms == NULL) {
    return -1;
  }

  char full_path[256];
  snprintf(full_path, sizeof(full_path), "%s/%s", MONITOR_DIR, file_path);

  FILE *file = fopen(full_path, "r");
  if (file == NULL) {
    syslog(LOG_WARNING, "Failed to open control file %s: %s", full_path, strerror(errno));
    return -1;
  }

  char buf[MAX_BUF];
  if (fgets(buf, sizeof(buf), file) == NULL) {
    syslog(LOG_WARNING, "Failed to read from control file %s", full_path);
    fclose(file);
    return -1;
  }

  fclose(file);

  // Parse the interval value
  errno = 0;
  char *endptr;
  double value = strtod(buf, &endptr);

  if (errno != 0 || value <= 0 || value > 60000) { // Max 60 seconds
    syslog(LOG_WARNING, "Invalid blink interval in file %s: %s (must be 0 < interval <= 60000 ms)",
           full_path, buf);
    return -1;
  }

  *interval_ms = value;
  return 0;
}

static void handle_signal(int sig)
{
  if (sig == SIGTERM || sig == SIGINT) {
    keep_running = 0;
  }
}

static void setup_signal_handling(void)
{
  struct sigaction sa;
  sa.sa_handler = handle_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGTERM, &sa, NULL) == -1 || sigaction(SIGINT, &sa, NULL) == -1) {
    syslog(LOG_ERR, "Error setting up signal handler");
    exit(EXIT_FAILURE);
  }
}

static void init_daemon(void)
{
  pid_t pid = fork();
  if (pid < 0) {
    exit(EXIT_FAILURE);
  }
  if (pid > 0) {
    exit(EXIT_SUCCESS);
  }

  if (setsid() < 0) {
    exit(EXIT_FAILURE);
  }

  pid = fork();
  if (pid < 0) {
    exit(EXIT_FAILURE);
  }
  if (pid > 0) {
    exit(EXIT_SUCCESS);
  }

  umask(0);
  chdir("/");

  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);

  open("/dev/null", O_RDWR);
  dup(0);
  dup(0);
}

// Start blinking for a GPIO pin
static void start_blinking_for_gpio(int gpio_pin, double interval_ms)
{
  if (gpio_pin < 0 || gpio_pin > 1000) {
    syslog(LOG_ERR, "Invalid GPIO pin: %d", gpio_pin);
    return;
  }

  if (interval_ms <= 0 || interval_ms > 60000) {
    syslog(LOG_ERR, "Invalid interval for GPIO %d: %.0f ms", gpio_pin, interval_ms);
    return;
  }

  pthread_mutex_lock(&gpio_list_lock);

  // Check if already blinking
  for (int i = 0; i < num_active_gpios; i++) {
    if (gpio_states[i].gpio_pin == gpio_pin) {
      syslog(LOG_WARNING, "GPIO %d is already blinking", gpio_pin);
      pthread_mutex_unlock(&gpio_list_lock);
      return;
    }
  }

  if (num_active_gpios >= MAX_GPIO_PINS) {
    syslog(LOG_ERR, "Maximum number of GPIO pins reached");
    pthread_mutex_unlock(&gpio_list_lock);
    return;
  }

  // Export GPIO
  if (export_gpio(gpio_pin) == -1) {
    syslog(LOG_ERR, "Failed to export GPIO %d", gpio_pin);
    pthread_mutex_unlock(&gpio_list_lock);
    return;
  }

  // Get current GPIO state
  int current_state = get_gpio_value(gpio_pin);
  if (current_state == -1) {
    syslog(LOG_WARNING, "Failed to read GPIO %d state, assuming 0", gpio_pin);
    current_state = 0;
  }

  // Initialize GPIO state structure
  gpio_state_t *state = &gpio_states[num_active_gpios];
  state->gpio_pin = gpio_pin;
  state->initial_state = current_state;
  state->blink_interval_ms = interval_ms;
  state->is_blinking = 1;
  pthread_mutex_init(&state->state_lock, NULL);

  // Create blinking thread
  if (pthread_create(&state->blink_thread, NULL, blink_led_thread, state) != 0) {
    syslog(LOG_ERR, "Failed to create blink thread for GPIO %d", gpio_pin);
    unexport_gpio(gpio_pin);
    pthread_mutex_unlock(&gpio_list_lock);
    return;
  }

  num_active_gpios++;
  syslog(LOG_INFO, "Started blinking GPIO %d with interval %.0f ms", gpio_pin, interval_ms);
  pthread_mutex_unlock(&gpio_list_lock);
}

// Stop blinking for a GPIO pin
static void stop_blinking_for_gpio(int gpio_pin)
{
  pthread_mutex_lock(&gpio_list_lock);

  for (int i = 0; i < num_active_gpios; i++) {
    if (gpio_states[i].gpio_pin == gpio_pin) {
      gpio_state_t *state = &gpio_states[i];

      // Signal thread to stop
      pthread_mutex_lock(&state->state_lock);
      state->is_blinking = 0;
      pthread_mutex_unlock(&state->state_lock);

      // Wait for thread to finish
      pthread_join(state->blink_thread, NULL);

      // Restore GPIO to initial state
      set_gpio_value(gpio_pin, state->initial_state);

      // Unexport GPIO
      unexport_gpio(gpio_pin);

      // Remove from active list
      for (int j = i; j < num_active_gpios - 1; j++) {
        gpio_states[j] = gpio_states[j + 1];
      }
      num_active_gpios--;

      syslog(LOG_INFO, "Stopped blinking GPIO %d", gpio_pin);
      pthread_mutex_unlock(&gpio_list_lock);
      return;
    }
  }

  syslog(LOG_WARNING, "GPIO %d not found in active list", gpio_pin);
  pthread_mutex_unlock(&gpio_list_lock);
}

// Cleanup all GPIO pins
static void cleanup_all_gpios(void)
{
  pthread_mutex_lock(&gpio_list_lock);

  while (num_active_gpios > 0) {
    gpio_state_t *state = &gpio_states[0];
    int gpio_pin = state->gpio_pin;

    pthread_mutex_lock(&state->state_lock);
    state->is_blinking = 0;
    pthread_mutex_unlock(&state->state_lock);

    pthread_join(state->blink_thread, NULL);
    set_gpio_value(gpio_pin, state->initial_state);
    unexport_gpio(gpio_pin);

    // Shift remaining entries
    for (int i = 0; i < num_active_gpios - 1; i++) {
      gpio_states[i] = gpio_states[i + 1];
    }
    num_active_gpios--;
  }

  pthread_mutex_unlock(&gpio_list_lock);
}

// Setup inotify watch on monitor directory
static int setup_inotify_watch(void)
{
  int inotify_fd = inotify_init1(IN_NONBLOCK);
  if (inotify_fd == -1) {
    syslog(LOG_ERR, "inotify_init1 failed: %s", strerror(errno));
    return -1;
  }

  int wd = inotify_add_watch(inotify_fd, MONITOR_DIR, IN_CREATE | IN_DELETE);
  if (wd == -1) {
    syslog(LOG_ERR, "inotify_add_watch failed for %s: %s", MONITOR_DIR, strerror(errno));
    close(inotify_fd);
    return -1;
  }

  return inotify_fd;
}
