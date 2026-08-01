#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <map>
#include <cstring>
#include <algorithm>
#include <sys/mman.h>
#include <jpeglib.h>
#include <libcamera/libcamera.h>
#include <libcamera/control_ids.h>

using namespace libcamera;

extern "C" {
    unsigned char *global_jpeg_buf = nullptr;
    size_t global_jpeg_size = 0;
    pthread_mutex_t global_buf_mutex = PTHREAD_MUTEX_INITIALIZER;
}

static std::unique_ptr<CameraManager> cm;
static std::shared_ptr<Camera> camera;
static FrameBufferAllocator *allocator = nullptr;
static std::vector<std::unique_ptr<Request>> requests;
static bool is_running = false;

static int current_width = 640;
static int current_height = 480;
static unsigned int current_stride = 640;
static int current_fps = 30;

static struct jpeg_compress_struct cinfo;
static struct jpeg_error_mgr jerr;
static bool jpeg_initialized = false;

static std::map<FrameBuffer *, std::vector<void *>> mapped_buffers;

static int map_frame_buffer(FrameBuffer *buffer)
{
    std::vector<void *> planes;

    const auto &fb_planes = buffer->planes();

    size_t total_size = 0;
    for (const auto &p : fb_planes)
        total_size = std::max(total_size,
                              p.offset + p.length);

    void *base = mmap(NULL,
                      total_size,
                      PROT_READ,
                      MAP_SHARED,
                      fb_planes[0].fd.get(),
                      0);

    if (base == MAP_FAILED) {
        std::cerr << "mmap failed\n";
        return -1;
    }

    // planeごとの先頭ポインタを保存
    for (const auto &p : fb_planes) {
        planes.push_back(
            (unsigned char *)base + p.offset
        );
    }

    mapped_buffers[buffer] = planes;

    return 0;
}


static void unmap_frame_buffers() {
    for (auto const &pair : mapped_buffers) {
        FrameBuffer *buffer = pair.first;
        const std::vector<void *> &planes = pair.second;
        for (size_t i = 0; i < planes.size(); ++i) {
            munmap(planes[i], buffer->planes()[i].length);
        }
    }
    mapped_buffers.clear();
}

bool compress_yuv420_to_jpeg_safe(FrameBuffer *buffer, int width, int height, unsigned char** jpeg_buf, unsigned long* jpeg_size) {
    const std::vector<void *> &planes = mapped_buffers[buffer];
    if (planes.empty()) return false;

    unsigned char *base_addr = (unsigned char *)planes[0];
	unsigned char *y_plane =    (unsigned char *)planes[0];
	unsigned char *uv_plane =    (unsigned char *)planes[1];

	unsigned int y_stride = current_stride;
	unsigned int uv_stride = buffer->planes()[1].length / (height/2);
	
    // --- これ以降（BMP保存テストロジックやJPEG圧縮）のコードは変更なしで大丈夫です ---

    // --- 【BMP保存ロジック（検証用）】 ---
		static int frame_count = 0;
		static bool bmp_saved = false;

		frame_count++;

		if (frame_count == 10 ||
		    frame_count == 60 ||
		    frame_count == 120)
		{
		    // 現在のBMP保存処理

        unsigned char bmp_file_header[14] = {'B','M', 0,0,0,0, 0,0, 0,0, 54,0,0,0};
        unsigned char bmp_info_header[40] = {40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, 24,0};
        unsigned int file_size = 54 + (width * height * 3);
        
        bmp_file_header[2] = (unsigned char)(file_size);
        bmp_file_header[3] = (unsigned char)(file_size >> 8);
        bmp_file_header[4] = (unsigned char)(file_size >> 16);
        bmp_file_header[5] = (unsigned char)(file_size >> 24);
        
        bmp_info_header[4] = (unsigned char)(width);
        bmp_info_header[5] = (unsigned char)(width >> 8);
        bmp_info_header[6] = (unsigned char)(width >> 16);
        bmp_info_header[7] = (unsigned char)(width >> 24);
        
        int neg_height = -height;
        bmp_info_header[8] = (unsigned char)(neg_height);
        bmp_info_header[9] = (unsigned char)(neg_height >> 8);
        bmp_info_header[10] = (unsigned char)(neg_height >> 16);
        bmp_info_header[11] = (unsigned char)(neg_height >> 24);

        std::vector<unsigned char> bmp_pixels(width * height * 3);
        unsigned char *rgb_ptr = bmp_pixels.data();

        for (int y = 0; y < height; y++) {
            unsigned char *y_line = y_plane + (y * y_stride);
            // NV12規格：色差UV行は、輝度2行に対して1行
            unsigned char *uv_line = uv_plane + ((y / 2) * uv_stride);

            for (int x = 0; x < width; x++) {
				int Y = y_line[x];
				int U = uv_line[(x & ~1)] ;
				int V = uv_line[(x & ~1) + 1];

				int C = Y - 16;
				int D = U - 128;
				int E = V - 128;

				int R = (298 * C + 409 * E + 128) >> 8;
				int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
				int B = (298 * C + 516 * D + 128) >> 8;

				R = std::clamp(R,0,255);
				G = std::clamp(G,0,255);
				B = std::clamp(B,0,255);

				*rgb_ptr++ = B;
				*rgb_ptr++ = G;
				*rgb_ptr++ = R;           	
            }
        }

				    	std::cout
				    << "plane count="
				    << buffer->planes().size()
				    << std::endl;

				for (unsigned int i = 0;
				     i < buffer->planes().size();
				     i++)
				{
				    std::cout
				        << "plane[" << i << "] "
				        << "offset="
				        << buffer->planes()[i].offset
				        << " length="
				        << buffer->planes()[i].length
				        << std::endl;
				}


				for (int i = 0; i < 32; i++) {
				    printf("%02x ", uv_plane[i]);
				}
				printf("\n");
    	
    			int x= 320;
    			int y= 240;
		    	unsigned char *y_line = y_plane + y*y_stride;
		unsigned char *uv_line = uv_plane + (y/2)*uv_stride;

		int Y = y_line[x];
		int U = uv_line[(x/2)*2];
		int V = uv_line[(x/2)*2+1];

		printf("center YUV=%d %d %d\n",Y,U,V);
    	
printf("Y0=%d Y100=%d Ycenter=%d\n",
       y_plane[0],
       y_plane[100],
       y_plane[240*y_stride+320]);

std::cout
    << "Y stride="
    << buffer->planes()[0].length / height
    << std::endl;

std::cout
    << "UV stride="
    << buffer->planes()[1].length / (height/2)
    << std::endl;
    	
		    char filename[64];
		    sprintf(filename,
	            "/tmp/debug_frame_%03d.bmp",
	            frame_count);
			
			FILE *f = fopen( filename , "wb");
        if (f) {
            fwrite(bmp_file_header, 1, 14, f);
            fwrite(bmp_info_header, 1, 40, f);
            fwrite(bmp_pixels.data(), 1, bmp_pixels.size(), f);
            fclose(f);
            std::cout << "========= [DEBUG] Saved corrected debug image to /tmp/debug_frame.bmp =========" << std::endl;
            bmp_saved = true;
        }
    }
    // --- 【BMP保存ロジックここまで】 ---

    // JPEG圧縮処理
    jpeg_abort_compress(&cinfo);
    jpeg_mem_dest(&cinfo, jpeg_buf, jpeg_size);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 80, TRUE);

    cinfo.comp_info[0].h_samp_factor = 2; cinfo.comp_info[0].v_samp_factor = 2;
    cinfo.comp_info[1].h_samp_factor = 1; cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1; cinfo.comp_info[2].v_samp_factor = 1;

    cinfo.raw_data_in = TRUE;
    jpeg_start_compress(&cinfo, TRUE);

    JSAMPROW y_rows[16];
    JSAMPROW u_rows[8];
    JSAMPROW v_rows[8];
    JSAMPARRAY image_rows[3] = { y_rows, u_rows, v_rows };

    std::vector<std::vector<unsigned char>> u_lines(8, std::vector<unsigned char>(width / 2));
    std::vector<std::vector<unsigned char>> v_lines(8, std::vector<unsigned char>(width / 2));

    for (int r = 0; r < height; r += 16) {
        for (int i = 0; i < 16; i++) {
            y_rows[i] = y_plane + (r + i) * y_stride;

            if (i % 2 == 0) {
                int uv_row_idx = i / 2;
                int actual_uv_y = (r + i) / 2;
                unsigned char *uv_ptr = uv_plane + (actual_uv_y * uv_stride);
                
                unsigned char *u_dest = u_lines[uv_row_idx].data();
                unsigned char *v_dest = v_lines[uv_row_idx].data();

                for (int x = 0; x < width / 2; x++) {
                    *u_dest++ = *uv_ptr++;
                    *v_dest++ = *uv_ptr++;
                }

                u_rows[uv_row_idx] = u_lines[uv_row_idx].data();
                v_rows[uv_row_idx] = v_lines[uv_row_idx].data();
            }
        }
        jpeg_write_raw_data(&cinfo, image_rows, 16);
    }

    jpeg_finish_compress(&cinfo);
    return true;
}

static void requestComplete(Request *request) {
    if (request->status() == Request::RequestCancelled || !is_running) return;

    const Request::BufferMap &buffers = request->buffers();
    for (auto const &pair : buffers) {
        FrameBuffer *buffer = pair.second;

        unsigned char *jpeg_buf = nullptr;
        unsigned long jpeg_size = 0;

        if (compress_yuv420_to_jpeg_safe(buffer, current_width, current_height, &jpeg_buf, &jpeg_size)) {
            pthread_mutex_lock(&global_buf_mutex);
            
            if (global_jpeg_buf) free(global_jpeg_buf);
            global_jpeg_buf = (unsigned char *)malloc(jpeg_size);
            if (global_jpeg_buf) {
                std::memcpy(global_jpeg_buf, jpeg_buf, jpeg_size);
                global_jpeg_size = jpeg_size;
            }
            free(jpeg_buf);
            
            pthread_mutex_unlock(&global_buf_mutex);
        }
    }

    request->reuse(Request::ReuseBuffers);
    camera->queueRequest(request);
}

extern "C" {

int libcamera_init(int width, int height, int fps) {
    std::cout << "[input_libcamera] Initializing libcamera..." << std::endl;
    
    current_width = width;
    current_height = height;
	current_fps = fps;
	
    if (!jpeg_initialized) {
        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_compress(&cinfo);
        jpeg_initialized = true;
    }

    cm = std::make_unique<CameraManager>();
    if (cm->start() != 0) return -1;
    if (cm->cameras().empty()) return -1;

    camera = cm->cameras()[0];
    if (camera->acquire() != 0) return -1;

    std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration({ StreamRole::Viewfinder });
    if (!config) return -1;

    StreamConfiguration &streamConfig = config->at(0);
    streamConfig.size.width = width;
    streamConfig.size.height = height;
    streamConfig.pixelFormat = formats::NV12; // NV12形式に固定

    if (config->validate() == CameraConfiguration::Invalid) return -1;
    if (camera->configure(config.get()) != 0) return -1;
	
	int64_t frame_time = 1000000 / current_fps;

	std::cout
	    << "Target FPS="
	    << current_fps
	    << " frame_time="
	    << frame_time
	    << "us"
	    << std::endl;
	
	// 【完全修正】ハードウェア（カメラ）が実際に決定した「本当の解像度とストライド」をここで自動取得
    current_width = streamConfig.size.width;    // ? 1920 が入ります
    current_height = streamConfig.size.height;  // ? 1080 が入ります
    current_stride = streamConfig.stride;       // ? 1920 が入ります
    
    std::cout << "[input_libcamera] Active Resolution: " << current_width << "x" << current_height << std::endl;
    std::cout << "[input_libcamera] Hardware Stride Auto-Detected: " << current_stride << std::endl;

	std::cout
	    << "After validate: "
	    << streamConfig.pixelFormat.toString()
	    << " "
	    << streamConfig.size.width
	    << "x"
	    << streamConfig.size.height
	    << " stride="
	    << streamConfig.stride
	    << std::endl;

	allocator = new FrameBufferAllocator(camera);
    Stream *stream = streamConfig.stream();
    if (allocator->allocate(stream) < 0) return -1;

    const std::vector<std::unique_ptr<FrameBuffer>> &allocated_buffers = allocator->buffers(stream);
    for (size_t i = 0; i < allocated_buffers.size(); ++i) {
        std::unique_ptr<Request> request = camera->createRequest();
        if (!request) return -1;

        if (map_frame_buffer(allocated_buffers[i].get()) != 0) return -1;

		if (request->addBuffer(stream, allocated_buffers[i].get()) < 0)
		    return -1;


		int64_t frame_time = 1000000 / current_fps;

		request->controls().set(
		    controls::FrameDurationLimits,
		    {frame_time, frame_time}
		);


		requests.push_back(std::move(request));
    }

	camera->requestCompleted.connect(requestComplete);
	return 0;
}

	int libcamera_start() {
	    if (is_running) return 0;
	    is_running = true;

	    if (camera->start() != 0) return -1;

	    for (std::unique_ptr<Request> &request : requests) {
	        if (camera->queueRequest(request.get()) < 0) return -1;
	    }

	    while (is_running) {
	        std::this_thread::sleep_for(std::chrono::milliseconds(100));
	    }
	    return 0;
	}

	void libcamera_stop() {
	    if (!is_running) return;
	    is_running = false;

	    camera->stop();
	    camera->requestCompleted.disconnect(requestComplete);
	    
	    requests.clear();
	    unmap_frame_buffers();

	    if (allocator) {
	        if (!camera->streams().empty()) {
	            allocator->free(*camera->streams().begin());
	        }
	        delete allocator;
	        allocator = nullptr;
	    }
	    camera->release();
	    camera.reset();
	    cm->stop();
	    cm.reset();

	    if (jpeg_initialized) {
	        jpeg_destroy_compress(&cinfo);
	        jpeg_initialized = false;
	    }
	}
}
 // extern "C"
