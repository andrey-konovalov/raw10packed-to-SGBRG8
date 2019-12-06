#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/videodev2.h>

#define SIZE(x)		(sizeof(x)/sizeof((x)[0]))

char *progname = "";

void error(const char *format, ...) {
	va_list ap;
	int err;

	err = errno;
	va_start(ap, format);
	fprintf(stderr, "%s: ", progname);
	if (format)
		vfprintf(stderr, format, ap);
	else
		fprintf(stderr, "error");
	if (err)
		fprintf(stderr, " (%s)", strerror(err));
	fprintf(stderr, "\n");
	va_end(ap);
	if (!err)
		err = 1;
	exit(err);
}

#define USAGE \
	"%s - convert headerless 10-bit packed raw image to GBRG 8-bit format\n" \
	"Usage: %s [-h] -s XxY [-f <format>] <inputfile> <outputfile>\n" \
	"-f <format>  Specify input file format (-f ? for list, default ‘pBAA’)\n" \
	"-s XxY       Specify input image size (e.g. 640x480)\n" \
	"-b           Write the original Bayer data to <infile>.bayer.pnm" \
	"-h           Shows this help\n"

static int parse_size(const char *p, int *w, int *h)
{
	char *end;

	for (; isspace(*p); ++p);

	*w = strtoul(p, &end, 10);
	if (*end != 'x')
		return -1;

	p = end + 1;
	*h = strtoul(p, &end, 10);
	if (*end != '\0')
		return -1;

	return 0;
}

static const struct format_info {
	__u32 fmt;
	char *name;
} v4l2_pix_fmt_str[] = {
	/* 10-bit packed Bayer formats */
	{ V4L2_PIX_FMT_SRGGB10P, "SRGGB10P (RGRG... GBGB... ; ‘pRAA’)" },
	{ V4L2_PIX_FMT_SGRBG10P, "SGRBG10P (GRGR... BGBG... ; ‘pgAA’)" },
	{ V4L2_PIX_FMT_SGBRG10P, "SGBRG10P (GBGB... RGRG... ; ‘pGAA’)" },
	{ V4L2_PIX_FMT_SBGGR10P, "SBGGR10P (BGBG... GRGR... ; ‘pBAA’)" },
};

static const char bayer_suffix[] = ".bayer.pnm";

int main(int argc, char* argv[]) {
	FILE *fp_in, *fp_out, *fp_out_bayer;
	int size[2] = {-1,-1};
	__u32 format;
	int write_bayer_pnm = 0;
	char *file_in = NULL, *file_out = NULL, *file_out_bayer = NULL;
	int line_len, width, height;
	int shift = 0;
	char *data;
	int file_size;

	progname = argv[0];

	for (;;) {
                int c = getopt(argc, argv, "f:s:bh");
                if (c == -1) break;
                switch (c) {
		case 'f':
			if (optarg[0]=='?' && optarg[1]==0) {
				unsigned int i,j;
				printf("Supported formats:\n");
				for (i=0; i < SIZE(v4l2_pix_fmt_str); i++) {
					for (j=0;
					     v4l2_pix_fmt_str[i].name[j]!=' '
					     && v4l2_pix_fmt_str[i].name[j]!=0;
					     j++)
						putchar(v4l2_pix_fmt_str[i].name[j]);
					printf("\n");
                                };
                                exit(0);
			} else {
				unsigned int i,j;
				for (i=0; i < SIZE(v4l2_pix_fmt_str); i++) {
					for (j=0;
					     v4l2_pix_fmt_str[i].name[j]!=' '
					     && v4l2_pix_fmt_str[i].name[j]!=0;
					     j++);
					if (memcmp(v4l2_pix_fmt_str[i].name,
						   optarg, j) == 0
					    && optarg[j]==0)
						break;
                                };
                                if (i >= SIZE(v4l2_pix_fmt_str)) {
					error("bad format");
					goto error_format;
				}
                                format = v4l2_pix_fmt_str[i].fmt;
			}
			break;
		case 's':
			if (parse_size(optarg, &size[0], &size[1]) < 0) {
				error("bad size");
				exit(0);
			}
			break;
		case 'b':
			write_bayer_pnm = 1;
			break;
		case 'h':
			printf(USAGE, argv[0], argv[0]);
			exit(0);
		default:
			error("bad argument");
		}
	}

	if (argc-optind != 2) {
		error("give input and output files");
		exit(-1);
	}
	file_in  = argv[optind++];
	file_out = argv[optind++];

	/* Calculate the file size and the line length (padding included) */
	fp_in = fopen(file_in, "rb");
	if (fp_in == NULL) error("%s: fopen failed", file_in);
	if (fseek(fp_in, 0, SEEK_END) != 0 || (file_size = ftell(fp_in)) == -1
	    || fseek(fp_in, 0, SEEK_SET) != 0) {
		error("%s: failed to get file size", file_in);
		goto error_fsize;
	}
	width = size[0];
	height = size[1];

	/* GBRG -> GRBG "conversion" reduces width and height by 2, so
	 * the assumption is that width and height are at least 2 */
	if (width < 2 || height < 2) {
		error("bad frame size: width=%d, height=%d", width, height);
		goto error_fsize;
	}
	line_len = file_size / height;
	if (file_size % height != 0) {
		error("the input file size is not multiple of frame height");
		goto error_fsize;
	}
	/* line_len >= width, as line may have padding bytes in the end */
	if (line_len < width) {
		error("line_len (%d) < width (%d)", line_len, width);
		goto error_fsize;
	}

	if ((data = malloc(line_len)) == NULL) {
		error("failed to allocate data buffer");
		goto error_malloc;
	}

	/* If requested, write the pnm file which shows the original
	 * Bayer layout of the pixels */
	if (write_bayer_pnm) {
		const int GRBG_indexes[2][2] = { { 1, 0 }, { 2, 1 } };
		const int RGGB_indexes[2][2] = { { 0, 1 }, { 1, 2 } };
		const int GBRG_indexes[2][2] = { { 1, 2 }, { 0, 1 } };
		const int BGGR_indexes[2][2] = { { 2, 1 }, { 1, 0 } };
		const int (*indexes)[2][2];
		__u8 pixel_buf[3];
		int index;

		file_out_bayer = malloc(strlen(file_in)
					+ strlen(bayer_suffix) + 1);
	        if (file_out_bayer == NULL) {
			error("memory allocation failed");
			goto error_fp_out;
		}
		sprintf(file_out_bayer, "%s%s", file_in, bayer_suffix);
		if((fp_out_bayer = fopen(file_out_bayer, "wb")) == NULL) {
			error("failed to create %s", file_out_bayer);
			free(file_out_bayer);
			goto error_fp_out;
		}
		fprintf(fp_out_bayer, "P6\n%i %i\n255\n", size[0], size[1]);

		switch (format) {
		case V4L2_PIX_FMT_SGRBG10P:
			indexes = &GRBG_indexes;
			break;
		case V4L2_PIX_FMT_SRGGB10P:
			indexes = &RGGB_indexes;
			break;
		case V4L2_PIX_FMT_SGBRG10P:
			indexes = &GBRG_indexes;
			break;
		case V4L2_PIX_FMT_SBGGR10P:
			indexes = &BGGR_indexes;
			break;
		};

		/* Read the input file line by line */
		for (int line=0; line < height; line++) {
			if (fread(data, line_len, 1, fp_in) != 1) {
				error("%s: read error", file_in);
				fclose(fp_out_bayer);
				free(file_out_bayer);
				goto error_fp_out;
			}

			/* drop every 5th byte to convert from 10-bit to 8-bit
			 * samples */
			for (int src = 5, dst = 4;
			     dst < width;
			     src += 5, dst += 4) {
				memmove(data+dst, data+src, 4);
			}

			for(int pos=0 ; pos < width; pos++) {
				memset(pixel_buf, 0, 3);
				index = (*indexes)[line%2][pos%2];
				pixel_buf[index] = data[pos];
				if (fwrite(pixel_buf, 3, 1, fp_out_bayer) != 1) {
					error("%s: write error", file_out);
					fclose(fp_out_bayer);
					free(file_out_bayer);
					goto error_fp_out;
				}
			}
		}
		fclose(fp_out_bayer);
		free(file_out_bayer);

		if(fseek(fp_in, 0, SEEK_SET) != 0) {
			error("%s: failed to revind the input file", file_in);
			goto error_fp_out;
        	}
	}

	if((fp_out = fopen(file_out, "wb")) == NULL) {
		error("failed to create %s", file_out);
		goto error_fp_out;
	}

	/* SGBRG10P, SBGGR10P:
	 * remove the 1st and the last line, height -= 2 */
	if (format == V4L2_PIX_FMT_SGBRG10P || format == V4L2_PIX_FMT_SBGGR10P) {
		if (fseek(fp_in, line_len, SEEK_SET) != 0) {
			error("%s: fseek failed", file_in);
			goto error_convert;
		}
		height -= 2;
	}

	/* SRGGB10P, SGBRG10P:
	 * remove the 1st and the last byte in each line, width -= 2 */
	if (format == V4L2_PIX_FMT_SRGGB10P || format == V4L2_PIX_FMT_SGBRG10P) {
		shift = 1;
		width -= 2;
	}

	/* Read the input file line by line */
	for (int line=0; line < height; line++) {
		if (fread(data, line_len, 1, fp_in) != 1) {
			error("%s: read error", file_in);
			goto error_convert;
		}

		/* drop every 5th byte to convert from 10-bit to 8-bit
		 * samples */
		for (int src = 5, dst = 4; dst < width; src += 5, dst += 4) {
			memmove(data+dst, data+src, 4);
		}


		if (fwrite(data + shift, width, 1, fp_out) != 1) {
			error("%s: write error", file_out);
			goto error_convert;
		}	
	}

	fclose(fp_out);
	free(data);
	fclose(fp_in);
	return 0;

error_convert:
	fclose(fp_out);
error_fp_out:
	free(data);
error_malloc:
error_fsize:
	fclose(fp_in);
error_format:
	return -1;
}
