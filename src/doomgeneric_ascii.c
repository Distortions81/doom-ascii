//
// Copyright(C) 2022-2024 Wojciech Graj
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     terminal-specific code
//

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(WIN32)
#define OS_WINDOWS
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef OS_WINDOWS
#define CLK 0

#define WINDOWS_CALL(cond_, format_)       \
	do {                               \
		if (UNLIKELY(cond_))       \
			winError(format_); \
	} while (0)

void winError(char *format)
{
	LPVOID lpMsgBuf;
	DWORD dw = GetLastError();
	errno = dw;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		dw,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf,
		0, NULL);
	I_Error(format, lpMsgBuf);
}

/* Modified from https://stackoverflow.com/a/31335254 */
struct timespec {
	long tv_sec;
	long tv_nsec;
};
int clock_gettime(int p, struct timespec *spec)
{
	(void)p;
	__int64 wintime;
	GetSystemTimeAsFileTime((FILETIME *)&wintime);
	wintime -= 116444736000000000ll;
	spec->tv_sec = wintime / 10000000ll;
	spec->tv_nsec = wintime % 10000000ll * 100;
	return 0;
}

#else
#define CLK CLOCK_REALTIME
#endif

#define UNLIKELY(x_) __builtin_expect((x_), 0)
#define CALL(stmt_, format_)                     \
	do {                                     \
		if (UNLIKELY(stmt_))             \
			I_Error(format_, errno); \
	} while (0)
#define CALL_STDOUT(stmt_, format_) CALL((stmt_) == EOF, format_)

#define BYTE_TO_TEXT(buf_, byte_)                      \
	do {                                           \
		*(buf_)++ = '0' + (byte_) / 100u;      \
		*(buf_)++ = '0' + (byte_) / 10u % 10u; \
		*(buf_)++ = '0' + (byte_) % 10u;       \
	} while (0)

const char grad[] = " .-+1x@";
#define GRAD_LEN 8u
#define INPUT_BUFFER_LEN 16u
#define EVENT_BUFFER_LEN (INPUT_BUFFER_LEN * 2u - 1u)

struct color_t {
	uint32_t b : 8;
	uint32_t g : 8;
	uint32_t r : 8;
	uint32_t a : 8;
};

char *output_buffer;
size_t output_buffer_size;
struct timespec ts_init;

char input_buffer[INPUT_BUFFER_LEN];
uint16_t event_buffer[EVENT_BUFFER_LEN];
uint16_t *event_buf_loc;

void DG_Init()
{
#ifdef OS_WINDOWS
	const HANDLE hOutputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	WINDOWS_CALL(hOutputHandle == INVALID_HANDLE_VALUE, "DG_Init: %s");
	DWORD mode;
	WINDOWS_CALL(!GetConsoleMode(hOutputHandle, &mode), "DG_Init: %s");
	mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	WINDOWS_CALL(!SetConsoleMode(hOutputHandle, mode), "DG_Init: %s");

	const HANDLE hInputHandle = GetStdHandle(STD_INPUT_HANDLE);
	WINDOWS_CALL(hInputHandle == INVALID_HANDLE_VALUE, "DG_Init: %s");
	WINDOWS_CALL(!GetConsoleMode(hInputHandle, &mode), "DG_Init: %s");
	mode &= ~(ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_QUICK_EDIT_MODE);
	WINDOWS_CALL(!SetConsoleMode(hInputHandle, mode), "DG_Init: %s");
#endif
	/* Longest SGR code: \033[38;2;RRR;GGG;BBBm (length 19)
	 * Maximum 21 bytes per pixel: SGR + 2 x char
	 * 1 Newline character per line
	 * SGR clear code: \033[0m (length 4)
	 */
	output_buffer_size = 21u * DOOMGENERIC_RESX * DOOMGENERIC_RESY + DOOMGENERIC_RESY + 4u;
	output_buffer = malloc(output_buffer_size);

	clock_gettime(CLK, &ts_init);

	memset(input_buffer, '\0', INPUT_BUFFER_LEN);
}

float getHue(int r, int g, int b)
{
	float hue;

	// Convert RGB to normalized values
	float R = (float)r / 255;
	float G = (float)g / 255;
	float B = (float)b / 255;

	float max = R > G ? (R > B ? R : B) : (G > B ? G : B);
	float min = R < G ? (R < B ? R : B) : (G < B ? G : B);

	float delta = max - min;

	// Calculate hue
	if (delta == 0) {
		hue = 0;
	} else if (max == R) {
		hue = 60 * ((G - B) / delta);
	} else if (max == G) {
		hue = 60 * ((B - R) / delta + 2);
	} else {
		hue = 60 * ((R - G) / delta + 4);
	}

	// Make sure hue is in the range [0, 360)
	hue = hue < 0 ? hue + 360 : hue;

	return hue;
}

float getBrightness(int r, int g, int b)
{
	// Convert RGB to normalized values
	float R = (float)r / 255;
	float G = (float)g / 255;
	float B = (float)b / 255;

	// Calculate maximum value among R, G, and B
	float max = R > G ? (R > B ? R : B) : (G > B ? G : B);

	return max;
}

float getSaturation(int r, int g, int b)
{
	// Convert RGB to normalized values
	float R = (float)r / 255;
	float G = (float)g / 255;
	float B = (float)b / 255;

	float max = R > G ? (R > B ? R : B) : (G > B ? G : B);
	float min = R < G ? (R < B ? R : B) : (G < B ? G : B);

	float delta = max - min;

	// Calculate saturation
	if (max == 0) {
		return 0;
	} else {
		return delta / max;
	}
}

char *rgb_to_color(float hue, float sat, float val)
{
	if (val > 0.3) {
		if (sat < 0.5) {
			return "1;37";
		}
		if (hue >= 330 || hue < 30) {
			return "1;31"; // red
		} else if (hue >= 30 && hue < 90) {
			return "1;33"; //yellow
		} else if (hue >= 90 && hue < 150) {
			return "1;32"; //green
		} else if (hue >= 150 && hue < 210) {
			return "1;36"; //cyan
		} else if (hue >= 210 && hue < 270) {
			return "1;34"; //blue
		} else if (hue >= 270 && hue < 330) {
			return "1;35"; //magenta
		}
	} else {
		if (sat < 0.5) {
			return "0;37";
		}
		if (hue >= 330 || hue < 30) {
			return "0;31"; // red
		} else if (hue >= 30 && hue < 90) {
			return "0;33"; //yellow
		} else if (hue >= 90 && hue < 150) {
			return "0;32"; //green
		} else if (hue >= 150 && hue < 210) {
			return "0;36"; //cyan
		} else if (hue >= 210 && hue < 270) {
			return "0;34"; //blue
		} else if (hue >= 270 && hue < 330) {
			return "0;35"; //magenta
		}
	}
}

void DG_DrawFrame()
{
	/* Clear screen if first frame */
	static bool first_frame = true;
	if (first_frame) {
		first_frame = false;
		fputs("\033[1;1H\033[2J", stdout);
	}

	/* fill output buffer */
	uint32_t color = 0xFFFFFF00;
	unsigned row, col;
	struct color_t *pixel = (struct color_t *)DG_ScreenBuffer;
	char *buf = output_buffer;
	char buff[3];

	for (row = 0; row < DOOMGENERIC_RESY; row++) {
		for (col = 0; col < DOOMGENERIC_RESX; col++) {
			if ((color ^ *(uint32_t *)pixel) & 0x00FFFFFF) {
				*buf++ = '\033';
				*buf++ = '[';
				float hue = getHue(pixel->r, pixel->g, pixel->b);
				float sat = getSaturation(pixel->r, pixel->g, pixel->b);
				float val = getBrightness(pixel->r, pixel->g, pixel->b);
				char *acc = rgb_to_color(hue, sat, val);
				*buf++ = acc[0];
				*buf++ = acc[1];
				*buf++ = acc[2];
				*buf++ = acc[3];
				*buf++ = 'm';
				color = *(uint32_t *)pixel;
			}
			char v_char = grad[(pixel->r + pixel->g + pixel->b) * GRAD_LEN / 766u];
			*buf++ = v_char;
			*buf++ = v_char;
			pixel++;
		}
		*buf++ = '\n';
	}
	*buf++ = '\033';
	*buf++ = '[';
	*buf++ = '0';
	*buf = 'm';

	/* move cursor to top left corner and set bold text*/
	fputs("\033[;H\033[1m", stdout);

	/* flush output buffer */
	CALL_STDOUT(fputs(output_buffer, stdout), "DG_DrawFrame: fputs error %d");

	/* clear output buffer */
	memset(output_buffer, '\0', buf - output_buffer + 1u);
}

void DG_SleepMs(uint32_t ms)
{
#ifdef OS_WINDOWS
	Sleep(ms);
#else
	struct timespec ts = (struct timespec){
		.tv_sec = ms / 1000,
		.tv_nsec = (ms % 1000ul) * 1000000,
	};
	nanosleep(&ts, NULL);
#endif
}

uint32_t DG_GetTicksMs()
{
	struct timespec ts;
	clock_gettime(CLK, &ts);

	return (ts.tv_sec - ts_init.tv_sec) * 1000 + (ts.tv_nsec - ts_init.tv_nsec) / 1000000;
}

char convertToDoomKey(char **buf)
{
	switch (**buf) {
	case '\012':
		(*buf)++;
		return KEY_ENTER;
	case '\033':
		(*buf)++;
		switch (**buf) {
		case '[':
			(*buf)++;
			switch (**buf) {
			case 'A':
				(*buf)++;
				return KEY_UPARROW;
			case 'B':
				(*buf)++;
				return KEY_DOWNARROW;
			case 'C':
				(*buf)++;
				return KEY_RIGHTARROW;
			case 'D':
				(*buf)++;
				return KEY_LEFTARROW;
			}
		default:
			return KEY_ESCAPE;
		}
	case ' ':
		(*buf)++;
		return KEY_FIRE;
	default:
		return tolower(*((*buf)++));
	}
}

void DG_ReadInput(void)
{
	static char prev_input_buffer[INPUT_BUFFER_LEN];
	static char raw_input_buffer[INPUT_BUFFER_LEN];

	memcpy(prev_input_buffer, input_buffer, INPUT_BUFFER_LEN);
	memset(raw_input_buffer, '\0', INPUT_BUFFER_LEN);
	memset(input_buffer, '\0', INPUT_BUFFER_LEN);
	memset(event_buffer, '\0', 2u * EVENT_BUFFER_LEN);
	event_buf_loc = event_buffer;
#if defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__))
	struct termios oldt, newt;

	/* Disable canonical mode */
	CALL(tcgetattr(STDIN_FILENO, &oldt), "DG_DrawFrame: tcgetattr error %d");
	newt = oldt;
	newt.c_lflag &= ~(ICANON);
	newt.c_cc[VMIN] = 0;
	newt.c_cc[VTIME] = 0;
	CALL(tcsetattr(STDIN_FILENO, TCSANOW, &newt), "DG_DrawFrame: tcsetattr error %d");

	CALL(read(2, raw_input_buffer, INPUT_BUFFER_LEN - 1u) < 0, "DG_DrawFrame: read error %d");

	CALL(tcsetattr(STDIN_FILENO, TCSANOW, &oldt), "DG_DrawFrame: tcsetattr error %d");

	/* Flush input buffer to prevent read of previous unread input */
	CALL(tcflush(STDIN_FILENO, TCIFLUSH), "DG_DrawFrame: tcflush error %d");
#else /* defined(OS_WINDOWS) */
	const HANDLE hInputHandle = GetStdHandle(STD_INPUT_HANDLE);
	WINDOWS_CALL(hInputHandle == INVALID_HANDLE_VALUE, "DG_ReadInput: %s");

	/* Disable canonical mode */
	DWORD old_mode, new_mode;
	WINDOWS_CALL(!GetConsoleMode(hInputHandle, &old_mode), "DG_ReadInput: %s");
	new_mode = old_mode;
	new_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
	WINDOWS_CALL(!SetConsoleMode(hInputHandle, new_mode), "DG_ReadInput: %s");

	DWORD event_cnt;
	WINDOWS_CALL(!GetNumberOfConsoleInputEvents(hInputHandle, &event_cnt), "DG_ReadInput: %s");

	/* ReadConsole is blocking so must manually process events */
	int input_count = 0;
	if (event_cnt) {
		INPUT_RECORD input_records[32];
		WINDOWS_CALL(!ReadConsoleInput(hInputHandle, input_records, 32, &event_cnt), "DG_ReadInput: %s");

		DWORD i;
		for (i = 0; i < event_cnt; i++) {
			if (input_records[i].Event.KeyEvent.bKeyDown && input_records[i].EventType == KEY_EVENT) {
				raw_input_buffer[input_count++] = input_records[i].Event.KeyEvent.uChar.AsciiChar;
				if (input_count == INPUT_BUFFER_LEN - 1u)
					break;
			}
		}
	}

	WINDOWS_CALL(!SetConsoleMode(hInputHandle, old_mode), "DG_ReadInput: %s");
#endif
	/* create input buffer */
	char *raw_input_buf_loc = raw_input_buffer;
	char *input_buf_loc = input_buffer;
	while (*raw_input_buf_loc)
		*input_buf_loc++ = convertToDoomKey(&raw_input_buf_loc);

	/* construct event array */
	int i, j;
	for (i = 0; input_buffer[i]; i++) {
		/* skip duplicates */
		for (j = i + 1; input_buffer[j]; j++) {
			if (input_buffer[i] == input_buffer[j])
				goto LBL_CONTINUE_1;
		}

		/* pressed events */
		for (j = 0; prev_input_buffer[j]; j++) {
			if (input_buffer[i] == prev_input_buffer[j])
				goto LBL_CONTINUE_1;
		}
		*event_buf_loc++ = 0x0100 | input_buffer[i];

	LBL_CONTINUE_1:;
	}

	/* depressed events */
	for (i = 0; prev_input_buffer[i]; i++) {
		for (j = 0; input_buffer[j]; j++) {
			if (prev_input_buffer[i] == input_buffer[j])
				goto LBL_CONTINUE_2;
		}
		*event_buf_loc++ = 0xFF & prev_input_buffer[i];

	LBL_CONTINUE_2:;
	}

	event_buf_loc = event_buffer;
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
	if (!*event_buf_loc)
		return 0;

	*pressed = *event_buf_loc >> 8;
	*doomKey = *event_buf_loc & 0xFF;
	event_buf_loc++;
	return 1;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}
