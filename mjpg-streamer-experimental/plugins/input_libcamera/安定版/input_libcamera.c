#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <unistd.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

extern int libcamera_init(int width, int height, int fps);
extern int libcamera_start(void);
extern void libcamera_stop(void);

extern unsigned char *global_jpeg_buf;
extern size_t global_jpeg_size;
extern pthread_mutex_t global_buf_mutex;

typedef struct {
    int width;
    int height;
    int fps;
    pthread_t thread;
    globals *pglobal; 
    int plugin_id;
} cam_config_t;

static cam_config_t config;

void *worker_thread(void *arg) {
    pthread_t camera_thread;
    pthread_create(&camera_thread, NULL, (void *(*)(void *))libcamera_start, NULL);

    while (1) {
        // カメラのフレームレート（10fps＝100ms）より少し細かくチェックを入れます
  //      usleep(1000 * 20); // 20ms待機

        pthread_mutex_lock(&global_buf_mutex);
        
        // 【重要】新しいJPEGデータが届いており、かつサイズが確定している場合のみ処理
        if (global_jpeg_buf && global_jpeg_size > 0) {
            
            pthread_mutex_lock(&config.pglobal->in[config.plugin_id].db);
            
            free(config.pglobal->in[config.plugin_id].buf);
            config.pglobal->in[config.plugin_id].buf = (unsigned char *)malloc(global_jpeg_size);
            
            if (config.pglobal->in[config.plugin_id].buf) {
                memcpy(config.pglobal->in[config.plugin_id].buf, global_jpeg_buf, global_jpeg_size);
                config.pglobal->in[config.plugin_id].size = global_jpeg_size;
                
                // コピーが完全に終わったため、条件変数で出力側に通知
                pthread_cond_broadcast(&config.pglobal->in[config.plugin_id].db_update);
            }
            
            pthread_mutex_unlock(&config.pglobal->in[config.plugin_id].db);

            // 【重要】古いバッファや書き換え途中のバッファを二重読み込みさせないため、
            // コピー完了後にサイズを一度0にリセットして同期を強制します
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

    int c;

    optind = 1;

    while ((c = getopt(param->argc, param->argv, "x:y:f:")) != -1) {

        switch(c) {

        case 'x':
            config.width = atoi(optarg);
            break;

        case 'y':
            config.height = atoi(optarg);
            break;

        case 'f':
            config.fps = atoi(optarg);
            break;

        default:
            break;
        }
    }


    config.pglobal = param->global;
    config.plugin_id = id;


    IPRINT(
        "libcamera size=%dx%d fps=%d\n",
        config.width,
        config.height,
        config.fps
    );


    if (libcamera_init(
            config.width,
            config.height,
            config.fps) != 0)
    {
        IPRINT("Failed to initialize libcamera\n");
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
    pthread_cancel(config.thread);
    pthread_join(config.thread, NULL);
    return 0;
}
