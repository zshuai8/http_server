#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <time.h>

#include "threadpool.h"
#include "list.h"
#include "threadpool_lib.h"

struct thread_pool {
        int thread_count; // How many threads

        pthread_t main_tid;

        struct worker * worker_array; // thread_count of workers

        struct list future_list; // Global task queue

        bool creation_finished; // If the creation of the thread_pool has been finished
        pthread_cond_t creation_cond; // Notify when user submits a new task
        pthread_mutex_t creation_mutex;

        pthread_cond_t new_condition; // New submission or destroy the pool
        pthread_mutex_t new_condition_mutex;

        bool need_shutdown; // If the pool needs to be shutdown

        pthread_mutex_t pool_mutex; // Lock it when change data in thread_pool

};

struct worker {
        pthread_t worker_tid; // The tid of the worker

        struct list future_list; // The task queue of the worker

        struct thread_pool *pool; // The thread pool the worker is in

        pthread_mutex_t worker_mutex; // Lock it when change data in the worker
};

enum future_state {
        IN_QUEUE,
        RUNNING,
        FINISHED
};

struct future {
        sem_t future_sem; // If the future has been finished

        enum future_state state;

        //struct worker* running_worker;

        // Thread_pool function arguments
        fork_join_task_t task;
        struct thread_pool *pool;
        void * data;
        void * results;

        pthread_mutex_t future_mutex; // Lock it when change data in the future

        struct list_elem elem; // Make the future can be linked by list
};


static void * worker_thread(void *worker_void){
        struct worker *current_worker=worker_void;

        struct thread_pool *pool=current_worker->pool;

        pthread_mutex_lock(&pool->creation_mutex);
        while(pool->creation_finished==false) {
                pthread_cond_wait(&pool->creation_cond,&pool->creation_mutex);
        }
        pthread_mutex_unlock(&pool->creation_mutex);

        while(true) {
                struct future *working_future;

                if(pool->need_shutdown) {
                        pthread_exit(0);
                }

                pthread_mutex_lock(&current_worker->worker_mutex);
                pthread_mutex_lock(&pool->pool_mutex);
                if(list_empty(&current_worker->future_list)) {
                        if(!list_empty(&pool->future_list)) {
                                // printf("%d:internal empty but global not empty\n",pthread_self());
                                struct list_elem *e=list_pop_front(&pool->future_list);
                                working_future = list_entry (e, struct future, elem);
                                // pthread_mutex_unlock(&current_worker->worker_mutex);
                        }else{
                                // printf("%d:internal empty but global empty\n",pthread_self());
                                int i=0;
                                while(true) {
                                        struct worker *victim_worker=&pool->worker_array[i];
                                        if(list_empty(&victim_worker->future_list)) {
                                                i++;
                                                continue;
                                        }else{
                                                break;
                                        }
                                }
                                if(i<pool->thread_count) {
                                        // printf("%d:internal empty but global not empty and others have\n",pthread_self());
                                        struct worker *victim_worker=&pool->worker_array[i];
                                        // pthread_mutex_lock(&pool->pool_mutex);
                                        struct list_elem *e=list_pop_front(&victim_worker->future_list);
                                        working_future = list_entry (e, struct future, elem);
                                        // pthread_mutex_unlock(&pool->pool_mutex);
                                }else{
                                        // printf("worker_thread waiting %d:\n",pthread_self());
                                        pthread_mutex_unlock(&pool->pool_mutex);
                                        pthread_mutex_unlock(&current_worker->worker_mutex);

                                        if(pool->need_shutdown) {
                                                // printf("%d:will exit\n",pthread_self());
                                                pthread_exit(0);
                                        }

                                        pthread_mutex_lock(&pool->new_condition_mutex);
                                        pthread_cond_wait(&pool->new_condition,&pool->new_condition_mutex);
                                        pthread_mutex_unlock(&pool->new_condition_mutex);

                                        if(pool->need_shutdown) {
                                                // printf("%d:will exit\n",pthread_self());
                                                pthread_exit(0);
                                        }else{
                                                // printf("%d:will continue\n",pthread_self());
                                                continue;
                                        }
                                }
                        }
                }else{
                        // printf("%d:internal not empty\n",pthread_self());
                        // pthread_mutex_lock(&current_worker->worker_mutex);
                        struct list_elem *e=list_pop_back(&current_worker->future_list);
                        working_future = list_entry (e, struct future, elem);
                        // pthread_mutex_unlock(&current_worker->worker_mutex);
                }

                pthread_mutex_unlock(&current_worker->worker_mutex);
                pthread_mutex_unlock(&pool->pool_mutex);
                // printf("worker_thread solving %d:\n",pthread_self());
                working_future->state=RUNNING;
                working_future->results=working_future->task(pool,working_future->data);
                working_future->state=FINISHED;

                int i=0;
                for(; i<pool->thread_count; i++) {
                        sem_post(&working_future->future_sem);
                }
        }
        return NULL;
}

/* Create a new thread pool with no more than n threads. */
struct thread_pool * thread_pool_new(int nthreads){
        struct thread_pool * pool=malloc(sizeof(struct thread_pool));

        if(pool==NULL) {
                printf("malloc error when creating thread_pool.");
                exit(1);
        }

        pool->thread_count=nthreads;

        pool->worker_array=calloc(pool->thread_count,sizeof(struct worker));

        list_init(&pool->future_list);

        pool->need_shutdown=false;

        pool->main_tid=pthread_self();

        pthread_cond_init(&pool->creation_cond,NULL);

        pthread_mutex_init(&pool->creation_mutex,NULL);

        pthread_cond_init(&pool->new_condition,NULL);

        pthread_mutex_init(&pool->new_condition_mutex,NULL);

        pthread_mutex_init(&pool->pool_mutex,NULL);

        pool->creation_finished=false;

        int i=0;
        for(; i<nthreads; i++) {
                struct worker * current_worker=&pool->worker_array[i];

                list_init(&current_worker->future_list);

                current_worker->pool=pool;

                pthread_mutex_init(&current_worker->worker_mutex,NULL);

                pthread_create(&current_worker->worker_tid, NULL, worker_thread, current_worker);
        }
        pool->creation_finished=true;

        pthread_cond_broadcast(&pool->creation_cond);

        return pool;
}

/*
 * Shutdown this thread pool in an orderly fashion.
 * Tasks that have been submitted but not executed may or
 * may not be executed.
 *
 * Deallocate the thread pool object before returning.
 */

void thread_pool_shutdown_and_destroy(struct thread_pool *pool){
        assert(pool != NULL);
        pthread_mutex_lock(&pool->pool_mutex);
        pool->need_shutdown=true;
        pthread_mutex_unlock(&pool->pool_mutex);

        pthread_cond_broadcast(&pool->new_condition);

        int i=0;
        for(; i<pool->thread_count; i++) {
                struct worker * current_worker=&pool->worker_array[i];
                pthread_join(current_worker->worker_tid,NULL);
        }


        pthread_cond_destroy(&pool->creation_cond);
        pthread_mutex_destroy(&pool->creation_mutex);

        pthread_cond_destroy(&pool->new_condition);
        pthread_mutex_destroy(&pool->new_condition_mutex);

        pthread_mutex_destroy(&pool->pool_mutex);

        free(pool->worker_array);
        free(pool);
}

/*
 * Submit a fork join task to the thread pool and return a
 * future.  The returned future can be used in future_get()
 * to obtain the result.
 * 'pool' - the pool to which to submit
 * 'task' - the task to be submitted.
 * 'data' - data to be passed to the task's function
 *
 * Returns a future representing this computation.
 */


struct future * thread_pool_submit(struct thread_pool *pool, fork_join_task_t task, void * data){
        struct future * return_future=malloc(sizeof(struct future));

        sem_init(&return_future->future_sem,0,0);
        return_future->task=task;
        return_future->data=data;
        return_future->pool=pool;

        pthread_mutex_lock(&pool->pool_mutex);
        list_push_back (&pool->future_list, &return_future->elem);
        return_future->state=IN_QUEUE;
        pthread_mutex_unlock(&pool->pool_mutex);

        pthread_mutex_init(&return_future->future_mutex,NULL);
        pthread_cond_broadcast(&pool->new_condition);
        return return_future;
}


/* Make sure that the thread pool has completed the execution
 * of the fork join task this future represents.
 *
 * Returns the value returned by this task.
 */
void * future_get(struct future *working_future){
        struct thread_pool * pool=working_future->pool;

        if(pool->main_tid==pthread_self()) {
                if(working_future->state!=FINISHED) {
                        sem_wait(&working_future->future_sem);
                }
                return working_future->results;
        }else{
                pthread_mutex_lock(&pool->pool_mutex);
                if(working_future->state==FINISHED) {
                        pthread_mutex_unlock(&pool->pool_mutex);

                        return working_future->results;
                }else if(working_future->state==IN_QUEUE) {
                        list_remove(&working_future->elem);
                        pthread_mutex_unlock(&pool->pool_mutex);
                        working_future->state=RUNNING;
                        working_future->results=working_future->task(pool,working_future->data);
                        working_future->state=FINISHED;

                        return working_future->results;
                }else{
                        pthread_mutex_unlock(&pool->pool_mutex);
                        sem_wait(&working_future->future_sem);
                        return working_future->results;
                }
        }
}


/* Deallocate this future.  Must be called after future_get() */
void future_free(struct future *f){
        if(f!=NULL) {
                if(f->state==FINISHED) {
                        sem_destroy(&f->future_sem);
                        pthread_mutex_destroy(&f->future_mutex);
                        free(f);
                }else{
                        sem_wait(&f->future_sem);
                        sem_destroy(&f->future_sem);
                        pthread_mutex_destroy(&f->future_mutex);
                        free(f);
                }
        }
}
