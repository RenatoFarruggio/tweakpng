// pngtodib.cpp - part of TweakPNG
/*
    Copyright (C) 2011 Jason Summers

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    See the file tweakpng-src.txt for more information.
*/

#include "twpng-config.h"

#ifdef TWPNG_SUPPORT_VIEWER

#include <windows.h>
#include <tchar.h>

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>

#include <png.h>

#include "pngtodib.h"
#include <strsafe.h>

#define PNGDIB_ERRMSG_MAX 200

// Color correction methods
#define P2D_CC_GAMMA 1
#define P2D_CC_SRGB  2


struct PNGD_COLOR_struct {
	unsigned char red, green, blue, reserved;
};

struct PNGD_COLOR_fltpt_struct {
	double red, green, blue;
};

struct p2d_struct {
	void *userdata;
	TCHAR *errmsg;

	pngdib_read_cb_type read_cb;

	png_uint_32 width, height;

	int use_file_bg_flag;

	struct PNGD_COLOR_struct bkgd_color_custom_srgb; // sRGB color space, 0-255
	struct PNGD_COLOR_fltpt_struct bkgd_color_custom_linear; // linear, 0.0-1.0
	int use_custom_bg_flag;

	struct PNGD_COLOR_struct bkgd_color_applied_src;
	struct PNGD_COLOR_struct bkgd_color_applied_srgb;
	struct PNGD_COLOR_fltpt_struct bkgd_color_applied_linear;
	int bkgd_color_applied_flag;

	int color_correction_enabled;

	BITMAPINFOHEADER*   pdib;
	int        dibsize;
	int        bits_offs;
	int        bitssize;
	RGBQUAD*   palette;
	void*      pbits;
	int        res_x,res_y;
	int        res_units;
	int        res_valid;  // are res_x, res_y, res_units valid?

	png_structp png_ptr;
	png_infop info_ptr;

	int is_grayscale;
	int pngf_bit_depth;
	int has_trns;
	int color_type;
	int manual_trns;

	// Color information about the source image. The destination image is always sRGB.
	int color_correction_type; // P2D_CC_*
	double file_gamma; // Used if color_correction_type==P2D_CC_GAMMA

	int pngf_palette_entries;
	png_colorp pngf_palette;

	unsigned char **dib_row_pointers;

	double *src_to_linear_table;
	unsigned char *src_to_dst_table;
	unsigned char *linear_to_srgb_table;

	int dib_palette_entries;
	int need_gray_palette;
};

struct errstruct {
	jmp_buf *jbufp;
	TCHAR *errmsg;
};

static void pngd_get_error_message(int errcode, TCHAR *e, int e_len)
{
	switch(errcode) {
	case PNGD_E_ERROR:   StringCchCopy(e,e_len,_T("Unknown error")); break;
	case PNGD_E_NOMEM:   StringCchCopy(e,e_len,_T("Unable to allocate memory")); break;
	case PNGD_E_LIBPNG:  StringCchCopy(e,e_len,_T("libpng reported an error")); break;
	case PNGD_E_BADPNG:  StringCchCopy(e,e_len,_T("Invalid PNG image")); break;
	case PNGD_E_READ:    StringCchCopy(e,e_len,_T("Unable to read file")); break;
	}
}

static void my_png_error_fn(png_structp png_ptr, const char *err_msg)
{
	struct errstruct *errinfop;
	jmp_buf *j;

	errinfop = (struct errstruct *)png_get_error_ptr(png_ptr);
	j = errinfop->jbufp;

#ifdef _UNICODE
	StringCchPrintf(errinfop->errmsg,PNGDIB_ERRMSG_MAX,_T("[libpng] %S"),err_msg);
#else
	StringCchPrintf(errinfop->errmsg,PNGDIB_ERRMSG_MAX,"[libpng] %s",err_msg);
#endif

	longjmp(*j, -1);
}

static void my_png_warning_fn(png_structp png_ptr, const char *warn_msg)
{
	return;
}

// A callback function used with custom I/O.
static void my_png_read_fn_custom(png_structp png_ptr,
      png_bytep data, png_size_t length)
{
	struct p2d_struct *p2d;
	int ret;

	p2d = (struct p2d_struct*)png_get_io_ptr(png_ptr);
	if(!p2d->read_cb) return;

	ret = p2d->read_cb(p2d->userdata,(void*)data,(int)length);
	if(ret < (int)length) {
		// This error message is just a guess. It might be nice to
		// have a way to get a real error message.
		png_error(png_ptr, "Read error: Unexpected end of file");
	}
}

// Reads pHYs chunk, sets p2d->res_*.
static void p2d_read_density(PNGDIB *p2d)
{
	int has_phys;
	int res_unit_type;
	png_uint_32 res_x, res_y;

	has_phys=png_get_valid(p2d->png_ptr,p2d->info_ptr,PNG_INFO_pHYs);
	if(!has_phys) return;

	png_get_pHYs(p2d->png_ptr,p2d->info_ptr,&res_x,&res_y,&res_unit_type);
	if(res_x<1 || res_y<1) return;

	p2d->res_x=res_x;
	p2d->res_y=res_y;
	p2d->res_units=res_unit_type;
	p2d->res_valid=1;
}

static double src255_to_linear_sample(PNGDIB *p2d, unsigned char sample)
{
	if(p2d->src_to_linear_table) {
		return p2d->src_to_linear_table[sample];
	}
	return ((double)sample)/255.0;
}

static unsigned char src255_to_srgb255_sample(PNGDIB *p2d, unsigned char sample)
{
	if(p2d->src_to_dst_table) {
		return p2d->src_to_dst_table[sample];
	}
	return sample;
}

static void p2d_read_bgcolor(PNGDIB *p2d)
{
	png_color_16p bg_colorp;
	unsigned char tmpcolor;
	int has_bkgd;

	if(!p2d->use_file_bg_flag) {
		// Using the background from the file is disabled.
		return;
	}

	has_bkgd = png_get_bKGD(p2d->png_ptr, p2d->info_ptr, &bg_colorp);
	if(!has_bkgd) {
		return;
	}

	if(p2d->is_grayscale) {
		if(p2d->pngf_bit_depth<8) {
			tmpcolor = (unsigned char) ( (bg_colorp->gray*255)/( (1<<p2d->pngf_bit_depth)-1 ) );
		}
		else if(p2d->pngf_bit_depth==16) {
			tmpcolor = (unsigned char)(bg_colorp->gray>>8);
		}
		else {
			tmpcolor = (unsigned char)(bg_colorp->gray);
		}

		p2d->bkgd_color_applied_src.red  = p2d->bkgd_color_applied_src.green = p2d->bkgd_color_applied_src.blue = tmpcolor;
		p2d->bkgd_color_applied_flag = 1;
	}
	else if(p2d->pngf_bit_depth<=8) { // RGB[A]8 or palette
		p2d->bkgd_color_applied_src.red  =(unsigned char)(bg_colorp->red);
		p2d->bkgd_color_applied_src.green=(unsigned char)(bg_colorp->green);
		p2d->bkgd_color_applied_src.blue =(unsigned char)(bg_colorp->blue);
		p2d->bkgd_color_applied_flag = 1;
	}
	else {
		p2d->bkgd_color_applied_src.red  =(unsigned char)(bg_colorp->red>>8);
		p2d->bkgd_color_applied_src.green=(unsigned char)(bg_colorp->green>>8);
		p2d->bkgd_color_applied_src.blue =(unsigned char)(bg_colorp->blue>>8);
		p2d->bkgd_color_applied_flag = 1;
	}

	if(!p2d->bkgd_color_applied_flag) return;

	p2d->bkgd_color_applied_linear.red   = src255_to_linear_sample(p2d,p2d->bkgd_color_applied_src.red);
	p2d->bkgd_color_applied_linear.green = src255_to_linear_sample(p2d,p2d->bkgd_color_applied_src.green);
	p2d->bkgd_color_applied_linear.blue  = src255_to_linear_sample(p2d,p2d->bkgd_color_applied_src.blue);

	p2d->bkgd_color_applied_srgb.red   = src255_to_srgb255_sample(p2d,p2d->bkgd_color_applied_src.red);
	p2d->bkgd_color_applied_srgb.green = src255_to_srgb255_sample(p2d,p2d->bkgd_color_applied_src.green);
	p2d->bkgd_color_applied_srgb.blue  = src255_to_srgb255_sample(p2d,p2d->bkgd_color_applied_src.blue);
}

static void p2d_read_gamma(PNGDIB *p2d)
{
	int intent;

	if(!p2d->color_correction_enabled) return;

	if (png_get_sRGB(p2d->png_ptr, p2d->info_ptr, &intent)) {
		p2d->color_correction_type = P2D_CC_SRGB;
		p2d->file_gamma = 0.45455;
	}
	else if(png_get_gAMA(p2d->png_ptr, p2d->info_ptr, &p2d->file_gamma)) {
		if(p2d->file_gamma<0.01) p2d->file_gamma=0.01;
		if(p2d->file_gamma>10.0) p2d->file_gamma=10.0;
		p2d->color_correction_type = P2D_CC_GAMMA;
	}
	else {
		// Assume unlabeled images are sRGB.
		p2d->color_correction_type = P2D_CC_SRGB;
		p2d->file_gamma = 0.45455;
	}
}

static void p2d_read_or_create_palette(PNGDIB *p2d)
{
	int i;

	if(p2d->need_gray_palette) {
		for(i=0;i<p2d->dib_palette_entries;i++) {
			p2d->palette[i].rgbRed   = i;
			p2d->palette[i].rgbGreen = i;
			p2d->palette[i].rgbBlue  = i;
		}
		return;
	}
}

// Expand 2bpp to 4bpp
static int p2d_convert_2bit_to_4bit(PNGDIB *p2d)
{
	unsigned char *tmprow;
	int i,j;

	tmprow = (unsigned char*)malloc((p2d->width+3)/4 );
	if(!tmprow) { return 0; }

	for(j=0;j<(int)p2d->height;j++) {
		CopyMemory(tmprow, p2d->dib_row_pointers[j], (p2d->width+3)/4 );
		ZeroMemory(p2d->dib_row_pointers[j], (p2d->width+1)/2 );

		for(i=0;i<(int)p2d->width;i++) {
			p2d->dib_row_pointers[j][i/2] |= 
				( ((tmprow[i/4] >> (2*(3-i%4)) ) & 0x03)<< (4*(1-i%2)) );
		}
	}
	free((void*)tmprow);
	return 1;
}

static double gamma_to_linear_sample(double v, double gamma)
{
	return pow(v,gamma);
}

static double linear_to_srgb_sample(double v_linear)
{
	if(v_linear <= 0.0031308) {
		return 12.92*v_linear;
	}
	return 1.055*pow(v_linear,1.0/2.4) - 0.055;
}

static double srgb_to_linear_sample(double v_srgb)
{
	if(v_srgb<=0.04045) {
		return v_srgb/12.92;
	}
	else {
		return pow( (v_srgb+0.055)/(1.055) , 2.4);
	}
}

static int p2d_make_color_correction_tables(PNGDIB *p2d)
{
	int n;
	double val_src;
	double val_linear;
	double val_dst;
	double val;

	p2d->src_to_linear_table = (double*)malloc(256*sizeof(double));
	if(!p2d->src_to_linear_table) return 0;
	p2d->linear_to_srgb_table = (unsigned char*)malloc(256*sizeof(unsigned char));
	if(!p2d->linear_to_srgb_table) return 0;
	p2d->src_to_dst_table = (unsigned char*)malloc(256*sizeof(unsigned char));
	if(!p2d->src_to_dst_table) return 0;

	for(n=0;n<256;n++) {
		val_src = ((double)n)/255.0;

		if(p2d->color_correction_enabled) {
			if(p2d->color_correction_type==P2D_CC_SRGB) {
				val_linear = srgb_to_linear_sample(val_src);
			}
			else if(p2d->color_correction_type==P2D_CC_GAMMA) {
				val_linear =  gamma_to_linear_sample(val_src,1.0/p2d->file_gamma);
			}
			else {
				val_linear = val_src;
			}

			// TODO: This is only needed if there is partial transparency.
			p2d->src_to_linear_table[n] = val_linear;
			
			val_dst = linear_to_srgb_sample(val_linear);
			p2d->src_to_dst_table[n] = (unsigned char)(0.5+val_dst*255.0);

			// TODO: This doesn't need to be recalculated every time.
			val = linear_to_srgb_sample(val_src);
			p2d->linear_to_srgb_table[n] = (unsigned char)(0.5+val*255.0);
		}
		else {
			// "dummy" tables
			p2d->src_to_linear_table[n] = val_src;
			p2d->src_to_dst_table[n] = n;
			p2d->linear_to_srgb_table[n] = n;
		}
	}
	return 1;
}

// Handle cases where the PNG image can be read directly into the DIB image
// buffer.
static int decode_strategy_8bit_direct(PNGDIB *p2d, int samples_per_pixel)
{
	size_t i, j;
	int samples_per_row;

	if(samples_per_pixel==3) {
		png_set_bgr(p2d->png_ptr);
	}

	png_read_image(p2d->png_ptr, p2d->dib_row_pointers);

	// With no transparency, sRGB source images don't need color correction.
	if(p2d->color_correction_type == P2D_CC_GAMMA) {
		samples_per_row = samples_per_pixel*p2d->width;

		for(j=0;j<p2d->height;j++) {
			for(i=0;i<samples_per_row;i++) {
				p2d->dib_row_pointers[j][i] = p2d->src_to_dst_table[p2d->dib_row_pointers[j][i]];
			}
		}
	}
	return 1;
}

static int decode_strategy_rgba(PNGDIB *p2d)
{
	size_t i, j;
	unsigned char *pngimage = NULL;
	unsigned char **pngrowpointers = NULL;
	double r,g,b,a;
	double r_b,g_b,b_b; // composited with background color

	pngimage = (unsigned char*)malloc(4*p2d->width*p2d->height);
	if(!pngimage) goto done;
	pngrowpointers = (unsigned char**)malloc(p2d->height*sizeof(unsigned char*));
	if(!pngrowpointers) goto done;
	for(j=0;j<p2d->height;j++) {
		pngrowpointers[j] = &pngimage[j*4*p2d->width];
	}

	png_read_image(p2d->png_ptr, pngrowpointers);

	for(j=0;j<p2d->height;j++) {
		for(i=0;i<p2d->width;i++) {
			r = src255_to_linear_sample(p2d,pngrowpointers[j][i*4+0]);
			g = src255_to_linear_sample(p2d,pngrowpointers[j][i*4+1]);
			b = src255_to_linear_sample(p2d,pngrowpointers[j][i*4+2]);
			a = ((double)pngrowpointers[j][i*4+3])/255.0;

			r_b  = a*r + (1.0-a)*p2d->bkgd_color_applied_linear.red;
			g_b  = a*g + (1.0-a)*p2d->bkgd_color_applied_linear.green;
			b_b  = a*b + (1.0-a)*p2d->bkgd_color_applied_linear.blue;

			// TODO: This is not perfect.
			// Instead of quantizing to the nearest linear color and then converting it to sRGB,
			// we should use the quantized sRGB color that is nearest in a linear
			// colorspace. There's no easy and efficient way to do that, though.
			p2d->dib_row_pointers[j][i*3+0] = p2d->linear_to_srgb_table[(unsigned char)(0.5+b_b*255.0)];
			p2d->dib_row_pointers[j][i*3+1] = p2d->linear_to_srgb_table[(unsigned char)(0.5+g_b*255.0)];
			p2d->dib_row_pointers[j][i*3+2] = p2d->linear_to_srgb_table[(unsigned char)(0.5+r_b*255.0)];
		}
	}

done:
	if(pngimage) free(pngimage);
	if(pngrowpointers) free(pngrowpointers);
	return 1;
}

// gray+alpha
static int decode_strategy_graya(PNGDIB *p2d, int tocolor)
{
	size_t i, j;
	unsigned char *pngimage = NULL;
	unsigned char **pngrowpointers = NULL;
	double g,a;
	double r_b,g_b,b_b; // composited with background color (g_b is gray or green)

	pngimage = (unsigned char*)malloc(2*p2d->width*p2d->height);
	if(!pngimage) goto done;
	pngrowpointers = (unsigned char**)malloc(p2d->height*sizeof(unsigned char*));
	if(!pngrowpointers) goto done;
	for(j=0;j<p2d->height;j++) {
		pngrowpointers[j] = &pngimage[j*2*p2d->width];
	}

	png_read_image(p2d->png_ptr, pngrowpointers);

	for(j=0;j<p2d->height;j++) {
		for(i=0;i<p2d->width;i++) {
			g = src255_to_linear_sample(p2d,pngrowpointers[j][i*2]);
			a = ((double)pngrowpointers[j][i*2+1])/255.0;

			if(tocolor) {
				r_b  = a*g + (1.0-a)*p2d->bkgd_color_applied_linear.red;
				g_b  = a*g + (1.0-a)*p2d->bkgd_color_applied_linear.green;
				b_b  = a*g + (1.0-a)*p2d->bkgd_color_applied_linear.blue;
				p2d->dib_row_pointers[j][i*3+0] = p2d->linear_to_srgb_table[(unsigned char)(0.5+b_b*255.0)];
				p2d->dib_row_pointers[j][i*3+1] = p2d->linear_to_srgb_table[(unsigned char)(0.5+g_b*255.0)];
				p2d->dib_row_pointers[j][i*3+2] = p2d->linear_to_srgb_table[(unsigned char)(0.5+r_b*255.0)];
			}
			else {
				g_b  = a*g + (1.0-a)*p2d->bkgd_color_applied_linear.red;
				p2d->dib_row_pointers[j][i] = p2d->linear_to_srgb_table[(unsigned char)(0.5+g_b*255.0)];
			}
		}
	}

done:
	if(pngimage) free(pngimage);
	if(pngrowpointers) free(pngrowpointers);
	return 1;
}

static int decode_strategy_palette(PNGDIB *p2d)
{
	int i;
	int retval=0;
	png_bytep trans_alpha;
	png_color_16p trans_color;
	int num_trans;
	double sm[3];
	double trns_alpha_1;

	// Copy the PNG palette to the DIB palette
	if(p2d->pngf_palette_entries != p2d->dib_palette_entries) return 0;

	num_trans=0;
	if(p2d->has_trns && p2d->bkgd_color_applied_flag) {
		png_get_tRNS(p2d->png_ptr, p2d->info_ptr, &trans_alpha, &num_trans, &trans_color);
	}
	// Copy the PNG palette to the DIB palette, handling color correction
	// and transparency in the process.
	for(i=0;i<p2d->dib_palette_entries;i++) {
		sm[0] = src255_to_linear_sample(p2d,p2d->pngf_palette[i].red);
		sm[1] = src255_to_linear_sample(p2d,p2d->pngf_palette[i].green);
		sm[2] = src255_to_linear_sample(p2d,p2d->pngf_palette[i].blue);

		// Apply background color
		if(i < num_trans) {
			trns_alpha_1 = ((double)trans_alpha[i])/255.0;
			sm[0] = trns_alpha_1*sm[0] + (1.0-trns_alpha_1)*p2d->bkgd_color_applied_linear.red;
			sm[1] = trns_alpha_1*sm[1] + (1.0-trns_alpha_1)*p2d->bkgd_color_applied_linear.green;
			sm[2] = trns_alpha_1*sm[2] + (1.0-trns_alpha_1)*p2d->bkgd_color_applied_linear.blue;
		}

		p2d->palette[i].rgbRed   = p2d->linear_to_srgb_table[(unsigned char)(0.5+sm[0]*255.0)];
		p2d->palette[i].rgbGreen = p2d->linear_to_srgb_table[(unsigned char)(0.5+sm[1]*255.0)];
		p2d->palette[i].rgbBlue  = p2d->linear_to_srgb_table[(unsigned char)(0.5+sm[2]*255.0)];
		p2d->palette[i].rgbReserved = 0;
	}

	// Directly read the image into the DIB.
	png_read_image(p2d->png_ptr, p2d->dib_row_pointers);

	if(p2d->pngf_bit_depth==2) {
		// Special handling for this bit depth, since it doesn't exist in DIBs.
		if(!p2d_convert_2bit_to_4bit(p2d)) goto done;
	}

	retval=1;
done:
	return retval;
}

int pngdib_p2d_run(PNGDIB *p2d)
{
	jmp_buf jbuf;
	struct errstruct errinfo;
	int interlace_type;
	unsigned char *lpdib;
	unsigned char *dib_palette;
	unsigned char *dib_bits;
	int dib_bpp, dib_bytesperrow;
	int j;
	int retval;
	size_t palette_offs;
	enum p2d_strategy { P2D_ST_NONE, P2D_ST_8BIT_DIRECT, P2D_ST_RGBA, P2D_ST_GRAYA,
	  P2D_ST_PALETTE };
	enum p2d_strategy decode_strategy;
	int strategy_spp=0;
	int strategy_tocolor=0;
	int bg_is_gray;

	p2d->manual_trns=0;
	p2d->has_trns=0;
	retval=PNGD_E_ERROR;
	p2d->png_ptr=NULL;
	p2d->info_ptr=NULL;
	p2d->dib_row_pointers=NULL;
	lpdib=NULL;
	decode_strategy= P2D_ST_NONE;

	StringCchCopy(p2d->errmsg,PNGDIB_ERRMSG_MAX,_T(""));


	if(p2d->use_custom_bg_flag) {
		p2d->bkgd_color_applied_srgb = p2d->bkgd_color_custom_srgb; // struct copy
		p2d->bkgd_color_applied_flag = 1;
	}
	else {
		p2d->bkgd_color_applied_srgb.red   = 255; // Should never get used. If the
		p2d->bkgd_color_applied_srgb.green = 128; // background turns orange, it's a bug.
		p2d->bkgd_color_applied_srgb.blue  =  0;
	}

	if(p2d->color_correction_enabled) {
		// Also store the custom background color in a linear colorspace, since that's
		// what we'll need if we have to apply it to the image.
		p2d->bkgd_color_applied_linear.red   = srgb_to_linear_sample(((double)p2d->bkgd_color_applied_srgb.red)/255.0);
		p2d->bkgd_color_applied_linear.green = srgb_to_linear_sample(((double)p2d->bkgd_color_applied_srgb.green)/255.0);
		p2d->bkgd_color_applied_linear.blue  = srgb_to_linear_sample(((double)p2d->bkgd_color_applied_srgb.blue)/255.0);
	}
	else {
		p2d->bkgd_color_applied_linear.red   = ((double)p2d->bkgd_color_applied_srgb.red)/255.0;
		p2d->bkgd_color_applied_linear.green = ((double)p2d->bkgd_color_applied_srgb.green)/255.0;
		p2d->bkgd_color_applied_linear.blue  = ((double)p2d->bkgd_color_applied_srgb.blue)/255.0;
	}


	// Set the user-defined pointer to point to our jmp_buf. This will
	// hopefully protect against potentially different sized jmp_buf's in
	// libpng, while still allowing this library to be threadsafe.
	errinfo.jbufp = &jbuf;
	errinfo.errmsg = p2d->errmsg;

	p2d->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,(void*)(&errinfo),
		my_png_error_fn, my_png_warning_fn);
	if(!p2d->png_ptr) { retval=PNGD_E_NOMEM; goto done; }

	png_set_user_limits(p2d->png_ptr,100000,100000); // max image dimensions

#if PNG_LIBPNG_VER >= 10400
	// Number of ancillary chunks stored.
	// I don't think we need any of these, but there appears to be no
	// way to set the limit to 0. (0 is reserved to mean "unlimited".)
	// I'll just set it to an arbitrary low number.
	png_set_chunk_cache_max(p2d->png_ptr,50);
#endif

#if PNG_LIBPNG_VER >= 10401
	png_set_chunk_malloc_max(p2d->png_ptr,1000000);
#endif

	p2d->info_ptr = png_create_info_struct(p2d->png_ptr);
	if(!p2d->info_ptr) {
		//png_destroy_read_struct(&png_ptr, (png_infopp)NULL, (png_infopp)NULL);
		retval=PNGD_E_NOMEM; goto done;
	}

	if(setjmp(jbuf)) {
		// we'll get here if an error occurred in any of the following
		// png_ functions

		retval=PNGD_E_LIBPNG;
		goto done;
	}

	png_set_read_fn(p2d->png_ptr, (void*)p2d, my_png_read_fn_custom);

	png_read_info(p2d->png_ptr, p2d->info_ptr);

	png_get_IHDR(p2d->png_ptr, p2d->info_ptr, &p2d->width, &p2d->height, &p2d->pngf_bit_depth, &p2d->color_type,
		&interlace_type, NULL, NULL);

	p2d->is_grayscale = !(p2d->color_type&PNG_COLOR_MASK_COLOR);

	p2d->has_trns = png_get_valid(p2d->png_ptr,p2d->info_ptr,PNG_INFO_tRNS);

	p2d_read_gamma(p2d);

	if(!p2d_make_color_correction_tables(p2d)) goto done;

	p2d_read_bgcolor(p2d);

	p2d_read_density(p2d);


	//////// Decide on DIB image type, etc.

	// This is inevitably a complicated part of the code, because we have to
	// cover a lot of different cases, which overlap in various ways.

	// TODO: In some cases this uses libpng to inefficiently convert to a
	// different image type (e.g. palette to RGB), to reduce the number of cases
	// we need to handle. This is intended to be temporary: more algorithms
	// will be added later to make it more efficient.

	p2d->dib_palette_entries=0; // default

	if(p2d->color_type==PNG_COLOR_TYPE_GRAY && !p2d->has_trns) {
		// TODO: It might be better to gamma-correct the palette, instead of the image.
		decode_strategy=P2D_ST_8BIT_DIRECT; strategy_spp=1;
		dib_bpp=8;
		p2d->need_gray_palette=1; p2d->dib_palette_entries=256;
		if(p2d->pngf_bit_depth<8) {
			// TODO: Don't do this.
			png_set_expand_gray_1_2_4_to_8(p2d->png_ptr);
		}
		else if(p2d->pngf_bit_depth==16) {
			png_set_strip_16(p2d->png_ptr);
		}
	}
	else if(p2d->color_type==PNG_COLOR_TYPE_GRAY_ALPHA || (p2d->color_type==PNG_COLOR_TYPE_GRAY && p2d->has_trns) ) {

		// Figure out if the background color is a shade of gray
		bg_is_gray=1;
		if(p2d->bkgd_color_applied_flag) {
			if(p2d->bkgd_color_applied_srgb.red!=p2d->bkgd_color_applied_srgb.green ||
				p2d->bkgd_color_applied_srgb.red!=p2d->bkgd_color_applied_srgb.blue)
			{
				bg_is_gray=0;
			}
		}

		if(p2d->bkgd_color_applied_flag && bg_is_gray) {
			// Applying a gray background.
			decode_strategy=P2D_ST_GRAYA; strategy_tocolor=0;
			dib_bpp=8;
			p2d->need_gray_palette=1; p2d->dib_palette_entries=256;
		}
		else if(p2d->bkgd_color_applied_flag && !bg_is_gray) {
			// Applying a color background to a grayscale image.
			decode_strategy=P2D_ST_GRAYA; strategy_tocolor=1;
			dib_bpp=24;
			//p2d->dib_palette_entries=0;
		}
		else if(!p2d->bkgd_color_applied_flag) {
			// Strip the alpha channel.
			decode_strategy=P2D_ST_8BIT_DIRECT; strategy_spp=1;
			dib_bpp=8;
			p2d->need_gray_palette=1; p2d->dib_palette_entries=256;
			png_set_strip_alpha(p2d->png_ptr);
		}

		if(p2d->color_type==PNG_COLOR_TYPE_GRAY) {
			// TODO: Don't do this.
			png_set_tRNS_to_alpha(p2d->png_ptr);
		}

		if(p2d->pngf_bit_depth<8) {
			// TODO: Don't do this.
			png_set_expand_gray_1_2_4_to_8(p2d->png_ptr);
		}
		else if(p2d->pngf_bit_depth==16) {
			png_set_strip_16(p2d->png_ptr);
		}
	}
	else if(p2d->color_type==PNG_COLOR_TYPE_RGB && !p2d->has_trns) {
		decode_strategy=P2D_ST_8BIT_DIRECT; strategy_spp=3;
		dib_bpp=24;
		if(p2d->pngf_bit_depth==16) {
			png_set_strip_16(p2d->png_ptr);
		}
	}
	else if(p2d->color_type==PNG_COLOR_TYPE_RGB && p2d->has_trns) {
		// RGB with binary transparency.

		dib_bpp=24;

		if(p2d->bkgd_color_applied_flag) {
			decode_strategy=P2D_ST_RGBA;
			if(p2d->pngf_bit_depth==16) {
				png_set_strip_16(p2d->png_ptr);
			}
			// TODO: We could handle transparency ourselves, more efficiently,
			// without promoting it to a whole alpha channel.
			png_set_tRNS_to_alpha(p2d->png_ptr);
		}
		else {
			// Transparency disabled; just ignore the trns chunk.
			decode_strategy=P2D_ST_8BIT_DIRECT; strategy_spp=3;
			if(p2d->pngf_bit_depth==16) {
				png_set_strip_16(p2d->png_ptr);
			}
		}
	}
	else if(p2d->color_type==PNG_COLOR_TYPE_RGB_ALPHA) {
		if(p2d->bkgd_color_applied_flag) {
			decode_strategy=P2D_ST_RGBA;
		}
		else {
			// No background color to use, so strip the alpha channel.
			decode_strategy=P2D_ST_8BIT_DIRECT; strategy_spp=3;
			png_set_strip_alpha(p2d->png_ptr);
		}
		dib_bpp=24;
		if(p2d->pngf_bit_depth==16) {
			png_set_strip_16(p2d->png_ptr);
		}
	}
	else if(p2d->color_type==PNG_COLOR_TYPE_PALETTE) {
		png_get_PLTE(p2d->png_ptr,p2d->info_ptr,&p2d->pngf_palette,&p2d->pngf_palette_entries);
		p2d->dib_palette_entries = p2d->pngf_palette_entries;
		if(p2d->pngf_bit_depth==2) {
			dib_bpp=4;
		}
		else {
			dib_bpp=p2d->pngf_bit_depth;
		}
		decode_strategy=P2D_ST_PALETTE;
	}

	if(decode_strategy==P2D_ST_NONE) {
		StringCchPrintf(p2d->errmsg,PNGDIB_ERRMSG_MAX,_T("Viewer doesn't support this image type"));
		goto done;
	}


	//////// Calculate the size of the DIB, and allocate memory for it.

	// DIB scanlines are padded to 4-byte boundaries.
	dib_bytesperrow= (((p2d->width * dib_bpp)+31)/32)*4;

	p2d->bitssize = p2d->height*dib_bytesperrow;

	p2d->dibsize=sizeof(BITMAPINFOHEADER) + 4*p2d->dib_palette_entries + p2d->bitssize;

	lpdib = (unsigned char*)calloc(p2d->dibsize,1);

	if(!lpdib) { retval=PNGD_E_NOMEM; goto done; }
	p2d->pdib = (LPBITMAPINFOHEADER)lpdib;

	////////

	// TODO: Clean this up.
	palette_offs=sizeof(BITMAPINFOHEADER);
	p2d->bits_offs   =sizeof(BITMAPINFOHEADER) + 4*p2d->dib_palette_entries;
	dib_palette= &lpdib[palette_offs];
	p2d->palette= (RGBQUAD*)dib_palette;
	dib_bits   = &lpdib[p2d->bits_offs];
	p2d->pbits = (VOID*)dib_bits;

	//////// Copy the PNG palette to the DIB palette,
	//////// or create the DIB palette.

	p2d_read_or_create_palette(p2d);

	//////// Allocate row_pointers, which point to each row in the DIB we allocated.

	p2d->dib_row_pointers=(unsigned char**)malloc(p2d->height*sizeof(unsigned char*));
	if(!p2d->dib_row_pointers) { retval=PNGD_E_NOMEM; goto done; }

	for(j=0;j<(int)p2d->height;j++) {
		p2d->dib_row_pointers[p2d->height-1-j]= &dib_bits[j*dib_bytesperrow];
	}

	//////// Read the PNG image into our DIB memory structure.

	switch(decode_strategy) {
	case P2D_ST_8BIT_DIRECT:
		decode_strategy_8bit_direct(p2d,strategy_spp);
		break;
	case P2D_ST_RGBA:
		decode_strategy_rgba(p2d);
		break;
	case P2D_ST_GRAYA:
		decode_strategy_graya(p2d,strategy_tocolor);
		break;
	case P2D_ST_PALETTE:
		decode_strategy_palette(p2d);
		break;
	default:
		retval=PNGD_E_ERROR; goto done;
	}

	png_read_end(p2d->png_ptr, p2d->info_ptr);

	// fill in the DIB header fields
	p2d->pdib->biSize=          sizeof(BITMAPINFOHEADER);
	p2d->pdib->biWidth=         p2d->width;
	p2d->pdib->biHeight=        p2d->height;
	p2d->pdib->biPlanes=        1;
	p2d->pdib->biBitCount=      dib_bpp;
	p2d->pdib->biCompression=   BI_RGB;
	// biSizeImage can also be 0 in uncompressed bitmaps
	p2d->pdib->biSizeImage=     p2d->height*dib_bytesperrow;

	if(p2d->res_valid) {
		p2d->pdib->biXPelsPerMeter= p2d->res_x;
		p2d->pdib->biYPelsPerMeter= p2d->res_y;
	}
	else {
		p2d->pdib->biXPelsPerMeter= 72;
		p2d->pdib->biYPelsPerMeter= 72;
	}

	p2d->pdib->biClrUsed=       p2d->dib_palette_entries;
	p2d->pdib->biClrImportant=  0;

	retval = PNGD_E_SUCCESS;

done:

	if(p2d->src_to_dst_table) free(p2d->src_to_dst_table);
	if(p2d->src_to_linear_table) free(p2d->src_to_linear_table);
	if(p2d->linear_to_srgb_table) free(p2d->linear_to_srgb_table);

	if(p2d->png_ptr) png_destroy_read_struct(&p2d->png_ptr, &p2d->info_ptr, (png_infopp)NULL);
	if(p2d->dib_row_pointers) free((void*)p2d->dib_row_pointers);

	if(retval!=PNGD_E_SUCCESS) {
		if(lpdib) {
			pngdib_p2d_free_dib((PNGDIB*)p2d,NULL);
		}

		// If we don't have an error message yet, use a
		// default one based on the code
		if(!lstrlen(p2d->errmsg)) {
			pngd_get_error_message(retval,p2d->errmsg,PNGDIB_ERRMSG_MAX);
		}
	}

	return retval;
}

void pngdib_p2d_free_dib(PNGDIB *p2d, BITMAPINFOHEADER* pdib)
{
	if(!p2d) {
		if(pdib) free((void*)pdib);
		return;
	}

	if(!pdib) {
		// DIB not explicitly given; use the one from the PNGDIB object.
		// (this is the normal case)
		pdib = p2d->pdib;
		p2d->pdib = NULL;
	}
	if(pdib) {
		free((void*)pdib);
	}
}

PNGDIB* pngdib_init(void)
{
	struct p2d_struct *p2d;

	p2d = (struct p2d_struct *)calloc(sizeof(struct p2d_struct),1);

	if(p2d) {
		p2d->errmsg = (TCHAR*)calloc(PNGDIB_ERRMSG_MAX,sizeof(TCHAR));
	}

	return p2d;
}

int pngdib_done(PNGDIB *p2d)
{
	if(!p2d) return 0;

	if(p2d->errmsg) free(p2d->errmsg);

	free(p2d);
	return 1;
}

TCHAR* pngdib_get_error_msg(PNGDIB *p2d)
{
	return p2d->errmsg;
}

void pngdib_set_userdata(PNGDIB* p2d, void* userdata)
{
	p2d->userdata = userdata;
}

void* pngdib_get_userdata(PNGDIB* p2d)
{
	return p2d->userdata;
}

void pngdib_p2d_set_png_read_fn(PNGDIB *p2d, pngdib_read_cb_type readfunc)
{
	p2d->read_cb = readfunc;
}

void pngdib_p2d_set_use_file_bg(PNGDIB *p2d, int flag)
{
	p2d->use_file_bg_flag = flag;
}

// Colors are given in sRGB color space.
void pngdib_p2d_set_custom_bg(PNGDIB *p2d, unsigned char r,
								  unsigned char g, unsigned char b)
{
	p2d->bkgd_color_custom_srgb.red = r;
	p2d->bkgd_color_custom_srgb.green = g;
	p2d->bkgd_color_custom_srgb.blue = b;
	p2d->bkgd_color_custom_linear.red   = srgb_to_linear_sample(((double)r)/255.0);
	p2d->bkgd_color_custom_linear.green = srgb_to_linear_sample(((double)g)/255.0);
	p2d->bkgd_color_custom_linear.blue  = srgb_to_linear_sample(((double)b)/255.0);
	p2d->use_custom_bg_flag = 1;
}

void pngdib_p2d_enable_color_correction(PNGDIB *p2d, int flag)
{
	p2d->color_correction_enabled = flag;
}

int pngdib_p2d_get_dib(PNGDIB *p2d,
   BITMAPINFOHEADER **ppdib, int *pdibsize)
{
	*ppdib = p2d->pdib;
	if(pdibsize) *pdibsize = p2d->dibsize;
	return 1;
}	

int pngdib_p2d_get_dibbits(PNGDIB *p2d, void **ppbits, int *pbitsoffset, int *pbitssize)
{
	*ppbits = p2d->pbits;
	if(pbitsoffset) *pbitsoffset = p2d->bits_offs;
	if(pbitssize) *pbitssize = p2d->bitssize;
	return 1;
}

int pngdib_p2d_get_density(PNGDIB *p2d, int *pres_x, int *pres_y, int *pres_units)
{
	if(p2d->res_valid) {
		*pres_x = p2d->res_x;
		*pres_y = p2d->res_y;
		*pres_units = p2d->res_units;
		return 1;
	}
	*pres_x = 1;
	*pres_y = 1;
	*pres_units = 0;
	return 0;
}

int pngdib_p2d_get_bgcolor(PNGDIB *p2d, unsigned char *pr, unsigned char *pg, unsigned char *pb)
{
	if(p2d->bkgd_color_applied_flag) {
		*pr = p2d->bkgd_color_applied_srgb.red;
		*pg = p2d->bkgd_color_applied_srgb.green;
		*pb = p2d->bkgd_color_applied_srgb.blue;
		return 1;
	}
	return 0;
}

void pngdib_get_libpng_version(TCHAR *buf, int buflen)
{
#ifdef UNICODE
	StringCchPrintf(buf,buflen,_T("%S"),png_get_libpng_ver(NULL));
#else
	StringCchPrintf(buf,buflen,"%s",png_get_libpng_ver(NULL));
#endif
}

#endif TWPNG_SUPPORT_VIEWER