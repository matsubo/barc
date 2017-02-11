//
//  frame_builder.c
//  barc
//
//  Created by Charley Robinson on 2/10/17.
//  Copyright © 2017 TokBox, Inc. All rights reserved.
//

extern "C" {
#include <stdlib.h>
#include <MagickWand/MagickWand.h>
#include <uv.h>
#include <unistd.h>

#include "frame_builder.h"
#include "magic_frame.h"
}

#include <vector>
#include <map>

static int job_counter;
static void crunch(uv_work_t* work);
static void after_crunch(uv_work_t* work, int status);
static void free_job(struct frame_job_t* job);
static void frame_builder_worker(void* p);

struct frame_job_t {
    uv_work_t request;
    int width;
    int height;
    enum AVPixelFormat format;
    std::vector<struct frame_builder_subframe_t*> subframes;
    frame_builder_cb_t callback;
    AVFrame* output_frame;
    frame_builder_t* builder;
    int serial_number;
    void* p;
};

struct frame_builder_t {
    struct frame_job_t* current_job;
    std::map<int, struct frame_job_t*>pending_jobs;
    std::map<int, struct frame_job_t*>finished_jobs;
    char running;
    uv_loop_t *loop;
    uv_thread_t loop_thread;
    int finish_serial;
};

int frame_builder_alloc(struct frame_builder_t** frame_builder) {
    struct frame_builder_t* result = (struct frame_builder_t*)
    calloc(1, sizeof(struct frame_builder_t));
    result->pending_jobs = std::map<int, struct frame_job_t*>();
    result->finished_jobs = std::map<int, struct frame_job_t*>();
    result->loop = (uv_loop_t*) malloc(sizeof(uv_loop_t));
    result->running = 1;
    result->finish_serial = 0;
    // abuse the thread pool a bit
    uv_cpu_info_t* cpu_infos;
    int cpu_count;
    uv_cpu_info(&cpu_infos, &cpu_count);
    char str[4];
    sprintf(str, "%d", cpu_count);
    uv_free_cpu_info(cpu_infos, cpu_count);
    // or, you know, don't. see what works for you.
    //setenv("UV_THREADPOOL_SIZE", str, 0);
    uv_loop_init(result->loop);
    uv_thread_create(&result->loop_thread, frame_builder_worker, result);

    *frame_builder = result;
    return 0;
}

void frame_builder_free(struct frame_builder_t* frame_builder) {
    int ret;
    frame_builder->running = 0;
    uv_stop(frame_builder->loop);
    do {
        ret = uv_loop_close(frame_builder->loop);
    } while (UV_EBUSY == ret);
    uv_thread_join(&frame_builder->loop_thread);
    free(frame_builder);
}

int frame_builder_begin_frame(struct frame_builder_t* frame_builder,
                              int width, int height,
                              enum AVPixelFormat format, void* p)
{
    struct frame_job_t* job = (struct frame_job_t*)
    calloc(1, sizeof(struct frame_job_t));
    job->builder = frame_builder;
    job->serial_number = job_counter++;
    job->width = width;
    job->height = height;
    job->format = format;
    job->p = p;
    frame_builder->current_job = job;
    return 0;
}

int frame_builder_add_subframe(struct frame_builder_t* frame_builder,
                               struct frame_builder_subframe_t* subframe)
{
    struct frame_builder_subframe_t* subframe_copy =
    (struct frame_builder_subframe_t*)
    calloc(1, sizeof(struct frame_builder_subframe_t));
    memcpy(subframe_copy, subframe, sizeof(struct frame_builder_subframe_t));
    smart_frame_retain(subframe->smart_frame);
    frame_builder->current_job->subframes.push_back(subframe_copy);
    return 0;
}

int frame_builder_finish_frame(struct frame_builder_t* frame_builder,
                               frame_builder_cb_t callback) {
    struct frame_job_t* job = frame_builder->current_job;
    job->callback = callback;
    job->request.data = job;
    frame_builder->pending_jobs[job->serial_number] = job;
    int ret = uv_queue_work(frame_builder->loop,
                            &(job->request),
                            crunch,
                            after_crunch);
    return ret;
}

int frame_builder_join(struct frame_builder_t* frame_builder) {
    while (!frame_builder->pending_jobs.empty() ||
           !frame_builder->finished_jobs.empty())
    {
        sleep(1);
    }
    return 0;
}

static void free_job(struct frame_job_t* job) {
    for (struct frame_builder_subframe_t* subframe : job->subframes) {
        smart_frame_release(subframe->smart_frame);
    }
    job->subframes.clear();
    av_frame_free(&job->output_frame);
    free(job);
}

static void after_crunch(uv_work_t* work, int status) {
    struct frame_job_t* job = (struct frame_job_t*)work->data;
    struct frame_builder_t* builder = job->builder;
    // move job from pending to finished, but don't call callback just yet...
    builder->pending_jobs.erase(job->serial_number);
    builder->finished_jobs[job->serial_number] = job;

    // invoke callbacks and flush all finished jobs in order they were received.
    auto iter = builder->finished_jobs.find(builder->finish_serial);
    while (iter != builder->finished_jobs.end()) {
        printf("Callback processed frame %d\n", iter->second->serial_number);
        iter->second->callback(iter->second->output_frame, iter->second->p);
        free_job(iter->second);
        builder->finished_jobs.erase(iter);
        builder->finish_serial++;
        iter = builder->finished_jobs.find(builder->finish_serial);
    }
}

static void crunch(uv_work_t* work) {
    struct frame_job_t* job = (struct frame_job_t*)work->data;
    int ret;
    MagickWand* output_wand;
    magic_frame_start(&output_wand, job->width, job->height);

    AVFrame* output_frame = av_frame_alloc();

    // Configure output frame buffer
    output_frame->format = job->format;
    output_frame->width = job->width;
    output_frame->height = job->height;
    ret = av_frame_get_buffer(output_frame, 1);
    if (ret) {
        printf("No output AVFrame buffer to write video. Error: %s\n",
               av_err2str(ret));
    }

    if (!output_frame) {
        perror("Could not allocate frame");
    }
    
    for (struct frame_builder_subframe_t* subframe : job->subframes) {
        magic_frame_add(output_wand,
                        smart_frame_get(subframe->smart_frame),
                        subframe->x_offset,
                        subframe->y_offset,
                        subframe->render_width,
                        subframe->render_height);
    }

    ret = magic_frame_finish(output_wand, output_frame);

    job->output_frame = output_frame;

    printf("Crunched job number %d\n", job->serial_number);
}

static void frame_builder_worker(void* p) {
    struct frame_builder_t* builder = (struct frame_builder_t*)p;
    int ret = 0;
    while (builder->running && 0 == ret) {
        ret = uv_run(builder->loop, UV_RUN_DEFAULT);
    }

    printf("goodbye loop!\n");
}
