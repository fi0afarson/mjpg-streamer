#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <unistd.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

extern int libcamera_init(int width, int height, int fps, int quality);
extern int libcamera_start(void);
extern void libcamera_stop(void);

// C++（libcamera）側にある実行フラグを外部変数として参照

extern unsigned char *global_jpeg_buf;
extern size_t global_jpeg_size;
extern pthread_mutex_t global_buf_mutex;

// 【修正】フルHD(1920x1080)のJPEGが余裕で収まる最大サイズ（2MB）を定義
#define MAX_JPEG_SIZE (2048 * 1024)

typedef struct {
    int width;
    int height;
    int fps;
    int quality;
	pthread_t thread;
    globals *pglobal; 
    int plugin_id;
    int sleep_time_us; // フレームレートに合わせた最適なスリープ時間（マイクロ秒）
} cam_config_t;

static cam_config_t config;

void *worker_thread(void *arg) {
    pthread_t camera_thread;
    pthread_create(&camera_thread, NULL, (void *(*)(void *))libcamera_start, NULL);

    while (1) {
        
        // 【最適化】設定されたfpsに合わせて自動調整された時間だけ待機
        usleep(config.sleep_time_us);

        pthread_mutex_lock(&global_buf_mutex);
        
        if (global_jpeg_buf && global_jpeg_size > 0) {
            
            pthread_mutex_lock(&config.pglobal->in[config.plugin_id].db);
            
            if (config.pglobal->in[config.plugin_id].buf) {
                
                // 【安全対策】万が一、最大サイズ（2MB）を超えそうな場合のバッファオーバーフロー防止
                size_t copy_size = global_jpeg_size;
                if (copy_size > MAX_JPEG_SIZE) {
                    copy_size = MAX_JPEG_SIZE;
                    IPRINT("Warning: JPEG size exceeded MAX_JPEG_SIZE!\n");
                }

                // 固定メモリに対して中身だけを高速に上書きコピー
                memcpy(config.pglobal->in[config.plugin_id].buf, global_jpeg_buf, copy_size);
                config.pglobal->in[config.plugin_id].size = copy_size;
                
                // コピー完了を他のスレッドに通知
                pthread_cond_broadcast(&config.pglobal->in[config.plugin_id].db_update);
            }
            
            pthread_mutex_unlock(&config.pglobal->in[config.plugin_id].db);

            global_jpeg_size = 0;
        }
        pthread_mutex_unlock(&global_buf_mutex);
    }

    pthread_join(camera_thread, NULL);
    return NULL;
}


int input_init(input_parameter *param, int id)
{
    config.width  = 640;
    config.height = 480;
    config.fps    = 30;
    config.quality = 75;
	
    int c;
    optind = 1;

    while ((c = getopt(param->argc, param->argv, "x:y:f:q:")) != -1) {
        switch(c) {
        case 'x': config.width = atoi(optarg); break;
        case 'y': config.height = atoi(optarg); break;
        case 'f': config.fps = atoi(optarg); break;
        case 'q': 
            config.quality = atoi(optarg);
            if (config.quality < 1)  config.quality = 1;
            if (config.quality > 100) config.quality = 100;
            break;
        default: break;
        }
    }

    config.pglobal = param->global;
    config.plugin_id = id;

    // 【自動計算】フレームレート周期（1000000 / fps）の約75%の時間をスリープ時間に設定
    // 例: 30fps(周期33.3ms) ? 約25msスリープ
    // 例: 10fps(周期100ms)  ? 約75msスリープ
    if (config.fps > 0) {
        config.sleep_time_us = (1000000 / config.fps) * 3 / 4;
    } else {
        config.sleep_time_us = 1000 * 25; // デフォルト25ms
    }

    IPRINT("libcamera size=%dx%d fps=%d quality=%d (sleep=%d us)\n", 
           config.width, config.height, config.fps, config.quality, config.sleep_time_us);

    // 初期化時にフルHDにも耐えられる固定バッファ（2MB）を1回だけ確保
    pthread_mutex_lock(&config.pglobal->in[config.plugin_id].db);
    config.pglobal->in[config.plugin_id].buf = (unsigned char *)malloc(MAX_JPEG_SIZE);
    config.pglobal->in[config.plugin_id].size = 0;
    pthread_mutex_unlock(&config.pglobal->in[config.plugin_id].db);

    if (!config.pglobal->in[config.plugin_id].buf) {
        IPRINT("Failed to allocate fixed frame buffer\n");
        return -1;
    }

    if (libcamera_init(config.width, config.height, config.fps, config.quality) != 0) {
        IPRINT("Failed to initialize libcamera\n");
        free(config.pglobal->in[config.plugin_id].buf);
        config.pglobal->in[config.plugin_id].buf = NULL;
        return -1;
    }

    IPRINT("input_libcamera initialized successfully\n");
    return 0;
}


int input_run(int id) {
    IPRINT("starting input_libcamera worker thread\n");
    if (pthread_create(&config.thread, NULL, worker_thread, NULL) != 0) {
        IPRINT("could not start worker thread\n");
        return -1;
    }
    return 0;
}

int input_stop(int id) {
    IPRINT("stopping input_libcamera\n");
    
    libcamera_stop();
    pthread_join(config.thread, NULL);

    // プラグイン停止時に固定バッファを綺麗に解放
    pthread_mutex_lock(&config.pglobal->in[config.plugin_id].db);
    if (config.pglobal->in[config.plugin_id].buf) {
        free(config.pglobal->in[config.plugin_id].buf);
        config.pglobal->in[config.plugin_id].buf = NULL;
    }
    config.pglobal->in[config.plugin_id].size = 0;
    pthread_mutex_unlock(&config.pglobal->in[config.plugin_id].db);

    return 0;
}
