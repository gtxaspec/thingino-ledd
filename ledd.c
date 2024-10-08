#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/stat.h>

#define MAX_BUF 64
#define FW_PRINTENV_CMD "fw_printenv | grep ^gpio_led_ | sort"

static int gpio_pin = -1;
static volatile sig_atomic_t keep_running = 1;
static double blink_interval = 1.0;  // Default blink interval in seconds
static const char *monitor_file = "/var/run/boot"; // Default file to monitor
static int active_low = 0;  // Default to active high (0 means active high)
static int off_value = 1;  // Default off value, assuming active high

// New flags
static int file_was_present = 0;
static int gpio_was_active = 0;  // Track if GPIO was being used for blinking

// prototypes
static void blink_led(int gpio_pin);
static int export_gpio(int gpio);
static int unexport_gpio(int gpio);
static int set_gpio_value(int gpio, int value);
static int get_gpio_pin_from_fw(void);
static void handle_signal(int sig);
static void setup_signal_handling(void);
static void init_daemon(void);
static void reset_gpio_state(void);
static double read_blink_interval_from_file(const char *file_path);

int main(int argc, char *argv[]) {
	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s <blink_interval> [file_to_monitor]\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	char *endptr;
	errno = 0;
	blink_interval = strtod(argv[1], &endptr);
	if (errno != 0 || *endptr != '\0' || blink_interval <= 0) {
		fprintf(stderr, "Invalid blink interval: %s\n", argv[1]);
		exit(EXIT_FAILURE);
	}

	// Set the file to monitor (default to /tmp/boot if not provided)
	if (argc == 3) {
		monitor_file = argv[2];
	}

	// Get GPIO pin from fw_printenv and set off state
	gpio_pin = get_gpio_pin_from_fw();
	if (gpio_pin == -1) {
		fprintf(stderr, "Failed to retrieve GPIO pin from fw_printenv\n");
		exit(EXIT_FAILURE);
	}

	// Export the GPIO using system command
	if (export_gpio(gpio_pin) == -1) {
		fprintf(stderr, "Failed to export GPIO %d\n", gpio_pin);
		exit(EXIT_FAILURE);
	}

	// Set the initial state of the GPIO to "off" based on the active_low flag
	set_gpio_value(gpio_pin, off_value);

	init_daemon();
	setup_signal_handling();

	// Open syslog connection
	openlog("led_blink_daemon", LOG_PID, LOG_DAEMON);

	while (keep_running) {
		// Check if the monitored file exists
		if (access(monitor_file, F_OK) == 0) {
			if (!file_was_present) {
				// The file has just appeared, so start blinking
				syslog(LOG_INFO, "Monitored file appeared, starting LED blink");
				double new_interval = read_blink_interval_from_file(monitor_file);
				if (new_interval > 0) {
					blink_interval = new_interval;
					syslog(LOG_INFO, "Blink interval updated to %.2f seconds", blink_interval);
				}
				blink_led(gpio_pin);  // Start blinking the LED
				file_was_present = 1;  // Mark that the file is present
				gpio_was_active = 1;   // Mark that the GPIO is active
			}
		} else {
			if (file_was_present) {
				// The file has just disappeared, so set the GPIO to the off state
				syslog(LOG_INFO, "Monitored file disappeared, turning off GPIO");
				set_gpio_value(gpio_pin, off_value);  // Set GPIO to "off"
				file_was_present = 0;  // Mark that the file is no longer present
				gpio_was_active = 0;   // Mark that the GPIO is inactive
			}
			usleep(100000);  // Sleep for 500ms before checking again
		}
	}

	set_gpio_value(gpio_pin, off_value);  // Ensure LED is "off" before exiting
	unexport_gpio(gpio_pin);
	closelog();
	return EXIT_SUCCESS;
}

static void blink_led(int gpio_pin) {
	unsigned int sleep_time = (unsigned int)(blink_interval * 1000000);

	while (keep_running) {
		// Check if the file still exists before each blink cycle
		if (access(monitor_file, F_OK) != 0) {
			break;  // Stop blinking if the file is no longer accessible
		}

		set_gpio_value(gpio_pin, 1 - off_value);  // Set GPIO to "on"
		usleep(sleep_time);
		set_gpio_value(gpio_pin, off_value);  // Set GPIO to "off"
		usleep(sleep_time);
	}
}

static int export_gpio(int gpio) {
	char command[MAX_BUF];
	snprintf(command, sizeof(command), "gpio export %d", gpio);
	snprintf(command, sizeof(command), "gpio output %d", gpio);
	return system(command);
}

static int unexport_gpio(int gpio) {
	char command[MAX_BUF];
	snprintf(command, sizeof(command), "gpio unexport %d", gpio);
	return system(command);
}

static int set_gpio_value(int gpio, int value) {
	char buf[MAX_BUF];
	snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/value", gpio);
	FILE *fd = fopen(buf, "w");
	if (fd == NULL) {
		syslog(LOG_ERR, "Failed to open GPIO value for GPIO %d", gpio);
		return -1;
	}
	fprintf(fd, "%d", value);
	fclose(fd);
	return 0;
}

static int get_gpio_pin_from_fw(void) {
	FILE *fp = popen(FW_PRINTENV_CMD, "r");
	if (fp == NULL) {
		syslog(LOG_ERR, "Failed to run fw_printenv");
		return -1;
	}

	char buffer[MAX_BUF];
	int gpio_pin = -1;

	// Parse the fw_printenv output for the GPIO pin and check if active_low or active_high
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		char *pos = strchr(buffer, '=');
		if (pos != NULL) {
			long val = strtol(pos + 1, NULL, 10);
			if (val >= 0) {
				gpio_pin = (int)val;

				// logic for interpreting the suffix 'o' or 'O'
				if (strchr(pos + 1, 'o')) {
					active_low = 1;  // Active low (0 means on, 1 means off)
					off_value = 1;   // Set off_value to 1 (high is off, low is on)
				} else if (strchr(pos + 1, 'O')) {
					active_low = 0;  // Active high (1 means on, 0 means off)
					off_value = 0;   // Set off_value to 0 (low is off, high is on)
				} else {
					// No suffix, assume active high and off is 0
					active_low = 0;
					off_value = 0;
				}
				break;
			}
		}
	}

	pclose(fp);

	// If no gpio_led entry was found, log an error and return -1
	if (gpio_pin == -1) {
		syslog(LOG_ERR, "No gpio_led entries found in fw_printenv");
	}

	return gpio_pin;
}

static void handle_signal(int sig) {
	if (sig == SIGTERM || sig == SIGINT) {
		keep_running = 0;
	}
}

static void setup_signal_handling(void) {
	struct sigaction sa;
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGTERM, &sa, NULL) == -1 || sigaction(SIGINT, &sa, NULL) == -1) {
		syslog(LOG_ERR, "Error setting up signal handler");
		exit(EXIT_FAILURE);
	}
}

static void init_daemon(void) {
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

	open("/dev/null", O_RDWR);  // Open /dev/null for input/output redirection
	dup(0);  // Duplicate file descriptor 0 (stdin)
	dup(0);  // Duplicate file descriptor 0 (stdout and stderr)
}

static void reset_gpio_state(void) {
	set_gpio_value(gpio_pin, off_value);  // Always set to "off"
}

static double read_blink_interval_from_file(const char *file_path) {
	FILE *file = fopen(file_path, "r");
	if (file == NULL) {
		syslog(LOG_ERR, "Failed to open monitored file %s", file_path);
		return -1.0;
	}

	char buf[MAX_BUF];
	if (fgets(buf, sizeof(buf), file) == NULL) {
		syslog(LOG_ERR, "Failed to read from monitored file %s", file_path);
		fclose(file);
		return -1.0;
	}

	fclose(file);

	// Convert the string to a double representing the blink interval
	double new_interval = strtod(buf, NULL);
	if (new_interval <= 0) {
		syslog(LOG_ERR, "Invalid blink interval value in file: %s", buf);
		return -1.0;
	}

	return new_interval;
}
