/* Copyright (C) Mark Zealey, 2002, <mark@zealos.org>
 *
 * 30/03/02: An almost total rewrite, added DR support and support for modes
 * other than 16bpp. Fixed the crash when playing multiple files
 * 07/04/02: Fixed DR support, added YUY2 support, fixed OSD stuff.
 * 08/04/02: Fixed a wierd sound corruption problem caused by some optomizations
 * I made.
 *
 * TODO:
 * - Add DR support (for stuff other than YUY2)
 * - Add UYVY support
 * - Add zoom support
 * - Remove some of the old dud code
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "config.h"
#include "video_out.h"
#include "video_out_internal.h"
#include "mp_image.h"

LIBVO_EXTERN(tdfxfb)

#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <linux/fb.h>

#include "drivers/3dfx.h"

static vo_info_t vo_info =
{
	"tdfxfb (/dev/fb?)",
	"tdfxfb",
	"Mark Zealey <mark@zealos.org>"
	""
};

/* Some registers on the card */
#define S2S_STRECH_BLT		2			// BLT + Strech
#define S2S_IMMED		(1 << 8)		// Do it immediatly
#define S2S_ROP			(0xCC << 24)		// ???

/* Stepping between the different YUV plane registers */
#define YUV_STRIDE 1024
struct YUV_plane {
  char Y[0x0100000];
  char U[0x0100000];
  char V[0x0100000];
};

/* XXX: Get rid of most of these vars... */
extern int verbose;

static int fd;
static struct fb_fix_screeninfo fb_finfo;
static struct fb_var_screeninfo fb_vinfo;

static uint32_t in_width, in_height, in_format, in_depth, in_banshee_format,
	screenwidth, screenheight, screendepth, vidwidth, vidheight, vidx, vidy,
	vid_banshee_format, *vidpage, *inpage, vidpageoffset,
	inpageoffset, *memBase0, *memBase1;

static volatile voodoo_io_reg *reg_IO;
static voodoo_2d_reg *reg_2d;
static voodoo_yuv_reg *reg_YUV;
static struct YUV_plane *YUV;

static uint32_t preinit(const char *arg)
{
	char *name;

	if(verbose) printf("tdfxfb: Open\n");

	if(!(name = getenv("FRAMEBUFFER")))
		name = "/dev/fb0";

	if (verbose)
		printf("tdfxfb: Opening %s\n", name);

	if ((fd = open(name, O_RDWR)) == -1) {
		printf("tdfxfb: can't open %s: %s\n", name, strerror(errno));
		return -1;
	}

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fb_finfo)) {
		printf("tdfxfb: problem with FBITGET_FSCREENINFO ioctl: %s\n",
				strerror(errno));
		return -1;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &fb_vinfo)) {
		printf("tdfxfb: problem with FBITGET_VSCREENINFO ioctl: %s\n",
				strerror(errno));
		return -1;
	}

	if (verbose) {
		printf("fb_finfo:\n");
		printf("  id: %s\n", fb_finfo.id);
		printf("  frame bufer at %x len %x (%d)\n", fb_finfo.smem_start,
				fb_finfo.smem_len, fb_finfo.smem_len);
		printf("  mem io      at %x len %x\n", fb_finfo.mmio_start,
				fb_finfo.mmio_len);

		printf("fb_vinfo:\n");
		printf("  resolution:  %dx%d\n", fb_vinfo.xres, fb_vinfo.yres);
		printf("  virtual res: %dx%d\n", fb_vinfo.xres_virtual,
				fb_vinfo.yres_virtual);
		printf("  virt offset: %dx%d\n", fb_vinfo.xoffset, fb_vinfo.yoffset);
	}

	if (fb_finfo.accel != FB_ACCEL_3DFX_BANSHEE)
		printf("tdfxfb: This driver is only known to support the banshee!\n");

	/* Open up a window to the hardware */
	memBase1 = mmap(0, fb_finfo.smem_len, PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, 0);
	memBase0 = mmap(0, fb_finfo.mmio_len, PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, fb_finfo.smem_len);

	if((long)memBase0 == -1 || (long)memBase1 == -1) {
		printf("tdfxfb: Couldn't map memory areas: %s\n", strerror(errno));
		return -1;
	}

	/* Set up global pointers to the voodoo's regs */
	reg_IO  = (void *)memBase0 + VOODOO_IO_REG_OFFSET;
	reg_2d  = (void *)memBase0 + VOODOO_2D_REG_OFFSET;
	reg_YUV = (void *)memBase0 + VOODOO_YUV_REG_OFFSET;
	YUV = (void *)memBase0 + VOODOO_YUV_PLANE_OFFSET;

	return 0;
}

static void uninit(void)
{
	if(verbose) printf("tdfxfb: Close\n");

	if(reg_IO) {
		/* Restore the screen (Linux lives at 0) */
		reg_IO->vidDesktopStartAddr = 0;
		reg_IO = NULL;
	}

	/* And close our mess */
	if(memBase1) {
		munmap(memBase1, fb_finfo.smem_len);
		memBase1 = NULL;
	}

	if(memBase0) {
		munmap(memBase0, fb_finfo.mmio_len);
		memBase0 = NULL;
	}

	if(fd != -1) {
		close(fd);
		fd = -1;
	}
}

static uint32_t config(uint32_t width, uint32_t height, uint32_t d_width, uint32_t d_height,
		uint32_t fullscreen, char *title, uint32_t format,const vo_tune_info_t *info)
{
	if(verbose) printf("tdfxfb: Config\n");

	screenwidth = fb_vinfo.xres;
	screenheight = fb_vinfo.yres;

	in_width = width;
	in_height = height;
	in_format = format;

	/* Setup the screen for rendering to */
	switch(fb_vinfo.bits_per_pixel) {
	case 16:
		screendepth = 2;
		vid_banshee_format = VOODOO_BLT_FORMAT_16;
		break;

	case 24:
		screendepth = 3;
		vid_banshee_format = VOODOO_BLT_FORMAT_24;
		break;

	case 32:
		screendepth = 4;
		vid_banshee_format = VOODOO_BLT_FORMAT_32;
		break;

	default:
		printf("tdfxfb: %d bpp output is not supported\n", fb_vinfo.bits_per_pixel);
		return -1;
	}

	vid_banshee_format |= screenwidth * screendepth;

	in_banshee_format = VOODOO_BLT_FORMAT_YUYV;
	/* Arpi says we don't need to support RGB */
	switch(in_format) {
	case IMGFMT_YV12:
	case IMGFMT_I420:
	case IMGFMT_IYUV:
		in_depth = 4;
		break;

	case IMGFMT_YUY2:
		in_depth = 2;
		break;

	default:
		printf("tdfxfb: Eik! Something's wrong with control().\n");
		return -1;
	}

	in_banshee_format |= in_width * in_depth;

	if (fullscreen) {
		double exrat;

		vidwidth = screenwidth;
		vidheight = screenheight;

		exrat = (double)in_width / in_height;

		if(screenwidth / exrat <= screenheight)
			vidheight = (double)screenwidth / exrat;
		else
			vidwidth = (double)screenheight * exrat;

		vidx = (screenwidth - vidwidth) / 2;
		vidy = (screenheight - vidheight) / 2;

		if(verbose)
			printf("vidwidth = %d, vidheight = %d, vidx = %d, vidy = %d\n",
					vidwidth, vidheight, vidx, vidy);
	} else {
		if (in_width > screenwidth || in_height > screenheight) {
			printf("tdfxfb: your resolution is too small to play the movie...\n");
			return -1;
		}

		vidwidth = in_width;
		vidheight = in_height;
		vidx = 0;
		vidy = 0;
	}

	/* Linux lives in the first frame */
	vidpageoffset = screenwidth * screenheight * screendepth;
	inpageoffset = vidpageoffset + screenwidth * screenheight * screendepth;

	if(inpageoffset + in_width * in_depth * in_height > fb_finfo.smem_len) {
		printf("tdfxfb: Not enough video memory to play this movie. Try at a lower resolution\n");
		return -1;
	}

	vidpage = (void *)memBase1 + (unsigned long)vidpageoffset;
	inpage = (void *)memBase1 + (unsigned long)inpageoffset;

	memset(vidpage, 0, screenwidth * screenheight * screendepth);
	memset(inpage, 0, in_width * in_height * in_depth);

	printf("tdfxfb: screen is %dx%d at %d bpp, in is %dx%d at %d bpp (%p/%p)\n",
			screenwidth, screenheight, screendepth * 8,
			in_width, in_height, in_depth * 8,
			memBase0, memBase1);

	return 0;
}

static const vo_info_t* get_info(void)
{
	return &vo_info;
}

/* Render onto the screen */
static void flip_page(void)
{
	voodoo_2d_reg regs = *reg_2d;		/* Copy the regs */
	int i = 0;

	reg_2d->commandExtra = 0;
	reg_2d->clip0Min = 0;
	reg_2d->clip0Max = 0xffffffff;

	reg_2d->srcBaseAddr = inpageoffset;
	reg_2d->srcXY = 0;
	reg_2d->srcFormat = in_banshee_format;
	reg_2d->srcSize = XYREG(in_width, in_height);

	reg_2d->dstBaseAddr = vidpageoffset;
	reg_2d->dstXY = XYREG(vidx, vidy);
	reg_2d->dstFormat = vid_banshee_format;
	reg_2d->dstSize = XYREG(vidwidth, vidheight);
	reg_2d->command = S2S_STRECH_BLT | S2S_IMMED | S2S_ROP;

	/* Wait for the command to finish (If we don't do this, we get wierd
	 * sound corruption... */
	while((reg_IO->status & 0x1f) < 1)
		/* Wait */;

	*((volatile uint32_t *)((uint32_t *)reg_IO + COMMAND_3D)) = COMMAND_3D_NOP;

	while(i < 3)
		if(!(reg_IO->status & STATUS_BUSY))
			i++;

	/* Reset the banshee to point at the video page if it got knocked off it
	 */
	reg_IO->vidDesktopStartAddr = vidpageoffset;

	/* Restore the old regs now */
	reg_2d->commandExtra = regs.commandExtra;
	reg_2d->clip0Min = regs.clip0Min;
	reg_2d->clip0Max = regs.clip0Max;

	reg_2d->srcBaseAddr = regs.srcBaseAddr;
	reg_2d->srcXY = regs.srcXY;
	reg_2d->srcFormat = regs.srcFormat;
	reg_2d->srcSize = regs.srcSize;

	reg_2d->dstBaseAddr = regs.dstBaseAddr;
	reg_2d->dstXY = regs.dstXY;
	reg_2d->dstFormat = regs.dstFormat;
	reg_2d->dstSize = regs.dstSize;

	reg_2d->command = 0;
}

static uint32_t draw_frame(uint8_t *src[])
{
	switch(in_format) {
	case IMGFMT_YUY2:
		memcpy(inpage, src[0], in_width * in_depth * in_height);
		break;
	}

	return 0;
}

static uint32_t draw_slice(uint8_t *i[], int s[], int w, int h, int x, int y)
{
	switch(in_format) {
	case IMGFMT_YV12:		/* The normal case */
	case IMGFMT_I420:
	case IMGFMT_IYUV: {
		int j;
		char *base;

		/* We want to render to the YUV to the input page + the location
		 * of the stripes we're doing */
		reg_YUV->yuvBaseAddr = inpageoffset + in_width * in_depth * y + x;
		reg_YUV->yuvStride = in_width * in_depth;

		/* Put the YUV channels into the banshees internal combiner unit
		 * thingie */
		for(base = YUV->Y, j = 0; j < h; j++, base += YUV_STRIDE, i[0] += s[0])
			memcpy(base, i[0], s[0]);
		for(base = YUV->U, j = 0; j < h / 2; j++, base += YUV_STRIDE, i[1] += s[1])
			memcpy(base, i[1], s[1]);
		for(base = YUV->V, j = 0; j < h / 2; j++, base += YUV_STRIDE, i[2] += s[2])
			memcpy(base, i[2], s[2]);
		break;
	}

	/* No need for a default case, config would have handled it */
	}

	return 0;
}

static void draw_alpha(int x, int y, int w, int h, unsigned char *src,
		unsigned char *srca, int stride)
{
	/* 2... WTF... */
	char *dst = (char *)inpage + (y * in_width + x) * 2;
	vo_draw_alpha_yuy2(w, h, src, srca, stride, dst, in_width * in_depth);
}

static void draw_osd(void)
{
	vo_draw_text(in_width, in_height, draw_alpha);
}

/* Attempt to start doing DR (Copied mostly from mga_common.c) */
static uint32_t get_image(mp_image_t *mpi)
{
	static int enabled = 0;

	if(!enabled) {
		if(mpi->flags & MP_IMGFLAG_READABLE)	/* slow video ram */
			return VO_FALSE;

		switch(in_format) {
		case IMGFMT_YUY2:
			mpi->planes[0] = (char *)inpage;
			mpi->stride[0] = in_width * in_depth;
			break;

		default:
#if 0
			if(!(mpi->flags & MP_IMGFLAG_ACCEPT_STRIDE))
				return VO_FALSE;

			mpi->planes[0] = YUV->Y;
			mpi->planes[1] = YUV->U;
			mpi->planes[2] = YUV->V;
			mpi->stride[0] = mpi->stride[1] = mpi->stride[2] = YUV_STRIDE;
#else /* 0 */
			return VO_FALSE;
#endif /* 0 */
		}

		printf("tdfxfb: get_image() SUCCESS -> Direct Rendering ENABLED\n");

		enabled = 1;
	}

	mpi->width = in_width;
	mpi->flags |= MP_IMGFLAG_DIRECT;

	return VO_TRUE;
}

static uint32_t control(uint32_t request, void *data, ...)
{
	switch(request) {
	case VOCTRL_GET_IMAGE:
		return get_image(data);

	case VOCTRL_QUERY_FORMAT:
		switch(*((uint32_t*)data)) {
		case IMGFMT_YV12:
		case IMGFMT_I420:
		case IMGFMT_IYUV:
		case IMGFMT_YUY2:
			return 7;	/* Supported without conversion, supports OSD */
		}

		return 0;		/* Not supported */
	}

	return VO_NOTIMPL;
}

/* Dummy funcs */
static void check_events(void) { }
