#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <libspe2.h>
#include <pthread.h>
#include <libmisc.h>
#include <malloc_align.h>
#include <free_align.h>

extern spe_program_handle_t lab9_spu;


//tipurile de mesaje transmise intre ppu si spu, prin mailbox
enum spu_msg {SPU_PROCESS, SPU_TASK_REQ, SPU_FINISH};

#define PRINT_ERR_MSG_AND_EXIT(format, ...) \
	{ \
	fprintf(stderr, "%s:%d: " format, __func__, __LINE__, ##__VA_ARGS__); \
	fflush(stderr); \
	exit(1); \
	}

#define NUM_STREAMS 		16	
#define MAX_FRAMES		100	//there are at most 100 frames available
#define MAX_PATH_LEN		256
#define IMAGE_TYPE_LEN 		2
#define SMALL_BUF_SIZE 		16
#define SCALE_FACTOR		4
#define NUM_CHANNELS		3 //red, green and blue
#define MAX_COLOR		255
#define NUM_IMAGES_WIDTH	4 // the final big image has 4 small images
#define NUM_IMAGES_HEIGHT	4 // on the width and 4 on the height
#define BUF_DATA_SIZE		29952 // height * NUM_CHANNELS * ROW_PROCESSED * NUM_IMAGES_WIDTH
#define ROW_PROCESSED		4
#define COPY_DIM		1872	// width * num_channels = 624 * 3
#define WIDTH			624
#define HEIGHT			352

//macros for easily accessing data
#define GET_COLOR_VALUE(img, i, j, k) \
	((img)->data[((i) * (img->width) + (j)) * NUM_CHANNELS + (k)])
#define RED(img, i, j)		GET_COLOR_VALUE(img, i, j, 0)
#define GREEN(img, i, j)	GET_COLOR_VALUE(img, i, j, 1)
#define BLUE(img, i, j)		GET_COLOR_VALUE(img, i, j, 2)

//macro for easily getting how much time has passed between two events
#define GET_TIME_DELTA(t1, t2) ((t2).tv_sec - (t1).tv_sec + \
				((t2).tv_usec - (t1).tv_usec) / 1000000.0)
				
//structure that is used to store an image into memory
struct image{
	unsigned char *data;
	unsigned int width, height;
	unsigned char *stuff;
};

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

//structure send as an argument to the ppu
struct aux {
	spe_context_ptr_t ctx;
	// adress of the data structure to send to the spu
	struct date_init *adr;
	//struct image *img;
	void *argp;
	void *envp;
};

struct main_arg{

	spe_context_ptr_t context;

	struct image *img; //un array de timp image

};

// array with data for each SPU
struct date_init info[NUM_STREAMS] __attribute__((aligned(16)));
// array with arguments for each thread
struct aux vector_thr[NUM_STREAMS] __attribute__((aligned(16)));

/* ===========================================================
 *
 * STUFF FROM THE SERIAL IMPLEMENTATION
 *
 * ============================================================
 */

//read a character from a file specified by a descriptor
char read_char(int fd, char* path){
	char c;
	int bytes_read;
	
	bytes_read = read(fd, &c, 1);
	if (bytes_read != 1){
		PRINT_ERR_MSG_AND_EXIT("Error reading from %s\n", path);
	}
	
	return c;
}

//allocate image data
void alloc_image(struct image* img){		
	img->data = (char *)_malloc_align(NUM_CHANNELS * img->width * img->height * sizeof(char), 4);
	
	if (!img->data){
		PRINT_ERR_MSG_AND_EXIT("malloc_align failed\n");
	}
	
	memset(img->data, -1, NUM_CHANNELS * img->width * img->height);
}

//free image data
void free_image(struct image* img){
	_free_align(img->data);
}

/* read from fd until character c is found
 * result will be atoi(str) where str is what was read before c was
 * found 
 */
unsigned int read_until(int fd, char c, char* path){

	char *buf;
	int i;
	unsigned int res;
	
	buf = (char *)_malloc_align(SMALL_BUF_SIZE * sizeof(char), 4);
	if (!buf)
	{
		PRINT_ERR_MSG_AND_EXIT("malloc_align failed\n");
	}
	
	i = 0;
	memset(buf, 0, SMALL_BUF_SIZE);
	buf[i] = read_char(fd, path);
	while (buf[i] != c){
		i++;
		if (i >= SMALL_BUF_SIZE){
			PRINT_ERR_MSG_AND_EXIT("Unexpected file format for %s\n", path);
		}
		buf[i] = read_char(fd, path);
	}
	res = atoi(buf);
	if (res <= 0) {
		PRINT_ERR_MSG_AND_EXIT("Result is %d when reading from %s\n", 
			res, path);
	}
	
	_free_align(buf);
	
	return res;
}

//read a pnm image
void read_pnm(char* path, struct image* img){
	int fd, bytes_read, bytes_left;
	char *image_type;
	unsigned char *ptr;
	unsigned int max_color;
	
	image_type = (char *)_malloc_align(IMAGE_TYPE_LEN * sizeof(char), 4);
	if (!image_type)
	{
		PRINT_ERR_MSG_AND_EXIT("malloc_align failed\n");
	}
	memset(image_type, -1, IMAGE_TYPE_LEN);
	
	fd = open(path, O_RDONLY);
	
	if (fd < 0){
		PRINT_ERR_MSG_AND_EXIT("Error opening %s\n", path);
		exit(1);
	}
	
	//read image type; should be P6
	bytes_read = read(fd, image_type, IMAGE_TYPE_LEN);
	if (bytes_read != IMAGE_TYPE_LEN){
		PRINT_ERR_MSG_AND_EXIT("Couldn't read image type for %s\n", path);
	}
	if (strncmp(image_type, "P6", IMAGE_TYPE_LEN)){
		PRINT_ERR_MSG_AND_EXIT("Expecting P6 image type for %s. Got %s\n", 
			path, image_type);
	}
	
	//read \n
	read_char(fd, path);
		
	//read width, height and max color value
	img->width = read_until(fd, ' ', path);
	img->height = read_until(fd, '\n', path);
	max_color = read_until(fd, '\n', path);
	if (max_color != MAX_COLOR){
		PRINT_ERR_MSG_AND_EXIT("Unsupported max color value %d for %s\n", 
			max_color, path);
	}
		
	//allocate image data
	alloc_image(img);
	
	//read the actual data 
	bytes_left = img->width * img->height * NUM_CHANNELS;
	ptr = img->data;
	while (bytes_left > 0){
		bytes_read = read(fd, ptr, bytes_left);
		if (bytes_read <= 0){
			PRINT_ERR_MSG_AND_EXIT("Error reading from %s\n", path);
		}
		ptr += bytes_read;
		bytes_left -= bytes_read;
	}
			
	close(fd);
	
	_free_align(image_type);
}

//write a pnm image
void write_pnm(char* path, struct image* img){
	int fd, bytes_written, bytes_left;
	char *buf;
	unsigned char* ptr;
	
	buf = (char *)_malloc_align(32 * sizeof(char), 4);
	if (!buf)
	{
		PRINT_ERR_MSG_AND_EXIT("malloc_align failed\n");
	}
	
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0){
		PRINT_ERR_MSG_AND_EXIT("Error opening %s\n", path);
	}
		
	//write image type, image width, height and max color
	sprintf(buf, "P6\n%d %d\n%d\n", img->width, img->height, MAX_COLOR);
	ptr = (unsigned char*)buf;
	bytes_left = strlen(buf);
	while (bytes_left > 0){
		bytes_written = write(fd, ptr, bytes_left);
		if (bytes_written <= 0){
			PRINT_ERR_MSG_AND_EXIT("Error writing to %s\n", path);
		}
		bytes_left -= bytes_written;
		ptr += bytes_written;
	}
	
	//write the actual data
	ptr = img->data;
	bytes_left = img->width * img->height * NUM_CHANNELS;
	while (bytes_left > 0){
		bytes_written = write(fd, ptr, bytes_left);
		if (bytes_written <= 0){
			PRINT_ERR_MSG_AND_EXIT("Error writing to %s\n", path);
		}
		bytes_left -= bytes_written;
		ptr += bytes_written;
	}
	
	close(fd);
	
	_free_align(buf);
}


//create final result image from downscaled images
void create_big_image(struct image* scaled, struct image* big_image){
	int i, j, k;
	unsigned char* ptr = big_image->data;
	struct image* img_ptr;
	unsigned int height = scaled[0].height;
	unsigned int width = scaled[0].width;
	
	for (i = 0; i < NUM_IMAGES_HEIGHT; i++){
		for (k = 0; k < height; k++) {
			//line by line copy
			for (j = 0; j < NUM_IMAGES_WIDTH; j++){
				img_ptr = &scaled[i * NUM_IMAGES_WIDTH + j];
				memcpy(ptr, &img_ptr->data[k * width * NUM_CHANNELS], width * NUM_CHANNELS);
				ptr += width * NUM_CHANNELS;
			}
		}
		
	}
}

/* ===========================================================
 *
 * STUFF WHERE THE ACTUAL PPU WORK IS TAKING PLACE
 *
 * ============================================================
 */
 
 void *ppu_pthread_function(void *arg)
 {
 	spe_context_ptr_t ctx = *(spe_context_ptr_t *)arg;
 	unsigned int entry = SPE_DEFAULT_ENTRY;
 	
 	// send as argument to the spu the address of the info structure
 	if (spe_context_run(ctx, &entry, 0, NULL, NULL, NULL) < 0)
 	{
 		perror("Failed running context");
 		exit(1);
 	}
 	pthread_exit(NULL);
 }


/* ===========================================================
 *
 * MAIN FUNCTION
 *
 * ============================================================
 */
 
 int main(int argc, char *argv[])
 {
 
 	unsigned int dim, i, j, ret = 0, done = 0, num_frames, n, curr, data;
	unsigned char data_spu;
	unsigned int k __attribute__((aligned(16)));
 	char buf[MAX_PATH_LEN] __attribute__((aligned(16)));
 	char input_path[MAX_PATH_LEN] __attribute__((aligned(16)));
	char output_path[MAX_PATH_LEN] __attribute__((aligned(16)));
	struct image input[NUM_STREAMS] __attribute__((aligned(16)));
	struct image big_image[NUM_STREAMS] __attribute__((aligned(16)));	
	struct image scaled[NUM_STREAMS] __attribute__((aligned(16)));	
	struct timeval t1, t2, t3, t4;
	double scale_time = 0, total_time = 0;
	unsigned char *big_img_p;

 	spe_context_ptr_t ctxs[NUM_STREAMS] __attribute__((aligned(16)));
 	pthread_t threads[NUM_STREAMS] __attribute__((aligned(16)));
 	spe_event_unit_t pevents[NUM_STREAMS] __attribute__((aligned(16)));
 	spe_event_unit_t events_received[NUM_STREAMS] __attribute__((aligned(16))), event_rec;
 	spe_event_handler_ptr_t event_handler;

	event_handler = spe_event_handler_create(); 

	if (argc != 4)
	{
		printf("Usage: ./lab_9_ppu input_path output_path num_frames\n");
		exit(1);
	}
	
	strncpy(input_path, argv[1], MAX_PATH_LEN - 1);
	strncpy(output_path, argv[2], MAX_PATH_LEN - 1);
	num_frames = atoi(argv[3]);

	if (num_frames > MAX_FRAMES)
		num_frames = MAX_FRAMES;


	// dimensions for the final image & alloc space for it
	big_image[0].width = WIDTH;
	big_image[0].height = HEIGHT;
	alloc_image(&big_image[0]);
	
	for (j = 0; j < NUM_STREAMS; j += 1)
	{
		if ((ctxs[j] = spe_context_create(SPE_EVENTS_ENABLE, NULL)) == NULL)
		{
			perror("Failed creating context\n");
			exit(1);
		}

		if (spe_program_load(ctxs[j], &lab9_spu))
		{
			perror("Failed loading program\n");
			exit(1);
		}

		pevents[j].events = SPE_EVENT_OUT_INTR_MBOX;
		pevents[j].spe = ctxs[j];
		pevents[j].data.u32 = j;
		spe_event_handler_register(event_handler, &pevents[j]);
	
		if (pthread_create(&threads[j], NULL, &ppu_pthread_function, &ctxs[j]))
		{
			perror("Failed creating thread\n");
			exit(1);
		}
	}

	gettimeofday(&t3, NULL);

	for (i = 0; i < num_frames; i += 1)
	{
		printf("Processing frame %d\n", i + 1);
		for (j = 0; j < NUM_STREAMS; j++)
		{
			sprintf(buf, "%s/stream%02d/image%d.pnm", input_path, 
						j + 1, i + 1);
			read_pnm(buf, &input[j]);


			scaled[j].width = input[j].width / SCALE_FACTOR;
			scaled[j].height = input[j].height / SCALE_FACTOR;
			alloc_image(&scaled[j]);

			info[j].index_spu = j;
			alloc_image(&info[j].img_orig);
			memcpy(&info[j].img_orig, &input[j], sizeof(struct image));
			info[j].w_orig = input[j].width;
			info[j].h_orig = input[j].height;
			// scale factor for the image
			info[j].scalef = 4;
			// number of rows to work on in the spu
			info[j].numR = 4;
			info[j].img_res = scaled[j].data;

		}
	
		
	/*
	 * get 4 rows of each initial image and concatenate them
	 * in an intermediary buffer to be send to the spu; send to the
	 * spu the address where to put the scaled result for the 
	 * transformation, the address should be the row of the iteratio
	 * in the final image
	 */

		memset(big_image[0].data, 1000, (big_image[0].width * big_image[0].height * NUM_CHANNELS));
		gettimeofday(&t1, NULL);
		done = 0;
		for (j = 0; j < NUM_STREAMS; j += 1)
		{
			unsigned int tmp = (unsigned int)&info[j];
			unsigned int size = sizeof(struct date_init);
			k = 1;
			spe_in_mbox_write(ctxs[j], (void *)&k, 1, SPE_MBOX_ANY_NONBLOCKING);
			spe_in_mbox_write(ctxs[j], &tmp, 1, SPE_MBOX_ANY_NONBLOCKING);
			spe_in_mbox_write(ctxs[j], &size, 1, SPE_MBOX_ANY_NONBLOCKING);
		}

		while (done < 16)
       		{
			ret = spe_event_wait(event_handler, events_received, NUM_STREAMS, -1);

               		if (ret < 0)
               		{
                		printf("Error: event wait error\n");
               		} else
                	{
                		if (events_received[0].events & SPE_EVENT_OUT_INTR_MBOX)
                		{
                        		// read the mailbox for the spe that issued the event
                        		spe_out_intr_mbox_read(events_received[0].spe, (unsigned int *)&data, 1, SPE_MBOX_ANY_BLOCKING);
                        		done += 1;
                        	}

              		}
		}
		gettimeofday(&t2, NULL);
		scale_time += GET_TIME_DELTA(t1, t2);
		// create the big image from the scaled images
		create_big_image(scaled, &big_image[0]);

		//write the big image
		sprintf(buf, "%s/result%d.pnm", output_path, i + 1);
		write_pnm(buf, &big_image[0]);
	}

	for (j = 0; j < NUM_STREAMS; j += 1)
	{
		k = 0;
		spe_in_mbox_write(ctxs[j], (void *)&k, 1, SPE_MBOX_ANY_NONBLOCKING);
	}

	
	done = 0;
	while (done < 16)
        {
		ret = spe_event_wait(event_handler, events_received, NUM_STREAMS, -1);

                if (ret < 0)
               	{
                	printf("Error: event wait error\n");
               	} else
                {
                	if (events_received[0].events & SPE_EVENT_OUT_INTR_MBOX)
                	{
                        	// read the mailbox for the spe that issued the event
                        	spe_out_intr_mbox_read(events_received[0].spe, (unsigned int *)&data, 1, SPE_MBOX_ANY_BLOCKING);
                        	done += 1;
                        }

              	}
	}


	for (i = 0; i < NUM_STREAMS; i += 1)
	{
		pthread_join(threads[i], NULL);
		spe_context_destroy(ctxs[i]);
	}	
	//free the image data
	for (j = 0; j < NUM_STREAMS; j++){
		free_image(&input[j]);
		//free_image(&scaled[j]);
	}
	free_image(&big_image[0]);

	gettimeofday(&t4, NULL);
	total_time += GET_TIME_DELTA(t3, t4);


	printf("Scale time : %lf\n", scale_time);
	printf("Total time : %lf\n", total_time);

	return 0;
 
 }
