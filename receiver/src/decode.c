/* JPEG decode via the PDK's libjpeg 6.2. That version has no
 * jpeg_mem_src, so we provide a memory source manager. */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>

#include "decode.h"

struct err_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void on_error_exit(j_common_ptr cinfo)
{
    struct err_mgr *e = (struct err_mgr *)cinfo->err;
    char msg[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, msg);
    fprintf(stderr, "jpeg: %s\n", msg);
    longjmp(e->jb, 1);
}

static void src_init(j_decompress_ptr cinfo) { (void)cinfo; }

static boolean src_fill(j_decompress_ptr cinfo)
{
    /* Truncated stream: feed a fake EOI so the decoder terminates. */
    static const JOCTET eoi[2] = { 0xFF, JPEG_EOI };
    cinfo->src->next_input_byte = eoi;
    cinfo->src->bytes_in_buffer = 2;
    return TRUE;
}

static void src_skip(j_decompress_ptr cinfo, long n)
{
    if (n <= 0) return;
    if ((size_t)n >= cinfo->src->bytes_in_buffer) {
        cinfo->src->bytes_in_buffer = 0;
    } else {
        cinfo->src->next_input_byte += n;
        cinfo->src->bytes_in_buffer -= (size_t)n;
    }
}

static void src_term(j_decompress_ptr cinfo) { (void)cinfo; }

int decode_jpeg(const uint8_t *data, size_t len, uint8_t *rgb,
                int max_w, int max_h, int *out_w, int *out_h)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_source_mgr src;
    struct err_mgr err;
    int w, h, stride;

    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error_exit;
    if (setjmp(err.jb)) {
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }

    jpeg_create_decompress(&cinfo);

    memset(&src, 0, sizeof src);
    src.init_source = src_init;
    src.fill_input_buffer = src_fill;
    src.skip_input_data = src_skip;
    src.resync_to_restart = jpeg_resync_to_restart;
    src.term_source = src_term;
    src.next_input_byte = data;
    src.bytes_in_buffer = len;
    cinfo.src = &src;

    jpeg_read_header(&cinfo, TRUE);
    /* RGB565 output: matches the display's native format so the GL
     * driver uploads without a slow CPU conversion pass */
    cinfo.out_color_space = JCS_RGB565;
    cinfo.dct_method = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&cinfo);

    w = (int)cinfo.output_width;
    h = (int)cinfo.output_height;
    if (w > max_w || h > max_h) {
        fprintf(stderr, "jpeg: unsupported frame %dx%d\n", w, h);
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }

    stride = w * 2;
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = rgb + (size_t)cinfo.output_scanline * stride;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    *out_w = w;
    *out_h = h;
    return 1;
}
