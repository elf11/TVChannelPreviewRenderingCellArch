#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <libmisc.h>
#include <time.h>
#include <limits.h>

#define waitag(t) mfc_write_tag_mask(1<<t); mfc_read_tag_status_all();

#define SCALE_FACTOR		4
#define NUM_CHANNELS		3
#define WIDTH			624
#define HEIGHT			352
#define	BUF_SIZE		32768 // enough for 16 lines at once
#define NUM_IMAGES_WIDTH	4
#define NUM_IMAGES_HEIGHT	4
#define LINE_SIZE		1872 // WIDTH * NUM_CHANNELS
#define RES_SIZE		8192

//tipurile de mesaje transmise intre ppu si spu, prin mailbox
enum spu_msg {SPU_PROCESS, SPU_TASK_REQ, SPU_FINISH};

//structure that is used to store an image into memory
struct image{
	unsigned char *data;
	unsigned int width, height;
	unsigned char *stuff;
} __attribute__((aligned(16)));

/* data layout for an image:
 * if RED_i, GREEN_i, BLUE_i are the red, green and blue values for 
 * the i-th pixel in the image than the data array inside struct image
 * looks like this:
 * RED_0 GREEN_0 BLUE_0 RED_1 GREEN_1 BLUE_1 RED_2 ...
*/

// structure with data used by the spu
struct date_init {
        // adress of the buff vector from which it can take the rows
        struct image img_orig;
        unsigned char *img_res;
	int index_spu;
        int w_orig;
        int h_orig;
        int scalef;
        int numR;
        int stuff;
	unsigned char *more_stuff;
};


//initial data received from the PPU
struct date_init info __attribute__((aligned(16)));

uint64_t addr __attribute__((aligned(16)));

//scale using area averaging
void scale_area_avg(unsigned char *data, unsigned char *dest, int img_width){
	int i, width;
	vector unsigned char *v1, *v2, *v3, *v4, *res;

	width = img_width / 4 * 3;

	v1 = (vector unsigned char *)data;
	v2 = v1 + width;
	v3 = v2 + width;
	v4 = v3 + width;

	res = (vector unsigned char *)dest;

	for (i = 0; i < width; i += 1)
	{
		v1[i] = spu_avg(v1[i], v2[i]);
		v3[i] = spu_avg(v3[i], v4[i]);
		res[i] = spu_avg(v1[i], v3[i]);
	}	
}


unsigned int check __attribute__((aligned(16)));

int main(unsigned long long speid, unsigned long long argp,
	unsigned long long envp)
{

	struct image big;
	unsigned int data;
	unsigned char cur_buf[BUF_SIZE] __attribute__((aligned(16)));
	struct date_init info_spu[4] __attribute__((aligned(16)));
	unsigned char img_res[LINE_SIZE] __attribute__((aligned(16)));
	int spu_index, offset, i, j, cur_off, numR, line_size;
	int height, size;
	uint32_t tag_info;
	unsigned char *final_pointer;
	unsigned char *copy_pointer, *cur_buf_p;

	// reserve a tag using tag manager
	tag_info = mfc_tag_reserve();
	if (tag_info==MFC_TAG_INVALID){
		printf("SPU: ERROR can't allocate tag ID\n");
		return -1;
	}
	
	while (1)
	{
		check = spu_read_in_mbox();
		if (check == 1)
		{
			data = spu_read_in_mbox();
			size = spu_read_in_mbox();


			memset(cur_buf, -1, sizeof(cur_buf));
		

			mfc_get(info_spu, (uint32_t)data, (uint32_t)size, tag_info, 0, 0);
			waitag(tag_info);
	
	
			line_size = info_spu[0].w_orig * NUM_CHANNELS;
			copy_pointer = info_spu[0].img_orig.data;

			final_pointer = info_spu[0].img_res;

			height = info_spu[0].img_orig.height / SCALE_FACTOR;
			
			while (height > 0)
			{
				offset = 0;
				for (i = 0; i < SCALE_FACTOR; i += 1)
				{
					for (j = 0; j < SCALE_FACTOR; j += 1)
					{
						mfc_get(cur_buf + offset, (uint32_t)copy_pointer, line_size, tag_info, 0, 0);
						copy_pointer += line_size;
						offset += line_size;
					}
				}
				waitag(tag_info);

				//scale the image
				scale_area_avg(&cur_buf, &img_res, info_spu[0].w_orig);

				mfc_put(img_res, (uint32_t)final_pointer, line_size, tag_info, 0, 0);
				waitag(tag_info);

				final_pointer += line_size;

				height -= SCALE_FACTOR;

			}
			
			spu_write_out_intr_mbox(data);
		} else
		{
			break;
		}

	} 
	spu_write_out_intr_mbox(data);
	return 0;

}
