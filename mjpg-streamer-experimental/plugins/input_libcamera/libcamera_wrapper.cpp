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
#include <turbojpeg.h>

using namespace libcamera;

// C言語側と共有するグローバル変数（extern "C" 内に配置）
extern "C" {
    unsigned char *global_jpeg_buf = nullptr;
    size_t global_jpeg_size = 0;
    pthread_mutex_t global_buf_mutex = PTHREAD_MUTEX_INITIALIZER;
    int  is_running = 0; // ? static を外し、C言語から見えるようにここに移動
}

static std::unique_ptr<CameraManager> cm;
static std::shared_ptr<Camera> camera;
static FrameBufferAllocator *allocator = nullptr;
static std::vector<std::unique_ptr<Request>> requests;

static int current_width = 640;
static int current_height = 480;
static unsigned int current_stride = 640;
static int current_fps = 30;
static int current_quality = 75; 

static struct jpeg_compress_struct cinfo;
static struct jpeg_error_mgr jerr;
static bool jpeg_initialized = false;

static tjhandle tj = nullptr;

// JPEG作業用
static std::vector<unsigned char> jpeg_work_buffer;

// UV変換バッファ（毎回確保しない）
static std::vector<unsigned char> u_buffer;
static std::vector<unsigned char> v_buffer;

// フレーム間引き
static unsigned int jpeg_frame_count = 0;

static std::string current_awb_mode = "auto";
static float current_r_gain = 1.0;
static float current_b_gain = 1.0;
// 文字列から libcamera の AWB モード（数値）へ変換するヘルパー関数
static int parse_awb_mode(const std::string &mode) {
    if (mode == "auto")      return controls::AwbAuto;
    if (mode == "incand")    return controls::AwbIncandescent;
    if (mode == "tungsten")  return controls::AwbTungsten;
    if (mode == "fluorescent") return controls::AwbFluorescent;
    if (mode == "sunlight")  return controls::AwbDaylight;
    if (mode == "cloudy")    return controls::AwbCloudy;
    return controls::AwbAuto; // 見つからない場合はデフォルトでauto
}

// 【修正】キーはバッファ、値は「mmapした先頭ポインタ(base)」と「トータルサイズ(total_size)」のペアを保存する
static std::map<FrameBuffer *, std::pair<void *, size_t>> mapped_buffers;

static int map_frame_buffer(FrameBuffer *buffer)
{
    const auto &fb_planes = buffer->planes();

    size_t total_size = 0;
    for (const auto &p : fb_planes)
        total_size = std::max(total_size, p.offset + p.length);

    // 1つのfdから、全体のサイズ分を一括でmmapする
    void *base = mmap(NULL, total_size, PROT_READ, MAP_SHARED, fb_planes[0].fd.get(), 0);

    if (base == MAP_FAILED) {
        std::cerr << "mmap failed\n";
        return -1;
    }

    // 解放時に一発でアンマップできるよう、baseポインタと全サイズを記録
    mapped_buffers[buffer] = std::make_pair(base, total_size);

    return 0;
}

static void unmap_frame_buffers() {
    for (auto const &pair : mapped_buffers) {
        void *base = pair.second.first;
        size_t total_size = pair.second.second;
        
        // 【修正】一括確保したものは、一括で解放する
        if (base && base != MAP_FAILED) {
            munmap(base, total_size);
        }
    }
    mapped_buffers.clear();
}

bool compress_yuv420_to_jpeg_fast(
    FrameBuffer *buffer,
    int width,
    int height,
    unsigned char **jpeg_buf,
    unsigned long *jpeg_size)
{
    // 【修正】マップ情報からbaseポインタを取得
    auto it = mapped_buffers.find(buffer);
    if (it == mapped_buffers.end())
        return false;

    void *base = it->second.first;
    const auto &fb_planes = buffer->planes();
    if (fb_planes.size() < 3)
        return false;

    // baseポインタに各プレーンのオフセットを足して正しい位置を計算
    unsigned char *srcPlanes[3];
    srcPlanes[0] = (unsigned char *)base + fb_planes[0].offset;
    srcPlanes[1] = (unsigned char *)base + fb_planes[1].offset;
    srcPlanes[2] = (unsigned char *)base + fb_planes[2].offset;

    int strides[3];
    // PiSPやlibcameraのYUV420における各プレーンの正確なストライド（歩進幅）を指定
    strides[0] = fb_planes[0].length / height;
    strides[1] = fb_planes[1].length / (height / 2);
    strides[2] = fb_planes[2].length / (height / 2);

    unsigned long outSize = 0;
    unsigned char *outBuf = nullptr;

    int ret = tjCompressFromYUVPlanes(
        tj,
        (const unsigned char **)srcPlanes,
        width,
        strides,
        height,
        TJSAMP_420,
        &outBuf,
        &outSize,
        current_quality,
        TJFLAG_FASTDCT);

    if(ret != 0)
    {
        std::cerr << "TurboJPEG: " << tjGetErrorStr() << std::endl;
        return false;
    }

    *jpeg_buf  = outBuf;
    *jpeg_size = outSize;

    return true;
}

// 【重要】リクエストオブジェクトに対してAWBとカラーゲインをセットする共通関数
static void apply_awb_and_gains(Request *request) {
    if (current_awb_mode == "off") {
        // AWBを完全にOFFにする
        request->controls().set(controls::AwbEnable, false);
        
        // RゲインとBゲインを配列（またはSpan相当）にしてカメラに渡す
        // libcamera の ColourGains は通常2つの要素 [R, B] の配列を受け付けます
        std::array<float, 2> gains = { current_r_gain, current_b_gain };
        request->controls().set(controls::ColourGains, gains);
    } else {
        // AWBがON（通常モード）の場合
        request->controls().set(controls::AwbEnable, true);
        int awb_val = parse_awb_mode(current_awb_mode);
        request->controls().set(controls::AwbMode, awb_val);
    }
}

static void requestComplete(Request *request)
{
    if (request->status() == Request::RequestCancelled)
        return;

    if (!is_running)
        return;

    for (auto &pair : request->buffers())
    {
        FrameBuffer *buffer = pair.second;
        unsigned char *jpeg = nullptr;
        unsigned long size = 0;

        if (compress_yuv420_to_jpeg_fast(buffer, current_width, current_height, &jpeg, &size))
        {
            pthread_mutex_lock(&global_buf_mutex);

            if (global_jpeg_buf)
            {
                tjFree(global_jpeg_buf);
            }

            global_jpeg_buf = jpeg;
            global_jpeg_size = size;

            pthread_mutex_unlock(&global_buf_mutex);
        }
    }

    request->reuse(Request::ReuseBuffers);
    
    // 最終チェック: stop処理中に queueRequest されるのを防ぐ
    if (is_running) {
        // 【追加】周回するリクエストに対しても毎回マニュアルゲインを適用し続ける
        int64_t frame_time = 1000000 / current_fps;
        request->controls().set(controls::FrameDurationLimits, {frame_time, frame_time});
        apply_awb_and_gains(request);
    	
    	camera->queueRequest(request);
    }
}


extern "C" {

int libcamera_init(int width, int height, int fps, int quality, const char *awb_mode, float r_gain, float b_gain) {
    std::cout << "[input_libcamera] Initializing libcamera..." << std::endl;
    
    current_width = width;
    current_height = height;
    current_fps = fps;
	current_quality = quality;
    current_awb_mode = awb_mode ? awb_mode : "auto";
    current_r_gain = r_gain;
    current_b_gain = b_gain;
	
    if (!jpeg_initialized)
    {
        cinfo.err = jpeg_std_error(&jerr);
        tj = tjInitCompress();
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
    streamConfig.pixelFormat = formats::YUV420;

    if (config->validate() == CameraConfiguration::Invalid) return -1;
    if (camera->configure(config.get()) != 0) return -1;
	
    int64_t frame_time = 1000000 / current_fps;

    std::cout << "Target FPS=" << current_fps << " frame_time=" << frame_time << "us" << std::endl;
	
    current_width = streamConfig.size.width;    
    current_height = streamConfig.size.height;  
    current_stride = streamConfig.stride;       
    
    std::cout << "[input_libcamera] Active Resolution: " << current_width << "x" << current_height << std::endl;
    std::cout << "[input_libcamera] Hardware Stride Auto-Detected: " << current_stride << std::endl;

    std::cout << "After validate: " << streamConfig.pixelFormat.toString()
              << " " << streamConfig.size.width << "x" << streamConfig.size.height
              << " stride=" << streamConfig.stride << std::endl;

    allocator = new FrameBufferAllocator(camera);
    Stream *stream = streamConfig.stream();
    if (allocator->allocate(stream) < 0) return -1;

    const std::vector<std::unique_ptr<FrameBuffer>> &allocated_buffers = allocator->buffers(stream);
    for (size_t i = 0; i < allocated_buffers.size(); ++i) {
        std::unique_ptr<Request> request = camera->createRequest();
        if (!request) return -1;

        // 【安全対策】前半で修正した一括mmapを呼び出す
        if (map_frame_buffer(allocated_buffers[i].get()) != 0) return -1;
        if (request->addBuffer(stream, allocated_buffers[i].get()) < 0) return -1;

        // 初回投入時のコントロールセット
        request->controls().set(controls::FrameDurationLimits, {frame_time, frame_time});
        apply_awb_and_gains(request.get()); 
    	
        requests.push_back(std::move(request));
    }

    camera->requestCompleted.connect(requestComplete);
    return 0;
}

int libcamera_start() {
    std::cout << "[input_libcamera] libcamera_start" << std::endl;

    if (is_running) return 0;
    is_running = 1; // ? 1 (true相当) に変更

    if (camera->start() != 0) return -1;

    for (std::unique_ptr<Request> &request : requests) {
        if (camera->queueRequest(request.get()) < 0) return -1;
    }

    while (is_running) { // ? 1の間ループ
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}


void libcamera_stop() {
    // 既に停止処理中の場合は何もしない
    if (!is_running) return;
    is_running = 0;

    std::cout << "[input_libcamera] Stopping camera..." << std::endl;

    // 1. まずシグナルを切断して、これ以上 requestComplete コールバックを発火させない
    camera->requestCompleted.disconnect(requestComplete);
    
    // 2. カメラを停止。これによりハードウェアのキャプチャが終わり、内部バッファが安全にフラッシュされます
    camera->stop();
    
    // 3. バッファを参照しているRequestオブジェクトを完全にクリア
    requests.clear();
    
    // 4. 前半で修正した「一括munmap」を呼び出し、メモリリークと不正アクセスを両方防ぐ
    unmap_frame_buffers();

    // 5. allocatorの解放
    if (allocator) {
        for (Stream *stream : camera->streams()) {
            allocator->free(stream);
        }
        delete allocator;
        allocator = nullptr;
    }

    // 6. カメラデバイスの解放
    camera->release();
    camera.reset();
    cm->stop();
    cm.reset();

    // 7. TurboJPEGおよびグローバルバッファの解放
    // global_buf_mutexの破棄を避けるため、排他制御をしてクリア
    pthread_mutex_lock(&global_buf_mutex);
    if (global_jpeg_buf) {
        tjFree(global_jpeg_buf);
        global_jpeg_buf = nullptr;
    }
    global_jpeg_size = 0;
    pthread_mutex_unlock(&global_buf_mutex);

    if (tj) {
        tjDestroy(tj);
        tj = nullptr;
    }	
    if (jpeg_initialized) {
        jpeg_destroy_compress(&cinfo);
        jpeg_initialized = false;
    }

    std::cout << "[input_libcamera] libcamera_stop finished cleanly." << std::endl;
}

} // extern "C"
