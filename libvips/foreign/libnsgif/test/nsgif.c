/*
 * Copyright 2008 Sean Fox <dyntryx@gmail.com>
 * Copyright 2008 James Bursa <james@netsurf-browser.org>
 * Copyright 2022 Michael Drake <tlsa@netsurf-browser.org>
 *
 * This file is part of NetSurf's libnsgif, http://www.netsurf-browser.org/
 * Licenced under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/nsgif.h"

#include "cli.h"
#include "cli.c"

#define BYTES_PER_PIXEL 4

static struct nsgif_options {
	const char *file;
	const char *ppm;
	uint64_t loops;
	bool info;
} nsgif_options;

static const struct cli_table_entry cli_entries[] = {
	{
		.s = 'm',
		.l = "ppm",
		.t = CLI_STRING,
		.v.s = &nsgif_options.ppm,
		.d = "Convert frames to PPM image at given path."
	},
	{
		.s = 'i',
		.l = "info",
		.t = CLI_BOOL,
		.v.b = &nsgif_options.info,
		.d = "Dump GIF info to stdout."
	},
	{
		.s = 'l',
		.l = "loops",
		.t = CLI_UINT,
		.v.u = &nsgif_options.loops,
		.d = "Loop through decoding all frames N times. "
		     "The default is 1."
	},
	{
		.p = true,
		.l = "FILE",
		.t = CLI_STRING,
		.v.s = &nsgif_options.file,
		.d = "Path to GIF file to load."
	},
};

const struct cli_table cli = {
	.entries = cli_entries,
	.count = (sizeof(cli_entries))/(sizeof(*cli_entries)),
	.min_positional = 1,
};

static void *bitmap_create(int width, int height)
{
	/* Ensure a stupidly large bitmap is not created */
	if (width > 4096 || height > 4096) {
		return NULL;
	}

	return calloc(width * height, BYTES_PER_PIXEL);
}

static unsigned char *bitmap_get_buffer(void *bitmap)
{
	return bitmap;
}

static void bitmap_destroy(void *bitmap)
{
	free(bitmap);
}

static uint8_t *load_file(const char *path, size_t *data_size)
{
	FILE *fd;
	struct stat sb;
	unsigned char *buffer;
	size_t size;
	size_t n;

	fd = fopen(path, "rb");
	if (!fd) {
		perror(path);
		exit(EXIT_FAILURE);
	}

	if (stat(path, &sb)) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	size = sb.st_size;

	buffer = malloc(size);
	if (!buffer) {
		fprintf(stderr, "Unable to allocate %lld bytes\n",
			(long long) size);
		exit(EXIT_FAILURE);
	}

	n = fread(buffer, 1, size, fd);
	if (n != size) {
		perror(path);
		exit(EXIT_FAILURE);
	}

	fclose(fd);

	*data_size = size;
	return buffer;
}

static void warning(const char *context, nsgif_error err)
{
	fprintf(stderr, "%s failed: %s\n",
			context, nsgif_strerror(err));
}

static void print_gif_info(const nsgif_info_t *info)
{
	fprintf(stdout, "gif:\n");
	fprintf(stdout, "  width: %"PRIu32"\n", info->width);
	fprintf(stdout, "  height: %"PRIu32"\n", info->height);
	fprintf(stdout, "  max-loops: %"PRIu32"\n", info->loop_max);
	fprintf(stdout, "  frame-count: %"PRIu32"\n", info->frame_count);
	fprintf(stdout, "  background:\n");
	fprintf(stdout, "    red: 0x%"PRIx8"\n", info->background[0]);
	fprintf(stdout, "    green: 0x%"PRIx8"\n", info->background[1]);
	fprintf(stdout, "    blue: 0x%"PRIx8"\n", info->background[2]);
	fprintf(stdout, "  frames:\n");
}

static void print_gif_frame_info(const nsgif_frame_info_t *info)
{
	const char *disposal = nsgif_str_disposal(info->disposal);

	fprintf(stdout, "  - disposal-method: %s\n", disposal);
	fprintf(stdout, "    transparency: %s\n", info->transparency ? "yes" : "no");
	fprintf(stdout, "    display: %s\n", info->display ? "yes" : "no");
	fprintf(stdout, "    delay: %"PRIu32"\n", info->delay);
	fprintf(stdout, "    rect:\n");
	fprintf(stdout, "      x: %"PRIu32"\n", info->rect.x0);
	fprintf(stdout, "      y: %"PRIu32"\n", info->rect.y0);
	fprintf(stdout, "      w: %"PRIu32"\n", info->rect.x1 - info->rect.x0);
	fprintf(stdout, "      h: %"PRIu32"\n", info->rect.y1 - info->rect.y0);
}

static void decode(FILE* ppm, const char *name, nsgif_t *gif)
{
	nsgif_error err;
	uint32_t frame_prev = 0;
	const nsgif_info_t *info;

	info = nsgif_get_info(gif);

	if (ppm != NULL) {
		fprintf(ppm, "P3\n");
		fprintf(ppm, "# %s\n", name);
		fprintf(ppm, "# width                %u \n", info->width);
		fprintf(ppm, "# height               %u \n", info->height);
		fprintf(ppm, "# frame_count          %u \n", info->frame_count);
		fprintf(ppm, "# loop_max             %u \n", info->loop_max);
		fprintf(ppm, "%u %u 256\n", info->width,
				info->height * info->frame_count);
	}

	if (nsgif_options.info == true) {
		print_gif_info(info);
	}

	/* decode the frames */
	while (true) {
		nsgif_bitmap_t *bitmap;
		const uint8_t *image;
		uint32_t frame_new;
		uint32_t delay_cs;
		nsgif_rect_t area;

		err = nsgif_frame_prepare(gif, &area,
				&delay_cs, &frame_new);
		if (err != NSGIF_OK) {
			warning("nsgif_frame_prepare", err);
			return;
		}

		if (frame_new < frame_prev) {
			/* Must be an animation that loops. We only care about
			 * decoding each frame once in this utility. */
			return;
		}
		frame_prev = frame_new;

		err = nsgif_frame_decode(gif, frame_new, &bitmap);
		if (err != NSGIF_OK) {
			warning("nsgif_decode_frame", err);
			return;
		}

		if (nsgif_options.info == true) {
			const nsgif_frame_info_t *f_info;

			f_info = nsgif_get_frame_info(gif, frame_new);
			assert(f_info != NULL);
			print_gif_frame_info(f_info);
		}

		if (ppm != NULL) {
			fprintf(ppm, "# frame %u:\n", frame_new);
			image = (const uint8_t *) bitmap;
			for (uint32_t y = 0; y != info->height; y++) {
				for (uint32_t x = 0; x != info->width; x++) {
					size_t z = (y * info->width + x) * 4;
					fprintf(ppm, "%u %u %u ",
							image[z],
							image[z + 1],
							image[z + 2]);
				}
				fprintf(ppm, "\n");
			}
		}

		if (delay_cs == NSGIF_INFINITE) {
			/** This frame is the last. */
			return;
		}
	}
}

int main(int argc, char *argv[])
{
	const nsgif_bitmap_cb_vt bitmap_callbacks = {
		.create     = bitmap_create,
		.destroy    = bitmap_destroy,
		.get_buffer = bitmap_get_buffer,
	};
	size_t size;
	nsgif_t *gif;
	uint8_t *data;
	nsgif_error err;
	FILE *ppm = NULL;

	/* Override default options with any command line args */
	if (!cli_parse(&cli, argc, (void *)argv)) {
		cli_help(&cli, argv[0]);
		return EXIT_FAILURE;
	}

	if (nsgif_options.ppm != NULL) {
		ppm = fopen(nsgif_options.ppm, "w+");
		if (ppm == NULL) {
			fprintf(stderr, "Unable to open %s for writing\n",
					nsgif_options.ppm);
			return EXIT_FAILURE;
		}
	}

	/* create our gif animation */
	err = nsgif_create(&bitmap_callbacks, &gif);
	if (err != NSGIF_OK) {
		warning("nsgif_create", err);
		return EXIT_FAILURE;
	}

	/* load file into memory */
	data = load_file(nsgif_options.file, &size);

	/* Scan the raw data */
	err = nsgif_data_scan(gif, size, data);
	if (err != NSGIF_OK) {
		warning("nsgif_data_scan", err);
		nsgif_destroy(gif);
		free(data);
		return EXIT_FAILURE;
	}

	if (nsgif_options.loops == 0) {
		nsgif_options.loops = 1;
	}

	for (uint64_t i = 0; i < nsgif_options.loops; i++) {
		decode((i == 0) ? ppm : NULL, nsgif_options.file, gif);
	}

	if (ppm != NULL) {
		fclose(ppm);
	}

	/* clean up */
	nsgif_destroy(gif);
	free(data);

	return 0;
}
