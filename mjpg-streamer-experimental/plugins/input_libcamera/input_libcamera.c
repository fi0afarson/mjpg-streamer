#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <unistd.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

extern int libcamera_init(int width, int height, int fps, int quality, const char *awb_mode, float r_gain, float b_gain);

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
    char awb_mode[32];
    float r_gain;      // ? 【追加】Rゲイン
    float b_gain;      // ? 【追加】Bゲイン
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
    config.width   = 640;
    config.height  = 480;
    config.fps     = 30;
    config.quality = 75;
    strcpy(config.awb_mode, "auto");
    config.r_gain  = 1.0; // ? 【追加】デフォルト値
    config.b_gain  = 1.0; // ? 【追加】デフォルト値

    int c;
    optind = 1;

    // 【修正】getoptの解析引数に "r:b:" を追加、"g:" を削除
    while ((c = getopt(param->argc, param->argv, "x:y:f:q:a:r:b:")) != -1) {
        switch(c) {
        case 'x': config.width = atoi(optarg); break;
        case 'y': config.height = atoi(optarg); break;
        case 'f': config.fps = atoi(optarg); break;
        case 'q':
            config.quality = atoi(optarg);
            if (config.quality < 1)  config.quality = 1;
            if (config.quality > 100) config.quality = 100;
            break;
        case 'a':
            strncpy(config.awb_mode, optarg, sizeof(config.awb_mode) - 1);
            break;
        case 'r': // ? 【追加】Rゲインの取得
            config.r_gain = atof(optarg);
            break;
        case 'b': // ? 【追加】Bゲインの取得
            config.b_gain = atof(optarg);
            break;
        default: break;
        }
    }

    config.pglobal = param->global;
    config.plugin_id = id;

    if (config.fps > 0) {
        config.sleep_time_us = (1000000 / config.fps) * 3 / 4;
    } else {
        config.sleep_time_us = 1000 * 25;
    }

    // ログにAWB設定を出力
    IPRINT("libcamera size=%dx%d fps=%d quality=%d awb=%s (R_Gain=%.2f, B_Gain=%.2f)\n", 
           config.width, config.height, config.fps, config.quality, config.awb_mode, config.r_gain, config.b_gain);

    pthread_mutex_lock(&config.pglobal->in[config.plugin_id].db);
    config.pglobal->in[config.plugin_id].buf = (unsigned char *)malloc(MAX_JPEG_SIZE);
    config.pglobal->in[config.plugin_id].size = 0;
    pthread_mutex_unlock(&config.pglobal->in[config.plugin_id].db);

    if (!config.pglobal->in[config.plugin_id].buf) {
        IPRINT("Failed to allocate fixed frame buffer\n");
        return -1;
    }

    // 【修正】引数を変更してC++側を呼び出し
    if (libcamera_init(config.width, config.height, config.fps, config.quality, config.awb_mode, config.r_gain, config.b_gain) != 0) {
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
